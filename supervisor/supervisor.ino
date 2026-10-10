/* Supervisor node (Elyssa, ESP32-S3) - step 2: STM32 link + Wi-Fi + web dashboard.

   Modules (same folder): link.* (STM32 frames), net.* (Wi-Fi, setup portal),
   web.* (HTTP server, WebSocket), web/ (dashboard sources; embedded into assets.h by tools/embed_assets.py), hil_proto.h (copy, see tools/sync_proto.ps1).

   Wi-Fi: the first time (or after "wifi reset", or a 3 s press on BOOT), Elyssa opens the
   Wi-Fi network "Elyssa-Setup": connect to it, a page lists the networks, choose yours.
   Then open http://elyssa.local (or the address printed below) on a computer of that network.

   USB console (115200, line ending NL or NL+CR):
     mode idle|local|remote|test     sp <rpm>     stat     wifi reset     wifi set <ssid> <password>

   Safety: the STM32 decides (clamps, ramps, refuses). In REMOTE the setpoint must be refreshed:
   the browser resends it every 200 ms; Elyssa forwards it only while the browser is fresh, so a
   closed page or a lost Wi-Fi brings the motor back to 0 within 1 s (STM32 dead-man).

   UART to the STM32 (3.3 V, common GND, never join the 3V3 rails):
     Elyssa RX (GPIO44) <- PB6      Elyssa TX (GPIO43) -> PB7 */

#include "link.h"
#include "net.h"
#include "web.h"

static const uint32_t HEARTBEAT_MS   = 100;
static const uint32_t SP_REFRESH_MS  = 200;
static const uint32_t WEB_FRESH_MS   = 1000;  /* browser setpoint older than this: stop forwarding */
static const uint32_t TEL_BATCH_MS   = 100;
static const uint32_t SYS_MS         = 1000;
static const uint32_t PRINT_MS       = 1000;

/* REMOTE setpoint holder: who asked for it and when. */
enum SpSource : uint8_t { SRC_NONE, SRC_CONSOLE, SRC_WEB };
static float    remoteRpm;
static SpSource remoteSrc = SRC_NONE;
static uint32_t remoteMs;

/* Telemetry samples collected between two WebSocket batches (100 Hz in, 10 Hz out). */
static Telemetry batch[16];
static uint8_t   batchN;

static uint32_t lastHbMs, lastSpMs, lastBatchMs, lastSysMs, lastPrintMs;
static char     cmdBuf[40];
static uint8_t  cmdLen;
static int16_t  lastAckValue[2] = { -1, -1 };  /* [0] mode, [1] setpoint: print only on change */

/* ---- Link callbacks ------------------------------------------------------------- */

void on_telemetry(const Telemetry &t)
{
  if (batchN < sizeof batch / sizeof batch[0]) batch[batchN++] = t;
}

void on_event(uint32_t t, uint8_t code, int32_t arg)
{
  Serial.printf("[EVENT] t=%lu ms %s arg=%ld\n", (unsigned long)t, link_event_name(code), (long)arg);
  char j[128];
  snprintf(j, sizeof j, "{\"type\":\"evt\",\"t\":%lu,\"code\":%u,\"name\":\"%s\",\"arg\":%ld}",
           (unsigned long)t, code, link_event_name(code), (long)arg);
  web_broadcast(j);
}

void on_ack(uint8_t type, uint8_t status, int16_t value)
{
  static const char *st[] = { "ok", "clamped", "rejected" };
  const char *name = (type == HIL_T_CMD_MODE) ? "mode" : (type == HIL_T_CMD_FAULT) ? "fault" : "sp";
  if (type != HIL_T_CMD_FAULT) {
    const int k = (type == HIL_T_CMD_MODE) ? 0 : 1;
    if (status == HIL_ACK_OK && value == lastAckValue[k]) return;   /* periodic refresh: stay quiet */
    lastAckValue[k] = value;
  }
  const char *s = status < 3 ? st[status] : "?";
  Serial.printf("[ACK] %s %s value=%d\n", name, s, (int)value);
  char j[96];
  snprintf(j, sizeof j, "{\"type\":\"ack\",\"cmd\":\"%s\",\"status\":\"%s\",\"value\":%d}", name, s, (int)value);
  web_broadcast(j);
}

void on_ack_timeout()
{
  Serial.println("[CMD] no ACK from the STM32 within 500 ms (command lost, or not received)");
  web_broadcast("{\"type\":\"info\",\"text\":\"no ACK from the STM32 within 500 ms\"}");
}

void on_link_state(bool alive)
{
  Serial.println(alive ? "[LINK] STM32 present" : "[LINK] STM32 LOST (no valid frame)");
  web_broadcast(alive ? "{\"type\":\"info\",\"text\":\"STM32 present\"}" : "{\"type\":\"info\",\"text\":\"STM32 LOST\"}");
}

void on_stm32_restart()
{
  Serial.println("[LINK] STM32 restarted");
  web_broadcast("{\"type\":\"info\",\"text\":\"STM32 restarted\"}");
}

/* ---- RTOS probe: configuration and statistics as JSON, trace as binary ------------------ */

/* Copies the ASCII name at the end of a payload, made safe for a JSON string. */
static void copyName(const uint8_t *p, uint8_t from, uint8_t len, char *out, size_t size)
{
  size_t n = 0;
  for (uint8_t i = from; i < len && n < size - 1; i++) {
    const char c = (char)p[i];
    out[n++] = (c >= 32 && c < 127 && c != '"' && c != '\\') ? c : '_';
  }
  out[n] = 0;
}

void on_task_table(const uint8_t *p, uint8_t len)
{
  if (len < 12) return;
  char name[32];
  copyName(p, 12, len, name, sizeof name);
  char j[200];
  snprintf(j, sizeof j,
           "{\"type\":\"task\",\"tag\":%u,\"prio\":%u,\"stack\":%u,\"deadline\":%u,\"flags\":%u,\"fcpu\":%lu,\"name\":\"%s\"}",
           p[0], p[1], hil_get_u16(p + 2), hil_get_u16(p + 4), p[6], (unsigned long)hil_get_u32(p + 8), name);
  web_broadcast(j);
}

void on_task_stats(const uint8_t *p, uint8_t len)
{
  if (len < 40) return;
  char j[320];
  snprintf(j, sizeof j,
           "{\"type\":\"tstat\",\"tag\":%u,\"state\":%u,\"prio\":%u,\"cpu\":%u,\"jobs\":%u,"
           "\"cmax\":%lu,\"cavg\":%lu,\"rmax\":%lu,\"rmin\":%lu,\"tavg\":%lu,\"tmin\":%lu,\"tmax\":%lu,"
           "\"miss\":%u,\"stack\":%u,\"cmaxl\":%lu,\"rmaxl\":%lu}",
           p[0], p[1], p[2], hil_get_u16(p + 4), hil_get_u16(p + 6),
           (unsigned long)hil_get_u32(p + 8), (unsigned long)hil_get_u32(p + 12),
           (unsigned long)hil_get_u32(p + 16), (unsigned long)hil_get_u32(p + 20),
           (unsigned long)hil_get_u32(p + 24), (unsigned long)hil_get_u32(p + 28),
           (unsigned long)hil_get_u32(p + 32), hil_get_u16(p + 36), hil_get_u16(p + 38),
           (unsigned long)(len >= 48 ? hil_get_u32(p + 40) : hil_get_u32(p + 8)),
           (unsigned long)(len >= 48 ? hil_get_u32(p + 44) : hil_get_u32(p + 16)));
  web_broadcast(j);
}

void on_rtos_sys(const uint8_t *p, uint8_t len)
{
  if (len < 44) return;
  char j[320];
  snprintf(j, sizeof j,
           "{\"type\":\"rsys\",\"fcpu\":%lu,\"sw\":%lu,\"ev\":%lu,\"drop\":%lu,\"idle\":%u,\"tick\":%u,"
           "\"ctx\":%lu,\"isr\":%lu,\"probe\":%u,\"kernel\":%u,\"heapTotal\":%lu,\"heapFree\":%lu,\"heapMin\":%lu}",
           (unsigned long)hil_get_u32(p), (unsigned long)hil_get_u32(p + 4), (unsigned long)hil_get_u32(p + 8),
           (unsigned long)hil_get_u32(p + 12), hil_get_u16(p + 16), hil_get_u16(p + 18),
           (unsigned long)hil_get_u32(p + 20), (unsigned long)hil_get_u32(p + 24),
           hil_get_u16(p + 28), hil_get_u16(p + 30), (unsigned long)hil_get_u32(p + 32),
           (unsigned long)hil_get_u32(p + 36), (unsigned long)hil_get_u32(p + 40));
  web_broadcast(j);
}

void on_queue_stats(const uint8_t *p, uint8_t len)
{
  if (len < 12) return;
  char name[24];
  copyName(p, 12, len, name, sizeof name);
  char j[200];
  snprintf(j, sizeof j,
           "{\"type\":\"qstat\",\"qid\":%u,\"len\":%u,\"wait\":%u,\"max\":%u,\"fail\":%lu,\"name\":\"%s\"}",
           p[0], hil_get_u16(p + 2), hil_get_u16(p + 4), hil_get_u16(p + 6), (unsigned long)hil_get_u32(p + 8), name);
  web_broadcast(j);
}

/* Trace events, decoded to absolute timestamps and batched for the browsers:
   one binary WebSocket message every 100 ms = 'T', u16 dropped (STM32), u16 dropped (here),
   then N x { u32 cycles, u8 id }, little-endian. */
static uint8_t  traceBuf[5 + 5 * 2400];
static size_t   traceLen;
static uint16_t traceDroppedStm, traceDroppedHere;

void on_trace(const uint8_t *p, uint8_t len)
{
  if (len < 6) return;
  uint32_t cyc = hil_get_u32(p);
  traceDroppedStm = hil_get_u16(p + 4);
  for (uint8_t i = 6; i + 4 <= len; i += 4) {
    cyc += (uint32_t)p[i + 1] | ((uint32_t)p[i + 2] << 8) | ((uint32_t)p[i + 3] << 16);
    if (traceLen == 0) traceLen = 5;                 /* room for the header */
    if (traceLen + 5 > sizeof traceBuf) { traceDroppedHere++; continue; }
    hil_put_u32(&traceBuf[traceLen], cyc);
    traceBuf[traceLen + 4] = p[i];
    traceLen += 5;
  }
}

static void flushTrace()
{
  if (traceLen <= 5) return;
  traceBuf[0] = 'T';
  hil_put_u16(&traceBuf[1], traceDroppedStm);
  hil_put_u16(&traceBuf[3], traceDroppedHere);
  web_broadcast_bin(traceBuf, traceLen);
  traceLen = 0;
}

/* ---- Commands (console and browser go through the same path) ---------------------- */

static void commandMode(uint8_t mode)
{
  remoteSrc = SRC_NONE;                        /* a new mode never reuses an old setpoint */
  remoteRpm = 0;
  lastAckValue[0] = -1;
  Serial.printf("[CMD] mode %s\n", link_mode_name(mode));
  link_send_mode(mode);
}

static void commandSetpoint(float rpm, SpSource src)
{
  const bool changed = (src != remoteSrc) || (rpm != remoteRpm);
  remoteRpm = rpm;
  remoteSrc = src;
  remoteMs  = millis();
  if (changed) {
    Serial.printf("[CMD] setpoint %.1f rpm (%s)\n", rpm, src == SRC_WEB ? "browser" : "console");
    link_send_setpoint(rpm, false);
  }
}

static void printStats()
{
  Serial.printf("[STAT] frames ok=%lu crc errors=%lu lost(seq)=%lu link lost events=%lu | wifi %s %s %d dBm, browsers=%d\n",
                (unsigned long)link_frames_ok(), (unsigned long)link_crc_errors(), (unsigned long)link_seq_lost(),
                (unsigned long)link_lost_events(), net_portal() ? "portal" : "station", net_ip().c_str(),
                net_rssi(), web_clients());
}

/* Case-insensitive; "mode test" and "test" both work; "sp 800" and "sp=800" too.
   "wifi set <ssid> <password>" is handled first, before lowercasing (SSID and password
   are case-sensitive). An SSID with spaces goes between double quotes:
   wifi set "My Network" secret. The password (no spaces) is never printed. */
static void handleConsoleLine(char *line)
{
  if (!strncasecmp(line, "wifi set ", 9)) {
    char ssid[33] = {0}, pass[65] = {0};
    const char *p = line + 9;
    while (*p == ' ') p++;
    bool ok = false;
    if (*p == '"') {
      const char *end = strchr(p + 1, '"');
      const size_t n = end ? (size_t)(end - p - 1) : 0;
      if (end != nullptr && n > 0 && n < sizeof ssid) {
        memcpy(ssid, p + 1, n);
        sscanf(end + 1, " %64s", pass);
        ok = true;
      }
    } else {
      ok = sscanf(p, "%32s %64s", ssid, pass) >= 1;
    }
    if (ok) net_save_and_restart(ssid, pass);
    else Serial.println("usage: wifi set <ssid> <password>   (or wifi set \"ssid with spaces\" <password>)");
    return;
  }

  for (char *c = line; *c != '\0'; c++) {
    if (*c >= 'A' && *c <= 'Z') *c = (char)(*c + 32);
    if (*c == '=') *c = ' ';
  }
  const char *p = line;
  while (*p == ' ') p++;
  if (*p == '\0') return;

  if (!strncmp(p, "wifi reset", 10)) { net_forget_and_restart(); return; }
  if (!strncmp(p, "fault", 5)) {                         /* fault hog|stack|queue|inversion|heap */
    p += 5;
    while (*p == ' ') p++;
    const uint8_t f = !strncmp(p, "hog", 3) ? HIL_FAULT_CPU_HOG : !strncmp(p, "stack", 5) ? HIL_FAULT_STACK
                    : !strncmp(p, "queue", 5) ? HIL_FAULT_QUEUE : !strncmp(p, "inversion", 9) ? HIL_FAULT_INVERSION
                    : !strncmp(p, "heap", 4) ? HIL_FAULT_HEAP : 0;
    if (f) link_send_fault(f); else Serial.println("faults: hog stack queue inversion heap");
    return;
  }
  if (!strncmp(p, "stat", 4))        { printStats(); return; }
  if (!strncmp(p, "mode", 4)) p += 4;
  while (*p == ' ') p++;

  float v = 0;
  if      (!strncmp(p, "idle", 4))   commandMode(HIL_MODE_IDLE);
  else if (!strncmp(p, "local", 5))  commandMode(HIL_MODE_LOCAL);
  else if (!strncmp(p, "remote", 6)) commandMode(HIL_MODE_REMOTE);
  else if (!strncmp(p, "test", 4))   commandMode(HIL_MODE_TEST);
  else if (!strncmp(p, "sp", 2) && sscanf(p + 2, "%f", &v) == 1) commandSetpoint(v, SRC_CONSOLE);
  else Serial.println("commands: mode idle|local|remote|test, sp <rpm>, fault <name>, stat, wifi reset, wifi set <ssid> <password>");
}

static void pollConsole()
{
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      cmdBuf[cmdLen] = '\0';
      handleConsoleLine(cmdBuf);
      cmdLen = 0;
    } else if (cmdLen < sizeof cmdBuf - 1) {
      cmdBuf[cmdLen++] = c;
    }
  }
}

static void pollWeb()
{
  WebCmd c;
  while (web_next_command(&c)) {
    if (c.kind == WEB_CMD_FAULT) {
      Serial.printf("[CMD] inject fault %u\n", c.fault);
      link_send_fault(c.fault);
      continue;
    }
    if (c.kind == WEB_CMD_MODE) commandMode(c.mode);
    else                        commandSetpoint(c.rpm, SRC_WEB);
  }
}

/* REMOTE: forward the held setpoint while its source is fresh. The console holds it until
   changed (debug tool); the browser must keep resending, otherwise forwarding stops and the
   STM32 dead-man brings the motor back to 0. */
static void refreshSetpoint()
{
  if (!link_have_telemetry() || link_last().mode != HIL_MODE_REMOTE || remoteSrc == SRC_NONE) return;
  if (remoteSrc == SRC_WEB && millis() - remoteMs > WEB_FRESH_MS) return;
  link_send_setpoint(remoteRpm, true);
}

/* ---- Periodic JSON for the browsers ------------------------------------------------- */

static void sendBatch()
{
  if (batchN == 0) return;
  char j[700];
  const Telemetry &l = batch[batchN - 1];
  int n = snprintf(j, sizeof j, "{\"type\":\"tel\",\"mode\":\"%s\",\"fault\":%u,\"s\":[",
                   link_mode_name(l.mode), l.fault);
  for (uint8_t i = 0; i < batchN && n < (int)sizeof j - 48; i++) {
    const Telemetry &t = batch[i];
    n += snprintf(j + n, sizeof j - n, "%s[%lu,%.1f,%.1f,%.1f]", i ? "," : "",
                  (unsigned long)t.t, t.sp10 / 10.0f, t.rpm10 / 10.0f, t.dutyPm / 10.0f);
  }
  snprintf(j + n, sizeof j - n, "]}");
  batchN = 0;
  web_broadcast(j);
}

static void sendSys()
{
  char j[256];
  snprintf(j, sizeof j,
           "{\"type\":\"sys\",\"link\":%d,\"rx\":%lu,\"crc\":%lu,\"lost\":%lu,\"rssi\":%d,\"ip\":\"%s\",\"clients\":%d,\"up\":%lu,\"heap\":%lu}",
           link_alive() ? 1 : 0, (unsigned long)link_frames_ok(), (unsigned long)link_crc_errors(),
           (unsigned long)link_seq_lost(), net_rssi(), net_ip().c_str(), web_clients(),
           (unsigned long)millis(), (unsigned long)ESP.getFreeHeap());
  web_broadcast(j);
}

static void updateLed()
{
  if (net_portal())                                   setLedColor(ELYSSA_BLUE);    /* waiting for Wi-Fi setup */
  else if (!link_alive())                             setLedColor(ELYSSA_YELLOW);  /* no STM32 */
  else if (link_have_telemetry() && link_last().fault) setLedColor(ELYSSA_RED);    /* motor fault */
  else                                                setLedColor(ELYSSA_GREEN);
}

/* Called by net.cpp during its blocking waits (scan, join): without it the Serial1
   buffer overflows and frames from the STM32 are lost while Wi-Fi starts. */
void net_idle()
{
  link_poll();
  const uint32_t now = millis();
  if (now - lastHbMs >= HEARTBEAT_MS) { lastHbMs = now; link_send_heartbeat(); }
}

void setup()
{
  Serial.begin(115200);
  delay(500);
  link_begin();
  const bool station = net_begin();
  web_begin(!station);
  Serial.println("supervisor ready. commands: mode idle|local|remote|test, sp <rpm>, fault <name>, stat, wifi reset, wifi set <ssid> <password>");
}

void loop()
{
  const uint32_t now = millis();

  link_poll();
  pollConsole();
  pollWeb();
  net_loop();

  if (now - lastHbMs >= HEARTBEAT_MS)  { lastHbMs = now;    link_send_heartbeat(); }
  if (now - lastSpMs >= SP_REFRESH_MS) { lastSpMs = now;    refreshSetpoint(); }
  if (now - lastBatchMs >= TEL_BATCH_MS) { lastBatchMs = now; sendBatch(); flushTrace(); }
  if (now - lastSysMs >= SYS_MS)       { lastSysMs = now;   sendSys(); updateLed(); }

  if (now - lastPrintMs >= PRINT_MS) {
    lastPrintMs = now;
    if (link_have_telemetry() && link_alive()) {
      const Telemetry &t = link_last();
      Serial.printf("t=%lu mode=%s sp=%.1f rpm=%.1f duty=%.1f%% fault=%u | rx ok=%lu crc=%lu lost=%lu | %s %s\n",
                    (unsigned long)t.t, link_mode_name(t.mode), t.sp10 / 10.0f, t.rpm10 / 10.0f, t.dutyPm / 10.0f,
                    t.fault, (unsigned long)link_frames_ok(), (unsigned long)link_crc_errors(),
                    (unsigned long)link_seq_lost(), net_portal() ? "portal" : "wifi", net_ip().c_str());
    } else if (!link_alive()) {
      Serial.printf("waiting for the STM32 ... (%s %s)\n", net_portal() ? "portal" : "wifi", net_ip().c_str());
    }
  }

  delay(1);                                  /* yield: the Wi-Fi and server tasks need CPU too */
}
