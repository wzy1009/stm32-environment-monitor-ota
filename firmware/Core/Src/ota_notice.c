#include "ota_notice.h"
#include "ota_layout.h"
#include <string.h>

typedef struct { const uint8_t *p, *end; } Reader;
static OtaNotice saved;
static uint8_t ready;

static void SkipSpace(Reader *r)
{
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' ||
           *r->p == '\r' || *r->p == '\n')) { ++r->p; }
}

static int Take(Reader *r, uint8_t c)
{
    SkipSpace(r);
    if (r->p == r->end || *r->p != c) { return 0; }
    ++r->p;
    return 1;
}

/* shortcut: flat ASCII JSON without escapes; generated commands use this form. */
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

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

static int VersionValid(const char *s)
{
    if (*s == '\0') { return 0; }
    for (; *s; ++s) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= 'a' && *s <= 'z') || *s == '.' || *s == '_' || *s == '-')) { return 0; }
    }
    return 1;
}

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

uint8_t OtaNotice_OnCommand(const uint8_t *data, uint16_t length, const char *running_version)
{
    OtaNotice candidate;
    OtaNoticeResult result = OtaNotice_Parse(data, length, &candidate);
    if (result == OTA_NOTICE_OTHER) return 0U;
    if (result != OTA_NOTICE_VALID || strcmp(candidate.version, running_version) == 0 || ready != 0U)
        return 1U;
    saved = candidate;
    ready = 1U;
    return 1U;
}

const OtaNotice *OtaNotice_Get(void) { return ready ? &saved : NULL; }
void OtaNotice_Clear(void) { ready = 0U; memset(&saved, 0, sizeof(saved)); }
