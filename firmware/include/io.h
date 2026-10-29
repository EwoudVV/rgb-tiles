#ifndef TILE_IO_H
#define TILE_IO_H
#include "tile.h"
void io_init(void);
void io_service(void);
bool io_send(unsigned port, const Packet *packet);
bool io_receive(unsigned port, Packet *packet);
void pixels_show(const uint8_t *rgb, uint8_t brightness);
bool pixels_ready(void);
extern uint32_t io_dropped, io_bad_packets;
#endif
