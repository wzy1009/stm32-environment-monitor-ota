#include "main.h"
#include "boot_flash.h"
#include "board_io.h"
#include "oled.h"
#include "usart.h"
#include "i2c.h"
#include "spi.h"
#include <stdio.h>
#include <string.h>

#define BOOT_SETUP_ADDRESS 0x08007800UL
static const uint16_t initial_setup_request[4]
    __attribute__((section("BOOT_SETUP"), used)) = {0xA55AU, 0x5AA5U, 0xC33CU, 0x3CC3U};
#define setup_request ((volatile const uint16_t *)initial_setup_request)
static uint8_t display_ok, frame[1024];
extern void Boot_Enter(uint32_t msp, uint32_t reset);

void Boot_Show(const char *stage, uint32_t done, uint32_t total)
{
    char line[32];
    if (display_ok == 0U) return;
    memset(frame, 0, sizeof(frame));
    Oled_DrawText(frame, 0U, 0U, "ENV BOOT");
    Oled_DrawText(frame, 0U, 2U, stage);
    (void)snprintf(line, sizeof(line), "%lu%%", (unsigned long)(total != 0U ? done * 100U / total : 0U));
    Oled_DrawText(frame, 0U, 4U, line);
    if (Oled_BeginFrame(frame) != HAL_OK) { display_ok = 0U; return; }
    while (Oled_IsFlushing() != 0U)
        if (Oled_FlushStep() != HAL_OK) { display_ok = 0U; break; }
}

static uint8_t VectorsValid(uint32_t msp, uint32_t reset)
{
    uint32_t entry = reset & ~1UL;
    return msp > OTA_SRAM_BASE && msp <= OTA_SRAM_BASE + OTA_SRAM_SIZE &&
           (msp & 7U) == 0U && (reset & 1U) != 0U &&
           entry >= OTA_APP_BASE + 8U && entry < OTA_APP_BASE + OTA_APP_MAX_SIZE;
}

static uint8_t Setup(OtaMetaResult result, OtaMeta *meta)
{
    uint32_t i;
    uint16_t any = setup_request[0] | setup_request[1] | setup_request[2] | setup_request[3];
    if (any == 0U) return result == OTA_META_OK;
    if (result == OTA_META_EMPTY || result == OTA_META_INVALID) {
        if (setup_request[0] != 0xA55AU || setup_request[1] != 0x5AA5U ||
            setup_request[2] != 0xC33CU || setup_request[3] != 0x3CC3U ||
            !VectorsValid(*(volatile const uint32_t *)OTA_APP_BASE,
                          *(volatile const uint32_t *)(OTA_APP_BASE + 4U))) return 0U;
        memset(meta, 0, sizeof(*meta));
        meta->image_size = 8U;
        strcpy(meta->version, "first-setup");
        if (OtaMeta_WriteDraft(meta) != OTA_META_OK) return 0U;
        result = OtaMeta_Read(meta);
    }
    if (result != OTA_META_OK || meta->pending != 0U || HAL_FLASH_Unlock() != HAL_OK) return 0U;
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);
    /* Close the factory setup gate by clearing bits; never erase this page in OTA. */
    for (i = 0U; i < 4U; ++i) {
        if (setup_request[i] != 0U &&
            HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, BOOT_SETUP_ADDRESS + i * 2U, 0U) != HAL_OK) break;
    }
    if (HAL_FLASH_Lock() != HAL_OK || i != 4U) return 0U;
    return (setup_request[0] | setup_request[1] | setup_request[2] | setup_request[3]) == 0U;
}

int main(void)
{
    OtaMeta meta;
    uint32_t i, msp, reset;
    HAL_Init();
    SystemClock_Config();
    BoardIo_Init();
    MX_USART1_UART_Init();
    MX_I2C1_Init();
    MX_SPI2_Init();
    display_ok = Oled_Init() == HAL_OK;
    Boot_Show("START", 0U, 1U);
    if (!Setup(OtaMeta_Read(&meta), &meta)) goto failed;
    if (meta.pending == OTA_RECORD_PENDING) {
        if (BootInstall_Run(&meta) != HAL_OK) goto failed;
        NVIC_SystemReset();
        goto failed;
    }
    msp = *(volatile const uint32_t *)OTA_APP_BASE;
    reset = *(volatile const uint32_t *)(OTA_APP_BASE + 4U);
    if (!VectorsValid(msp, reset) || __get_IPSR() != 0U) goto failed;
    if (HAL_UART_DeInit(&huart1) != HAL_OK || HAL_I2C_DeInit(&hi2c1) != HAL_OK ||
        HAL_SPI_DeInit(&hspi2) != HAL_OK || HAL_RCC_DeInit() != HAL_OK) goto failed;
    __disable_irq();
    SysTick->CTRL = SysTick->LOAD = SysTick->VAL = 0U;
    for (i = 0U; i < 8U; ++i) {
        NVIC->ICER[i] = 0xFFFFFFFFUL;
        NVIC->ICPR[i] = 0xFFFFFFFFUL;
    }
    SCB->ICSR = SCB_ICSR_PENDSTCLR_Msk | SCB_ICSR_PENDSVCLR_Msk;
    SCB->VTOR = OTA_APP_BASE;
    __DSB();
    __ISB();
    Boot_Enter(msp, reset);
failed:
    Boot_Show("FAILED", 0U, 1U);
    for (;;) HAL_Delay(100U);
}
