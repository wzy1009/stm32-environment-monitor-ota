#include "esp_at.h"
#include "esp_stream.h"
#include "usart.h"
#include <string.h>
#include <stdio.h>

#define RX_CAPACITY 2048U

static uint8_t rx_byte;
static volatile uint8_t rx_queue[RX_CAPACITY];
static volatile uint16_t rx_write;
static volatile uint16_t rx_read;
static volatile uint8_t rx_recover;
static volatile uint32_t overflow_count;
static volatile uint32_t uart_error_count;
static volatile uint32_t rearm_failure_count;
static volatile uint32_t last_uart_error;

static uint8_t send_prompt;
static uint8_t stream_fault;
static EspAt_DataHandler data_handler;
static EspAt_FrameHandler frame_handler;
static EspAt_LineHandler line_handler;

static char response_line[160];
static uint16_t response_length;
static EspAt_Response response;
static uint8_t wifi_join_active;
static char station_ip[16];

void EspAt_ResetResponse(void)
{
    response_length = 0U;
    send_prompt = 0U;
    response = ESP_AT_RESPONSE_NONE;
    station_ip[0] = '\0';
}

EspAt_Response EspAt_GetResponse(void)
{
    return response;
}

const char *EspAt_GetIP(void)
{
    return station_ip;
}

static void ParseIP(const char *line)
{
    static const char current_prefix[] = "+CIPSTA_CUR:ip:\"";
    static const char old_prefix[] = "+CIPSTA_CUR:\"";
    const char *value;
    const char *end;
    char candidate[16];
    char extra;
    unsigned int a, b, c, d;
    size_t length;

    /* Accept labeled and older plain NONOS response formats. */
    if (strncmp(line, current_prefix, sizeof(current_prefix) - 1U) == 0) {
        value = line + sizeof(current_prefix) - 1U;
    } else if (strncmp(line, old_prefix, sizeof(old_prefix) - 1U) == 0) {
        value = line + sizeof(old_prefix) - 1U;
    } else {
        return;
    }
    end = strchr(value, '"');
    if (end == NULL || end[1] != '\0') {
        return;
    }
    length = (size_t)(end - value);
    if (length == 0U || length >= sizeof(candidate)) {
        return;
    }
    memcpy(candidate, value, length);
    candidate[length] = '\0';
    if (sscanf(candidate, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 ||
        a == 0U || a == 127U || a >= 224U ||
        b > 255U || c > 255U || d > 255U ||
        (a == 169U && b == 254U)) {
        return;
    }
    memcpy(station_ip, candidate, length + 1U);
}

static void ParseResponseByte(uint8_t byte)
{
    if (response == ESP_AT_RESPONSE_BAD_TEXT) {
        return;
    }
    if (byte == '\r') {
        return;
    }
    if (byte == '\n') {
        response_line[response_length] = '\0';
        if (strcmp(response_line, "OK") == 0 &&
            response == ESP_AT_RESPONSE_NONE) {
            response = ESP_AT_RESPONSE_OK;
        } else if (strcmp(response_line, "ERROR") == 0 ||
                   strcmp(response_line, "FAIL") == 0 ||
                   strcmp(response_line, "ready") == 0 ||
                   strncmp(response_line, "busy", 4U) == 0) {
            response = ESP_AT_RESPONSE_ERROR;
        }
        ParseIP(response_line);
        response_length = 0U;
    } else if (byte == 0U || response_length >= sizeof(response_line) - 1U) {
        response = ESP_AT_RESPONSE_BAD_TEXT;
    } else {
        response_line[response_length++] = (char)byte;
    }
}


void EspAt_SetDataHandler(EspAt_DataHandler handler)
{
    data_handler = handler;
}

HAL_StatusTypeDef EspAt_SendBytes(const uint8_t *data, uint16_t length)
{
    if (data == NULL || length == 0U) {
        return HAL_ERROR;
    }
    return HAL_UART_Transmit(&huart3, data, length, 50U);
}

uint8_t EspAt_HasPrompt(void)
{
    return send_prompt;
}

uint8_t EspAt_HasStreamFault(void)
{
    return stream_fault;
}

void EspStream_OnPrompt(void)
{
    send_prompt = 1U;
}

void EspStream_OnFault(void)
{
    stream_fault = 1U;
    response = ESP_AT_RESPONSE_BAD_TEXT;
}

void EspStream_OnData(uint8_t byte)
{
    if (data_handler != NULL) {
        data_handler(byte);
    } else {
        EspStream_OnFault();
    }
}

void EspStream_OnLine(const char *text)
{
    const char *p = text;
    if (line_handler != NULL && line_handler(text)) { return; }
    while (*p != '\0') {
        ParseResponseByte((uint8_t)*p++);
    }
    ParseResponseByte('\n');
    if (strcmp(text, "SEND OK") == 0) {
        response = ESP_AT_RESPONSE_SEND_OK;
    } else if (strcmp(text, "SEND FAIL") == 0 ||
               (data_handler != NULL && strcmp(text, "WIFI DISCONNECT") == 0)) {
        response = ESP_AT_RESPONSE_ERROR;
    } else if (strcmp(text, "CLOSED") == 0 &&
               !(wifi_join_active != 0U && data_handler == NULL)) {
        response = ESP_AT_RESPONSE_CLOSED;
    }
}

void EspAt_SetFrameHandler(EspAt_FrameHandler handler) { frame_handler = handler; }
void EspAt_SetLineHandler(EspAt_LineHandler handler) { line_handler = handler; }
uint8_t EspAt_StreamIdle(void) { return EspStream_IsIdle(); }
HAL_StatusTypeDef EspAt_SetPassive(uint8_t enabled)
{ return EspStream_SetPassive(enabled) ? HAL_OK : HAL_ERROR; }
void EspStream_OnDataBegin(uint32_t length)
{ if (frame_handler != NULL) { frame_handler(length); } }

static HAL_StatusTypeDef ArmReceive(void)
{
    HAL_StatusTypeDef status = HAL_UART_Receive_IT(&huart3, &rx_byte, 1U);
    if (status != HAL_OK) {
        ++rearm_failure_count;
        rx_recover = 1U;
    }
    return status;
}

void EspAt_ResetTransport(void)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    rx_read = rx_write;
    __set_PRIMASK(mask);
    EspStream_Init();
    stream_fault = 0U; wifi_join_active = 0U;
    data_handler = NULL; frame_handler = NULL; line_handler = NULL;
    EspAt_ResetResponse();
}

HAL_StatusTypeDef EspAt_Start(void)
{
    /* Call once after MX_USART3_UART_Init(), before sending commands. */
    EspStream_Init();
    __HAL_UART_CLEAR_OREFLAG(&huart3);
    return ArmReceive();
}

HAL_StatusTypeDef EspAt_Send(const char *command)
{
    size_t length;
    if (command == NULL) {
        return HAL_ERROR;
    }
    length = strlen(command);
    if (length == 0U || length > 128U) {
        return HAL_ERROR;
    }
    /* Only a Wi-Fi JOIN without a socket consumer may close an old connection. */
    wifi_join_active = (uint8_t)(strncmp(command, "AT+CWJAP_CUR=",
                              sizeof("AT+CWJAP_CUR=") - 1U) == 0);
    return HAL_UART_Transmit(&huart3, (const uint8_t *)command,
                             (uint16_t)length, 50U);
}

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    uint16_t next;
    if (huart != &huart3) {
        return;
    }
    /* HAL reports RX completion before errors; rearming here would clear ErrorCode. */
    if (huart->ErrorCode != HAL_UART_ERROR_NONE) {
        return;
    }
    /* 中断生产数据，主循环消费数据；预留一个位置区分队满和队空。 */
    next = (uint16_t)((rx_write + 1U) % RX_CAPACITY);
    if (next == rx_read) {
        ++overflow_count;
    } else {
        rx_queue[rx_write] = rx_byte;
        rx_write = next;
    }
    (void)ArmReceive();
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart != &huart3) {
        return;
    }
    ++uart_error_count;
    last_uart_error = huart->ErrorCode;
    /* Stop repeated error IRQs; recovery is done in the main loop. */
    __HAL_UART_DISABLE_IT(huart, UART_IT_RXNE);
    __HAL_UART_DISABLE_IT(huart, UART_IT_PE);
    __HAL_UART_DISABLE_IT(huart, UART_IT_ERR);
    rx_recover = 1U;
}

void EspAt_GetStats(EspAt_Stats *stats)
{
    uint32_t mask = __get_PRIMASK();
    __disable_irq();
    stats->overflow = overflow_count;
    stats->uart_errors = uart_error_count;
    stats->rearm_failures = rearm_failure_count;
    stats->last_uart_error = last_uart_error;
    __set_PRIMASK(mask);
}

void EspAt_Poll(void)
{
    uint8_t text[64];
    uint16_t length = 0U;
    uint32_t mask = __get_PRIMASK();
    HAL_StatusTypeDef status;

    __disable_irq();
    if (rx_recover != 0U) {
        rx_recover = 0U;
        /* This recovery path assumes no RX DMA. */
        status = HAL_UART_AbortReceive(&huart3);
        if (status == HAL_OK) {
            __HAL_UART_CLEAR_OREFLAG(&huart3);
            (void)ArmReceive();
        } else {
            ++rearm_failure_count;
            rx_recover = 1U;
        }
    }
    while (length < sizeof(text) && rx_read != rx_write) {
        text[length++] = rx_queue[rx_read];
        rx_read = (uint16_t)((rx_read + 1U) % RX_CAPACITY);
    }
    __set_PRIMASK(mask);

    /* Parse outside the interrupt; keep binary payload out of the AT line parser. */
    if (length != 0U) {
        uint16_t i;
        for (i = 0U; i < length; ++i) {
            EspStream_Feed(text[i]);
        }
    }
}
