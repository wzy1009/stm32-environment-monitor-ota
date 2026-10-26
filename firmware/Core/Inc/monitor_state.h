#ifndef MONITOR_STATE_H
#define MONITOR_STATE_H

#include <stdint.h>

#define MONITOR_ARM_DELAY_MS 3000U
#define MONITOR_MOTION_FILTER_MS 100U
#define MONITOR_KEY_DEBOUNCE_MS 30U

typedef struct {
    uint32_t changed_at;
    uint8_t raw;
    uint8_t stable;
} MonitorKey;

typedef struct {
    uint32_t armed_at;
    uint32_t motion_changed_at;
    MonitorKey key0;
    MonitorKey key1;
    uint8_t armed;
    uint8_t alarm;
    uint8_t ready;
    uint8_t raw_motion;
    uint8_t motion;
} MonitorState;

void Monitor_Init(MonitorState *state, uint32_t now, uint8_t key0, uint8_t key1, uint8_t motion);
void Monitor_Update(MonitorState *state, uint32_t now, uint8_t key0, uint8_t key1, uint8_t motion);
void Monitor_SetArmed(MonitorState *state, uint32_t now, uint8_t armed);
uint8_t Monitor_IsArming(const MonitorState *state, uint32_t now);

#endif
