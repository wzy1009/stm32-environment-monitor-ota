/**
 * @file http_image.c
 * @brief 【HTTP 固件】HTTP 响应解析与外部 Flash 落盘。
 *        APP 固件：OTA 版本从内部 Flash 0x08008000 运行，负责联网下载与业务。
 * 只把响应正文当作固件；
 * 验证状态码和 Content-Length，按 256 字节页缓存写入暂存区，并分别统计收到和写入的字节数。
 */
#include "http_image.h"
#include "ota_layout.h"
#include "w25q128.h"
#include <string.h>

static char line[512];
static uint8_t page[256];
static uint16_t used, page_used;
static uint32_t expected, received, written, header_bytes;
static uint8_t status_seen, length_seen, body, cr;
static const char *error;

/**
 * @brief 【HTTP 固件】保存 HTTP 或外部存储错误原因并返回 HAL_ERROR，供上层停止下载。
 *
 * @param reason 错误或状态原因。
 *
 * @return HAL_ERROR。
 */
static HAL_StatusTypeDef Fail(const char *reason)
{
    error = reason;
    return HAL_ERROR;
}

/**
 * @brief 【HTTP 固件】比较 HTTP 字段名等文本，规范化首个参数的 ASCII 大小写。
 *        调用方使用小写关键字，不影响固件正文。
 *
 * @param a 待比较的 HTTP 字段或值，比较时规范化大小写。
 * @param b 预期的小写 ASCII 文本。
 *
 * @return 非零表示两段文本按当前比较规则匹配，0 表示不满足该条件。
 */
static int Equal(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        char c = *a;
        if (c >= 'A' && c <= 'Z') { c = (char)(c + ('a' - 'A')); }
        if (c != *b) { return 0; }
    }
    return *a == *b;
}

/**
 * @brief 【HTTP 固件】处理一条已收齐的 HTTP 状态行/头字段。
 *        1. 第一行只接受 HTTP/1.0 或 HTTP/1.1 的 200 状态。
 *        2. 其余头字段按冒号拆分并检查名称、编码和 Content-Length。
 *        3. 拒绝 chunked、压缩和与升级通知不一致的正文长度。
 *        4. 遇到空行且长度字段有效时，切换为固件正文接收状态。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
static HAL_StatusTypeDef Header(void)
{
    char *colon, *value, *end, *p;
    uint32_t size = 0U;
    if (!status_seen) {
        if (used < 12U || (strncmp(line, "HTTP/1.0 200", 12U) != 0 &&
            strncmp(line, "HTTP/1.1 200", 12U) != 0) || (used > 12U && line[12] != ' ')) {
            return Fail("HTTP状态码不是200");
        }
        status_seen = 1U;
        return HAL_OK;
    }
    if (!used) {
        if (!length_seen) { return Fail("缺少正文长度字段"); }
        body = 1U;
        return HAL_OK;
    }
    colon = strchr(line, ':');
    if (!colon || colon == line) { return Fail("HTTP响应头无效"); }
    for (p = line; p < colon; ++p) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-')) { return Fail("HTTP头字段名无效"); }
    }
    *colon = '\0'; value = colon + 1;
    while (*value == ' ' || *value == '\t') { ++value; }
    end = value + strlen(value);
    while (end > value && (end[-1] == ' ' || end[-1] == '\t')) { *--end = '\0'; }
    if (Equal(line, "transfer-encoding")) { return Fail("不支持HTTP分块传输"); }
    if (Equal(line, "content-encoding") && !Equal(value, "identity")) { return Fail("不支持压缩响应"); }
    if (Equal(line, "content-length")) {
        if (length_seen || !*value) { return Fail("正文长度字段重复或为空"); }
        for (p = value; *p; ++p) {
            uint32_t digit;
            if (*p < '0' || *p > '9') { return Fail("正文长度字段无效"); }
            digit = (uint32_t)(*p - '0');
            if (size > (OTA_APP_MAX_SIZE - digit) / 10U) { return Fail("正文长度超出APP分区"); }
            size = size * 10U + digit;
        }
        if (size != expected) { return Fail("正文长度与通知不符"); }
        length_seen = 1U;
    }
    return HAL_OK;
}

/**
 * @brief 【HTTP 固件】把页缓存中的数据写到外部暂存区，并在写成功后推进 written。
 *        末尾不足 256 字节也由此写入；
 *        不是每收到一个网络包就立即写一整页。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
static HAL_StatusTypeDef Flush(void)
{
    if (page_used) {
        if (written > expected || page_used > expected - written ||
            W25Q128_Program(OTA_STAGE_BASE + written, page, page_used) != HAL_OK) {
            return Fail("外部Flash写入失败");
        }
        written += page_used; page_used = 0U;
    }
    return HAL_OK;
}

/**
 * @brief 【HTTP 固件】设置期望 BIN 大小并清空解析状态，大小单位为字节。
 *        仅初始化 RAM，不擦除外部 Flash。
 *
 * @param size 预期 BIN 正文大小，单位为字节。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
HAL_StatusTypeDef HttpImage_Begin(uint32_t size)
{
    expected = size; received = written = header_bytes = 0U;
    used = page_used = 0U; status_seen = length_seen = body = cr = 0U; error = NULL;
    if (size < 8U || size > OTA_APP_MAX_SIZE) { return Fail("固件大小无效"); }
    return HAL_OK;
}

/**
 * @brief 【HTTP 固件】解析 HTTP 字节块并按页写入固件正文。
 *        1. 按当前状态区分 HTTP 响应头和固件正文。
 *        2. 响应头按 CRLF 组行，调用 Header() 检查状态码和长度。
 *        3. 进入正文后累计 received，把 BIN 字节填入 256 字节页缓存。
 *        4. 缓存写满时调用 Flush()，成功后推进 written；禁止正文超过声明大小。
 *
 * @param data 输入数据缓冲区。
 * @param length 数据长度，单位为字节。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
HAL_StatusTypeDef HttpImage_Feed(const uint8_t *data, uint16_t length)
{
    uint16_t i;
    if (error || !data || !length) { return HAL_ERROR; }
    for (i = 0U; i < length; ++i) {
        uint8_t c = data[i];
        if (body) {
            if (received >= expected) { return Fail("正文超出声明长度"); }
            page[page_used++] = c; ++received;
            if (page_used == sizeof(page) && Flush() != HAL_OK) { return HAL_ERROR; }
        } else {
            if (++header_bytes > 4096U) { return Fail("HTTP响应头超过4096字节"); }
            if (cr) {
                if (c != '\n') { return Fail("HTTP换行格式无效"); }
                line[used] = '\0';
                if (Header() != HAL_OK) { return HAL_ERROR; }
                used = 0U; cr = 0U;
            } else if (c == '\r') { cr = 1U; }
            else if (c == '\n' || (c < 32U && c != '\t') || c > 126U || used + 1U >= sizeof(line)) {
                return Fail("HTTP头行无效或过长");
            } else { line[used++] = (char)c; }
        }
    }
    return HAL_OK;
}

/**
 * @brief 【HTTP 固件】在正文收全后检查长度并写出最后一页余量。
 *        1. 确认响应头已完整结束，且收到的正文字节数与预期一致。
 *        2. 写出最后不足 256 字节的页缓存余量。
 *        3. 比较最终写入字节数，长度一致后返回成功。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
HAL_StatusTypeDef HttpImage_Finish(void)
{
    if (error || !body || received != expected) { return Fail("HTTP响应头或正文不完整"); }
    if (Flush() != HAL_OK) { return HAL_ERROR; }
    return written == expected ? HAL_OK : Fail("写入长度不一致");
}

/**
 * @brief 【HTTP 固件】返回已接收的固件正文数量，不包含 HTTP 响应头，单位为字节。
 *
 * @return 累计收到的固件正文字节数，不含 HTTP 响应头。
 */
uint32_t HttpImage_Received(void) { return received; }
/**
 * @brief 【HTTP 固件】返回已成功写入外部 Flash 的正文数量。
 *        received 与 written 的差通常是 RAM 中未刷出的页尾数据，不是一个余数计算函数。
 *
 * @return 已成功写入外部 Flash 的固件字节数。
 */
uint32_t HttpImage_Written(void) { return written; }
/**
 * @brief 【HTTP 固件】返回最近一次错误的说明，供下载器输出日志。
 *        未记录特定错误时返回通用文本。
 *
 * @return 最近错误的说明字符串；无特定记录时返回通用错误文本。
 */
const char *HttpImage_Error(void) { return error ? error : "HTTP固件处理错误"; }
