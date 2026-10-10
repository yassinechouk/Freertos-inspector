#include "net.h"
#include <WiFi.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <ESPmDNS.h>

static const char    *AP_NAME     = "Elyssa-Setup";
static const char    *HOSTNAME    = "elyssa";          /* http://elyssa.local */
static const uint32_t JOIN_MS     = 15000;
static const uint32_t FORGET_MS   = 3000;              /* BOOT held this long: forget the network */
static const int      BOOT_BUTTON_PIN = 0;          /* BOOT_PIN is already a core macro */

/* Implemented by the sketch: keeps the STM32 link serviced while Wi-Fi blocks. */
void net_idle();

static DNSServer dns;
static bool      portal;
static String    options;
static uint32_t  restartAt;                            /* 0 = no restart scheduled */
static uint32_t  bootPressedAt;

static String htmlEscape(const String &s)
{
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    const char c = s[i];
    if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;"; else if (c == '&') o += "&amp;";
    else if (c == '"') o += "&quot;"; else if (c == '\'') o += "&#39;"; else o += c;
  }
  return o;
}

static void buildOptions()
{
  options = "";
  WiFi.scanNetworks(true);                             /* asynchronous: the link keeps running */
  int n;
  while ((n = WiFi.scanComplete()) == WIFI_SCAN_RUNNING) { net_idle(); delay(5); }
  for (int i = 0; i < n; i++) {
    const String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) continue;
    const String e = htmlEscape(ssid);
    options += "<option value=\"" + e + "\">" + e + " (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  WiFi.scanDelete();
}

bool net_begin()
{
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  Preferences prefs;
  prefs.begin("wifi", true);
  const String ssid = prefs.getString("ssid", "");
  const String pass = prefs.getString("pass", "");
  prefs.end();

  if (ssid.length() > 0) {
    Serial.printf("[WIFI] joining \"%s\" ...\n", ssid.c_str());
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < JOIN_MS) {
      net_idle();
      delay(5);
    }
    if (WiFi.status() == WL_CONNECTED) {
      MDNS.begin(HOSTNAME);
      MDNS.addService("http", "tcp", 80);
      Serial.printf("[WIFI] connected: http://%s.local  or  http://%s  (%d dBm)\n",
                    HOSTNAME, WiFi.localIP().toString().c_str(), WiFi.RSSI());
      portal = false;
      return true;
    }
    Serial.println("[WIFI] could not join the saved network: opening the setup portal");
  } else {
    Serial.println("[WIFI] no network saved: opening the setup portal");
  }

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP_STA);
  buildOptions();
  WiFi.softAP(AP_NAME);
  dns.start(53, "*", WiFi.softAPIP());                 /* every name -> this page (captive portal) */
  Serial.printf("[WIFI] connect to the Wi-Fi \"%s\", then open http://%s\n",
                AP_NAME, WiFi.softAPIP().toString().c_str());
  portal = true;
  return false;
}

void net_loop()
{
  if (portal) dns.processNextRequest();

  /* Long press on BOOT while running: forget the saved network. (Holding BOOT at reset
     would start the ROM bootloader instead, so it is read only after startup.) */
  if (digitalRead(BOOT_BUTTON_PIN) == LOW) {
    if (bootPressedAt == 0) bootPressedAt = millis();
    else if (millis() - bootPressedAt > FORGET_MS) net_forget_and_restart();
  } else {
    bootPressedAt = 0;
  }

  if (restartAt != 0 && (int32_t)(millis() - restartAt) >= 0) ESP.restart();
}

bool   net_portal()         { return portal; }
String net_ip()             { return portal ? WiFi.softAPIP().toString() : WiFi.localIP().toString(); }
int    net_rssi()           { return portal ? 0 : WiFi.RSSI(); }
String net_portal_options() { return options; }

void net_save_and_restart(const String &ssid, const String &pass)
{
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  Serial.printf("[WIFI] saved \"%s\", restarting\n", ssid.c_str());
  restartAt = millis() + 1500;                          /* let the HTTP answer leave first */
}

void net_forget_and_restart()
{
  Preferences prefs;
  prefs.begin("wifi", false);
  prefs.clear();
  prefs.end();
  Serial.println("[WIFI] network forgotten, restarting into the setup portal");
  restartAt = millis() + 500;
}
