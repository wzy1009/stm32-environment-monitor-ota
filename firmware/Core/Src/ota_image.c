/**
 * @file ota_image.c
 * @brief 【固件校验】固件镜像检查与安装标志提交。
 *        APP 固件：OTA 版本从内部 Flash 0x08008000 运行，负责联网下载与业务。
 * 分块读取固件计算 MD5，并检查向量表中的初始栈和复位入口；
 * CRC 用于元数据，MD5 用于固件内容，二者不是同一层检查。
 */
#include "ota_image.h"
#include "at24c02.h"
#include "w25q128.h"
#include "utils_md5.h"
#include <string.h>

/**
 * @brief 【固件校验】检查外部固件存储器是否响应且型号/读数受支持，不擦除也不写入芯片。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
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

/**
 * @brief 【固件校验】internal 决定从内部 APP 区还是外部暂存区读取。
 *        1. 检查元数据，再选择内部 APP 或外部暂存区作为读取源。
 *        2. 读取 BIN 前 8 字节，校验初始 MSP、Thumb 标志和复位入口范围。
 *        3. 以 256 字节缓存循环读取，持续更新 MD5 上下文。
 *        4. 计算最终 16 字节摘要，与通知中的期望 MD5 比较。
 *
 * @param meta 固件大小、版本、MD5 及安装状态。
 * @param internal 非零检查内部 APP，0 检查外部暂存区。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - 其他 HAL 状态表示操作失败、器件未就绪或超时。
 */
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
        memcpy(data, (const void *)OTA_APP_BASE, 8U);
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
            memcpy(data, (const void *)(OTA_APP_BASE + offset), count);
        } else if (W25Q128_Read(OTA_STAGE_BASE + offset, data, count) != HAL_OK) {
            utils_md5_free(&ctx);
            return HAL_ERROR;
        }
        utils_md5_update(&ctx, data, count);
        offset += count;
    }
    utils_md5_finish(&ctx, digest);
    utils_md5_free(&ctx);
    return memcmp(digest, meta->md5, sizeof(digest)) == 0 ? HAL_OK : HAL_ERROR;
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
    return Verify(meta, 0);
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
    return Verify(meta, 1);
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
