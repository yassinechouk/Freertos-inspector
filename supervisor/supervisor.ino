/* Supervisor node (Elyssa, ESP32-S3) - step 1: framed UART link with the STM32.

   - Decodes the frames of hil_proto.h (CRC, sequence gaps), prints telemetry,
     events and acknowledgements on the USB serial console.
   - Detects the loss of the STM32 (no valid frame for LOST_MS) and reports it.
   - Sends a heartbeat every 100 ms: it is the keep-alive of the STM32 dead-man timer.
   - Console commands (USB serial, 115200, end each line with Enter):
       mode idle | local | remote | test     select the controller mode
       sp <rpm>                               requested speed, used in REMOTE mode
       stat                                   link statistics
     The STM32 is the authority: it clamps, ramps and may refuse; the ACK says what it accepted.

   UART link to the STM32 (3.3 V logic, common GND, never join the 3V3 rails):
     Elyssa RX (GPIO44) <- STM32 PB6 (TX)      Elyssa TX (GPIO43) -> STM32 PB7 (RX)
   Serial = USB (USB CDC On Boot enabled); Serial1 = the STM32 link, remapped to GPIO44/43. */

#include "hil_proto.h"

static const uint32_t LINK_BAUD = 921600;
static const int      LINK_RX   = 44;
static const int      LINK_TX   = 43;
static const uint32_t LOST_MS   = 300;       /* no valid frame for this long: STM32 lost */
static const uint32_t PRINT_MS  = 500;       /* telemetry line period on the console */

static hil_parser_t parser;
static hil_seq_t    rxSeq;
static uint8_t      txSeq;

/* Latest telemetry. */
static uint32_t tT;
static int16_t  tSp, tRpm10;
static uint16_t tDuty;
static uint8_t  tFault, tMode;
static bool     haveTelemetry;

static uint32_t lastFrameMs, lastHbMs, lastPrintMs;
static bool     alive;                        /* STM32 considered present */
static uint32_t lostEvents;

static char     cmdBuf[40];
static uint8_t  cmdLen;

static const char *modeName(uint8_t m)
{
  switch (m) {
    case HIL_MODE_IDLE:   return "IDLE";
    case HIL_MODE_LOCAL:  return "LOCAL";
    case HIL_MODE_REMOTE: return "REMOTE";
    case HIL_MODE_TEST:   return "TEST";
    default:              return "?";
  }
}

static void sendFrame(uint8_t type, const uint8_t *payload, uint8_t len)
{
  uint8_t buf[HIL_OVERHEAD + 16];
  const size_t n = hil_encode(buf, type, txSeq++, payload, len);
  Serial1.write(buf, n);
}

static void sendHeartbeat()
{
  uint8_t pl[6];
  hil_put_u32(&pl[0], millis());
  pl[4] = 0;
  pl[5] = HIL_MODE_NONE;
  sendFrame(HIL_T_HEARTBEAT, pl, sizeof pl);
}

static void sendMode(uint8_t mode)
{
  sendFrame(HIL_T_CMD_MODE, &mode, 1);
}

static void sendSetpoint(float rpm)
{
  float v = rpm * 10.0f;
  if (v > 32767.0f) v = 32767.0f;
  if (v < -32768.0f) v = -32768.0f;
  uint8_t pl[2];
  hil_put_u16(pl, (uint16_t)(int16_t)v);
  sendFrame(HIL_T_CMD_SETPOINT, pl, sizeof pl);
}

static void handleFrame(const hil_frame_t &f)
{
  switch (f.type) {
    case HIL_T_TELEMETRY:
      if (f.len == 12) {
        tT      = hil_get_u32(f.payload);
        tSp     = hil_get_i16(f.payload + 4);
        tRpm10  = hil_get_i16(f.payload + 6);
        tDuty   = hil_get_u16(f.payload + 8);
        tFault  = f.payload[10];
        tMode   = f.payload[11];
        haveTelemetry = true;
      }
      break;
    case HIL_T_HEARTBEAT:
      break;                                   /* presence only */
    case HIL_T_EVENT:
      if (f.len == 9) {
        Serial.printf("[EVENT] t=%lu ms code=%u arg=%ld\n",
                      (unsigned long)hil_get_u32(f.payload), f.payload[4], (long)hil_get_i32(f.payload + 5));
      }
      break;
    case HIL_T_ACK:
      if (f.len == 4) {
        static const char *st[] = { "ok", "clamped", "rejected" };
        const uint8_t s = f.payload[1];
        Serial.printf("[ACK] type=0x%02X %s value=%d\n", f.payload[0], s < 3 ? st[s] : "?", (int)hil_get_i16(f.payload + 2));
      }
      break;
    default:
      Serial.printf("[RX] unhandled type 0x%02X len=%u\n", f.type, f.len);
      break;
  }
}

static void pollLink()
{
  hil_frame_t f;
  while (Serial1.available() > 0) {
    hil_parser_push(&parser, (uint8_t)Serial1.read());
    while (hil_parser_next(&parser, &f)) {
      hil_seq_update(&rxSeq, f.seq);
      lastFrameMs = millis();
      if (!alive) {
        alive = true;
        Serial.println("[LINK] STM32 present");
      }
      handleFrame(f);
    }
  }
  if (alive && (millis() - lastFrameMs) > LOST_MS) {
    alive = false;
    lostEvents++;
    Serial.println("[LINK] STM32 LOST (no valid frame)");
  }
}

static void printStats()
{
  Serial.printf("[STAT] frames ok=%lu crc errors=%lu bad len=%lu lost(seq)=%lu link lost events=%lu\n",
                (unsigned long)parser.framesOk, (unsigned long)parser.crcErrors, (unsigned long)parser.badLen,
                (unsigned long)rxSeq.lost, (unsigned long)lostEvents);
}

static void handleCommand(char *line)
{
  char word[12] = {0};
  float v = 0;
  if (sscanf(line, "mode %11s", word) == 1) {
    if      (!strcmp(word, "idle"))   sendMode(HIL_MODE_IDLE);
    else if (!strcmp(word, "local"))  sendMode(HIL_MODE_LOCAL);
    else if (!strcmp(word, "remote")) sendMode(HIL_MODE_REMOTE);
    else if (!strcmp(word, "test"))   sendMode(HIL_MODE_TEST);
    else Serial.println("modes: idle local remote test");
  } else if (sscanf(line, "sp %f", &v) == 1) {
    sendSetpoint(v);
  } else if (!strncmp(line, "stat", 4)) {
    printStats();
  } else if (line[0] != '\0') {
    Serial.println("commands: mode idle|local|remote|test, sp <rpm>, stat");
  }
}

static void pollConsole()
{
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      cmdBuf[cmdLen] = '\0';
      handleCommand(cmdBuf);
      cmdLen = 0;
    } else if (cmdLen < sizeof cmdBuf - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

static void updateLed()
{
  if (!alive)                          setLedColor(ELYSSA_YELLOW);   /* no STM32 */
  else if (haveTelemetry && tFault)    setLedColor(ELYSSA_RED);      /* motor fault */
  else                                 setLedColor(ELYSSA_GREEN);
}

void setup()
{
  Serial.begin(115200);
  Serial1.setRxBufferSize(2048);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  hil_parser_init(&parser);
  hil_seq_init(&rxSeq);
  setLedColor(ELYSSA_YELLOW);
  Serial.println("supervisor ready: link 921600 8N1, commands: mode idle|local|remote|test, sp <rpm>, stat");
}

void loop()
{
  const uint32_t now = millis();

  pollLink();
  pollConsole();

  if (now - lastHbMs >= 100) {
    lastHbMs = now;
    sendHeartbeat();
  }

  if (now - lastPrintMs >= PRINT_MS) {
    lastPrintMs = now;
    if (haveTelemetry && alive) {
      Serial.printf("t=%lu mode=%s sp=%.1f rpm=%.1f duty=%.1f%% fault=%u | rx ok=%lu crc=%lu lost=%lu\n",
                    (unsigned long)tT, modeName(tMode), tSp / 10.0f, tRpm10 / 10.0f, tDuty / 10.0f, tFault,
                    (unsigned long)parser.framesOk, (unsigned long)parser.crcErrors, (unsigned long)rxSeq.lost);
    } else if (!alive) {
      Serial.println("waiting for the STM32 ...");
    }
    updateLed();
  }
}
