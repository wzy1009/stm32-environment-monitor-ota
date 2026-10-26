/**
 * @file boot_display.c
 * @brief 【安装进度】Boot 阶段的 OLED 状态与进度显示。
 *        Boot 固件：在内部 Flash 0x08000000 启动，负责安装和转交控制权。
 * 用于呈现预检查、擦除、写入和结果；
 * 与 APP 菜单分属两个独立固件，屏幕故障不能代替镜像校验结论。
 */
#include "boot_display.h"
#include "oled.h"
#include <stdio.h>
#include <string.h>

static uint8_t available;
static uint8_t shown_percent;
static char shown_stage[9];
static uint8_t frame[1024];

/**
 * @brief 【安装进度】在 Boot 内初始化屏幕状态，供安装阶段显示使用。
 *        APP 后续会重新初始化自己的显示模块。
 *
 * @return 无返回值。
 */
void BootDisplay_Init(void)
{
    available = Oled_Init() == HAL_OK ? 1U : 0U;
    shown_stage[0] = '\0';
    shown_percent = 0U;
}

/**
 * @brief 【安装进度】显示 stage 阶段、version 版本及 percent 进度。
 *        force 非零时立即更新，否则按限频策略刷新，避免显示操作过度拖慢安装。
 *
 * @param stage 显示的阶段名称。
 * @param version 固件版本字符串。
 * @param percent 待显示的进度百分比。
 * @param force 非零立即刷新，否则按限频条件刷新。
 *
 * @return 无返回值。
 */
void BootDisplay_Show(const char *stage, const char *version, uint8_t percent, uint8_t force)
{
    char line[24];
    uint8_t i;
    uint8_t fill;
    if (available == 0U || stage == NULL || version == NULL) return;
    if (percent > 100U) percent = 100U;
    if (force == 0U && strcmp(stage, shown_stage) == 0 &&
        percent < shown_percent + 10U && percent >= shown_percent) return;

    memset(frame, 0, sizeof(frame));
    if (strcmp(stage, "BOOT") == 0) {
        Oled_DrawText(frame, 0U, 0U, "BOOT READY");
        Oled_DrawText(frame, 0U, 3U, "CHECK UPDATE RECORD");
    } else {
        Oled_DrawText(frame, 0U, 0U, stage);
        (void)snprintf(line, sizeof(line), "VER %s", version);
        Oled_DrawText(frame, 0U, 2U, line);
        (void)snprintf(line, sizeof(line), "PROGRESS %u", (unsigned int)percent);
        Oled_DrawText(frame, 0U, 7U, line);
        for (i = 8U; i < 120U; ++i) {
            frame[5U * 128U + i] = 0x81U;
            frame[6U * 128U + i] = 0x81U;
        }
        frame[5U * 128U + 8U] = frame[6U * 128U + 8U] = 0xFFU;
        frame[5U * 128U + 119U] = frame[6U * 128U + 119U] = 0xFFU;
        fill = (uint8_t)((110U * percent) / 100U);
        for (i = 0U; i < fill; ++i) {
            frame[5U * 128U + 9U + i] = 0xFFU;
            frame[6U * 128U + 9U + i] = 0xFFU;
        }
    }
    if (Oled_BeginFrame(frame) != HAL_OK) return;
    while (Oled_IsFlushing() != 0U) {
        if (Oled_FlushStep() != HAL_OK) {
            available = 0U;
            return;
        }
    }
    (void)strncpy(shown_stage, stage, sizeof(shown_stage) - 1U);
    shown_stage[sizeof(shown_stage) - 1U] = '\0';
    shown_percent = percent;
}
