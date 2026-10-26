#ifndef OTA_META_H
#define OTA_META_H

#include <stdint.h>
#include "ota_layout.h"

typedef enum {
    OTA_META_OK = 0,
    OTA_META_EMPTY,
    OTA_META_INVALID,
    OTA_META_IO_ERROR
} OtaMetaResult;

/* RAM representation only; EEPROM bytes are encoded explicitly. */
typedef struct {
    uint32_t image_size;
    uint8_t md5[16];
    char version[32];
    uint8_t pending;
} OtaMeta;

uint32_t OtaMeta_Crc32(const uint8_t *data, uint32_t length);
OtaMetaResult OtaMeta_Encode(const OtaMeta *meta, uint8_t raw[OTA_RECORD_SIZE]);
OtaMetaResult OtaMeta_Decode(const uint8_t raw[OTA_RECORD_SIZE], OtaMeta *meta);
OtaMetaResult OtaMeta_Read(OtaMeta *meta);
OtaMetaResult OtaMeta_ClearPending(void);
/* Draft only: deliberately cannot set the install flag. */
OtaMetaResult OtaMeta_WriteDraft(const OtaMeta *meta);

#endif
