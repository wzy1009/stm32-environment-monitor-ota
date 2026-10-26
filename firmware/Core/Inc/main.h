#ifndef MAIN_H
#define MAIN_H
#include "stm32f1xx_hal.h"
#define FLASH_CS_Pin GPIO_PIN_12
#define FLASH_CS_GPIO_Port GPIOB
void SystemClock_Config(void);
void Error_Handler(void);
#endif
