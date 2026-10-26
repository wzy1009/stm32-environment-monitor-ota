#include "monitor_state.h"
#include <string.h>

static uint8_t KeyPressed(MonitorKey *key, uint32_t now, uint8_t down)
{
    if (down != key->raw) {
        key->raw = down;
        key->changed_at = now;
    }
    if (key->stable != key->raw &&
        (uint32_t)(now - key->changed_at) >= MONITOR_KEY_DEBOUNCE_MS) {
        key->stable = key->raw;
        return key->stable;
    }
    return 0U;
}

void Monitor_Init(MonitorState *state, uint32_t now, uint8_t key0, uint8_t key1, uint8_t motion)
{
    memset(state, 0, sizeof(*state));
    state->ready = 1U;
    state->motion_changed_at = now;
    state->raw_motion = motion;
    /* A key held during reset must be released before it can generate an event. */
    state->key0.raw = state->key0.stable = key0;
    state->key1.raw = state->key1.stable = key1;
    state->key0.changed_at = state->key1.changed_at = now;
}

uint8_t Monitor_IsArming(const MonitorState *state, uint32_t now)
{
    return state->armed != 0U && (uint32_t)(now - state->armed_at) < MONITOR_ARM_DELAY_MS;
}

void Monitor_Update(MonitorState *state, uint32_t now, uint8_t key0, uint8_t key1, uint8_t motion)
{
    uint8_t toggle = KeyPressed(&state->key0, now, key0);
    uint8_t disarm = KeyPressed(&state->key1, now, key1);
    if (motion != state->raw_motion) {
        state->raw_motion = motion;
        state->motion_changed_at = now;
    }
    if ((uint32_t)(now - state->motion_changed_at) >= MONITOR_MOTION_FILTER_MS) {
        state->motion = state->raw_motion;
    }
    /* Keep KEY1 authoritative even when both buttons are pressed together. */
    if (disarm != 0U || state->key1.stable != 0U || key1 != 0U) {
        state->armed = 0U;
        state->alarm = 0U;
    } else if (toggle != 0U) {
        state->armed = state->armed == 0U ? 1U : 0U;
        state->armed_at = now;
        state->alarm = 0U;
    }
    if (state->armed != 0U && state->ready != 0U &&
        Monitor_IsArming(state, now) == 0U && state->motion != 0U) {
        state->alarm = 1U;
    }
}

void Monitor_SetArmed(MonitorState *state, uint32_t now, uint8_t armed)
{
    /* Local emergency disarm remains authoritative over a remote ARM. */
    if (state->key1.raw != 0U || state->key1.stable != 0U) armed = 0U;
    armed = armed != 0U;
    if (state->armed != armed) {
        state->armed = armed;
        state->armed_at = now;
    }
    if (armed == 0U) state->alarm = 0U;
}
