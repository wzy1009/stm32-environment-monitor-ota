#ifndef HOST_HAL_H
#define HOST_HAL_H
#include <stdint.h>
typedef enum {HAL_OK, HAL_ERROR, HAL_BUSY, HAL_TIMEOUT} HAL_StatusTypeDef;
typedef struct { unsigned unused; } UART_HandleTypeDef;
typedef struct { uint32_t TypeErase, PageAddress, NbPages; } FLASH_EraseInitTypeDef;
#define FLASH_TYPEERASE_PAGES 0U
#define FLASH_TYPEPROGRAM_HALFWORD 1U
#define FLASH_FLAG_EOP 1U
#define FLASH_FLAG_PGERR 2U
#define FLASH_FLAG_WRPERR 4U
#define __HAL_FLASH_CLEAR_FLAG(flags) ((void)(flags))
HAL_StatusTypeDef HAL_FLASH_Unlock(void);
HAL_StatusTypeDef HAL_FLASH_Lock(void);
HAL_StatusTypeDef HAL_FLASHEx_Erase(FLASH_EraseInitTypeDef *erase, uint32_t *error);
HAL_StatusTypeDef HAL_FLASH_Program(uint32_t kind, uint32_t address, uint64_t value);
uint32_t HAL_GetTick(void);
HAL_StatusTypeDef HAL_UART_Transmit(UART_HandleTypeDef *uart, const uint8_t *data, uint16_t length, uint32_t timeout);
void NVIC_SystemReset(void);
#endif
