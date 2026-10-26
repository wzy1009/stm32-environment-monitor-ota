#ifndef BOOT_DISPLAY_H
#define BOOT_DISPLAY_H

#include <stdint.h>

void BootDisplay_Init(void);
void BootDisplay_Show(const char *stage, const char *version, uint8_t percent, uint8_t force);

#endif
