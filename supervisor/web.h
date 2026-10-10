/* HTTP server (ESP-IDF esp_http_server, no external library) and WebSocket at /ws.
   Portal mode: Wi-Fi setup page. Station mode: dashboard page.
   The server runs in its own task: browser commands are queued and executed by loop(). */
#pragma once
#include <Arduino.h>

enum WebCmdKind : uint8_t { WEB_CMD_MODE, WEB_CMD_SETPOINT, WEB_CMD_FAULT };

struct WebCmd
{
  WebCmdKind kind;
  uint8_t    mode;          /* WEB_CMD_MODE */
  float      rpm;           /* WEB_CMD_SETPOINT */
  uint8_t    fault;         /* WEB_CMD_FAULT: HIL_FAULT_* */
};

void web_begin(bool portal);
bool web_next_command(WebCmd *c);          /* from loop(): next command received from a browser */
void web_broadcast(const char *json);      /* to every WebSocket client; safe from loop() */
void web_broadcast_bin(const uint8_t *data, size_t len);   /* binary WebSocket message */
int  web_clients();
