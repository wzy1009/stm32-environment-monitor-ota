#ifndef OTA_LAYOUT_H
#define OTA_LAYOUT_H

/* One layout contract for the future APP and BootLoader. */
#define OTA_INTERNAL_FLASH_BASE 0x08000000UL
#define OTA_BOOT_BASE           0x08000000UL
#define OTA_BOOT_SIZE           0x00008000UL
#define OTA_APP_BASE            0x08008000UL
#define OTA_APP_MAX_SIZE        0x00078000UL
#define OTA_FLASH_PAGE_SIZE     0x00000800UL
#define OTA_SRAM_BASE           0x20000000UL
#define OTA_SRAM_SIZE           0x00010000UL

#define OTA_STAGE_BASE          0x00000000UL
#define OTA_STAGE_MAX_SIZE      OTA_APP_MAX_SIZE
#define OTA_FLASH_TEST_SECTOR   0x00100000UL

#define OTA_RECORD_ADDRESS      0x00U
#define OTA_RECORD_SIZE         64U
#define OTA_RECORD_SCHEMA       1U
#define OTA_RECORD_FLAG_OFFSET  5U
#define OTA_RECORD_PENDING      0xA5U
#define OTA_EEPROM_TEST_ADDRESS 0xF0U

#if OTA_APP_BASE != (OTA_BOOT_BASE + OTA_BOOT_SIZE)
#error OTA layout overlaps or leaves an unintended gap
#endif
#if (OTA_APP_BASE + OTA_APP_MAX_SIZE) != 0x08080000UL
#error OTA APP does not match STM32F103ZET6 flash size
#endif
#if (OTA_STAGE_BASE + OTA_STAGE_MAX_SIZE) > OTA_FLASH_TEST_SECTOR
#error OTA staging overlaps the flash test sector
#endif

#endif
