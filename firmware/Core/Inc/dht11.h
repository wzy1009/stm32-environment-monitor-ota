#ifndef DHT11_H
#define DHT11_H

#include <stdint.h>

typedef enum {
    DHT11_WAIT,
    DHT11_OK,
    DHT11_TIMEOUT,
    DHT11_CHECKSUM,
    DHT11_DATA_ERROR
} Dht11Status;

typedef struct {
    int16_t temperature_x10;
    uint16_t humidity_x10;
    uint8_t valid;
} Dht11Sample;

void Dht11_Init(uint32_t now);
void Dht11_Task(uint32_t now);
uint8_t Dht11_IsBusy(void);
const Dht11Sample *Dht11_GetSample(void);
Dht11Status Dht11_GetStatus(void);
Dht11Status Dht11_DecodeFrame(const uint8_t frame[5], Dht11Sample *sample);

#endif
