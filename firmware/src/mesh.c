#include "tile.h"
#include <string.h>
#define LEASE_MS 6500u
#define NEIGHBOR_MS 1700u
static const int dx[4] = {0, 1, 0, -1}, dy[4] = {-1, 0, 1, 0};
int mesh_find(const Mesh *m, uint64_t id) {
  for (int i = 0; i < TILE_LIMIT; i++)
    if (m->node[i].used && m->node[i].id == id)
      return i;
  return -1;
}
static int slot(Mesh *m, uint64_t id) {
  int i = mesh_find(m, id);
  if (i >= 0)
    return i;
  for (i = 0; i < TILE_LIMIT; i++)
    if (!m->node[i].used)
      return i;
  m->full = true;
  return -1;
}
void mesh_init(Mesh *m, uint64_t id, uint32_t boot) {
  memset(m, 0, sizeof *m);
  m->self = id;
  m->boot = boot;
  m->root = id;
  memset(m->route, NO_PORT, sizeof m->route);
}
bool mesh_hello(Mesh *m, unsigned port, uint64_t id, uint32_t boot,
                uint32_t now) {
  if (port > 3 || !id || id == m->self)
    return false;
  bool changed = m->neighbor[port] != id || m->neighbor_boot[port] != boot;
  m->neighbor[port] = id;
  m->neighbor_boot[port] = boot;
  m->neighbor_seen[port] = now;
  if (changed) {
    int i = mesh_find(m, id);
    if (i >= 0 && m->node[i].boot != boot)
      m->node[i].used = false;
  }
  return changed;
}
void mesh_expire(Mesh *m, uint32_t now) {
  if (m->full && (uint32_t)(now - m->overflow_at) > LEASE_MS)
    m->full = false;
  for (unsigned p = 0; p < 4; p++)
    if (m->neighbor[p] && (uint32_t)(now - m->neighbor_seen[p]) > NEIGHBOR_MS)
      m->neighbor[p] = 0;
  for (int i = 0; i < TILE_LIMIT; i++)
    if (m->node[i].used && m->node[i].id != m->self &&
        (uint32_t)(now - m->node[i].seen) > LEASE_MS)
      m->node[i].used = false;
}
void mesh_advertise(Mesh *m, bool usb, uint32_t now, Packet *p) {
  p->type = ADVERTISE;
  p->ttl = TILE_LIMIT - 1;
  p->size = 49;
  write64(p->data, m->self);
  write32(p->data + 8, m->boot);
  write32(p->data + 12, ++m->sequence);
  p->data[16] = usb;
  for (unsigned d = 0; d < 4; d++)
    write64(p->data + 17 + 8 * d, m->neighbor[d]);
  (void)mesh_receive(m, p, now);
}
bool mesh_receive(Mesh *m, const Packet *p, uint32_t now) {
  if (p->type != ADVERTISE || p->size != 49 || p->data[16] > 1)
    return false;
  uint64_t id = read64(p->data);
  uint32_t boot = read32(p->data + 8), seq = read32(p->data + 12);
  if (!id)
    return false;
  int i = slot(m, id);
  if (i < 0) {
    m->overflow_at = now;
    return false;
  }
  Node *v = &m->node[i];
  if (v->used) {
    if (v->boot != boot)
      return false;
    if ((int32_t)(seq - v->sequence) <= 0)
      return false;
  }
  v->used = true;
  v->id = id;
  v->boot = boot;
  v->sequence = seq;
  v->seen = now;
  v->usb = p->data[16];
  for (unsigned d = 0; d < 4; d++) {
    v->neighbor[d] = read64(p->data + 17 + d * 8);
    if (v->neighbor[d] == id)
      v->neighbor[d] = 0;
  }
  return true;
}
void mesh_rebuild(Mesh *m, uint32_t now) {
  mesh_expire(m, now);
  int self = mesh_find(m, m->self);
  if (self < 0)
    return;
  int queue[TILE_LIMIT], head = 0, tail = 0;
  memset(m->reachable, 0, sizeof m->reachable);
  memset(m->route, NO_PORT, sizeof m->route);
  m->reachable[self] = true;
  m->x[self] = m->y[self] = 0;
  queue[tail++] = self;
  m->conflict = false;
  while (head < tail) {
    int i = queue[head++];
    for (unsigned d = 0; d < 4; d++) {
      int j = mesh_find(m, m->node[i].neighbor[d]);
      if (j < 0 || m->node[j].neighbor[(d + 2) % 4] != m->node[i].id)
        continue;
      int x = m->x[i] + dx[d], y = m->y[i] + dy[d];
      if (m->reachable[j]) {
        if (m->x[j] != x || m->y[j] != y)
          m->conflict = true;
        continue;
      }
      m->reachable[j] = true;
      m->x[j] = (int16_t)x;
      m->y[j] = (int16_t)y;
      m->route[j] = (i == self) ? (uint8_t)d : m->route[i];
      queue[tail++] = j;
    }
  }
  m->count = (unsigned)tail;
  int root = self;
  for (int i = 0; i < TILE_LIMIT; i++)
    if (m->reachable[i]) {
      if (m->node[i].usb > m->node[root].usb ||
          (m->node[i].usb == m->node[root].usb &&
           m->node[i].id < m->node[root].id))
        root = i;
      for (int j = 0; j < i; j++)
        if (m->reachable[j] && m->x[j] == m->x[i] && m->y[j] == m->y[i])
          m->conflict = true;
    }
  m->root = m->node[root].id;
  int rx = m->x[root], ry = m->y[root];
  for (int i = 0; i < TILE_LIMIT; i++)
    if (m->reachable[i]) {
      m->x[i] -= rx;
      m->y[i] -= ry;
    }
}
int mesh_at(const Mesh *m, int x, int y) {
  if (m->conflict)
    return -1;
  for (int i = 0; i < TILE_LIMIT; i++)
    if (m->reachable[i] && m->x[i] == x && m->y[i] == y)
      return i;
  return -1;
}
