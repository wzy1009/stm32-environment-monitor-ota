#ifndef AT24C02_H
#define AT24C02_H
#include "main.h"

#define AT24C02_ADDRESS_7BIT 0x50U
#define AT24C02_HAL_ADDRESS (AT24C02_ADDRESS_7BIT << 1)
#define AT24C02_CAPACITY 256UL

HAL_StatusTypeDef At24c02_Probe(void);
HAL_StatusTypeDef At24c02_Read(uint32_t address, uint8_t *data, uint32_t length);
HAL_StatusTypeDef At24c02_Write(uint32_t address, const uint8_t *data, uint32_t length);
#endif
