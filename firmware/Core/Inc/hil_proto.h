/* GENERATED COPY of protocol/hil_proto.h - do not edit; run tools/sync_proto.ps1 */
/* HIL bench link protocol: STM32 <-> supervisor (Elyssa), UART 921600 8N1.
   Single source of truth. Copies live in firmware/Core/Inc and supervisor/
   (see tools/sync_proto.ps1): edit THIS file, then run the script.

   Frame (little-endian fields):
     | 0xA5 | 0x5A | LEN | TYPE | SEQ | payload (LEN bytes) | CRC16 (2 bytes) |
   CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) over LEN, TYPE, SEQ, payload.
   SEQ is a per-sender counter, +1 per frame (wraps at 256): the receiver counts
   the gaps. Header-only, plain C99, no dynamic allocation. */
#ifndef HIL_PROTO_H
#define HIL_PROTO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HIL_SOF0          0xA5u
#define HIL_SOF1          0x5Au
#define HIL_MAX_PAYLOAD   128u
#define HIL_OVERHEAD      7u        /* SOF0 SOF1 LEN TYPE SEQ CRC_LO CRC_HI */

/* ---- Frame types ------------------------------------------------------- */
/* STM32 -> supervisor */
#define HIL_T_HEARTBEAT   0x01u     /* u32 uptime_ms, u8 fault, u8 mode (also supervisor -> STM32) */
#define HIL_T_TELEMETRY   0x02u     /* u32 t_ms, i16 sp_rpm10, i16 rpm10, u16 duty_permille, u8 fault, u8 mode */
/* RTOS layer. Times are CPU cycles (f_cpu given in TASK_TABLE and RTOS_SYS). */
#define HIL_T_TASK_TABLE  0x10u     /* static configuration of one task, sent every few seconds:
                                       u8 tag, u8 priority, u16 period_ms (0 = sporadic), u16 deadline_ms (0 = none),
                                       u32 wcet_cycles (declared C, 0 = unknown), u16 stack_words, u32 f_cpu_hz,
                                       name: the rest of the payload (ASCII, no terminator) */
#define HIL_T_TASK_STATS  0x11u     /* measurements of one task, once per second:
                                       u8 tag, u16 cpu_permille (last second), u16 jobs (last second),
                                       u32 c_max, u32 c_avg, u32 r_max, u32 r_min (cycles, since reset),
                                       u16 deadline_misses (since reset), u16 stack_free_words */
#define HIL_T_TRACE       0x12u     /* continuous trace, a burst of events:
                                       u32 t0 (cycles), u16 dropped (events lost so far, wraps),
                                       then N x { u8 id, u24 delta } with delta in cycles from the previous
                                       event (the first one from t0). Event ids: HIL_TR_* below */
#define HIL_T_RTOS_SYS    0x13u     /* once per second: u32 f_cpu_hz, u32 switches (last second),
                                       u32 trace_events (last second), u32 trace_dropped (total),
                                       u16 idle_permille, u16 tick_hz,
                                       u32 ctx_switch_max, u32 isr_to_task_max (cycles, measured since reset) */
#define HIL_T_EVENT       0x20u     /* u32 t_ms, u8 code, i32 arg */
#define HIL_T_ACK         0x82u     /* u8 acked_type, u8 status, i16 value */
/* supervisor -> STM32 */
#define HIL_T_CMD_SETPOINT 0x80u    /* i16 rpm10 (requested speed, used in REMOTE mode) */
#define HIL_T_CMD_MODE     0x81u    /* u8 mode */

/* ---- Control modes ----------------------------------------------------- */
#define HIL_MODE_IDLE     0u        /* setpoint 0; default after reset */
#define HIL_MODE_LOCAL    1u        /* potentiometer */
#define HIL_MODE_REMOTE   2u        /* setpoint received from the supervisor */
#define HIL_MODE_TEST     3u        /* software square wave (step-response test) */
#define HIL_MODE_NONE     0xFFu     /* "not applicable" (supervisor heartbeat) */

/* ---- Event codes (HIL_T_EVENT) ------------------------------------------ */
#define HIL_EVT_STALL          1u   /* arg = 0: motor driven hard without motion, output forced off */
#define HIL_EVT_LINK_LOST      2u   /* arg = 0: no frame at all from the supervisor for 1 s */
#define HIL_EVT_LINK_RESTORED  3u
#define HIL_EVT_MODE_CHANGED   4u   /* arg = new mode */
#define HIL_EVT_CMD_REJECTED   5u   /* arg = frame type */
#define HIL_EVT_SP_TIMEOUT     6u   /* arg = 0: REMOTE setpoint not refreshed for 1 s, setpoint -> 0.
                                       The operator must resend the setpoint at least every second:
                                       a supervisor that is merely alive does not keep the motor running. */

/* ---- Trace event ids (HIL_T_TRACE) --------------------------------------- */
#define HIL_TR_TASK_MAX   0x3Fu     /* 0x00..0x3F: task with this tag switched in (0 = untagged task) */
#define HIL_TR_ISR_IN     0x40u     /* 0x40 | isr: interrupt handler entered */
#define HIL_TR_ISR_OUT    0x80u     /* 0x80 | isr: interrupt handler left */
#define HIL_TR_TICK       0xC0u     /* RTOS tick (instant) */
#define HIL_ISR_ADC_DMA   1u        /* ADC DMA half/full transfer: wakes the acquisition task */

/* ---- ACK status ---------------------------------------------------------- */
#define HIL_ACK_OK        0u
#define HIL_ACK_CLAMPED   1u        /* value limited: ACK carries the accepted one */
#define HIL_ACK_REJECTED  2u

/* ---- Little-endian helpers ------------------------------------------------ */
static inline void hil_put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void hil_put_u32(uint8_t *p, uint32_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }
static inline uint16_t hil_get_u16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static inline uint32_t hil_get_u32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline int16_t hil_get_i16(const uint8_t *p) { return (int16_t)hil_get_u16(p); }
static inline int32_t hil_get_i32(const uint8_t *p) { return (int32_t)hil_get_u32(p); }

/* ---- CRC-16/CCITT-FALSE ---------------------------------------------------- */
static inline uint16_t hil_crc16_update(uint16_t crc, uint8_t b)
{
  crc ^= (uint16_t)((uint16_t)b << 8);
  for (uint8_t i = 0u; i < 8u; i++)
  {
    const uint32_t c = (uint32_t)crc << 1;
    crc = (uint16_t)(((crc & 0x8000u) != 0u) ? (c ^ 0x1021u) : c);
  }
  return crc;
}

/* ---- Encoder -------------------------------------------------------------- */
/* Writes one frame to out (needs HIL_OVERHEAD + len bytes). Returns the frame length. */
static inline size_t hil_encode(uint8_t *out, uint8_t type, uint8_t seq,
                                const uint8_t *payload, uint8_t len)
{
  size_t n = 0u;
  uint16_t crc = 0xFFFFu;

  out[n++] = (uint8_t)HIL_SOF0;
  out[n++] = (uint8_t)HIL_SOF1;
  out[n++] = len;   crc = hil_crc16_update(crc, len);
  out[n++] = type;  crc = hil_crc16_update(crc, type);
  out[n++] = seq;   crc = hil_crc16_update(crc, seq);
  for (uint8_t i = 0u; i < len; i++)
  {
    out[n++] = payload[i];
    crc = hil_crc16_update(crc, payload[i]);
  }
  out[n++] = (uint8_t)crc;
  out[n++] = (uint8_t)(crc >> 8);
  return n;
}

/* ---- Parser (byte-by-byte state machine with resynchronisation) -----------------
   Usage:   hil_parser_push(&p, byte);  while (hil_parser_next(&p, &f)) { ...use f... }
   A bad CRC (or an impossible LEN) may be caused by a false header inside noise,
   in which case the real frames are hidden in the bytes just consumed. The parser
   therefore keeps every byte since the last SOF0 and, on failure, replays them
   (minus the first) so that no valid frame is lost. */
#define HIL_RAW_MAX   (HIL_OVERHEAD + HIL_MAX_PAYLOAD)
#define HIL_QCAP      (2u * HIL_RAW_MAX)

typedef struct
{
  uint8_t  type;
  uint8_t  seq;
  uint8_t  len;
  const uint8_t *payload;       /* valid until the next call to hil_parser_push()/next() */
} hil_frame_t;

typedef struct
{
  uint8_t  state;
  uint8_t  len, type, seq, idx;
  uint16_t crc;                 /* running CRC */
  uint16_t crcRx;
  uint8_t  raw[HIL_RAW_MAX];    /* bytes since the last SOF0 */
  uint8_t  rawN;
  uint8_t  q[HIL_QCAP];         /* bytes waiting to be parsed (new input and replays) */
  uint16_t qHead, qCount;
  uint32_t framesOk;
  uint32_t crcErrors;           /* complete frames rejected by the CRC */
  uint32_t badLen;              /* LEN field above HIL_MAX_PAYLOAD */
  uint32_t overflow;            /* input dropped because the queue was full */
} hil_parser_t;

enum { HIL_S_SOF0, HIL_S_SOF1, HIL_S_LEN, HIL_S_TYPE, HIL_S_SEQ, HIL_S_DATA, HIL_S_CRC0, HIL_S_CRC1 };

static inline void hil_parser_init(hil_parser_t *p)
{
  p->state = HIL_S_SOF0; p->len = 0u; p->type = 0u; p->seq = 0u; p->idx = 0u;
  p->crc = 0xFFFFu; p->crcRx = 0u; p->rawN = 0u; p->qHead = 0u; p->qCount = 0u;
  p->framesOk = 0u; p->crcErrors = 0u; p->badLen = 0u; p->overflow = 0u;
}

/* Queues one received byte. */
static inline void hil_parser_push(hil_parser_t *p, uint8_t b)
{
  if (p->qCount >= HIL_QCAP) { p->overflow++; return; }
  if ((uint32_t)p->qHead + p->qCount >= HIL_QCAP)
  {
    for (uint16_t i = 0u; i < p->qCount; i++) { p->q[i] = p->q[p->qHead + i]; }
    p->qHead = 0u;
  }
  p->q[p->qHead + p->qCount] = b;
  p->qCount++;
}

/* Frame rejected: put the bytes after the first SOF0 back at the front of the queue. */
static inline void hil_parser_replay(hil_parser_t *p)
{
  const uint16_t n = (uint16_t)(p->rawN - 1u);
  uint16_t keep = p->qCount;
  if (p->qHead != 0u)                                   /* bring the pending bytes to the start */
  {
    for (uint16_t i = 0u; i < keep; i++) { p->q[i] = p->q[p->qHead + i]; }
    p->qHead = 0u;
  }
  if ((uint32_t)n + keep > HIL_QCAP)
  {
    p->overflow += (uint32_t)n + keep - HIL_QCAP;
    keep = (uint16_t)(HIL_QCAP - n);
  }
  for (uint16_t i = keep; i > 0u; i--) { p->q[n + i - 1u] = p->q[i - 1u]; }   /* shift up by n */
  for (uint16_t i = 0u; i < n; i++) { p->q[i] = p->raw[1u + i]; }
  p->qHead = 0u; p->qCount = (uint16_t)(n + keep);
  p->state = HIL_S_SOF0; p->rawN = 0u;
}

/* One state-machine step. Returns 1 when a valid frame has just been completed. */
static inline int hil_parser_step(hil_parser_t *p, uint8_t b, hil_frame_t *f)
{
  if (p->state != HIL_S_SOF0) { p->raw[p->rawN++] = b; }

  switch (p->state)
  {
    case HIL_S_SOF0:
      if (b == HIL_SOF0) { p->raw[0] = b; p->rawN = 1u; p->state = HIL_S_SOF1; }
      break;
    case HIL_S_SOF1:
      if (b == HIL_SOF1) { p->state = HIL_S_LEN; p->crc = 0xFFFFu; }
      else               { hil_parser_replay(p); }   /* not a header: look again from this byte */
      break;
    case HIL_S_LEN:
      if (b > HIL_MAX_PAYLOAD) { p->badLen++; hil_parser_replay(p); break; }
      p->len = b; p->crc = hil_crc16_update(p->crc, b); p->state = HIL_S_TYPE;
      break;
    case HIL_S_TYPE:
      p->type = b; p->crc = hil_crc16_update(p->crc, b); p->state = HIL_S_SEQ;
      break;
    case HIL_S_SEQ:
      p->seq = b; p->crc = hil_crc16_update(p->crc, b); p->idx = 0u;
      p->state = (p->len == 0u) ? HIL_S_CRC0 : HIL_S_DATA;
      break;
    case HIL_S_DATA:
      p->crc = hil_crc16_update(p->crc, b);
      if (++p->idx >= p->len) { p->state = HIL_S_CRC0; }
      break;
    case HIL_S_CRC0:
      p->crcRx = b; p->state = HIL_S_CRC1;
      break;
    case HIL_S_CRC1:
      p->crcRx |= (uint16_t)((uint16_t)b << 8);
      if (p->crcRx == p->crc)
      {
        p->framesOk++;
        f->type = p->type; f->seq = p->seq; f->len = p->len; f->payload = &p->raw[5];
        p->state = HIL_S_SOF0; p->rawN = 0u;
        return 1;
      }
      p->crcErrors++;
      hil_parser_replay(p);
      break;
    default:
      p->state = HIL_S_SOF0; p->rawN = 0u;
      break;
  }
  return 0;
}

/* Parses queued bytes until the next valid frame. Returns 0 when the queue is empty. */
static inline int hil_parser_next(hil_parser_t *p, hil_frame_t *f)
{
  while (p->qCount > 0u)
  {
    const uint8_t b = p->q[p->qHead++];
    p->qCount--;
    if (p->qCount == 0u) { p->qHead = 0u; }
    if (hil_parser_step(p, b, f) != 0) { return 1; }
  }
  return 0;
}

/* ---- Sequence-gap tracker (one per direction) ------------------------------------ */
typedef struct { uint8_t last; uint8_t have; uint32_t lost; } hil_seq_t;

static inline void hil_seq_init(hil_seq_t *s) { s->last = 0u; s->have = 0u; s->lost = 0u; }

/* Call for every valid frame. Adds the number of frames missing since the previous one. */
static inline void hil_seq_update(hil_seq_t *s, uint8_t seq)
{
  if (s->have != 0u) { s->lost += (uint8_t)(seq - (uint8_t)(s->last + 1u)); }
  s->last = seq; s->have = 1u;
}

#ifdef __cplusplus
}
#endif

#endif /* HIL_PROTO_H */
