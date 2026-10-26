#ifndef OTA_DOWNLOAD_H
#define OTA_DOWNLOAD_H
#include <stdint.h>
#include "main.h"

typedef enum { OTA_PROGRESS_IDLE, OTA_PROGRESS_PREPARE, OTA_PROGRESS_ERASE,
    OTA_PROGRESS_DOWNLOAD, OTA_PROGRESS_VERIFY, OTA_PROGRESS_COMMIT, OTA_PROGRESS_FAILED } OtaProgressStage;
typedef struct {
    uint32_t sequence, done, total;
    OtaProgressStage stage;
    char version[32], error[64];
} OtaDownloadProgress;
void OtaDownload_GetProgress(OtaDownloadProgress *out);
void OtaDownload_Task(void);
uint8_t OtaDownload_IsBusy(void);
uint8_t OtaDownload_NeedsRecovery(void);
uint8_t OtaDownload_ResetForRetry(void);
#endif
