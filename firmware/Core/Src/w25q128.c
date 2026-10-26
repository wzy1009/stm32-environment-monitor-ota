#include "w25q128.h"
#ifdef ENV_OTA_APP
#include "ota_app.h"
#endif
#include "spi.h"

#define SPI_TIMEOUT_MS  100U
#define BUSY_TIMEOUT_MS 2000U

static void Select(void)
{
    HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET);
}

static void Release(void)
{
    HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);
}

static int RangeValid(uint32_t address, uint32_t length)
{
    return length != 0U && address < W25Q128_CAPACITY &&
           length <= W25Q128_CAPACITY - address;
}

static HAL_StatusTypeDef Send(const uint8_t *data, uint16_t length)
{
    return HAL_SPI_Transmit(&hspi2, (uint8_t *)data, length, SPI_TIMEOUT_MS);
}

static void AddressCommand(uint8_t *command, uint8_t opcode, uint32_t address)
{
    command[0] = opcode;
    command[1] = (uint8_t)(address >> 16);
    command[2] = (uint8_t)(address >> 8);
    command[3] = (uint8_t)address;
}

HAL_StatusTypeDef W25Q128_ReadJedecId(uint32_t *id)
{
    uint8_t command = 0x9FU;
    uint8_t bytes[3];
    HAL_StatusTypeDef result;
    if (id == NULL) {
        return HAL_ERROR;
    }
    Select();
    result = Send(&command, 1U);
    if (result == HAL_OK) {
        result = HAL_SPI_Receive(&hspi2, bytes, sizeof(bytes), SPI_TIMEOUT_MS);
    }
    Release();
    if (result == HAL_OK) {
        *id = ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
    }
    return result;
}

HAL_StatusTypeDef W25Q128_ReadStatus(uint8_t *status)
{
    uint8_t command = 0x05U;
    HAL_StatusTypeDef result;
    if (status == NULL) {
        return HAL_ERROR;
    }
    Select();
    result = Send(&command, 1U);
    if (result == HAL_OK) {
        result = HAL_SPI_Receive(&hspi2, status, 1U, SPI_TIMEOUT_MS);
    }
    Release();
    return result;
}

static HAL_StatusTypeDef WaitReady(void)
{
    uint32_t start = HAL_GetTick();
    uint8_t status;
    HAL_StatusTypeDef result;
    for (;;) {
        result = W25Q128_ReadStatus(&status);
        if (result != HAL_OK) {
            return result;
        }
        if ((status & 0x01U) == 0U) {
            return HAL_OK;
        }
        if ((uint32_t)(HAL_GetTick() - start) >= BUSY_TIMEOUT_MS) {
            return HAL_TIMEOUT;
        }
        #ifdef ENV_OTA_APP
        OtaApp_Checkpoint();
        #endif
        HAL_Delay(1U);
    }
}

static HAL_StatusTypeDef WriteEnable(void)
{
    uint8_t command = 0x06U;
    uint8_t status;
    HAL_StatusTypeDef result = WaitReady();
    if (result != HAL_OK) {
        return result;
    }
    Select();
    result = Send(&command, 1U);
    Release();
    if (result == HAL_OK) {
        result = W25Q128_ReadStatus(&status);
        if (result == HAL_OK && (status & 0x02U) == 0U) {
            result = HAL_ERROR;
        }
    }
    return result;
}

HAL_StatusTypeDef W25Q128_Read(uint32_t address, uint8_t *data, uint32_t length)
{
    uint8_t command[4];
    uint16_t count;
    HAL_StatusTypeDef result;
    if (data == NULL || !RangeValid(address, length)) {
        return HAL_ERROR;
    }
    result = WaitReady();
    if (result != HAL_OK) {
        return result;
    }
    AddressCommand(command, 0x03U, address);
    Select();
    result = Send(command, sizeof(command));
    while (result == HAL_OK && length != 0U) {
        count = (uint16_t)((length > 256U) ? 256U : length);
        result = HAL_SPI_Receive(&hspi2, data, count, SPI_TIMEOUT_MS);
        data += count;
        length -= count;
    }
    Release();
    return result;
}

HAL_StatusTypeDef W25Q128_EraseSector(uint32_t address)
{
    uint8_t command[4];
    HAL_StatusTypeDef result;
    if (address % W25Q128_SECTOR_SIZE != 0U ||
        !RangeValid(address, W25Q128_SECTOR_SIZE)) {
        return HAL_ERROR;
    }
    result = WriteEnable();
    if (result != HAL_OK) {
        return result;
    }
    AddressCommand(command, 0x20U, address);
    Select();
    result = Send(command, sizeof(command));
    Release();
    if (result != HAL_OK) {
        return result;
    }
    return WaitReady();
}

HAL_StatusTypeDef W25Q128_Program(uint32_t address, const uint8_t *data, uint32_t length)
{
    uint8_t command[4];
    uint16_t count;
    uint32_t page_room;
    HAL_StatusTypeDef result;
    if (data == NULL || !RangeValid(address, length)) {
        return HAL_ERROR;
    }
    while (length != 0U) {
        page_room = W25Q128_PAGE_SIZE - address % W25Q128_PAGE_SIZE;
        count = (uint16_t)((length < page_room) ? length : page_room);
        result = WriteEnable();
        if (result != HAL_OK) {
            return result;
        }
        AddressCommand(command, 0x02U, address);
        Select();
        result = Send(command, sizeof(command));
        if (result == HAL_OK) {
            result = Send(data, count);
        }
        Release();
        if (result != HAL_OK) {
            return result;
        }
        result = WaitReady();
        if (result != HAL_OK) {
            return result;
        }
        address += count;
        data += count;
        length -= count;
    }
    return HAL_OK;
}
