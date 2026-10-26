/**
 * @file boot_enter.c
 * @brief 【APP 跳转】Boot 到 APP 的 C 语言交接入口。
 *        Boot 固件：在内部 Flash 0x08000000 启动，负责安装和转交控制权。
 * 使用 CMSIS 接口设置处理器状态和 APP 主栈，再调用 APP 的 Reset_Handler 地址；
 * 此函数不可正常返回 Boot。
 */
#include "main.h"

/* Keep CMSIS register setters inline, including in a debug build. */
#if defined(__CC_ARM)
#pragma push
#pragma O3
#endif

/**
 * @brief 【APP 跳转】设置 APP 初始栈并转交到 APP 复位入口。
 *        1. 把 APP 复位地址转换为无参数的入口函数指针。
 *        2. 清理 CONTROL、BASEPRI 和 FAULTMASK，建立 APP 启动所需的处理器状态。
 *        3. 使用 __set_MSP() 装入 APP 初始栈地址。
 *        4. 执行数据及指令屏障，恢复中断后调用 APP 复位入口。
 *        5. 若 APP 意外返回，停留在本函数末尾，不返回使用旧栈的 Boot 调用者。
 *        调用前须关闭 Boot 中断并设置 APP 的 VTOR；换栈后不得再访问旧栈。
 *
 * @param msp APP 向量表第一项，初始主栈指针地址。
 * @param reset APP 向量表第二项，带 Thumb 标志位的复位入口地址。
 *
 * @return 无返回值。
 */
__attribute__((noinline, noreturn))
void Boot_Enter(uint32_t msp, uint32_t reset)
{
    typedef void (*AppEntry)(void);
    AppEntry entry = (AppEntry)reset;

    __set_CONTROL(0U);
    __set_BASEPRI(0U);
    __set_FAULTMASK(0U);
    /* After changing MSP, do not access locals or return to the Boot caller. */
    __set_MSP(msp);
    __DSB();
    __ISB();
    __enable_irq();
    entry();
    for (;;) {
        /* An APP reset handler is not expected to return. */
    }
}

#if defined(__CC_ARM)
#pragma pop
#endif
