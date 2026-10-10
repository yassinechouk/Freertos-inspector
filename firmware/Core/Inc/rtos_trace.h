/* RTOS doctor instrumentation: per-task CPU time, per-job execution and response times,
   and a continuous event trace (context switches, RTOS tick, interrupts).

   Hooks (all O(1), interrupts masked for a few cycles):
     - rt_switched_in / rt_tick: called by the FreeRTOS trace macros (FreeRTOSConfig.h, USER CODE).
     - rt_isr_enter / rt_isr_exit: called at the start and end of an instrumented interrupt.
     - rt_job_begin / rt_job_end: called by each periodic task around one job.
   Timestamps are DWT->CYCCNT (CPU cycles, wraps every 53.7 s at 80 MHz).
   The link task drains the trace and sends the statistics (HIL_T_TASK_* frames). */
#ifndef RTOS_TRACE_H
#define RTOS_TRACE_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

#define RT_MAX_TAGS    16U     /* task tags 0..15; 0 = untagged */
#define RT_TRACE_LEN   1024U   /* events buffered between two link cycles (power of two) */

typedef struct
{
  const char  *name;           /* NULL: tag unused */
  TaskHandle_t handle;
  uint8_t      priority;
  uint16_t     periodMs;       /* minimum inter-arrival time for a sporadic task; 0 = none */
  uint16_t     deadlineMs;     /* relative deadline; 0 = none */
  uint32_t     wcetCycles;     /* declared execution time; 0 = unknown */
  uint16_t     stackWords;
} RtTaskInfo_t;

typedef struct
{
  uint16_t cpuPermille;        /* share of the CPU in the last second */
  uint16_t jobs;               /* jobs completed in the last second */
  uint32_t cMax, cAvg;         /* execution time per job, cycles, since reset */
  uint32_t rMax, rMin;         /* response time (release -> end of job), cycles, since reset */
  uint16_t misses;             /* jobs with r > deadline, since reset */
  uint16_t stackFree;          /* words never used */
} RtTaskStats_t;

typedef struct
{
  uint32_t switches;           /* context switches in the last second */
  uint32_t events;             /* trace events in the last second */
  uint32_t dropped;            /* trace events lost (buffer full), total */
  uint16_t idlePermille;
} RtSysStats_t;

void     rt_init(void);
void     rt_register(uint8_t tag, TaskHandle_t h, const char *name, uint16_t periodMs,
                     uint16_t deadlineMs, uint32_t wcetCycles, uint16_t stackWords);

/* Hooks */
void     rt_switched_in(void *tag);
void     rt_tick(uint32_t tickBeforeIncrement);
void     rt_isr_enter(uint8_t isr);
void     rt_isr_exit(uint8_t isr);
void     rt_job_begin(uint8_t tag, uint32_t releaseCyc);
void     rt_job_end(uint8_t tag);
uint32_t rt_release_from_tick(TickType_t wakeTick);   /* cycle stamp of a recent tick (last 16) */

/* Consumer side (link task) */
uint32_t rt_trace_read(uint32_t *cyc, uint8_t *id, uint32_t max);
uint32_t rt_trace_dropped(void);
void     rt_second(void);                             /* closes the 1 s statistics window */
int      rt_task_info(uint8_t tag, RtTaskInfo_t *info);
void     rt_task_stats(uint8_t tag, RtTaskStats_t *s);
void     rt_sys_stats(RtSysStats_t *s);

#endif /* RTOS_TRACE_H */
