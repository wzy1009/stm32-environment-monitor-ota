/**
 * @file stm32f1xx_it.c
 * @brief 【中断处理】Boot 的异常入口和 SysTick 节拍更新。
 *        Boot 固件：在内部 Flash 0x08000000 启动，负责安装和转交控制权。
 * 安装结束或跳转前清理中断环境，APP 使用自己的向量表与处理函数。
 */
#include "main.h"
#include "stm32f1xx_it.h"
void NMI_Handler(void) { }
void HardFault_Handler(void) { while (1) { } }
void MemManage_Handler(void) { while (1) { } }
void BusFault_Handler(void) { while (1) { } }
void UsageFault_Handler(void) { while (1) { } }
void SVC_Handler(void) { }
void DebugMon_Handler(void) { }
void PendSV_Handler(void) { }
/**
 * @brief 【中断处理】调用 HAL 的节拍更新，支撑联网、超时和任务调度的毫秒计时。
 *        Boot 跳转时会停用自己的 SysTick。
 *
 * @return 无返回值。
 */
void SysTick_Handler(void) { HAL_IncTick(); }
