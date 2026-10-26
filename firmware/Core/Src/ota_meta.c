#include "ota_meta.h"
#include "at24c02.h"
#include <string.h>

#if OTA_RECORD_SIZE != 64U || OTA_RECORD_FLAG_OFFSET != 5U
#error OTA metadata requires the agreed 64-byte layout and flag offset 5
#endif
#if OTA_RECORD_ADDRESS + OTA_RECORD_SIZE > OTA_EEPROM_TEST_ADDRESS
#error OTA metadata overlaps the EEPROM test area
#endif

static uint32_t CrcByte(uint32_t crc, uint8_t value)
{
    uint32_t bit;
    crc ^= value;
    for (bit = 0U; bit < 8U; ++bit) {
        crc = (crc >> 1) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
    }
    return crc;
}

uint32_t OtaMeta_Crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;
    for (i = 0U; i < length; ++i) {
        crc = CrcByte(crc, data[i]);
    }
    return crc ^ 0xFFFFFFFFUL;
}

/* CRC excludes pending so the final commit is a separate verified byte write. */
static uint32_t RecordCrc(const uint8_t *raw)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;
    for (i = 0U; i < 60U; ++i) {
        /* Commit/clear is a separate byte write after the body is verified. */
        crc = CrcByte(crc, i == OTA_RECORD_FLAG_OFFSET ? 0U : raw[i]);
    }
    return crc ^ 0xFFFFFFFFUL;
}

static void Put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t Get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int VersionChar(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-';
}

static uint32_t VersionLength(const char version[32])
{
    uint32_t i;
    for (i = 0U; i < 32U; ++i) {
        if (version[i] == '\0') {
            return i;
        }
        if (!VersionChar((uint8_t)version[i])) {
            return 0U;
        }
    }
    return 0U;
}

OtaMetaResult OtaMeta_Encode(const OtaMeta *meta, uint8_t raw[OTA_RECORD_SIZE])
{
    uint32_t length;
    if (meta == NULL || raw == NULL) {
        return OTA_META_INVALID;
    }
    length = VersionLength(meta->version);
    if (length == 0U || meta->image_size < 8U ||
        meta->image_size > OTA_APP_MAX_SIZE || meta->pending != 0U) {
        return OTA_META_INVALID;
    }
    memset(raw, 0, OTA_RECORD_SIZE);
    memcpy(raw, "OTA1", 4U);
    raw[4] = OTA_RECORD_SCHEMA;
    raw[6] = (uint8_t)length;
    Put32(&raw[8], meta->image_size);
    memcpy(&raw[12], meta->md5, 16U);
    memcpy(&raw[28], meta->version, length);
    Put32(&raw[60], RecordCrc(raw));
    return OTA_META_OK;
}

OtaMetaResult OtaMeta_Decode(const uint8_t raw[OTA_RECORD_SIZE], OtaMeta *meta)
{
    uint32_t i;
    uint32_t size;
    uint32_t length;
    int all_zero = 1;
    int all_ff = 1;
    if (raw == NULL || meta == NULL) {
        return OTA_META_INVALID;
    }
    for (i = 0U; i < OTA_RECORD_SIZE; ++i) {
        all_zero = all_zero && raw[i] == 0U;
        all_ff = all_ff && raw[i] == 0xFFU;
    }
    if (all_zero || all_ff) {
        return OTA_META_EMPTY;
    }
    size = Get32(&raw[8]);
    length = raw[6];
    if (memcmp(raw, "OTA1", 4U) != 0 || raw[4] != OTA_RECORD_SCHEMA ||
        (raw[5] != 0U && raw[5] != OTA_RECORD_PENDING) || raw[7] != 0U ||
        length == 0U || length > 31U || size < 8U || size > OTA_APP_MAX_SIZE ||
        Get32(&raw[60]) != RecordCrc(raw)) {
        return OTA_META_INVALID;
    }
    for (i = 0U; i < 32U; ++i) {
        if (i < length ? !VersionChar(raw[28U + i]) : raw[28U + i] != 0U) {
            return OTA_META_INVALID;
        }
    }
    memset(meta, 0, sizeof(*meta));
    meta->image_size = size;
    memcpy(meta->md5, &raw[12], 16U);
    memcpy(meta->version, &raw[28], length);
    meta->pending = raw[5];
    return OTA_META_OK;
}

OtaMetaResult OtaMeta_Read(OtaMeta *meta)
{
    uint8_t raw[OTA_RECORD_SIZE];
    if (meta == NULL) {
        return OTA_META_INVALID;
    }
    if (At24c02_Read(OTA_RECORD_ADDRESS, raw, sizeof(raw)) != HAL_OK) {
        return OTA_META_IO_ERROR;
    }
    return OtaMeta_Decode(raw, meta);
}

OtaMetaResult OtaMeta_ClearPending(void)
{
    uint8_t flag = 0U;
    uint8_t actual = 0xFFU;
    uint32_t address = OTA_RECORD_ADDRESS + OTA_RECORD_FLAG_OFFSET;
    if (At24c02_Write(address, &flag, 1U) != HAL_OK ||
        At24c02_Read(address, &actual, 1U) != HAL_OK || actual != 0U) {
        return OTA_META_IO_ERROR;
    }
    return OTA_META_OK;
}

OtaMetaResult OtaMeta_WriteDraft(const OtaMeta *meta)
{
    uint8_t raw[OTA_RECORD_SIZE];
    uint8_t actual[OTA_RECORD_SIZE];
    OtaMeta decoded;
    OtaMetaResult result;
    result = OtaMeta_Encode(meta, raw);
    if (result != OTA_META_OK) {
        return result;
    }
    /* If invalidation is not confirmed, never replace the old record body. */
    result = OtaMeta_ClearPending();
    if (result != OTA_META_OK) {
        return result;
    }
    if (At24c02_Write(OTA_RECORD_ADDRESS, raw, sizeof(raw)) != HAL_OK ||
        At24c02_Read(OTA_RECORD_ADDRESS, actual, sizeof(actual)) != HAL_OK ||
        memcmp(raw, actual, sizeof(raw)) != 0) {
        return OTA_META_IO_ERROR;
    }
    return OtaMeta_Decode(actual, &decoded);
}
