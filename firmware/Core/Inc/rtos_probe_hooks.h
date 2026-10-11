/* FreeRTOS Inspector probe: the functions called by the FreeRTOS trace macros.
   Included from FreeRTOSConfig.h, so it must not include any FreeRTOS header.
   The macros themselves (FreeRTOSConfig.h, USER CODE Defines) read the kernel
   structures inline, inside tasks.c / queue.c, and only pass plain values here. */
#ifndef RTOS_PROBE_HOOKS_H
#define RTOS_PROBE_HOOKS_H

#include <stdint.h>

uint32_t rtp_on_create(const char *name, uint32_t prio, uint32_t stackWords, void *handle);
void     rtp_on_ready(uint32_t tag);
void     rtp_on_switch_in(uint32_t tag);
void     rtp_on_switch_out(uint32_t tag, uint32_t blocked);
void     rtp_on_tick(uint32_t tickBeforeIncrement);
void     rtp_on_inherit(uint32_t holderTag, uint32_t newPrio, uint32_t waiterTag);
void     rtp_on_disinherit(uint32_t holderTag, uint32_t basePrio);
void     rtp_on_queue_send(uint32_t qid, uint32_t fill, uint32_t length);
void     rtp_on_queue_full(uint32_t qid);

#endif /* RTOS_PROBE_HOOKS_H */
