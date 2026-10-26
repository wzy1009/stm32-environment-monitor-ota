/**
 * @file oled.c
 * @brief 【OLED 显示】OLED 底层显示驱动。
 *        Boot 固件：在内部 Flash 0x08000000 启动，负责安装和转交控制权。
 * 通过模拟 I2C 发送命令和像素，提供文本、位图及分步刷屏接口；
 * OTA 只通过显示模块使用它，不经它修改固件。
 */
#include "oled.h"
#include <string.h>

#define OLED_SCL_PORT GPIOD
#define OLED_SCL_PIN  GPIO_PIN_9
#define OLED_SDA_PORT GPIOE
#define OLED_SDA_PIN  GPIO_PIN_15

static uint32_t low_ticks, high_ticks, period_ticks, hold_ticks, stretch_ticks;
static uint32_t low_since;
static uint8_t displayed[1024];
static uint8_t displayed_valid;
static uint8_t address;
static uint8_t ready;
static const uint8_t *pending_frame;
static uint16_t pending_offset;

/* Five columns per glyph: 0-9, A-Z, dot; bit 0 is the top pixel. */
static const uint8_t font[][5] = {
    {0x3E,0x51,0x49,0x45,0x3E}, {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46}, {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30}, {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36}, {0x06,0x49,0x49,0x29,0x1E},
    {0x7E,0x11,0x11,0x11,0x7E}, {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22}, {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41}, {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A}, {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00}, {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41}, {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F}, {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E}, {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E}, {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31}, {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F}, {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F}, {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07}, {0x61,0x51,0x49,0x45,0x43},
    {0x00,0x60,0x60,0x00,0x00}, {0x00,0x36,0x36,0x00,0x00},
    {0x08,0x08,0x08,0x08,0x08}, {0x20,0x10,0x08,0x04,0x02},
    {0x00,0x41,0x22,0x14,0x08}
};

/* Cycle timing stays valid across IRQs and counter wrap; IRQs remain enabled. */
__STATIC_FORCEINLINE void WaitFrom(uint32_t start, uint32_t cycles)
{
    while ((uint32_t)(DWT->CYCCNT - start) < cycles) {__NOP();}
}

__STATIC_FORCEINLINE void Delay(void)
{
    WaitFrom(DWT->CYCCNT, low_ticks);
}

__STATIC_FORCEINLINE void Sda(GPIO_PinState state)
{
    WaitFrom(low_since, hold_ticks);
    if (state == GPIO_PIN_SET) OLED_SDA_PORT->BSRR = OLED_SDA_PIN;
    else OLED_SDA_PORT->BRR = OLED_SDA_PIN;
}

__STATIC_FORCEINLINE void SclLow(void)
{
    OLED_SCL_PORT->BRR = OLED_SCL_PIN;
    low_since = DWT->CYCCNT;
}

static HAL_StatusTypeDef SclHigh(void)
{
    uint32_t released, risen;
    WaitFrom(low_since, low_ticks);
    OLED_SCL_PORT->BSRR = OLED_SCL_PIN;
    released = DWT->CYCCNT;
    while ((OLED_SCL_PORT->IDR & OLED_SCL_PIN) == 0U) {
        if ((uint32_t)(DWT->CYCCNT - released) >= stretch_ticks) return HAL_TIMEOUT;
    }
    risen = DWT->CYCCNT;
    WaitFrom(risen, high_ticks);
    /* >=2.5 us period, >=1.3 us low, >=0.6 us high (SSD1306 Fast-mode). */
    WaitFrom(low_since, period_ticks);
    return HAL_OK;
}

static HAL_StatusTypeDef Stop(void)
{
    HAL_StatusTypeDef result;
    SclLow();
    Sda(GPIO_PIN_RESET);
    result = SclHigh();
    Sda(GPIO_PIN_SET);
    Delay();
    return result;
}

static HAL_StatusTypeDef Start(void)
{
    Sda(GPIO_PIN_SET);
    if (SclHigh() != HAL_OK) return HAL_TIMEOUT;
    if ((OLED_SDA_PORT->IDR & OLED_SDA_PIN) == 0U) return HAL_ERROR;
    Sda(GPIO_PIN_RESET);
    Delay();
    SclLow();
    return HAL_OK;
}

static HAL_StatusTypeDef WriteByte(uint8_t value)
{
    uint8_t bit;
    uint32_t ack;
    for (bit = 0U; bit < 8U; ++bit) {
        Sda((value & 0x80U) != 0U ? GPIO_PIN_SET : GPIO_PIN_RESET);
        if (SclHigh() != HAL_OK) return HAL_TIMEOUT;
        SclLow();
        value <<= 1U;
    }
    Sda(GPIO_PIN_SET);
    if (SclHigh() != HAL_OK) return HAL_TIMEOUT;
    ack = OLED_SDA_PORT->IDR & OLED_SDA_PIN;
    SclLow();
    return ack == 0U ? HAL_OK : HAL_ERROR;
}

static HAL_StatusTypeDef Transfer(uint8_t control, const uint8_t *data, uint16_t size)
{
    HAL_StatusTypeDef result = Start();
    HAL_StatusTypeDef stopped;
    uint16_t i;
    if (result == HAL_OK) result = WriteByte((uint8_t)(address << 1U));
    if (result == HAL_OK) result = WriteByte(control);
    for (i = 0U; i < size && result == HAL_OK; ++i) result = WriteByte(data[i]);
    stopped = Stop();
    return result == HAL_OK ? stopped : result;
}

static HAL_StatusTypeDef SelectPage(uint8_t page)
{
    uint8_t command[3] = {(uint8_t)(0xB0U | page), 0x00U, 0x10U};
    return Transfer(0x00U, command, sizeof(command));
}

HAL_StatusTypeDef Oled_Init(void)
{
    GPIO_InitTypeDef gpio = {0};
    uint8_t i;
    static const uint8_t blank[128] = {0};
    static const uint8_t off = 0xAEU;
    static const uint8_t on = 0xAFU;
    static const uint8_t commands[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x02,
        /* Both scan directions reversed: 180 degrees for the rotated module. */
        0xA0, 0xC0,
        0xDA, 0x12, 0x81, 0x7F, 0xD9, 0xF1, 0xDB, 0x20, 0xA4, 0xA6
    };

    ready = 0U;
    pending_frame = NULL;
    displayed_valid = 0U;
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    low_ticks = (HAL_RCC_GetHCLKFreq() / 1000000U * 13U + 9U) / 10U;
    high_ticks = (HAL_RCC_GetHCLKFreq() / 1000000U * 6U + 9U) / 10U;
    period_ticks = (HAL_RCC_GetHCLKFreq() / 1000000U * 25U + 9U) / 10U;
    hold_ticks = (HAL_RCC_GetHCLKFreq() / 1000000U * 3U + 9U) / 10U;
    stretch_ticks = HAL_RCC_GetHCLKFreq() / 1000000U * 200U;
    low_since = DWT->CYCCNT;
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_GPIOE_CLK_ENABLE();
    HAL_GPIO_WritePin(OLED_SCL_PORT, OLED_SCL_PIN, GPIO_PIN_SET);
    HAL_GPIO_WritePin(OLED_SDA_PORT, OLED_SDA_PIN, GPIO_PIN_SET);
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Pin = OLED_SCL_PIN;
    HAL_GPIO_Init(OLED_SCL_PORT, &gpio);
    gpio.Pin = OLED_SDA_PIN;
    HAL_GPIO_Init(OLED_SDA_PORT, &gpio);
    /* The OLED module must provide SDA/SCL pull-ups to 3.3 V. */
    HAL_Delay(100U);

    /* Release a slave left mid-byte after an MCU reset. */
    Sda(GPIO_PIN_SET);
    for (i = 0U; i < 9U; ++i) {
        if (HAL_GPIO_ReadPin(OLED_SDA_PORT, OLED_SDA_PIN) == GPIO_PIN_SET) break;
        SclLow();
        Delay();
        if (SclHigh() != HAL_OK) return HAL_TIMEOUT;
    }
    if (Stop() != HAL_OK) return HAL_TIMEOUT;

    address = 0x3CU;
    if (Transfer(0x00U, &off, 1U) != HAL_OK) {
        address = 0x3DU;
        if (Transfer(0x00U, &off, 1U) != HAL_OK) return HAL_ERROR;
    }
    if (Transfer(0x00U, commands, sizeof(commands)) != HAL_OK) return HAL_ERROR;
    for (i = 0U; i < 8U; ++i) {
        if (SelectPage(i) != HAL_OK || Transfer(0x40U, blank, sizeof(blank)) != HAL_OK) {
            return HAL_ERROR;
        }
    }
    if (Transfer(0x00U, &on, 1U) != HAL_OK) return HAL_ERROR;
    memset(displayed, 0, sizeof(displayed));
    displayed_valid = 1U;
    ready = 1U;
    return HAL_OK;
}

HAL_StatusTypeDef Oled_ShowBitmap(const uint8_t *bitmap, uint16_t size)
{
    uint8_t page;
    HAL_StatusTypeDef result;
    if (ready == 0U || bitmap == NULL || size != 1024U) return HAL_ERROR;
    displayed_valid = 0U;
    for (page = 0U; page < 8U; ++page) {
        result = SelectPage(page);
        if (result != HAL_OK) return result;
        result = Transfer(0x40U, bitmap + (uint16_t)page * 128U, 128U);
        if (result != HAL_OK) return result;
    }
    return HAL_OK;
}

HAL_StatusTypeDef Oled_ShowText(uint8_t page, const char *text)
{
    uint8_t row[128] = {0};
    uint8_t column = 0U;
    uint8_t ch;
    uint8_t index;
    if (ready == 0U || page >= 8U || text == NULL) return HAL_ERROR;
    displayed_valid = 0U;
    while (*text != '\0' && column <= 122U) {
        ch = (uint8_t)*text++;
        if (ch >= '0' && ch <= '9') index = ch - '0';
        else if (ch >= 'A' && ch <= 'Z') index = ch - 'A' + 10U;
        else if (ch == '.') index = 36U;
        else index = 0xFFU;
        if (index != 0xFFU) memcpy(&row[column], font[index], 5U);
        column += 6U;
    }
    if (SelectPage(page) != HAL_OK) return HAL_ERROR;
    return Transfer(0x40U, row, sizeof(row));
}

void Oled_DrawText(uint8_t *frame, uint8_t x, uint8_t page, const char *text)
{
    static const char characters[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ.:-/>";
    const char *found;
    uint8_t ch;
    uint16_t offset;
    if (frame == NULL || text == NULL || page >= 8U) return;
    while (*text != '\0' && x <= 122U) {
        ch = (uint8_t)*text++;
        if (ch >= 'a' && ch <= 'z') ch = ch - 'a' + 'A';
        found = strchr(characters, ch);
        offset = (uint16_t)page * 128U + x;
        memset(&frame[offset], 0, 6U);
        if (found != NULL && ch != 0U) memcpy(&frame[offset], font[found - characters], 5U);
        x += 6U;
    }
}

HAL_StatusTypeDef Oled_BeginFrame(const uint8_t *frame)
{
    if (ready == 0U || frame == NULL) return HAL_ERROR;
    if (pending_frame != NULL) return HAL_BUSY;
    /* Caller keeps this frame unchanged until the last flush step completes. */
    pending_frame = frame;
    pending_offset = 0U;
    return HAL_OK;
}

uint8_t Oled_IsFlushing(void)
{
    return pending_frame != NULL ? 1U : 0U;
}

HAL_StatusTypeDef Oled_FlushStep(void)
{
    HAL_StatusTypeDef result;
    uint16_t first, end, base;
    uint8_t command[3];
    if (pending_frame == NULL) return HAL_OK;
    while (pending_offset < 1024U) {
        base = pending_offset;
        first = 0U;
        end = 128U;
        if (displayed_valid != 0U) {
            while (first < end && pending_frame[base + first] == displayed[base + first]) ++first;
            while (end > first && pending_frame[base + end - 1U] == displayed[base + end - 1U]) --end;
        }
        pending_offset += 128U;
        if (first == end) continue;
        command[0] = (uint8_t)(0xB0U | (base / 128U));
        command[1] = (uint8_t)(first & 0x0FU);
        command[2] = (uint8_t)(0x10U | (first >> 4U));
        result = Transfer(0x00U, command, sizeof(command));
        if (result == HAL_OK) result = Transfer(0x40U, pending_frame + base + first, end - first);
        if (result != HAL_OK) {
            displayed_valid = 0U;
            pending_frame = NULL;
            return result;
        }
        memcpy(displayed + base + first, pending_frame + base + first, end - first);
        break; /* At most one 128-byte page, then service UART/network again. */
    }
    if (pending_offset == 1024U) {
        pending_frame = NULL;
        displayed_valid = 1U;
    }
    return HAL_OK;
}
