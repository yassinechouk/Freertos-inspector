/* Fault lab, see faultlab.h. Every fault is bounded in time and in resources:
   it must be visible to the probe, never fatal to the system. */
#include "faultlab.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include "stm32l4xx.h"
#include "hil_proto.h"
#include "rtos_probe.h"

#define FL_CTL_PRIO     18U     /* above taskB (16), below taskA (20) and acquisition (22) */
#define FL_HIGH_PRIO    21U     /* inversion victim: above taskA, below acquisition */
#define FL_LOW_PRIO      9U     /* mutex holder and queue consumer: low */
#define FL_CTL_STACK   384U     /* words; the stack fault uses about 70 % of it */
#define FL_STACK       192U
#define FL_Q_LEN         8U

#define FL_HOG_MS       150U    /* CPU hog: one job of 150 ms without blocking */
#define FL_HOLD_MS        6U    /* inversion: time the low task holds the mutex */
#define FL_HOLD_WAIT_MS 100U    /* inversion: longest wait for the low task to take the mutex */
#define FL_FLOOD         40U    /* queue: items pushed at once */
#define FL_RX_PERIOD_MS  20U    /* queue: one item consumed every 20 ms */
#define FL_HEAP_HOLD_MS 3000U
#define FL_HEAP_BLOCK   256U

static StaticTask_t ctlTcb, lowTcb, highTcb, rxTcb;
static StackType_t  ctlStack[FL_CTL_STACK], lowStack[FL_STACK], highStack[FL_STACK], rxStack[FL_STACK];
static TaskHandle_t hCtl, hLow, hHigh, hRx;

static StaticQueue_t qBuf;
static uint8_t       qStorage[FL_Q_LEN * sizeof(uint32_t)];
static QueueHandle_t q;
static StaticSemaphore_t mBuf;
static SemaphoreHandle_t mtx;

static volatile uint8_t pending;   /* fault requested, 0 = none */
static volatile uint8_t busy;
static volatile uint8_t lowHolds;  /* inversion: 1 while the low task holds the mutex */

static void busy_wait_ms(uint32_t ms)
{
  const uint32_t start = DWT->CYCCNT;
  const uint32_t cycles = ms * (SystemCoreClock / 1000U);
  while ((DWT->CYCCNT - start) < cycles) { }
}

/* Uses ~1000 bytes of stack once: the high-water mark keeps the trace of it. */
static void __attribute__((noinline)) deep_stack(void)
{
  volatile uint8_t buf[1000];
  for (uint32_t i = 0U; i < sizeof buf; i++) { buf[i] = (uint8_t)i; }
  (void)buf[sizeof buf - 1U];
}

static void heap_pressure(void)
{
  void *blocks[16];
  uint32_t n = 0U;
  const size_t keep = (size_t)configTOTAL_HEAP_SIZE / 4U;      /* stop at about 25 % free */
  size_t freeNow, minEver;
  for (;;)
  {
    rtp_heap(&freeNow, &minEver);                               /* not xPortGetFreeHeapSize(): 0 before the first malloc */
    if ((n >= 16U) || (freeNow <= keep + FL_HEAP_BLOCK + 16U)) { break; }
    blocks[n] = pvPortMalloc(FL_HEAP_BLOCK);
    if (blocks[n] == NULL) { break; }
    n++;
  }
  vTaskDelay(pdMS_TO_TICKS(FL_HEAP_HOLD_MS));
  while (n > 0U) { vPortFree(blocks[--n]); }
}

/* Controller: waits for a request, runs the fault, then is ready for the next one. */
static void CtlTask(void *arg)
{
  (void)arg;
  for (;;)
  {
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    switch (pending)
    {
      case HIL_FAULT_CPU_HOG:
        busy_wait_ms(FL_HOG_MS);                      /* never blocks: lower tasks starve */
        break;
      case HIL_FAULT_STACK:
        deep_stack();
        break;
      case HIL_FAULT_QUEUE:
        for (uint32_t i = 0U; i < FL_FLOOD; i++)
        {
          (void)xQueueSend(q, &i, 0);                 /* no wait: failures when full */
        }
        break;
      case HIL_FAULT_INVERSION:
        xTaskNotifyGive(hLow);                        /* low task takes the mutex... */
        for (uint32_t ms = 0U; (lowHolds == 0U) && (ms < FL_HOLD_WAIT_MS); ms++)
        {
          vTaskDelay(pdMS_TO_TICKS(1));               /* ...when it gets the CPU: at priority 9 under load,
                                                         that took 5.5 ms on the bench, so a fixed 2 ms wait
                                                         let the high task find the mutex free */
        }
        xTaskNotifyGive(hHigh);                       /* high task now needs it */
        vTaskDelay(pdMS_TO_TICKS(20));
        break;
      case HIL_FAULT_HEAP:
        heap_pressure();
        break;
      default:
        break;
    }
    pending = 0U;
    busy = 0U;
  }
}

static void LowTask(void *arg)
{
  (void)arg;
  for (;;)
  {
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (xSemaphoreTake(mtx, portMAX_DELAY) == pdTRUE)
    {
      lowHolds = 1U;
      busy_wait_ms(FL_HOLD_MS);                       /* boosted to 21 while the high task waits */
      lowHolds = 0U;
      (void)xSemaphoreGive(mtx);
    }
  }
}

static void HighTask(void *arg)
{
  (void)arg;
  for (;;)
  {
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (xSemaphoreTake(mtx, portMAX_DELAY) == pdTRUE)
    {
      (void)xSemaphoreGive(mtx);
    }
  }
}

static void RxTask(void *arg)
{
  (void)arg;
  uint32_t item;
  for (;;)
  {
    if (xQueueReceive(q, &item, portMAX_DELAY) == pdTRUE)
    {
      vTaskDelay(pdMS_TO_TICKS(FL_RX_PERIOD_MS));     /* slow consumer */
    }
  }
}

void faultlab_init(void)
{
  q = xQueueCreateStatic(FL_Q_LEN, sizeof(uint32_t), qStorage, &qBuf);
  mtx = xSemaphoreCreateMutexStatic(&mBuf);
  (void)rtp_queue_register(q, "lab_queue");

  hCtl  = xTaskCreateStatic(CtlTask,  "labCtl",  FL_CTL_STACK, NULL, FL_CTL_PRIO,  ctlStack,  &ctlTcb);
  hLow  = xTaskCreateStatic(LowTask,  "labLow",  FL_STACK,     NULL, FL_LOW_PRIO,  lowStack,  &lowTcb);
  hHigh = xTaskCreateStatic(HighTask, "labHigh", FL_STACK,     NULL, FL_HIGH_PRIO, highStack, &highTcb);
  hRx   = xTaskCreateStatic(RxTask,   "labRx",   FL_STACK,     NULL, FL_LOW_PRIO,  rxStack,   &rxTcb);
  configASSERT((hCtl != NULL) && (hLow != NULL) && (hHigh != NULL) && (hRx != NULL));
}

int faultlab_inject(uint8_t fault)
{
  if ((busy != 0U) || (fault < HIL_FAULT_CPU_HOG) || (fault > HIL_FAULT_HEAP)) { return -1; }
  busy = 1U;
  pending = fault;
  rtp_event(HIL_EVT_FAULT_INJECT, (int32_t)fault);
  xTaskNotifyGive(hCtl);
  return 0;
}
