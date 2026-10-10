# Lessons learned

Things that went wrong or surprised, and what they taught. The measurement-related findings of the
early phases are in [MEASUREMENTS.md](MEASUREMENTS.md#key-findings-so-far).

## Real-time and RTOS

1. **Priorities must come from deadlines.** The first instrumented run showed two tasks missing deadlines
   (ctxTx: 7 561 misses, link: 223) for one reason: their periods were shorter than taskB's but their
   priorities lower. The response-time analysis predicts it without running anything.
2. **Elapsed time is not CPU time.** A 2 000 µs busy-wait measured 1 955 µs of CPU time: higher-priority
   work ran during the wait. A profiler that measures wall time between two points measures interference too.
3. **A finite response time can still be a miss, and 75 % above you is not starvation.** Two unit tests
   first failed because the predictions written for them were wrong: R converged at 12 (past a deadline of 8),
   and at 24 with 75 % of the CPU taken above, not to infinity. Divergence needs 100 % taken above.
4. **A job ends when the task blocks.** Telling "blocked" from "preempted" at switch-out is a read-only check
   of the ready list; it made the probe generic (no code in the tasks) instead of relying on markers.
5. **The tool must prove its detectors.** Each detector has a fault in the fault lab that triggers it.
   The first bench run also tested the detectors themselves: "starved" fired every second for a task
   that runs a few microseconds once per second (0 per mille, and ready at the instant of the check).
   The rule became "released more than 1 s ago and never given the CPU since".
6. **A late periodic task merges its jobs.** After a 150 ms CPU hog, `vTaskDelayUntil()` returns without
   blocking until the task has caught up: six periods of taskB ran as one job, C = 28 ms instead of
   4.9 ms, and the analysis built on "worst C since reset" announced 197 % utilisation. The probe now
   also reports the worst of the last second, and the analysis uses the last 10 s.
7. **heap_4 reads 0 free bytes before the first allocation.** With static allocation only, the free list
   is never initialised: the dashboard showed an empty heap, and the heap-pressure fault allocated nothing.

## Signals and control

6. **Quantisation can hide an error from a controller.** With whole-tick encoder periods, the plant ran at
   1 499.3 rpm while the controller measured exactly 1 500.0 and was satisfied. Dithering the period
   (sigma-delta between n and n+1 ticks) removed the dead zone: the integral sees the error again.
7. **Saturation changes the step response.** The PI rise time was 103 ms, not the 73 ms of the linear model:
   the step drives the duty to 100 %. Anti-windup kept the overshoot at zero.
8. **A display is a sampler too.** 3 000 rpm is 50 rev/s; a 60 Hz screen cannot show it (Nyquist). The 3D
   motor is slowed down 25 times, and says so.

## Links and tools

9. **A parser must survive a false header.** Noise containing `A5 5A` with a plausible length swallowed
   the following real frames. The fix (replay the bytes after a CRC failure) came from a host test, not from
   the bench, where the bug would have looked like random frame loss.
10. **Radio link budget matters.** At −82 dBm with an 11 dBm transmitter, 70 % of the pings were lost; a hotspot
    one metre away gave −45 dBm and 0 %. Disabling Wi-Fi modem sleep cut the ping from 154 ms to 2 ms.
11. **A serial monitor can hold a board in reset.** The DTR/RTS lines of the ESP32's USB-UART bridge are wired to
    EN and BOOT; an open monitor stopped the plant, and the stall protection rightly cut the motor.
12. **The toolchain is part of the system.** Windows Smart App Control silently blocked the unsigned ESP-IDF
    archiver; the build moved to Docker, and the old binary kept being flashed until the cause was found.
14. **A peripheral can die silently after an error.** A framing error (most likely the supervisor's boot
    ROM log at 115200 baud arriving on a 2 Mbaud receiver) made the HAL abort the UART DMA reception
    but left the DMA channel BUSY; every restart attempt then failed, and the target stopped hearing
    commands while still sending telemetry. Seen live on the bench: a register dump over SWD
    (`RxState` READY, DMA state BUSY, `CR3.DMAR` 0, `ISR.FE` 1) gave the cause in a minute. The receiver
    now resets both sides and restarts on error or after 2 s of silence.
15. **Flag stale data.** A Live Expressions view showing statistics accumulated before the plant was running
    looked like a broken encoder. Worst cases "since reset" need a reset before a test.
