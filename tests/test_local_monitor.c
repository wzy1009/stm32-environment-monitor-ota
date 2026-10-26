#include "monitor_state.h"
#include "dht11.h"
#include "board_io.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t fake_cycles;
static uint32_t released_at;
static uint8_t driving_low;
static uint8_t wire_mode;
static uint8_t wire_frame[5];

/* Emulate DHT11 response edges; execute the production polling driver. */
void BoardIo_DhtDriveLow(void) { driving_low = 1U; }
void BoardIo_DhtRelease(void) { driving_low = 0U; released_at = fake_cycles; }
uint32_t BoardIo_CyclesPerUs(void) { return 72U; }
uint32_t BoardIo_Cycles(void) { fake_cycles += 72U; return fake_cycles; }
uint8_t BoardIo_DhtLevel(void)
{
    uint32_t us = (uint32_t)(fake_cycles - released_at) / 72U;
    uint32_t high;
    uint8_t bit;
    if (driving_low != 0U) return 0U;
    if (wire_mode != 0U) return wire_mode == 1U ? 1U : 0U;
    if (us < 30U) return 1U;
    if (us < 110U) return 0U;
    if (us < 190U) return 1U;
    us -= 190U;
    for (bit = 0U; bit < 40U; ++bit) {
        if (us < 50U) return 0U;
        us -= 50U;
        high = (wire_frame[bit / 8U] & (0x80U >> (bit % 8U))) != 0U ? 70U : 26U;
        if (us < high) return 1U;
        us -= high;
    }
    return us >= 50U;
}

static void SetWire(uint8_t humidity, uint8_t humidity_decimal, uint8_t temperature, uint8_t temperature_decimal)
{
    wire_frame[0] = humidity;
    wire_frame[1] = humidity_decimal;
    wire_frame[2] = temperature;
    wire_frame[3] = temperature_decimal;
    wire_frame[4] = (uint8_t)(humidity + humidity_decimal + temperature + temperature_decimal);
    wire_mode = 0U;
}

static void PollDht(uint32_t now)
{
    fake_cycles = now * 72000U;
    Dht11_Task(now);
}

static void TestFrameValidation(void)
{
    Dht11Sample sample = {0};
    SetWire(53U, 0U, 24U, 4U);
    assert(Dht11_DecodeFrame(wire_frame, &sample) == DHT11_OK);
    assert(sample.temperature_x10 == 244 && sample.humidity_x10 == 530U);
    SetWire(40U, 0U, 10U, 0x81U);
    assert(Dht11_DecodeFrame(wire_frame, &sample) == DHT11_OK);
    assert(sample.temperature_x10 == -101);
    wire_frame[4] ^= 1U;
    assert(Dht11_DecodeFrame(wire_frame, &sample) == DHT11_CHECKSUM);
    assert(sample.temperature_x10 == -101);
    SetWire(101U, 0U, 24U, 0U);
    assert(Dht11_DecodeFrame(wire_frame, &sample) == DHT11_DATA_ERROR);
    SetWire(50U, 0U, 24U, 10U);
    assert(Dht11_DecodeFrame(wire_frame, &sample) == DHT11_DATA_ERROR);
    puts("PASS 1: DHT frame validation, decimals, negative temperature, checksum");
}

static void TestDhtCaptureAndSchedule(void)
{
    SetWire(53U, 0U, 24U, 4U);
    Dht11_Init(0U);
    PollDht(1999U);
    assert(Dht11_IsBusy() == 0U && Dht11_GetSample()->valid == 0U);
    PollDht(2000U);
    PollDht(2019U);
    assert(Dht11_IsBusy() != 0U && driving_low != 0U);
    PollDht(2020U);
    assert(Dht11_GetStatus() == DHT11_OK && Dht11_IsBusy() == 0U);
    assert(Dht11_GetSample()->temperature_x10 == 244);
    PollDht(4019U);
    assert(Dht11_IsBusy() == 0U);
    PollDht(4020U);
    /* Receive across the 32-bit DWT cycle counter wrap. */
    fake_cycles = 0xFFFFFF00U;
    Dht11_Task(4040U);
    assert(Dht11_GetStatus() == DHT11_OK);
    puts("PASS 2: DHT wire capture, sample interval, start pulse, cycle wrap");
}

static void TestDhtFaultRecovery(void)
{
    uint32_t before;
    SetWire(53U, 0U, 24U, 4U);
    Dht11_Init(0U);
    PollDht(2000U);
    PollDht(2020U);
    assert(Dht11_GetSample()->valid != 0U);
    wire_frame[4] ^= 1U;
    PollDht(4020U);
    PollDht(4040U);
    assert(Dht11_GetStatus() == DHT11_CHECKSUM && Dht11_GetSample()->valid == 0U);
    wire_mode = 1U;
    PollDht(6040U);
    fake_cycles = 0U;
    Dht11_Task(6060U);
    assert(Dht11_GetStatus() == DHT11_TIMEOUT && fake_cycles < 72000U);
    wire_mode = 2U;
    PollDht(8060U);
    fake_cycles = 0U;
    Dht11_Task(8080U);
    assert(Dht11_GetStatus() == DHT11_TIMEOUT && fake_cycles < 72000U);
    SetWire(48U, 0U, 25U, 0U);
    PollDht(10080U);
    PollDht(10100U);
    assert(Dht11_GetStatus() == DHT11_OK && Dht11_GetSample()->valid != 0U);
    PollDht(12100U);
    before = fake_cycles;
    Dht11_Task(12131U);
    assert(Dht11_GetStatus() == DHT11_TIMEOUT && driving_low == 0U && fake_cycles == before);
    puts("PASS 3: DHT stuck lines, invalidation, recovery, delayed-start rejection");
}

static void PressToggle(MonitorState *s, uint32_t now, uint8_t motion)
{
    Monitor_Update(s, now, 1U, 0U, motion);
    Monitor_Update(s, now + 30U, 1U, 0U, motion);
    Monitor_Update(s, now + 31U, 0U, 0U, motion);
    Monitor_Update(s, now + 61U, 0U, 0U, motion);
}

static void TestStartupReady(void)
{
    MonitorState s;
    Monitor_Init(&s, 0U, 0U, 0U, 1U);
    Monitor_Update(&s, 100U, 0U, 0U, 1U);
    assert(s.armed == 0U && s.alarm == 0U && s.motion != 0U);
    PressToggle(&s, 1000U, 1U);
    Monitor_Update(&s, 4029U, 0U, 0U, 1U);
    assert(s.ready != 0U && s.alarm == 0U);
    Monitor_Update(&s, 4030U, 0U, 0U, 1U);
    assert(s.ready != 0U && s.alarm != 0U);
    puts("PASS 4: ready at startup, default disarm and 3-second arming delay");
}

static void TestButtonDebounceAndBootHold(void)
{
    MonitorState s;
    Monitor_Init(&s, 0U, 1U, 0U, 0U);
    Monitor_Update(&s, 100U, 1U, 0U, 0U);
    assert(s.armed == 0U);
    Monitor_Update(&s, 101U, 0U, 0U, 0U);
    Monitor_Update(&s, 131U, 0U, 0U, 0U);
    Monitor_Update(&s, 200U, 1U, 0U, 0U);
    Monitor_Update(&s, 210U, 0U, 0U, 0U);
    Monitor_Update(&s, 220U, 1U, 0U, 0U);
    Monitor_Update(&s, 249U, 1U, 0U, 0U);
    assert(s.armed == 0U);
    Monitor_Update(&s, 250U, 1U, 0U, 0U);
    Monitor_Update(&s, 500U, 1U, 0U, 0U);
    assert(s.armed != 0U && s.armed_at == 250U);
    puts("PASS 5: button bounce, hold, and reset-held button");
}

static void TestArmingFilterAndLatch(void)
{
    MonitorState s;
    Monitor_Init(&s, 0U, 0U, 0U, 0U);
    PressToggle(&s, 60000U, 0U);
    Monitor_Update(&s, 61000U, 0U, 0U, 1U);
    Monitor_Update(&s, 61099U, 0U, 0U, 1U);
    assert(s.motion == 0U && s.alarm == 0U);
    Monitor_Update(&s, 61100U, 0U, 0U, 1U);
    Monitor_Update(&s, 63029U, 0U, 0U, 1U);
    assert(s.motion != 0U && s.alarm == 0U);
    Monitor_Update(&s, 63030U, 0U, 0U, 1U);
    assert(s.alarm != 0U);
    Monitor_Update(&s, 64000U, 0U, 0U, 0U);
    Monitor_Update(&s, 64100U, 0U, 0U, 0U);
    assert(s.motion == 0U && s.alarm != 0U);
    PressToggle(&s, 65000U, 0U);
    assert(s.armed == 0U && s.alarm == 0U);
    /* A short high pulse must not become a motion event. */
    PressToggle(&s, 66000U, 0U);
    Monitor_Update(&s, 70000U, 0U, 0U, 1U);
    Monitor_Update(&s, 70099U, 0U, 0U, 0U);
    Monitor_Update(&s, 70200U, 0U, 0U, 0U);
    assert(s.motion == 0U && s.alarm == 0U);
    puts("PASS 6: arm delay, motion filter, alarm latch, toggle disarm");
}

static void TestDisarmPriority(void)
{
    MonitorState s;
    Monitor_Init(&s, 0U, 0U, 0U, 0U);
    PressToggle(&s, 60000U, 0U);
    Monitor_Update(&s, 64000U, 0U, 0U, 1U);
    Monitor_Update(&s, 64100U, 0U, 0U, 1U);
    assert(s.alarm != 0U);
    Monitor_Update(&s, 65000U, 1U, 1U, 1U);
    Monitor_Update(&s, 65030U, 1U, 1U, 1U);
    assert(s.armed == 0U && s.alarm == 0U);
    Monitor_Update(&s, 65100U, 0U, 1U, 1U);
    Monitor_Update(&s, 65130U, 0U, 1U, 1U);
    Monitor_Update(&s, 65200U, 1U, 1U, 1U);
    Monitor_Update(&s, 65230U, 1U, 1U, 1U);
    assert(s.armed == 0U);
    puts("PASS 7: dedicated disarm wins over simultaneous or held keys");
}

static void TestTickWrap(void)
{
    MonitorState s;
    uint32_t start = 0xFFFF8000U;
    Monitor_Init(&s, start, 0U, 0U, 0U);
    PressToggle(&s, start + 59000U, 0U);
    Monitor_Update(&s, start + 60000U, 0U, 0U, 1U);
    Monitor_Update(&s, start + 60100U, 0U, 0U, 1U);
    assert(s.ready != 0U && s.alarm == 0U);
    Monitor_Update(&s, start + 62030U, 0U, 0U, 1U);
    assert(s.alarm != 0U);
    /* Arming itself can also straddle the tick counter wrap. */
    Monitor_Init(&s, 0U, 0U, 0U, 0U);
    PressToggle(&s, 0xFFFFFF00U, 0U);
    Monitor_Update(&s, 0xFFFFFF00U + 3029U, 0U, 0U, 1U);
    assert(Monitor_IsArming(&s, 0xFFFFFF00U + 3029U) != 0U);
    Monitor_Update(&s, 0xFFFFFF00U + 3129U, 0U, 0U, 1U);
    assert(s.alarm != 0U);
    puts("PASS 8: arm delay and filters across tick wrap");
}

int main(void)
{
    TestFrameValidation();
    TestDhtCaptureAndSchedule();
    TestDhtFaultRecovery();
    TestStartupReady();
    TestButtonDebounceAndBootHold();
    TestArmingFilterAndLatch();
    TestDisarmPriority();
    TestTickWrap();
    return 0;
}
