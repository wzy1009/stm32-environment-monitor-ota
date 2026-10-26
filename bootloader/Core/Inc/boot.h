#ifndef BOOT_H
#define BOOT_H
#include <stdint.h>

#define BOOT_SETUP_ADDRESS 0x08007800UL

int Boot_VectorsValid(uint32_t msp, uint32_t reset);
void Boot_Run(void);

#endif
