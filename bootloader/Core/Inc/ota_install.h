#ifndef OTA_INSTALL_H
#define OTA_INSTALL_H
#include "ota_image.h"

/* 1: pause 10s after the first programmed chunk for a controlled power-cut test. */
#define OTA_INSTALL_POWER_CUT_TEST 0U

HAL_StatusTypeDef OtaInstall_Run(const OtaMeta *meta);

#endif
