#include "board_io.h"
#include "main.h"

/* ZhanShen V3.0: DHT11=P13/PG11, buzzer=PB8, KEY0=PE4, KEY1=PE3. */
#define DHT_PIN GPIO_PIN_11
/* PG10 is available on expansion header P10 pin 22. */
#define PIR_PIN GPIO_PIN_10
#define BEEP_PIN GPIO_PIN_8

void BoardIo_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    __HAL_RCC_GPIOG_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, BEEP_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, FLASH_CS_Pin, GPIO_PIN_SET);
    gpio.Pin = FLASH_CS_Pin;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_5 | BEEP_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_5;
    HAL_GPIO_Init(GPIOE, &gpio);
    gpio.Pin = GPIO_PIN_3 | GPIO_PIN_4;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOE, &gpio);
    gpio.Pin = PIR_PIN;
    gpio.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOG, &gpio);
    HAL_GPIO_WritePin(GPIOG, DHT_PIN, GPIO_PIN_SET);
    gpio.Pin = DHT_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOG, &gpio);
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

uint8_t BoardIo_Key0Down(void) { return (GPIOE->IDR & GPIO_PIN_4) == 0U; }
uint8_t BoardIo_Key1Down(void) { return (GPIOE->IDR & GPIO_PIN_3) == 0U; }
uint8_t BoardIo_Motion(void) { return (GPIOG->IDR & PIR_PIN) != 0U; }
void BoardIo_DhtDriveLow(void) { GPIOG->BRR = DHT_PIN; }
void BoardIo_DhtRelease(void) { GPIOG->BSRR = DHT_PIN; }
uint8_t BoardIo_DhtLevel(void) { return (GPIOG->IDR & DHT_PIN) != 0U; }
uint32_t BoardIo_Cycles(void) { return DWT->CYCCNT; }
uint32_t BoardIo_CyclesPerUs(void) { return HAL_RCC_GetHCLKFreq() / 1000000U; }

void BoardIo_SetOutputs(uint32_t now, uint8_t alarm, uint8_t display_error)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_5,
                     ((now / 500U) & 1U) != 0U ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOE, GPIO_PIN_5,
                     (alarm != 0U || display_error != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, BEEP_PIN,
                     (alarm != 0U && ((now / 200U) & 1U) == 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
