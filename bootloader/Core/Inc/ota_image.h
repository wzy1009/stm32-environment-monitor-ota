#ifndef OTA_IMAGE_H
#define OTA_IMAGE_H
#include "main.h"
#include "ota_meta.h"

HAL_StatusTypeDef OtaImage_CheckDevice(void);
HAL_StatusTypeDef OtaImage_VerifyStage(const OtaMeta *meta);
HAL_StatusTypeDef OtaImage_VerifyInternal(const OtaMeta *meta);
HAL_StatusTypeDef OtaImage_Commit(const OtaMeta *meta);

#endif
