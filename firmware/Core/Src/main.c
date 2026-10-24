#include "main.h"
#include "oled.h"
#include "board_io.h"
#include "dht11.h"
#include "monitor_state.h"
#include "cloud_task.h"
#include "usart.h"
#include <stdio.h>
#include <string.h>

static uint8_t display_frame[1024];

static void RenderStatus(const MonitorState *state, uint32_t now)
{
    const Dht11Sample *sample = Dht11_GetSample();
    static const char * const dht_text[] = {"DHT WAIT", "DHT OK", "DHT TIMEOUT", "DHT CHECKSUM", "DHT DATA ERROR"};
    char line[32];
    int temperature;
    uint32_t elapsed;
    memset(display_frame, 0, sizeof(display_frame));
    Oled_DrawText(display_frame, 0U, 0U, "ENV MONITOR V0.3");
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
    if (state->ready == 0U) {
        elapsed = (uint32_t)(now - state->started_at);
        (void)snprintf(line, sizeof(line), "PIR WARMUP %luS",
                       (unsigned long)(elapsed >= MONITOR_PIR_WARMUP_MS ? 0U :
                       (MONITOR_PIR_WARMUP_MS - elapsed + 999U) / 1000U));
        Oled_DrawText(display_frame, 0U, 4U, line);
    } else {
        Oled_DrawText(display_frame, 0U, 4U, state->motion != 0U ? "PIR MOTION" : "PIR CLEAR");
    }
    Oled_DrawText(display_frame, 0U, 5U, state->armed == 0U ? "MODE DISARMED" :
                  Monitor_IsArming(state, now) != 0U ? "MODE ARMING" : "MODE ARMED");
    Oled_DrawText(display_frame, 0U, 6U, state->alarm != 0U ? "ALARM ON" : "ALARM OFF");
    Oled_DrawText(display_frame, 0U, 7U, Cloud_StatusText());
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
    MX_USART3_UART_Init();
    now = HAL_GetTick();
    Monitor_Init(&monitor, now, BoardIo_Key0Down(), BoardIo_Key1Down(), BoardIo_Motion());
    Dht11_Init(now);
    Cloud_Init(now);
    display_retry = now - 2000U;
    while (1) {
        now = HAL_GetTick();
        Monitor_Update(&monitor, now, BoardIo_Key0Down(), BoardIo_Key1Down(), BoardIo_Motion());
        Cloud_Task(now, &monitor, Dht11_GetSample(), Dht11_GetStatus(), Dht11_IsBusy() == 0U);
        /* Network work may take time; start the DHT window using a fresh tick. */
        Dht11_Task(HAL_GetTick());
        now = HAL_GetTick();
        BoardIo_SetOutputs(now, monitor.alarm, display_ok == 0U);
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

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSEPredivValue = RCC_HSE_PREDIV_DIV1;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLMUL = RCC_PLL_MUL9;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

void Error_Handler(void)
{
    __disable_irq();
    while (1) {}
}
