#include "link.h"

static const uint32_t LINK_BAUD = 921600;
static const int      LINK_RX   = 44;
static const int      LINK_TX   = 43;
static const uint32_t LOST_MS   = 300;      /* no valid frame for this long: STM32 lost */
static const uint32_t ACK_MS    = 500;

static hil_parser_t parser;
static hil_seq_t    rxSeq;
static uint8_t      txSeq;
static Telemetry    last;
static bool         haveTelemetry;
static bool         alive;
static uint32_t     lastFrameMs;
static uint32_t     lostEvents;
static bool         cmdPending;
static uint32_t     cmdSentMs;

void link_begin()
{
  Serial1.setRxBufferSize(2048);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  hil_parser_init(&parser);
  hil_seq_init(&rxSeq);
}

static void sendFrame(uint8_t type, const uint8_t *payload, uint8_t len)
{
  uint8_t buf[HIL_OVERHEAD + 16];
  const size_t n = hil_encode(buf, type, txSeq++, payload, len);
  Serial1.write(buf, n);
}

void link_send_heartbeat()
{
  uint8_t pl[6];
  hil_put_u32(&pl[0], millis());
  pl[4] = 0;
  pl[5] = HIL_MODE_NONE;
  sendFrame(HIL_T_HEARTBEAT, pl, sizeof pl);
}

void link_send_mode(uint8_t mode)
{
  sendFrame(HIL_T_CMD_MODE, &mode, 1);
  cmdSentMs = millis();
  cmdPending = true;
}

void link_send_setpoint(float rpm, bool quiet)
{
  float v = rpm * 10.0f;
  if (v > 32767.0f) v = 32767.0f;
  if (v < -32768.0f) v = -32768.0f;
  uint8_t pl[2];
  hil_put_u16(pl, (uint16_t)(int16_t)v);
  sendFrame(HIL_T_CMD_SETPOINT, pl, sizeof pl);
  if (!quiet) {
    cmdSentMs = millis();
    cmdPending = true;
  }
}

static void handleFrame(const hil_frame_t &f)
{
  switch (f.type) {
    case HIL_T_TELEMETRY:
      if (f.len == 12) {
        last.t      = hil_get_u32(f.payload);
        last.sp10   = hil_get_i16(f.payload + 4);
        last.rpm10  = hil_get_i16(f.payload + 6);
        last.dutyPm = hil_get_u16(f.payload + 8);
        last.fault  = f.payload[10];
        last.mode   = f.payload[11];
        haveTelemetry = true;
        on_telemetry(last);
      }
      break;
    case HIL_T_EVENT:
      if (f.len == 9) {
        on_event(hil_get_u32(f.payload), f.payload[4], hil_get_i32(f.payload + 5));
      }
      break;
    case HIL_T_ACK:
      if (f.len == 4) {
        cmdPending = false;
        on_ack(f.payload[0], f.payload[1], hil_get_i16(f.payload + 2));
      }
      break;
    default:
      break;                                   /* heartbeat: presence only */
  }
}

void link_poll()
{
  hil_frame_t f;
  while (Serial1.available() > 0) {
    hil_parser_push(&parser, (uint8_t)Serial1.read());
    while (hil_parser_next(&parser, &f)) {
      /* The STM32 clock going backwards means it was reset: its sequence numbers
         restart at 0, which must not be counted as lost frames. */
      if (f.type == HIL_T_TELEMETRY && f.len == 12 && haveTelemetry &&
          (hil_get_u32(f.payload) + 1000U) < last.t) {
        hil_seq_init(&rxSeq);
        on_stm32_restart();
      }
      hil_seq_update(&rxSeq, f.seq);
      lastFrameMs = millis();
      if (!alive) {
        alive = true;
        on_link_state(true);
      }
      handleFrame(f);
    }
  }
  if (alive && (millis() - lastFrameMs) > LOST_MS) {
    alive = false;
    lostEvents++;
    on_link_state(false);
  }
  if (cmdPending && (millis() - cmdSentMs) > ACK_MS) {
    cmdPending = false;
    on_ack_timeout();
  }
}

bool             link_alive()          { return alive; }
bool             link_have_telemetry() { return haveTelemetry; }
const Telemetry &link_last()           { return last; }
uint32_t         link_frames_ok()      { return parser.framesOk; }
uint32_t         link_crc_errors()     { return parser.crcErrors; }
uint32_t         link_seq_lost()       { return rxSeq.lost; }
uint32_t         link_lost_events()    { return lostEvents; }

const char *link_mode_name(uint8_t m)
{
  switch (m) {
    case HIL_MODE_IDLE:   return "IDLE";
    case HIL_MODE_LOCAL:  return "LOCAL";
    case HIL_MODE_REMOTE: return "REMOTE";
    case HIL_MODE_TEST:   return "TEST";
    default:              return "?";
  }
}

const char *link_event_name(uint8_t code)
{
  switch (code) {
    case HIL_EVT_STALL:         return "STALL";
    case HIL_EVT_LINK_LOST:     return "LINK_LOST";
    case HIL_EVT_LINK_RESTORED: return "LINK_RESTORED";
    case HIL_EVT_MODE_CHANGED:  return "MODE_CHANGED";
    case HIL_EVT_CMD_REJECTED:  return "CMD_REJECTED";
    case HIL_EVT_SP_TIMEOUT:    return "SP_TIMEOUT";
    default:                    return "?";
  }
}
