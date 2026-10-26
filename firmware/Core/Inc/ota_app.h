#ifndef OTA_APP_H
#define OTA_APP_H
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "0.4.5"
#endif
#include <stdint.h>
void OtaApp_Checkpoint(void);
uint8_t OtaApp_CanInstall(void);
#endif
