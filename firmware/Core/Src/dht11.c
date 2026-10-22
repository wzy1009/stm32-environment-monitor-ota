#include "dht11.h"
#include "board_io.h"

static Dht11Sample latest;
static Dht11Status status;
static uint32_t last_read_at;
static uint32_t low_started_at;
static uint32_t cycles_per_us;
static uint8_t busy;

Dht11Status Dht11_DecodeFrame(const uint8_t frame[5], Dht11Sample *sample)
{
    int16_t temperature;
    uint16_t humidity;
    if ((uint8_t)(frame[0] + frame[1] + frame[2] + frame[3]) != frame[4]) {
        return DHT11_CHECKSUM;
    }
    if (frame[1] > 9U || (frame[3] & 0x7FU) > 9U || frame[2] > 80U) {
        return DHT11_DATA_ERROR;
    }
    humidity = (uint16_t)(frame[0] * 10U + frame[1]);
    if (humidity > 1000U) return DHT11_DATA_ERROR;
    temperature = (int16_t)(frame[2] * 10U + (frame[3] & 0x7FU));
    if ((frame[3] & 0x80U) != 0U) temperature = (int16_t)-temperature;
    sample->temperature_x10 = temperature;
    sample->humidity_x10 = humidity;
    sample->valid = 1U;
    return DHT11_OK;
}

static uint8_t WaitLevel(uint8_t level)
{
    uint32_t started = BoardIo_Cycles();
    while (BoardIo_DhtLevel() != level) {
        if ((uint32_t)(BoardIo_Cycles() - started) >= 150U * cycles_per_us) return 0U;
    }
    return 1U;
}

static Dht11Status ReadFrame(void)
{
    uint8_t frame[5] = {0};
    uint8_t bit;
    uint32_t high_started;
    uint32_t frame_started = BoardIo_Cycles();
    if (WaitLevel(0U) == 0U || WaitLevel(1U) == 0U || WaitLevel(0U) == 0U) {
        return DHT11_TIMEOUT;
    }
    for (bit = 0U; bit < 40U; ++bit) {
        if ((uint32_t)(BoardIo_Cycles() - frame_started) >= 8000U * cycles_per_us ||
            WaitLevel(1U) == 0U) return DHT11_TIMEOUT;
        high_started = BoardIo_Cycles();
        if (WaitLevel(0U) == 0U) return DHT11_TIMEOUT;
        frame[bit / 8U] <<= 1;
        if ((uint32_t)(BoardIo_Cycles() - high_started) >= 50U * cycles_per_us) {
            frame[bit / 8U] |= 1U;
        }
    }
    return Dht11_DecodeFrame(frame, &latest);
}

void Dht11_Init(uint32_t now)
{
    latest.temperature_x10 = 0;
    latest.humidity_x10 = 0U;
    latest.valid = 0U;
    status = DHT11_WAIT;
    busy = 0U;
    last_read_at = now;
    cycles_per_us = BoardIo_CyclesPerUs();
    BoardIo_DhtRelease();
}

void Dht11_Task(uint32_t now)
{
    uint32_t low_ms;
    if (busy == 0U) {
        if ((uint32_t)(now - last_read_at) < 2000U) return;
        low_started_at = now;
        BoardIo_DhtDriveLow();
        busy = 1U;
        return;
    }
    low_ms = (uint32_t)(now - low_started_at);
    if (low_ms < 20U) return;
    BoardIo_DhtRelease();
    /* Reject a delayed start rather than driving beyond the 18-30 ms window. */
    status = low_ms > 29U ? DHT11_TIMEOUT : ReadFrame();
    if (status != DHT11_OK) latest.valid = 0U;
    busy = 0U;
    last_read_at = now;
}

uint8_t Dht11_IsBusy(void) { return busy; }
const Dht11Sample *Dht11_GetSample(void) { return &latest; }
Dht11Status Dht11_GetStatus(void) { return status; }
