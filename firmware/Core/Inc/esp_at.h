#ifndef ESP_AT_H
#define ESP_AT_H

#include "main.h"

typedef struct {
    uint32_t overflow;
    uint32_t uart_errors;
    uint32_t rearm_failures;
    uint32_t last_uart_error;
} EspAt_Stats;

HAL_StatusTypeDef EspAt_Start(void);
/* Recovery owner only: discard old protocol bytes, retain cumulative RX counters. */
void EspAt_ResetTransport(void);
HAL_StatusTypeDef EspAt_Send(const char *command);
void EspAt_Poll(void);
void EspAt_GetStats(EspAt_Stats *stats);

typedef enum {
    ESP_AT_RESPONSE_NONE = 0,
    ESP_AT_RESPONSE_OK,
    ESP_AT_RESPONSE_ERROR,
    ESP_AT_RESPONSE_BAD_TEXT,
    ESP_AT_RESPONSE_SEND_OK,
    ESP_AT_RESPONSE_CLOSED
} EspAt_Response;

void EspAt_ResetResponse(void);
EspAt_Response EspAt_GetResponse(void);
const char *EspAt_GetIP(void);

typedef void (*EspAt_DataHandler)(uint8_t byte);
void EspAt_SetDataHandler(EspAt_DataHandler handler);
typedef void (*EspAt_FrameHandler)(uint32_t length);
typedef uint8_t (*EspAt_LineHandler)(const char *line);
void EspAt_SetFrameHandler(EspAt_FrameHandler handler);
void EspAt_SetLineHandler(EspAt_LineHandler handler);
uint8_t EspAt_StreamIdle(void);
HAL_StatusTypeDef EspAt_SetPassive(uint8_t enabled);
HAL_StatusTypeDef EspAt_SendBytes(const uint8_t *data, uint16_t length);
uint8_t EspAt_HasPrompt(void);
uint8_t EspAt_HasStreamFault(void);

#endif
