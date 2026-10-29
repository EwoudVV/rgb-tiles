#include "io.h"
#include "pico/rand.h"
#include "pico/stdio_usb.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"
#include "tile.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static Mesh mesh;
static uint8_t staged[PIXEL_BYTES], display[PIXEL_BYTES];
static uint32_t staged_frame, last_show, root_boot, await_frame, await_since;
static uint64_t await_tile;
static bool staged_valid, show_valid, display_dirty, await_ack, had_usb;
static uint8_t brightness = 32;
static bool await_show, show_received[TILE_LIMIT];
static uint64_t show_target[TILE_LIMIT];
static uint32_t show_sent[TILE_LIMIT], show_since;
static unsigned show_count, show_cursor;
static Packet show_packet;
static char line[512];
static unsigned line_size;
static bool line_overflow;
static int list_at = -1;
static void flood(const Packet *p, unsigned except) {
  for (unsigned d = 0; d < 4; d++)
    if (d != except && mesh.neighbor[d])
      io_send(d, p);
}
static bool route(const Packet *p, uint64_t id) {
  int i = mesh_find(&mesh, id);
  return !mesh.conflict && i >= 0 && mesh.reachable[i] &&
         mesh.route[i] != NO_PORT && io_send(mesh.route[i], p);
}
static bool root_matches(const Packet *p) {
  return p->size >= 16 && read64(p->data) == mesh.root &&
         read32(p->data + 8) == root_boot && !mesh.conflict;
}
static void pattern(void) {
  int i = mesh_find(&mesh, mesh.self);
  int tx = i >= 0 ? mesh.x[i] : 0, ty = i >= 0 ? mesh.y[i] : 0;
  for (unsigned y = 0; y < 8; y++)
    for (unsigned x = 0; x < 8; x++) {
      unsigned at = (y * 8 + x) * 3;
      display[at] = (uint8_t)((tx * 8 + (int)x) * 9);
      display[at + 1] = (uint8_t)((ty * 8 + (int)y) * 9);
      display[at + 2] = 96;
    }
  display_dirty = true;
}
static void accept_pixels(const Packet *p) {
  uint32_t f = read32(p->data + 12);
  if (show_valid && (int32_t)(f - last_show) <= 0)
    return;
  memcpy(staged, p->data + 24, PIXEL_BYTES);
  staged_frame = f;
  staged_valid = true;
  Packet ack = {.type = ACK, .ttl = TILE_LIMIT - 1, .size = 25};
  memcpy(ack.data, p->data, 24);
  ack.data[24] = 0;
  if (mesh.self == mesh.root) {
    if (await_ack && await_frame == f && await_tile == mesh.self) {
      printf("OK FRAME %" PRIu32 "\n", f);
      await_ack = false;
    }
  } else
    route(&ack, mesh.root);
}
static bool accept_show(const Packet *p) {
  uint32_t f = read32(p->data + 12);
  if (show_valid && f == last_show)
    return true;
  if ((show_valid && (int32_t)(f - last_show) < 0) || p->data[17] > 2)
    return false;
  if (p->data[17] == 0 && (!staged_valid || staged_frame != f))
    return false;
  last_show = f;
  show_valid = true;
  brightness = p->data[16];
  if (p->data[17] == 1)
    pattern();
  else if (p->data[17] == 2) {
    memset(display, 0, sizeof display);
    display_dirty = true;
  } else {
    memcpy(display, staged, sizeof display);
    display_dirty = true;
  }
  staged_valid = false;
  return true;
}
static void service_show(uint32_t now) {
  if (!await_show)
    return;
  bool done = true;
  for (unsigned i = 0; i < show_count; i++)
    if (!show_received[i])
      done = false;
  if (done) {
    printf("OK SHOW %" PRIu32 "\n", read32(show_packet.data + 12));
    await_show = false;
    return;
  }
  if ((uint32_t)(now - show_since) > 5000) {
    puts("ERR SHOW_TIMEOUT");
    await_show = false;
    return;
  }
  unsigned i = show_cursor++ % show_count;
  if (show_received[i] ||
      (show_sent[i] && (uint32_t)(now - show_sent[i]) < 250))
    return;
  write64(show_packet.data + 18, show_target[i]);
  if (show_target[i] == mesh.self)
    show_received[i] = accept_show(&show_packet);
  else if (route(&show_packet, show_target[i]))
    show_sent[i] = now;
}
static void begin_show(const Packet *p, uint32_t now) {
  show_packet = *p;
  show_count = 0;
  show_cursor = 0;
  show_since = now;
  await_show = true;
  for (int i = 0; i < TILE_LIMIT; i++)
    if (mesh.reachable[i]) {
      show_target[show_count] = mesh.node[i].id;
      show_received[show_count] = false;
      show_sent[show_count] = 0;
      show_count++;
    }
  for (unsigned i = 0; i < show_count; i++)
    service_show(now);
  service_show(now);
}
static void receive(unsigned port, Packet *p, uint32_t now) {
  if (p->type == HELLO) {
    if (p->size == 12)
      mesh_hello(&mesh, port, read64(p->data), read32(p->data + 8), now);
  } else if (p->type == ADVERTISE) {
    if (p->size != 49 || read64(p->data) == mesh.self)
      return;
    if (mesh_receive(&mesh, p, now) && p->ttl > 1) {
      p->ttl--;
      flood(p, port);
    }
  } else if (p->type == PIXELS && p->size == 24 + PIXEL_BYTES &&
             root_matches(p)) {
    uint64_t target = read64(p->data + 16);
    if (target == mesh.self)
      accept_pixels(p);
    else if (p->ttl > 1) {
      p->ttl--;
      route(p, target);
    }
  } else if (p->type == ACK && p->size == 25 && root_matches(p)) {
    if (mesh.self == mesh.root) {
      if (p->data[24] == 0 && await_ack &&
          read32(p->data + 12) == await_frame &&
          read64(p->data + 16) == await_tile) {
        printf("OK FRAME %" PRIu32 "\n", await_frame);
        await_ack = false;
      }
      if (p->data[24] == 1 && await_show &&
          read32(p->data + 12) == read32(show_packet.data + 12)) {
        for (unsigned i = 0; i < show_count; i++)
          if (show_target[i] == read64(p->data + 16))
            show_received[i] = true;
      }
    } else if (p->ttl > 1) {
      p->ttl--;
      route(p, mesh.root);
    }
  } else if (p->type == SHOW && p->size == 26 && root_matches(p)) {
    uint64_t target = read64(p->data + 18);
    if (target == mesh.self) {
      if (accept_show(p)) {
        Packet ack = {.type = ACK, .ttl = TILE_LIMIT - 1, .size = 25};
        memcpy(ack.data, p->data, 16);
        write64(ack.data + 16, mesh.self);
        ack.data[24] = 1;
        route(&ack, mesh.root);
      }
    } else if (p->ttl > 1) {
      p->ttl--;
      route(p, target);
    }
  }
}
static int hex(char c) {
  if (c >= '0' && c <= '9')
    return c - '0';
  if (c >= 'a' && c <= 'f')
    return c - 'a' + 10;
  if (c >= 'A' && c <= 'F')
    return c - 'A' + 10;
  return -1;
}
static bool number(const char *s, long long min, long long max, long long *v) {
  if (!s || !*s)
    return false;
  char *end;
  *v = strtoll(s, &end, 10);
  return !*end && *v >= min && *v <= max;
}
static void command(uint32_t now) {
  char *argv[7];
  unsigned argc = 0;
  char *save, *tok = strtok_r(line, " \t\r", &save);
  while (tok && argc < 7) {
    argv[argc++] = tok;
    tok = strtok_r(NULL, " \t\r", &save);
  }
  if (!argc)
    return;
  if (argc == 1 && !strcmp(argv[0], "STATUS")) {
    printf("STATUS %016" PRIx64 " ROOT %016" PRIx64
           " TILES %u CONFLICT %u FULL %u DROP %" PRIu32 " BAD %" PRIu32
           " FRAME %" PRIu32 "\n",
           mesh.self, mesh.root, mesh.count, mesh.conflict, mesh.full,
           io_dropped, io_bad_packets, show_valid ? last_show : 0);
    return;
  }
  if (argc == 1 && !strcmp(argv[0], "LIST")) {
    list_at = 0;
    return;
  }
  if (mesh.self != mesh.root) {
    puts("ERR NOT_ROOT");
    return;
  }
  if (mesh.conflict || mesh.full) {
    puts("ERR TOPOLOGY");
    return;
  }
  if (await_ack || await_show) {
    puts("ERR BUSY");
    return;
  }
  long long frame, x, y, b;
  if (argc == 5 && !strcmp(argv[0], "FRAME") &&
      number(argv[1], 0, UINT32_MAX, &frame) &&
      number(argv[2], INT16_MIN, INT16_MAX, &x) &&
      number(argv[3], INT16_MIN, INT16_MAX, &y) && strlen(argv[4]) == 384) {
    if (show_valid && (int32_t)((uint32_t)frame - last_show) <= 0) {
      puts("ERR OLD_FRAME");
      return;
    }
    int i = mesh_at(&mesh, (int)x, (int)y);
    if (i < 0) {
      puts("ERR NO_TILE");
      return;
    }
    Packet p = {
        .type = PIXELS, .ttl = TILE_LIMIT - 1, .size = 24 + PIXEL_BYTES};
    write64(p.data, mesh.self);
    write32(p.data + 8, mesh.boot);
    write32(p.data + 12, (uint32_t)frame);
    write64(p.data + 16, mesh.node[i].id);
    for (unsigned j = 0; j < PIXEL_BYTES; j++) {
      int a = hex(argv[4][j * 2]), c = hex(argv[4][j * 2 + 1]);
      if (a < 0 || c < 0) {
        puts("ERR HEX");
        return;
      }
      p.data[j + 24] = (uint8_t)((a << 4) | c);
    }
    await_ack = true;
    await_frame = (uint32_t)frame;
    await_tile = mesh.node[i].id;
    await_since = now;
    if (await_tile == mesh.self)
      accept_pixels(&p);
    else if (!route(&p, await_tile)) {
      await_ack = false;
      puts("ERR QUEUE");
    }
    return;
  }
  if (argc == 3 &&
      (!strcmp(argv[0], "SHOW") || !strcmp(argv[0], "DEMO") ||
       !strcmp(argv[0], "OFF")) &&
      number(argv[1], 0, UINT32_MAX, &frame) && number(argv[2], 0, 255, &b)) {
    if (show_valid && (int32_t)((uint32_t)frame - last_show) <= 0) {
      puts("ERR OLD_FRAME");
      return;
    }
    Packet p = {.type = SHOW, .ttl = TILE_LIMIT - 1, .size = 26};
    write64(p.data, mesh.self);
    write32(p.data + 8, mesh.boot);
    write32(p.data + 12, (uint32_t)frame);
    p.data[16] = (uint8_t)b;
    p.data[17] = !strcmp(argv[0], "DEMO") ? 1 : !strcmp(argv[0], "OFF") ? 2 : 0;
    begin_show(&p, now);
    return;
  }
  puts("ERR COMMAND");
}
int main(void) {
  io_init();
  stdio_init_all();
  pico_unique_board_id_t id;
  pico_get_unique_board_id(&id);
  uint64_t self = read64(id.id);
  if (!self)
    self = 1;
  mesh_init(&mesh, self, get_rand_32());
  uint32_t last_hello = 0, last_lsa = 0, last_map = 0;
  uint64_t old_root = 0;
  uint32_t old_boot = 0;
  memset(display, 0, sizeof display);
  display_dirty = true;
  for (;;) {
    uint32_t now = to_ms_since_boot(get_absolute_time());
    io_service();
    for (unsigned d = 0; d < 4; d++) {
      Packet p;
      unsigned budget = 8;
      while (budget-- && io_receive(d, &p))
        receive(d, &p, now);
    }
    bool usb = !gpio_get(VBUS_N_PIN) && stdio_usb_connected();
    if ((uint32_t)(now - last_hello) >= 500) {
      last_hello = now;
      Packet p = {.type = HELLO, .ttl = 1, .size = 12};
      write64(p.data, mesh.self);
      write32(p.data + 8, mesh.boot);
      for (unsigned d = 0; d < 4; d++)
        io_send(d, &p);
    }
    if ((uint32_t)(now - last_lsa) >= 1800 || usb != had_usb ||
        !mesh.sequence) {
      last_lsa = now;
      had_usb = usb;
      mesh_expire(&mesh, now);
      Packet p;
      mesh_advertise(&mesh, usb, now, &p);
      flood(&p, NO_PORT);
    }
    if ((uint32_t)(now - last_map) >= 100) {
      last_map = now;
      mesh_rebuild(&mesh, now);
      int i = mesh_find(&mesh, mesh.root);
      root_boot = i >= 0 ? mesh.node[i].boot : 0;
      if (old_root != mesh.root || old_boot != root_boot) {
        old_root = mesh.root;
        old_boot = root_boot;
        staged_valid = show_valid = false;
        if (await_ack || await_show) {
          await_ack = await_show = false;
          puts("ERR ROOT_CHANGED");
        }
        memset(display, 0, sizeof display);
        display_dirty = true;
      }
    }
    if (await_ack && (uint32_t)(now - await_since) > 5000) {
      await_ack = false;
      puts("ERR TIMEOUT");
    }
    service_show(now);
    for (unsigned n = 0; n < 64; n++) {
      int c = getchar_timeout_us(0);
      if (c == PICO_ERROR_TIMEOUT)
        break;
      if (c == '\n') {
        line[line_size] = 0;
        if (line_overflow)
          puts("ERR LINE_TOO_LONG");
        else
          command(now);
        line_size = 0;
        line_overflow = false;
      } else if (line_size < sizeof line - 1)
        line[line_size++] = (char)c;
      else
        line_overflow = true;
    }
    if (list_at >= 0) {
      while (list_at < TILE_LIMIT && !mesh.reachable[list_at])
        list_at++;
      if (list_at == TILE_LIMIT) {
        puts("END");
        list_at = -1;
      } else {
        int i = list_at++;
        printf("TILE %016" PRIx64 " %d %d\n", mesh.node[i].id, mesh.x[i],
               mesh.y[i]);
      }
    }
    if (display_dirty && pixels_ready()) {
      pixels_show(display, brightness);
      display_dirty = false;
    }
    tight_loop_contents();
  }
}
