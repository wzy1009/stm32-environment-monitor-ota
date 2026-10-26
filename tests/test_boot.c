#include "boot_flash.h"
#include "ota_image.h"
#include "utils_md5.h"
#include "w25q128.h"
#include "at24c02.h"
#include <windows.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t stage[OTA_APP_MAX_SIZE], eeprom[256];
static uint8_t *internal;
static uint32_t image_size = 4097U, programs, erases;
static uint8_t erase_fault, program_fault, clear_fault, corrupt_readback, corrupt_final;
static OtaMeta meta;

HAL_StatusTypeDef HAL_FLASH_Unlock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASH_Lock(void) { return HAL_OK; }
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase, uint32_t *error)
{
    assert(erase->PageAddress >= OTA_APP_BASE && erase->NbPages == 1U);
    assert(erase->PageAddress + OTA_FLASH_PAGE_SIZE <= OTA_APP_BASE + OTA_APP_MAX_SIZE);
    ++erases; *error = 0xFFFFFFFFU;
    if (erase_fault && erases == 2U) return HAL_ERROR;
    memset((void *)erase->PageAddress, 0xFF, OTA_FLASH_PAGE_SIZE);
    return HAL_OK;
}
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t kind, uint32_t address, uint64_t value)
{
    assert(kind == FLASH_TYPEPROGRAM_HALFWORD && address >= OTA_APP_BASE);
    assert(address + 2U <= OTA_APP_BASE + OTA_APP_MAX_SIZE && !(address & 1U));
    ++programs;
    if (program_fault && programs == 129U) return HAL_ERROR;
    *(uint16_t *)address = (uint16_t)value;
    if (corrupt_readback && programs == 129U) *(uint16_t *)address ^= 1U;
    if (corrupt_final && address == OTA_APP_BASE + image_size - 1U) internal[OTA_BOOT_SIZE + 100U] ^= 1U;
    return HAL_OK;
}
HAL_StatusTypeDef At24c02_Read(uint32_t a, uint8_t *p, uint32_t n)
{ assert(a + n <= sizeof(eeprom)); memcpy(p, eeprom + a, n); return HAL_OK; }
HAL_StatusTypeDef At24c02_Write(uint32_t a, const uint8_t *p, uint32_t n)
{
    assert(a + n <= sizeof(eeprom));
    if (clear_fault && a == OTA_RECORD_FLAG_OFFSET && n == 1U && *p == 0U) return HAL_ERROR;
    memcpy(eeprom + a, p, n); return HAL_OK;
}
HAL_StatusTypeDef W25Q128_ReadJedecId(uint32_t *id) { *id = 0xEF4018U; return HAL_OK; }
HAL_StatusTypeDef W25Q128_Read(uint32_t a, uint8_t *p, uint32_t n)
{ assert(a + n <= sizeof(stage)); memcpy(p, stage + a, n); return HAL_OK; }
void Boot_Show(const char *stage_name, uint32_t done, uint32_t total)
{ assert(stage_name != NULL && done <= total); }

static void Prepare(void)
{
    uint32_t i, msp = OTA_SRAM_BASE + OTA_SRAM_SIZE, entry = OTA_APP_BASE + 9U;
    memset(internal, 0xA5, OTA_BOOT_SIZE + OTA_APP_MAX_SIZE);
    for (i = 0U; i < image_size; ++i) stage[i] = (uint8_t)(i * 17U);
    memcpy(stage, &msp, 4U); memcpy(stage + 4U, &entry, 4U);
    memset(&meta, 0, sizeof(meta));
    meta.image_size = image_size; strcpy(meta.version, "0.4.1");
    utils_md5(stage, image_size, meta.md5);
    memset(eeprom, 0xFF, sizeof(eeprom));
    programs = erases = erase_fault = program_fault = clear_fault = corrupt_readback = corrupt_final = 0U;
    assert(OtaImage_Commit(&meta) == HAL_OK);
    assert(OtaMeta_Read(&meta) == OTA_META_OK && meta.pending == OTA_RECORD_PENDING);
}
static void BootIntact(void)
{
    uint32_t i;
    for (i = 0U; i < OTA_BOOT_SIZE; ++i) assert(internal[i] == 0xA5U);
}
static void Installed(void)
{
    OtaMeta saved;
    assert(BootInstall_Run(&meta) == HAL_OK);
    assert(memcmp(internal + OTA_BOOT_SIZE, stage, image_size) == 0);
    assert(internal[OTA_BOOT_SIZE + image_size] == 0xFFU);
    assert(OtaMeta_Read(&saved) == OTA_META_OK && saved.pending == 0U);
    BootIntact();
}
static void InstallationAndBounds(void)
{
    uint8_t data[4] = {0};
    uint32_t calls;
    Prepare(); Installed(); calls = programs + erases;
    assert(BootFlash_Read(OTA_BOOT_BASE, data, 4U) == HAL_ERROR);
    assert(BootFlash_Read(OTA_APP_BASE + OTA_APP_MAX_SIZE - 1U, data, 2U) == HAL_ERROR);
    assert(BootFlash_ErasePage(OTA_APP_BASE - OTA_FLASH_PAGE_SIZE) == HAL_ERROR);
    assert(BootFlash_ErasePage(OTA_APP_BASE + 1U) == HAL_ERROR);
    assert(BootFlash_Program(OTA_APP_BASE - 2U, data, 2U) == HAL_ERROR);
    assert(BootFlash_Program(OTA_APP_BASE + OTA_APP_MAX_SIZE - 2U, data, 3U) == HAL_ERROR);
    assert(BootFlash_Program(OTA_APP_BASE + 1U, data, 2U) == HAL_ERROR);
    assert(programs + erases == calls); BootIntact();
    puts("PASS boot: installation, odd tail, readback, metadata clear and protected boundaries");
}
static void FailuresAndRetry(void)
{
    unsigned mode;
    for (mode = 0U; mode < 8U; ++mode) {
        Prepare();
        if (mode == 0U) stage[500] ^= 1U;
        if (mode == 1U) eeprom[10] ^= 1U;
        if (mode == 2U) erase_fault = 1U;
        if (mode == 3U) program_fault = 1U;
        if (mode == 4U) corrupt_readback = 1U;
        if (mode == 5U) corrupt_final = 1U;
        if (mode == 6U) clear_fault = 1U;
        if (mode == 7U) { uint32_t entry = OTA_BOOT_BASE + 9U;
            memcpy(stage + 4U, &entry, 4U); utils_md5(stage, image_size, meta.md5);
            { uint8_t raw[OTA_RECORD_SIZE]; OtaMeta draft = meta; draft.pending = 0U;
              assert(OtaMeta_Encode(&draft, raw) == OTA_META_OK); memcpy(eeprom, raw, sizeof(raw));
              eeprom[OTA_RECORD_FLAG_OFFSET] = OTA_RECORD_PENDING; } }
        assert(BootInstall_Run(&meta) == HAL_ERROR);
        assert(eeprom[OTA_RECORD_FLAG_OFFSET] == OTA_RECORD_PENDING); BootIntact();
        if (mode == 0U || mode == 1U || mode == 7U) assert(erases == 0U);
        else {
            /* Reset loses RAM counters but keeps staged data, Flash and EEPROM. */
            programs = erases = 0U;
            erase_fault = program_fault = corrupt_readback = corrupt_final = clear_fault = 0U;
            assert(OtaMeta_Read(&meta) == OTA_META_OK);
            Installed();
        }
    }
    puts("PASS boot: invalid stage/record/vector, erase/write/readback/commit faults and restart retry");
}
int main(void)
{
    internal = VirtualAlloc((void *)OTA_BOOT_BASE, OTA_BOOT_SIZE + OTA_APP_MAX_SIZE,
                            MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    assert(internal == (uint8_t *)OTA_BOOT_BASE);
    InstallationAndBounds(); FailuresAndRetry();
    assert(VirtualFree(internal, 0U, MEM_RELEASE));
    return 0;
}
