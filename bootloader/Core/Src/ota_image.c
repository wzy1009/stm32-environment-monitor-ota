/**
 * @file ota_image.c
 * @brief 【固件校验】固件镜像检查与安装标志提交。
 *        Boot 固件：在内部 Flash 0x08000000 启动，负责安装和转交控制权。
 * 分块读取固件计算 MD5，并检查向量表中的初始栈和复位入口；
 * CRC 用于元数据，MD5 用于固件内容，二者不是同一层检查。
 */
#include "ota_image.h"
#include "ota_image_diag.h"
#include "at24c02.h"
#include "w25q128.h"
#include "utils_md5.h"
#include <string.h>

/**
 * @brief 【固件校验】填充镜像预检查的失败原因、HAL 状态与偏移，向上层提供可定位的诊断，并返回 HAL_ERROR。
 *
 * @param diag 可选诊断信息，NULL 表示不保存诊断。
 * @param reason 详细镜像检查失败原因枚举。
 * @param status 引起失败的 HAL 状态或检查步骤状态。
 * @param offset 失败的数据偏移或检查步骤索引。
 *
 * @return HAL_ERROR。
 */
static HAL_StatusTypeDef Reject(OtaImageDiag *diag, OtaImageDiagReason reason,
                                HAL_StatusTypeDef status, uint32_t offset)
{
    if (diag != NULL) {
        diag->reason = reason; diag->hal_status = status; diag->offset = offset;
    }
    return HAL_ERROR;
}

/**
 * @brief 【固件校验】连续三次读取外部 Flash 的 JEDEC ID，检查是否为支持芯片且读数稳定。
 *        可选 diag 保存每次读数。
 *
 * @param diag 可选诊断信息，NULL 表示不保存诊断。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
static HAL_StatusTypeDef CheckDevice(OtaImageDiag *diag)
{
    uint32_t id;
    uint32_t first = 0U;
    uint32_t i;
    HAL_StatusTypeDef result;
    for (i = 0U; i < 3U; ++i) {
        result = W25Q128_ReadJedecId(&id);
        if (result != HAL_OK) { return Reject(diag, OTA_IMAGE_DIAG_ID_IO, result, i); }
        if (diag != NULL) { diag->id[i] = id; diag->id_count = i + 1U; }
        if (id != 0x522118UL && id != 0xEF4018UL) {
            return Reject(diag, OTA_IMAGE_DIAG_ID_UNSUPPORTED, HAL_OK, i);
        }
        if (i != 0U && id != first) {
            return Reject(diag, OTA_IMAGE_DIAG_ID_UNSTABLE, HAL_OK, i);
        }
        first = id;
    }
    return HAL_OK;
}

/**
 * @brief 【固件校验】检查外部固件存储器是否响应且型号/读数受支持，不擦除也不写入芯片。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
HAL_StatusTypeDef OtaImage_CheckDevice(void) { return CheckDevice(NULL); }

/**
 * @brief 【固件校验】internal 决定从内部 APP 区还是外部暂存区读取。
 *        1. 检查元数据，再选择内部 APP 或外部暂存区作为读取源。
 *        2. 读取 BIN 前 8 字节，校验初始 MSP、Thumb 标志和复位入口范围。
 *        3. 以 256 字节缓存循环读取，持续更新 MD5 上下文。
 *        4. 计算最终 16 字节摘要，与通知中的期望 MD5 比较。
 *
 * @param meta 固件大小、版本、MD5 及安装状态。
 * @param internal 非零检查内部 APP，0 检查外部暂存区。
 * @param diag 可选诊断信息，NULL 表示不保存诊断。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
static HAL_StatusTypeDef Verify(const OtaMeta *meta, int internal, OtaImageDiag *diag)
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
    HAL_StatusTypeDef result;
    if (diag != NULL) { memset(diag, 0, sizeof(*diag)); }
    if (meta == NULL || (meta->pending != 0U && meta->pending != OTA_RECORD_PENDING)) {
        return Reject(diag, OTA_IMAGE_DIAG_META, HAL_ERROR, 0U);
    }
    draft = *meta;
    draft.pending = 0U;
    if (OtaMeta_Encode(&draft, raw) != OTA_META_OK) {
        return Reject(diag, OTA_IMAGE_DIAG_META, HAL_ERROR, 0U);
    }
    if (internal) {
        memcpy(data, (const void *)OTA_APP_BASE, 8U);
    } else {
        if (CheckDevice(diag) != HAL_OK) { return HAL_ERROR; }
        result = W25Q128_Read(OTA_STAGE_BASE, data, 8U);
        if (result != HAL_OK) { return Reject(diag, OTA_IMAGE_DIAG_VECTOR_READ, result, 0U); }
    }
    memcpy(&msp, data, 4U);
    memcpy(&reset, data + 4U, 4U);
    if (diag != NULL) { diag->msp = msp; diag->reset = reset; }
    /* BIN 前 8 字节是初始 MSP 与复位入口，不是可直接执行的普通数据。 */
    entry = reset & ~1UL;
    if (msp <= OTA_SRAM_BASE || msp > OTA_SRAM_BASE + OTA_SRAM_SIZE ||
        (msp & 7U) != 0U || (reset & 1U) == 0U || entry < OTA_APP_BASE + 8U ||
        entry >= OTA_APP_BASE + meta->image_size) {
        return Reject(diag, OTA_IMAGE_DIAG_VECTOR_INVALID, HAL_ERROR, 0U);
    }
    utils_md5_init(&ctx);
    utils_md5_starts(&ctx);
    while (offset < meta->image_size) {
        count = meta->image_size - offset;
        if (count > sizeof(data)) count = sizeof(data);
        if (internal) {
            memcpy(data, (const void *)(OTA_APP_BASE + offset), count);
        } else {
            result = W25Q128_Read(OTA_STAGE_BASE + offset, data, count);
            if (result != HAL_OK) {
                utils_md5_free(&ctx);
                return Reject(diag, OTA_IMAGE_DIAG_DATA_READ, result, offset);
            }
        }
        utils_md5_update(&ctx, data, count);
        offset += count;
    }
    utils_md5_finish(&ctx, digest);
    utils_md5_free(&ctx);
    if (diag != NULL) { memcpy(diag->actual_md5, digest, sizeof(digest)); }
    if (memcmp(digest, meta->md5, sizeof(digest)) != 0) {
        return Reject(diag, OTA_IMAGE_DIAG_MD5, HAL_OK, 0U);
    }
    return HAL_OK;
}

/**
 * @brief 【固件校验】校验外部暂存镜像的向量和 MD5。
 *        验证外部暂存 BIN 的向量与 MD5。
 *        APP 提交前和 Boot 擦除内部程序前都需要这个关卡，不能仅信任曾经下载成功的日志。
 *
 * @param meta 固件大小、版本、MD5 及安装状态。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
HAL_StatusTypeDef OtaImage_VerifyStage(const OtaMeta *meta)
{
    return Verify(meta, 0, NULL);
}

/**
 * @brief 【固件校验】校验外部暂存镜像并返回详细诊断信息。
 *        Boot 预检查使用的详细版本；
 *        除 HAL 结果外还返回芯片 ID、向量、失败偏移或实际 MD5，便于排查偶发外部读错。
 *
 * @param meta 固件大小、版本、MD5 及安装状态。
 * @param diag 可选诊断信息，NULL 表示不保存诊断。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
HAL_StatusTypeDef OtaImage_VerifyStageDetailed(const OtaMeta *meta, OtaImageDiag *diag)
{
    return Verify(meta, 0, diag);
}

/**
 * @brief 【固件校验】回读校验内部 APP 镜像的向量和 MD5。
 *        从内部 APP 地址重新读出安装后的数据并校验向量和 MD5；
 *        写入函数返回成功不足以代替此回读检查。
 *
 * @param meta 固件大小、版本、MD5 及安装状态。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
HAL_StatusTypeDef OtaImage_VerifyInternal(const OtaMeta *meta)
{
    return Verify(meta, 1, NULL);
}

/**
 * @brief 【固件校验】确认外部镜像及记录后提交待安装标志。
 *        1. 确认输入为未提交元数据，再次验证外部 BIN。
 *        2. 写入完整的非提交记录，并确认 EEPROM 回读校验成功。
 *        3. 最后单独把 pending 字节写为 0xA5。
 *        4. 重新读取记录，核对标志、大小、版本和 MD5，全部一致才返回成功。
 *
 * @param meta 固件大小、版本、MD5 及安装状态。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
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
