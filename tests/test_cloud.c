#include "cloud_task.h"
#include "cloud_mqtt.h"
#include "esp_at.h"
#include "esp_stream.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static MqttParser parser;
static unsigned at_lines, prompts, stream_faults, data_count;
static uint8_t data_bytes[64];
void EspStream_OnLine(const char *line) { assert(strcmp(line, "OK") == 0); ++at_lines; }
void EspStream_OnPrompt(void) { ++prompts; }
void EspStream_OnFault(void) { ++stream_faults; }
void EspStream_OnDataBegin(uint32_t n) { assert(n <= 65535U); }
void EspStream_OnData(uint8_t b) { assert(data_count < sizeof(data_bytes)); data_bytes[data_count++] = b; }

static MqttEvent Feed(const uint8_t *bytes, uint16_t n)
{
    MqttEvent result = MQTT_NONE;
    uint16_t i;
    for (i = 0U; i < n; ++i) {
        MqttEvent event = Mqtt_Feed(&parser, bytes[i]);
        if (event != MQTT_NONE) result = event;
    }
    return result;
}

static void TestProtocolAndFraming(void)
{
    uint8_t b[384], small[8], guard[8];
    uint16_t n;
    unsigned i;
    static const uint8_t conn[] = {0x20U, 2U, 0U, 0U};
    static const uint8_t sub[] = {0x90U, 3U, 0U, 1U, 0U};
    static const uint8_t denied[] = {0x90U, 3U, 0U, 1U, 0x80U};
    static const uint8_t overflow[] = {0x30U, 0x81U, 3U};
    static const uint8_t bad_length[] = {0x30U, 0x80U, 0U};
    static const uint8_t wire[] = "\r\nOK\r\n> +IPD,7:OK\r\n\0>x\r\nOK\r\n";
    Mqtt_Init(&parser, "envCmd");
    assert(Feed(conn, sizeof(conn)) == MQTT_CONNACK);
    assert(Feed(sub, sizeof(sub)) == MQTT_SUBACK);
    n = Mqtt_Publish(b, sizeof(b), "envCmd", "ARM");
    assert(n != 0U && Feed(b, n) == MQTT_ARM);
    assert(Mqtt_IsPartial(&parser) == 0U);
    b[0] |= 1U;
    assert(Feed(b, n) == MQTT_NONE); /* retained */
    n = Mqtt_Publish(b, sizeof(b), "envCmdOther", "ARM");
    assert(Feed(b, n) == MQTT_NONE);
    n = Mqtt_Publish(b, sizeof(b), "envCmd", "DISARM");
    assert(Feed(b, n) == MQTT_DISARM);
    n = Mqtt_Publish(b, sizeof(b), "envCmd", "STATUS");
    assert(Feed(b, n) == MQTT_STATUS);
    n = Mqtt_Publish(b, sizeof(b), "envCmd", "ARM\n");
    assert(Feed(b, n) == MQTT_NONE);
    b[n - 1U] = 0U;
    assert(Feed(b, n) == MQTT_NONE);
    b[0] = 0x32U;
    assert(Feed(b, n) == MQTT_FAULT); /* QoS exceeds subscription */
    Mqtt_Init(&parser, "envCmd");
    assert(Feed(denied, sizeof(denied)) == MQTT_FAULT);
    Mqtt_Init(&parser, "envCmd");
    assert(Feed(overflow, sizeof(overflow)) == MQTT_FAULT);
    assert(Mqtt_Feed(&parser, 0x20U) == MQTT_FAULT);
    Mqtt_Init(&parser, "envCmd");
    assert(Feed(bad_length, sizeof(bad_length)) == MQTT_FAULT);
    Mqtt_Init(&parser, "envCmd");
    assert(Mqtt_Feed(&parser, 0x30U) == MQTT_NONE && Mqtt_IsPartial(&parser));
    memset(small, 0xA5, sizeof(small)); memcpy(guard, small, sizeof(small));
    assert(Mqtt_Connect(small, sizeof(small), "test") == 0U);
    assert(Mqtt_Subscribe(small, sizeof(small), "envCmd") == 0U);
    assert(Mqtt_Publish(small, sizeof(small), "envState", "payload") == 0U);
    assert(memcmp(small, guard, sizeof(small)) == 0);
    assert(Mqtt_TopicValid("envCmd") && !Mqtt_TopicValid("env/#") && !Mqtt_TopicValid(""));
    n = Mqtt_Connect(b, sizeof(b), "00000000000000000000000000000000");
    assert(n == 46U && b[0] == 0x10U && b[1] == 44U && b[10] == 0U && b[11] == 60U);
    n = Mqtt_Subscribe(b, sizeof(b), "envCmd");
    assert(n == 13U && b[0] == 0x82U && b[2] == 0U && b[3] == 1U && b[n - 1U] == 0U);
    memset(b, 'a', 200U); b[200] = 0U;
    { char payload[201]; memcpy(payload, b, 201U);
      n = Mqtt_Publish(b, sizeof(b), "envCmd", payload);
      Mqtt_Init(&parser, "envCmd");
      assert(n == 211U && (b[1] & 128U) != 0U && Feed(b, n) == MQTT_NONE); }
    EspStream_Init();
    for (i = 0U; i < sizeof(wire) - 1U; ++i) EspStream_Feed(wire[i]);
    assert(at_lines == 2U && prompts == 1U && stream_faults == 0U && data_count == 7U);
    assert(memcmp(data_bytes, "OK\r\n\0>x", 7U) == 0 && EspStream_IsIdle());
    { const char *bad = "+IPD,65536:";
      while (*bad) EspStream_Feed((uint8_t)*bad++); }
    assert(stream_faults == 1U);
    puts("PASS 9: MQTT framing, command boundaries, rejection, buffers and binary IPD");
}

static EspAt_Response response;
static EspAt_Stats stats;
static EspAt_DataHandler receiver;
static uint8_t queue[4096];
static uint16_t queued;
static unsigned sends, publishes, connects, subscriptions, pings, resets;
static uint8_t prompt, allow_prompt = 1U, allow_send_ok = 1U, allow_pong = 1U;
static char last_json[384], last_command[128];
static MonitorState monitor;
static Dht11Sample sample = {254, 602U, 1U};
static uint32_t now;
static uint8_t key1;
char test_ssid[40] = "test_wifi", test_password[80] = "test_password";
char test_uid[40] = "00000000000000000000000000000000";
char test_state_topic[48] = "envState", test_command_topic[48] = "envCmd";

static void Inject(const uint8_t *b, uint16_t n)
{
    assert(queued + n <= sizeof(queue));
    memcpy(queue + queued, b, n); queued += n;
}
static void Command(const char *topic, const char *value, uint8_t retained)
{
    uint8_t b[384];
    uint16_t n = Mqtt_Publish(b, sizeof(b), topic, value);
    assert(n != 0U); b[0] |= retained; Inject(b, n);
}
HAL_StatusTypeDef EspAt_Start(void) { return HAL_OK; }
void EspAt_ResetTransport(void)
{
    queued = 0U; receiver = NULL; prompt = 0U; response = ESP_AT_RESPONSE_NONE;
}
void EspAt_ResetResponse(void) { response = ESP_AT_RESPONSE_NONE; prompt = 0U; }
EspAt_Response EspAt_GetResponse(void) { return response; }
void EspAt_SetDataHandler(EspAt_DataHandler handler) { receiver = handler; }
uint8_t EspAt_HasPrompt(void) { return prompt; }
uint8_t EspAt_HasStreamFault(void) { return 0U; }
void EspAt_GetStats(EspAt_Stats *out) { *out = stats; }
HAL_StatusTypeDef EspAt_Send(const char *command)
{
    ++sends;
    assert(strlen(command) < sizeof(last_command)); strcpy(last_command, command);
    if (strncmp(command, "AT+CIPSEND=", 11U) == 0) { prompt = allow_prompt; }
    else { response = ESP_AT_RESPONSE_OK; if (strcmp(command, "AT+RST\r\n") == 0) ++resets; }
    return HAL_OK;
}
HAL_StatusTypeDef EspAt_SendBytes(const uint8_t *b, uint16_t n)
{
    static const uint8_t conn[] = {0x20U, 2U, 0U, 0U};
    static const uint8_t sub[] = {0x90U, 3U, 0U, 1U, 0U};
    static const uint8_t pong[] = {0xD0U, 0U};
    assert(n > 1U);
    ++sends;
    response = allow_send_ok ? ESP_AT_RESPONSE_SEND_OK : ESP_AT_RESPONSE_NONE;
    if (b[0] == 0x10U) {
        assert(!Cloud_IsOnline()); ++connects;
        Command("envCmd", "ARM", 0U); /* commands before subscription are ignored */
        Inject(conn, sizeof(conn));
    } else if (b[0] == 0x82U) {
        assert(!Cloud_IsOnline()); ++subscriptions; Inject(sub, sizeof(sub));
    } else if (b[0] == 0x30U) {
        uint16_t prefix = (b[1] & 128U) ? 3U : 2U;
        uint16_t topic = (uint16_t)((uint16_t)b[prefix] * 256U + b[prefix + 1U]);
        uint16_t start = prefix + topic + 2U;
        assert(topic == 11U && memcmp(b + prefix + 2U, "envState/up", 11U) == 0);
        assert(start <= n && n - start < sizeof(last_json));
        memcpy(last_json, b + start, n - start); last_json[n - start] = '\0'; ++publishes;
    } else { assert(b[0] == 0xC0U && n == 2U); ++pings; if (allow_pong) Inject(pong, sizeof(pong)); }
    return HAL_OK;
}
void EspAt_Poll(void)
{
    uint16_t n = queued < 64U ? queued : 64U, i;
    if (receiver == NULL) return;
    for (i = 0U; i < n; ++i) receiver(queue[i]);
    memmove(queue, queue + n, queued - n); queued -= n;
}
static void Tick(uint8_t can_send)
{
    Monitor_Update(&monitor, now, 0U, key1, monitor.raw_motion);
    Cloud_Task(now, &monitor, &sample, sample.valid ? DHT11_OK : DHT11_TIMEOUT, can_send);
    now += 10U;
}
static void Settle(void) { unsigned i; for (i = 0U; i < 20U; ++i) Tick(1U); }
static void Online(void)
{
    unsigned i;
    for (i = 0U; i < 4000U && !Cloud_IsOnline(); ++i) Tick(1U);
    assert(Cloud_IsOnline()); Settle();
}

static void TestCloudLifecycle(void)
{
    unsigned before, reset_count;
    uint32_t armed_at;
    now = 0xFFFFFF00U;
    Monitor_Init(&monitor, now, 0U, 0U, 0U);
    test_ssid[0] = '\0'; Cloud_Init(now); Tick(1U);
    assert(sends == 0U && strcmp(Cloud_StatusText(), "NET CONFIG") == 0);
    strcpy(test_ssid, "test_wifi"); strcpy(test_password, "x\"\r\nAT"); Cloud_Init(now); Tick(1U);
    assert(sends == 0U);
    strcpy(test_password, "test_password"); test_uid[0] = 'z'; Cloud_Init(now); Tick(1U);
    assert(sends == 0U); test_uid[0] = '0';
    strcpy(test_command_topic, "env/#"); Cloud_Init(now); Tick(1U); assert(sends == 0U);
    strcpy(test_command_topic, "envState"); Cloud_Init(now); Tick(1U); assert(sends == 0U);
    strcpy(test_command_topic, "envCmd");
    Cloud_Init(now); Online();
    assert(connects == 1U && subscriptions == 1U && monitor.armed == 0U && publishes == 1U);
    assert(strstr(last_json, "\"temp_x10\":254") && strstr(last_json, "\"humi_x10\":602"));
    Command("envCmd", "ARM", 1U); Command("wrong", "ARM", 0U); Settle();
    assert(monitor.armed == 0U);
    before = sends;
    Command("envCmd", "ARM", 0U); Tick(0U);
    assert(monitor.armed == 1U && sends == before);
    armed_at = monitor.armed_at;
    Command("envCmd", "ARM", 0U); Tick(0U);
    assert(monitor.armed_at == armed_at);
    monitor.started_at = now - 60000U;
    monitor.armed_at = now - 3000U;
    monitor.raw_motion = monitor.motion = 1U;
    monitor.motion_changed_at = now - 100U;
    Tick(0U); assert(monitor.alarm == 1U);
    Command("envCmd", "ARM", 0U); Tick(0U); assert(monitor.alarm == 1U);
    now += 1200U; Settle();
    assert(strstr(last_json, "\"alarm\":1") && strstr(last_json, "\"alarm_count\":1"));
    Command("envCmd", "DISARM", 0U); Command("envCmd", "ARM", 0U); Tick(0U);
    assert(monitor.armed == 0U && monitor.alarm == 0U);
    key1 = 1U; Command("envCmd", "ARM", 0U); Tick(0U); assert(!monitor.armed);
    key1 = 0U; Settle();
    sample.valid = 0U; now += 1200U; Settle();
    assert(strstr(last_json, "\"temp_x10\":null") && strstr(last_json, "\"valid\":0"));
    before = publishes; Command("envCmd", "STATUS", 0U); now += 1200U; Settle();
    assert(publishes > before);
    before = publishes; now += 10000U; Settle(); assert(publishes > before);
    /* PING still runs with regular QoS0 telemetry; a silent peer must go offline. */
    allow_pong = 0U; now += 30000U; Settle(); assert(pings != 0U);
    now += 10000U; Tick(1U); assert(!Cloud_IsOnline());
    allow_pong = 1U; Online();
    assert(connects == 2U && subscriptions == 2U);
    assert(strstr(last_json, "\"alarm_count\":1"));
    Command("envCmd", "ARM", 0U); Tick(0U); assert(monitor.armed);
    reset_count = resets;
    response = ESP_AT_RESPONSE_CLOSED; Tick(1U);
    assert(!Cloud_IsOnline() && monitor.armed);
    key1 = 1U; Tick(0U); assert(!monitor.armed); key1 = 0U;
    Online(); assert(resets == reset_count + 1U);
    /* UART loss invalidates the protocol; never process the queued ARM. */
    ++stats.overflow; Command("envCmd", "ARM", 0U); Tick(1U);
    assert(!Cloud_IsOnline() && !monitor.armed);
    Online();
    /* A truncated frame is bounded by a 10 s receive deadline. */
    { const uint8_t half[] = {0x30U, 10U, 0U}; Inject(half, sizeof(half)); }
    Tick(1U); now += 10000U; Tick(1U); assert(!Cloud_IsOnline()); Online();
    allow_prompt = 0U; Command("envCmd", "STATUS", 0U); now += 1200U; Tick(1U);
    now += 5000U; Tick(1U); assert(!Cloud_IsOnline()); allow_prompt = 1U; Online();
    allow_send_ok = 0U; Command("envCmd", "STATUS", 0U); now += 1200U;
    Tick(1U); Tick(1U); now += 5000U; Tick(1U); assert(!Cloud_IsOnline());
    allow_send_ok = 1U; Online();
    puts("PASS 10: handshake, telemetry, controls, alarm sync, local priority, wrap and recovery");
}

int main(void)
{
    TestProtocolAndFraming();
    TestCloudLifecycle();
    return 0;
}
