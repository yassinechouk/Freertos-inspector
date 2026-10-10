/* Supervisor node (Elyssa, ESP32-S3) - bring-up.

   Checks the toolchain and the link with the STM32:
   - green LED blinks every 500 ms: the sketch is running;
   - USB serial (115200) prints a status line every second;
   - everything received on the UART link from the STM32 is echoed to USB.

   UART link to the STM32 (3.3 V logic, common GND, never join the 3V3 rails):
     Elyssa RX (GPIO44) <- STM32 TX
     Elyssa TX (GPIO43) -> STM32 RX
   The USB port is "Serial" (USB CDC On Boot enabled); the STM32 link uses
   Serial1, remapped to GPIO44/43 through the GPIO matrix. */

static const uint32_t LINK_BAUD = 115200;
static const int      LINK_RX   = 44;   /* header RX */
static const int      LINK_TX   = 43;   /* header TX */

static uint32_t lastBlink  = 0;
static uint32_t lastStatus = 0;
static uint32_t linkBytes  = 0;         /* bytes received from the STM32 */
static bool     ledOn      = false;

void setup()
{
  Serial.begin(115200);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_RX, LINK_TX);
  setLedColor(ELYSSA_OFF);
}

void loop()
{
  const uint32_t now = millis();

  if (now - lastBlink >= 500U) {
    lastBlink = now;
    ledOn = !ledOn;
    setLedColor(ledOn ? ELYSSA_GREEN : ELYSSA_OFF);
  }

  /* Echo the STM32 link to USB, byte by byte (no line framing yet). */
  while (Serial1.available() > 0) {
    Serial.write((uint8_t)Serial1.read());
    linkBytes++;
  }

  if (now - lastStatus >= 1000U) {
    lastStatus = now;
    Serial.printf("\n[supervisor] up %lu s, link bytes received: %lu\n",
                  (unsigned long)(now / 1000U), (unsigned long)linkBytes);
  }
}
