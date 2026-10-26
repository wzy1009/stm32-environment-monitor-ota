#ifndef W25Q128_H
#define W25Q128_H
#include "main.h"

#define W25Q128_CAPACITY    0x01000000UL
#define W25Q128_SECTOR_SIZE 4096UL
#define W25Q128_PAGE_SIZE   256UL

HAL_StatusTypeDef W25Q128_ReadJedecId(uint32_t *id);
HAL_StatusTypeDef W25Q128_ReadStatus(uint8_t *status);
HAL_StatusTypeDef W25Q128_Read(uint32_t address, uint8_t *data, uint32_t length);
HAL_StatusTypeDef W25Q128_EraseSector(uint32_t address);
HAL_StatusTypeDef W25Q128_Program(uint32_t address, const uint8_t *data, uint32_t length);
#endif
