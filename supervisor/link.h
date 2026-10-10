/* STM32 link: hil_proto.h frames over Serial1 (2 Mbaud 8N1, RX GPIO44, TX GPIO43). */
#pragma once
#include <Arduino.h>
#include "hil_proto.h"

struct Telemetry
{
  uint32_t t;          /* STM32 time, ms */
  int16_t  sp10;       /* setpoint, 0.1 rpm */
  int16_t  rpm10;      /* measured speed, 0.1 rpm */
  uint16_t dutyPm;     /* duty, per mille */
  uint8_t  fault;
  uint8_t  mode;
};

void            link_begin();
void            link_poll();                       /* call often: parses, detects loss and missing ACKs */
void            link_send_heartbeat();
void            link_send_mode(uint8_t mode);      /* explicit command: an ACK is expected */
void            link_send_fault(uint8_t fault);    /* HIL_FAULT_*: deliberate fault on the target */
void            link_send_setpoint(float rpm, bool quiet);
bool            link_alive();
bool            link_have_telemetry();
const Telemetry &link_last();
uint32_t        link_frames_ok();
uint32_t        link_crc_errors();
uint32_t        link_seq_lost();
uint32_t        link_lost_events();
const char     *link_mode_name(uint8_t m);
const char     *link_event_name(uint8_t code);

/* Callbacks, implemented by the sketch. */
void on_telemetry(const Telemetry &t);
void on_event(uint32_t t, uint8_t code, int32_t arg);
void on_ack(uint8_t type, uint8_t status, int16_t value);
void on_ack_timeout();
void on_link_state(bool alive);
void on_stm32_restart();
/* RTOS layer frames (payload as defined in hil_proto.h) */
void on_task_table(const uint8_t *p, uint8_t len);
void on_task_stats(const uint8_t *p, uint8_t len);
void on_rtos_sys(const uint8_t *p, uint8_t len);
void on_trace(const uint8_t *p, uint8_t len);
void on_queue_stats(const uint8_t *p, uint8_t len);
