#include "main.h"

/* No C stack access is permitted after replacing MSP. */
__asm void Boot_Enter(uint32_t msp, uint32_t reset)
{
    MOVS r2, #0
    MSR CONTROL, r2
    MSR BASEPRI, r2
    MSR FAULTMASK, r2
    MSR MSP, r0
    DSB
    ISB
    CPSIE i
    BX r1
}
