# RTOS Doctor

**A live monitor for FreeRTOS applications: it discovers the tasks, measures their timing, draws the
schedule, computes whether every deadline is guaranteed, and explains what goes wrong.**
It is built and proven on a hardware-in-the-loop motor-control bench (STM32 + two ESP32-S3).

Timing bugs in an RTOS are invisible: a task that misses one deadline in a thousand, a stack two calls away
from overflowing, a mutex that blocks a high-priority task. RTOS Doctor makes them visible, and every number
it shows is either **measured on the target** or **computed from those measurements**, with the steps shown.

> Signature of the project: compute the timing analytically first, then verify it by measurement.

## What it does

| View | What you see |
| --- | --- |
| **Overview** | one verdict, six health lights (scheduling, CPU, memory, queues, interrupts, observation cost), key figures, CPU share per task, latest warnings |
| **Tasks** | every task, discovered automatically: state, priority (and inherited priority), period and jitter, deadline, execution time C, response time R, misses, CPU, stack; a card per task with its history |
| **Timeline** | Gantt chart rebuilt from a continuous trace of every context switch: running, ready-and-waiting, interrupts, ticks |
| **Analysis** | response-time analysis with the measured values, the iteration steps for each task, and "what if" editing of priority, period and C |
| **Resources** | stack and heap usage, queue fill and overflows, interrupt latency, cost of the probe itself |
| **Alerts** | deadline misses, CPU hogs, starvation, stack and heap pressure, full queues, priority inheritance: each one explained, with what to do |
| **Fault lab** | buttons that make the target misbehave on purpose; the matching detector must fire (this is how the tool proves it works) |
| **Sessions** | record everything, save it to a file, replay it later, or publish it as a demo that runs without hardware |

## How it works

```
 STM32L476 + FreeRTOS            Elyssa (ESP32-S3)                Browser
 application tasks, unchanged    gateway                          dashboard (analysis runs here)
 └ rtos_probe (kernel hooks) ──UART 2 Mbaud──► frames → JSON + binary trace ──Wi-Fi/WebSocket──► 8 views
                             ◄── commands ────  (mode, setpoint, fault)    ◄──────────────────
```

- **The probe needs no code in your tasks.** It is attached through FreeRTOS trace macros. A task is discovered when
  FreeRTOS creates it; a job runs from the task's release (it becomes ready) to the moment it blocks. At each context
  switch, a read-only look at the kernel's ready list tells "preempted" from "blocked".
- **Measured on the target, analysed in the browser.** The target sends C, R, periods, misses, stacks and a trace
  (about 6 000 events/s); the browser computes the response-time analysis, so it can show its steps and answer "what if".
- **Honest about itself.** Trace losses are counted, the probe measures its own CPU cost, and worst cases are labelled
  "since reset".

Details: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Proof

- **Fault lab:** CPU hog, stack pressure, queue flood, priority inversion and heap pressure are injected by isolated
  demo tasks; each one has an expected detection.
- **Tests:** link protocol (every single-bit corruption, resynchronisation, AddressSanitizer), response-time analysis
  (textbook task set and edge cases), and a UI test that loads every view in a headless browser. See [Tests](#tests).
- **Measured on the bench** (more in [docs/MEASUREMENTS.md](docs/MEASUREMENTS.md)):

| Measurement | Predicted | Measured |
| --- | --- | --- |
| PI speed step 0 → 1500 rpm, rise time | 103 ms (simulation) | 103 ms |
| Motor model time constant | 100 ms | 99.5 ms |
| Notify-to-wake latency (`-O2`) | – | 6.3 to 7.0 µs |
| First RTOS-layer run: tasks with a shorter period but a lower priority than a 5 ms task | deadlines missed (analysis) | 7 561 and 223 misses |
| Link over 2 Mbaud UART | – | 0 CRC errors, 0 lost frames |

## Status

| Part | State |
| --- | --- |
| Motor bench: plant model, 1 kHz PI loop, stall protection, encoder dithering | working, measured |
| Framed link with CRC and sequence numbers, dead-man on the operator | working, measured |
| Wi-Fi gateway, setup portal, dashboard served from flash | working |
| RTOS layer, first version (job markers in each task) | working, measured (table above) |
| **Generic probe** (automatic discovery and job detection, detectors, fault lab), dashboard v2 | written, compiled, UI tested with synthetic data; **validation on the bench pending** |

## Quick start

**Hardware:** NUCLEO-L476RG, two ESP32-S3 boards (one plant, one gateway "Elyssa"), a few wires.
Wiring and board notes: [docs/HARDWARE.md](docs/HARDWARE.md).

1. **Target (STM32):** open `firmware/` in STM32CubeIDE 2.2, build (GCC `-O2`) and flash. Or build from the
   command line: `tools/build_firmware.sh`.
2. **Plant (ESP32 #1):** `esp32-motor-sim/build_flash.ps1` (builds in the `espressif/idf:v6.1` Docker image, flashes
   with esptool), or `idf.py build flash` with ESP-IDF v6.1.
3. **Gateway (Elyssa):** Arduino IDE with the "Moovma" board package, open `supervisor/supervisor.ino`, upload.
4. **Wi-Fi:** on first start the gateway opens the network **Elyssa-Setup**: join it, pick your network in the page.
   Or type `wifi set "network" password` in its USB console.
5. Open **http://elyssa.local** (or the address printed on the console).

**Without hardware:** open `supervisor/web/index.html?session=<session file>` from any web server to replay a
recorded session.

## Using the probe in your own FreeRTOS project

1. Copy `rtos_probe.c`, `rtos_probe.h` and `rtos_probe_hooks.h` (from `firmware/Core/`) and `protocol/hil_proto.h`.
2. Paste the probe block of `firmware/Core/Inc/FreeRTOSConfig.h` (trace macros and three settings) into yours.
3. Enable the DWT cycle counter, then call `rtp_init()` before creating the first task.
4. From a low-priority task, call `rtp_second()` once per second and send `rtp_trace_read()`, `rtp_task_stats()`,
   `rtp_event_pop()`... over any link (see `LinkTask` in `firmware/Core/Src/main.c`).
5. Optional: `rtp_set_deadline()`, `rtp_queue_register()`, `rtp_isr_enter()/rtp_isr_exit()`.

Requirements: Cortex-M with DWT, FreeRTOS 10.x, static or dynamic allocation.

## Repository layout

```
firmware/          STM32 target: FreeRTOS application, rtos_probe, fault lab (STM32CubeIDE project)
esp32-motor-sim/   ESP32 #1: DC motor model, PWM capture, quadrature encoder (ESP-IDF)
supervisor/        Elyssa gateway (Arduino) and the dashboard (supervisor/web, plain ES modules)
protocol/          the link protocol header (single source) and its host tests
tools/             build, test, asset embedding and sync scripts
docs/              architecture, measurement log, hardware, lessons learned
captures/          logic-analyser captures
```

## Tests

| Test | Command |
| --- | --- |
| Link protocol (host, gcc, sanitizers) | `gcc -Wall -Wextra -Werror -fsanitize=address,undefined protocol/test_proto.c -o t && ./t` |
| Response-time analysis | `node supervisor/web/tests/analysis.test.mjs` |
| Dashboard in a headless browser (Docker) | `powershell -File tools/test_ui.ps1` |
| Firmware build | `tools/build_firmware.sh` |

They also run on every push (GitHub Actions, `.github/workflows/ci.yml`).

## Design decisions

- **Priorities from deadlines, analysis before measurement.** The tool exists to check exactly this.
- **Measure on the target, analyse elsewhere.** The probe stays small and O(1); the analysis can evolve freely.
- **No instrumentation in the application's tasks.** Kernel hooks only, so the probe is reusable.
- **The gateway observes, the target decides.** Commands are proposals: the target clamps, ramps, or refuses them,
  and falls back to a safe state when the operator disappears. The emergency stop never goes through a link.
- **Static allocation, fail-stop faults, nothing claimed without a measurement.**

What went wrong along the way and what it taught: [docs/LESSONS.md](docs/LESSONS.md).
The original phase plan: [docs/ROADMAP-HISTORY.md](docs/ROADMAP-HISTORY.md).

## Next

- Validate the generic probe and the fault lab on the bench; record a reference session and publish the demo page.
- Measure the probe's own cost and the end-to-end latency from a fault to the dashboard.
- Latency histogram for the unexplained 1.6k-cycle interrupt-to-task maximum.
- Hardware emergency stop (TIM1 break input), watchdog, long-run test.
