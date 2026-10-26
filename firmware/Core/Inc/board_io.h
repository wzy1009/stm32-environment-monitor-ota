#ifndef BOARD_IO_H
#define BOARD_IO_H

#include <stdint.h>

void BoardIo_Init(void);
uint8_t BoardIo_Key0Down(void);
uint8_t BoardIo_Key1Down(void);
uint8_t BoardIo_Motion(void);
void BoardIo_SetOutputs(uint32_t now, uint8_t motion, uint8_t alarm);
void BoardIo_DhtDriveLow(void);
void BoardIo_DhtRelease(void);
uint8_t BoardIo_DhtLevel(void);
uint32_t BoardIo_Cycles(void);
uint32_t BoardIo_CyclesPerUs(void);

#endif
