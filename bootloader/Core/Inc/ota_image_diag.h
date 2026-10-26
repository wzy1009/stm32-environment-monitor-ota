#ifndef OTA_IMAGE_DIAG_H
#define OTA_IMAGE_DIAG_H
#include "ota_image.h"

typedef enum {
    OTA_IMAGE_DIAG_OK = 0,
    OTA_IMAGE_DIAG_META,
    OTA_IMAGE_DIAG_ID_IO,
    OTA_IMAGE_DIAG_ID_UNSUPPORTED,
    OTA_IMAGE_DIAG_ID_UNSTABLE,
    OTA_IMAGE_DIAG_VECTOR_READ,
    OTA_IMAGE_DIAG_VECTOR_INVALID,
    OTA_IMAGE_DIAG_DATA_READ,
    OTA_IMAGE_DIAG_MD5
} OtaImageDiagReason;

typedef struct {
    OtaImageDiagReason reason;
    HAL_StatusTypeDef hal_status;
    uint32_t id[3];
    uint32_t id_count;
    uint32_t offset;
    uint32_t msp;
    uint32_t reset;
    uint8_t actual_md5[16];
} OtaImageDiag;

HAL_StatusTypeDef OtaImage_VerifyStageDetailed(const OtaMeta *meta, OtaImageDiag *diag);
#endif
