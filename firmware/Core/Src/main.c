#include "main.h"
#include "oled.h"
#include "board_io.h"
#include "dht11.h"
#include "monitor_state.h"
#include "cloud_task.h"
#include "usart.h"
#include "i2c.h"
#include "spi.h"
#include "ota_download.h"
#include "ota_app.h"
#include "ota_notice.h"
#include <stdio.h>
#include <string.h>

static uint8_t display_frame[1024];
static MonitorState *active_monitor;

void OtaApp_Checkpoint(void)
{
    if (active_monitor != NULL) {
        uint32_t now = HAL_GetTick();
        Monitor_Update(active_monitor, now, BoardIo_Key0Down(), BoardIo_Key1Down(), BoardIo_Motion());
        BoardIo_SetOutputs(now, active_monitor->motion, active_monitor->alarm);
    }
}


uint8_t OtaApp_CanInstall(void)
{
    return active_monitor != NULL && active_monitor->armed == 0U;
}

static void RenderStatus(const MonitorState *state, uint32_t now)
{
    const Dht11Sample *sample = Dht11_GetSample();
    static const char * const dht_text[] = {"DHT WAIT", "DHT OK", "DHT TIMEOUT", "DHT CHECKSUM", "DHT DATA ERROR"};
    char line[32];
    int temperature;
    memset(display_frame, 0, sizeof(display_frame));
    Oled_DrawText(display_frame, 0U, 0U, "ENV V" FIRMWARE_VERSION);
    if (sample->valid != 0U) {
        temperature = sample->temperature_x10;
        (void)snprintf(line, sizeof(line), "TEMP %s%d.%d C", temperature < 0 ? "-" : "",
                       (temperature < 0 ? -temperature : temperature) / 10,
                       (temperature < 0 ? -temperature : temperature) % 10);
        Oled_DrawText(display_frame, 0U, 1U, line);
        (void)snprintf(line, sizeof(line), "HUM %u.%u RH", (unsigned)(sample->humidity_x10 / 10U),
                       (unsigned)(sample->humidity_x10 % 10U));
        Oled_DrawText(display_frame, 0U, 2U, line);
    } else {
        Oled_DrawText(display_frame, 0U, 1U, "TEMP --.- C");
        Oled_DrawText(display_frame, 0U, 2U, "HUM --.- RH");
    }
    Oled_DrawText(display_frame, 0U, 3U, dht_text[Dht11_GetStatus()]);
    Oled_DrawText(display_frame, 0U, 4U, state->motion != 0U ? "PIR MOTION" : "PIR CLEAR");
    Oled_DrawText(display_frame, 0U, 5U, state->armed == 0U ? "MODE DISARMED" :
                  Monitor_IsArming(state, now) != 0U ? "MODE ARMING" : "MODE ARMED");
    Oled_DrawText(display_frame, 0U, 6U, state->alarm != 0U ? "ALARM ON" : "ALARM OFF");
    {
        OtaDownloadProgress progress;
        OtaDownload_GetProgress(&progress);
        if (progress.stage != OTA_PROGRESS_IDLE) {
            static const char * const stages[] = {"IDLE", "PREP", "ERASE", "DOWNLOAD", "VERIFY", "COMMIT", "FAILED"};
            (void)snprintf(line, sizeof(line), "OTA %s %lu%%", stages[progress.stage],
                (unsigned long)(progress.total ? progress.done * 100U / progress.total : 0U));
            Oled_DrawText(display_frame, 0U, 7U, line);
        } else Oled_DrawText(display_frame, 0U, 7U, OtaNotice_Get() != NULL ? "OTA WAIT DISARM" : Cloud_StatusText());
    }
}

void SystemClock_Config(void);

int main(void)
{
    MonitorState monitor;
    uint32_t now;
    uint32_t display_tick = 0U;
    uint32_t display_retry;
    uint8_t display_ok = 0U;
    HAL_Init();
    SystemClock_Config();
    BoardIo_Init();
    MX_USART1_UART_Init();
    MX_USART3_UART_Init();
    MX_I2C1_Init();
    MX_SPI2_Init();
    now = HAL_GetTick();
    Monitor_Init(&monitor, now, BoardIo_Key0Down(), BoardIo_Key1Down(), BoardIo_Motion());
    active_monitor = &monitor;
    Dht11_Init(now);
    Cloud_Init(now);
    display_retry = now - 2000U;
    while (1) {
        now = HAL_GetTick();
        Monitor_Update(&monitor, now, BoardIo_Key0Down(), BoardIo_Key1Down(), BoardIo_Motion());
        Cloud_Task(now, &monitor, Dht11_GetSample(), Dht11_GetStatus(), Dht11_IsBusy() == 0U);
        if (Dht11_IsBusy() == 0U) {
            /* New upgrades wait for local disarm; an active transfer owns the modem. */
            if (monitor.armed == 0U || OtaDownload_IsBusy() != 0U) OtaDownload_Task();
            if (OtaDownload_NeedsRecovery() && OtaDownload_ResetForRetry())
                Cloud_ResumeAfterOta(HAL_GetTick());
        }
        /* Network work may take time; start the DHT window using a fresh tick. */
        Dht11_Task(HAL_GetTick());
        now = HAL_GetTick();
        BoardIo_SetOutputs(now, monitor.motion, monitor.alarm);
        /* Keep OLED work out of the DHT11 start/receive window. */
        if (Dht11_IsBusy() != 0U) continue;
        if (display_ok == 0U && (uint32_t)(now - display_retry) >= 2000U) {
            display_retry = now;
            display_ok = Oled_Init() == HAL_OK;
            if (display_ok != 0U) display_tick = now - 250U;
        }
        if (display_ok != 0U) {
            if (Oled_IsFlushing() == 0U && (uint32_t)(now - display_tick) >= 250U) {
                display_tick = now;
                RenderStatus(&monitor, now);
                if (Oled_BeginFrame(display_frame) != HAL_OK) display_ok = 0U;
            }
            if (Oled_FlushStep() != HAL_OK) display_ok = 0U;
        }
    }
}
