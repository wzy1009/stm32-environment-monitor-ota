#include "esp_stream.h"
#include <string.h>

static char line[160];
static uint16_t used;
static uint32_t remaining;
static uint8_t failed, prompt_space, passive;

void EspStream_Init(void)
{
    used = 0U; remaining = 0U; failed = prompt_space = passive = 0U;
}

uint8_t EspStream_IsIdle(void) { return !failed && !used && !remaining; }

uint8_t EspStream_SetPassive(uint8_t enabled)
{
    if (!EspStream_IsIdle()) { return 0U; }
    passive = enabled ? 1U : 0U;
    return 1U;
}

static void StreamFault(void) { failed = 1U; EspStream_OnFault(); }

static void Begin(uint16_t offset, uint32_t maximum)
{
    uint16_t i;
    uint32_t length = 0U;
    if (offset == used) { StreamFault(); return; }
    for (i = offset; i < used; ++i) {
        if (line[i] < '0' || line[i] > '9') { StreamFault(); return; }
        length = length * 10U + (uint32_t)(line[i] - '0');
        if (length > maximum) { StreamFault(); return; }
    }
    used = 0U; remaining = length;
    EspStream_OnDataBegin(length);
}

void EspStream_Feed(uint8_t byte)
{
    if (failed) { return; }
    if (remaining) { EspStream_OnData(byte); --remaining; return; }
    if (prompt_space) {
        prompt_space = 0U;
        if (byte == ' ') { return; }
    }
    if (byte == '>' && !used) { prompt_space = 1U; EspStream_OnPrompt(); return; }
    if (!passive && byte == ':' && used >= 5U && strncmp(line, "+IPD,", 5U) == 0) {
        Begin(5U, 65535U); return;
    }
    /* NONOS manuals show both delimiters; only the length-framed payload differs. */
    if (passive && used >= 13U &&
        ((byte == ':' && strncmp(line, "+CIPRECVDATA,", 13U) == 0) ||
         (byte == ',' && strncmp(line, "+CIPRECVDATA:", 13U) == 0))) {
        Begin(13U, 2048U); return;
    }
    if (byte == '\r') { return; }
    if (byte == '\n') {
        line[used] = '\0';
        if (used) { EspStream_OnLine(line); }
        used = 0U;
    } else if (!byte || used + 1U >= sizeof(line)) { StreamFault(); }
    else { line[used++] = (char)byte; }
}
