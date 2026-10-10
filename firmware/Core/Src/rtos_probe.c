/* RTOS Doctor probe for FreeRTOS, see rtos_probe.h. */
#include "rtos_probe.h"
#include "stm32l4xx.h"
#include "hil_proto.h"

#define NO_TAG   0xFFU

#ifndef configIDLE_TASK_NAME
#define configIDLE_TASK_NAME "IDLE"          /* same default as tasks.c */
#endif

typedef struct
{
  /* configuration, discovered at creation */
  const char  *name;
  TaskHandle_t handle;
  uint8_t      basePrio, flags;
  uint16_t     stackWords, deadlineMs;
  uint32_t     deadlineCyc;
  /* CPU-time accounting */
  uint64_t     run, runPrev;
  /* current job */
  uint8_t      open;
  uint32_t     release, lastRelease, releases;
  uint64_t     runAtRelease;
  /* job statistics since reset */
  uint32_t     cMax, rMax, rMin, jobs, jobsWin, misses, missesReported;
  uint64_t     cSum;
  /* release interval (period) */
  uint32_t     tAvg, tMin, tMax, intervals;
  /* last closed second and detector latches */
  uint16_t     cpuPm, jobsLast, stackFree;
  uint8_t      stackLowSent, starvedSent, everRan;
  uint32_t     inheritStart;
} RtpTask_t;

typedef struct
{
  const char   *name;
  QueueHandle_t handle;
  uint16_t      length, maxFill;
  uint32_t      failures, failuresReported;
} RtpQueue_t;

static RtpTask_t  task[RTP_MAX_TASKS];
static RtpQueue_t queue[RTP_MAX_QUEUES];
static uint32_t   nextTag = 1U, nextQueue = 1U;

static uint32_t curTag = NO_TAG, lastOutTag = NO_TAG;
static uint32_t sliceStart, lastOutCyc;
static uint64_t kernelCyc, kernelPrev, hookCyc, hookPrev;
static uint32_t switches, switchesPrev, switchesLast;
static volatile uint32_t curTick;
static uint32_t hogCyc, cyclesPerUs;

/* Trace ring: several producers (hooks at several interrupt levels), one consumer. */
static volatile uint32_t trCyc[RTP_TRACE_LEN];
static volatile uint8_t  trId[RTP_TRACE_LEN];
static volatile uint32_t trHead, trTail, trDropped, trEvents;
static uint32_t eventsPrev, eventsLast;

/* Detector events */
static volatile uint32_t evT[RTP_EVT_LEN];
static volatile uint8_t  evCode[RTP_EVT_LEN];
static volatile int32_t  evArg[RTP_EVT_LEN];
static volatile uint32_t evHead, evTail;

static uint32_t secStart;
static uint8_t  heapLowSent;
static uint16_t idlePm, probePm, kernelPm;

/* ---- Internal helpers (interrupts masked by the caller) ----------------------- */
static inline void tr_put(uint32_t now, uint8_t id)
{
  const uint32_t h = trHead;
  if ((h - trTail) >= RTP_TRACE_LEN) { trDropped++; }
  else
  {
    trCyc[h & (RTP_TRACE_LEN - 1U)] = now;
    trId[h & (RTP_TRACE_LEN - 1U)]  = id;
    trHead = h + 1U;
  }
  trEvents++;
}

static inline void ev_put(uint8_t code, int32_t arg)
{
  const uint32_t h = evHead;
  if ((h - evTail) < RTP_EVT_LEN)
  {
    evT[h & (RTP_EVT_LEN - 1U)]    = curTick;
    evCode[h & (RTP_EVT_LEN - 1U)] = code;
    evArg[h & (RTP_EVT_LEN - 1U)]  = arg;
    evHead = h + 1U;
  }
}

static inline uint32_t lock(void)            { const uint32_t pm = __get_PRIMASK(); __disable_irq(); return pm; }
static inline void     unlock(uint32_t pm)   { __set_PRIMASK(pm); }
static inline int      valid(uint32_t tag)   { return (tag > 0U) && (tag < RTP_MAX_TASKS) && (task[tag].name != NULL); }

/* ---- Application side --------------------------------------------------------- */
void rtp_init(void)
{
  cyclesPerUs = SystemCoreClock / 1000000U;
  hogCyc = RTP_HOG_MS * 1000U * cyclesPerUs;
  secStart = DWT->CYCCNT;
  for (uint32_t i = 0U; i < RTP_MAX_TASKS; i++) { task[i].rMin = UINT32_MAX; task[i].tMin = UINT32_MAX; }
}

void rtp_set_deadline(TaskHandle_t h, uint16_t deadlineMs)
{
  for (uint32_t i = 1U; i < RTP_MAX_TASKS; i++)
  {
    if ((task[i].name != NULL) && (task[i].handle == h))
    {
      task[i].deadlineMs = deadlineMs;
      task[i].deadlineCyc = (uint32_t)deadlineMs * 1000U * cyclesPerUs;
    }
  }
}

uint8_t rtp_queue_register(QueueHandle_t q, const char *name)
{
  if ((q == NULL) || (nextQueue >= RTP_MAX_QUEUES)) { return 0U; }
  const uint8_t id = (uint8_t)nextQueue++;
  queue[id].name = name;
  queue[id].handle = q;
  queue[id].length = (uint16_t)(uxQueueMessagesWaiting(q) + uxQueueSpacesAvailable(q));
  vQueueSetQueueNumber(q, id);
  return id;
}

void rtp_isr_enter(uint8_t isr)
{
  const uint32_t pm = lock();
  const uint32_t t0 = DWT->CYCCNT;
  tr_put(t0, (uint8_t)(HIL_TR_ISR_IN | (isr & 0x3FU)));
  hookCyc += DWT->CYCCNT - t0;
  unlock(pm);
}

void rtp_isr_exit(uint8_t isr)
{
  const uint32_t pm = lock();
  const uint32_t t0 = DWT->CYCCNT;
  tr_put(t0, (uint8_t)(HIL_TR_ISR_OUT | (isr & 0x3FU)));
  hookCyc += DWT->CYCCNT - t0;
  unlock(pm);
}

void rtp_event(uint8_t code, int32_t arg)
{
  const uint32_t pm = lock();
  ev_put(code, arg);
  unlock(pm);
}

/* ---- Kernel hooks (called by the trace macros, see FreeRTOSConfig.h) ------------ */

/* Inside xTaskCreate*, before the task is first made ready: gives it its tag. */
uint32_t rtp_on_create(const char *name, uint32_t prio, uint32_t stackWords, void *handle)
{
  const uint32_t pm = lock();
  uint32_t tag = 0U;
  if (nextTag < RTP_MAX_TASKS)
  {
    tag = nextTag++;
    RtpTask_t *t = &task[tag];
    t->name = name;
    t->handle = (TaskHandle_t)handle;
    t->basePrio = (uint8_t)prio;
    t->stackWords = (uint16_t)stackWords;
    const char *idle = configIDLE_TASK_NAME;
    uint32_t k = 0U;
    while ((idle[k] != '\0') && (name[k] == idle[k])) { k++; }
    t->flags = ((idle[k] == '\0') && (name[k] == '\0')) ? HIL_TF_IDLE : 0U;
  }
  unlock(pm);
  return tag;
}

/* A task enters a ready list: a release, unless its current job is still open
   (then it is only moved between ready lists, e.g. by priority inheritance). */
void rtp_on_ready(uint32_t tag)
{
  if (!valid(tag)) { return; }
  const uint32_t pm = lock();
  const uint32_t now = DWT->CYCCNT;
  RtpTask_t *t = &task[tag];
  if (t->open == 0U)
  {
    if (t->releases != 0U)
    {
      const uint32_t dt = now - t->lastRelease;
      if (t->intervals == 0U) { t->tAvg = dt; }
      else { t->tAvg = (uint32_t)((int32_t)t->tAvg + ((int32_t)(dt - t->tAvg) / 16)); }
      if (dt < t->tMin) { t->tMin = dt; }
      if (dt > t->tMax) { t->tMax = dt; }
      t->intervals++;
    }
    t->releases++;
    t->lastRelease = now;
    t->release = now;
    t->runAtRelease = t->run;
    t->open = 1U;
    tr_put(now, (uint8_t)(HIL_TR_READY | (tag & 0x3FU)));   /* tags < 16: never 0xFF (tick) */
  }
  hookCyc += DWT->CYCCNT - now;
  unlock(pm);
}

void rtp_on_switch_in(uint32_t tag)
{
  const uint32_t pm = lock();
  const uint32_t now = DWT->CYCCNT;
  if (lastOutTag != NO_TAG) { kernelCyc += now - lastOutCyc; }
  if (tag != lastOutTag) { switches++; }
  curTag = tag;
  sliceStart = now;
  if (valid(tag)) { task[tag].everRan = 1U; task[tag].starvedSent = 0U; }
  tr_put(now, (uint8_t)(tag & HIL_TR_TASK_MAX));
  hookCyc += DWT->CYCCNT - now;
  unlock(pm);
}

/* blocked = 1 when the task leaves the CPU because it waits (not a preemption). */
void rtp_on_switch_out(uint32_t tag, uint32_t blocked)
{
  const uint32_t pm = lock();
  const uint32_t now = DWT->CYCCNT;
  if (valid(tag) && (tag == curTag))
  {
    RtpTask_t *t = &task[tag];
    t->run += now - sliceStart;
    if ((blocked != 0U) && (t->open != 0U) && ((t->flags & HIL_TF_IDLE) == 0U))
    {
      t->open = 0U;
      if (t->releases > 1U)                       /* the first job (creation) is not a normal one */
      {
        const uint32_t c = (uint32_t)(t->run - t->runAtRelease);
        const uint32_t r = now - t->release;
        if (c > t->cMax) { t->cMax = c; }
        if (r > t->rMax) { t->rMax = r; }
        if (r < t->rMin) { t->rMin = r; }
        t->cSum += c;
        t->jobs++;
        t->jobsWin++;
        const uint32_t d = (t->deadlineCyc != 0U) ? t->deadlineCyc : ((t->intervals >= 4U) ? t->tAvg : 0U);
        if ((d != 0U) && (r > d)) { t->misses++; }
        if (c >= hogCyc) { ev_put(HIL_EVT_CPU_HOG, (int32_t)(tag | ((c / (1000U * cyclesPerUs)) << 8))); }
      }
    }
  }
  curTag = NO_TAG;
  lastOutTag = tag;
  lastOutCyc = now;
  hookCyc += DWT->CYCCNT - now;
  unlock(pm);
}

void rtp_on_tick(uint32_t tickBeforeIncrement)
{
  const uint32_t pm = lock();
  const uint32_t now = DWT->CYCCNT;
  curTick = tickBeforeIncrement + 1U;
  tr_put(now, (uint8_t)HIL_TR_TICK);
  hookCyc += DWT->CYCCNT - now;
  unlock(pm);
}

void rtp_on_inherit(uint32_t holderTag, uint32_t newPrio, uint32_t waiterTag)
{
  if (!valid(holderTag)) { return; }
  const uint32_t pm = lock();
  if (task[holderTag].inheritStart == 0U) { task[holderTag].inheritStart = DWT->CYCCNT | 1U; }
  ev_put(HIL_EVT_PRIO_INHERIT, (int32_t)(holderTag | ((newPrio & 0xFFU) << 8) | ((waiterTag & 0xFFU) << 16)));
  unlock(pm);
}

void rtp_on_disinherit(uint32_t holderTag, uint32_t basePrio)
{
  (void)basePrio;
  if (!valid(holderTag)) { return; }
  const uint32_t pm = lock();
  RtpTask_t *t = &task[holderTag];
  if (t->inheritStart != 0U)
  {
    const uint32_t us = (DWT->CYCCNT - t->inheritStart) / cyclesPerUs;
    ev_put(HIL_EVT_PRIO_RESTORE, (int32_t)(holderTag | ((us > 0xFFFFFFU ? 0xFFFFFFU : us) << 8)));
    t->inheritStart = 0U;
  }
  unlock(pm);
}

void rtp_on_queue_send(uint32_t qid, uint32_t fill, uint32_t length)
{
  if ((qid == 0U) || (qid >= RTP_MAX_QUEUES)) { return; }
  if (fill > length) { fill = length; }
  if (fill > queue[qid].maxFill) { queue[qid].maxFill = (uint16_t)fill; }
}

void rtp_on_queue_full(uint32_t qid)
{
  if ((qid == 0U) || (qid >= RTP_MAX_QUEUES)) { return; }
  queue[qid].failures++;
}

/* ---- Consumer side ------------------------------------------------------------- */
uint32_t rtp_trace_read(uint32_t *cyc, uint8_t *id, uint32_t max)
{
  uint32_t n = 0U, t = trTail;
  const uint32_t h = trHead;
  while ((t != h) && (n < max))
  {
    cyc[n] = trCyc[t & (RTP_TRACE_LEN - 1U)];
    id[n]  = trId[t & (RTP_TRACE_LEN - 1U)];
    t++;
    n++;
  }
  trTail = t;
  return n;
}

uint32_t rtp_trace_dropped(void) { return trDropped; }

int rtp_event_pop(uint32_t *tMs, uint8_t *code, int32_t *arg)
{
  const uint32_t pm = lock();
  int got = 0;
  if (evTail != evHead)
  {
    const uint32_t i = evTail & (RTP_EVT_LEN - 1U);
    *tMs = evT[i]; *code = evCode[i]; *arg = evArg[i];
    evTail++;
    got = 1;
  }
  unlock(pm);
  return got;
}

/* Once per second, from a task: CPU shares, then the detectors. */
void rtp_second(void)
{
  uint64_t run[RTP_MAX_TASKS];
  const uint32_t pm = lock();
  const uint32_t now = DWT->CYCCNT;
  for (uint32_t i = 0U; i < RTP_MAX_TASKS; i++)
  {
    run[i] = task[i].run + ((i == curTag) ? (uint64_t)(now - sliceStart) : 0U);
  }
  const uint32_t sw = switches, ev = trEvents;
  const uint64_t kc = kernelCyc, hc = hookCyc;
  unlock(pm);

  const uint32_t elapsed = now - secStart;
  secStart = now;
  if (elapsed == 0U) { return; }

  idlePm = 0U;
  for (uint32_t i = 1U; i < RTP_MAX_TASKS; i++)
  {
    RtpTask_t *t = &task[i];
    if (t->name == NULL) { continue; }
    t->cpuPm = (uint16_t)(((run[i] - t->runPrev) * 1000U) / elapsed);
    t->runPrev = run[i];
    t->jobsLast = (uint16_t)t->jobsWin;
    t->jobsWin = 0U;
    if ((t->flags & HIL_TF_IDLE) != 0U) { idlePm = t->cpuPm; }

    if (t->misses != t->missesReported)                     /* deadline misses, once per second */
    {
      const uint32_t n = t->misses - t->missesReported;
      t->missesReported = t->misses;
      rtp_event(HIL_EVT_DEADLINE_MISS, (int32_t)(i | ((n > 0xFFFFU ? 0xFFFFU : n) << 8)));
    }

    t->stackFree = (uint16_t)uxTaskGetStackHighWaterMark(t->handle);
    if ((t->stackLowSent == 0U) && ((uint32_t)t->stackFree * 100U < (uint32_t)t->stackWords * RTP_STACK_LOW_PCT))
    {
      t->stackLowSent = 1U;
      rtp_event(HIL_EVT_STACK_LOW, (int32_t)(i | ((uint32_t)t->stackFree << 8)));
    }

    if ((t->starvedSent == 0U) && (t->everRan != 0U) && (t->cpuPm == 0U) &&
        ((t->flags & HIL_TF_IDLE) == 0U) && (eTaskGetState(t->handle) == eReady))
    {
      t->starvedSent = 1U;                                  /* re-armed when the task runs again */
      rtp_event(HIL_EVT_STARVED, (int32_t)i);
    }
  }

  for (uint32_t q = 1U; q < RTP_MAX_QUEUES; q++)
  {
    RtpQueue_t *k = &queue[q];
    if ((k->name != NULL) && (k->failures != k->failuresReported))
    {
      const uint32_t n = k->failures - k->failuresReported;
      k->failuresReported = k->failures;
      rtp_event(HIL_EVT_QUEUE_FULL, (int32_t)(q | ((n > 0xFFFFU ? 0xFFFFU : n) << 8)));
    }
  }

  const size_t heapMin = xPortGetMinimumEverFreeHeapSize();
  if ((heapLowSent == 0U) && (heapMin * 100U < (size_t)configTOTAL_HEAP_SIZE * RTP_HEAP_LOW_PCT))
  {
    heapLowSent = 1U;
    rtp_event(HIL_EVT_HEAP_LOW, (int32_t)heapMin);
  }

  switchesLast = sw - switchesPrev;  switchesPrev = sw;
  eventsLast = ev - eventsPrev;      eventsPrev = ev;
  probePm  = (uint16_t)(((hc - hookPrev) * 1000U) / elapsed);    hookPrev = hc;
  kernelPm = (uint16_t)(((kc - kernelPrev) * 1000U) / elapsed);  kernelPrev = kc;
}

int rtp_task_info(uint8_t tag, RtpTaskInfo_t *info)
{
  if (!valid(tag)) { return 0; }
  const RtpTask_t *t = &task[tag];
  info->name = t->name;
  info->handle = t->handle;
  info->basePrio = t->basePrio;
  info->flags = t->flags;
  info->stackWords = t->stackWords;
  info->deadlineMs = t->deadlineMs;
  return 1;
}

void rtp_task_stats(uint8_t tag, RtpTaskStats_t *s)
{
  const RtpTask_t *t = &task[tag];
  s->state = (uint8_t)eTaskGetState(t->handle);
  s->prio = (uint8_t)uxTaskPriorityGet(t->handle);
  s->cpuPermille = t->cpuPm;
  s->jobs = t->jobsLast;
  s->cMax = t->cMax;
  s->cAvg = (t->jobs != 0U) ? (uint32_t)(t->cSum / t->jobs) : 0U;
  s->rMax = t->rMax;
  s->rMin = (t->rMin == UINT32_MAX) ? 0U : t->rMin;
  s->tAvg = t->tAvg;
  s->tMin = (t->tMin == UINT32_MAX) ? 0U : t->tMin;
  s->tMax = t->tMax;
  s->misses = (uint16_t)((t->misses > 0xFFFFU) ? 0xFFFFU : t->misses);
  s->stackFree = t->stackFree;
}

void rtp_sys_stats(RtpSysStats_t *s)
{
  s->switches = switchesLast;
  s->events = eventsLast;
  s->dropped = trDropped;
  s->idlePermille = idlePm;
  s->probePermille = probePm;
  s->kernelPermille = kernelPm;
}

int rtp_queue_stats(uint8_t qid, RtpQueueStats_t *s)
{
  if ((qid == 0U) || (qid >= RTP_MAX_QUEUES) || (queue[qid].name == NULL)) { return 0; }
  const RtpQueue_t *k = &queue[qid];
  s->name = k->name;
  s->length = k->length;
  s->waiting = (uint16_t)uxQueueMessagesWaiting(k->handle);
  s->maxFill = k->maxFill;
  s->failures = k->failures;
  return 1;
}
