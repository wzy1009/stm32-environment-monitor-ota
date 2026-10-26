#ifndef BOOT_FLASH_H
#define BOOT_FLASH_H
#include "main.h"
#include "ota_meta.h"
HAL_StatusTypeDef BootFlash_Read(uint32_t address, uint8_t *data, uint32_t length);
HAL_StatusTypeDef BootFlash_ErasePage(uint32_t address);
HAL_StatusTypeDef BootFlash_Program(uint32_t address, const uint8_t *data, uint32_t length);
HAL_StatusTypeDef BootInstall_Run(const OtaMeta *meta);
void Boot_Show(const char *stage, uint32_t done, uint32_t total);
#endif
