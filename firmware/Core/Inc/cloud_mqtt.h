#ifndef CLOUD_MQTT_H
#define CLOUD_MQTT_H
#include <stdint.h>

#define MQTT_PACKET_CAPACITY 448U
typedef enum {
    MQTT_NONE, MQTT_CONNACK, MQTT_SUBACK, MQTT_PINGRESP,
    MQTT_ARM, MQTT_DISARM, MQTT_STATUS, MQTT_OTA, MQTT_FAULT
} MqttEvent;
typedef struct {
    uint8_t data[MQTT_PACKET_CAPACITY];
    const char *topic;
    uint16_t remaining, used, payload_offset, payload_length;
    uint8_t header, length_bytes, receiving, fault;
} MqttParser;

void Mqtt_Init(MqttParser *parser, const char *command_topic);
MqttEvent Mqtt_Feed(MqttParser *parser, uint8_t byte);
uint8_t Mqtt_IsPartial(const MqttParser *parser);
uint16_t Mqtt_Connect(uint8_t *out, uint16_t capacity, const char *client_id);
uint16_t Mqtt_Subscribe(uint8_t *out, uint16_t capacity, const char *topic);
uint16_t Mqtt_Publish(uint8_t *out, uint16_t capacity, const char *topic, const char *payload);
uint8_t Mqtt_TopicValid(const char *topic);
#endif
