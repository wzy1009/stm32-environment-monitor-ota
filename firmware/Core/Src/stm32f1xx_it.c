#include "main.h"
#include "usart.h"
void SysTick_Handler(void)
{
    HAL_IncTick();
}

void USART3_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart3);
}
