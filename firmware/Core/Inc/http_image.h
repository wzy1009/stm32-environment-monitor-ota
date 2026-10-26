#ifndef HTTP_IMAGE_H
#define HTTP_IMAGE_H
#include "main.h"

HAL_StatusTypeDef HttpImage_Begin(uint32_t size);
HAL_StatusTypeDef HttpImage_Feed(const uint8_t *data, uint16_t length);
HAL_StatusTypeDef HttpImage_Finish(void);
uint32_t HttpImage_Received(void);
uint32_t HttpImage_Written(void);
const char *HttpImage_Error(void);
#endif
