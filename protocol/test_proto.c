/* Host test for hil_proto.h. Build and run:  gcc -Wall -Wextra -Werror -o test_proto test_proto.c && ./test_proto
   (or through tools/test_proto.ps1, which uses the ESP-IDF Docker image). */
#include <stdio.h>
#include <string.h>
#include "hil_proto.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

/* Feeds a buffer, returns the number of valid frames; last one is kept in *last. */
static int feed_all(hil_parser_t *p, const uint8_t *buf, size_t n, hil_frame_t *last, uint8_t *payloadCopy)
{
  int frames = 0;
  hil_frame_t f;
  for (size_t i = 0; i < n; i++)
  {
    hil_parser_push(p, buf[i]);
    while (hil_parser_next(p, &f))
    {
      frames++;
      *last = f;
      if (payloadCopy != NULL) { memcpy(payloadCopy, f.payload, f.len); last->payload = payloadCopy; }
    }
  }
  return frames;
}

int main(void)
{
  uint8_t buf[512], pay[HIL_MAX_PAYLOAD];
  hil_parser_t p;
  hil_frame_t f;

  /* 1. CRC-16/CCITT-FALSE reference vector. */
  uint16_t crc = 0xFFFFu;
  for (const char *s = "123456789"; *s != '\0'; s++) { crc = hil_crc16_update(crc, (uint8_t)*s); }
  CHECK(crc == 0x29B1u);

  /* 2. Round trip of a telemetry frame. */
  uint8_t tel[12];
  hil_put_u32(&tel[0], 123456u); hil_put_u16(&tel[4], (uint16_t)15000); hil_put_u16(&tel[6], (uint16_t)14993);
  hil_put_u16(&tel[8], 500u); tel[10] = 0u; tel[11] = HIL_MODE_TEST;
  size_t n = hil_encode(buf, HIL_T_TELEMETRY, 7u, tel, sizeof tel);
  CHECK(n == HIL_OVERHEAD + 12u);
  hil_parser_init(&p);
  CHECK(feed_all(&p, buf, n, &f, pay) == 1);
  CHECK(f.type == HIL_T_TELEMETRY && f.seq == 7u && f.len == 12u);
  CHECK(hil_get_u32(f.payload) == 123456u && hil_get_i16(f.payload + 4) == 15000 && hil_get_i16(f.payload + 6) == 14993);

  /* 3. Empty payload and maximum payload. */
  hil_parser_init(&p);
  n = hil_encode(buf, HIL_T_HEARTBEAT, 0u, NULL, 0u);
  CHECK(feed_all(&p, buf, n, &f, pay) == 1 && f.len == 0u);
  uint8_t big[HIL_MAX_PAYLOAD];
  for (unsigned i = 0; i < sizeof big; i++) { big[i] = (uint8_t)i; }
  n = hil_encode(buf, HIL_T_TRACE, 1u, big, (uint8_t)sizeof big);
  CHECK(feed_all(&p, buf, n, &f, pay) == 1 && f.len == HIL_MAX_PAYLOAD && memcmp(f.payload, big, sizeof big) == 0);

  /* 4. A flipped bit anywhere in the frame is rejected (every byte position). */
  n = hil_encode(buf, HIL_T_TELEMETRY, 9u, tel, sizeof tel);
  for (size_t i = 0; i < n; i++)
  {
    uint8_t bad[64];
    memcpy(bad, buf, n);
    bad[i] ^= 0x10u;
    hil_parser_init(&p);
    CHECK(feed_all(&p, bad, n, &f, pay) == 0);
  }

  /* 5. Resynchronisation: garbage, a corrupted frame, then two good frames. */
  uint8_t stream[512];
  size_t m = 0;
  const uint8_t junk[] = { 0x00, 0xFF, 0xA5, 0xA5, 0x5A, 0x33, 0xA5 };   /* contains false starts */
  memcpy(&stream[m], junk, sizeof junk); m += sizeof junk;
  size_t k = hil_encode(&stream[m], HIL_T_TELEMETRY, 1u, tel, sizeof tel);
  stream[m + 8] ^= 0x01u; m += k;                                          /* corrupted */
  m += hil_encode(&stream[m], HIL_T_TELEMETRY, 2u, tel, sizeof tel);
  m += hil_encode(&stream[m], HIL_T_EVENT, 3u, tel, 9u);
  hil_parser_init(&p);
  CHECK(feed_all(&p, stream, m, &f, pay) == 2);
  CHECK(f.type == HIL_T_EVENT && f.seq == 3u);
  CHECK(p.crcErrors >= 1u && p.framesOk == 2u);

  /* 5b. A false header with a long LEN swallowing real frames: nothing is lost. */
  hil_parser_init(&p);
  m = 0;
  stream[m++] = HIL_SOF0; stream[m++] = HIL_SOF1; stream[m++] = 100u;   /* claims 100 bytes of payload */
  m += hil_encode(&stream[m], HIL_T_TELEMETRY, 5u, tel, sizeof tel);
  m += hil_encode(&stream[m], HIL_T_TELEMETRY, 6u, tel, sizeof tel);
  m += hil_encode(&stream[m], HIL_T_TELEMETRY, 7u, tel, sizeof tel);
  for (int i = 0; i < 120; i++) { stream[m++] = 0u; }                   /* enough bytes to end the false frame */
  CHECK(feed_all(&p, stream, m, &f, pay) == 3);

  /* 6. Invalid LEN is counted and skipped. */
  hil_parser_init(&p);
  uint8_t badlen[] = { HIL_SOF0, HIL_SOF1, 129u, 0u, 0u };
  CHECK(feed_all(&p, badlen, sizeof badlen, &f, pay) == 0 && p.badLen == 1u);

  /* 7. Sequence gaps, including the 255 -> 0 wrap. */
  hil_seq_t s;
  hil_seq_init(&s);
  hil_seq_update(&s, 254u); hil_seq_update(&s, 255u); hil_seq_update(&s, 0u);
  CHECK(s.lost == 0u);
  hil_seq_update(&s, 3u);                                                  /* 1 and 2 missing */
  CHECK(s.lost == 2u);

  printf(failures == 0 ? "hil_proto: all tests passed\n" : "hil_proto: %d failure(s)\n", failures);
  return failures != 0;
}
