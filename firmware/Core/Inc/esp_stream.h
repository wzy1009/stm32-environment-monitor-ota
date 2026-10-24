#ifndef ESP_STREAM_H
#define ESP_STREAM_H
#include <stdint.h>

void EspStream_Init(void);
void EspStream_Feed(uint8_t byte);
uint8_t EspStream_IsIdle(void);
uint8_t EspStream_SetPassive(uint8_t enabled);
void EspStream_OnDataBegin(uint32_t length);

/* The application implements these; all run in the main loop. */
void EspStream_OnLine(const char *line);
void EspStream_OnPrompt(void);
void EspStream_OnData(uint8_t byte);
void EspStream_OnFault(void);
#endif
