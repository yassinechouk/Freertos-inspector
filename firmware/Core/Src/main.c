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
#include "hil_proto.h"
#include "rtos_probe.h"
#include "faultlab.h"
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
  float    spRamp;              /* setpoint after the slew-rate limiter        */
  uint32_t stallMs;             /* consecutive ms of "driven hard, not moving" */
  uint32_t fault;               /* 0 = none, latched until reset (CTL_FAULT_*) */
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

/* Supervisor link (USART1, hil_proto.h frames, DMA both ways). The supervisor
   may only PROPOSE: modes and setpoints go through the checks in link_handle_frame()
   and the slew limiter in ctl_setpoint(); the control loop never waits for it. */
typedef struct
{
  uint32_t sent;                /* TX cycles handed to the DMA                 */
  uint32_t busy;                /* previous transfer still running: skipped    */
  uint32_t timeouts;            /* DMA transfer-complete not seen in time      */
  uint32_t errors;              /* UART error callbacks                        */
  uint32_t rxFrames;            /* valid frames received                       */
  uint32_t rxCrcErrors;         /* frames rejected by the CRC                  */
  uint32_t rxLost;              /* frames missing, from the sequence numbers   */
  uint32_t rxRestarts;          /* RX path restarted (UART error or silence)   */
  uint32_t rxRestartFails;      /* restart refused by the HAL                  */
  uint32_t cmdRejected;         /* commands refused                            */
} LinkStats_t;

/* Control mode and remote setpoint, written by the link task, read by the control loop. */
typedef struct
{
  uint32_t mode;                /* HIL_MODE_*; IDLE after reset                */
  int32_t  remoteRpm10;         /* requested speed in REMOTE, already clamped  */
  uint32_t lastRxMs;            /* g_acq.blocks at the last valid frame        */
  uint32_t lastSpMs;            /* g_acq.blocks at the last setpoint command   */
  uint32_t spTimedOut;          /* 1 once the REMOTE setpoint has expired      */
  uint32_t linkAlive;           /* 1 while frames from the supervisor arrive   */
  uint32_t modeSinceMs;         /* when the current mode was entered           */
  uint32_t evtPending;          /* bit n set: event code n is to be reported   */
  int32_t  evtArg[8];
} CtlCmd_t;
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
#define CTX_TX_PRIORITY       21U   /* 5 ms period: above A (10 ms) and B (25 ms), rate-monotonic.
                                       Was 12: RTOS Doctor measured ~40 misses/s, R = 7.7 ms > T = 5 ms */
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

/* Stall / encoder-loss protection: if the output is held at or above
   CTL_STALL_DUTY while the measured speed stays within +-CTL_STALL_RPM for
   CTL_STALL_MS, the motor is not following (blocked shaft, broken encoder
   wire, dead plant). The motor output is then forced off (Safe_State) and the
   fault is latched until reset. A healthy start leaves 30 rpm in a few ms. */
#define CTL_STALL_DUTY        0.8f
#define CTL_STALL_RPM         30.0f
#define CTL_STALL_MS          500U
#define CTL_FAULT_NONE        0U
#define CTL_FAULT_STALL       1U
_Static_assert(REC_LEN == sizeof(((CtlRec_t *)0)->sp) / sizeof(int16_t),
               "CtlRec_t arrays must have REC_LEN entries");

/* Supervisor link: one TX cycle every LINK_PERIOD_MS (telemetry every cycle,
   heartbeat every LINK_HEARTBEAT_DIV cycles), sent by DMA from a task below every
   control task. RX is a circular DMA buffer polled by the same task. */
#define LINK_PERIOD_MS        10U
#define LINK_PRIORITY         17U   /* 10 ms period: above taskB (25 ms), rate-monotonic; below the
                                       control loop (and labCtl, so a CPU hog also delays it). Was 10: 6 % margin */
#define LINK_STACK_WORDS      384U  /* 256 measured 75 % used (trace frame buffer on the stack) */
#define LINK_BUF_LEN          1280U
#define LINK_RX_LEN           256U
#define LINK_HEARTBEAT_DIV    10U
#define LINK_DEADMAN_MS       1000U /* REMOTE: no frame for this long -> setpoint ramps to 0 */
#define LINK_MAX_EVENTS       2U    /* events per TX cycle, to bound the buffer */
#define LINK_TRACE_MAX_EVENTS 180U  /* trace events per TX cycle (6 frames); the rest waits in the ring */

#define LINK_PROBE_EVENTS     4U    /* probe events per TX cycle */
#define CTL_DEFAULT_MODE      HIL_MODE_IDLE   /* HIL_MODE_TEST for a bench without supervisor */
#define CTL_RPM_MAX           3000.0f
#define CTL_SLEW_RPM_PER_MS   3.0f          /* 3000 rpm/s on LOCAL and REMOTE setpoints */

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
DMA_HandleTypeDef hdma_usart1_rx;

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
static StackType_t  link_Stack[LINK_STACK_WORDS];
static TaskHandle_t hLink = NULL;
static uint8_t      linkBuf[2][LINK_BUF_LEN]; /* one is built while the DMA sends the other */
static uint8_t     *txBuf = linkBuf[0];
static uint8_t      rxBuf[LINK_RX_LEN];      /* written by the DMA (circular)    */
static uint32_t     rxRd;                    /* read index into rxBuf            */
static hil_parser_t rxParser;
static hil_seq_t    rxSeq;
static uint8_t      txSeq;

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
volatile CtlCmd_t       g_cmd = { .mode = CTL_DEFAULT_MODE };
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

/* Sender: lower priority than the receiver, timestamps then notifies it.
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

/* Setpoint of the current mode, then limited in slew rate (except in TEST, which
   must be a clean step). TEST: 0 for CTL_HOLD_MS after entering the mode, then a
   square wave between CTL_STEP_RPM and 0. REMOTE: the speed requested by the
   supervisor, forced to 0 when the setpoint has not been refreshed for
   LINK_DEADMAN_MS (dead-man on the operator, not only on the link). */
static float ctl_setpoint(uint32_t ms)
{
  float target = 0.0f;                           /* IDLE */

  switch (g_cmd.mode)
  {
    case HIL_MODE_LOCAL:
      target = (float)g_acq.lastRaw * (CTL_RPM_MAX / 4095.0f);
      break;
    case HIL_MODE_REMOTE:
      if ((ms - g_cmd.lastSpMs) <= LINK_DEADMAN_MS)
      {
        target = (float)g_cmd.remoteRpm10 * 0.1f;
      }
      break;
    case HIL_MODE_TEST:
    {
      const uint32_t t = ms - g_cmd.modeSinceMs;
      if (t >= CTL_HOLD_MS)
      {
        target = ((((t - CTL_HOLD_MS) / CTL_HALF_PERIOD_MS) & 1U) == 0U) ? CTL_STEP_RPM : 0.0f;
      }
      g_ctl.spRamp = target;
      return target;
    }
    default:
      break;
  }

  float sp = g_ctl.spRamp;
  if (target > sp + CTL_SLEW_RPM_PER_MS)      { sp += CTL_SLEW_RPM_PER_MS; }
  else if (target < sp - CTL_SLEW_RPM_PER_MS) { sp -= CTL_SLEW_RPM_PER_MS; }
  else                                        { sp = target; }
  g_ctl.spRamp = sp;
  return sp;
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

  if (g_ctl.fault != CTL_FAULT_NONE)         /* latched: output already forced off */
  {
    g_ctl.spRpm = sp;
    g_ctl.rpm   = rpm;
    g_ctl.duty  = 0.0f;
    g_ctl.ticks++;
    return;
  }

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

  /* Stall detection on the measured speed (valid only once the estimate is full). */
  if ((g_enc.winN == ENC_WIN) && (u >= CTL_STALL_DUTY) &&
      (rpm < CTL_STALL_RPM) && (rpm > -CTL_STALL_RPM))
  {
    g_ctl.stallMs++;
  }
  else
  {
    g_ctl.stallMs = 0U;
  }

  if (g_ctl.stallMs >= CTL_STALL_MS)
  {
    g_ctl.fault = CTL_FAULT_STALL;
    g_cmd.evtPending |= (1UL << HIL_EVT_STALL);
    Safe_State();                            /* MOE off, PA9 low, LD2 on: no reset of the CPU */
    u = 0.0f;
  }

  const uint32_t cmp = (uint32_t)(u * (float)(__HAL_TIM_GET_AUTORELOAD(&htim1) + 1U));
  __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, cmp);

  g_ctl.spRpm = sp;
  g_ctl.rpm   = rpm;
  g_ctl.duty  = u;
  g_ctl.ticks++;

  /* One-shot capture in TEST mode, starting at the first sample where the setpoint is non-zero. */
  if ((g_rec.done == 0U) && (g_cmd.mode == HIL_MODE_TEST) && (sp > 0.0f) && (g_rec.n < REC_LEN))
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

/* ---- Supervisor link: frames from hil_proto.h, no printf (project rule) ---- */

static int16_t sat_i16(float v)
{
  if (v > 32767.0f)  { return 32767; }
  if (v < -32768.0f) { return -32768; }
  return (int16_t)v;
}

static uint32_t link_add(uint32_t n, uint8_t type, const uint8_t *payload, uint8_t len)
{
  return n + (uint32_t)hil_encode(&txBuf[n], type, txSeq++, payload, len);
}

static uint32_t link_add_telemetry(uint32_t n)
{
  uint8_t pl[12];
  hil_put_u32(&pl[0], g_acq.blocks);
  hil_put_u16(&pl[4], (uint16_t)sat_i16(g_ctl.spRpm * 10.0f));
  hil_put_u16(&pl[6], (uint16_t)sat_i16((float)g_enc.rpm10));
  hil_put_u16(&pl[8], (uint16_t)(g_ctl.duty * 1000.0f));
  pl[10] = (uint8_t)g_ctl.fault;
  pl[11] = (uint8_t)g_cmd.mode;
  return link_add(n, HIL_T_TELEMETRY, pl, (uint8_t)sizeof pl);
}

static uint32_t link_add_heartbeat(uint32_t n)
{
  uint8_t pl[6];
  hil_put_u32(&pl[0], g_acq.blocks);
  pl[4] = (uint8_t)g_ctl.fault;
  pl[5] = (uint8_t)g_cmd.mode;
  return link_add(n, HIL_T_HEARTBEAT, pl, (uint8_t)sizeof pl);
}

/* Pending events, lowest code first, at most LINK_MAX_EVENTS per cycle. */
static uint32_t link_add_events(uint32_t n)
{
  uint32_t sent = 0U;
  for (uint32_t code = 1U; (code < 8U) && (sent < LINK_MAX_EVENTS); code++)
  {
    if ((g_cmd.evtPending & (1UL << code)) != 0U)
    {
      uint8_t pl[9];
      hil_put_u32(&pl[0], g_acq.blocks);
      pl[4] = (uint8_t)code;
      hil_put_u32(&pl[5], (uint32_t)g_cmd.evtArg[code]);
      g_cmd.evtPending &= ~(1UL << code);
      n = link_add(n, HIL_T_EVENT, pl, (uint8_t)sizeof pl);
      sent++;
    }
  }
  return n;
}

/* Acknowledgements waiting to be sent (filled while handling received commands). */
#define ACK_QUEUE 4U
static struct { uint8_t type, status; int16_t value; } ackQ[ACK_QUEUE];
static uint32_t ackN;

static void ack_push(uint8_t type, uint8_t status, int16_t value)
{
  if (ackN < ACK_QUEUE)
  {
    ackQ[ackN].type = type; ackQ[ackN].status = status; ackQ[ackN].value = value;
    ackN++;
  }
}

static uint32_t link_add_acks(uint32_t n)
{
  for (uint32_t i = 0U; i < ackN; i++)
  {
    uint8_t pl[4];
    pl[0] = ackQ[i].type;
    pl[1] = ackQ[i].status;
    hil_put_u16(&pl[2], (uint16_t)ackQ[i].value);
    n = link_add(n, HIL_T_ACK, pl, (uint8_t)sizeof pl);
  }
  ackN = 0U;
  return n;
}

static void link_event(uint32_t code, int32_t arg)
{
  g_cmd.evtArg[code] = arg;
  g_cmd.evtPending |= (1UL << code);
}

/* The only place where the supervisor can influence the controller. */
static void link_set_mode(uint32_t mode)
{
  if (mode == g_cmd.mode) { return; }
  if (mode == HIL_MODE_REMOTE)                                     /* nothing requested yet: stay at 0 */
  {
    g_cmd.remoteRpm10 = 0;
    g_cmd.spTimedOut  = 1U;                                        /* no setpoint to time out yet */
  }
  if (mode == HIL_MODE_TEST)   { g_rec.n = 0U; g_rec.done = 0U; } /* re-arm the step recording */
  g_cmd.modeSinceMs = g_acq.blocks;
  g_cmd.mode = mode;
  link_event(HIL_EVT_MODE_CHANGED, (int32_t)mode);
}

static void link_handle_frame(const hil_frame_t *f)
{
  switch (f->type)
  {
    case HIL_T_HEARTBEAT:
      break;                                                       /* keep-alive only */

    case HIL_T_CMD_SETPOINT:
      if (f->len == 2U)
      {
        int32_t v = hil_get_i16(f->payload);
        uint8_t status = HIL_ACK_OK;
        const int32_t vmax = (int32_t)(CTL_RPM_MAX * 10.0f);
        if (v < 0)    { v = 0;    status = HIL_ACK_CLAMPED; }
        if (v > vmax) { v = vmax; status = HIL_ACK_CLAMPED; }
        g_cmd.remoteRpm10 = v;
        g_cmd.lastSpMs    = g_acq.blocks;
        g_cmd.spTimedOut  = 0U;
        ack_push(HIL_T_CMD_SETPOINT, status, (int16_t)v);
        break;
      }
      g_link.cmdRejected++;
      ack_push(HIL_T_CMD_SETPOINT, HIL_ACK_REJECTED, 0);
      link_event(HIL_EVT_CMD_REJECTED, f->type);
      break;

    case HIL_T_CMD_MODE:
      if ((f->len == 1U) && (f->payload[0] <= HIL_MODE_TEST))
      {
        link_set_mode(f->payload[0]);
        ack_push(HIL_T_CMD_MODE, HIL_ACK_OK, (int16_t)f->payload[0]);
        break;
      }
      g_link.cmdRejected++;
      ack_push(HIL_T_CMD_MODE, HIL_ACK_REJECTED, 0);
      link_event(HIL_EVT_CMD_REJECTED, f->type);
      break;

    case HIL_T_CMD_FAULT:
      if ((f->len == 1U) && (faultlab_inject(f->payload[0]) == 0))
      {
        ack_push(HIL_T_CMD_FAULT, HIL_ACK_OK, (int16_t)f->payload[0]);
        break;
      }
      g_link.cmdRejected++;
      ack_push(HIL_T_CMD_FAULT, HIL_ACK_REJECTED, (int16_t)((f->len == 1U) ? f->payload[0] : 0U));
      break;

    default:
      g_link.cmdRejected++;
      link_event(HIL_EVT_CMD_REJECTED, f->type);
      break;
  }
}

/* Full restart of the RX path. A UART error in DMA mode makes the HAL abort the reception;
   seen on the bench: the DMA channel left BUSY while the UART was READY, so a plain
   HAL_UART_Receive_DMA() kept failing and the target stopped hearing the supervisor.
   Typical trigger: the ESP32-S3 boot ROM log (115200 baud on its U0TXD = our RX) at each
   supervisor reset. Both sides are reset here, and the error flags cleared. */
static void link_rx_restart(void)
{
  (void)HAL_UART_AbortReceive(&huart1);
  (void)HAL_DMA_Abort(&hdma_usart1_rx);                            /* no-op error if already stopped */
  __HAL_UART_CLEAR_FLAG(&huart1, UART_CLEAR_PEF | UART_CLEAR_FEF | UART_CLEAR_NEF | UART_CLEAR_OREF);
  rxRd = 0U;
  if (HAL_UART_Receive_DMA(&huart1, rxBuf, LINK_RX_LEN) == HAL_OK) { g_link.rxRestarts++; }
  else { g_link.rxRestartFails++; }
}

/* Drains the circular RX DMA buffer into the parser. */
static void link_rx_poll(void)
{
  static uint32_t lastRestartMs;
  const uint32_t ms = g_acq.blocks;
  /* Reception stopped by a UART error, or silent for 2 s although the supervisor sends a
     heartbeat every 100 ms: restart, at most once per second. */
  if ((huart1.RxState == HAL_UART_STATE_READY) ||
      (((ms - g_cmd.lastRxMs) > (2U * LINK_DEADMAN_MS)) && ((ms - lastRestartMs) > LINK_DEADMAN_MS)))
  {
    lastRestartMs = ms;
    link_rx_restart();
    return;
  }

  const uint32_t wr = LINK_RX_LEN - __HAL_DMA_GET_COUNTER(&hdma_usart1_rx);
  hil_frame_t f;
  while (rxRd != wr)
  {
    hil_parser_push(&rxParser, rxBuf[rxRd]);
    rxRd = (rxRd + 1U) % LINK_RX_LEN;
    while (hil_parser_next(&rxParser, &f) != 0)
    {
      hil_seq_update(&rxSeq, f.seq);
      g_cmd.lastRxMs = g_acq.blocks;
      link_handle_frame(&f);
    }
  }
  g_link.rxFrames    = rxParser.framesOk;
  g_link.rxCrcErrors = rxParser.crcErrors;
  g_link.rxLost      = rxSeq.lost;
}

/* Supervisor silent, or REMOTE setpoint not refreshed, for LINK_DEADMAN_MS: report it.
   The setpoint itself already falls back to 0 in ctl_setpoint(). */
static void link_supervise(void)
{
  if ((g_cmd.mode == HIL_MODE_REMOTE) && (g_cmd.spTimedOut == 0U) &&
      ((g_acq.blocks - g_cmd.lastSpMs) > LINK_DEADMAN_MS))
  {
    g_cmd.spTimedOut = 1U;
    link_event(HIL_EVT_SP_TIMEOUT, 0);
  }

  const uint32_t silent = g_acq.blocks - g_cmd.lastRxMs;
  if ((g_cmd.linkAlive != 0U) && (silent > LINK_DEADMAN_MS))
  {
    g_cmd.linkAlive = 0U;
    link_event(HIL_EVT_LINK_LOST, 0);
  }
  else if ((g_cmd.linkAlive == 0U) && (g_link.rxFrames != 0U) && (silent <= LINK_DEADMAN_MS))
  {
    g_cmd.linkAlive = 1U;
    link_event(HIL_EVT_LINK_RESTORED, 0);
  }
}

/* ---- RTOS Doctor frames (rtos_probe.h -> hil_proto.h HIL_T_TASK_* / TRACE / RTOS_SYS / QUEUE) ---- */

static uint32_t trCycTmp[LINK_TRACE_MAX_EVENTS];
static uint8_t  trIdTmp[LINK_TRACE_MAX_EVENTS];

/* Drains the probe trace into TRACE frames: t0, then (id, 24-bit delta) per event. */
static uint32_t link_add_trace(uint32_t n)
{
  const uint32_t k = rtp_trace_read(trCycTmp, trIdTmp, LINK_TRACE_MAX_EVENTS);
  const uint16_t dropped = (uint16_t)rtp_trace_dropped();
  uint32_t i = 0U;
  while (i < k)
  {
    uint8_t pl[HIL_MAX_PAYLOAD];
    uint32_t len = 6U;
    uint32_t prev = trCycTmp[i];
    hil_put_u32(&pl[0], prev);
    hil_put_u16(&pl[4], dropped);
    while ((i < k) && ((len + 4U) <= HIL_MAX_PAYLOAD))
    {
      const uint32_t d = trCycTmp[i] - prev;
      if (d > 0xFFFFFFU) { break; }                          /* too long for 24 bits: new frame, new t0 */
      pl[len]      = trIdTmp[i];
      pl[len + 1U] = (uint8_t)d;
      pl[len + 2U] = (uint8_t)(d >> 8);
      pl[len + 3U] = (uint8_t)(d >> 16);
      len += 4U;
      prev = trCycTmp[i];
      i++;
    }
    n = link_add(n, HIL_T_TRACE, pl, (uint8_t)len);
  }
  return n;
}

static uint32_t link_add_task_table(uint32_t n, uint8_t tag)
{
  RtpTaskInfo_t ti;
  if (rtp_task_info(tag, &ti) == 0) { return n; }
  uint8_t pl[40];
  pl[0] = tag;
  pl[1] = ti.basePrio;
  hil_put_u16(&pl[2], ti.stackWords);
  hil_put_u16(&pl[4], ti.deadlineMs);
  pl[6] = ti.flags;
  pl[7] = 0U;
  hil_put_u32(&pl[8], SystemCoreClock);
  uint32_t len = 12U;
  for (const char *c = ti.name; (*c != '\0') && (len < sizeof pl); c++) { pl[len++] = (uint8_t)*c; }
  return link_add(n, HIL_T_TASK_TABLE, pl, (uint8_t)len);
}

static uint32_t link_add_task_stats(uint32_t n, uint8_t tag)
{
  RtpTaskInfo_t ti;
  if (rtp_task_info(tag, &ti) == 0) { return n; }
  RtpTaskStats_t st;
  rtp_task_stats(tag, &st);
  uint8_t pl[48];
  pl[0] = tag;
  pl[1] = st.state;
  pl[2] = st.prio;
  pl[3] = 0U;
  hil_put_u16(&pl[4], st.cpuPermille);
  hil_put_u16(&pl[6], st.jobs);
  hil_put_u32(&pl[8], st.cMax);
  hil_put_u32(&pl[12], st.cAvg);
  hil_put_u32(&pl[16], st.rMax);
  hil_put_u32(&pl[20], st.rMin);
  hil_put_u32(&pl[24], st.tAvg);
  hil_put_u32(&pl[28], st.tMin);
  hil_put_u32(&pl[32], st.tMax);
  hil_put_u16(&pl[36], st.misses);
  hil_put_u16(&pl[38], st.stackFree);
  hil_put_u32(&pl[40], st.cMaxLast);
  hil_put_u32(&pl[44], st.rMaxLast);
  return link_add(n, HIL_T_TASK_STATS, pl, (uint8_t)sizeof pl);
}

static uint32_t link_add_queue_stats(uint32_t n, uint8_t qid)
{
  RtpQueueStats_t qs;
  if (rtp_queue_stats(qid, &qs) == 0) { return n; }
  uint8_t pl[32];
  pl[0] = qid;
  pl[1] = 0U;
  hil_put_u16(&pl[2], qs.length);
  hil_put_u16(&pl[4], qs.waiting);
  hil_put_u16(&pl[6], qs.maxFill);
  hil_put_u32(&pl[8], qs.failures);
  uint32_t len = 12U;
  for (const char *c = qs.name; (*c != '\0') && (len < sizeof pl); c++) { pl[len++] = (uint8_t)*c; }
  return link_add(n, HIL_T_QUEUE_STATS, pl, (uint8_t)len);
}

static uint32_t link_add_rtos_sys(uint32_t n)
{
  RtpSysStats_t s;
  rtp_sys_stats(&s);
  uint8_t pl[44];
  hil_put_u32(&pl[0], SystemCoreClock);
  hil_put_u32(&pl[4], s.switches);
  hil_put_u32(&pl[8], s.events);
  hil_put_u32(&pl[12], s.dropped);
  hil_put_u16(&pl[16], s.idlePermille);
  hil_put_u16(&pl[18], (uint16_t)configTICK_RATE_HZ);
  hil_put_u32(&pl[20], g_ctxSwitch.max);
  hil_put_u32(&pl[24], g_isrToTask.max);
  hil_put_u16(&pl[28], s.probePermille);
  hil_put_u16(&pl[30], s.kernelPermille);
  size_t heapFree, heapMin;
  rtp_heap(&heapFree, &heapMin);
  hil_put_u32(&pl[32], (uint32_t)configTOTAL_HEAP_SIZE);
  hil_put_u32(&pl[36], (uint32_t)heapFree);
  hil_put_u32(&pl[40], (uint32_t)heapMin);
  return link_add(n, HIL_T_RTOS_SYS, pl, (uint8_t)sizeof pl);
}

/* Detector events from the probe (deadline misses, inheritance, queue full, stack/heap low...). */
static uint32_t link_add_probe_events(uint32_t n)
{
  uint32_t tMs;
  uint8_t  code;
  int32_t  arg;
  for (uint32_t k = 0U; (k < LINK_PROBE_EVENTS) && (rtp_event_pop(&tMs, &code, &arg) != 0); k++)
  {
    uint8_t pl[9];
    hil_put_u32(&pl[0], tMs);
    pl[4] = code;
    hil_put_u32(&pl[5], (uint32_t)arg);
    n = link_add(n, HIL_T_EVENT, pl, (uint8_t)sizeof pl);
  }
  return n;
}

/* Supervisor link: below the control loop, so it can never delay control.
   It blocks at one place only (vTaskDelayUntil): one job per period for the probe.
   Two TX buffers: one is built while the other may still be on the wire. */
static void LinkTask(void *arg)
{
  (void)arg;
  const TickType_t period = pdMS_TO_TICKS(LINK_PERIOD_MS);
  TickType_t lastWake = xTaskGetTickCount();
  uint32_t hbDiv = 0U, cycle = 0U, statIdx = 0U, sysPending = 0U;
  uint8_t  tableTag = 1U;

  hil_parser_init(&rxParser);
  hil_seq_init(&rxSeq);
  rxRd = 0U;
  (void)HAL_UART_Receive_DMA(&huart1, rxBuf, LINK_RX_LEN);

  for (;;)
  {
    vTaskDelayUntil(&lastWake, period);

    link_rx_poll();
    link_supervise();

    /* Once per second: the probe closes its window and runs its detectors, then the
       statistics follow, one task (then one queue) per cycle. */
    cycle++;
    if ((cycle % 100U) == 0U) { rtp_second(); sysPending = 1U; statIdx = 1U; }

    if (huart1.gState != HAL_UART_STATE_READY) { g_link.busy++; continue; }   /* previous burst still sent */
    txBuf = linkBuf[cycle & 1U];

    uint32_t n = link_add_telemetry(0U);
    if (++hbDiv >= LINK_HEARTBEAT_DIV) { hbDiv = 0U; n = link_add_heartbeat(n); }
    n = link_add_acks(n);
    n = link_add_events(n);
    n = link_add_probe_events(n);
    if (sysPending != 0U) { sysPending = 0U; n = link_add_rtos_sys(n); }
    if ((statIdx != 0U) && (statIdx < RTP_MAX_TASKS))
    {
      n = link_add_task_stats(n, (uint8_t)statIdx);
      statIdx++;
    }
    else if ((statIdx >= RTP_MAX_TASKS) && (statIdx < (RTP_MAX_TASKS + RTP_MAX_QUEUES)))
    {
      n = link_add_queue_stats(n, (uint8_t)(statIdx - RTP_MAX_TASKS));
      statIdx++;
    }
    if ((cycle % 20U) == 0U)
    {
      n = link_add_task_table(n, tableTag);
      tableTag = (uint8_t)((tableTag % (RTP_MAX_TASKS - 1U)) + 1U);
    }
    n = link_add_trace(n);

    if (HAL_UART_Transmit_DMA(&huart1, txBuf, (uint16_t)n) == HAL_OK) { g_link.sent++; }
    else                                                             { g_link.busy++; }
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
  rtp_init();                    /* before the first task is created */

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

  hLink = xTaskCreateStatic(LinkTask, "link", LINK_STACK_WORDS, NULL,
                            LINK_PRIORITY, link_Stack, &link_TCB);
  configASSERT(hLink != NULL);

  /* Fault lab: demo tasks that inject faults on request (HIL_T_CMD_FAULT). The RTOS Doctor
     probe needs no registration: every task is discovered when FreeRTOS creates it. */
  faultlab_init();

  /* Deadlines of the periodic tasks (D = T). Without them the probe would use the measured mean
     period; declaring them lets the dashboard show and check a deadline for every periodic task. */
  rtp_set_deadline(hAcq, 1U);
  rtp_set_deadline(hCtxTx, CTX_TX_PERIOD_MS);
  rtp_set_deadline(hTaskA, TASK_A_PERIOD_MS);
  rtp_set_deadline(hLink, LINK_PERIOD_MS);
  rtp_set_deadline(hTaskB, TASK_B_PERIOD_MS);
  rtp_set_deadline((TaskHandle_t)healthTaskHandle, HEALTH_PERIOD_MS);
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
  huart1.Init.BaudRate = 2000000;
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
  /* DMA1_Channel5_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel5_IRQn, 7, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel5_IRQn);

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
