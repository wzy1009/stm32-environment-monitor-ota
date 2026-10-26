#include "ota_image.h"
#ifdef ENV_BOOT
#include "boot_flash.h"
#endif
#ifdef ENV_OTA_APP
#include "ota_app.h"
#endif
#include "at24c02.h"
#include "w25q128.h"
#include "utils_md5.h"
#include <string.h>

HAL_StatusTypeDef OtaImage_CheckDevice(void)
{
    uint32_t id;
    uint32_t first = 0U;
    uint32_t i;
    for (i = 0U; i < 3U; ++i) {
        if (W25Q128_ReadJedecId(&id) != HAL_OK ||
            (id != 0x522118UL && id != 0xEF4018UL) || (i != 0U && id != first)) {
            return HAL_ERROR;
        }
        first = id;
    }
    return HAL_OK;
}

static HAL_StatusTypeDef Verify(const OtaMeta *meta, int internal)
{
    OtaMeta draft;
    uint8_t raw[OTA_RECORD_SIZE];
    uint8_t data[256];
    uint8_t digest[16];
    iot_md5_context ctx;
    uint32_t offset = 0U;
    uint32_t count;
    uint32_t msp;
    uint32_t reset;
    uint32_t entry;
    if (meta == NULL || (meta->pending != 0U && meta->pending != OTA_RECORD_PENDING)) {
        return HAL_ERROR;
    }
    draft = *meta;
    draft.pending = 0U;
    if (OtaMeta_Encode(&draft, raw) != OTA_META_OK) {
        return HAL_ERROR;
    }
    if (internal) {
#ifdef ENV_BOOT
        if (BootFlash_Read(OTA_APP_BASE, data, 8U) != HAL_OK) return HAL_ERROR;
#else
        memcpy(data, (const void *)OTA_APP_BASE, 8U);
#endif
    } else if (OtaImage_CheckDevice() != HAL_OK ||
               W25Q128_Read(OTA_STAGE_BASE, data, 8U) != HAL_OK) {
        return HAL_ERROR;
    }
    memcpy(&msp, data, 4U);
    memcpy(&reset, data + 4U, 4U);
    /* BIN 前 8 字节是初始 MSP 与复位入口，不是可直接执行的普通数据。 */
    entry = reset & ~1UL;
    if (msp <= OTA_SRAM_BASE || msp > OTA_SRAM_BASE + OTA_SRAM_SIZE ||
        (msp & 7U) != 0U || (reset & 1U) == 0U || entry < OTA_APP_BASE + 8U ||
        entry >= OTA_APP_BASE + meta->image_size) {
        return HAL_ERROR;
    }
    utils_md5_init(&ctx);
    utils_md5_starts(&ctx);
    while (offset < meta->image_size) {
        count = meta->image_size - offset;
        if (count > sizeof(data)) count = sizeof(data);
        if (internal) {
#ifdef ENV_BOOT
            if (BootFlash_Read(OTA_APP_BASE + offset, data, count) != HAL_OK) {
                utils_md5_free(&ctx);
                return HAL_ERROR;
            }
#else
            memcpy(data, (const void *)(OTA_APP_BASE + offset), count);
#endif
        } else if (W25Q128_Read(OTA_STAGE_BASE + offset, data, count) != HAL_OK) {
            utils_md5_free(&ctx);
            return HAL_ERROR;
        }
        utils_md5_update(&ctx, data, count);
        offset += count;
#ifdef ENV_OTA_APP
        OtaApp_Checkpoint();
#endif
    }
    utils_md5_finish(&ctx, digest);
    utils_md5_free(&ctx);
    return memcmp(digest, meta->md5, sizeof(digest)) == 0 ? HAL_OK : HAL_ERROR;
}

HAL_StatusTypeDef OtaImage_VerifyStage(const OtaMeta *meta)
{
    return Verify(meta, 0);
}

HAL_StatusTypeDef OtaImage_VerifyInternal(const OtaMeta *meta)
{
    return Verify(meta, 1);
}

HAL_StatusTypeDef OtaImage_Commit(const OtaMeta *meta)
{
    uint8_t flag = OTA_RECORD_PENDING;
    OtaMeta actual;
    if (meta == NULL || meta->pending != 0U ||
        OtaImage_VerifyStage(meta) != HAL_OK || OtaMeta_WriteDraft(meta) != OTA_META_OK) {
        return HAL_ERROR;
    }
    /* 这是提交点。只有外部 BIN 和完整记录都确认后，才单独写 A5。 */
    if (At24c02_Write(OTA_RECORD_ADDRESS + OTA_RECORD_FLAG_OFFSET, &flag, 1U) != HAL_OK ||
        OtaMeta_Read(&actual) != OTA_META_OK || actual.pending != OTA_RECORD_PENDING ||
        actual.image_size != meta->image_size || strcmp(actual.version, meta->version) != 0 ||
        memcmp(actual.md5, meta->md5, sizeof(actual.md5)) != 0) {
        return HAL_ERROR;
    }
    return HAL_OK;
}
