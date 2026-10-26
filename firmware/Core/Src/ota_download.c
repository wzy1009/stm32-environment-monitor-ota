/**
 * @file ota_download.c
 * @brief 【OTA 下载】APP 端 OTA 下载总调度。
 *        APP 固件：OTA 版本从内部 Flash 0x08008000 运行，负责联网下载与业务。
 * 取得已校验通知，暂停 MQTT，擦除外部暂存区，通过 HTTP 拉取 BIN，校验后最后提交 A5 标志并复位；
 * 不在这里擦写 MCU 内部 APP。
 */
#include "ota_download.h"
#include "ota_app.h"
#include "ota_notice.h"
#include "ota_meta.h"
#include "ota_image.h"
#include "http_image.h"
#include "cloud_task.h"
#include "esp_at.h"
#include "w25q128.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

/* 状态转换：IDLE 等通知 -> CLOSE_OLD/DRAIN 释放旧连接 -> ERASE 擦外部区
 * -> SET_PASSIVE/OPEN_SOCKET 建 HTTP -> SEND_SIZE/SEND_GET 发请求
 * -> QUERY_LENGTH/PULL 反复取数 -> VERIFY 校验 -> COMMIT 提交并复位。
 * FAILED 停止本次任务；只有 EEPROM 状态允许时才开放重新下载。
 */
typedef enum {
    IDLE, CLOSE_OLD, DRAIN, ERASE, SET_PASSIVE, OPEN_SOCKET, SEND_SIZE,
    SEND_GET, QUERY_LENGTH, PULL, VERIFY, COMMIT, FAILED
} Phase;
static Phase phase;
static OtaNotice notice;
static OtaMeta meta;
static EspAt_Stats baseline;
static uint32_t tick, started, last_data, quiet, erase_offset, available, progress;
static uint8_t waiting, closed, length_seen, frame_seen, passive;
static uint16_t chunk_used, frame_length, requested, request_length;
/* One reply plus framing fits the 2048-byte UART ring. Flash pages stay 256 bytes. */
static uint8_t chunk[1024];
static char request[384];
static const char *callback_error;
static uint8_t recoverable;
static OtaDownloadProgress screen;

/**
 * @brief 【OTA 下载】输出 HTTP/OTA 状态日志，调用发生在主循环上下文。
 *
 * @param text 以结束符结尾的文本。
 *
 * @return 无返回值。
 */
static void Log(const char *text)
{
    if (HAL_UART_Transmit(&huart1, (const uint8_t *)text, (uint16_t)strlen(text), 100U) != HAL_OK) { Error_Handler(); }
}

/**
 * @brief 【OTA 下载】锁定失败状态并记录屏幕错误。
 *        仅当尚未进入提交且 EEPROM 可读、pending 为 0 时开放网络恢复；
 *        提交结果不明确时保留现场，防止自动重试破坏镜像。
 *
 * @param reason 错误或状态原因。
 *
 * @return 无返回值。
 */
static void Fail(const char *reason)
{
    OtaMeta actual;
    Log("[下载] 已停止："); Log(reason); Log("\r\n");
    recoverable = (uint8_t)(phase != COMMIT && OtaMeta_Read(&actual) == OTA_META_OK && actual.pending == 0U);
    Log(recoverable ? "[下载] 旧APP保留，网络恢复后请重新发送升级通知\r\n" :
        "[下载] 恢复已锁定：请检查EEPROM及提交状态；保留暂存固件，禁止自动重试\r\n");
    screen.stage = OTA_PROGRESS_FAILED;
    (void)snprintf(screen.error, sizeof(screen.error), "%s", reason);
    phase = FAILED;
    /* Keep the handlers to drain late bytes; do not reinterpret them as MQTT. */
}

/**
 * @brief 【OTA 下载】把当前串口错误计数与下载开始时的基线比较。
 *        新溢出、硬件错误或拆帧异常意味着本次数据流不再可信。
 *
 * @return 非零表示接收错误计数未增加且字节流可信，0 表示不满足该条件。
 */
static uint8_t StatsValid(void)
{
    EspAt_Stats s;
    EspAt_GetStats(&s);
    return s.overflow == baseline.overflow && s.uart_errors == baseline.uart_errors &&
           s.rearm_failures == baseline.rearm_failures && !EspAt_HasStreamFault();
}

/**
 * @brief 【OTA 下载】处理一个 TCP 接收帧的长度通知。
 *        仅在等待 PULL 响应时接收，拒绝重复帧和超出本次请求长度的帧。
 *
 * @param length 本次被动接收帧声明的字节数。
 *
 * @return 无返回值。
 */
static void Frame(uint32_t length)
{
    if (phase <= DRAIN || phase == FAILED) { return; }
    if (phase != PULL || !waiting || frame_seen || length > requested) {
        callback_error = "接收帧时序异常或长度过大"; return;
    }
    frame_seen = 1U; frame_length = (uint16_t)length; chunk_used = 0U;
}

/**
 * @brief 【OTA 下载】把当前 TCP 帧的正文逐字节存入 1024 字节接收缓存。
 *        完整帧由 Task 再交给 HTTP 解析器，这里不在每个字节上擦写 Flash。
 *
 * @param byte 待处理的一个字节。
 *
 * @return 无返回值。
 */
static void Byte(uint8_t byte)
{
    quiet = HAL_GetTick();
    if (phase <= DRAIN || phase == FAILED) { return; }
    if (phase != PULL || !frame_seen || chunk_used >= frame_length || chunk_used >= sizeof(chunk)) {
        callback_error = "收到非预期TCP字节"; return;
    }
    chunk[chunk_used++] = byte;
}

/**
 * @brief 【OTA 下载】解析 ESP 的 CIPRECVLEN 长度列表，提取当前单连接可读字节数，并拒绝数值溢出或其他有效连接的数据。
 *
 * @param text 以结束符结尾的文本。
 * @param value 当前单连接可读取的字节数。
 *
 * @return 非零表示长度列表解析成功，0 表示不满足该条件。
 */
static int BufferLength(const char *text, uint32_t *value)
{
    const char *p = text;
    uint8_t first = 1U;
    do {
        uint32_t n = 0U;
        uint8_t negative = 0U;
        if (*p == '-') { negative = 1U; ++p; }
        if (*p < '0' || *p > '9') { return 0; }
        while (*p >= '0' && *p <= '9') {
            n = n * 10U + (uint32_t)(*p++ - '0');
            if (n > 65535U) { return 0; }
        }
        if (negative && n != 1U) { return 0; }
        if (first) { *value = negative ? 0U : n; first = 0U; }
        else if (!negative && n != 0U) { return 0; }
        if (!*p) { return 1; }
        if (*p++ != ',') { return 0; }
    } while (*p);
    return 0;
}

/**
 * @brief 【OTA 下载】接收下载所需的 AT 文本，例如 CLOSED 和 CIPRECVLEN。
 *        返回非零表示该行已消费；
 *        二进制固件字节由 Byte 接收。
 *
 * @param text 以结束符结尾的文本。
 *
 * @return 非零表示当前文本行已被本模块消费，0 表示不满足该条件。
 */
static uint8_t Line(const char *text)
{
    quiet = HAL_GetTick();
    if (strcmp(text, "CLOSED") == 0) { closed = 1U; return 1U; }
    if (strncmp(text, "+IPD,", 5U) == 0 && passive) { return 1U; }
    if (strncmp(text, "+CIPRECVLEN:", 12U) == 0) {
        if (phase != QUERY_LENGTH || !waiting || length_seen || !BufferLength(text + 12U, &available)) {
            callback_error = "可读长度应答无效或时序异常";
        } else { length_seen = 1U; }
        return 1U;
    }
    return 0U;
}

/**
 * @brief 【OTA 下载】清空旧应答后发送 AT 命令，并登记等待状态与起始时刻，供超时检测使用。
 *
 * @param command 待发送的 AT 命令字符串，包含所需换行。
 *
 * @return 非零表示AT 命令成功发送并登记等待，0 表示不满足该条件。
 */
static uint8_t Send(const char *command)
{
    EspAt_ResetResponse();
    if (EspAt_Send(command) != HAL_OK) { Fail("AT命令发送失败"); return 0U; }
    waiting = 1U; tick = HAL_GetTick();
    return 1U;
}

/**
 * @brief 【OTA 下载】进入下一个下载阶段，同时重置等待计时和 AT 应答，并映射到屏幕进度阶段。
 *
 * @param next 操作完成后进入的状态。
 *
 * @return 无返回值。
 */
static void Next(Phase next)
{
    screen.stage = next == ERASE ? OTA_PROGRESS_ERASE :
        next == VERIFY ? OTA_PROGRESS_VERIFY : next == COMMIT ? OTA_PROGRESS_COMMIT :
        next >= SET_PASSIVE && next <= PULL ? OTA_PROGRESS_DOWNLOAD : OTA_PROGRESS_PREPARE;
    phase = next; waiting = 0U; tick = HAL_GetTick(); EspAt_ResetResponse();
}

/**
 * @brief 【OTA 下载】复制通知与元数据，构建带 Host 和原始路径的 HTTP GET，注册下载回调并保存接收统计基线。
 *        1. 复制已验证通知，准备目标版本、大小和 MD5 元数据。
 *        2. 构建保留路径及查询参数的 HTTP GET，并初始化 HTTP 正文解析器。
 *        3. 保存接收统计基线，注册下载专用的数据、帧和文本回调。
 *        4. 清空本次任务状态，进入关闭旧连接阶段。
 *
 * @param n 已校验并排队的 OTA 通知。
 *
 * @return 无返回值。
 */
static void Start(const OtaNotice *n)
{
    int length;
    notice = *n; recoverable = 0U;
    ++screen.sequence; screen.total = n->size; screen.done = 0U;
    screen.stage = OTA_PROGRESS_PREPARE; screen.error[0] = '\0';
    (void)snprintf(screen.version, sizeof(screen.version), "%s", n->version);
    memset(&meta, 0, sizeof(meta));
    meta.image_size = notice.size; memcpy(meta.md5, notice.md5, 16U); strcpy(meta.version, notice.version);
    length = snprintf(request, sizeof(request),
        "GET %s HTTP/1.0\r\nHost: %s:%u\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
        notice.path, notice.host, (unsigned int)notice.port);
    if (length < 0 || length >= (int)sizeof(request) || HttpImage_Begin(notice.size) != HAL_OK) {
        Fail("GET请求生成失败或固件大小无效"); return;
    }
    request_length = (uint16_t)length;
    EspAt_GetStats(&baseline);
    EspAt_SetDataHandler(Byte); EspAt_SetFrameHandler(Frame); EspAt_SetLineHandler(Line);
    started = last_data = quiet = HAL_GetTick(); progress = erase_offset = 0U;
    closed = passive = length_seen = frame_seen = 0U; callback_error = NULL;
    Next(CLOSE_OLD);
    Log("[下载] 开始：被动TCP接收，每次最多1024字节；MQTT已暂停\r\n");
}

/**
 * @brief 【OTA 下载】获取当前 OTA 进度快照。
 *        复制进度快照给 UI。
 *        擦除阶段使用擦除偏移，下载及后续阶段使用实际写入字节数；
 *        done 和 total 的单位都是字节，stage 用于解释当前含义。
 *
 * @param out 下载阶段、目标版本及字节计数快照，可为 NULL。
 *
 * @return 无返回值。
 */
void OtaDownload_GetProgress(OtaDownloadProgress *out)
{
    if (out == NULL) return;
    *out = screen;
    if (screen.stage == OTA_PROGRESS_ERASE) out->done = erase_offset;
    else if (screen.stage >= OTA_PROGRESS_DOWNLOAD) out->done = HttpImage_Written();
    if (out->done > out->total) out->done = out->total;
}

/**
 * @brief 【OTA 下载】返回下载状态机是否占用 OTA 流程。
 *        它覆盖准备、下载、校验和提交，网络恢复模块据此避免抢占。
 *
 * @return 非零表示OTA 仍在准备、下载、校验或提交，0 表示不满足该条件。
 */
uint8_t OtaDownload_IsBusy(void) { return phase != IDLE && phase != FAILED; }
/**
 * @brief 【OTA 下载】仅当处于失败且已判定可恢复时返回非零。
 *        不是所有下载失败都允许无条件重新开始。
 *
 * @return 非零表示下载失败且允许网络恢复，0 表示不满足该条件。
 */
uint8_t OtaDownload_NeedsRecovery(void) { return phase == FAILED && recoverable; }
/**
 * @brief 【OTA 下载】再次确认 EEPROM 有效且未提交，才释放下载回调和 RAM 通知并退回 IDLE。
 *        1. 确认下载失败且已被判定为可恢复。
 *        2. 重读 EEPROM，只有有效记录且 pending 为 0 时才继续。
 *        3. 释放下载回调并清除 RAM 通知，状态退回 IDLE，等待重新发送通知。
 *
 * @return 非零表示状态已安全重置，可接收下一次通知，0 表示不满足该条件。
 */
uint8_t OtaDownload_ResetForRetry(void)
{
    OtaMeta actual;
    if (!OtaDownload_NeedsRecovery()) { return 0U; }
    if (OtaMeta_Read(&actual) != OTA_META_OK || actual.pending != 0U) {
        Log("[下载] 恢复已锁定：记录已变化或无法读取，保留暂存固件\r\n");
        recoverable = 0U; return 0U;
    }
    EspAt_SetDataHandler(NULL); EspAt_SetFrameHandler(NULL); EspAt_SetLineHandler(NULL);
    OtaNotice_Clear(); recoverable = 0U; phase = IDLE;
    return 1U;
}

/**
 * @brief 【OTA 下载】轮询推进 HTTP 固件下载、校验与提交状态机。
 *        1. 空闲时取得有效通知，并等待 MQTT 允许下载器接管单连接。
 *        2. 关闭旧连接并排空残留数据，确认 Flash 与旧安装标志状态。
 *        3. 按扇区擦除外部暂存区，设置 ESP 被动接收并连接 HTTP 服务器。
 *        4. 发送 HTTP GET，循环查询可读长度，每次最多拉取 1024 字节。
 *        5. 把收到的完整数据块交给 HTTP 解析器，按页写入外部 Flash。
 *        6. 正文收齐后刷新最后一页，验证外部 BIN 的向量和 MD5。
 *        7. 调用提交函数写入并确认 0xA5，随后复位 STM32 进入 Boot 安装。
 *        8. 任一阶段出现新串口错误、协议错误或超时，停止本次任务并判定能否恢复。
 *
 * @return 无返回值。
 */
void OtaDownload_Task(void)
{
    uint32_t now = HAL_GetTick();
    EspAt_Response response;
    char command[96], message[96];
    int n;
    if (phase == IDLE) {
        const OtaNotice *candidate = OtaNotice_Get();
        if (candidate && Cloud_PauseForOta()) { Start(candidate); }
        return;
    }
    if (phase == FAILED) { return; }
    if (!StatsValid() || callback_error) {
        Fail(callback_error ? callback_error : "串口或数据流异常，请检查串口3及硬件"); return;
    }
    if ((uint32_t)(now - started) >= 600000U) { Fail("下载总时间超时"); return; }
    response = EspAt_GetResponse();
    if (response == ESP_AT_RESPONSE_BAD_TEXT || (phase != CLOSE_OLD &&
        (response == ESP_AT_RESPONSE_ERROR || response == ESP_AT_RESPONSE_CLOSED))) {
        Fail("AT报错或连接断开，请检查最后操作"); return;
    }
    if (waiting && (uint32_t)(now - tick) >= (phase == OPEN_SOCKET ? 15000U : 5000U)) {
        Fail("等待AT应答、发送提示或发送完成超时"); return;
    }
    if (phase == CLOSE_OLD) {
        if (!waiting) { (void)Send("AT+CIPCLOSE\r\n"); }
        else if (response == ESP_AT_RESPONSE_OK || response == ESP_AT_RESPONSE_ERROR || closed) {
            Next(DRAIN); quiet = now;
        }
    } else if (phase == DRAIN) {
        if ((uint32_t)(now - quiet) >= 200U && EspAt_StreamIdle()) {
            /* 确认旧安装请求已清除后，才能擦外部旧 BIN，避免 Boot 安装半包。 */
            OtaMeta current;
            /* Never erase a staged image while an installation is pending. */
            if (OtaImage_CheckDevice() != HAL_OK || OtaMeta_Read(&current) != OTA_META_OK || current.pending != 0U) {
                Fail("Flash标识检查或清除旧安装标志失败"); return;
            }
            Log("[下载] 已确认安装标志为0，开始擦除暂存扇区\r\n");
            Next(ERASE);
        } else if ((uint32_t)(now - tick) >= 5000U) { Fail("旧连接残留数据未排空"); }
    } else if (phase == ERASE) {
        if (erase_offset < notice.size) {
            if (W25Q128_EraseSector(OTA_STAGE_BASE + erase_offset) != HAL_OK) {
                Fail("暂存扇区擦除失败"); return;
            }
            erase_offset += W25Q128_SECTOR_SIZE;
        } else { Next(SET_PASSIVE); }
    } else if (phase == SET_PASSIVE) {
        if (!waiting) { (void)Send("AT+CIPRECVMODE=1\r\n"); }
        else if (response == ESP_AT_RESPONSE_OK) {
            if (EspAt_SetPassive(1U) != HAL_OK) { Fail("数据流未空闲，无法切换接收模式"); return; }
            passive = 1U; closed = 0U; Next(OPEN_SOCKET);
        }
    } else if (phase == OPEN_SOCKET) {
        if (!waiting) {
            n = snprintf(command, sizeof(command), "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n",
                         notice.host, (unsigned int)notice.port);
            if (n < 0 || n >= (int)sizeof(command)) { Fail("连接命令过长"); return; }
            Log("[下载] 按通知中的主机和端口连接\r\n"); (void)Send(command);
        } else if (response == ESP_AT_RESPONSE_OK) { Next(SEND_SIZE); }
    } else if (phase == SEND_SIZE) {
        n = snprintf(command, sizeof(command), "AT+CIPSEND=%u\r\n", (unsigned int)request_length);
        if (n < 0 || n >= (int)sizeof(command)) { Fail("GET请求长度溢出"); return; }
        if (Send(command)) { phase = SEND_GET; }
    } else if (phase == SEND_GET) {
        if (waiting == 1U && EspAt_HasPrompt()) {
            EspAt_ResetResponse();
            if (EspAt_SendBytes((const uint8_t *)request, request_length) != HAL_OK) { Fail("GET请求发送失败"); return; }
            waiting = 2U; tick = now;
            Log("[下载] GET请求已发送，路径及查询参数已保留\r\n");
        } else if (waiting == 2U && response == ESP_AT_RESPONSE_SEND_OK) {
            last_data = now; Next(QUERY_LENGTH);
        }
    } else if (phase == QUERY_LENGTH) {
        if (!waiting && (uint32_t)(now - tick) >= 50U) {
            length_seen = 0U; (void)Send("AT+CIPRECVLEN?\r\n");
        } else if (waiting && response == ESP_AT_RESPONSE_OK) {
            if (!length_seen) { Fail("缺少可读长度应答"); return; }
            if (available) { Next(PULL); }
            else if (closed) { Fail("正文未收全时连接已关闭"); }
            else if ((uint32_t)(now - last_data) >= 30000U) { Fail("连续30秒无HTTP数据"); }
            else { Next(QUERY_LENGTH); }
        }
    } else if (phase == PULL) {
        if (!waiting) {
            /* 网络每次最多取 1024 字节；Flash 编程页仍是 256 字节，二者不同。 */
            requested = (uint16_t)(available < sizeof(chunk) ? available : sizeof(chunk));
            frame_seen = 0U; chunk_used = frame_length = 0U;
            n = snprintf(command, sizeof(command), "AT+CIPRECVDATA=%u\r\n", (unsigned int)requested);
            if (n < 0 || n >= (int)sizeof(command)) { Fail("拉取长度溢出"); return; }
            (void)Send(command);
        } else if (response == ESP_AT_RESPONSE_OK) {
            if (!frame_seen || chunk_used != frame_length) { Fail("接收帧不完整"); return; }
            if (chunk_used) {
                if (HttpImage_Feed(chunk, chunk_used) != HAL_OK) { Fail(HttpImage_Error()); return; }
                last_data = now;
            } else if ((uint32_t)(now - last_data) >= 30000U) { Fail("连续30秒拉取结果为空"); return; }
            if (HttpImage_Received() >= notice.size) {
                if (HttpImage_Finish() != HAL_OK) { Fail(HttpImage_Error()); return; }
                Next(VERIFY);
            } else { Next(QUERY_LENGTH); }
            if (HttpImage_Received() - progress >= 4096U || phase == VERIFY) {
                progress = HttpImage_Received();
                n = snprintf(message, sizeof(message), "[下载] 已接收=%lu/%lu字节 已写入=%lu字节\r\n",
                    (unsigned long)progress, (unsigned long)notice.size, (unsigned long)HttpImage_Written());
                if (n > 0 && n < (int)sizeof(message)) { Log(message); }
            }
        }
    } else if (phase == VERIFY) {
        OtaApp_Checkpoint();
        if (OtaImage_VerifyStage(&meta) != HAL_OK || !StatsValid()) {
            Fail("外部BIN的MD5或向量校验失败，未提交安装"); return;
        }
        Log("[下载] MD5及向量校验通过，外部实际BIN已验证\r\n"); Next(COMMIT);
    } else if (phase == COMMIT) {
        if (OtaImage_Commit(&meta) != HAL_OK) {
            Fail("提交或回读失败，请保留固件并检查记录后再重试"); return;
        }
        Log("[下载] 提交成功：标志=0xA5，复位进入安装器\r\n");
        OtaApp_Checkpoint();
        /* 此处只触发复位；真正改写内部 APP 的是重启后的 Boot 安装器。 */
        NVIC_SystemReset();
        phase = FAILED;
    }
}
