#include "at24c02.h"
#include "i2c.h"

#define TRANSFER_TIMEOUT_MS 100U
#define READY_TIMEOUT_MS 20U

static int RangeValid(uint32_t address, uint32_t length)
{
    return length != 0U && address < AT24C02_CAPACITY &&
           length <= AT24C02_CAPACITY - address;
}

static HAL_StatusTypeDef WaitReady(void)
{
    uint32_t start = HAL_GetTick();
    uint32_t error;
    HAL_StatusTypeDef result;
    for (;;) {
        result = HAL_I2C_IsDeviceReady(&hi2c1, AT24C02_HAL_ADDRESS, 1U, 2U);
        if (result == HAL_OK) {
            return HAL_OK;
        }
        error = HAL_I2C_GetError(&hi2c1);
        /* CubeF1 readiness NACK can return HAL_ERROR with ErrorCode still zero. */
        if (result != HAL_ERROR ||
            (error != HAL_I2C_ERROR_NONE && error != HAL_I2C_ERROR_AF)) {
            return result;
        }
        if ((uint32_t)(HAL_GetTick() - start) >= READY_TIMEOUT_MS) {
            return HAL_TIMEOUT;
        }
        HAL_Delay(1U);
    }
}

HAL_StatusTypeDef At24c02_Probe(void)
{
    return WaitReady();
}

HAL_StatusTypeDef At24c02_Read(uint32_t address, uint8_t *data, uint32_t length)
{
    HAL_StatusTypeDef result;
    if (data == NULL || !RangeValid(address, length)) {
        return HAL_ERROR;
    }
    result = WaitReady();
    if (result != HAL_OK) {
        return result;
    }
    return HAL_I2C_Mem_Read(&hi2c1, AT24C02_HAL_ADDRESS, (uint16_t)address,
                            I2C_MEMADD_SIZE_8BIT, data, (uint16_t)length,
                            TRANSFER_TIMEOUT_MS);
}

HAL_StatusTypeDef At24c02_Write(uint32_t address, const uint8_t *data, uint32_t length)
{
    HAL_StatusTypeDef result;
    if (data == NULL || !RangeValid(address, length)) {
        return HAL_ERROR;
    }
    result = WaitReady();
    if (result != HAL_OK) {
        return result;
    }
    while (length != 0U) {
        /* Byte writes avoid assuming a page size for an unconfirmed 24C02 variant. */
        result = HAL_I2C_Mem_Write(&hi2c1, AT24C02_HAL_ADDRESS, (uint16_t)address,
                                   I2C_MEMADD_SIZE_8BIT, (uint8_t *)data, 1U,
                                   TRANSFER_TIMEOUT_MS);
        if (result != HAL_OK) {
            return result;
        }
        result = WaitReady();
        if (result != HAL_OK) {
            return result;
        }
        ++address;
        ++data;
        --length;
    }
    return HAL_OK;
}
