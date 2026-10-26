/**
 * @file boot.c
 * @brief 【Boot 启动】Boot 启动与升级决策。
 *        Boot 固件：在内部 Flash 0x08000000 启动，负责安装和转交控制权。
 * 读取 EEPROM，完成受一次性标记控制的新板初始化；
 * 有 A5 则安装，否则检查 APP 向量、清理运行环境并交给 boot_enter.c 跳转。
 */
#include "boot.h"
#include "main.h"
#include "ota_meta.h"
#include "ota_install.h"
#include "usart.h"
#include "i2c.h"
#include "spi.h"
#include <stdio.h>
#include <string.h>

extern void Boot_Enter(uint32_t msp, uint32_t reset);

/* Kept in the last Boot page by boot_layout.sct; OTA never erases it. */
#define SETUP_REQUEST_0 0xA55AU
#define SETUP_REQUEST_1 0x5AA5U
#define SETUP_REQUEST_2 0xC33CU
#define SETUP_REQUEST_3 0x3CC3U
const volatile uint16_t BootSetupRequest[4]
    __attribute__((section("BOOT_SETUP"), used)) = {
        SETUP_REQUEST_0, SETUP_REQUEST_1, SETUP_REQUEST_2, SETUP_REQUEST_3
    };

/**
 * @brief 【Boot 启动】检查 8 字节首次初始化请求是否完整。
 *        只有完整请求才允许把无效 EEPROM 记录重建为初始占位记录，CRC 错误本身不代表新板。
 *
 * @return 非零表示完整的一次性首次初始化请求仍存在，0 表示不满足该条件。
 */
static int SetupRequested(void)
{
    return BootSetupRequest[0] == SETUP_REQUEST_0 &&
           BootSetupRequest[1] == SETUP_REQUEST_1 &&
           BootSetupRequest[2] == SETUP_REQUEST_2 &&
           BootSetupRequest[3] == SETUP_REQUEST_3;
}

/**
 * @brief 【Boot 启动】关闭一次性初始化入口。
 *        1. 解锁内部 Flash，依次把首次初始化标记的四个半字写为 0。
 *        2. 重新锁定 Flash，再逐项回读确认标记已全部关闭。
 *        3. 只清除标记位，不擦除 Flash 页；中途失败则返回错误，等待复位后处理。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
static HAL_StatusTypeDef FinishSetup(void)
{
    uint32_t i;
    HAL_StatusTypeDef status = HAL_OK;
    if (HAL_FLASH_Unlock() != HAL_OK) return HAL_ERROR;
    /* Only clear bits: a reset during this step can finish with valid metadata. */
    for (i = 0U; i < 4U; ++i) {
        if (BootSetupRequest[i] != 0U &&
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                              BOOT_SETUP_ADDRESS + 2U * i, 0U) != HAL_OK) {
            status = HAL_ERROR;
            break;
        }
    }
    if (HAL_FLASH_Lock() != HAL_OK) status = HAL_ERROR;
    for (i = 0U; i < 4U; ++i) {
        if (BootSetupRequest[i] != 0U) status = HAL_ERROR;
    }
    return status;
}


/**
 * @brief 【Boot 启动】通过调试串口输出 Boot 日志。
 *        用于区分启动决策、安装和 APP 启动三个阶段。
 *
 * @param text 以结束符结尾的文本。
 *
 * @return 无返回值。
 */
static void Log(const char *text)
{
    if (HAL_UART_Transmit(&huart1, (const uint8_t *)text,
                         (uint16_t)strlen(text), 200U) != HAL_OK) {
        Error_Handler();
    }
}

/**
 * @brief 【Boot 启动】记录停止原因并返回上层。
 *        这里不会强行跳入可能无效的 APP，也不会因报错而擅自重建升级记录。
 *
 * @param reason 错误或状态原因。
 *
 * @return 无返回值。
 */
static void Stop(const char *reason)
{
    Log("[启动] 已停止：");
    Log(reason);
    Log("；已停机，请检查硬件、记录，或用FlyMCU恢复联合HEX\r\n");
}

/**
 * @brief 【Boot 启动】执行 Boot 启动检查、安装决策与 APP 跳转。
 *        1. 读取 EEPROM 升级记录，区分读取失败、记录无效与有效记录。
 *        2. 按 Boot 内的一次性请求处理新板初始化，写入后回读并关闭初始化入口。
 *        3. 若 pending 为 0xA5，调用安装器处理外部暂存固件。
 *        4. 无待安装任务时，检查 APP 的初始 MSP 和复位入口。
 *        5. 关闭 Boot 外设和中断，停用 SysTick 并清理中断挂起状态。
 *        6. 将 VTOR 指向 APP 向量表，再调用 Boot_Enter() 转交执行。
 *
 * @return 无返回值。
 */
void Boot_Run(void)
{
    OtaMeta meta;
    OtaMetaResult result;
    uint32_t msp;
    uint32_t reset;
    uint32_t i;
    char line[120];
    Log("[启动] 开始，离线安装器已启用\r\n");
    (void)snprintf(line, sizeof(line), "[启动] 分区：Boot=0x%08lX APP=0x%08lX\r\n",
                   (unsigned long)OTA_BOOT_BASE, (unsigned long)OTA_APP_BASE);
    Log(line);
    result = OtaMeta_Read(&meta);
    if (result == OTA_META_IO_ERROR) {
        Stop("EEPROM的I2C读取失败");
        return;
    }
    /* 首次请求未关闭时才允许初始化；常规 CRC 故障不会无条件清空记录。 */
    if (BootSetupRequest[0] != 0U || BootSetupRequest[1] != 0U ||
        BootSetupRequest[2] != 0U || BootSetupRequest[3] != 0U) {
        if (result == OTA_META_INVALID || result == OTA_META_EMPTY) {
            if (!SetupRequested()) {
                Stop("首次初始化标记不完整且EEPROM无效");
                return;
            }
            msp = *(volatile const uint32_t *)OTA_APP_BASE;
            reset = *(volatile const uint32_t *)(OTA_APP_BASE + 4U);
            if (!Boot_VectorsValid(msp, reset)) {
                Stop("首次初始化失败：APP向量无效，EEPROM未改动");
                return;
            }
            Log("[启动] 首次初始化：仅建立EEPROM记录\r\n");
            memset(&meta, 0, sizeof(meta));
            /* Placeholder only: the running APP reports its own real version. */
            meta.image_size = 8U;
            (void)strcpy(meta.version, "first-setup");
            if (OtaMeta_WriteDraft(&meta) != OTA_META_OK) {
                Stop("首次初始化时EEPROM写入或回读失败");
                return;
            }
            result = OtaMeta_Read(&meta);
            if (result != OTA_META_OK || meta.pending != 0U) {
                Stop("首次初始化时EEPROM校验失败");
                return;
            }
            Log("[启动] 首次初始化通过：标志=0x00\r\n");
        }
        if (result != OTA_META_OK || FinishSetup() != HAL_OK) {
            Stop("首次初始化收尾失败，复位后重试");
            return;
        }
        Log("[启动] 首次初始化入口已关闭，正常OTA已启用\r\n");
    }
    if (result != OTA_META_OK) {
        Stop("初始化后EEPROM记录为空或格式、CRC无效");
        return;
    }
    if (result == OTA_META_OK) {
        (void)snprintf(line, sizeof(line),
                       "[启动] 升级记录：版本=%s 大小=%lu 标志=0x%02X\r\n",
                       meta.version, (unsigned long)meta.image_size, (unsigned int)meta.pending);
        Log(line);
        if (meta.pending == OTA_RECORD_PENDING) {
            if (OtaInstall_Run(&meta) != HAL_OK) {
                Stop("安装失败，保留暂存固件，复位后恢复");
                return;
            }
            return;
        }
        Log("[启动] 无已提交的安装任务\r\n");
    }
    msp = *(volatile const uint32_t *)OTA_APP_BASE;
    reset = *(volatile const uint32_t *)(OTA_APP_BASE + 4U);
    (void)snprintf(line, sizeof(line), "[启动] APP初始栈=0x%08lX 复位入口=0x%08lX\r\n",
                   (unsigned long)msp, (unsigned long)reset);
    Log(line);
    if (!Boot_VectorsValid(msp, reset) || __get_IPSR() != 0U) {
        Stop("APP向量无效或处于异常上下文");
        return;
    }
    Log("[启动] 跳转APP：0x08008000\r\n");

    /* Finish all HAL calls while SysTick and the Boot stack still work. */
    if (HAL_UART_DeInit(&huart1) != HAL_OK || HAL_I2C_DeInit(&hi2c1) != HAL_OK ||
        HAL_SPI_DeInit(&hspi2) != HAL_OK || HAL_RCC_DeInit() != HAL_OK) {
        Error_Handler();
    }
    __disable_irq();
    SysTick->CTRL = 0U;
    SysTick->LOAD = 0U;
    SysTick->VAL = 0U;
    for (i = 0U; i < 8U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    /* VTOR 指向 APP 的中断向量表；仅调用 APP 函数而不改 VTOR 会用错中断入口。 */
    SCB->VTOR = OTA_APP_BASE;
    __DSB();
    __ISB();
    /* The C entry sets APP MSP and never returns; no HAL calls follow. */
    Boot_Enter(msp, reset);
    for (;;) {
    }
}
