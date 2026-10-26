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

static HAL_StatusTypeDef Fail(const char *reason)
{
    error = reason;
    return HAL_ERROR;
}

static int Equal(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b) {
        char c = *a;
        if (c >= 'A' && c <= 'Z') { c = (char)(c + ('a' - 'A')); }
        if (c != *b) { return 0; }
    }
    return *a == *b;
}

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

HAL_StatusTypeDef HttpImage_Begin(uint32_t size)
{
    expected = size; received = written = header_bytes = 0U;
    used = page_used = 0U; status_seen = length_seen = body = cr = 0U; error = NULL;
    if (size < 8U || size > OTA_APP_MAX_SIZE) { return Fail("固件大小无效"); }
    return HAL_OK;
}

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

HAL_StatusTypeDef HttpImage_Finish(void)
{
    if (error || !body || received != expected) { return Fail("HTTP响应头或正文不完整"); }
    if (Flush() != HAL_OK) { return HAL_ERROR; }
    return written == expected ? HAL_OK : Fail("写入长度不一致");
}

uint32_t HttpImage_Received(void) { return received; }
uint32_t HttpImage_Written(void) { return written; }
const char *HttpImage_Error(void) { return error ? error : "HTTP固件处理错误"; }
