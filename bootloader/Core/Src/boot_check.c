/**
 * @file boot_check.c
 * @brief 【启动检查】APP 启动向量的范围检查。
 *        Boot 固件：在内部 Flash 0x08000000 启动，负责安装和转交控制权。
 * 验证初始 MSP 位于 SRAM 且正确对齐、复位入口为 Thumb 地址且位于 APP 分区；
 * 它不是整个固件的 MD5 校验。
 */
#include "boot.h"
#include "ota_layout.h"

/**
 * @brief 【启动检查】检查 APP 初始主栈指针和 Thumb 复位入口。
 *        1. 清除复位地址最低位，得到用于范围比较的入口地址。
 *        2. 检查初始 MSP 是否位于 SRAM，并满足 8 字节对齐。
 *        3. 检查复位地址的 Thumb 标志位及 APP 分区范围。
 *
 * @param msp APP 向量表第一项，初始主栈指针地址。
 * @param reset APP 向量表第二项，带 Thumb 标志位的复位入口地址。
 *
 * @return 非零表示APP 栈地址和复位入口合法，0 表示不满足该条件。
 */
int Boot_VectorsValid(uint32_t msp, uint32_t reset)
{
    uint32_t entry = reset & ~1UL;
    return msp > OTA_SRAM_BASE && msp <= OTA_SRAM_BASE + OTA_SRAM_SIZE &&
           (msp & 7U) == 0U && (reset & 1U) != 0U &&
           entry >= OTA_APP_BASE + 8U && entry < OTA_APP_BASE + OTA_APP_MAX_SIZE;
}
