/* RTOS Doctor probe for FreeRTOS: observes an application without touching its tasks.

   What it measures, from kernel trace hooks only (no code in the application's tasks):
     - every task, discovered when FreeRTOS creates it (name, priority, stack);
     - per task: CPU share, jobs (release -> block), execution time C, response time R,
       release interval (period) and its jitter, deadline misses, stack margin, state;
     - priority inheritance on mutexes, registered queues (fill, overflows), heap, ISR;
     - a continuous event trace (switches, releases, ticks, interrupts) for the timeline.
   Its own cost is measured too (cycles spent in the hooks).

   Integration (see FreeRTOSConfig.h, USER CODE Defines):
     configUSE_TRACE_FACILITY = 1, configUSE_APPLICATION_TASK_TAG = 1 (the probe owns task tags),
     configRECORD_STACK_HIGH_ADDRESS = 1, and the trace macros. Call rtp_init() once, after the
     DWT cycle counter is enabled and before the first task is created.

   Model and limits: a job ends when the task blocks (or is suspended); a task that blocks
   several times per cycle is seen as several jobs. Deadline = rtp_set_deadline() if given,
   else the measured mean period (after 4 releases). Timestamps: DWT->CYCCNT. */
#ifndef RTOS_PROBE_H
#define RTOS_PROBE_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "rtos_probe_hooks.h"

#define RTP_MAX_TASKS    16U     /* tags 1..15; 0 = not tracked */
#define RTP_MAX_QUEUES    8U     /* queue ids 1..7 */
#define RTP_TRACE_LEN  1024U     /* trace events buffered between two drains (power of two) */
#define RTP_EVT_LEN      32U     /* detector events buffered (power of two) */

/* Detector thresholds */
#define RTP_STACK_LOW_PCT     30U   /* free stack below this: STACK_LOW */
#define RTP_HEAP_LOW_PCT      30U   /* minimum free heap below this: HEAP_LOW */
#define RTP_HOG_MS            50U   /* one job running this long without blocking: CPU_HOG */

typedef struct
{
  const char *name;            /* NULL: tag unused */
  TaskHandle_t handle;
  uint8_t      basePrio;
  uint8_t      flags;          /* HIL_TF_* */
  uint16_t     stackWords;
  uint16_t     deadlineMs;     /* 0 = automatic */
} RtpTaskInfo_t;

typedef struct
{
  uint8_t  state;              /* HIL_TS_* */
  uint8_t  prio;               /* current priority (may be inherited) */
  uint16_t cpuPermille;        /* last second */
  uint16_t jobs;               /* last second */
  uint32_t cMax, cAvg, rMax, rMin;
  uint32_t tAvg, tMin, tMax;
  uint16_t misses;
  uint16_t stackFree;
  uint32_t cMaxLast, rMaxLast; /* worst C and R of the last second */
} RtpTaskStats_t;

typedef struct
{
  uint32_t switches, events, dropped;   /* last second, last second, total */
  uint16_t idlePermille, probePermille, kernelPermille;
} RtpSysStats_t;

typedef struct
{
  const char *name;
  uint16_t length, waiting, maxFill;
  uint32_t failures;
} RtpQueueStats_t;

/* Application side (all optional except rtp_init) */
void    rtp_init(void);
void    rtp_set_deadline(TaskHandle_t task, uint16_t deadlineMs);
uint8_t rtp_queue_register(QueueHandle_t q, const char *name);   /* returns the queue id, 0 if full */
void    rtp_isr_enter(uint8_t isr);
void    rtp_isr_exit(uint8_t isr);
void    rtp_event(uint8_t code, int32_t arg);                     /* application events, same channel */

/* Consumer side (a low-priority task that sends the data out) */
uint32_t rtp_trace_read(uint32_t *cyc, uint8_t *id, uint32_t max);
uint32_t rtp_trace_dropped(void);
int      rtp_event_pop(uint32_t *tMs, uint8_t *code, int32_t *arg);
void     rtp_second(void);                       /* closes the 1 s window and runs the detectors */
int      rtp_task_info(uint8_t tag, RtpTaskInfo_t *info);
void     rtp_task_stats(uint8_t tag, RtpTaskStats_t *s);
void     rtp_sys_stats(RtpSysStats_t *s);
int      rtp_queue_stats(uint8_t qid, RtpQueueStats_t *s);
void     rtp_heap(size_t *freeNow, size_t *minEver);   /* free heap now / minimum ever, in bytes */

#endif /* RTOS_PROBE_H */
