/**
 * @file w25q128.c
 * @brief 【SPI Flash】外部 SPI NOR Flash 驱动。
 *        APP 固件：OTA 版本从内部 Flash 0x08008000 运行，负责联网下载与业务。
 * 提供 JEDEC ID、忙状态、读取、4 KiB 扇区擦除和按页编程；
 * APP 下载与 Boot 安装通过同一暂存区交换 BIN。
 */
#include "w25q128.h"
#include "spi.h"

#define SPI_TIMEOUT_MS  100U
#define BUSY_TIMEOUT_MS 2000U

/**
 * @brief 【SPI Flash】拉低 Flash 片选，开始一条 SPI 命令。
 *        一条命令的操作码、地址和数据在同次片选内发送。
 *
 * @return 无返回值。
 */
static void Select(void)
{
    HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_RESET);
}

/**
 * @brief 【SPI Flash】拉高片选，结束本条 SPI 命令。
 *
 * @return 无返回值。
 */
static void Release(void)
{
    HAL_GPIO_WritePin(FLASH_CS_GPIO_Port, FLASH_CS_Pin, GPIO_PIN_SET);
}

/**
 * @brief 【SPI Flash】检查起始地址与长度是否落在芯片容量内，防止计算溢出或越界访问。
 *
 * @param address 器件内部起始地址或偏移，单位为字节。
 * @param length 数据长度，单位为字节。
 *
 * @return 非零表示地址和长度位于器件有效范围，0 表示不满足该条件。
 */
static int RangeValid(uint32_t address, uint32_t length)
{
    return length != 0U && address < W25Q128_CAPACITY &&
           length <= W25Q128_CAPACITY - address;
}

/**
 * @brief 【SPI Flash】调用 HAL SPI 发送指定长度的字节，不在这里解释 Flash 操作码。
 *
 * @param data 输入数据缓冲区。
 * @param length 数据长度，单位为字节。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
static HAL_StatusTypeDef Send(const uint8_t *data, uint16_t length)
{
    return HAL_SPI_Transmit(&hspi2, (uint8_t *)data, length, SPI_TIMEOUT_MS);
}

/**
 * @brief 【SPI Flash】组装操作码和三字节 Flash 地址。
 *        外部 Flash 地址是芯片内偏移，不是 MCU 的 0x08000000 地址。
 *
 * @param command 至少 4 字节，保存操作码和三字节地址。
 * @param opcode SPI Flash 命令操作码。
 * @param address 器件内部起始地址或偏移，单位为字节。
 *
 * @return 无返回值。
 */
static void AddressCommand(uint8_t *command, uint8_t opcode, uint32_t address)
{
    command[0] = opcode;
    command[1] = (uint8_t)(address >> 16);
    command[2] = (uint8_t)(address >> 8);
    command[3] = (uint8_t)address;
}

/**
 * @brief 【SPI Flash】发送 JEDEC ID 读取命令，返回厂商/器件标识，用来检查型号、接线和读数稳定性。
 *
 * @param id 读取到的 24 位 JEDEC ID。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
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

/**
 * @brief 【SPI Flash】读取状态寄存器，供忙等待和写使能确认使用。
 *
 * @param status 读取到的 Flash 状态寄存器值。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
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

/**
 * @brief 【SPI Flash】轮询忙位并设置超时，等待上一条擦除/编程结束。
 *        SPI 字节发完并不代表存储器内部写周期完成。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
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
        HAL_Delay(1U);
    }
}

/**
 * @brief 【SPI Flash】在擦除或页编程前置写使能锁存位并确认状态。
 *        每轮写操作都要遵守芯片协议。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
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

/**
 * @brief 【SPI Flash】检查范围后按地址读出数据，供 HTTP 写后验证、Boot 复制和 MD5 流式计算调用。
 *
 * @param address 器件内部起始地址或偏移，单位为字节。
 * @param data 保存读取结果的缓冲区，容量不小于 length。
 * @param length 数据长度，单位为字节。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
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

/**
 * @brief 【SPI Flash】按 4 KiB 对齐擦除一个扇区，检查写使能并等待完成。
 *        1. 检查地址是否满足 4 KiB 扇区对齐，并验证容量范围。
 *        2. 设置写使能，发送扇区擦除命令和三字节地址。
 *        3. 结束片选后等待忙位清除，再返回擦除结果。
 *
 * @param address 器件内部起始地址或偏移，单位为字节。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
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

/**
 * @brief 【SPI Flash】把数据按 256 字节页边界分段编程，每段写使能并等待完成，避免跨页回绕。
 *        1. 检查地址、数据指针和长度范围。
 *        2. 按当前地址到 256 字节页边界的剩余空间拆分数据。
 *        3. 每段先写使能，再发送页编程命令、地址和数据。
 *        4. 等待本页编程结束后继续下一页，直到数据全部写完或失败。
 *
 * @param address 器件内部起始地址或偏移，单位为字节。
 * @param data 输入数据缓冲区。
 * @param length 数据长度，单位为字节。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
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
