/* Host substitute for the UART/AT boundary, never included in the Keil target. */
#ifndef ESP_AT_H
#define ESP_AT_H
#include <stdint.h>
typedef enum {HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT} HAL_StatusTypeDef;
typedef struct {
    uint32_t overflow, uart_errors, rearm_failures, last_uart_error;
} EspAt_Stats;
typedef enum {
    ESP_AT_RESPONSE_NONE, ESP_AT_RESPONSE_OK, ESP_AT_RESPONSE_ERROR,
    ESP_AT_RESPONSE_BAD_TEXT, ESP_AT_RESPONSE_SEND_OK, ESP_AT_RESPONSE_CLOSED
} EspAt_Response;
typedef void (*EspAt_DataHandler)(uint8_t byte);
HAL_StatusTypeDef EspAt_Start(void);
void EspAt_ResetTransport(void);
void EspAt_ResetResponse(void);
EspAt_Response EspAt_GetResponse(void);
HAL_StatusTypeDef EspAt_Send(const char *command);
HAL_StatusTypeDef EspAt_SendBytes(const uint8_t *data, uint16_t length);
void EspAt_SetDataHandler(EspAt_DataHandler handler);
void EspAt_Poll(void);
void EspAt_GetStats(EspAt_Stats *stats);
uint8_t EspAt_HasPrompt(void);
uint8_t EspAt_HasStreamFault(void);
#endif
