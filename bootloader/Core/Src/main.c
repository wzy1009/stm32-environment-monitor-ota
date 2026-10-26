/**
 * @file main.c
 * @brief 【主程序】工程入口与主循环。
 *        Boot 固件：在内部 Flash 0x08000000 启动，负责安装和转交控制权。
 * 初始化时钟、外设和应用模块，反复调用任务函数；
 * 详细入口说明见本文件 main() 前的函数注释。
 */
#include "main.h"
#include "gpio.h"
#include "usart.h"
#include "i2c.h"
#include "spi.h"
/* USER CODE BEGIN Includes */
#include "boot.h"
#include "ota_layout.h"
#include "boot_display.h"
/* USER CODE END Includes */

void SystemClock_Config(void);

/**
 * @brief 【主程序】Boot 上电入口：初始化 HAL、时钟及串口/I2C/SPI、显示状态，再调用 Boot_Run。
 *        1. 初始化 HAL、系统时钟和 Boot 所需的 GPIO、串口、I2C、SPI。
 *        2. 初始化 Boot 显示并调用 Boot_Run() 执行启动决策。
 *        3. 若安装或跳转被拒绝，停留在后续循环，保留错误现场。
 *
 * @return 正常运行时保持主循环，不返回调用方。
 */
int main(void)
{
    /* USER CODE BEGIN 1 */
    SCB->VTOR = OTA_BOOT_BASE;
    __DSB();
    __ISB();
    /* USER CODE END 1 */
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_USART1_UART_Init();
    MX_SPI2_Init();
    MX_I2C1_Init();
    BootDisplay_Init();
    BootDisplay_Show("BOOT", "READY", 0U, 1U);
    /* USER CODE BEGIN 2 */
    HAL_Delay(5000U);
    Boot_Run();
    /* USER CODE END 2 */
    /* USER CODE BEGIN WHILE */
    while (1) {
        /* Reached only when Boot refused to jump. */
        HAL_GPIO_TogglePin(GPIOE, GPIO_PIN_5);
        HAL_Delay(500U);
    }
    /* USER CODE END WHILE */
}

/**
 * @brief 【主程序】配置本固件使用的系统与总线时钟。
 *        Boot 与 APP 各自初始化，不能假设跳转后可以直接沿用 Boot 的外设运行状态。
 *
 * @return 无返回值。
 */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clocks = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState = RCC_HSE_ON;
    osc.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
    osc.HSIState = RCC_HSI_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    osc.PLL.PLLMUL = RCC_PLL_MUL9;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();
    }
    clocks.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                       RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clocks.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clocks.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clocks.APB1CLKDivider = RCC_HCLK_DIV2;
    clocks.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clocks, FLASH_LATENCY_2) != HAL_OK) {
        Error_Handler();
    }
}

/**
 * @brief 【主程序】底层初始化或不可继续的 HAL 错误处理入口。
 *        进入后停止正常调度，需结合前面的日志定位原因。
 *
 * @return 无返回值。
 */
void Error_Handler(void)
{
    __disable_irq();
    for (;;) {
    }
}
