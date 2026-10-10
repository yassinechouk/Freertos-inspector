/* Wi-Fi: joins the network saved in flash, or opens the "Elyssa-Setup" portal. */
#pragma once
#include <Arduino.h>

bool   net_begin();                 /* true: connected as a station; false: setup portal running */
void   net_loop();                  /* captive DNS, BOOT long press, deferred restart */
bool   net_portal();
String net_ip();
int    net_rssi();
String net_portal_options();        /* <option> list of the networks seen at startup (HTML-escaped) */
void   net_save_and_restart(const String &ssid, const String &pass);
void   net_forget_and_restart();
