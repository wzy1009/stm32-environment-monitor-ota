/**
 * @file ota_install.c
 * @brief 【固件安装】Boot 端的离线安装器。
 *        Boot 固件：在内部 Flash 0x08000000 启动，负责安装和转交控制权。
 * 先检查外部 BIN 与元数据，再只擦除内部 APP 页并逐半字写入；
 * 内部 MD5/向量验证成功后清 A5 并复位，失败保留待安装状态以供重试。
 */
#include "ota_install.h"
#include "ota_image_diag.h"
#include "w25q128.h"
#include "usart.h"
#include "boot_display.h"
#include <stdio.h>
#include <string.h>

#define PRECHECK_ATTEMPTS 3U
#define PRECHECK_RETRY_MS 200U
#define PRECHECK_RESET_MAGIC 0xB071U

#if FLASH_PAGE_SIZE != OTA_FLASH_PAGE_SIZE
#error HAL flash page size does not match STM32F103ZE OTA layout
#endif

/**
 * @brief 【固件安装】输出安装阶段日志，用于观察预检查、擦写、回读及复位的先后顺序。
 *
 * @param text 以结束符结尾的文本。
 *
 * @return 无返回值。
 */
static void Log(const char *text)
{
    if (HAL_UART_Transmit(&huart1, (const uint8_t *)text,
                         (uint16_t)strlen(text), 200U) != HAL_OK) {
        Error_Handler();
    }
}

/**
 * @brief 【固件安装】把 16 字节 MD5 转为 32 位十六进制文本，末尾补字符串结束符。
 *        仅用于诊断，不改变摘要内容。
 *
 * @param digest 16 字节 MD5 摘要。
 * @param text 至少 33 字节，保存 32 位十六进制摘要和结束符。
 *
 * @return 无返回值。
 */
static void HexDigest(const uint8_t digest[16], char text[33])
{
    static const char digits[] = "0123456789abcdef";
    uint32_t i;
    for (i = 0U; i < 16U; ++i) {
        text[i * 2U] = digits[digest[i] >> 4];
        text[i * 2U + 1U] = digits[digest[i] & 15U];
    }
    text[32] = '\0';
}

/**
 * @brief 【固件安装】外部镜像预检查连续失败后，按当前记录的 CRC 在备份寄存器中登记一次自动复位机会。
 *        1. 以当前记录的 CRC 值作为本次任务标识，检查备份寄存器。
 *        2. 尚未用过自动复位机会时，写入任务标识并回读确认。
 *        3. 确认成功后复位 STM32；相同任务已用过一次时停止，避免无限复位。
 *
 * @param meta 固件大小、版本、MD5 及安装状态。
 *
 * @return 无返回值。
 */
static void ResetAfterPrecheckFailure(const OtaMeta *meta)
{
    OtaMeta draft;
    uint8_t raw[OTA_RECORD_SIZE];
    uint32_t key, access;
    int used, saved = 0;
    if (meta == NULL || meta->pending != OTA_RECORD_PENDING) { return; }
    draft = *meta;
    draft.pending = 0U;
    if (OtaMeta_Encode(&draft, raw) != OTA_META_OK) { return; }
    key = (uint32_t)raw[60] | ((uint32_t)raw[61] << 8) |
          ((uint32_t)raw[62] << 16) | ((uint32_t)raw[63] << 24);
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_BKP_CLK_ENABLE();
    access = PWR->CR & PWR_CR_DBP;
    HAL_PWR_EnableBkUpAccess();
    /* DR1..3 survive system reset; a RAM counter would cause a reset loop. */
    used = BKP->DR1 == PRECHECK_RESET_MAGIC &&
           BKP->DR2 == (key & 0xFFFFU) && BKP->DR3 == (key >> 16);
    if (!used) {
        BKP->DR1 = 0U;
        BKP->DR2 = key & 0xFFFFU;
        BKP->DR3 = key >> 16;
        BKP->DR1 = PRECHECK_RESET_MAGIC;
        __DSB();
        saved = BKP->DR1 == PRECHECK_RESET_MAGIC &&
                BKP->DR2 == (key & 0xFFFFU) && BKP->DR3 == (key >> 16);
    }
    if (access == 0U) { HAL_PWR_DisableBkUpAccess(); }
    if (used) {
        Log("[预检] 一次自动复位已用尽，停止并保留A5及暂存固件\r\n");
    } else if (!saved) {
        Log("[预检] 自动复位已禁用：备份标记回读失败\r\n");
    } else {
        Log("[预检] 第1次自动复位：保留A5及固件，内部APP未擦除\r\n");
        HAL_Delay(PRECHECK_RETRY_MS);
        NVIC_SystemReset();
    }
}

/**
 * @brief 【固件安装】安装成功后清理备份寄存器中的预检查复位标记，使后续新任务不继承本次故障记录。
 *
 * @return 无返回值。
 */
static void ClearPrecheckResetMark(void)
{
    uint32_t access;
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_BKP_CLK_ENABLE();
    access = PWR->CR & PWR_CR_DBP;
    HAL_PWR_EnableBkUpAccess();
    BKP->DR1 = 0U;
    __DSB();
    if (access == 0U) { HAL_PWR_DisableBkUpAccess(); }
}

/**
 * @brief 【固件安装】擦除内部 Flash 前的安全关卡。
 *        1. 确认输入记录存在且 pending 为 0xA5。
 *        2. 重读 EEPROM，比较版本、大小、MD5 和安装标志是否仍与本次任务一致。
 *        3. 详细检查外部 Flash 的 JEDEC ID、固件向量和完整 MD5，输出诊断信息。
 *        4. 最多检查三轮，轮间等待 200 ms；全部失败则返回，内部 APP 尚未擦除。
 *
 * @param meta 固件大小、版本、MD5 及安装状态。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
static HAL_StatusTypeDef Precheck(const OtaMeta *meta)
{
    static const char *const reasons[] = {
        "通过", "记录无效", "标识读取失败", "型号不支持", "标识不稳定",
        "向量读取失败", "向量无效", "数据读取失败", "MD5不一致"
    };
    OtaMeta saved;
    OtaMetaResult record_result;
    OtaImageDiag diag;
    uint32_t attempt, i;
    char line[224], expected[33], actual[33];
    if (meta == NULL || meta->pending != OTA_RECORD_PENDING) {
        Log("[预检] 输入错误：缺少已提交升级记录\r\n");
        return HAL_ERROR;
    }
    for (attempt = 1U; attempt <= PRECHECK_ATTEMPTS; ++attempt) {
        (void)snprintf(line, sizeof(line), "[预检] 第%lu/%u轮检查\r\n",
                       (unsigned long)attempt, (unsigned int)PRECHECK_ATTEMPTS);
        Log(line);
        record_result = OtaMeta_Read(&saved);
        if (record_result != OTA_META_OK) {
            (void)snprintf(line, sizeof(line), "[预检] 记录读取结果=%u（1空记录，2格式或CRC，3读写错误）\r\n",
                           (unsigned int)record_result);
            Log(line);
        } else if (saved.pending != OTA_RECORD_PENDING || saved.image_size != meta->image_size ||
                   memcmp(saved.md5, meta->md5, 16U) != 0 ||
                   memcmp(saved.version, meta->version, sizeof(saved.version)) != 0) {
            (void)snprintf(line, sizeof(line),
                "[预检] 记录已变：标志=0x%02X 大小=%lu 大小/MD5/版本匹配=%u/%u/%u\r\n",
                (unsigned int)saved.pending, (unsigned long)saved.image_size,
                (unsigned int)(saved.image_size == meta->image_size),
                (unsigned int)(memcmp(saved.md5, meta->md5, 16U) == 0),
                (unsigned int)(memcmp(saved.version, meta->version, sizeof(saved.version)) == 0));
            Log(line);
        } else {
            HAL_StatusTypeDef checked = OtaImage_VerifyStageDetailed(meta, &diag);
            (void)snprintf(line, sizeof(line),
                "[预检] 镜像=%s HAL=%u 偏移=0x%08lX 初始栈=0x%08lX 入口=0x%08lX\r\n",
                reasons[diag.reason], (unsigned int)diag.hal_status, (unsigned long)diag.offset,
                (unsigned long)diag.msp, (unsigned long)diag.reset);
            Log(line);
            for (i = 0U; i < diag.id_count; ++i) {
                (void)snprintf(line, sizeof(line), "[预检] JEDEC[%lu]=0x%06lX\r\n",
                               (unsigned long)i, (unsigned long)diag.id[i]);
                Log(line);
            }
            if (diag.reason == OTA_IMAGE_DIAG_MD5 || checked == HAL_OK) {
                HexDigest(meta->md5, expected); HexDigest(diag.actual_md5, actual);
                (void)snprintf(line, sizeof(line), "[预检] MD5期望=%s 实际=%s\r\n", expected, actual);
                Log(line);
            }
            if (checked == HAL_OK) { return HAL_OK; }
        }
        if (attempt < PRECHECK_ATTEMPTS) {
            Log("[预检] 200毫秒后重试，内部APP未改动\r\n");
            HAL_Delay(PRECHECK_RETRY_MS);
        }
    }
    return HAL_ERROR;
}

/**
 * @brief 【固件安装】将已提交的外部固件安装到内部 APP 分区。
 *        1. 调用 Precheck() 再次验证外部固件和已提交记录。
 *        2. 根据 BIN 大小向上取整计算 2 KiB 页数，只擦除内部 APP 所需区域。
 *        3. 从外部 Flash 每次读取最多 256 字节，按 16 位半字写入内部 Flash。
 *        4. 锁定内部 Flash，重新读取 APP 并校验向量和 MD5。
 *        5. 成功后清除 pending 并回读确认，再清理本任务的预检查复位标记。
 *        6. 复位 STM32，由下一次 Boot 启动新 APP；失败时保留必要安装状态。
 *        保留外部新 BIN 支持重装，不等于备份了旧 APP 并能自动回滚。
 *
 * @param meta 固件大小、版本、MD5 及安装状态。
 *
 * @return 返回状态：
 *         - HAL_OK：操作成功。
 *         - HAL_ERROR：参数、数据校验或访问操作失败。
 */
HAL_StatusTypeDef OtaInstall_Run(const OtaMeta *meta)
{
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error = 0xFFFFFFFFUL;
    uint32_t offset = 0U;
    uint32_t count;
    uint32_t i;
    uint16_t halfword;
    uint8_t data[256];
    char line[100];
    HAL_StatusTypeDef result;
#if OTA_INSTALL_POWER_CUT_TEST == 1U
    uint8_t paused = 0U;
#elif OTA_INSTALL_POWER_CUT_TEST != 0U
#error OTA_INSTALL_POWER_CUT_TEST must be 0 or 1
#endif
    BootDisplay_Show("PRECHECK", meta != NULL ? meta->version : "", 0U, 1U);
    if (Precheck(meta) != HAL_OK) {
        BootDisplay_Show("FAILED", meta != NULL ? meta->version : "", 0U, 1U);
        HAL_Delay(500U);
        Log("[安装] 已停止：预检未通过，内部未擦除，保留A5及暂存固件\r\n");
        ResetAfterPrecheckFailure(meta);
        return HAL_ERROR;
    }
    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    /* 只从 APP 首地址擦除，Boot 和首次初始化标记不在擦除范围。 */
    erase.PageAddress = OTA_APP_BASE;
    erase.NbPages = (meta->image_size + OTA_FLASH_PAGE_SIZE - 1U) / OTA_FLASH_PAGE_SIZE;
    if (erase.NbPages == 0U || erase.NbPages > OTA_APP_MAX_SIZE / OTA_FLASH_PAGE_SIZE) {
        BootDisplay_Show("FAILED", meta->version, 0U, 1U);
        return HAL_ERROR;
    }
    (void)snprintf(line, sizeof(line), "[安装] 开始：版本=%s 大小=%lu 页数=%lu\r\n",
                   meta->version, (unsigned long)meta->image_size, (unsigned long)erase.NbPages);
    Log(line);
    Log("[安装] 暂存固件MD5及向量通过，仅擦除APP页\r\n");
    BootDisplay_Show("ERASE", meta->version, 10U, 1U);
    if (HAL_FLASH_Unlock() != HAL_OK) {
        BootDisplay_Show("FAILED", meta->version, 0U, 1U);
        return HAL_ERROR;
    }
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_EOP | FLASH_FLAG_PGERR | FLASH_FLAG_WRPERR);
    result = HAL_FLASHEx_Erase(&erase, &page_error);
    if (result == HAL_OK) BootDisplay_Show("WRITE", meta->version, 20U, 1U);
    while (result == HAL_OK && offset < meta->image_size) {
        count = meta->image_size - offset;
        if (count > sizeof(data)) count = sizeof(data);
        /* 内部 Flash 按半字写；BIN 若为奇数字节，最后一个半字高位补 FF。 */
        memset(data, 0xFF, sizeof(data));
        result = W25Q128_Read(OTA_STAGE_BASE + offset, data, count);
        if (result != HAL_OK) break;
        for (i = 0U; i < count; i += 2U) {
            halfword = (uint16_t)data[i] | ((uint16_t)data[i + 1U] << 8);
            result = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD,
                                      OTA_APP_BASE + offset + i, halfword);
            if (result != HAL_OK) break;
        }
        offset += count;
        BootDisplay_Show("WRITE", meta->version,
                         (uint8_t)(20U + (offset * 60U) / meta->image_size), 0U);
#if OTA_INSTALL_POWER_CUT_TEST == 1U
        if (result == HAL_OK && paused == 0U) {
            Log("[安装] 断电测试暂停10秒：首块已写入，标志=0xA5\r\n");
            paused = 1U;
            HAL_Delay(10000U);
        }
#endif
    }
    if (HAL_FLASH_Lock() != HAL_OK) result = HAL_ERROR;
    if (result == HAL_OK) BootDisplay_Show("VERIFY", meta->version, 90U, 1U);
    if (result != HAL_OK || OtaImage_VerifyInternal(meta) != HAL_OK) {
        BootDisplay_Show("FAILED", meta->version, 0U, 1U);
        Log("[安装] 已停止：擦除、写入或回读失败，保留待安装固件\r\n");
        return HAL_ERROR;
    }
    Log("[安装] 内部APP的MD5及向量校验通过\r\n");
    /* 内部镜像已回读验证成功，才清 A5；中途断电则保留 A5，下次重装。 */
    if (OtaMeta_ClearPending() != OTA_META_OK) {
        BootDisplay_Show("FAILED", meta->version, 0U, 1U);
        Log("[安装] 已停止：清除标志未确认，复位后重试\r\n");
        return HAL_ERROR;
    }
    ClearPrecheckResetMark();
    Log("[安装] 成功，标志已清除，复位后启动新APP\r\n");
    BootDisplay_Show("SUCCESS", meta->version, 100U, 1U);
    HAL_Delay(500U);
    /* New APP starts from a fresh MCU reset; cleared flag prevents reinstall. */
    NVIC_SystemReset();
    return HAL_OK;
}
