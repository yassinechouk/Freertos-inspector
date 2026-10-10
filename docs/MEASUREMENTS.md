# Measurement log

Engineering record of the bench, from the first clock measurement to the RTOS layer. Each value is
predicted first, then measured; the conditions are given per section. Kept as it was written, so the
reasoning (and its corrections) stays visible.


All measurements: GCC `-O2` unless stated, FreeRTOS 10.3.1, tick 1 kHz, SYSCLK 80 MHz.
The clock source is stated per section: **HSI** for the early measurements (clock, scheduling,
PWM), **HSE** (8 MHz from the ST-LINK MCO, bypass) from the encoder work on.
Instruments: DWT cycle counter (12.5 ns), logic analyser (FX2, up to 24 MS/s), CubeIDE Live Expressions.

### Clock

| Metric | Predicted | Measured |
| --- | --- | --- |
| MCO (SYSCLK/16) on PA8, HSI source | 5 MHz | 5.0035 MHz → SYSCLK **80.06 MHz (+0.07 %)**, resolution ±0.02 % |
| `RCC_CR` with HSE bypass (HSEON, HSEBYP, HSERDY, CSSON, PLLON, PLLRDY = 1) | 0x0F0F0063 | 0x0F0F0063 |
| `RCC_PLLCFGR` (PLLSRC = HSE, M = 1, N = 20, R = 2, PLLREN = 1) | 0x01001403 | 0x01001403 |
| `RCC_CFGR` (SW = SWS = PLL, AHB/APB /1, MCO = SYSCLK/16) | 0x4100000F | 0x4100000F |
| MCO (SYSCLK/16) on PA8, HSE source | 5.0000 MHz (analyser crystal limits the resolution) | <<FILL: measured MCO frequency>> |
| Generator (ESP32 crystal) vs STM32 clock, 189.58 s | \|error\| < 50 ppm | **+18.2 ppm** (±0.5), see *Encoder input* |

[![MCO on HSI, PulseView timing decoder](../captures/mco-hsi-timing.png)](../captures/mco-hsi-timing.png)

The HSE error is about 40 times smaller than the HSI error measured earlier (+700 ppm).
The board needed SB16 and SB50 ON and SB55 OFF, because the ST-LINK MCO is not connected to OSC_IN
by default (UM1724 §6.7.1).

### Scheduling (task A: C=2 ms, T=10 ms; task B: C=5 ms, T=25 ms; deadline-monotonic priorities)

Measured on HSI with `-O0`, logic analyser at 1 MS/s.

| Metric | Predicted | Measured |
| --- | --- | --- |
| Task B response, not preempted | 5 ms | 4.998–4.999 ms (HSI running 0.07 % fast) |
| Task B worst-case response, preempted once by A | 7 ms (naive model) | **7.010–7.011 ms** |
| Preemption overhead (tick + 2 context switches + delay call) | not modelled | ~10 µs (first estimate) |
| Task B start jitter | ±2 ms (= C\_A) | ±2.02 ms |

[![Task A (D0) and task B (D1), PulseView timing decoder](../captures/task-b-response-timing.png)](../captures/task-b-response-timing.png)

### Kernel overhead

Notify-to-wake latency: a low-priority task timestamps with DWT, calls `xTaskNotifyGive`, and a higher-priority task blocked in `ulTaskNotifyTake` reads DWT when it wakes.

| Build | Samples | Min | Avg | Max |
| --- | --- | --- | --- | --- |
| `-O0` | 13 841 | 987 cycles (12.3 µs) | 987 | 987 |
| `-O2` | 11 488 | **503 cycles (6.3 µs)** | 519 | 556 cycles (7.0 µs) |

### Controller I/O

Setpoint chain: TIM3 (10 kHz) triggers ADC1 in hardware, circular DMA fills 2 × 10
samples, and each half-transfer interrupt notifies the acquisition task, which averages
the block (1 kHz setpoint). The task itself starts the hardware, so no interrupt can
ever notify a task that does not exist yet. Motor PWM: TIM1 CH2, 20 kHz, 4000 steps,
compare preload enabled; currently driven open-loop from the setpoint.

| Metric | Predicted | Measured |
| --- | --- | --- |
| Setpoint acquisition rate | 1000 blocks/s, 0 late, 0 overrun | 1000 blocks/s, 0 late, 0 overrun |
| ISR-to-task latency (DMA ISR → `xTaskNotifyFromISR` → task), steady state, binary before the encoder input, HSI | — | min 776 / avg 815 / max **847 cycles** (9.7 / 10.2 / 10.6 µs) |
| Same, first wake-ups after reset (cold instruction cache) | — | up to 1432 cycles (17.9 µs) |
| Same, binary with the encoder input, HSE, 189 577 samples | — | min 815 / avg 863 / max **1599 cycles** (10.2 / 10.8 / 20.0 µs), `lateBlocks` 0, `overruns` 0. Cause of the higher maximum not identified. |
| Same, binary with the encoder input and the fail-stop code (`Safe_State`), HSE, 340 868 samples | — | min 809 / avg 857 / max **1579 cycles** (10.1 / 10.7 / 19.7 µs). Another binary, other numbers. |
| PWM frequency (TIM1 CH2, PA9), HSI | 20 kHz (20.014 kHz with HSI +0.07 %) | 20.017 kHz on one period, ±0.08 % (analyser: 1199 samples/period) |
| PWM duty follows setpoint (open loop) | proportional | 31.7 / 67.6 / 74.2 % at three positions, ±1 sample (0.08 %) |
| PWM period while duty changes (preload enabled) | constant | constant, no glitch observed |

[![PWM at 68 % duty, PulseView PWM decoder](../captures/pwm-duty-68.png)](../captures/pwm-duty-68.png)

### Encoder input (TIM2, x4) and quadrature source

Source: ESP32-S3 (ESP32 #1), MCPWM, 5 kHz per channel, channel A leading B by 90°
(two operators, three comparators, no CPU involved once started).
It was first verified alone with the ESP32's own PCNT in loopback (same crystal for the generator,
the PCNT and the time base, so this checks the logic, not the absolute frequency).
Then it was read by the STM32 on TIM2 (PA15 / PA1, 100 ns input filter, x4 mode), timed by the
STM32's own clock: the count and a DWT cycle stamp are read back-to-back once per 1 ms window.

| Metric | Predicted | Measured |
| --- | --- | --- |
| PCNT loopback, 5 kHz, count over the measured window (999 903 µs) | +19 998 (20 000 × 0.999903) | +19 998 |
| Same, channels swapped (direction check) | −19 998 | −19 998 |
| Same, 4 kHz (window 999 902 µs) | +15 998.4 | +15 999 |
| STM32: counts per 1 ms window | 20 | 20 (min 19, max 21) |
| STM32: window length | 80 000 cycles | 80 000.0007 cycles |
| STM32: total count over 189.58 s (189 577 windows) | 3 791 540.0 if the clocks were equal | 3 791 609 → **+18.2 ppm** (±0.5) |

The min 19 / max 21 come from the read instant (the ISR-to-task latency varies by about 0.9 µs),
not from the generator: the cumulative count is what compares the clocks.
The +18 ppm is the difference between the two crystals (ST-LINK 8 MHz vs ESP32 40 MHz);
it cannot say which one is off without a third reference.

### Fail-stop (safe state)

`Safe_State()` sets `TIM1->BDTR.MOE` to 0, drives PA9 low, switches PA9 to a plain output and lights LD2.
It works at register level (no HAL or RTOS call), so it can run from a task, an interrupt or the NMI.
Each path was exercised 5 s after start by a temporary self-test build (`CSS_SELFTEST`, 0 in the reference build)
and read back with the core running, from the registers by address.

| State, read 5 s after start | `g_acq.blocks` | PA9 mode | TIM1 MOE | LD2 |
| --- | --- | --- | --- | --- |
| Running (reference build) | counting | 2 (alternate function) | 1 | off |
| `Error_Handler()` called from the acquisition task | frozen at 5000 | 1 (plain output) | 0 | on |
| CSS callback `HAL_RCC_CSSCallback()` called from the acquisition task | frozen at 5000 | 1 | 0 | on |
| Software NMI (`NMI_Handler`, then `Safe_State`) | frozen at 5000 | 1 | 0 | on |

PA9 in mode 1 with MOE at 0 can only come from `Safe_State`: the rest of the firmware leaves PA9 in mode 2.
The pin level was read as 0 in the input register; it was not measured with an instrument, and a single read of
a 64 % PWM would not prove anything on its own.

Not exercised: a real loss of the 8 MHz (the CSS event itself, then the NMI with the CSS flag set), and the two
FreeRTOS hooks, which call `Error_Handler()` (checked by reading the code, not triggered).
The behaviour before `Safe_State` existed was not measured either, so no defect is claimed here.

### Memory

| Metric | Measured |
| --- | --- |
| Footprint, FreeRTOS enabled, no application tasks (`-O0`) | FLASH 18.91 KB / 1024 KB, RAM 8.98 KB / 96 KB, RAM2 0 / 32 KB |
| FreeRTOS heap usage | 0 allocations: heap\_4 never initialised, malloc-failed hook never hit |
| Stack high-water mark, tasks A / B / health (256 words each) | 222 / 223 / 222 words free (~34 used, no FPU context yet) |

### Key findings so far

1. **The naive response-time model is optimistic.** `R = C_B + ⌈R/T_A⌉·C_A` predicts 7 ms; the
measurement is 7.011 ms. The model ignores kernel preemption overhead, which must be measured
and added to the analysis.
2. **A measured maximum is not a bound.** At `-O0`, every one of 13 841 latency samples was
identical because the tick, the HAL time base and all task releases are phase-locked: no
interrupt ever fell inside the measurement window. TIM3 is derived from the same clock, so the
DMA interrupt is phase-locked too. Only analysis gives a worst case.
3. **The binary matters.** The same kernel on the same hardware wakes a task twice as fast
at `-O2` as at `-O0`, and even an unrelated code change moved the ISR-to-task minimum from
795 to 776 cycles by shifting code in flash. Adding the encoder input moved the maximum from
847 to 1599 cycles. Numbers are valid for one binary only.
4. **Start-up is not steady state.** The first wake-ups after reset cost up to 1432 cycles
(cold instruction cache) against 847 in steady state. Statistics skip a 10-sample warm-up.
5. **A debugger stops the core, not the peripherals.** Halting the core let the DMA overwrite
both buffer halves (4 late blocks). TIM3, TIM1 and TIM2 are now frozen on debug halt; for TIM1 this
also disables the PWM outputs, so a halted controller never keeps driving the motor.
6. **Clock error propagates.** The +0.07 % HSI error measured on MCO reappears in task timing:
a 5 ms busy-wait counted in cycles lasts 4.9965 ms.
7. **Know the instrument's resolution.** At 24 MS/s the analyser resolves the PWM duty to
1/1199 (0.08 %), coarser than the PWM itself (1/4000). A 0.08 % step between periods is the
instrument, not the signal. A count over about 1 s resolves a rate to about ±1 count, which is
50 ppm at 20 000 counts/s: only a long accumulation reaches 1 ppm.
8. **A reading is only meaningful together with the instant it was taken.** `PLLCFGR` read with the
core halted before `SystemClock_Config` returned the reset value, contradicting `CR`.
Live Expressions reads variables one after another while the program runs: with 1 ms between two
reads, two counters that must be equal differed, and the derived ppm was wrong (1 ms out of 90 s is
11 ppm). Coherent readings are now taken with the core halted: TIM2 is frozen on halt and DWT stops
with the core, so the count and the cycle counter stay consistent.
9. **Predict with the measured window, not the nominal one.** A 1 s delay lasts 999 903 µs, so 20 000
counts/s gives 19 998, not 20 000. Comparing rates (count divided by measured time) removes the effect.
10. **A clone must reproduce the configuration.** `sdkconfig` is generated and ignored by the ESP-IDF
template, so a clone silently fell back to defaults (160 MHz, 2 MB flash, 100 Hz tick, wrong target).
The chosen settings now live in `esp32-motor-sim/sdkconfig.defaults`, and a fresh clone is built and flashed
to check it.

**Open questions**

- Task-to-task latency varies by 53 cycles at `-O2` (503–556): tick interrupt inside the window
or instruction-cache state, to be decided with a latency histogram.
- ISR-to-task maximum is 1599 cycles with the encoder binary (815 / 863 / 1599 on 189 577 samples,
identical over three runs of different length) and 1579 with the fail-stop binary (809 / 857 / 1579 on
340 868 samples). A deterministic cause is likely; not identified.
- The ADC overrun counter has never been seen to increment: the error path must be exercised
by forcing an overrun (phase 5).
- The Clock Security System is enabled and its callback goes to the safe state. The callback and the NMI
path were exercised by software; a real loss of the 8 MHz (removing SB16 while running) has not been tested,
so the detection path is not claimed.
- The FreeRTOS hooks (stack overflow, malloc failed) call `Error_Handler()`; they have not been triggered.
- PLLSAI1 stays running because the ADC clock mux points to it in CubeMX, although the ADC uses HCLK/4.
- The MCO measurement on HSE is a check of the analyser's crystal as much as of the STM32's.

## Closed loop, links and RTOS layer (2026-10-10)

SYSCLK 80 MHz on HSE, GCC `-O2`. Motor model on ESP32 #1: first order, K = 3000 rpm at 100 % duty, τ = 100 ms, 500 lines.

### Plant and speed loop

| Metric | Predicted | Measured |
| --- | --- | --- |
| Open loop, 50 % duty | 1500 rpm, `rpm10` = 15000 | model 1500.0 rpm, STM32 `rpm10` = 15000 |
| Model time constant (free deceleration, ratio over 250 ms) | 100 ms | **99.5 ms** (ratio 0.081 vs e^-2.5 = 0.082) |
| PI step 0 → 1500 rpm, rise 10-90 % (offline simulation) | 103 ms | **103 ms** |
| Overshoot | 0 % | −0.07 % |
| Settling to 2 % | 260 ms | 273 ms: equivalent to 5-7 ms of loop delay not in the simulation |
| Steady state, encoder period rounded to whole ticks | – | model 1499.3 rpm while the STM32 reads 1500.0: 0.125 % frequency steps hide the error |
| Same, with period dithering (n / n+1 ticks, sigma-delta) | 1500.0 | **1500.0** (1499.6 to 1500.1) |

The stall protection fired on a real case (plant not running): duty 100 % without motion, motor output forced off
after 0.5 s, event reported to the supervisor, fault latched.

### Supervisor link and Wi-Fi

| Metric | Measured |
| --- | --- |
| Framed UART, 921 600 then 2 Mbaud | 0 CRC error, 0 frame lost (tens of thousands of frames) |
| Parser host tests (corruption at every byte, resynchronisation, false header) | pass, with AddressSanitizer and UBSan |
| Wi-Fi, home box at the bench | −82 dBm, **70 % ping loss** (Elyssa transmits at 11 dBm) |
| Wi-Fi, PC hotspot next to the bench | −45 dBm, 0 % loss |
| Ping with Wi-Fi modem sleep / without | 154 ms average / **2 ms** |
| Dashboard WebSocket | 100 telemetry samples/s, about 6 000 trace events/s, 0 lost |

### RTOS layer, first version (job markers in each task, about 4 min of run time)

| Task | Priority | T / D | C max | R max | CPU | Deadline misses |
| --- | --- | --- | --- | --- | --- | --- |
| acq | 22 | 1 / 1 ms | 286 µs | 300 µs | 3.0 % | 0 |
| ctxRx | 24 | 5 ms, sporadic | 14 µs | 25 µs | 0.4 % | – |
| taskA | 20 | 10 / 10 ms | 1955 µs | 2023 µs | 19.5 % | 0 |
| taskB | 16 | 25 / 25 ms | 4872 µs | 7081 µs | 19.5 % | 0 |
| ctxTx | 12 | 5 / 5 ms | 11 µs | **7123 µs** | 0.5 % | **7561** |
| link | 10 | 10 / 10 ms | 718 µs | **12123 µs** | 3.6 % | **223** |
| health | 8 | 1000 ms | 230 µs | 8150 µs | 0 % | 0 |
| idle | 0 | – | – | – | 53 % | – |

System: 3 100 context switches/s, worst context switch 954 cycles (11.9 µs), worst interrupt-to-task 1879 cycles (23.5 µs).

Findings: ctxTx (period 5 ms) and link (10 ms) have lower priorities than taskB (25 ms, 4.9 ms of computation), which is
not rate-monotonic: their deadlines are missed, exactly as the response-time analysis predicts. taskA's C (1955 µs) is
below its 2000 µs busy-wait because the wait counts wall time while the acquisition task and the DMA interrupt take
the CPU in between: elapsed time is not CPU time.
