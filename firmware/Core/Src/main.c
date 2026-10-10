/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "FreeRTOS.h"
#include "task.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
typedef StaticTask_t osStaticThreadDef_t;
/* USER CODE BEGIN PTD */
typedef struct
{
  size_t      heapFree;         /* xPortGetFreeHeapSize()                      */
  UBaseType_t stackFreeA;       /* minimum free stack ever seen, in words      */
  UBaseType_t stackFreeB;
  UBaseType_t stackFreeHealth;
  uint32_t    checks;           /* incremented once per health check           */
} HealthStatus_t;

typedef struct
{
  uint32_t last;                /* last sample, in CPU cycles                  */
  uint32_t min;                 /* best case since reset                       */
  uint32_t max;                 /* worst case since reset                      */
  uint32_t avg;                 /* running average                             */
  uint32_t count;               /* number of samples                           */
  uint64_t sum;
} CycleStats_t;

typedef struct
{
  uint32_t blocks;              /* half-buffers processed                      */
  uint32_t lastRaw;             /* average of the last block, 0..4095          */
  uint32_t lastMv;              /* same, in millivolts (nominal 3300 mV ref)   */
  uint32_t lateBlocks;          /* both halves ready at once: task too late    */
  uint32_t overruns;            /* ADC overrun events                          */
} AcqStatus_t;

/* Encoder input (TIM2, x4 quadrature decoding). The count is compared with
   a CPU-cycle stamp taken at the same instant, so the ratio counts/cycles
   measures the generator frequency against the STM32 clock (HSE). */
typedef struct
{
  uint32_t lastCnt;             /* TIM2->CNT at the previous window            */
  uint32_t lastCyc;             /* DWT->CYCCNT at the previous window          */
  uint32_t warmup;              /* windows to discard after start              */
  int32_t  delta;               /* encoder counts in the last 1 ms window      */
  int32_t  dmin;
  int32_t  dmax;
  uint32_t windows;             /* valid windows since warm-up                 */
  int64_t  total;               /* encoder counts since warm-up                */
  uint64_t cycles;              /* CPU cycles elapsed since warm-up            */

  /* Speed: sliding sum over the last ENC_WIN windows, counts / real cycles. */
  int32_t  winD[16];            /* counts of each window (ENC_WIN entries)     */
  uint32_t winDc[16];           /* CPU cycles of each window                   */
  uint32_t winIdx;              /* slot to overwrite next                      */
  uint32_t winN;                /* valid slots, grows to ENC_WIN               */
  int32_t  sumD;                /* sum of winD                                 */
  uint32_t sumDc;               /* sum of winDc                                */
  int32_t  rpm10;               /* speed in 0.1 rpm, signed (sign = direction) */
} EncStats_t;

/* Speed control loop (PI, 1 kHz, single-precision float on the FPU). */
typedef struct
{
  float    spRpm;               /* setpoint                                    */
  float    rpm;                 /* measured speed                              */
  float    integ;               /* integral term, in duty units (0..1)         */
  float    duty;                /* output, 0..1                                */
  uint32_t ticks;               /* control steps since start                   */
  uint32_t saturated;           /* steps where the output was clamped          */
} CtlState_t;

/* One-shot recording of the first setpoint step: 1 sample per ms. Results are
   computed on the target right after the capture, so they can be read in Live
   Expressions and compared with the offline simulation. */
typedef struct
{
  int16_t  sp[256 * 2];         /* setpoint, rpm (REC_LEN entries)             */
  int16_t  rpm10[256 * 2];      /* measured speed, 0.1 rpm                     */
  uint16_t duty[256 * 2];       /* TIM1 compare value, 0..4000                 */
  uint32_t n;                   /* samples recorded so far                     */
  uint32_t done;                /* 1 when the capture is complete              */
  float    riseMs;              /* 10 % -> 90 % of the final value             */
  float    overshootPct;        /* peak above the target, in % (<= 0: none)    */
  float    settleMs;            /* time after which the speed stays within 2 % */
  float    finalRpm;            /* mean over the last 32 samples               */
} CtlRec_t;

/* Supervisor link (USART1, TX by DMA). Observe-only: nothing received from the
   supervisor ever reaches the control loop. */
typedef struct
{
  uint32_t sent;                /* lines handed to the DMA                     */
  uint32_t busy;                /* previous line still on the wire: skipped    */
  uint32_t timeouts;            /* DMA transfer-complete not seen in time      */
  uint32_t errors;              /* UART error callbacks                        */
} LinkStats_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define TASK_A_PERIOD_MS      10U
#define TASK_A_WORK_US        2000U
#define TASK_A_PRIORITY       20U

#define TASK_B_PERIOD_MS      25U
#define TASK_B_WORK_US        5000U
#define TASK_B_PRIORITY       16U

#define TASK_STACK_WORDS      256U

#define HEALTH_PERIOD_MS      1000U

/* Context-switch measurement: a low-priority sender notifies a
   high-priority receiver; the receiver measures the delay with DWT. */
#define CTX_RX_PRIORITY       24U   /* highest: wakes immediately           */
#define CTX_TX_PRIORITY       12U   /* below A and B                        */
#define CTX_TX_PERIOD_MS      5U

/* Setpoint acquisition: TIM3 (10 kHz) -> ADC1 -> circular DMA, 2 x 10 samples.
   Each half is ready every 1 ms and processed by the acquisition task. */
#define ADC_BUF_LEN           20U
#define ADC_HALF_LEN          (ADC_BUF_LEN / 2U)
#define ACQ_PRIORITY          22U   /* above A and B, below ctxRx           */
#define ACQ_BIT_HALF          (1UL << 0)
#define ACQ_BIT_FULL          (1UL << 1)

/* Speed estimate: encoder lines per revolution (x4 gives 4 counts per line)
   and the number of 1 ms windows in the sliding sum. */
#define ENC_LINES             500U
#define ENC_WIN               16U

_Static_assert(ENC_WIN == sizeof(((EncStats_t *)0)->winD) / sizeof(int32_t),
               "EncStats_t.winD/winDc size must match ENC_WIN");

/* Speed control (phase 4). Gains from the offline simulation (plant gain
   K = 3000 rpm per 100 % duty, time constant 100 ms; zero cancels the pole,
   Ti = tau, closed loop about 3 times faster than the motor). */
#define CTL_KP                0.001f    /* duty per rpm                       */
#define CTL_KI                0.010f    /* duty per (rpm * s)                 */
#define CTL_DT_S              0.001f    /* one control step per 1 ms block    */
#define CTL_STEP_RPM          1500.0f   /* software setpoint: square wave     */
#define CTL_HOLD_MS           1000U     /* setpoint 0 after reset: safe start */
#define CTL_HALF_PERIOD_MS    2000U     /* 2 s at the setpoint, 2 s at 0      */
#define REC_LEN               512U
_Static_assert(REC_LEN == sizeof(((CtlRec_t *)0)->sp) / sizeof(int16_t),
               "CtlRec_t arrays must have REC_LEN entries");

/* Supervisor link: one text line every LINK_PERIOD_MS, sent by DMA from a task
   below every control task. About 40 bytes = 3.5 ms on the wire at 115200. */
#define LINK_PERIOD_MS        100U
#define LINK_PRIORITY         10U   /* above the health task, below all control tasks */
#define LINK_BUF_LEN          64U

/* Fail-stop self-test, run from the acquisition task 5 s after start.
   0 = off (reference build), 1 = call the CSS callback, 2 = call Error_Handler,
   3 = software NMI (goes through NMI_Handler).
   Must be 0 in the reference build. */
#define CSS_SELFTEST          0
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2;
TIM_HandleTypeDef htim3;

UART_HandleTypeDef huart1;
DMA_HandleTypeDef hdma_usart1_tx;

/* Definitions for healthTask */
osThreadId_t healthTaskHandle;
uint32_t healthTaskBuffer[ 256 ];
osStaticThreadDef_t healthTaskControlBlock;
const osThreadAttr_t healthTask_attributes = {
  .name = "healthTask",
  .cb_mem = &healthTaskControlBlock,
  .cb_size = sizeof(healthTaskControlBlock),
  .stack_mem = &healthTaskBuffer[0],
  .stack_size = sizeof(healthTaskBuffer),
  .priority = (osPriority_t) osPriorityLow,
};
/* USER CODE BEGIN PV */
static StaticTask_t taskA_TCB;
static StackType_t  taskA_Stack[TASK_STACK_WORDS];
static StaticTask_t taskB_TCB;
static StackType_t  taskB_Stack[TASK_STACK_WORDS];

static TaskHandle_t hTaskA = NULL;
static TaskHandle_t hTaskB = NULL;

static StaticTask_t ctxRx_TCB;
static StackType_t  ctxRx_Stack[TASK_STACK_WORDS];
static StaticTask_t ctxTx_TCB;
static StackType_t  ctxTx_Stack[TASK_STACK_WORDS];

static TaskHandle_t hCtxRx = NULL;
static TaskHandle_t hCtxTx = NULL;

static uint16_t     adcBuf[ADC_BUF_LEN];     /* written by DMA only          */
static StaticTask_t acq_TCB;
static StackType_t  acq_Stack[TASK_STACK_WORDS];
static TaskHandle_t hAcq = NULL;

static StaticTask_t link_TCB;
static StackType_t  link_Stack[TASK_STACK_WORDS];
static TaskHandle_t hLink = NULL;
static uint8_t      linkBuf[LINK_BUF_LEN];   /* read by the DMA during a transfer */

/* Globals (not static) so they are easy to watch in Live Expressions */
volatile HealthStatus_t g_health;
volatile CycleStats_t   g_ctxSwitch = { .min = UINT32_MAX };
volatile uint32_t       g_ctxT0;      /* timestamp written by the sender  */

volatile AcqStatus_t    g_acq;
volatile CycleStats_t   g_isrToTask = { .min = UINT32_MAX };
volatile uint32_t       g_isrT0;      /* timestamp written by the DMA ISR */

volatile EncStats_t     g_enc = { .warmup = 10U, .dmin = INT32_MAX, .dmax = INT32_MIN };
volatile CtlState_t     g_ctl;
volatile CtlRec_t       g_rec;
volatile LinkStats_t    g_link;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM3_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART1_UART_Init(void);
void StartHealthTask(void *argument);

/* USER CODE BEGIN PFP */
void Safe_State(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
static void dwt_init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  /* enable the trace block    */
  DWT->CYCCNT = 0U;
  DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;            /* start the cycle counter  */
}

static void busy_work_us(uint32_t us)
{
  const uint32_t start  = DWT->CYCCNT;
  const uint32_t cycles = us * (SystemCoreClock / 1000000U);
  while ((DWT->CYCCNT - start) < cycles) { }       /* wrap-safe (unsigned)     */
}

static void TaskA(void *arg)
{
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(TASK_A_PERIOD_MS);
  TickType_t lastWake = xTaskGetTickCount();

  for (;;)
  {
    vTaskDelayUntil(&lastWake, period);
    HAL_GPIO_WritePin(TP_TASK_A_GPIO_Port, TP_TASK_A_Pin, GPIO_PIN_SET);
    busy_work_us(TASK_A_WORK_US);
    HAL_GPIO_WritePin(TP_TASK_A_GPIO_Port, TP_TASK_A_Pin, GPIO_PIN_RESET);
  }
}

static void TaskB(void *arg)
{
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(TASK_B_PERIOD_MS);
  TickType_t lastWake = xTaskGetTickCount();

  for (;;)
  {
    vTaskDelayUntil(&lastWake, period);
    HAL_GPIO_WritePin(TP_TASK_B_GPIO_Port, TP_TASK_B_Pin, GPIO_PIN_SET);
    busy_work_us(TASK_B_WORK_US);
    HAL_GPIO_WritePin(TP_TASK_B_GPIO_Port, TP_TASK_B_Pin, GPIO_PIN_RESET);
  }
}

static void stats_add(volatile CycleStats_t *s, uint32_t sample)
{
  s->last = sample;
  if (sample < s->min) { s->min = sample; }
  if (sample > s->max) { s->max = sample; }
  s->sum += sample;
  s->count++;
  s->avg = (uint32_t)(s->sum / s->count);
}

/* Receiver: highest priority, blocked until notified. */
static void CtxRxTask(void *arg)
{
  (void)arg;
  for (;;)
  {
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uint32_t dt = DWT->CYCCNT - g_ctxT0;
    stats_add(&g_ctxSwitch, dt);
  }
}

/* Sender: low priority, timestamps then notifies the receiver.
   The receiver preempts it immediately, so dt covers:
   notify call + PendSV + context save/restore + return from NotifyTake. */
static void CtxTxTask(void *arg)
{
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(CTX_TX_PERIOD_MS);
  TickType_t lastWake = xTaskGetTickCount();

  for (;;)
  {
    vTaskDelayUntil(&lastWake, period);
    g_ctxT0 = DWT->CYCCNT;
    xTaskNotifyGive(hCtxRx);
  }
}

/* Called once per acquisition window (1 ms). Both readings are taken
   back-to-back by the caller, so the count and the cycle stamp describe
   the same instant. Unsigned subtraction keeps it wrap-safe. */
static void enc_update(uint32_t cyc, uint32_t cnt)
{
  const int32_t  d  = (int32_t)(cnt - g_enc.lastCnt);
  const uint32_t dc = cyc - g_enc.lastCyc;
  g_enc.lastCnt = cnt;
  g_enc.lastCyc = cyc;

  if (g_enc.warmup > 0U) { g_enc.warmup--; return; }   /* first windows not valid */

  g_enc.delta = d;
  if (d < g_enc.dmin) { g_enc.dmin = d; }
  if (d > g_enc.dmax) { g_enc.dmax = d; }
  g_enc.total  += d;
  g_enc.cycles += dc;
  g_enc.windows++;

  /* Sliding sum: drop the oldest window once the buffer is full, add the new one. */
  const uint32_t i = g_enc.winIdx;
  if (g_enc.winN == ENC_WIN)
  {
    g_enc.sumD  -= g_enc.winD[i];
    g_enc.sumDc -= g_enc.winDc[i];
  }
  else
  {
    g_enc.winN++;
  }
  g_enc.winD[i]  = d;
  g_enc.winDc[i] = dc;
  g_enc.sumD    += d;
  g_enc.sumDc   += dc;
  g_enc.winIdx   = (i + 1U) % ENC_WIN;

  /* rpm = sumD / (4 * LINES) / (sumDc / f_cpu) * 60 ; in 0.1 rpm: x150.
     Numerator reaches ~4e13 at 6000 rpm, so the maths is done in 64 bits. */
  if (g_enc.sumDc != 0U)
  {
    const int64_t num = (int64_t)g_enc.sumD * (int64_t)SystemCoreClock * 150;
    const int64_t den = (int64_t)ENC_LINES * (int64_t)g_enc.sumDc;
    g_enc.rpm10 = (int32_t)((num >= 0 ? num + den / 2 : num - den / 2) / den);
  }
}

/* Software setpoint: 0 during CTL_HOLD_MS after reset (the motor never starts
   by itself), then a square wave between CTL_STEP_RPM and 0. */
static float ctl_setpoint(uint32_t ms)
{
  if (ms < CTL_HOLD_MS) { return 0.0f; }
  return ((((ms - CTL_HOLD_MS) / CTL_HALF_PERIOD_MS) & 1U) == 0U) ? CTL_STEP_RPM : 0.0f;
}

/* Step-response figures of the recording, computed once on the target. */
static void rec_analyse(void)
{
  uint32_t i10 = 0U, i90 = 0U, settle = 0U;
  uint32_t found10 = 0U, found90 = 0U;
  float peak = 0.0f, tail = 0.0f;

  for (uint32_t i = 0U; i < REC_LEN; i++)
  {
    const float r = (float)g_rec.rpm10[i] * 0.1f;
    if ((found10 == 0U) && (r > 0.1f * CTL_STEP_RPM)) { i10 = i; found10 = 1U; }
    if ((found90 == 0U) && (r > 0.9f * CTL_STEP_RPM)) { i90 = i; found90 = 1U; }
    if (r > peak) { peak = r; }

    float err = r - CTL_STEP_RPM;
    if (err < 0.0f) { err = -err; }
    if (err > 0.02f * CTL_STEP_RPM) { settle = i + 1U; }
    if (i >= (REC_LEN - 32U)) { tail += r; }
  }

  g_rec.riseMs       = (found90 != 0U) ? (float)(i90 - i10) : -1.0f;
  g_rec.overshootPct = (peak / CTL_STEP_RPM - 1.0f) * 100.0f;
  g_rec.settleMs     = (float)settle;
  g_rec.finalRpm     = tail / 32.0f;
  g_rec.done         = 1U;
}

/* One PI step per 1 ms block, right after enc_update(). Anti-windup by
   conditional integration: the integral stops growing while the output is
   clamped and the error would push it further into the clamp. */
static void ctl_update(uint32_t ms)
{
  const float sp  = ctl_setpoint(ms);
  const float rpm = (float)g_enc.rpm10 * 0.1f;
  float u = 0.0f;

  if (g_enc.winN == ENC_WIN)                 /* speed estimate fully valid */
  {
    const float e    = sp - rpm;
    const float uRaw = CTL_KP * e + g_ctl.integ;

    u = uRaw;
    if (u > 1.0f)      { u = 1.0f; }
    else if (u < 0.0f) { u = 0.0f; }

    if ((u == uRaw) || ((uRaw > 1.0f) && (e < 0.0f)) || ((uRaw < 0.0f) && (e > 0.0f)))
    {
      g_ctl.integ += CTL_KI * e * CTL_DT_S;
    }
    else
    {
      g_ctl.saturated++;
    }
  }
  else
  {
    g_ctl.integ = 0.0f;                      /* no valid measurement: output stays 0 */
  }

  const uint32_t cmp = (uint32_t)(u * (float)(__HAL_TIM_GET_AUTORELOAD(&htim1) + 1U));
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, cmp);

  g_ctl.spRpm = sp;
  g_ctl.rpm   = rpm;
  g_ctl.duty  = u;
  g_ctl.ticks++;

  /* One-shot capture, starting at the first sample where the setpoint is non-zero. */
  if ((g_rec.done == 0U) && (sp > 0.0f) && (g_rec.n < REC_LEN))
  {
    int32_t r10 = g_enc.rpm10;
    if (r10 > 32767) { r10 = 32767; }
    if (r10 < -32768) { r10 = -32768; }
    g_rec.sp[g_rec.n]    = (int16_t)sp;
    g_rec.rpm10[g_rec.n] = (int16_t)r10;
    g_rec.duty[g_rec.n]  = (uint16_t)cmp;
    g_rec.n++;
    if (g_rec.n == REC_LEN) { rec_analyse(); }
  }
}

/* Text formatting by hand: no printf/snprintf from a task (project rule). */
static char *put_str(char *p, const char *s)
{
  while (*s != '\0') { *p++ = *s++; }
  return p;
}

static char *put_i32(char *p, int32_t v)
{
  char tmp[11];
  uint32_t n = 0U;
  uint32_t u;

  if (v < 0) { *p++ = '-'; u = (uint32_t)(-(v + 1)) + 1U; }
  else       { u = (uint32_t)v; }

  do { tmp[n++] = (char)('0' + (u % 10U)); u /= 10U; } while (u != 0U);
  while (n > 0U) { *p++ = tmp[--n]; }
  return p;
}

/* "t=<ms> sp=<rpm> rpm10=<0.1 rpm> duty=<per mille>\r\n" */
static uint32_t link_format(uint8_t *buf)
{
  char *p = (char *)buf;
  p = put_str(p, "t=");      p = put_i32(p, (int32_t)g_acq.blocks);
  p = put_str(p, " sp=");    p = put_i32(p, (int32_t)g_ctl.spRpm);
  p = put_str(p, " rpm10="); p = put_i32(p, g_enc.rpm10);
  p = put_str(p, " duty=");  p = put_i32(p, (int32_t)(g_ctl.duty * 1000.0f));
  p = put_str(p, "\r\n");
  return (uint32_t)((uint8_t *)p - buf);
}

/* Lowest-priority real-time job: it may be late, it must never delay control. */
static void LinkTask(void *arg)
{
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(LINK_PERIOD_MS);
  TickType_t lastWake = xTaskGetTickCount();

  for (;;)
  {
    vTaskDelayUntil(&lastWake, period);

    (void)ulTaskNotifyTake(pdTRUE, 0U);                 /* drop a stale completion */
    const uint32_t n = link_format(linkBuf);
    if (HAL_UART_Transmit_DMA(&huart1, linkBuf, (uint16_t)n) == HAL_OK)
    {
      g_link.sent++;
      if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20U)) == 0U)
      {
        g_link.timeouts++;
      }
    }
    else
    {
      g_link.busy++;
    }
  }
}

/* Acquisition task: woken by the DMA half/full-transfer interrupt.
   It starts the hardware itself, so that no interrupt can ever try to
   notify a task that does not exist yet. */
static void AcqTask(void *arg)
{
  (void)arg;

  /* Debug halt: stop TIM3 (no acquisition while the core is frozen) and
     TIM1 (motor PWM must not keep driving while nobody is in control). */
  __HAL_DBGMCU_FREEZE_TIM3();
  __HAL_DBGMCU_FREEZE_TIM1();
  __HAL_DBGMCU_FREEZE_TIM2();   /* keeps encoder counts consistent with DWT, which also stops */

  /* Motor PWM: start at 0 % duty. HAL_TIM_PWM_Start also sets MOE,
     the main output enable of the advanced timer. */
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, 0U);
  if (HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_TIM_Encoder_Start(&htim2, TIM_CHANNEL_ALL) != HAL_OK)
  {
    Error_Handler();
  }

  if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adcBuf, ADC_BUF_LEN) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_Base_Start(&htim3) != HAL_OK)
  {
    Error_Handler();
  }

  uint32_t warmup = 10U;        /* ignore the first wake-ups (cold cache) */

  for (;;)
  {
    uint32_t bits = 0U;
    (void)xTaskNotifyWait(0U, ACQ_BIT_HALF | ACQ_BIT_FULL, &bits, portMAX_DELAY);

    const uint32_t dt = DWT->CYCCNT - g_isrT0;
    if (warmup > 0U)
    {
      warmup--;
    }
    else
    {
      stats_add(&g_isrToTask, dt);
    }

    /* Encoder: count and cycle stamp read back-to-back (see enc_update). */
    const uint32_t encCyc = DWT->CYCCNT;
    const uint32_t encCnt = __HAL_TIM_GET_COUNTER(&htim2);
    enc_update(encCyc, encCnt);

#if CSS_SELFTEST
    if (g_acq.blocks == 5000U)                               /* 5 s after start */
    {
#if CSS_SELFTEST == 1
      HAL_RCC_CSSCallback();
#elif CSS_SELFTEST == 2
      Error_Handler();
#else
      SCB->ICSR = SCB_ICSR_NMIPENDSET_Msk;   /* software NMI: runs NMI_Handler */
#endif
    }
#endif

    HAL_GPIO_WritePin(TP_TASK_C_GPIO_Port, TP_TASK_C_Pin, GPIO_PIN_SET);

    if (((bits & ACQ_BIT_HALF) != 0U) && ((bits & ACQ_BIT_FULL) != 0U))
    {
      g_acq.lateBlocks++;   /* one block was overwritten before being read */
    }

    /* Process the half the DMA has just completed; it is filling the other. */
    const uint16_t *block = ((bits & ACQ_BIT_FULL) != 0U) ? &adcBuf[ADC_HALF_LEN]
                                                          : &adcBuf[0];
    uint32_t sum = 0U;
    for (uint32_t i = 0U; i < ADC_HALF_LEN; i++)
    {
      sum += block[i];
    }
    const uint32_t avg = sum / ADC_HALF_LEN;

    g_acq.lastRaw = avg;
    g_acq.lastMv  = (avg * 3300U) / 4095U;
    g_acq.blocks++;

    /* The pot (g_acq.lastRaw) is still sampled but no longer drives the motor:
       the setpoint is a software square wave for now (see ctl_setpoint). */
    ctl_update(g_acq.blocks);

    HAL_GPIO_WritePin(TP_TASK_C_GPIO_Port, TP_TASK_C_Pin, GPIO_PIN_RESET);
  }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_TIM3_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_USART1_UART_Init();
  /* USER CODE BEGIN 2 */
  dwt_init();

  /* Calibrate the ADC once, while it is still disabled. */
  if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of healthTask */
  healthTaskHandle = osThreadNew(StartHealthTask, NULL, &healthTask_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  configASSERT(healthTaskHandle != NULL);

  hTaskA = xTaskCreateStatic(TaskA, "taskA", TASK_STACK_WORDS, NULL,
                             TASK_A_PRIORITY, taskA_Stack, &taskA_TCB);
  hTaskB = xTaskCreateStatic(TaskB, "taskB", TASK_STACK_WORDS, NULL,
                             TASK_B_PRIORITY, taskB_Stack, &taskB_TCB);
  configASSERT(hTaskA != NULL);
  configASSERT(hTaskB != NULL);

  hCtxRx = xTaskCreateStatic(CtxRxTask, "ctxRx", TASK_STACK_WORDS, NULL,
                             CTX_RX_PRIORITY, ctxRx_Stack, &ctxRx_TCB);
  hCtxTx = xTaskCreateStatic(CtxTxTask, "ctxTx", TASK_STACK_WORDS, NULL,
                             CTX_TX_PRIORITY, ctxTx_Stack, &ctxTx_TCB);
  configASSERT(hCtxRx != NULL);
  configASSERT(hCtxTx != NULL);

  hAcq = xTaskCreateStatic(AcqTask, "acq", TASK_STACK_WORDS, NULL,
                           ACQ_PRIORITY, acq_Stack, &acq_TCB);
  configASSERT(hAcq != NULL);

  hLink = xTaskCreateStatic(LinkTask, "link", TASK_STACK_WORDS, NULL,
                            LINK_PRIORITY, link_Stack, &link_TCB);
  configASSERT(hLink != NULL);
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 1;
  RCC_OscInitStruct.PLL.PLLN = 20;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV7;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_SYSCLK, RCC_MCODIV_16);

  /** Enables the Clock Security System
  */
  HAL_RCC_EnableCSS();
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_MultiModeTypeDef multimode = {0};
  ADC_ChannelConfTypeDef sConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Common config
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T3_TRGO;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
  hadc1.Init.OversamplingMode = DISABLE;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the ADC multi-mode
  */
  multimode.Mode = ADC_MODE_INDEPENDENT;
  if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_5;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_47CYCLES_5;
  sConfig.SingleDiff = ADC_SINGLE_ENDED;
  sConfig.OffsetNumber = ADC_OFFSET_NONE;
  sConfig.Offset = 0;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 0;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 3999;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_DISABLE;
  sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_DISABLE;
  sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
  sBreakDeadTimeConfig.DeadTime = 0;
  sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
  sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
  sBreakDeadTimeConfig.BreakFilter = 0;
  sBreakDeadTimeConfig.Break2State = TIM_BREAK2_DISABLE;
  sBreakDeadTimeConfig.Break2Polarity = TIM_BREAK2POLARITY_HIGH;
  sBreakDeadTimeConfig.Break2Filter = 0;
  sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
  if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  /* USER CODE END TIM1_Init 2 */
  HAL_TIM_MspPostInit(&htim1);

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_Encoder_InitTypeDef sConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4294967295;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  sConfig.EncoderMode = TIM_ENCODERMODE_TI12;
  sConfig.IC1Polarity = TIM_ICPOLARITY_RISING;
  sConfig.IC1Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC1Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC1Filter = 3;
  sConfig.IC2Polarity = TIM_ICPOLARITY_RISING;
  sConfig.IC2Selection = TIM_ICSELECTION_DIRECTTI;
  sConfig.IC2Prescaler = TIM_ICPSC_DIV1;
  sConfig.IC2Filter = 3;
  if (HAL_TIM_Encoder_Init(&htim2, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief TIM3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM3_Init(void)
{

  /* USER CODE BEGIN TIM3_Init 0 */

  /* USER CODE END TIM3_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM3_Init 1 */

  /* USER CODE END TIM3_Init 1 */
  htim3.Instance = TIM3;
  htim3.Init.Prescaler = 79;
  htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim3.Init.Period = 99;
  htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim3) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim3, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM3_Init 2 */

  /* USER CODE END TIM3_Init 2 */

}

/**
  * @brief USART1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART1_UART_Init(void)
{

  /* USER CODE BEGIN USART1_Init 0 */

  /* USER CODE END USART1_Init 0 */

  /* USER CODE BEGIN USART1_Init 1 */

  /* USER CODE END USART1_Init 1 */
  huart1.Instance = USART1;
  huart1.Init.BaudRate = 115200;
  huart1.Init.WordLength = UART_WORDLENGTH_8B;
  huart1.Init.StopBits = UART_STOPBITS_1;
  huart1.Init.Parity = UART_PARITY_NONE;
  huart1.Init.Mode = UART_MODE_TX_RX;
  huart1.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart1.Init.OverSampling = UART_OVERSAMPLING_16;
  huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART1_Init 2 */

  /* USER CODE END USART1_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
  /* DMA1_Channel4_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel4_IRQn, 7, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel4_IRQn);

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, LED_FAULT_Pin|TP_TASK_A_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, TP_ISR_Pin|TP_TASK_C_Pin|TP_TASK_B_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_FAULT_Pin */
  GPIO_InitStruct.Pin = LED_FAULT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_FAULT_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : TP_ISR_Pin TP_TASK_C_Pin TP_TASK_B_Pin */
  GPIO_InitStruct.Pin = TP_ISR_Pin|TP_TASK_C_Pin|TP_TASK_B_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : PA8 */
  GPIO_InitStruct.Pin = GPIO_PIN_8;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  GPIO_InitStruct.Alternate = GPIO_AF0_MCO;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : TP_TASK_A_Pin */
  GPIO_InitStruct.Pin = TP_TASK_A_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(TP_TASK_A_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
/* Called from the DMA interrupt (priority 6, allowed to use FromISR APIs).
   Kept minimal: timestamp, notify, request a context switch if needed. */
static void acq_notify_from_isr(uint32_t bit)
{
  HAL_GPIO_WritePin(TP_ISR_GPIO_Port, TP_ISR_Pin, GPIO_PIN_SET);
  g_isrT0 = DWT->CYCCNT;

  BaseType_t woken = pdFALSE;
  if (hAcq != NULL)
  {
    (void)xTaskNotifyFromISR(hAcq, bit, eSetBits, &woken);
  }

  HAL_GPIO_WritePin(TP_ISR_GPIO_Port, TP_ISR_Pin, GPIO_PIN_RESET);
  portYIELD_FROM_ISR(woken);
}

void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    acq_notify_from_isr(ACQ_BIT_HALF);
  }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
  if (hadc->Instance == ADC1)
  {
    acq_notify_from_isr(ACQ_BIT_FULL);
  }
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
  if ((hadc->Instance == ADC1) && ((hadc->ErrorCode & HAL_ADC_ERROR_OVR) != 0U))
  {
    g_acq.overruns++;
  }
}

/* USART1 DMA transfer complete (interrupt priority 7, FromISR APIs allowed). */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
  if ((huart->Instance == USART1) && (hLink != NULL))
  {
    BaseType_t woken = pdFALSE;
    vTaskNotifyGiveFromISR(hLink, &woken);
    portYIELD_FROM_ISR(woken);
  }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
  if (huart->Instance == USART1)
  {
    g_link.errors++;
  }
}

/* Safe state: motor PWM output forced low and LD2 lit. Register level, no HAL or RTOS
   call, so it is safe from any context (task, ISR, NMI, fault handler). Disabling
   interrupts does not stop TIM1, a hardware peripheral that would keep driving the
   motor at the last duty cycle: the output itself has to be forced. */
void Safe_State(void)
{
  TIM1->BDTR &= ~TIM_BDTR_MOE;                                                /* timer outputs off          */
  GPIOA->BSRR = (uint32_t)GPIO_PIN_9 << 16U;                                  /* PA9 low first              */
  GPIOA->MODER = (GPIOA->MODER & ~(3UL << (9U * 2U))) | (1UL << (9U * 2U));   /* then PA9 = plain output    */
  LED_FAULT_GPIO_Port->BSRR = LED_FAULT_Pin;                                  /* LD2 on                     */
}

/* Clock Security System: HAL_RCC_NMI_IRQHandler() calls this from the NMI when the
   HSE stops. The hardware has already switched SYSCLK to MSI (4 MHz), so everything
   timed by the 80 MHz clock is wrong: fail-stop. */
void HAL_RCC_CSSCallback(void)
{
  Safe_State();
  __disable_irq();
  while (1) { }
}
/* USER CODE END 4 */

/* USER CODE BEGIN Header_StartHealthTask */
/**
  * @brief  Health monitor: once per HEALTH_PERIOD_MS, records heap usage and
  *         the minimum free stack ever seen by each task.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartHealthTask */
void StartHealthTask(void *argument)
{
  /* USER CODE BEGIN 5 */
  (void)argument;
  const TickType_t period = pdMS_TO_TICKS(HEALTH_PERIOD_MS);
  TickType_t lastWake = xTaskGetTickCount();

  for (;;)
  {
    vTaskDelayUntil(&lastWake, period);
    g_health.heapFree        = xPortGetFreeHeapSize();
    g_health.stackFreeA      = uxTaskGetStackHighWaterMark(hTaskA);
    g_health.stackFreeB      = uxTaskGetStackHighWaterMark(hTaskB);
    g_health.stackFreeHealth = uxTaskGetStackHighWaterMark(NULL);
    g_health.checks++;
  }
  /* USER CODE END 5 */
}

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* Fail-stop: freeze everything, force the motor output low and light LD2 so the
     fault is visible even without a debugger attached. */
  __disable_irq();
  Safe_State();
  while (1) { }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
