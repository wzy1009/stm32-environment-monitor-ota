#ifndef OLED_H
#define OLED_H

#include "stm32f1xx_hal.h"

/* P1: VCC=24, GND=22, SCL=20 (PD9), SDA=18 (PE15). */
HAL_StatusTypeDef Oled_Init(void);
/* One 8-pixel page, up to 21 characters; supports uppercase, digits and dot. */
HAL_StatusTypeDef Oled_ShowText(uint8_t page, const char *text);
/* Full 128x64 frame, page-major, vertical LSB first; size must be 1024. */
HAL_StatusTypeDef Oled_ShowBitmap(const uint8_t *bitmap, uint16_t size);
void Oled_DrawText(uint8_t *frame, uint8_t x, uint8_t page, const char *text);
HAL_StatusTypeDef Oled_BeginFrame(const uint8_t *frame);
HAL_StatusTypeDef Oled_FlushStep(void);
uint8_t Oled_IsFlushing(void);

#endif
