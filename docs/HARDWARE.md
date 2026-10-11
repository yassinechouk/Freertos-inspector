# Hardware


- NUCLEO-L476RG (MB1136 C-05). **Modification done**: SB16 and SB50 ON, SB55 OFF, so that
HSE receives the 8 MHz MCO from the ST-LINK (bypass mode). On this board the MCO is not connected
to OSC_IN by default, whatever the revision (UM1724 §6.7.1).
- 2 × ESP32. ESP32 #1 is an ESP32-S3 (QFN56, revision v0.2, 16 MB flash, 8 MB embedded PSRAM).
- Potentiometer (speed setpoint): ends on 3V3 and GND, wiper on A0. Never on 5 V.
- Push buttons, LEDs, I2C RTC module (fault timestamps)
- Logic analyser (8 ch, 24 MS/s)

## Pin map (STM32)

| Pin | Arduino | Function |
| --- | --- | --- |
| PA0 | A0 | Setpoint potentiometer (ADC1 IN5) |
| PA9 | D8 | Motor PWM (TIM1 CH2, 20 kHz) |
| PA15 | — | Encoder channel A (TIM2 CH1, AF1) |
| PA1 | A1 | Encoder channel B (TIM2 CH2, AF1) |
| PA6 | D12 | Reserved: TIM1 break input (hardware emergency stop, phase 5) |
| PA8 | D7 | MCO (SYSCLK/16) |
| PA10 | D2 | `TP_TASK_A` timing pin |
| PB5 | D4 | `TP_TASK_B` timing pin |
| PB4 | D5 | `TP_TASK_C` timing pin (acquisition task) |
| PB10 | D6 | `TP_ISR` timing pin (DMA interrupt) |
| PA5 | D13 | `LED_FAULT` (LD2) |
| PA13 / PA14 / PB3 | — | SWD + SWO (reserved) |

## Pin map (ESP32-S3, ESP32 #1)

| GPIO | Function |
| --- | --- |
| 4 | Quadrature channel A output (MCPWM) → STM32 PA15 |
| 5 | Quadrature channel B output (MCPWM) → STM32 PA1 |
| 6 / 7 | PCNT test inputs, wired to GPIO 4 / 5 for the standalone loopback test only |
| GND | Common with the STM32 board. The 3V3 rails are never connected together. |

## Current wiring (2026-10)

![Bench wiring](images/bench-wiring.svg)


| From | To | Signal |
| --- | --- | --- |
| STM32 PA9 (D8) | ESP32 #1 GPIO14 | Motor PWM, 20 kHz, measured by the plant model (MCPWM capture) |
| ESP32 #1 GPIO4 / GPIO5 | STM32 PA15 / PA1 | Quadrature encoder A / B |
| STM32 PB6 (D10) | Elyssa GPIO44 (RX) | Target → supervisor frames, 2 Mbaud |
| Elyssa GPIO43 (TX) | STM32 PB7 (CN7 pin 21) | Supervisor → target commands |
| GND | GND | Common ground for the three boards. The 3V3 rails are never connected together. |

Supervisor: **Elyssa** dev board (ESP32-S3FN8, 8 MB flash, native USB, RGB LED), programmed with the Arduino IDE
(board package "Moovma"). Its Wi-Fi transmitter is limited to 11 dBm by the board package: keep it close to the
access point (a PC hotspot next to the bench works well).

ESP32 #1 is built in Docker (`esp32-motor-sim/build_flash.ps1`) because Windows Smart App Control blocks the
unsigned ESP-IDF toolchain on the development PC. Do not leave a serial monitor open on its CH340 port during tests:
its DTR/RTS lines can hold the ESP32 in reset.
