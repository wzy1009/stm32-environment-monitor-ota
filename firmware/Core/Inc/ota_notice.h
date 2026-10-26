#ifndef OTA_NOTICE_H
#define OTA_NOTICE_H
#include <stdint.h>

#define OTA_NOTICE_URL_CAPACITY 192U
#define OTA_NOTICE_MAX_PAYLOAD 360U
typedef struct {
    char version[32];
    char url[OTA_NOTICE_URL_CAPACITY];
    char host[64];
    char path[OTA_NOTICE_URL_CAPACITY];
    uint32_t size;
    uint16_t port;
    uint8_t md5[16];
} OtaNotice;
typedef enum {
    OTA_NOTICE_INVALID, OTA_NOTICE_OTHER, OTA_NOTICE_VALID
} OtaNoticeResult;

OtaNoticeResult OtaNotice_Parse(const uint8_t *data, uint16_t length, OtaNotice *out);
/* Returns 0 for another command; accepted parameters stay in RAM until reset. */
uint8_t OtaNotice_OnCommand(const uint8_t *data, uint16_t length, const char *running_version);
const OtaNotice *OtaNotice_Get(void);
void OtaNotice_Clear(void);
#endif
