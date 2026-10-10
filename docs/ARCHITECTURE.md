# Architecture

RTOS Doctor has four parts: a **probe** inside the FreeRTOS application, a **framed link** to a
**gateway** (Elyssa, ESP32-S3), and a **dashboard** that runs in the browser. The motor bench
(STM32 + ESP32 plant) is the demonstration workload, not the product.

```
 Target: STM32L476 + FreeRTOS                    Gateway: Elyssa (ESP32-S3)             Browser
┌───────────────────────────────────┐  UART    ┌────────────────────────────┐  Wi-Fi  ┌──────────────────────┐
│ application tasks (unchanged)     │ 2 Mbaud  │ frame parser (CRC, seq)    │  HTTP   │ store ← live / replay│
│   ▲ trace macros (FreeRTOSConfig) │ ───────► │ trace → absolute cycles    │ ──────► │ model: analysis,     │
│ rtos_probe: discovery, jobs,      │ frames   │ JSON stats, binary trace   │  WS     │   health, alerts     │
│   detectors, trace ring           │ ◄─────── │ commands, dead-man refresh │ ◄────── │ 8 views, sessions    │
│ LinkTask (priority 10, every 10ms)│ commands │ setup portal, mDNS         │         │                      │
└───────────────────────────────────┘          └────────────────────────────┘         └──────────────────────┘
```

## 1. The probe (`firmware/Core/Src/rtos_probe.c`)

### Integration
The application's tasks contain **no instrumentation**. The probe is connected through the FreeRTOS
trace macros, defined in the `USER CODE Defines` section of `FreeRTOSConfig.h`. They expand inside the
kernel (`tasks.c`, `queue.c`), where the kernel structures are visible: they **read** fields and pass
plain values to the probe, and write only `pxTaskTag`, which the probe owns.

| Macro | Probe call | Purpose |
| --- | --- | --- |
| `traceTASK_CREATE` | `rtp_on_create(name, priority, stack words, handle)` → tag | discovery, idle and timer tasks included |
| `traceMOVED_TASK_TO_READY_STATE` | `rtp_on_ready(tag)` | release of a job |
| `traceTASK_SWITCHED_IN` | `rtp_on_switch_in(tag)` | start of a CPU slice |
| `traceTASK_SWITCHED_OUT` | `rtp_on_switch_out(tag, blocked)` | end of a slice; end of the job if `blocked` |
| `traceTASK_INCREMENT_TICK` | `rtp_on_tick(tick)` | time base for events, tick marks |
| `traceTASK_PRIORITY_INHERIT` / `DISINHERIT` | `rtp_on_inherit` / `rtp_on_disinherit` | priority inversion on mutexes |
| `traceQUEUE_SEND(_FAILED)` (and `_FROM_ISR`) | `rtp_on_queue_send` / `rtp_on_queue_full` | fill level, overflows of registered queues |

Required settings: `configUSE_TRACE_FACILITY = 1`, `configUSE_APPLICATION_TASK_TAG = 1`,
`configRECORD_STACK_HIGH_ADDRESS = 1`. One call at start-up: `rtp_init()`, after the DWT cycle counter
is enabled and before the first task is created. Optional: `rtp_set_deadline(task, ms)`,
`rtp_queue_register(queue, name)`, `rtp_isr_enter/exit(id)` in an interrupt handler.

### Job model
A **job** (instance) runs from the moment the task enters a ready list (its release) to the moment it
leaves the CPU **blocked or suspended**. At `traceTASK_SWITCHED_OUT`, the probe checks, without modifying
anything, which list the task's state item is in:

```c
blocked = listLIST_ITEM_CONTAINER(&pxCurrentTCB->xStateListItem) != &pxReadyTasksLists[pxCurrentTCB->uxPriority];
```

Still in its ready list means it was **preempted** (the job continues); anywhere else means it **blocked**
(the job ends). This is reliable because a blocking API moves the task to the delayed or event list before
it yields. A ready event for a task whose job is still open is a requeue (priority change), not a release.

Per job: **C** = CPU time accumulated between release and end (slices only), **R** = end − release.
Per task: release interval (mean, min, max: the period and its jitter), deadline misses (R > D, with
D = the declared deadline or, by default, the measured mean period after 4 releases), CPU share per second.

Limits, stated: a task that blocks several times per cycle is seen as several jobs (one per segment, as
most trace tools do by default); interrupt time spent during a task's slice is counted in that task's C
unless the interrupt is instrumented; the first job of each task (from creation) is ignored.

### Detectors
| Event | Rule |
| --- | --- |
| `DEADLINE_MISS` | jobs with R > D in the last second |
| `CPU_HOG` | one job ran 50 ms or more without blocking (reported when it ends) |
| `STARVED` | a task that ran before was ready for a whole second without running |
| `STACK_LOW` | less than 30 % of the stack never used (high-water mark), once per task |
| `HEAP_LOW` | minimum-ever free heap below 30 %, once |
| `QUEUE_FULL` | send failures on a registered queue in the last second |
| `PRIO_INHERIT` / `PRIO_RESTORE` | a mutex holder inherited a higher priority / got its own back, with the duration |

Events are queued in a small ring (interrupt-safe) and sent by the link task.

### Trace
Every switch-in, release, tick and instrumented interrupt entry/exit is stored as `(DWT cycles, id)` in a
1024-entry ring, filled from several interrupt levels with interrupts masked for a few cycles. The link
task drains it every 10 ms into TRACE frames: `t0`, then `id + 24-bit delta` per event (4 bytes). About
6 000 to 12 000 events/s, 25 to 50 kB/s: the UART runs at 2 Mbaud (80 MHz / 40, no baud error). Losses are
counted, never hidden.

### Cost
Each hook masks interrupts and accumulates its own cycles; the probe reports this as `probe_permille`,
together with `kernel_permille` (time between two tasks). Memory: about 7 KB of RAM (trace ring, task and
queue records).

## 2. The link (`protocol/hil_proto.h`)
One header, shared by the target and the gateway (copied by `tools/sync_proto.ps1`). Frames:
`A5 5A | LEN | TYPE | SEQ | payload | CRC-16/CCITT`. The parser keeps the bytes since the last start
marker and **replays** them after a CRC failure, so a false header in noise cannot swallow real frames
(this was a real bug, found by the host tests). Sequence numbers give the number of lost frames.
Host tests: `protocol/test_proto.c` (every single-bit corruption, resynchronisation, sequence wrap).

## 3. The gateway (`supervisor/`, Arduino)
Decodes frames, converts the trace to absolute timestamps and sends it to the browsers as binary
WebSocket messages (5 bytes per event, every 100 ms); statistics and events go as JSON. It serves the
dashboard from flash (gzipped files embedded by `tools/embed_assets.py`), offers a Wi-Fi setup portal,
and relays commands. Safety: the gateway forwards a REMOTE setpoint only while the browser keeps
refreshing it, and the target falls back to 0 if no setpoint arrives for 1 s (dead-man on the operator).

## 4. The dashboard (`supervisor/web/`)
Plain ES modules, no build step: `store.js` (state, ingestion), `model.js` + `analysis-core.js`
(response-time analysis, health grading), `alerts.js` (events → explained alerts), `session.js`
(record, export, replay), one module per view. The analysis is computed in the browser: the target
only measures, so the analysis can change, explain its steps and answer "what if" without reflashing.

Response-time analysis (fixed priorities, preemptive):

    R(n+1) = C_i + Σ_{j ∈ hp(i)} ⌈R(n) / T_j⌉ · (C_j + ctx) + Σ_irq ⌈R(n) / T_q⌉ · C_q

with equal priorities counted as interference (time slicing), the measured context-switch cost, and the
ADC interrupt measured from the trace. Unit tests: `supervisor/web/tests/analysis.test.mjs` (textbook
task set, divergence, interference terms).

## 5. Demonstration workload
- STM32: 1 kHz acquisition and PI speed loop, synthetic load tasks A and B, a notify-latency pair, the link
  task, a health task, and the **fault lab** (`faultlab.c`): isolated tasks that inject a CPU hog, stack
  pressure, a queue flood, a priority inversion or heap pressure on request, all below the control loop.
- ESP32 #1: DC motor model, PWM capture, quadrature encoder output (hardware in the loop).
