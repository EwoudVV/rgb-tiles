#include "tile.h"
#include <string.h>
uint64_t read64(const uint8_t *p) {
  uint64_t n = 0;
  for (int i = 7; i >= 0; --i)
    n = (n << 8) | p[i];
  return n;
}
uint32_t read32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}
void write64(uint8_t *p, uint64_t n) {
  for (unsigned i = 0; i < 8; i++, n >>= 8)
    p[i] = (uint8_t)n;
}
void write32(uint8_t *p, uint32_t n) {
  for (unsigned i = 0; i < 4; i++, n >>= 8)
    p[i] = (uint8_t)n;
}
uint16_t crc16(const uint8_t *p, size_t n) {
  uint16_t c = 0xffff;
  while (n--) {
    c ^= (uint16_t)*p++ << 8;
    for (unsigned i = 0; i < 8; i++)
      c = (uint16_t)((c << 1) ^ ((c & 0x8000) ? 0x1021 : 0));
  }
  return c;
}
size_t packet_encode(const Packet *p, uint8_t *out) {
  if (p->size > PACKET_LIMIT)
    return 0;
  uint8_t raw[PACKET_LIMIT + 6];
  raw[0] = p->type;
  raw[1] = p->ttl;
  raw[2] = (uint8_t)p->size;
  raw[3] = (uint8_t)(p->size >> 8);
  memcpy(raw + 4, p->data, p->size);
  uint16_t c = crc16(raw, p->size + 4);
  raw[p->size + 4] = (uint8_t)c;
  raw[p->size + 5] = (uint8_t)(c >> 8);
  size_t at = 1, code_at = 0;
  uint8_t code = 1;
  for (size_t i = 0; i < p->size + 6u; i++) {
    if (!raw[i]) {
      out[code_at] = code;
      code_at = at++;
      code = 1;
    } else {
      out[at++] = raw[i];
      if (++code == 255) {
        out[code_at] = code;
        code_at = at++;
        code = 1;
      }
    }
  }
  out[code_at] = code;
  out[at++] = 0;
  return at;
}
bool packet_decode(const uint8_t *in, size_t n, Packet *p) {
  uint8_t raw[PACKET_LIMIT + 6];
  size_t at = 0, i = 0;
  if (!n || n > WIRE_LIMIT)
    return false;
  while (i < n) {
    uint8_t code = in[i++];
    if (!code || i + code - 1 > n)
      return false;
    for (unsigned j = 1; j < code; j++) {
      if (at == sizeof raw || !in[i])
        return false;
      raw[at++] = in[i++];
    }
    if (code < 255 && i < n) {
      if (at == sizeof raw)
        return false;
      raw[at++] = 0;
    }
  }
  if (at < 6)
    return false;
  size_t len = (size_t)raw[2] | (size_t)raw[3] << 8;
  if (len > PACKET_LIMIT || at != len + 6 || !raw[1] || raw[0] < HELLO ||
      raw[0] > ACK)
    return false;
  if (crc16(raw, len + 4) !=
      ((uint16_t)raw[len + 4] | (uint16_t)raw[len + 5] << 8))
    return false;
  p->type = raw[0];
  p->ttl = raw[1];
  p->size = (uint16_t)len;
  memcpy(p->data, raw + 4, len);
  return true;
}
unsigned led_index(unsigned x, unsigned y) {
  return y * 8 + ((y & 1) ? 7 - x : x);
}
