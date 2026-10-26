/**
 * @file ota_meta.c
 * @brief 【升级记录】Boot 与 APP 共用的升级记录协议。
 *        APP 固件：OTA 版本从内部 Flash 0x08008000 运行，负责联网下载与业务。
 * 把结构体显式编码成 EEPROM 中的 64 字节记录，包含版本、大小、MD5、pending 标志和 CRC32，避免结构体对齐差异。
 */
#include "ota_meta.h"
#include "at24c02.h"
#include <string.h>

#if OTA_RECORD_SIZE != 64U || OTA_RECORD_FLAG_OFFSET != 5U
#error OTA metadata requires the agreed 64-byte layout and flag offset 5
#endif
#if OTA_RECORD_ADDRESS + OTA_RECORD_SIZE > OTA_EEPROM_TEST_ADDRESS
#error OTA metadata overlaps the EEPROM test area
#endif

/**
 * @brief 【升级记录】用反射多项式 0xEDB88320 将一个字节折入 CRC32。
 *        CRC 用来发现记录被意外改动，不代表来源认证。
 *
 * @param crc 上一步累计的 CRC32 状态。
 * @param value 参与本轮 CRC 累计的数据字节。
 *
 * @return 折入当前字节后的 CRC32 中间状态。
 */
static uint32_t CrcByte(uint32_t crc, uint8_t value)
{
    uint32_t bit;
    crc ^= value;
    for (bit = 0U; bit < 8U; ++bit) {
        crc = (crc >> 1) ^ ((crc & 1U) != 0U ? 0xEDB88320UL : 0U);
    }
    return crc;
}

/**
 * @brief 【升级记录】计算任意字节序列的 CRC32，使用全 1 初值和结果异或，供记录自检等调用。
 *
 * @param data 输入数据缓冲区。
 * @param length 数据长度，单位为字节。
 *
 * @return 输入字节序列的 CRC32 值。
 */
uint32_t OtaMeta_Crc32(const uint8_t *data, uint32_t length)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;
    for (i = 0U; i < length; ++i) {
        crc = CrcByte(crc, data[i]);
    }
    return crc ^ 0xFFFFFFFFUL;
}

/**
 * @brief 【升级记录】计算记录前 60 字节的 CRC，并把 pending 位置按 0 参与计算。
 *        这样最后独立提交或清除标志时，不必重写整份记录的 CRC。
 *
 * @param raw 64 字节升级记录的原始内容。
 *
 * @return 将 pending 字节按 0 处理后得到的记录 CRC32。
 */
/* 持久化记录布局（字节偏移）：
 * 0..3 魔数 OTA1；4 格式版本；5 pending；6 版本文本长度；7 保留。
 * 8..11 BIN 大小；12..27 MD5；28..59 版本文本；60..63 CRC32。
 * pending=0 表示没有已提交安装任务，A5 表示请求 Boot 安装。
 * A5 不是新 APP 健康确认，更不是旧版本回滚标志。
 */
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

/**
 * @brief 【升级记录】按小端顺序写入一个 32 位整数，明确持久化格式，避免依赖结构体内存布局。
 *
 * @param p 至少可写 4 字节的小端输出缓冲区。
 * @param value 待编码或累加的整数值。
 *
 * @return 无返回值。
 */
static void Put32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

/**
 * @brief 【升级记录】从记录中按小端顺序取出 32 位整数，是 Put32 的对应读取操作。
 *
 * @param p 小端整数的字节地址。
 *
 * @return 按小端顺序解码的 32 位整数。
 */
static uint32_t Get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief 【升级记录】检查版本字符是否属于数字、字母、点、下划线或连字符。
 *
 * @param c 待检查的单个字符。
 *
 * @return 非零表示版本字符合法，0 表示不满足该条件。
 */
static int VersionChar(uint8_t c)
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') || c == '.' || c == '_' || c == '-';
}

/**
 * @brief 【升级记录】在固定 32 字节容量内寻找结束符并验证字符。
 *        格式不合法或未结束返回 0。
 *
 * @param version 固件版本字符串。
 *
 * @return 有效版本字符数；格式错误或缺少结束符时返回 0。
 */
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

/**
 * @brief 【升级记录】将非提交元数据编码为 64 字节升级记录。
 *        1. 检查参数、版本长度、固件大小和未提交状态。
 *        2. 清空输出记录，写入魔数、格式版本、大小、MD5 和版本文本。
 *        3. 计算记录 CRC32 并写入末尾，形成固定 64 字节格式。
 *
 * @param meta 固件大小、版本、MD5 及安装状态。
 * @param raw 接收编码结果的 64 字节缓冲区。
 *
 * @return 返回状态：
 *         - OTA_META_OK：编码成功。
 *         - OTA_META_INVALID：参数、版本、大小或提交状态无效。
 */
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

/**
 * @brief 【升级记录】验证并解码 64 字节升级记录。
 *        1. 检测全 00 或全 FF 的空记录。
 *        2. 按小端格式取出大小，检查魔数、格式版本、标志和字段范围。
 *        3. 检查 CRC32、版本字符及未使用字段是否符合格式。
 *        4. 全部通过后清空并填充输出结构体，失败时不提供有效元数据。
 *
 * @param raw 64 字节升级记录的原始内容。
 * @param meta 解码成功后填充的元数据；失败时不可使用。
 *
 * @return 返回状态：
 *         - OTA_META_OK：记录有效，输出已填充。
 *         - OTA_META_EMPTY：记录全为 00 或 FF。
 *         - OTA_META_INVALID：字段、格式或 CRC 校验失败。
 */
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

/**
 * @brief 【升级记录】从 AT24C02 读取固定记录再解码，返回值区分 I2C 访问失败和数据格式失败。
 *        这两类故障的处理不能混为一谈。
 *
 * @param meta 读取并校验成功后填充的元数据。
 *
 * @return 返回状态：
 *         - OTA_META_OK：记录有效，输出已填充。
 *         - OTA_META_EMPTY：记录为空。
 *         - OTA_META_INVALID：参数或记录无效。
 *         - OTA_META_IO_ERROR：EEPROM 读取失败。
 */
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

/**
 * @brief 【升级记录】清除并回读确认待安装标志。
 *        1. 只向 pending 所在字节写入 0，不删除版本、大小或 MD5。
 *        2. 立即回读标志，确认值为 0 后返回成功。
 *
 * @return 返回状态：
 *         - OTA_META_OK：标志已清除且回读一致。
 *         - OTA_META_IO_ERROR：标志写入、读取或回读确认失败。
 */
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

/**
 * @brief 【升级记录】写入并回读验证未提交的升级记录。
 *        1. 将输入元数据编码为 pending 为 0 的完整记录。
 *        2. 先清除并回读确认旧安装标志，使旧任务失效。
 *        3. 写入 64 字节记录，随后读取并逐字节比较。
 *        4. 再次解码回读记录，成功后才向上层报告草稿写入完成。
 *
 * @param meta 固件大小、版本、MD5 及安装状态。
 *
 * @return 返回状态：
 *         - OTA_META_OK：非提交记录写入且回读校验成功。
 *         - OTA_META_INVALID：输入元数据或回读记录无效。
 *         - OTA_META_IO_ERROR：标志清除或记录读写确认失败。
 */
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
    /* 先让旧任务失效，确认后才改写其余字段，防止半写记录被当成任务。 */
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
