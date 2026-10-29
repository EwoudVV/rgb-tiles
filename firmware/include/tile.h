#ifndef RGB_TILE_H
#define RGB_TILE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define TILE_LIMIT 128
#define PIXEL_BYTES 192
#define PACKET_LIMIT 256
#define WIRE_LIMIT 266
#define NO_PORT 255
#define LINK_BAUD 1000000
#define LINK_DISABLE_PIN 17
#define LED_PIN 16
#define VBUS_N_PIN 24

enum { NORTH, EAST, SOUTH, WEST };
enum { HELLO = 1, ADVERTISE, PIXELS, SHOW, ACK };
typedef struct {
  uint8_t type, ttl;
  uint16_t size;
  uint8_t data[PACKET_LIMIT];
} Packet;
typedef struct {
  uint64_t id, neighbor[4];
  uint32_t boot, sequence, seen;
  bool usb, used;
} Node;
typedef struct {
  uint64_t self, root;
  uint32_t boot, sequence, overflow_at;
  Node node[TILE_LIMIT];
  uint64_t neighbor[4];
  uint32_t neighbor_boot[4], neighbor_seen[4];
  int16_t x[TILE_LIMIT], y[TILE_LIMIT];
  uint8_t route[TILE_LIMIT];
  bool reachable[TILE_LIMIT], conflict, full;
  unsigned count;
} Mesh;
uint16_t crc16(const uint8_t *data, size_t size);
size_t packet_encode(const Packet *p, uint8_t *wire);
bool packet_decode(const uint8_t *wire, size_t size, Packet *p);
uint64_t read64(const uint8_t *p);
uint32_t read32(const uint8_t *p);
void write64(uint8_t *p, uint64_t n);
void write32(uint8_t *p, uint32_t n);
void mesh_init(Mesh *m, uint64_t id, uint32_t boot);
bool mesh_hello(Mesh *m, unsigned port, uint64_t id, uint32_t boot,
                uint32_t now);
void mesh_expire(Mesh *m, uint32_t now);
void mesh_advertise(Mesh *m, bool usb, uint32_t now, Packet *p);
bool mesh_receive(Mesh *m, const Packet *p, uint32_t now);
void mesh_rebuild(Mesh *m, uint32_t now);
int mesh_find(const Mesh *m, uint64_t id);
int mesh_at(const Mesh *m, int x, int y);
unsigned led_index(unsigned x, unsigned y);
#endif
