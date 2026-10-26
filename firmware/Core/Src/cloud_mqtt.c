#include "cloud_mqtt.h"
#include <string.h>

uint8_t Mqtt_TopicValid(const char *topic)
{
    size_t i, n = strlen(topic);
    if (n == 0U || n > 40U) return 0U;
    for (i = 0U; i < n; ++i) {
        char c = topic[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_')) return 0U;
    }
    return 1U;
}

static uint16_t Wrap(uint8_t *out, uint16_t capacity, uint8_t header, uint16_t size)
{
    uint16_t prefix = size < 128U ? 2U : 3U;
    if ((uint32_t)size + prefix > capacity) return 0U;
    memmove(out + prefix, out + 3U, size);
    out[0] = header;
    out[1] = (uint8_t)(size & 127U);
    if (prefix == 3U) {
        out[1] |= 128U;
        out[2] = (uint8_t)(size >> 7);
    }
    return size + prefix;
}

static uint16_t String(uint8_t *out, const char *text)
{
    uint16_t n = (uint16_t)strlen(text);
    out[0] = (uint8_t)(n >> 8);
    out[1] = (uint8_t)n;
    memcpy(out + 2U, text, n);
    return n + 2U;
}

uint16_t Mqtt_Connect(uint8_t *out, uint16_t capacity, const char *client_id)
{
    /* Resume the UID session, then explicitly refresh the subscription. */
    static const uint8_t header[] = {0U, 4U, 'M', 'Q', 'T', 'T', 4U, 0U, 0U, 60U};
    size_t n = strlen(client_id);
    if (n == 0U || n > 64U || n + 15U > capacity) return 0U;
    memcpy(out + 3U, header, sizeof(header));
    return Wrap(out, capacity, 0x10U, 10U + String(out + 13U, client_id));
}

uint16_t Mqtt_Subscribe(uint8_t *out, uint16_t capacity, const char *topic)
{
    uint16_t n;
    if (!Mqtt_TopicValid(topic) || strlen(topic) + 8U > capacity) return 0U;
    out[3] = 0U; out[4] = 1U;
    n = String(out + 5U, topic);
    out[5U + n] = 0U; /* QoS 0 subscription; command duplicates are idempotent. */
    return Wrap(out, capacity, 0x82U, n + 3U);
}

uint16_t Mqtt_Publish(uint8_t *out, uint16_t capacity, const char *topic, const char *payload)
{
    size_t n = strlen(topic), m = strlen(payload);
    if (n == 0U || n > 44U || n + m + 5U > capacity) return 0U;
    String(out + 3U, topic);
    memcpy(out + 5U + n, payload, m);
    return Wrap(out, capacity, 0x30U, (uint16_t)(2U + n + m));
}

void Mqtt_Init(MqttParser *p, const char *command_topic)
{
    memset(p, 0, sizeof(*p));
    p->topic = command_topic;
}

uint8_t Mqtt_IsPartial(const MqttParser *p)
{
    return p->header != 0U;
}

static MqttEvent Decode(MqttParser *p)
{
    uint16_t n, body;
    const uint8_t *b = p->data;
    if (p->header == 0x20U)
        return p->remaining == 2U && (b[0] & 0xFEU) == 0U && b[1] == 0U ? MQTT_CONNACK : MQTT_FAULT;
    if (p->header == 0x90U)
        return p->remaining == 3U && b[0] == 0U && b[1] == 1U && b[2] == 0U ? MQTT_SUBACK : MQTT_FAULT;
    if (p->header == 0xD0U) return p->remaining == 0U ? MQTT_PINGRESP : MQTT_FAULT;
    if ((p->header & 0xFEU) != 0x30U || p->remaining < 2U) return MQTT_FAULT;
    n = (uint16_t)((uint16_t)b[0] * 256U + b[1]);
    if (n == 0U || n > p->remaining - 2U) return MQTT_FAULT;
    /* A retained ARM must never arm the board after a reconnect. */
    if ((p->header & 1U) != 0U || n != strlen(p->topic) || memcmp(b + 2U, p->topic, n) != 0)
        return MQTT_NONE;
    body = p->remaining - n - 2U;
    b += n + 2U;
    p->payload_offset = n + 2U;
    p->payload_length = body;
    if (body != 0U && b[0] == '{') return MQTT_OTA;
    if (body == 3U && memcmp(b, "ARM", 3U) == 0) return MQTT_ARM;
    if (body == 6U && memcmp(b, "DISARM", 6U) == 0) return MQTT_DISARM;
    if (body == 6U && memcmp(b, "STATUS", 6U) == 0) return MQTT_STATUS;
    return MQTT_NONE;
}

MqttEvent Mqtt_Feed(MqttParser *p, uint8_t byte)
{
    MqttEvent event;
    if (p->fault != 0U) return MQTT_FAULT;
    if (p->header == 0U) {
        if (byte == 0U) { p->fault = 1U; return MQTT_FAULT; }
        p->header = byte;
        return MQTT_NONE;
    }
    if (p->receiving == 0U) {
        if (p->length_bytes >= 2U || (p->length_bytes == 1U && (byte & 127U) == 0U)) {
            p->fault = 1U; return MQTT_FAULT;
        }
        p->remaining |= (uint16_t)((uint16_t)(byte & 127U) << (7U * p->length_bytes));
        ++p->length_bytes;
        if (p->remaining > sizeof(p->data) || (p->length_bytes == 2U && (byte & 128U) != 0U)) {
            p->fault = 1U; return MQTT_FAULT;
        }
        if ((byte & 128U) != 0U) return MQTT_NONE;
        p->receiving = 1U;
    } else {
        p->data[p->used++] = byte;
    }
    if (p->receiving == 0U || p->used != p->remaining) return MQTT_NONE;
    event = Decode(p);
    p->header = p->length_bytes = p->receiving = 0U;
    p->used = p->remaining = 0U;
    if (event == MQTT_FAULT) p->fault = 1U;
    return event;
}
