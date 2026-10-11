/* Fault lab: deliberate, bounded faults that prove the FreeRTOS Inspector detectors work.
   Isolated demo tasks, all below the acquisition / control task (priority 22), so the
   motor loop keeps running whatever is injected. One fault at a time. */
#ifndef FAULTLAB_H
#define FAULTLAB_H

#include <stdint.h>

void faultlab_init(void);                 /* creates the lab tasks, queue and mutex (static) */
int  faultlab_inject(uint8_t fault);      /* HIL_FAULT_*; 0 = started, -1 = busy or unknown */

#endif /* FAULTLAB_H */
