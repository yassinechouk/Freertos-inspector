# Original roadmap (bench phases 0 to 9)

The phase plan the project started with, kept for reference. The current status is in the README.


### Phase 0 — Foundations

- [x] Repository structure
- [x] CubeMX project for NUCLEO-L476RG
- [x] Clock tree designed: HSE bypass → PLL → 80 MHz, MCO on PA8
- [x] HSE start-up failure diagnosed with debugger and UM1724
- [x] Temporary clock: HSI → PLL → 80 MHz, verified on MCO
- [x] Board modification (SB16, SB50 ON, SB55 OFF)
- [x] Switch back to HSE, verified by registers, and repeat the MCO measurement

### Phase 1 — STM32 real-time core

- [x] FreeRTOS, native API, static allocation only
- [x] HAL time base moved to TIM6, SysTick left to the kernel
- [x] Two periodic tasks with dedicated timing pins
- [x] DWT cycle counter as the measurement base
- [x] First response-time analysis, predicted then measured
- [x] Health task: heap usage and stack high-water marks
- [x] Fail-stop fault handling with visible LED
- [x] Notify-to-wake latency measured at `-O0` and `-O2`
- [ ] Latency histogram to explain the `-O2` variation and the 1599-cycle ISR maximum

### Phase 2 — Controller I/O

- [x] Setpoint acquisition: timer-triggered ADC, circular DMA, double buffer
- [x] ISR → task hand-off with task notifications, latency measured
- [x] PWM output to the motor (TIM1, 20 kHz, preload), open loop from setpoint
- [x] TIM1 / TIM2 / TIM3 frozen on debug halt
- [x] Quadrature encoder input (TIM2, encoder mode x4), checked against an independent time base
- [ ] Speed estimate from the count (fixed window; resolution 60 000 / (4·N) rpm per count at 1 ms, N = pulses per revolution)
- [ ] Health task extended to the new tasks' stacks
- [ ] Phase-1 experiment tasks (A, B, ctxRx, ctxTx) behind a build switch

### Phase 3 — Plant simulator (ESP32 #1)

- [ ] DC motor model, fixed-step integration, parameters from a real motor datasheet
- [ ] PWM duty measurement
- [x] Quadrature signal generation (MCPWM, verified with the PCNT and read by the STM32)
- [ ] Simulator characterised: step jitter, signal timing accuracy

### Phase 4 — Closed-loop control

- [ ] Speed control loop (PI) at a fixed rate
- [ ] Shared state protected by mutex, priority inversion reproduced and resolved
- [ ] I2C RTC as slow peripheral task
- [ ] Step response validated against the model

### Phase 5 — Fault management

- [ ] State machine: normal / alert / fault
- [ ] Fault injection from the simulator: stall, encoder loss, noise
- [ ] Detection deadlines specified and verified
- [ ] Hardware emergency stop
- [x] Safe state (motor output forced low, LD2 on) reachable from `Error_Handler`, the NMI and the CSS callback
- [ ] Clock-loss fault (CSS): callback and NMI path exercised by software; test with a real loss of the HSE

### Phase 6 — Supervisor and links (ESP32 #2)

- [ ] Framed UART protocol: sequence numbers, CRC, heartbeat
- [ ] Node-loss detection
- [ ] Wi-Fi gateway to the PC

### Phase 7 — Dashboard

- [ ] Live setpoint and speed curves
- [ ] Fault log with timestamps
- [ ] Task timeline built from traces

### Phase 8 — Timing analysis and measurement campaign

- [ ] Response-time analysis including measured kernel overhead
- [ ] End-to-end latency, from fault to dashboard
- [ ] CPU load per task, trace intrusiveness

### Phase 9 — Hardening

- [ ] Watchdog, degraded modes
- [ ] Long-run tests (24 h)
- [ ] Final documentation
