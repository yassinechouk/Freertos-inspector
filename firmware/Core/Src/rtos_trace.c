/* RTOS doctor instrumentation, see rtos_trace.h. */
#include "rtos_trace.h"
#include "stm32l4xx.h"
#include "hil_proto.h"

/* ---- Event trace: multi-producer (hooks at several interrupt levels), single consumer ---- */
static volatile uint32_t trCyc[RT_TRACE_LEN];
static volatile uint8_t  trId[RT_TRACE_LEN];
static volatile uint32_t trHead;                 /* written by producers, interrupts masked */
static volatile uint32_t trTail;                 /* written by the consumer only */
static volatile uint32_t trDropped;
static volatile uint32_t trEvents;

/* ---- CPU-time accounting, updated at each context switch ---------------------------- */
static uint64_t runCyc[RT_MAX_TAGS];            /* cycles spent in each tag since reset */
static uint32_t curTag;                          /* tag of the running task */
static uint32_t lastSwitchCyc;
static volatile uint32_t switches;
static uint32_t tickCyc[16];                     /* cycle stamp of the last 16 ticks */

/* ---- Per-task configuration and job statistics -------------------------------------- */
typedef struct
{
  uint64_t runAtBegin;
  uint64_t cSum;
  uint32_t release, active;
  uint32_t cMax, rMax, rMin, jobs, jobsWin, misses;
  uint32_t deadlineCyc;
} RtJob_t;

static RtTaskInfo_t info[RT_MAX_TAGS];
static RtJob_t      job[RT_MAX_TAGS];

/* Last closed 1 s window */
static uint64_t runPrev[RT_MAX_TAGS];
static uint16_t cpuPm[RT_MAX_TAGS];
static uint16_t jobsLast[RT_MAX_TAGS];
static uint32_t secStartCyc, switchesLast, eventsLast, switchesPrev, eventsPrev;

static inline void tr_put_locked(uint32_t now, uint8_t id)
{
  const uint32_t h = trHead;
  if ((h - trTail) >= RT_TRACE_LEN) { trDropped++; }
  else
  {
    trCyc[h & (RT_TRACE_LEN - 1U)] = now;
    trId[h & (RT_TRACE_LEN - 1U)]  = id;
    trHead = h + 1U;
  }
  trEvents++;
}

void rt_init(void)
{
  lastSwitchCyc = DWT->CYCCNT;
  secStartCyc = lastSwitchCyc;
  for (uint32_t i = 0U; i < RT_MAX_TAGS; i++) { job[i].rMin = UINT32_MAX; }
}

void rt_register(uint8_t tag, TaskHandle_t h, const char *name, uint16_t periodMs,
                 uint16_t deadlineMs, uint32_t wcetCycles, uint16_t stackWords)
{
  if ((tag == 0U) || (tag >= RT_MAX_TAGS) || (h == NULL)) { return; }
  info[tag].name = name;
  info[tag].handle = h;
  info[tag].priority = (uint8_t)uxTaskPriorityGet(h);
  info[tag].periodMs = periodMs;
  info[tag].deadlineMs = deadlineMs;
  info[tag].wcetCycles = wcetCycles;
  info[tag].stackWords = stackWords;
  job[tag].deadlineCyc = (uint32_t)deadlineMs * (SystemCoreClock / 1000U);
  vTaskSetApplicationTaskTag(h, (TaskHookFunction_t)(uintptr_t)tag);
}

/* Called inside vTaskSwitchContext (PendSV, BASEPRI raised). */
void rt_switched_in(void *tagp)
{
  uint32_t tag = (uint32_t)(uintptr_t)tagp;
  if (tag >= RT_MAX_TAGS) { tag = 0U; }
  const uint32_t pm = __get_PRIMASK();
  __disable_irq();
  if (tag != curTag)
  {
    const uint32_t now = DWT->CYCCNT;
    runCyc[curTag] += now - lastSwitchCyc;
    lastSwitchCyc = now;
    curTag = tag;
    switches++;
    tr_put_locked(now, (uint8_t)tag);
  }
  __set_PRIMASK(pm);
}

/* Called at the start of xTaskIncrementTick (tick interrupt). */
void rt_tick(uint32_t tickBeforeIncrement)
{
  const uint32_t pm = __get_PRIMASK();
  __disable_irq();
  const uint32_t now = DWT->CYCCNT;
  tickCyc[(tickBeforeIncrement + 1U) & 15U] = now;
  tr_put_locked(now, (uint8_t)HIL_TR_TICK);
  __set_PRIMASK(pm);
}

void rt_isr_enter(uint8_t isr)
{
  const uint32_t pm = __get_PRIMASK();
  __disable_irq();
  tr_put_locked(DWT->CYCCNT, (uint8_t)(HIL_TR_ISR_IN | (isr & 0x3FU)));
  __set_PRIMASK(pm);
}

void rt_isr_exit(uint8_t isr)
{
  const uint32_t pm = __get_PRIMASK();
  __disable_irq();
  tr_put_locked(DWT->CYCCNT, (uint8_t)(HIL_TR_ISR_OUT | (isr & 0x3FU)));
  __set_PRIMASK(pm);
}

uint32_t rt_release_from_tick(TickType_t wakeTick)
{
  return tickCyc[wakeTick & 15U];
}

/* CPU time of a tag up to now, including the slice in progress. Interrupts masked by caller. */
static inline uint64_t run_now(uint32_t tag, uint32_t now)
{
  return runCyc[tag] + ((curTag == tag) ? (uint64_t)(now - lastSwitchCyc) : 0U);
}

void rt_job_begin(uint8_t tag, uint32_t releaseCyc)
{
  if (tag >= RT_MAX_TAGS) { return; }
  const uint32_t pm = __get_PRIMASK();
  __disable_irq();
  job[tag].runAtBegin = run_now(tag, DWT->CYCCNT);
  __set_PRIMASK(pm);
  job[tag].release = releaseCyc;
  job[tag].active = 1U;
}

void rt_job_end(uint8_t tag)
{
  if ((tag >= RT_MAX_TAGS) || (job[tag].active == 0U)) { return; }
  const uint32_t pm = __get_PRIMASK();
  __disable_irq();
  const uint32_t now = DWT->CYCCNT;
  const uint64_t run = run_now(tag, now);
  __set_PRIMASK(pm);

  RtJob_t *j = &job[tag];
  const uint32_t c = (uint32_t)(run - j->runAtBegin);
  const uint32_t r = now - j->release;
  if (c > j->cMax) { j->cMax = c; }
  if (r > j->rMax) { j->rMax = r; }
  if (r < j->rMin) { j->rMin = r; }
  if ((j->deadlineCyc != 0U) && (r > j->deadlineCyc)) { j->misses++; }
  j->cSum += c;
  j->jobs++;
  j->jobsWin++;
  j->active = 0U;
}

uint32_t rt_trace_read(uint32_t *cyc, uint8_t *id, uint32_t max)
{
  uint32_t n = 0U;
  uint32_t t = trTail;
  const uint32_t h = trHead;
  while ((t != h) && (n < max))
  {
    cyc[n] = trCyc[t & (RT_TRACE_LEN - 1U)];
    id[n]  = trId[t & (RT_TRACE_LEN - 1U)];
    t++;
    n++;
  }
  trTail = t;
  return n;
}

uint32_t rt_trace_dropped(void) { return trDropped; }

void rt_second(void)
{
  uint64_t run[RT_MAX_TAGS];
  const uint32_t pm = __get_PRIMASK();
  __disable_irq();
  const uint32_t now = DWT->CYCCNT;
  for (uint32_t i = 0U; i < RT_MAX_TAGS; i++) { run[i] = run_now(i, now); }
  const uint32_t sw = switches, ev = trEvents;
  __set_PRIMASK(pm);

  const uint32_t elapsed = now - secStartCyc;
  secStartCyc = now;
  for (uint32_t i = 0U; i < RT_MAX_TAGS; i++)
  {
    const uint64_t d = run[i] - runPrev[i];
    runPrev[i] = run[i];
    cpuPm[i] = (elapsed != 0U) ? (uint16_t)((d * 1000U) / elapsed) : 0U;
    jobsLast[i] = (uint16_t)job[i].jobsWin;
    job[i].jobsWin = 0U;
  }
  switchesLast = sw - switchesPrev; switchesPrev = sw;
  eventsLast = ev - eventsPrev;     eventsPrev = ev;
}

int rt_task_info(uint8_t tag, RtTaskInfo_t *out)
{
  if ((tag >= RT_MAX_TAGS) || (info[tag].name == NULL)) { return 0; }
  *out = info[tag];
  return 1;
}

void rt_task_stats(uint8_t tag, RtTaskStats_t *s)
{
  const RtJob_t *j = &job[tag];
  s->cpuPermille = cpuPm[tag];
  s->jobs = jobsLast[tag];
  s->cMax = j->cMax;
  s->cAvg = (j->jobs != 0U) ? (uint32_t)(j->cSum / j->jobs) : 0U;
  s->rMax = j->rMax;
  s->rMin = (j->rMin == UINT32_MAX) ? 0U : j->rMin;
  s->misses = (uint16_t)((j->misses > 0xFFFFU) ? 0xFFFFU : j->misses);
  s->stackFree = (info[tag].handle != NULL) ? (uint16_t)uxTaskGetStackHighWaterMark(info[tag].handle) : 0U;
}

void rt_sys_stats(RtSysStats_t *s)
{
  s->switches = switchesLast;
  s->events = eventsLast;
  s->dropped = trDropped;
  s->idlePermille = 0U;                           /* filled by the caller from the idle tag */
}
