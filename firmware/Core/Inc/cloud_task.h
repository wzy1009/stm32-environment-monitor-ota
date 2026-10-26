#ifndef CLOUD_TASK_H
#define CLOUD_TASK_H
#include "monitor_state.h"
#include "dht11.h"
void Cloud_Init(uint32_t now);
void Cloud_Task(uint32_t now, MonitorState *state, const Dht11Sample *sample,
                Dht11Status status, uint8_t can_send);
uint8_t Cloud_IsOnline(void);
uint8_t Cloud_PauseForOta(void);
void Cloud_ResumeAfterOta(uint32_t now);
const char *Cloud_StatusText(void);
#endif
