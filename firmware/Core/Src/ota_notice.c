/**
 * @file ota_notice.c
 * @brief 【OTA 通知】OTA JSON 通知的入口检查。
 *        APP 固件：OTA 版本从内部 Flash 0x08008000 运行，负责联网下载与业务。
 * 解析 cmd、version、url、size、md5，拒绝格式错误、同运行版本及重复通知；
 * 通过后仅保存 RAM 请求，实际下载由 ota_download.c 执行。
 */
#include "ota_notice.h"
#include "ota_layout.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

typedef struct { const uint8_t *p, *end; } Reader;
static OtaNotice saved;
static uint8_t ready;

/**
 * @brief 【OTA 通知】跳过 JSON 允许的空白字符，通过 end 指针限制读取范围。
 *
 * @param r JSON 读取游标，包含当前位置和结束位置。
 *
 * @return 无返回值。
 */
static void SkipSpace(Reader *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' ||
           *r->p == '\r' || *r->p == '\n')) { ++r->p; }
}

/**
 * @brief 【OTA 通知】跳过空白后匹配指定符号，例如冒号或右花括号。
 *        失败返回 0，不继续猜测格式。
 *
 * @param r JSON 读取游标，包含当前位置和结束位置。
 * @param c 期望匹配的 JSON 符号。
 *
 * @return 非零表示目标 JSON 符号匹配且游标已前进，0 表示不满足该条件。
 */
static int Take(Reader *r, uint8_t c)
{
    SkipSpace(r);
    if (r->p == r->end || *r->p != c) { return 0; }
    ++r->p;
    return 1;
}

/* shortcut: flat ASCII JSON without escapes; generated commands use this form. */
/**
 * @brief 【OTA 通知】读取有界 JSON 字符串到 out，capacity 包含结束符空间。
 *        该通知解析器只支持代码允许的字符形式，并不是通用 JSON 库。
 *
 * @param r JSON 读取游标，包含当前位置和结束位置。
 * @param out 用于保存字符串及结束符的缓冲区。
 * @param capacity 输出缓冲区总容量，包含字符串结束符。
 *
 * @return 非零表示JSON 字符串解析成功，0 表示不满足该条件。
 */
static int Text(Reader *r, char *out, uint16_t capacity)
{
    uint16_t n = 0U;
    if (!Take(r, '"')) { return 0; }
    while (r->p < r->end && *r->p != '"') {
        uint8_t c = *r->p++;
        if (c < 32U || c > 126U || c == '\\' || n + 1U >= capacity) { return 0; }
        out[n++] = (char)c;
    }
    if (r->p == r->end) { return 0; }
    ++r->p;
    out[n] = '\0';
    return 1;
}

/**
 * @brief 【OTA 通知】读取十进制固件大小并检查数值范围。
 *        不能把负数或溢出结果当作擦除、写入长度。
 *
 * @param r JSON 读取游标，包含当前位置和结束位置。
 * @param out 解析成功后的固件字节数。
 *
 * @return 非零表示固件大小解析并校验成功，0 表示不满足该条件。
 */
static int Number(Reader *r, uint32_t *out)
{
    uint32_t n = 0U;
    SkipSpace(r);
    if (r->p == r->end || *r->p < '1' || *r->p > '9') { return 0; }
    while (r->p < r->end && *r->p >= '0' && *r->p <= '9') {
        uint32_t digit = (uint32_t)(*r->p++ - '0');
        if (n > (OTA_APP_MAX_SIZE - digit) / 10U) { return 0; }
        n = n * 10U + digit;
    }
    if (n < 8U) { return 0; }
    *out = n;
    return 1;
}

/**
 * @brief 【OTA 通知】将一个十六进制字符转换为 0 到 15，非法字符返回负值。
 *        用于还原 MD5 与检查 URL 百分号编码。
 *
 * @param c 待检查的单个字符。
 *
 * @return 合法字符对应的 0 到 15；非法字符返回 -1。
 */
static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/**
 * @brief 【OTA 通知】检查版本字符串长度和允许字符。
 *        这里只约束格式，当前通知入口按字符串相等去重，并未实现语义化版本大小比较。
 *
 * @param s 待验证的字符串。
 *
 * @return 非零表示版本字符串合法，0 表示不满足该条件。
 */
static int VersionValid(const char *s)
{
    if (*s == '\0') { return 0; }
    for (; *s; ++s) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= 'a' && *s <= 'z') || *s == '.' || *s == '_' || *s == '-')) { return 0; }
    }
    return 1;
}

/**
 * @brief 【OTA 通知】把 HTTP URL 拆为主机、端口和路径，默认端口 80。
 *        保留查询参数并检查危险或非法字符。
 *        当前实现只接受 http://，不是 HTTPS 下载器。
 *
 * @param out 输入原始 URL，输出拆分的主机、端口和路径。
 *
 * @return 非零表示HTTP URL 拆分和检查成功，0 表示不满足该条件。
 */
static int Url(OtaNotice *out)
{
    const char *p = out->url + 7U, *start = p;
    uint16_t n, label = 0U;
    uint32_t port = 80U;
    if (strncmp(out->url, "http://", 7U) != 0) { return 0; }
    while (*p && *p != ':' && *p != '/') { ++p; }
    n = (uint16_t)(p - start);
    if (n == 0U || n >= sizeof(out->host)) { return 0; }
    memcpy(out->host, start, n); out->host[n] = '\0';
    for (n = 0U; out->host[n]; ++n) {
        char c = out->host[n];
        if (c == '.') {
            if (!label || out->host[n - 1U] == '-') { return 0; }
            label = 0U;
        } else {
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || (c == '-' && label))) { return 0; }
            ++label;
        }
    }
    if (!label || out->host[n - 1U] == '-') { return 0; }
    if (*p == ':') {
        ++p; port = 0U;
        if (*p < '1' || *p > '9') { return 0; }
        while (*p >= '0' && *p <= '9') {
            port = port * 10U + (uint32_t)(*p++ - '0');
            if (port > 65535U) { return 0; }
        }
    }
    if (*p != '/') { return 0; }
    for (start = p; *start; ++start) {
        if (*start <= 32 || *start > 126 || *start == '#' || *start == '"' || *start == '\\') { return 0; }
        if (*start == '%') {
            if (start[1] == '\0' || start[2] == '\0' ||
                HexDigit(start[1]) < 0 || HexDigit(start[2]) < 0) { return 0; }
            start += 2;
        }
    }
    strcpy(out->path, p);
    out->port = (uint16_t)port;
    return 1;
}

/**
 * @brief 【OTA 通知】解析并验证 MQTT 载荷中的 OTA 升级通知。
 *        1. 检查输入指针、载荷长度以及 JSON 对象结构。
 *        2. 识别 cmd、version、url、size、md5，拒绝未知字段和重复字段。
 *        3. 验证大小与版本格式，将 32 位十六进制 MD5 文本转换为 16 字节摘要。
 *        4. 检查必需字段和输入结尾，再拆分 HTTP 主机、端口及路径。
 *        5. 全部通过后才填充输出通知，未通过时返回对应解析状态。
 *
 * @param data 输入数据缓冲区。
 * @param length 数据长度，单位为字节。
 * @param out 通知有效时填充的解析结果。
 *
 * @return 返回状态：
 *         - OTA_NOTICE_VALID：有效的 OTA 通知。
 *         - OTA_NOTICE_OTHER：非 OTA 命令。
 *         - OTA_NOTICE_INVALID：通知格式、字段或范围无效。
 */
OtaNoticeResult OtaNotice_Parse(const uint8_t *data, uint16_t length, OtaNotice *out)
{
    Reader r;
    OtaNotice result;
    char key[16], command[24], digest[33];
    uint8_t seen = 0U, bit;
    uint16_t i;
    if (!data || !out || !length || length > OTA_NOTICE_MAX_PAYLOAD) { return OTA_NOTICE_INVALID; }
    memset(&result, 0, sizeof(result));
    r.p = data; r.end = data + length;
    if (!Take(&r, '{')) { return OTA_NOTICE_INVALID; }
    for (;;) {
        if (!Text(&r, key, sizeof(key)) || !Take(&r, ':')) { return OTA_NOTICE_INVALID; }
        if (strcmp(key, "cmd") == 0) { bit = 1U; }
        else if (strcmp(key, "version") == 0) { bit = 2U; }
        else if (strcmp(key, "url") == 0) { bit = 4U; }
        else if (strcmp(key, "size") == 0) { bit = 8U; }
        else if (strcmp(key, "md5") == 0) { bit = 16U; }
        else { return OTA_NOTICE_INVALID; }
        if (seen & bit) { return OTA_NOTICE_INVALID; }
        seen |= bit;
        if (bit == 1U) {
            if (!Text(&r, command, sizeof(command))) { return OTA_NOTICE_INVALID; }
            if (strcmp(command, "ota") != 0) { return OTA_NOTICE_OTHER; }
        } else if (bit == 2U) {
            if (!Text(&r, result.version, sizeof(result.version)) || !VersionValid(result.version)) { return OTA_NOTICE_INVALID; }
        } else if (bit == 4U) {
            if (!Text(&r, result.url, sizeof(result.url))) { return OTA_NOTICE_INVALID; }
        } else if (bit == 8U) {
            if (!Number(&r, &result.size)) { return OTA_NOTICE_INVALID; }
        } else {
            if (!Text(&r, digest, sizeof(digest)) || strlen(digest) != 32U) { return OTA_NOTICE_INVALID; }
            for (i = 0U; i < 16U; ++i) {
                int high = HexDigit(digest[i * 2U]), low = HexDigit(digest[i * 2U + 1U]);
                if (high < 0 || low < 0) { return OTA_NOTICE_INVALID; }
                result.md5[i] = (uint8_t)(high * 16 + low);
            }
        }
        SkipSpace(&r);
        if (r.p < r.end && *r.p == ',') { ++r.p; continue; }
        if (!Take(&r, '}')) { return OTA_NOTICE_INVALID; }
        break;
    }
    SkipSpace(&r);
    if (r.p != r.end || seen != 31U || !Url(&result)) { return OTA_NOTICE_INVALID; }
    *out = result;
    return OTA_NOTICE_VALID;
}

/**
 * @brief 【OTA 通知】输出通知接收、拒绝及去重原因，不在这里执行安装。
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
 * @brief 【OTA 通知】处理 OTA 通知并将有效请求加入 RAM 队列。
 *        1. 调用 OtaNotice_Parse() 识别并验证 OTA 通知。
 *        2. 拒绝与当前运行版本相同的通知，检查重复请求和待处理请求冲突。
 *        3. 把有效通知复制到模块 RAM 并置 ready 标志。
 *        4. 输出接收日志，等待下载状态机取得请求；本函数不下载或安装 BIN。
 *
 * @param data 输入数据缓冲区。
 * @param length 数据长度，单位为字节。
 * @param running_version 当前运行 APP 的版本文本。
 *
 * @return 非零表示已处理或拒绝该 OTA 命令；0 表示交由其他命令处理。
 */
uint8_t OtaNotice_OnCommand(const uint8_t *data, uint16_t length, const char *running_version)
{
    OtaNotice candidate;
    OtaNoticeResult result = OtaNotice_Parse(data, length, &candidate);
    char line[192], hex[33];
    uint16_t i;
    int n;
    if (result == OTA_NOTICE_OTHER) { return 0U; }
    if (result != OTA_NOTICE_VALID) {
        Log("[升级] 已拒绝：JSON、字段、HTTP地址、大小或MD5无效\r\n"); return 1U;
    }
    if (strcmp(candidate.version, running_version) == 0) {
        Log("[升级] 已忽略：与当前运行版本相同\r\n"); return 1U;
    }
    if (ready) {
        if (candidate.size == saved.size && strcmp(candidate.version, saved.version) == 0 &&
            strcmp(candidate.url, saved.url) == 0 && memcmp(candidate.md5, saved.md5, 16U) == 0) {
            Log("[升级] 已忽略：重复通知\r\n");
        } else { Log("[升级] 正忙：已有待处理下载通知\r\n"); }
        return 1U;
    }
    saved = candidate; ready = 1U;
    for (i = 0U; i < 16U; ++i) {
        hex[i * 2U] = "0123456789abcdef"[saved.md5[i] >> 4];
        hex[i * 2U + 1U] = "0123456789abcdef"[saved.md5[i] & 15U];
    }
    hex[32] = '\0';
    n = snprintf(line, sizeof(line), "[升级] 通知有效：版本=%s 大小=%lu字节\r\n[升级] MD5=%s\r\n",
                 saved.version, (unsigned long)saved.size, hex);
    if (n < 0 || n >= (int)sizeof(line)) { Error_Handler(); return 1U; }
    Log(line);
    n = snprintf(line, sizeof(line), "[升级] HTTP主机=%s 端口=%u，路径及查询参数已保存\r\n",
                 saved.host, (unsigned int)saved.port);
    if (n < 0 || n >= (int)sizeof(line)) { Error_Handler(); return 1U; }
    Log(line);
    Log("[升级] 通知已存入RAM，等待MQTT空闲后下载\r\n");
    return 1U;
}

/**
 * @brief 【OTA 通知】返回待下载通知的只读指针。
 *        无通知时返回 NULL。
 *        下载状态机取得它后仍需接管连接并校验镜像。
 *
 * @return 当前待下载通知的只读指针；没有通知时为 NULL。
 */
const OtaNotice *OtaNotice_Get(void) { return ready ? &saved : NULL; }
/**
 * @brief 【OTA 通知】清除 RAM 中的通知与 ready 状态，允许恢复网络后接收新的请求。
 *        不会清除 EEPROM 的正式安装标志。
 *
 * @return 无返回值。
 */
void OtaNotice_Clear(void) { ready = 0U; memset(&saved, 0, sizeof(saved)); }
