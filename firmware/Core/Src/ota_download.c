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

static void Log(const char *text)
{
    if (HAL_UART_Transmit(&huart1, (const uint8_t *)text, (uint16_t)strlen(text), 100U) != HAL_OK) { Error_Handler(); }
}

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

static uint8_t StatsValid(void)
{
    EspAt_Stats s;
    EspAt_GetStats(&s);
    return s.overflow == baseline.overflow && s.uart_errors == baseline.uart_errors &&
           s.rearm_failures == baseline.rearm_failures && !EspAt_HasStreamFault();
}

static void Frame(uint32_t length)
{
    if (phase <= DRAIN || phase == FAILED) { return; }
    if (phase != PULL || !waiting || frame_seen || length > requested) {
        callback_error = "接收帧时序异常或长度过大"; return;
    }
    frame_seen = 1U; frame_length = (uint16_t)length; chunk_used = 0U;
}

static void Byte(uint8_t byte)
{
    quiet = HAL_GetTick();
    if (phase <= DRAIN || phase == FAILED) { return; }
    if (phase != PULL || !frame_seen || chunk_used >= frame_length || chunk_used >= sizeof(chunk)) {
        callback_error = "收到非预期TCP字节"; return;
    }
    chunk[chunk_used++] = byte;
}

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

static uint8_t Send(const char *command)
{
    EspAt_ResetResponse();
    if (EspAt_Send(command) != HAL_OK) { Fail("AT命令发送失败"); return 0U; }
    waiting = 1U; tick = HAL_GetTick();
    return 1U;
}

static void Next(Phase next)
{
    screen.stage = next == ERASE ? OTA_PROGRESS_ERASE :
        next == VERIFY ? OTA_PROGRESS_VERIFY : next == COMMIT ? OTA_PROGRESS_COMMIT :
        next >= SET_PASSIVE && next <= PULL ? OTA_PROGRESS_DOWNLOAD : OTA_PROGRESS_PREPARE;
    phase = next; waiting = 0U; tick = HAL_GetTick(); EspAt_ResetResponse();
}

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

void OtaDownload_GetProgress(OtaDownloadProgress *out)
{
    if (out == NULL) return;
    *out = screen;
    if (screen.stage == OTA_PROGRESS_ERASE) out->done = erase_offset;
    else if (screen.stage >= OTA_PROGRESS_DOWNLOAD) out->done = HttpImage_Written();
    if (out->done > out->total) out->done = out->total;
}

uint8_t OtaDownload_IsBusy(void) { return phase != IDLE && phase != FAILED; }
uint8_t OtaDownload_NeedsRecovery(void) { return phase == FAILED && recoverable; }
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
    OtaApp_Checkpoint();
    if (!OtaApp_CanInstall()) { Fail("已布防，升级取消"); return; }
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
        NVIC_SystemReset();
        phase = FAILED;
    }
}
