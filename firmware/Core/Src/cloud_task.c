#include "cloud_task.h"
#include "cloud_config.h"
#include "cloud_mqtt.h"
#include "esp_at.h"
#include <stdio.h>
#include <string.h>

typedef enum {
    DISABLED, RETRY, BOOT, COMMAND, PREP_CONNECT, PREP_SUBSCRIBE,
    PROMPT, SEND, WAIT_CONNACK, WAIT_SUBACK, ONLINE, WAIT_PING
} Phase;
typedef enum {TX_CONNECT, TX_SUBSCRIBE, TX_REPORT, TX_PING} TxKind;
typedef struct {
    int16_t temperature;
    uint16_t humidity;
    uint8_t valid, dht, ready, motion, armed, alarm;
} Snapshot;

static const char * const commands[] = {
    "AT\r\n", "ATE0\r\n", "AT+CWMODE_CUR=1\r\n", "AT+CWDHCP_CUR=1,1\r\n",
    "AT+CIPMUX=0\r\n", "AT+CIPMODE=0\r\n", NULL, "AT+CIPSTART=\"TCP\",\"bemfa.com\",9501\r\n"
};
static Phase phase;
static TxKind tx_kind;
static MqttParser parser;
static Snapshot snapshot;
static EspAt_Stats rx_stats;
static uint8_t packet[MQTT_PACKET_CAPACITY];
static uint16_t packet_length;
static uint32_t since, wait_ms, retry_ms, last_ping, last_report, generation, tx_generation;
static uint32_t alarm_count, last_alarm_ms, partial_since;
static uint8_t step, online, connack, suback, pong, fault, pending_command, dirty, partial;

uint8_t Cloud_IsOnline(void) { return online; }

const char *Cloud_StatusText(void)
{
    return phase == DISABLED ? "NET CONFIG" : online != 0U ? "NET ONLINE" : "NET OFFLINE";
}

static uint8_t ConfigValid(void)
{
    const char *values[] = {CLOUD_WIFI_SSID, CLOUD_WIFI_PASSWORD};
    size_t i, j, n;
    n = strlen(CLOUD_BEMFA_UID);
    if (n != 32U) return 0U;
    for (i = 0U; i < n; ++i) {
        char c = CLOUD_BEMFA_UID[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return 0U;
    }
    for (i = 0U; i < 2U; ++i) {
        n = strlen(values[i]);
        if ((i == 0U && (n == 0U || n > 32U)) || (i == 1U && n > 64U)) return 0U;
        for (j = 0U; j < n; ++j) {
            uint8_t c = (uint8_t)values[i][j];
            if (c < 32U || c == 127U || c == '"' || c == '\\') return 0U;
        }
    }
    return Mqtt_TopicValid(CLOUD_STATE_TOPIC) && Mqtt_TopicValid(CLOUD_COMMAND_TOPIC) &&
           strcmp(CLOUD_STATE_TOPIC, CLOUD_COMMAND_TOPIC) != 0;
}

static void Fail(uint32_t now)
{
    online = connack = suback = pong = pending_command = partial = 0U;
    phase = RETRY;
    since = now;
    wait_ms = retry_ms;
    retry_ms = retry_ms < 15000U ? retry_ms * 2U : 30000U;
    EspAt_ResetTransport();
    Mqtt_Init(&parser, CLOUD_COMMAND_TOPIC);
    EspAt_GetStats(&rx_stats);
}

static void Receive(uint8_t byte)
{
    MqttEvent event = Mqtt_Feed(&parser, byte);
    if (event == MQTT_FAULT) fault = 1U;
    if (event == MQTT_CONNACK) {
        if ((tx_kind == TX_CONNECT && (phase == PROMPT || phase == SEND)) || phase == WAIT_CONNACK)
            connack = 1U;
        else fault = 1U;
    }
    if (event == MQTT_SUBACK) {
        if ((tx_kind == TX_SUBSCRIBE && (phase == PROMPT || phase == SEND)) || phase == WAIT_SUBACK)
            suback = 1U;
        else fault = 1U;
    }
    if (event == MQTT_PINGRESP) pong = 1U;
    if (online == 0U && suback == 0U) return;
    if (event == MQTT_DISARM) pending_command = 2U;
    if (event == MQTT_ARM && pending_command != 2U) pending_command = 1U;
    if (event == MQTT_STATUS) dirty = 1U;
}

void Cloud_Init(uint32_t now)
{
    memset(&snapshot, 0, sizeof(snapshot));
    phase = DISABLED;
    online = connack = suback = pong = fault = pending_command = partial = 0U;
    dirty = 1U;
    generation = alarm_count = last_alarm_ms = 0U;
    retry_ms = 2000U;
    Mqtt_Init(&parser, CLOUD_COMMAND_TOPIC);
    if (ConfigValid() == 0U) return;
    (void)EspAt_Start();
    EspAt_ResetTransport();
    EspAt_GetStats(&rx_stats);
    phase = RETRY;
    since = now;
    wait_ms = 0U;
}

static uint8_t SendCommand(const char *command, uint32_t now)
{
    EspAt_ResetResponse();
    since = now;
    if (EspAt_Send(command) != HAL_OK) { Fail(now); return 0U; }
    return 1U;
}

static void StartPacket(TxKind kind, uint32_t now)
{
    char command[32];
    if (packet_length == 0U) { Fail(now); return; }
    tx_kind = kind;
    (void)snprintf(command, sizeof(command), "AT+CIPSEND=%u\r\n", (unsigned)packet_length);
    if (SendCommand(command, now) != 0U) phase = PROMPT;
}

static void Observe(const MonitorState *state, const Dht11Sample *sample, Dht11Status status, uint32_t now)
{
    Snapshot next;
    memset(&next, 0, sizeof(next));
    next.temperature = sample->valid != 0U ? sample->temperature_x10 : 0;
    next.humidity = sample->valid != 0U ? sample->humidity_x10 : 0U;
    next.valid = sample->valid;
    next.dht = (uint8_t)status;
    next.ready = state->ready;
    next.motion = state->motion;
    next.armed = state->armed;
    next.alarm = state->alarm;
    if (state->alarm != 0U && snapshot.alarm == 0U) {
        ++alarm_count;
        last_alarm_ms = now;
    }
    if (memcmp(&next, &snapshot, sizeof(next)) != 0) {
        snapshot = next;
        ++generation;
        dirty = 1U;
    }
}

static void Report(uint32_t now)
{
    char json[280], topic[48], temperature[16], humidity[16];
    int n;
    if (snapshot.valid != 0U) {
        (void)snprintf(temperature, sizeof(temperature), "%d", (int)snapshot.temperature);
        (void)snprintf(humidity, sizeof(humidity), "%u", (unsigned)snapshot.humidity);
    } else {
        strcpy(temperature, "null"); strcpy(humidity, "null");
    }
    n = snprintf(json, sizeof(json),
        "{\"temp_x10\":%s,\"humi_x10\":%s,\"valid\":%u,\"dht\":%u,\"ready\":%u,"
        "\"motion\":%u,\"armed\":%u,\"alarm\":%u,\"alarm_count\":%lu,\"last_alarm_ms\":%lu,\"uptime_s\":%lu}",
        temperature, humidity, (unsigned)snapshot.valid, (unsigned)snapshot.dht,
        (unsigned)snapshot.ready, (unsigned)snapshot.motion, (unsigned)snapshot.armed,
        (unsigned)snapshot.alarm, (unsigned long)alarm_count, (unsigned long)last_alarm_ms,
        (unsigned long)(now / 1000U));
    if (n < 0 || (size_t)n >= sizeof(json)) { Fail(now); return; }
    (void)snprintf(topic, sizeof(topic), "%s/up", CLOUD_STATE_TOPIC);
    packet_length = Mqtt_Publish(packet, sizeof(packet), topic, json);
    tx_generation = generation;
    /* Clear before transmission: a STATUS request received during SEND must survive. */
    dirty = 0U;
    StartPacket(TX_REPORT, now);
}

void Cloud_Task(uint32_t now, MonitorState *state, const Dht11Sample *sample,
                Dht11Status status, uint8_t can_send)
{
    EspAt_Response response;
    EspAt_Stats stats;
    char join[128];
    uint8_t is_partial;
    if (phase == DISABLED) return;
    Observe(state, sample, status, now);
    EspAt_Poll();
    EspAt_GetStats(&stats);
    if (stats.overflow != rx_stats.overflow || stats.uart_errors != rx_stats.uart_errors ||
        stats.rearm_failures != rx_stats.rearm_failures) { Fail(now); return; }
    response = EspAt_GetResponse();
    if (phase != RETRY && phase != BOOT &&
        (fault != 0U || EspAt_HasStreamFault() != 0U || response == ESP_AT_RESPONSE_ERROR ||
         response == ESP_AT_RESPONSE_BAD_TEXT || response == ESP_AT_RESPONSE_CLOSED)) {
        Fail(now); return;
    }
    is_partial = Mqtt_IsPartial(&parser);
    if (is_partial != 0U && partial == 0U) partial_since = now;
    partial = is_partial;
    if (partial != 0U && (uint32_t)(now - partial_since) >= 10000U) { Fail(now); return; }
    if (pending_command != 0U) {
        Monitor_SetArmed(state, now, pending_command == 1U);
        pending_command = 0U;
        dirty = 1U;
    }
    Observe(state, sample, status, now);
    /* Receive/control continue during DHT's start-low window, but UART TX is deferred. */
    if (can_send == 0U) return;
    switch (phase) {
    case RETRY:
        if ((uint32_t)(now - since) < wait_ms) break;
        EspAt_ResetTransport();
        Mqtt_Init(&parser, CLOUD_COMMAND_TOPIC);
        fault = partial = 0U;
        if (SendCommand("AT+RST\r\n", now) != 0U) phase = BOOT;
        break;
    case BOOT:
        if ((uint32_t)(now - since) < 2000U) break;
        EspAt_ResetTransport();
        step = 0U;
        if (SendCommand(commands[step], now) != 0U) phase = COMMAND;
        break;
    case COMMAND:
        if (response == ESP_AT_RESPONSE_OK) {
            ++step;
            if (step == sizeof(commands) / sizeof(commands[0])) {
                EspAt_ResetResponse();
                EspAt_SetDataHandler(Receive);
                phase = PREP_CONNECT;
            } else if (step == 6U) {
                (void)snprintf(join, sizeof(join), "AT+CWJAP_CUR=\"%s\",\"%s\"\r\n",
                               CLOUD_WIFI_SSID, CLOUD_WIFI_PASSWORD);
                (void)SendCommand(join, now);
            } else (void)SendCommand(commands[step], now);
        } else if ((uint32_t)(now - since) >= (step == 6U ? 30000U : 10000U)) Fail(now);
        break;
    case PREP_CONNECT:
        packet_length = Mqtt_Connect(packet, sizeof(packet), CLOUD_BEMFA_UID);
        StartPacket(TX_CONNECT, now);
        break;
    case PREP_SUBSCRIBE:
        packet_length = Mqtt_Subscribe(packet, sizeof(packet), CLOUD_COMMAND_TOPIC);
        StartPacket(TX_SUBSCRIBE, now);
        break;
    case PROMPT:
        if (EspAt_HasPrompt() != 0U) {
            EspAt_ResetResponse();
            if (EspAt_SendBytes(packet, packet_length) != HAL_OK) Fail(now);
            else { phase = SEND; since = now; }
        } else if ((uint32_t)(now - since) >= 5000U) Fail(now);
        break;
    case SEND:
        if (response == ESP_AT_RESPONSE_SEND_OK) {
            EspAt_ResetResponse();
            since = now;
            if (tx_kind == TX_CONNECT) phase = WAIT_CONNACK;
            else if (tx_kind == TX_SUBSCRIBE) phase = WAIT_SUBACK;
            else if (tx_kind == TX_PING) phase = WAIT_PING;
            else {
                last_report = now;
                if (generation != tx_generation) dirty = 1U;
                phase = ONLINE;
            }
        } else if ((uint32_t)(now - since) >= 5000U) Fail(now);
        break;
    case WAIT_CONNACK:
        if (connack != 0U) { connack = 0U; phase = PREP_SUBSCRIBE; }
        else if ((uint32_t)(now - since) >= 10000U) Fail(now);
        break;
    case WAIT_SUBACK:
        if (suback != 0U) {
            suback = 0U;
            online = dirty = 1U;
            retry_ms = 2000U;
            last_ping = now;
            last_report = now - 1000U;
            phase = ONLINE;
        } else if ((uint32_t)(now - since) >= 10000U) Fail(now);
        break;
    case ONLINE:
        if ((uint32_t)(now - last_ping) >= 30000U) {
            packet[0] = 0xC0U; packet[1] = 0U; packet_length = 2U;
            pong = 0U;
            last_ping = now;
            StartPacket(TX_PING, now);
        } else if ((dirty != 0U && (uint32_t)(now - last_report) >= 1000U) ||
                   (uint32_t)(now - last_report) >= 10000U) Report(now);
        break;
    case WAIT_PING:
        if (pong != 0U) { pong = 0U; phase = ONLINE; }
        else if ((uint32_t)(now - since) >= 10000U) Fail(now);
        break;
    default: break;
    }
}
