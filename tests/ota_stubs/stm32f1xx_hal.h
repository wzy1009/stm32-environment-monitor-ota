#ifndef HOST_HAL_H
#define HOST_HAL_H
#include <stdint.h>
typedef enum {HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT} HAL_StatusTypeDef;
typedef struct { unsigned unused; } UART_HandleTypeDef;
uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *uart, const uint8_t *data, uint16_t length, uint32_t timeout);
void NVIC_SystemReset(void);
#endif
