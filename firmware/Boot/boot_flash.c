#include "boot_flash.h"
#include <string.h>

static uint8_t InApp(uint32_t address, uint32_t length)
{
    return address >= OTA_APP_BASE && address - OTA_APP_BASE < OTA_APP_MAX_SIZE &&
           length != 0U && length <= OTA_APP_MAX_SIZE - (address - OTA_APP_BASE);
}

HAL_StatusTypeDef BootFlash_Read(uint32_t address, uint8_t *data, uint32_t length)
{
    if (data == NULL || !InApp(address, length)) return HAL_ERROR;
    memcpy(data, (const void *)address, length);
    return HAL_OK;
}

HAL_StatusTypeDef BootFlash_ErasePage(uint32_t address)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t error;
    HAL_StatusTypeDef status;
    if ((address % OTA_FLASH_PAGE_SIZE) != 0U || !InApp(address, OTA_FLASH_PAGE_SIZE)) return HAL_ERROR;
    if (HAL_FLASH_Unlock() != HAL_OK) return HAL_ERROR;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.PageAddress = address;
    erase.NbPages = 1U;
    status = HAL_FLASHEx_Erase(&erase, &error);
    if (HAL_FLASH_Lock() != HAL_OK) status = HAL_ERROR;
    return status;
}

HAL_StatusTypeDef BootFlash_Program(uint32_t address, const uint8_t *data, uint32_t length)
{
    uint32_t i;
    uint16_t value;
    HAL_StatusTypeDef status = HAL_OK;
    if (data == NULL || length == 0U || length > 256U || (address & 1U) != 0U ||
        !InApp(address, (length + 1U) & ~1U)) return HAL_ERROR;
    if (HAL_FLASH_Unlock() != HAL_OK) return HAL_ERROR;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);
    for (i = 0U; i < length; i += 2U) {
        value = data[i] | (uint16_t)((i + 1U < length ? data[i + 1U] : 0xFFU) << 8);
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, address + i, value);
        if (status != HAL_OK || *(volatile const uint16_t *)(address + i) != value) {
            status = HAL_ERROR;
            break;
        }
    }
    if (HAL_FLASH_Lock() != HAL_OK) status = HAL_ERROR;
    return status;
}
