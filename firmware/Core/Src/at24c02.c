/**
 * @file at24c02.c
 * @brief 【EEPROM】AT24C02 EEPROM 的 I2C 读写驱动。
 *        APP 固件：OTA 版本从内部 Flash 0x08008000 运行，负责联网下载与业务。
 * 保存断电不丢失的 OTA 元数据；
 * 负责地址范围、页写入和写周期等待，不负责解释升级记录。
 */
#include "at24c02.h"
#include "i2c.h"

#define TRANSFER_TIMEOUT_MS 100U
#define READY_TIMEOUT_MS 20U

/**
 * @brief 【EEPROM】检查 EEPROM 地址和长度是否在容量内，防止越界破坏其他记录。
 *
 * @param address 器件内部起始地址或偏移，单位为字节。
 * @param length 数据长度，单位为字节。
 *
 * @return 非零表示地址和长度位于器件有效范围，0 表示不满足该条件。
 */
static int RangeValid(uint32_t address, uint32_t length)
{
    return length != 0U && address < AT24C02_CAPACITY &&
           length <= AT24C02_CAPACITY - address;
}

/**
 * @brief 【EEPROM】等待 EEPROM 内部写周期结束并探测应答。
 *        I2C 传输完成后器件可能仍短暂忙，需带超时轮询。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
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

/**
 * @brief 【EEPROM】探测 EEPROM 是否应答，供自检判断器件与总线是否可用。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
HAL_StatusTypeDef At24c02_Probe(void)
{
    return WaitReady();
}

/**
 * @brief 【EEPROM】按 EEPROM 内部地址读取明确长度的数据，返回 HAL 状态，记录格式由 ota_meta.c 解码。
 *
 * @param address 器件内部起始地址或偏移，单位为字节。
 * @param data 保存读取结果的缓冲区，容量不小于 length。
 * @param length 数据长度，单位为字节。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
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

/**
 * @brief 【EEPROM】按 EEPROM 页边界拆分写入，并等待每页写周期完成。
 *        1. 检查 EEPROM 地址、输入指针和长度范围。
 *        2. 按 EEPROM 的页边界拆分写入，避免跨页回绕。
 *        3. 每页发送后等待器件写周期完成，再写下一页。
 *
 * @param address 器件内部起始地址或偏移，单位为字节。
 * @param data 输入数据缓冲区。
 * @param length 数据长度，单位为字节。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
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
