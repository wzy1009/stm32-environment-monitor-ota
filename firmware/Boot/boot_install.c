#include "boot_flash.h"
#include "ota_image.h"
#include "w25q128.h"
#include <string.h>

HAL_StatusTypeDef BootInstall_Run(const OtaMeta *meta)
{
    OtaMeta saved;
    uint8_t data[256];
    uint32_t offset, count;
    if (meta == NULL || meta->pending != OTA_RECORD_PENDING ||
        OtaMeta_Read(&saved) != OTA_META_OK || saved.pending != OTA_RECORD_PENDING ||
        saved.image_size != meta->image_size || strcmp(saved.version, meta->version) != 0 ||
        memcmp(saved.md5, meta->md5, sizeof(saved.md5)) != 0 || OtaImage_VerifyStage(meta) != HAL_OK)
        return HAL_ERROR;
    /* Keep pending and the staged image intact until internal readback passes. */
    for (offset = 0U; offset < meta->image_size; offset += OTA_FLASH_PAGE_SIZE) {
        Boot_Show("ERASE", offset, meta->image_size);
        if (BootFlash_ErasePage(OTA_APP_BASE + offset) != HAL_OK) return HAL_ERROR;
    }
    for (offset = 0U; offset < meta->image_size; offset += count) {
        count = meta->image_size - offset;
        if (count > sizeof(data)) count = sizeof(data);
        if (W25Q128_Read(OTA_STAGE_BASE + offset, data, count) != HAL_OK ||
            BootFlash_Program(OTA_APP_BASE + offset, data, count) != HAL_OK) return HAL_ERROR;
        if (offset % OTA_FLASH_PAGE_SIZE == 0U) Boot_Show("WRITE", offset, meta->image_size);
    }
    Boot_Show("VERIFY", meta->image_size, meta->image_size);
    if (OtaImage_VerifyInternal(meta) != HAL_OK || OtaMeta_ClearPending() != OTA_META_OK ||
        OtaMeta_Read(&saved) != OTA_META_OK || saved.pending != 0U) return HAL_ERROR;
    Boot_Show("DONE", meta->image_size, meta->image_size);
    return HAL_OK;
}
