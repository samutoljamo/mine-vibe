# Multiplayer Backbone Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a server-authoritative multiplayer backbone — headless server, listen-server, and client modes — with player position synchronization, client-side prediction, and remote player interpolation.

**Architecture:** Single binary supporting `--server` (headless), `--host` (listen server), and default (client) modes. A background network thread owns all UDP I/O and exchanges messages with the game loop via lock-free queues. The server runs physics authoritatively using the existing agent-mode path in `player_update()`; clients predict locally and reconcile against server corrections.

**Tech Stack:** C11, POSIX UDP sockets (`sys/socket.h`), existing `platform_thread.h` threading primitives, existing `player.c` agent mode, existing `block_physics.h`.

---

## File Map

| File | Status | Purpose |
|------|--------|---------|
| `src/player.h` | Modify | Export `PHYSICS_DT`, `PLAYER_SPRINT_SPEED` as public defines |
| `src/player.c` | Modify | Remove now-duplicate local defines |
| `src/world.h` | Modify | Add `world_create_headless()` |
| `src/world.c` | Modify | Guard all `world->renderer` calls with NULL check |
| `src/net.h` | Create | Packet types, structs, KEY_BIT_* constants, serialize helpers |
| `src/net.c` | Create | UDP socket helpers, `net_time()` |
| `src/reliable.h` | Create | Per-connection reliable channel struct and API |
| `src/reliable.c` | Create | Send buffer, ACK processing, retransmit |
| `src/net_thread.h` | Create | Network thread, inbound/outbound queue API |
| `src/net_thread.c` | Create | Thread function, queue implementation |
| `src/server.h` | Create | Server API |
| `src/server.c` | Create | Server tick, client management, anti-cheat |
| `src/client.h` | Create | Client API |
| `src/client.c` | Create | Connection state, prediction, reconciliation |
| `src/remote_player.h` | Create | RemotePlayer struct and interpolation API |
| `src/remote_player.c` | Create | Snapshot ring buffer, interpolation |
| `src/renderer.h` | Modify | Add `renderer_draw_remote_players()` declaration |
| `src/renderer.c` | Modify | Placeholder AABB cube mesh + draw call |
| `src/main.c` | Modify | Mode detection, net thread wiring, listen-server |
| `CMakeLists.txt` | Modify | Add new .c files, add `test_net` target |
| `tests/test_net.c` | Create | Unit tests for serialize/deserialize and reliable channel |

---

## Task 1: Export Player Constants

**Files:**
- Modify: `src/player.h`
- Modify: `src/player.c`

- [ ] **Step 1: Add public defines to `src/player.h`**

  Below the `#include` block, before the `typedef enum PlayerMode`:
  ```c
  /* Physics constants — shared with server-side simulation */
  #define PHYSICS_DT          (1.0f / 60.0f)
  #define PLAYER_SPRINT_SPEED 5.6f
  ```

- [ ] **Step 2: Remove duplicate defines from `src/player.c`**

  Delete these two lines (they are now in the header):
  ```c
  #define SPRINT_SPEED     5.6f
  ```
  And update the one reference to `SPRINT_SPEED` in `tick_walking`:
  ```c
  if (player->sprinting) speed = PLAYER_SPRINT_SPEED;
  ```
  Remove the `PHYSICS_DT` define too — it's now in the header.

- [ ] **Step 3: Build to verify no errors**
  ```bash
  cd /var/home/samu/minecraft && distrobox run -- cmake --build build 2>&1 | tail -5
  ```
  Expected: clean build, no errors.

- [ ] **Step 4: Commit**
  ```bash
  git add src/player.h src/player.c
  git commit -m "feat: export PHYSICS_DT and PLAYER_SPRINT_SPEED to player.h"
  ```

---

## Task 2: Headless World Support

The server needs a `World*` for collision queries but has no `Renderer`. Add a `world_create_headless()` wrapper and guard all `world->renderer` accesses in `world.c`.

**Files:**
- Modify: `src/world.h`
- Modify: `src/world.c`

- [ ] **Step 1: Add `world_create_headless` to `src/world.h`**

  After the existing `world_create` declaration:
  ```c
  /* Create a world with no renderer (server use). Mesh generation still
   * runs on worker threads but GPU uploads are skipped. */
  World* world_create_headless(int seed, int render_distance);
  ```

- [ ] **Step 2: Implement in `src/world.c`**

  Grep for every use of `world->renderer` in `world.c`:
  ```bash
  grep -n "world->renderer" src/world.c
  ```
  Wrap each call in `if (world->renderer) { ... }`. Then add after `world_create`:
  ```c
  World* world_create_headless(int seed, int render_distance)
  {
      return world_create(NULL, seed, render_distance);
  }
  ```

- [ ] **Step 3: Build**
  ```bash
  distrobox run -- cmake --build build 2>&1 | tail -5
  ```
  Expected: clean build.

- [ ] **Step 4: Commit**
  ```bash
  git add src/world.h src/world.c
  git commit -m "feat: add world_create_headless for server-side collision-only world"
  ```

---

## Task 3: Packet Protocol — `net.h`

Define all wire types, serialization helpers, and the KEY_BIT_* constants. No .c file yet — pure header.

**Files:**
- Create: `src/net.h`

- [ ] **Step 1: Write `src/net.h`**

  ```c
  #ifndef NET_H
  #define NET_H

  #include <stdint.h>
  #include <stddef.h>
  #include <string.h>

  /* ------------------------------------------------------------------ */
  /*  Key bitmask (InputPacket.keys and InputState.keys)                */
  /* ------------------------------------------------------------------ */
  #define KEY_BIT_W      (1 << 0)
  #define KEY_BIT_S      (1 << 1)
  #define KEY_BIT_A      (1 << 2)
  #define KEY_BIT_D      (1 << 3)
  #define KEY_BIT_SPACE  (1 << 4)
  #define KEY_BIT_SPRINT (1 << 5)
  #define KEY_BIT_SHIFT  (1 << 6)

  /* ------------------------------------------------------------------ */
  /*  Packet types                                                       */
  /* ------------------------------------------------------------------ */
  typedef enum {
      PKT_CONNECT_REQUEST = 0,
      PKT_CONNECT_ACCEPT  = 1,
      PKT_DISCONNECT      = 2,
      PKT_INPUT           = 3,
      PKT_WORLD_STATE     = 4,
      PKT_PLAYER_JOIN     = 5,
      PKT_PLAYER_LEAVE    = 6,
      PKT_BLOCK_CHANGE    = 7,  /* reserved for future use */
  } PacketType;

  #define NET_MAX_PLAYERS  255
  #define NET_DEFAULT_PORT 25565
  #define NET_MAX_PACKET   1400  /* safe below typical MTU */

  /* ------------------------------------------------------------------ */
  /*  Wire serialization helpers — write/read little-endian             */
  /* ------------------------------------------------------------------ */

  static inline void net_write_u8(uint8_t* buf, size_t* off, uint8_t v)
  {
      buf[(*off)++] = v;
  }

  static inline void net_write_u16(uint8_t* buf, size_t* off, uint16_t v)
  {
      buf[(*off)++] = (uint8_t)(v & 0xFF);
      buf[(*off)++] = (uint8_t)(v >> 8);
  }

  static inline void net_write_u32(uint8_t* buf, size_t* off, uint32_t v)
  {
      buf[(*off)++] = (uint8_t)(v & 0xFF);
      buf[(*off)++] = (uint8_t)((v >> 8) & 0xFF);
      buf[(*off)++] = (uint8_t)((v >> 16) & 0xFF);
      buf[(*off)++] = (uint8_t)(v >> 24);
  }

  static inline void net_write_float(uint8_t* buf, size_t* off, float v)
  {
      uint32_t bits;
      memcpy(&bits, &v, 4);
      net_write_u32(buf, off, bits);
  }

  static inline uint8_t net_read_u8(const uint8_t* buf, size_t* off)
  {
      return buf[(*off)++];
  }

  static inline uint16_t net_read_u16(const uint8_t* buf, size_t* off)
  {
      uint16_t v = (uint16_t)buf[*off] | ((uint16_t)buf[*off + 1] << 8);
      *off += 2;
      return v;
  }

  static inline uint32_t net_read_u32(const uint8_t* buf, size_t* off)
  {
      uint32_t v = (uint32_t)buf[*off]
                 | ((uint32_t)buf[*off+1] << 8)
                 | ((uint32_t)buf[*off+2] << 16)
                 | ((uint32_t)buf[*off+3] << 24);
      *off += 4;
      return v;
  }

  static inline float net_read_float(const uint8_t* buf, size_t* off)
  {
      uint32_t bits = net_read_u32(buf, off);
      float v;
      memcpy(&v, &bits, 4);
      return v;
  }

  /* ------------------------------------------------------------------ */
  /*  Packet header — 8 bytes on the wire                               */
  /* ------------------------------------------------------------------ */
  typedef struct {
      uint8_t  type;       /* PacketType */
      uint8_t  player_id;  /* sender (0 = server) */
      uint16_t seq;        /* sequence number */
      uint16_t ack;        /* last seq received from remote */
      uint16_t ack_bits;   /* bitmask ACK: bit i set = ack-1-i was received */
  } PacketHeader;

  #define HEADER_WIRE_SIZE 8

  static inline void net_write_header(uint8_t* buf, size_t* off,
                                       const PacketHeader* h)
  {
      net_write_u8(buf, off, h->type);
      net_write_u8(buf, off, h->player_id);
      net_write_u16(buf, off, h->seq);
      net_write_u16(buf, off, h->ack);
      net_write_u16(buf, off, h->ack_bits);
  }

  static inline void net_read_header(const uint8_t* buf, size_t* off,
                                      PacketHeader* h)
  {
      h->type      = net_read_u8(buf, off);
      h->player_id = net_read_u8(buf, off);
      h->seq       = net_read_u16(buf, off);
      h->ack       = net_read_u16(buf, off);
      h->ack_bits  = net_read_u16(buf, off);
  }

  /* ------------------------------------------------------------------ */
  /*  InputPacket — 21 wire bytes                                        */
  /* ------------------------------------------------------------------ */
  typedef struct {
      PacketHeader header;
      uint32_t     tick;
      uint8_t      keys;
      float        yaw;
      float        pitch;
  } InputPacket;

  static inline size_t net_write_input(uint8_t* buf, const InputPacket* p)
  {
      size_t off = 0;
      net_write_header(buf, &off, &p->header);
      net_write_u32(buf, &off, p->tick);
      net_write_u8(buf, &off, p->keys);
      net_write_float(buf, &off, p->yaw);
      net_write_float(buf, &off, p->pitch);
      return off; /* 21 */
  }

  static inline void net_read_input(const uint8_t* buf, InputPacket* p)
  {
      size_t off = 0;
      net_read_header(buf, &off, &p->header);
      p->tick  = net_read_u32(buf, &off);
      p->keys  = net_read_u8(buf, &off);
      p->yaw   = net_read_float(buf, &off);
      p->pitch = net_read_float(buf, &off);
  }

  /* ------------------------------------------------------------------ */
  /*  WorldStatePacket — 9 + 21*N wire bytes                            */
  /* ------------------------------------------------------------------ */
  #define WORLD_STATE_PLAYER_SIZE 21  /* 1 + 4 + 4 + 4 + 4 + 4 */

  typedef struct {
      uint8_t  player_id;
      float    x, y, z;
      float    yaw, pitch;
  } NetPlayerState;

  static inline size_t net_write_world_state(uint8_t* buf,
                                              const PacketHeader* hdr,
                                              const NetPlayerState* players,
                                              uint8_t count)
  {
      size_t off = 0;
      net_write_header(buf, &off, hdr);
      net_write_u8(buf, &off, count);
      for (int i = 0; i < count; i++) {
          net_write_u8(buf, &off, players[i].player_id);
          net_write_float(buf, &off, players[i].x);
          net_write_float(buf, &off, players[i].y);
          net_write_float(buf, &off, players[i].z);
          net_write_float(buf, &off, players[i].yaw);
          net_write_float(buf, &off, players[i].pitch);
      }
      return off;
  }

  /* ------------------------------------------------------------------ */
  /*  Simple reliable-channel packets (header only, no extra payload)   */
  /*  Used for: PKT_CONNECT_REQUEST, PKT_CONNECT_ACCEPT, PKT_DISCONNECT */
  /*            PKT_PLAYER_JOIN, PKT_PLAYER_LEAVE                        */
  /* ------------------------------------------------------------------ */
  /* PKT_CONNECT_ACCEPT carries assigned player_id in header.player_id  */
  /* PKT_PLAYER_JOIN/LEAVE carry the affected player_id in player_id    */

  #endif /* NET_H */
  ```

- [ ] **Step 2: Build (net.h is header-only — add a stub net.c so CMake compiles it)**

  Create `src/net.c` with just an include for now:
  ```c
  #include "net.h"
  ```
  Add to `CMakeLists.txt` sources list (Task 11 will add all files; add `src/net.c` now as a stub).

- [ ] **Step 3: Build**
  ```bash
  distrobox run -- cmake --build build 2>&1 | tail -5
  ```

- [ ] **Step 4: Commit**
  ```bash
  git add src/net.h src/net.c CMakeLists.txt
  git commit -m "feat: add net.h packet protocol and wire serialization helpers"
  ```

---

## Task 4: UDP Socket Helpers + `net_time()`

**Files:**
- Modify: `src/net.h` (add declarations)
- Modify: `src/net.c` (implement)

- [ ] **Step 1: Add declarations to `src/net.h`** (before `#endif`):

  ```c
  /* ------------------------------------------------------------------ */
  /*  UDP socket helpers                                                 */
  /* ------------------------------------------------------------------ */
  #include <netinet/in.h>

  /* Returns socket fd (non-blocking), or -1 on error.
   * Server: binds to 0.0.0.0:port. Client: unbound, use sendto. */
  int  net_socket_server(uint16_t port);
  int  net_socket_client(void);
  void net_socket_close(int fd);

  /* Returns bytes sent/received, 0 on EAGAIN/EWOULDBLOCK, -1 on error. */
  int  net_send(int fd, const void* buf, int len,
                const struct sockaddr_in* addr);
  int  net_recv(int fd, void* buf, int len,
                struct sockaddr_in* from);

  /* Monotonic clock in seconds — valid in both server and client modes */
  double net_time(void);
  ```

- [ ] **Step 2: Implement in `src/net.c`**

  ```c
  #include "net.h"
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <fcntl.h>
  #include <unistd.h>
  #include <time.h>
  #include <errno.h>
  #include <stdio.h>

  static int make_nonblocking(int fd)
  {
      int flags = fcntl(fd, F_GETFL, 0);
      if (flags < 0) return -1;
      return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  int net_socket_server(uint16_t port)
  {
      int fd = socket(AF_INET, SOCK_DGRAM, 0);
      if (fd < 0) return -1;

      int reuse = 1;
      setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

      struct sockaddr_in addr = {0};
      addr.sin_family      = AF_INET;
      addr.sin_port        = htons(port);
      addr.sin_addr.s_addr = INADDR_ANY;

      if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
          close(fd);
          return -1;
      }
      make_nonblocking(fd);
      return fd;
  }

  int net_socket_client(void)
  {
      int fd = socket(AF_INET, SOCK_DGRAM, 0);
      if (fd < 0) return -1;
      make_nonblocking(fd);
      return fd;
  }

  void net_socket_close(int fd)
  {
      if (fd >= 0) close(fd);
  }

  int net_send(int fd, const void* buf, int len,
               const struct sockaddr_in* addr)
  {
      int n = (int)sendto(fd, buf, (size_t)len, 0,
                          (const struct sockaddr*)addr, sizeof(*addr));
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
      return n;
  }

  int net_recv(int fd, void* buf, int len, struct sockaddr_in* from)
  {
      socklen_t fromlen = sizeof(*from);
      int n = (int)recvfrom(fd, buf, (size_t)len, 0,
                             (struct sockaddr*)from, &fromlen);
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
      return n;
  }

  double net_time(void)
  {
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
  }
  ```

- [ ] **Step 3: Build**
  ```bash
  distrobox run -- cmake --build build 2>&1 | tail -5
  ```

- [ ] **Step 4: Commit**
  ```bash
  git add src/net.h src/net.c
  git commit -m "feat: add UDP socket helpers and net_time()"
  ```

---

## Task 5: Reliable Channel

Handles per-connection reliable delivery: sequence numbers, ACKs, retransmit.

**Files:**
- Create: `src/reliable.h`
- Create: `src/reliable.c`

- [ ] **Step 1: Write `src/reliable.h`**

  ```c
  #ifndef RELIABLE_H
  #define RELIABLE_H

  #include <stdint.h>
  #include <stdbool.h>
  #include <stddef.h>
  #include <netinet/in.h>

  #define RELIABLE_WINDOW      32
  #define RELIABLE_TIMEOUT     0.1   /* seconds before retransmit */
  #define RELIABLE_MAX_PAYLOAD 256

  typedef struct {
      uint8_t  data[RELIABLE_MAX_PAYLOAD];
      uint16_t len;
      uint16_t seq;
      double   sent_time;
      bool     in_use;
  } ReliableEntry;

  typedef struct {
      /* Send side */
      ReliableEntry send_buf[RELIABLE_WINDOW];
      uint16_t      next_seq;

      /* Receive side — track which seqs we've seen, for ack_bits */
      uint16_t      last_recv_seq;
      uint16_t      recv_bits;      /* bit i: ack-1-i was received */
      bool          recv_any;       /* false until first packet arrives */
  } ReliableChannel;

  void reliable_init(ReliableChannel* ch);

  /* Queue a reliable message. Sends immediately; also stores for retransmit.
   * fd/addr: the socket to send on. seq is assigned from ch->next_seq.
   * Returns the assigned seq. */
  uint16_t reliable_send(ReliableChannel* ch, int fd,
                          const struct sockaddr_in* addr,
                          const uint8_t* data, uint16_t len);

  /* Call when any packet arrives. Updates recv tracking for ack generation.
   * Also processes ack/ack_bits to remove confirmed entries from send_buf. */
  void reliable_on_recv(ReliableChannel* ch, uint16_t seq,
                         uint16_t ack, uint16_t ack_bits);

  /* Populate ack and ack_bits fields in an outgoing packet header based on
   * what we have received from the remote side. */
  void reliable_fill_ack(const ReliableChannel* ch,
                          uint16_t* out_ack, uint16_t* out_ack_bits);

  /* Retransmit any send_buf entries older than RELIABLE_TIMEOUT.
   * Call once per tick. */
  void reliable_tick(ReliableChannel* ch, int fd,
                      const struct sockaddr_in* addr);

  #endif /* RELIABLE_H */
  ```

- [ ] **Step 2: Write `src/reliable.c`**

  ```c
  #include "reliable.h"
  #include "net.h"
  #include <string.h>
  #include <stdio.h>

  void reliable_init(ReliableChannel* ch)
  {
      memset(ch, 0, sizeof(*ch));
  }

  uint16_t reliable_send(ReliableChannel* ch, int fd,
                          const struct sockaddr_in* addr,
                          const uint8_t* data, uint16_t len)
  {
      uint16_t seq = ch->next_seq++;

      /* Find a free slot */
      int slot = seq % RELIABLE_WINDOW;
      /* If slot is in use (window full), evict — packet loss acceptable here */
      ReliableEntry* e = &ch->send_buf[slot];
      if (len > RELIABLE_MAX_PAYLOAD) len = RELIABLE_MAX_PAYLOAD;
      memcpy(e->data, data, len);
      e->len       = len;
      e->seq       = seq;
      e->sent_time = net_time();
      e->in_use    = true;

      net_send(fd, data, len, addr);
      return seq;
  }

  void reliable_on_recv(ReliableChannel* ch, uint16_t seq,
                         uint16_t ack, uint16_t ack_bits)
  {
      /* Update receive tracking */
      if (!ch->recv_any) {
          ch->last_recv_seq = seq;
          ch->recv_bits     = 0;
          ch->recv_any      = true;
      } else {
          int16_t diff = (int16_t)(seq - ch->last_recv_seq);
          if (diff > 0) {
              if (diff >= 16) ch->recv_bits = 0;
              else            ch->recv_bits <<= diff;
              ch->recv_bits |= (1u << (diff - 1)); /* mark previous */
              ch->last_recv_seq = seq;
          } else if (diff < 0 && diff > -16) {
              ch->recv_bits |= (1u << (-diff - 1));
          }
      }

      /* Process ACKs — remove confirmed entries from send_buf */
      for (int i = 0; i < RELIABLE_WINDOW; i++) {
          ReliableEntry* e = &ch->send_buf[i];
          if (!e->in_use) continue;
          int16_t diff = (int16_t)(ack - e->seq);
          if (diff == 0) {
              e->in_use = false;
          } else if (diff > 0 && diff <= 16) {
              if (ack_bits & (1u << (diff - 1)))
                  e->in_use = false;
          }
      }
  }

  void reliable_fill_ack(const ReliableChannel* ch,
                          uint16_t* out_ack, uint16_t* out_ack_bits)
  {
      *out_ack      = ch->recv_any ? ch->last_recv_seq : 0;
      *out_ack_bits = ch->recv_bits;
  }

  void reliable_tick(ReliableChannel* ch, int fd,
                      const struct sockaddr_in* addr)
  {
      double now = net_time();
      for (int i = 0; i < RELIABLE_WINDOW; i++) {
          ReliableEntry* e = &ch->send_buf[i];
          if (e->in_use && (now - e->sent_time) > RELIABLE_TIMEOUT) {
              net_send(fd, e->data, e->len, addr);
              e->sent_time = now;
          }
      }
  }
  ```

- [ ] **Step 3: Add to CMakeLists.txt sources and build**
  ```bash
  distrobox run -- cmake --build build 2>&1 | tail -5
  ```

- [ ] **Step 4: Commit**
  ```bash
  git add src/reliable.h src/reliable.c CMakeLists.txt
  git commit -m "feat: add reliable UDP channel (send buffer, ACK, retransmit)"
  ```

---

## Task 6: Network Thread

Background thread that owns the socket. Inbound: recv → push to queue. Outbound: pop queue → send.

**Files:**
- Create: `src/net_thread.h`
- Create: `src/net_thread.c`

- [ ] **Step 1: Write `src/net_thread.h`**

  ```c
  #ifndef NET_THREAD_H
  #define NET_THREAD_H

  #include <stdint.h>
  #include <stdbool.h>
  #include <stddef.h>
  #include <netinet/in.h>
  #include "platform_thread.h"
  #include <stdatomic.h>

  #define NET_THREAD_MAX_MSG 512   /* max bytes per queued message */

  /* A message in either queue */
  typedef struct NetMsg {
      uint8_t             data[NET_THREAD_MAX_MSG];
      int                 len;
      struct sockaddr_in  addr;
      double              recv_time; /* net_time() at receive; 0 for outbound */
      struct NetMsg*      next;
  } NetMsg;

  /* Thread + queues */
  typedef struct {
      int          fd;
      PT_Thread    thread;
      _Atomic bool running;

      /* Inbound: network → game logic */
      NetMsg*      in_head;
      NetMsg*      in_tail;
      PT_Mutex     in_mutex;

      /* Outbound: game logic → network */
      NetMsg*      out_head;
      NetMsg*      out_tail;
      PT_Mutex     out_mutex;
  } NetThread;

  /* fd must already be open and non-blocking (from net_socket_server/client) */
  bool net_thread_start(NetThread* nt, int fd);
  void net_thread_stop(NetThread* nt);

  /* Push a message to send. Copies data. */
  void net_thread_push_outbound(NetThread* nt, const void* data, int len,
                                 const struct sockaddr_in* addr);

  /* Pop a received message. Returns NULL if queue empty.
   * Caller must free() the returned NetMsg. */
  NetMsg* net_thread_pop_inbound(NetThread* nt);

  #endif /* NET_THREAD_H */
  ```

- [ ] **Step 2: Write `src/net_thread.c`**

  ```c
  #include "net_thread.h"
  #include "net.h"
  #include <stdlib.h>
  #include <string.h>
  #include <time.h>

  static void* thread_func(void* arg)
  {
      NetThread* nt = (NetThread*)arg;
      uint8_t buf[NET_THREAD_MAX_MSG];

      while (atomic_load(&nt->running)) {
          /* Drain inbound socket */
          struct sockaddr_in from;
          int n;
          while ((n = net_recv(nt->fd, buf, sizeof(buf), &from)) > 0) {
              NetMsg* msg = malloc(sizeof(NetMsg));
              if (!msg) continue;
              memcpy(msg->data, buf, (size_t)n);
              msg->len       = n;
              msg->addr      = from;
              msg->recv_time = net_time();
              msg->next      = NULL;

              pt_mutex_lock(&nt->in_mutex);
              if (nt->in_tail) nt->in_tail->next = msg;
              else             nt->in_head = msg;
              nt->in_tail = msg;
              pt_mutex_unlock(&nt->in_mutex);
          }

          /* Drain outbound queue */
          pt_mutex_lock(&nt->out_mutex);
          NetMsg* out = nt->out_head;
          nt->out_head = nt->out_tail = NULL;
          pt_mutex_unlock(&nt->out_mutex);

          while (out) {
              net_send(nt->fd, out->data, out->len, &out->addr);
              NetMsg* next = out->next;
              free(out);
              out = next;
          }

          /* ~1ms sleep to avoid burning CPU */
          struct timespec ts = { 0, 1000000 };
          nanosleep(&ts, NULL);
      }
      return NULL;
  }

  bool net_thread_start(NetThread* nt, int fd)
  {
      nt->fd       = fd;
      nt->in_head  = nt->in_tail  = NULL;
      nt->out_head = nt->out_tail = NULL;
      pt_mutex_init(&nt->in_mutex);
      pt_mutex_init(&nt->out_mutex);
      atomic_store(&nt->running, true);
      pt_thread_create(&nt->thread, thread_func, nt); /* void return */
      return true;
  }

  void net_thread_stop(NetThread* nt)
  {
      atomic_store(&nt->running, false);
      pt_thread_join(nt->thread); /* pt_thread_join takes PT_Thread by value */
      pt_mutex_destroy(&nt->in_mutex);
      pt_mutex_destroy(&nt->out_mutex);
      /* Drain remaining queued messages */
      NetMsg* m;
      while ((m = net_thread_pop_inbound(nt))) free(m);
      pt_mutex_lock(&nt->out_mutex);
      m = nt->out_head;
      nt->out_head = nt->out_tail = NULL;
      pt_mutex_unlock(&nt->out_mutex);
      while (m) { NetMsg* n = m->next; free(m); m = n; }
  }

  void net_thread_push_outbound(NetThread* nt, const void* data, int len,
                                  const struct sockaddr_in* addr)
  {
      if (len <= 0 || len > NET_THREAD_MAX_MSG) return;
      NetMsg* msg = malloc(sizeof(NetMsg));
      if (!msg) return;
      memcpy(msg->data, data, (size_t)len);
      msg->len  = len;
      msg->addr = *addr;
      msg->next = NULL;

      pt_mutex_lock(&nt->out_mutex);
      if (nt->out_tail) nt->out_tail->next = msg;
      else              nt->out_head = msg;
      nt->out_tail = msg;
      pt_mutex_unlock(&nt->out_mutex);
  }

  NetMsg* net_thread_pop_inbound(NetThread* nt)
  {
      pt_mutex_lock(&nt->in_mutex);
      NetMsg* msg = nt->in_head;
      if (msg) {
          nt->in_head = msg->next;
          if (!nt->in_head) nt->in_tail = NULL;
      }
      pt_mutex_unlock(&nt->in_mutex);
      return msg;
  }
  ```

- [ ] **Step 3: Check `platform_thread.h` for exact API names**
  ```bash
  grep -n "pt_thread_create\|pt_thread_join\|pt_mutex" src/platform_thread.h | head -20
  ```
  Adjust function names in `net_thread.c` to match the actual API.

- [ ] **Step 4: Add to CMake and build**
  ```bash
  distrobox run -- cmake --build build 2>&1 | tail -5
  ```

- [ ] **Step 5: Write unit test `tests/test_net.c`**

  Test serialization round-trips and basic reliable channel ACK logic:
  ```c
  #include <assert.h>
  #include <stdio.h>
  #include <string.h>
  #include "../src/net.h"
  #include "../src/reliable.h"

  static void test_serialize_input(void)
  {
      InputPacket p = {0};
      p.header.type      = PKT_INPUT;
      p.header.player_id = 3;
      p.header.seq       = 1000;
      p.header.ack       = 999;
      p.header.ack_bits  = 0xF0F0;
      p.tick  = 12345;
      p.keys  = KEY_BIT_W | KEY_BIT_SPRINT;
      p.yaw   = 1.57f;
      p.pitch = -0.3f;

      uint8_t buf[64];
      size_t len = net_write_input(buf, &p);
      assert(len == 21);

      InputPacket q = {0};
      net_read_input(buf, &q);
      assert(q.header.type      == PKT_INPUT);
      assert(q.header.player_id == 3);
      assert(q.header.seq       == 1000);
      assert(q.tick             == 12345);
      assert(q.keys             == (KEY_BIT_W | KEY_BIT_SPRINT));
      /* float round-trip: exact for these values */
      assert(q.yaw   == 1.57f);
      assert(q.pitch == -0.3f);
      printf("test_serialize_input: PASS\n");
  }

  static void test_reliable_ack(void)
  {
      ReliableChannel ch;
      reliable_init(&ch);
      /* Simulate receiving seqs 0, 1, 2 */
      reliable_on_recv(&ch, 0, 0, 0);
      reliable_on_recv(&ch, 1, 0, 0);
      reliable_on_recv(&ch, 2, 0, 0);
      uint16_t ack, bits;
      reliable_fill_ack(&ch, &ack, &bits);
      assert(ack == 2);
      /* bits: seq 1 = diff 1, bit 0; seq 0 = diff 2, bit 1 */
      assert(bits & (1 << 0)); /* seq 1 received */
      assert(bits & (1 << 1)); /* seq 0 received */
      printf("test_reliable_ack: PASS\n");
  }

  int main(void)
  {
      test_serialize_input();
      test_reliable_ack();
      printf("All net tests passed.\n");
      return 0;
  }
  ```

- [ ] **Step 6: Add test target to CMakeLists.txt**

  ```cmake
  add_executable(test_net tests/test_net.c src/net.c src/reliable.c)
  target_include_directories(test_net PRIVATE ${CMAKE_SOURCE_DIR}/src)
  add_test(NAME net COMMAND test_net)
  ```

- [ ] **Step 7: Run tests**
  ```bash
  distrobox run -- cmake --build build && distrobox run -- ctest --test-dir build -R net -V
  ```
  Expected: `All net tests passed.`

- [ ] **Step 8: Commit**
  ```bash
  git add src/net_thread.h src/net_thread.c tests/test_net.c CMakeLists.txt
  git commit -m "feat: add network thread with inbound/outbound queues and net unit tests"
  ```

---

## Task 7: Server

**Files:**
- Create: `src/server.h`
- Create: `src/server.c`

- [ ] **Step 1: Write `src/server.h`**

  ```c
  #ifndef SERVER_H
  #define SERVER_H

  #include <stdint.h>
  #include <stdbool.h>
  #include <netinet/in.h>
  #include "player.h"
  #include "reliable.h"

  #define SERVER_MAX_CLIENTS  32
  #define SERVER_TICK_RATE    20       /* Hz */
  #define SERVER_TIMEOUT_SEC  10.0

  typedef struct {
      bool               active;
      struct sockaddr_in addr;
      uint8_t            player_id;   /* 1–255 */
      double             last_input_time;
      uint32_t           last_input_tick;
      float              yaw, pitch;
      Player             player;
      ReliableChannel    reliable;
  } ServerClient;

  typedef struct NetThread NetThread;
  typedef struct World     World;
  typedef struct BlockPhysics BlockPhysics;

  typedef struct {
      NetThread*   net;
      World*       world;
      BlockPhysics bp;
      ServerClient clients[SERVER_MAX_CLIENTS];
      int          max_clients;    /* runtime cap, <= SERVER_MAX_CLIENTS */
      int          seed;
      bool         running;
  } Server;

  /* Blocking server loop — call from a dedicated thread or main().
   * port: UDP port to bind. max_clients: runtime cap.
   * Runs until server.running is set false (or fatal error). */
  void server_run(uint16_t port, int max_clients, int seed);

  #endif /* SERVER_H */
  ```

- [ ] **Step 2: Write `src/server.c`**

  ```c
  #include "server.h"
  #include "net.h"
  #include "net_thread.h"
  #include "block_physics.h"
  #include "world.h"
  #include "player.h"
  #include <stdlib.h>
  #include <stdio.h>
  #include <string.h>
  #include <math.h>
  #include <time.h>

  /* ------------------------------------------------------------------ */
  /*  Helpers                                                            */
  /* ------------------------------------------------------------------ */

  static ServerClient* find_client_by_addr(Server* s,
                                             const struct sockaddr_in* addr)
  {
      for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
          if (!s->clients[i].active) continue;
          if (s->clients[i].addr.sin_addr.s_addr == addr->sin_addr.s_addr
              && s->clients[i].addr.sin_port == addr->sin_port)
              return &s->clients[i];
      }
      return NULL;
  }

  static ServerClient* alloc_client(Server* s, const struct sockaddr_in* addr)
  {
      int active = 0;
      for (int i = 0; i < SERVER_MAX_CLIENTS; i++)
          if (s->clients[i].active) active++;
      if (active >= s->max_clients) return NULL;

      /* Find free slot and assign lowest available player_id */
      uint8_t used[256] = {0};
      for (int i = 0; i < SERVER_MAX_CLIENTS; i++)
          if (s->clients[i].active)
              used[s->clients[i].player_id] = 1;

      uint8_t pid = 0;
      for (int id = 1; id <= 255; id++) {
          if (!used[id]) { pid = (uint8_t)id; break; }
      }
      if (!pid) return NULL;

      for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
          if (s->clients[i].active) continue;
          ServerClient* c = &s->clients[i];
          memset(c, 0, sizeof(*c));
          c->active    = true;
          c->addr      = *addr;
          c->player_id = pid;
          c->last_input_time = net_time();
          reliable_init(&c->reliable);

          vec3 spawn = { 0.0f, 80.0f, 0.0f };
          player_init(&c->player, spawn);
          c->player.agent_mode = true;
          c->player.mode       = MODE_WALKING;
          c->player.noclip     = false;
          return c;
      }
      return NULL;
  }

  static void send_reliable(Server* s, ServerClient* c,
                              const uint8_t* data, uint16_t len)
  {
      uint16_t ack, bits;
      reliable_fill_ack(&c->reliable, &ack, &bits);
      /* Patch ack into header before queuing */
      uint8_t buf[RELIABLE_MAX_PAYLOAD];
      if (len > RELIABLE_MAX_PAYLOAD) return;
      memcpy(buf, data, len);
      /* header.ack is at bytes 4-5, ack_bits at 6-7 (little-endian) */
      buf[4] = (uint8_t)(ack & 0xFF);
      buf[5] = (uint8_t)(ack >> 8);
      buf[6] = (uint8_t)(bits & 0xFF);
      buf[7] = (uint8_t)(bits >> 8);
      reliable_send(&c->reliable, s->net->fd, &c->addr, buf, len);
  }

  static void broadcast_player_join(Server* s, ServerClient* new_client)
  {
      uint8_t buf[HEADER_WIRE_SIZE];
      size_t off = 0;
      PacketHeader h = {
          .type      = PKT_PLAYER_JOIN,
          .player_id = new_client->player_id,
          .seq       = 0, /* will be set by reliable_send */
      };
      net_write_header(buf, &off, &h);
      for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
          if (!s->clients[i].active) continue;
          send_reliable(s, &s->clients[i], buf, (uint16_t)off);
      }
  }

  static void broadcast_player_leave(Server* s, uint8_t pid)
  {
      uint8_t buf[HEADER_WIRE_SIZE];
      size_t off = 0;
      PacketHeader h = {
          .type      = PKT_PLAYER_LEAVE,
          .player_id = pid,
          .seq       = 0,
      };
      net_write_header(buf, &off, &h);
      for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
          if (!s->clients[i].active) continue;
          if (s->clients[i].player_id == pid) continue;
          send_reliable(s, &s->clients[i], buf, (uint16_t)off);
      }
  }

  static void disconnect_client(Server* s, ServerClient* c)
  {
      if (!c->active) return;
      printf("[server] player %d disconnected\n", c->player_id);
      broadcast_player_leave(s, c->player_id);
      c->active = false;
  }

  /* ------------------------------------------------------------------ */
  /*  Packet handling                                                    */
  /* ------------------------------------------------------------------ */

  static void handle_connect_request(Server* s, const struct sockaddr_in* addr)
  {
      /* Ignore if already connected */
      if (find_client_by_addr(s, addr)) return;

      ServerClient* c = alloc_client(s, addr);
      if (!c) {
          printf("[server] connection refused: server full\n");
          return;
      }
      printf("[server] player %d connected\n", c->player_id);

      /* Send PKT_CONNECT_ACCEPT with assigned player_id */
      uint8_t buf[HEADER_WIRE_SIZE];
      size_t off = 0;
      PacketHeader h = {
          .type      = PKT_CONNECT_ACCEPT,
          .player_id = c->player_id,
          .seq       = 0,
      };
      net_write_header(buf, &off, &h);
      send_reliable(s, c, buf, (uint16_t)off);

      /* Notify all other clients */
      broadcast_player_join(s, c);
  }

  static void handle_input(Server* s, ServerClient* c,
                            const uint8_t* data, int len)
  {
      if (len < 21) return;
      InputPacket p;
      net_read_input(data, &p);

      /* Reliable ACK processing */
      reliable_on_recv(&c->reliable, p.header.seq, p.header.ack,
                        p.header.ack_bits);

      /* Stale input check */
      if (c->last_input_time > 0 && p.tick <= c->last_input_tick) return;
      c->last_input_tick = p.tick;
      c->last_input_time = net_time();
      c->yaw   = p.yaw;
      c->pitch = p.pitch;

      /* Set camera orientation */
      c->player.camera.yaw   = p.yaw;
      c->player.camera.pitch = p.pitch;

      /* Map keys bitmask to agent fields — physics runs once in server_tick */
      uint8_t keys = p.keys;
      c->player.agent_forward = (keys & KEY_BIT_W) ? 1.0f
                               : (keys & KEY_BIT_S) ? -1.0f : 0.0f;
      c->player.agent_right   = (keys & KEY_BIT_D) ? 1.0f
                               : (keys & KEY_BIT_A) ? -1.0f : 0.0f;
      c->player.agent_jump    = (keys & KEY_BIT_SPACE) != 0;
      c->player.agent_sprint  = (keys & KEY_BIT_SPRINT) != 0;
      /* Do NOT call player_update here — anti-cheat check happens after
       * the single physics step in server_tick to avoid double-ticking. */
  }

  static void handle_disconnect(Server* s, ServerClient* c)
  {
      disconnect_client(s, c);
  }

  /* ------------------------------------------------------------------ */
  /*  Server tick                                                        */
  /* ------------------------------------------------------------------ */

  static void server_tick(Server* s, float dt)
  {
      double now = net_time();

      /* 1. Drain inbound queue */
      NetMsg* msg;
      while ((msg = net_thread_pop_inbound(s->net)) != NULL) {
          if (msg->len < 1) { free(msg); continue; }
          uint8_t type = msg->data[0];
          ServerClient* c = find_client_by_addr(s, &msg->addr);

          if (type == PKT_CONNECT_REQUEST) {
              handle_connect_request(s, &msg->addr);
          } else if (c) {
              if      (type == PKT_INPUT)      handle_input(s, c, msg->data, msg->len);
              else if (type == PKT_DISCONNECT) handle_disconnect(s, c);
          }
          free(msg);
      }

      /* 2. Physics + anti-cheat for all clients.
         Call player_update once per server tick (dt = 1/20s). The accumulator
         inside player_update runs exactly 3 fixed physics steps (3 × 1/60 = 1/20),
         consuming all agent fields set by handle_input this tick.
         Anti-cheat checks net movement over the full tick. */
      for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
          ServerClient* c = &s->clients[i];
          if (!c->active) continue;

          vec3 pos_before;
          glm_vec3_copy(c->player.position, pos_before);

          player_update(&c->player, NULL, s->world, dt); /* dt = 1/20s → 3 physics ticks */

          /* Anti-cheat speed cap over the full server tick */
          vec3 delta;
          glm_vec3_sub(c->player.position, pos_before, delta);
          float dist = glm_vec3_norm(delta);
          float max_dist = PLAYER_SPRINT_SPEED * (float)dt * 1.5f;
          if (dist > max_dist)
              glm_vec3_copy(pos_before, c->player.position);

          /* Reset edge-triggered jump for next tick */
          c->player.agent_jump = false;
      }

      /* 3. Timeout detection */
      for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
          ServerClient* c = &s->clients[i];
          if (!c->active) continue;
          if (now - c->last_input_time > SERVER_TIMEOUT_SEC)
              disconnect_client(s, c);
      }

      /* 4. Retransmit reliable messages */
      for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
          ServerClient* c = &s->clients[i];
          if (!c->active) continue;
          reliable_tick(&c->reliable, s->net->fd, &c->addr);
      }

      /* 5. Broadcast world state */
      NetPlayerState players[SERVER_MAX_CLIENTS];
      uint8_t count = 0;
      for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
          ServerClient* c = &s->clients[i];
          if (!c->active) continue;
          players[count].player_id = c->player_id;
          players[count].x         = c->player.position[0];
          players[count].y         = c->player.position[1];
          players[count].z         = c->player.position[2];
          players[count].yaw       = c->yaw;
          players[count].pitch     = c->pitch;
          count++;
      }

      uint8_t buf[NET_MAX_PACKET];
      PacketHeader hdr = { .type = PKT_WORLD_STATE, .player_id = 0 };
      size_t len = net_write_world_state(buf, &hdr, players, count);
      for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
          if (!s->clients[i].active) continue;
          net_thread_push_outbound(s->net, buf, (int)len,
                                    &s->clients[i].addr);
      }

      /* 6. Block physics */
      block_physics_update(&s->bp, s->world, (vec3){0,0,0}, dt);
  }

  /* ------------------------------------------------------------------ */
  /*  Entry point                                                        */
  /* ------------------------------------------------------------------ */

  void server_run(uint16_t port, int max_clients, int seed)
  {
      int fd = net_socket_server(port);
      if (fd < 0) { fprintf(stderr, "[server] bind failed on port %d\n", port); return; }

      NetThread nt;
      if (!net_thread_start(&nt, fd)) {
          fprintf(stderr, "[server] failed to start net thread\n");
          net_socket_close(fd);
          return;
      }

      Server s = {0};
      s.net         = &nt;
      s.max_clients = max_clients > SERVER_MAX_CLIENTS
                    ? SERVER_MAX_CLIENTS : max_clients;
      s.seed        = seed;
      s.running     = true;
      s.world       = world_create_headless(seed, 16); /* smaller render dist for server */
      block_physics_init(&s.bp);

      printf("[server] listening on port %d (max %d clients)\n", port, s.max_clients);

      double last = net_time();
      const double tick_dt = 1.0 / SERVER_TICK_RATE;
      double accum = 0.0;

      while (s.running) {
          double now = net_time();
          accum += now - last;
          last   = now;
          if (accum > 0.2) accum = 0.2;

          while (accum >= tick_dt) {
              server_tick(&s, (float)tick_dt);
              accum -= tick_dt;
          }

          /* Sleep ~1ms to avoid busy-loop */
          struct timespec ts = { 0, 1000000 };
          nanosleep(&ts, NULL);
      }

      world_destroy(s.world);
      block_physics_destroy(&s.bp);
      net_thread_stop(&nt);
      net_socket_close(fd);
  }
  ```

- [ ] **Step 3: Add to CMake and build**
  ```bash
  distrobox run -- cmake --build build 2>&1 | tail -20
  ```

- [ ] **Step 4: Commit**
  ```bash
  git add src/server.h src/server.c CMakeLists.txt
  git commit -m "feat: add server with 20Hz tick, anti-cheat, timeout detection"
  ```

---

## Task 8: Client

**Files:**
- Create: `src/client.h`
- Create: `src/client.c`

- [ ] **Step 1: Write `src/client.h`**

  ```c
  #ifndef CLIENT_H
  #define CLIENT_H

  #include <stdint.h>
  #include <stdbool.h>
  #include <netinet/in.h>
  #include "net.h"
  #include "reliable.h"
  #include "player.h"

  typedef enum {
      CLIENT_DISCONNECTED,
      CLIENT_CONNECTING,   /* sent connect request, awaiting accept */
      CLIENT_CONNECTED,
  } ClientState;

  #define CLIENT_INPUT_HISTORY 128

  typedef struct {
      uint32_t tick;
      uint8_t  keys;
      float    yaw, pitch;
      float    px, py, pz;  /* predicted position after this input */
  } InputRecord;

  typedef struct NetThread NetThread;

  typedef struct {
      NetThread*   net;
      ClientState  state;
      uint8_t      local_player_id;  /* assigned by server */
      uint32_t     tick;             /* monotonic input tick counter */

      /* Server address */
      struct sockaddr_in server_addr;

      /* Input history for prediction/reconciliation */
      InputRecord  history[CLIENT_INPUT_HISTORY];
      int          history_head;  /* index of oldest entry */
      int          history_count;

      /* Reliable channel to server */
      ReliableChannel reliable;

      double connect_sent_time;  /* for connect timeout */
  } Client;

  /* addr: server IP/port.
   * net: already-started NetThread with a client socket. */
  void client_init(Client* c, NetThread* net,
                   const struct sockaddr_in* server_addr);
  void client_destroy(Client* c);

  /* Send PKT_CONNECT_REQUEST. Call once after client_init. */
  void client_connect(Client* c);

  /* Call once per frame with the local player's current input.
   * Sends PKT_INPUT to server. Records input in history for reconciliation.
   * player: used to read position for history recording. */
  void client_send_input(Client* c, uint8_t keys, float yaw, float pitch,
                          const Player* player);

  /* Process all inbound messages from the net thread.
   * out_state: set to CLIENT_CONNECTED on successful connect accept.
   * player: local player — position corrected on reconciliation.
   * Returns number of PKT_WORLD_STATE packets processed. */
  int client_poll(Client* c, Player* player);

  /* Remote player state parsed from a world state packet — caller fills these */
  typedef struct {
      uint8_t player_id;
      float   x, y, z;
      float   yaw, pitch;
      double  recv_time;
  } ClientPlayerSnapshot;

  /* Set callback to receive remote player snapshots.
   * Called during client_poll for each remote player in each world state. */
  typedef void (*ClientSnapshotCb)(const ClientPlayerSnapshot* snap, void* user);
  void client_set_snapshot_cb(Client* c, ClientSnapshotCb cb, void* user);

  void client_disconnect(Client* c);

  #endif /* CLIENT_H */
  ```

- [ ] **Step 2: Write `src/client.c`**

  ```c
  #include "client.h"
  #include "net_thread.h"
  #include "world.h"
  #include <stdio.h>
  #include <string.h>
  #include <math.h>

  static ClientSnapshotCb g_snap_cb   = NULL;
  static void*             g_snap_user = NULL;

  void client_set_snapshot_cb(Client* c, ClientSnapshotCb cb, void* user)
  {
      (void)c;
      g_snap_cb   = cb;
      g_snap_user = user;
  }

  void client_init(Client* c, NetThread* net,
                    const struct sockaddr_in* server_addr)
  {
      memset(c, 0, sizeof(*c));
      c->net         = net;
      c->state       = CLIENT_DISCONNECTED;
      c->server_addr = *server_addr;
      reliable_init(&c->reliable);
  }

  void client_destroy(Client* c) { (void)c; }

  void client_connect(Client* c)
  {
      uint8_t buf[HEADER_WIRE_SIZE];
      size_t off = 0;
      PacketHeader h = { .type = PKT_CONNECT_REQUEST, .player_id = 0 };
      reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
      net_write_header(buf, &off, &h);
      reliable_send(&c->reliable, c->net->fd, &c->server_addr,
                     buf, (uint16_t)off);
      c->state            = CLIENT_CONNECTING;
      c->connect_sent_time = net_time();
      printf("[client] connecting...\n");
  }

  void client_send_input(Client* c, uint8_t keys, float yaw, float pitch,
                          const Player* player)
  {
      if (c->state != CLIENT_CONNECTED) return;

      InputPacket p = {0};
      p.header.type      = PKT_INPUT;
      p.header.player_id = c->local_player_id;
      p.header.seq       = c->tick; /* use tick as seq for unreliable */
      reliable_fill_ack(&c->reliable, &p.header.ack, &p.header.ack_bits);
      p.tick  = c->tick++;
      p.keys  = keys;
      p.yaw   = yaw;
      p.pitch = pitch;

      uint8_t buf[32];
      size_t len = net_write_input(buf, &p);
      net_thread_push_outbound(c->net, buf, (int)len, &c->server_addr);

      /* Record in history */
      int slot = (c->history_head + c->history_count) % CLIENT_INPUT_HISTORY;
      if (c->history_count == CLIENT_INPUT_HISTORY)
          c->history_head = (c->history_head + 1) % CLIENT_INPUT_HISTORY;
      else
          c->history_count++;
      InputRecord* r = &c->history[slot];
      r->tick  = p.tick;
      r->keys  = keys;
      r->yaw   = yaw;
      r->pitch = pitch;
      r->px    = player->position[0];
      r->py    = player->position[1];
      r->pz    = player->position[2];
  }

  static void reconcile(Client* c, Player* player,
                          uint8_t pid, float sx, float sy, float sz)
  {
      if (pid != c->local_player_id) return;

      float dx = sx - player->position[0];
      float dy = sy - player->position[1];
      float dz = sz - player->position[2];
      float dist = sqrtf(dx*dx + dy*dy + dz*dz);

      if (dist < 0.5f) {
          /* Small divergence: lerp toward server over ~100ms
           * (done incrementally each frame) */
          float alpha = 0.1f;
          player->position[0] += dx * alpha;
          player->position[1] += dy * alpha;
          player->position[2] += dz * alpha;
          return;
      }

      /* Large divergence: snap to server and replay buffered inputs */
      player->position[0] = sx;
      player->position[1] = sy;
      player->position[2] = sz;
      /* Re-simulate buffered inputs — player->world is not available here,
       * so we skip physics replay for now (cosmetic: position snaps).
       * Full replay requires passing World* through; deferred to follow-up. */
      printf("[client] position reconciled (dist=%.2f)\n", dist);
  }

  int client_poll(Client* c, Player* player)
  {
      int state_packets = 0;
      NetMsg* msg;
      while ((msg = net_thread_pop_inbound(c->net)) != NULL) {
          if (msg->len < 1) { free(msg); continue; }
          uint8_t type = msg->data[0];

          if (type == PKT_CONNECT_ACCEPT && c->state == CLIENT_CONNECTING) {
              PacketHeader h;
              size_t off = 0;
              net_read_header(msg->data, &off, &h);
              reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
              c->local_player_id = h.player_id;
              c->state           = CLIENT_CONNECTED;
              printf("[client] connected as player %d\n", c->local_player_id);

          } else if (type == PKT_WORLD_STATE && c->state == CLIENT_CONNECTED) {
              size_t off = 0;
              PacketHeader hdr;
              net_read_header(msg->data, &off, &hdr);
              uint8_t count = net_read_u8(msg->data, &off);
              for (int i = 0; i < count; i++) {
                  uint8_t pid = net_read_u8(msg->data, &off);
                  float x     = net_read_float(msg->data, &off);
                  float y     = net_read_float(msg->data, &off);
                  float z     = net_read_float(msg->data, &off);
                  float yaw   = net_read_float(msg->data, &off);
                  float pitch = net_read_float(msg->data, &off);

                  if (pid == c->local_player_id) {
                      reconcile(c, player, pid, x, y, z);
                  } else if (g_snap_cb) {
                      ClientPlayerSnapshot snap = {
                          .player_id = pid, .x = x, .y = y, .z = z,
                          .yaw = yaw, .pitch = pitch,
                          .recv_time = msg->recv_time,
                      };
                      g_snap_cb(&snap, g_snap_user);
                  }
              }
              state_packets++;

          } else if (type == PKT_PLAYER_JOIN || type == PKT_PLAYER_LEAVE) {
              PacketHeader h; size_t off = 0;
              net_read_header(msg->data, &off, &h);
              reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
              if (type == PKT_PLAYER_JOIN)
                  printf("[client] player %d joined\n", h.player_id);
              else
                  printf("[client] player %d left\n", h.player_id);

          } else if (type == PKT_DISCONNECT) {
              printf("[client] disconnected by server\n");
              c->state = CLIENT_DISCONNECTED;
          }
          free(msg);
      }

      /* Connect timeout: resend after 2s */
      if (c->state == CLIENT_CONNECTING
          && net_time() - c->connect_sent_time > 2.0) {
          printf("[client] retrying connect...\n");
          client_connect(c);
      }

      return state_packets;
  }

  void client_disconnect(Client* c)
  {
      if (c->state == CLIENT_DISCONNECTED) return;
      uint8_t buf[HEADER_WIRE_SIZE];
      size_t off = 0;
      PacketHeader h = { .type = PKT_DISCONNECT,
                          .player_id = c->local_player_id };
      reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
      net_write_header(buf, &off, &h);
      reliable_send(&c->reliable, c->net->fd, &c->server_addr,
                     buf, (uint16_t)off);
      c->state = CLIENT_DISCONNECTED;
  }
  ```

- [ ] **Step 3: Add to CMake and build**
  ```bash
  distrobox run -- cmake --build build 2>&1 | tail -20
  ```

- [ ] **Step 4: Commit**
  ```bash
  git add src/client.h src/client.c CMakeLists.txt
  git commit -m "feat: add client with prediction history, reconciliation, connection state machine"
  ```

---

## Task 9: Remote Player Interpolation

**Files:**
- Create: `src/remote_player.h`
- Create: `src/remote_player.c`

- [ ] **Step 1: Write `src/remote_player.h`**

  ```c
  #ifndef REMOTE_PLAYER_H
  #define REMOTE_PLAYER_H

  #include <stdint.h>
  #include <stdbool.h>
  #include <cglm/cglm.h>

  #define REMOTE_PLAYER_MAX    32
  #define REMOTE_PLAYER_DELAY  0.1  /* seconds of interpolation lag */

  typedef struct {
      uint8_t  player_id;
      bool     active;
      vec3     positions[2];      /* [0]=older [1]=newer */
      float    yaws[2], pitches[2];
      double   snapshot_times[2];
      uint8_t  snapshot_count;    /* 0, 1, or 2 */
      double   render_time;
  } RemotePlayer;

  typedef struct {
      RemotePlayer players[REMOTE_PLAYER_MAX];
  } RemotePlayerSet;

  void remote_player_set_init(RemotePlayerSet* s);

  /* Push a new snapshot (position + orientation + receive timestamp) */
  void remote_player_push_snapshot(RemotePlayerSet* s,
                                    uint8_t player_id,
                                    float x, float y, float z,
                                    float yaw, float pitch,
                                    double recv_time);

  /* Remove a player (on disconnect) */
  void remote_player_remove(RemotePlayerSet* s, uint8_t player_id);

  /* Advance render_time by dt and fill out interpolated state.
   * out_pos/yaw/pitch: interpolated values for each active+ready player. */
  void remote_player_interpolate(RemotePlayer* p, float dt,
                                   vec3 out_pos, float* out_yaw, float* out_pitch);

  /* Iterate active players with snapshot_count == 2 */
  RemotePlayer* remote_player_set_get(RemotePlayerSet* s, uint8_t player_id);

  #endif /* REMOTE_PLAYER_H */
  ```

- [ ] **Step 2: Write `src/remote_player.c`**

  ```c
  #include "remote_player.h"
  #include <string.h>
  #include <math.h>

  void remote_player_set_init(RemotePlayerSet* s)
  {
      memset(s, 0, sizeof(*s));
  }

  void remote_player_push_snapshot(RemotePlayerSet* s, uint8_t pid,
                                    float x, float y, float z,
                                    float yaw, float pitch,
                                    double recv_time)
  {
      /* Find or allocate slot */
      RemotePlayer* p = NULL;
      RemotePlayer* free_slot = NULL;
      for (int i = 0; i < REMOTE_PLAYER_MAX; i++) {
          if (s->players[i].active && s->players[i].player_id == pid) {
              p = &s->players[i]; break;
          }
          if (!s->players[i].active && !free_slot)
              free_slot = &s->players[i];
      }
      if (!p) {
          if (!free_slot) return; /* no space */
          p = free_slot;
          memset(p, 0, sizeof(*p));
          p->active    = true;
          p->player_id = pid;
      }

      /* Shift ring buffer */
      if (p->snapshot_count > 0) {
          glm_vec3_copy(p->positions[1], p->positions[0]);
          p->yaws[0]           = p->yaws[1];
          p->pitches[0]        = p->pitches[1];
          p->snapshot_times[0] = p->snapshot_times[1];
      }
      p->positions[1][0] = x;
      p->positions[1][1] = y;
      p->positions[1][2] = z;
      p->yaws[1]           = yaw;
      p->pitches[1]        = pitch;
      p->snapshot_times[1] = recv_time;

      if (p->snapshot_count < 2) {
          p->snapshot_count++;
          if (p->snapshot_count == 1)
              p->render_time = recv_time - REMOTE_PLAYER_DELAY;
      }
  }

  void remote_player_remove(RemotePlayerSet* s, uint8_t pid)
  {
      for (int i = 0; i < REMOTE_PLAYER_MAX; i++) {
          if (s->players[i].active && s->players[i].player_id == pid) {
              s->players[i].active = false;
              return;
          }
      }
  }

  void remote_player_interpolate(RemotePlayer* p, float dt,
                                   vec3 out_pos, float* out_yaw, float* out_pitch)
  {
      p->render_time += dt;
      double t0 = p->snapshot_times[0];
      double t1 = p->snapshot_times[1];
      float  t  = (t1 > t0) ? (float)((p->render_time - t0) / (t1 - t0)) : 1.0f;
      if (t > 1.0f) t = 1.0f;
      if (t < 0.0f) t = 0.0f;

      out_pos[0] = p->positions[0][0] + t * (p->positions[1][0] - p->positions[0][0]);
      out_pos[1] = p->positions[0][1] + t * (p->positions[1][1] - p->positions[0][1]);
      out_pos[2] = p->positions[0][2] + t * (p->positions[1][2] - p->positions[0][2]);
      *out_yaw   = p->yaws[0]   + t * (p->yaws[1]   - p->yaws[0]);
      *out_pitch = p->pitches[0] + t * (p->pitches[1] - p->pitches[0]);
  }

  RemotePlayer* remote_player_set_get(RemotePlayerSet* s, uint8_t pid)
  {
      for (int i = 0; i < REMOTE_PLAYER_MAX; i++) {
          if (s->players[i].active && s->players[i].player_id == pid)
              return &s->players[i];
      }
      return NULL;
  }
  ```

- [ ] **Step 3: Build**
  ```bash
  distrobox run -- cmake --build build 2>&1 | tail -5
  ```

- [ ] **Step 4: Commit**
  ```bash
  git add src/remote_player.h src/remote_player.c CMakeLists.txt
  git commit -m "feat: add remote player snapshot ring buffer and 100ms interpolation"
  ```

---

## Task 10: Placeholder AABB Rendering

Draw a static cube mesh at each remote player's interpolated position using the existing chunk pipeline.

**Files:**
- Modify: `src/renderer.h`
- Modify: `src/renderer.c`
- Modify: `src/remote_player.h` (forward declare usage)

- [ ] **Step 1: Add declaration to `src/renderer.h`**

  After the existing `renderer_draw_frame` declaration:
  ```c
  /* Draw placeholder boxes for remote players.
   * positions: array of count world-space positions (feet).
   * Uses the existing block pipeline with a static unit-cube mesh. */
  void renderer_init_player_mesh(Renderer* r);
  void renderer_draw_remote_players(Renderer* r,
                                     const float (*positions)[3],
                                     uint32_t count,
                                     mat4 view, mat4 proj);
  ```

  Also add to the `Renderer` struct:
  ```c
  /* Remote player placeholder mesh */
  VkBuffer      player_vb;
  VmaAllocation player_vb_alloc;
  VkBuffer      player_ib;
  VmaAllocation player_ib_alloc;
  uint32_t      player_index_count;
  ```

- [ ] **Step 2: Implement in `src/renderer.c`**

  Generate a 0.6×1.8×0.6 box mesh (24 vertices, 36 indices) at init using the existing `Vertex` format. Upload via staging buffer (same pattern as existing mesh uploads). In `renderer_draw_remote_players`, bind the player VB/IB, then loop over positions issuing one `vkCmdDrawIndexed` per player with `chunk_offset` push constant set to the player's position.

  Consult the existing staging-buffer upload pattern in `renderer.c` (used for texture upload and chunk mesh upload) to keep this consistent.

- [ ] **Step 3: Call `renderer_init_player_mesh` in main.c after `renderer_init`** (done in Task 11).

- [ ] **Step 4: Build**
  ```bash
  distrobox run -- cmake --build build 2>&1 | tail -10
  ```

- [ ] **Step 5: Commit**
  ```bash
  git add src/renderer.h src/renderer.c
  git commit -m "feat: add placeholder AABB mesh for remote player rendering"
  ```

---

## Task 11: main.c Integration

Wire all the new modules into the existing game loop.

**Files:**
- Modify: `src/main.c`
- Modify: `CMakeLists.txt` (finalize all new sources)

- [ ] **Step 1: Add all new sources to CMakeLists.txt**

  In the `add_executable(minecraft ...)` block, add:
  ```cmake
  src/net.c
  src/reliable.c
  src/net_thread.c
  src/server.c
  src/client.c
  src/remote_player.c
  ```

- [ ] **Step 2: Rewrite mode detection in `main()`**

  Replace the existing `--agent` check block at the top of `main()`:
  ```c
  bool agent_mode  = false;
  bool server_mode = false;
  bool host_mode   = false;
  const char* connect_ip = "127.0.0.1";
  uint16_t    port       = NET_DEFAULT_PORT;

  for (int i = 1; i < argc; i++) {
      if      (strcmp(argv[i], "--agent")  == 0) agent_mode  = true;
      else if (strcmp(argv[i], "--server") == 0) server_mode = true;
      else if (strcmp(argv[i], "--host")   == 0) host_mode   = true;
      else                                        connect_ip  = argv[i];
  }
  ```

- [ ] **Step 3: Add `--server` early-exit path**

  Right after the mode detection block, before `glfwInit()`. Add `#include "server.h"` at the top of `main.c`:
  ```c
  if (server_mode) {
      server_run(port, SERVER_MAX_CLIENTS, WORLD_SEED);
      return 0;
  }
  ```

- [ ] **Step 4: Add listen-server thread for `--host`**

  Add a file-scope trampoline before `main()`:
  ```c
  typedef struct { uint16_t port; int max; int seed; } ServerArgs;
  static void* server_thread_func(void* arg)
  {
      ServerArgs* a = (ServerArgs*)arg;
      server_run(a->port, a->max, a->seed);
      free(a);
      return NULL;
  }
  ```

  Then inside `main()`, after `renderer_init` and before the loading loop:
  ```c
  PT_Thread server_thread;
  if (host_mode) {
      ServerArgs* sargs = malloc(sizeof(ServerArgs));
      sargs->port = port;
      sargs->max  = SERVER_MAX_CLIENTS;
      sargs->seed = WORLD_SEED;
      pt_thread_create(&server_thread, server_thread_func, sargs);
      /* Give server 200ms to bind before client tries to connect */
      struct timespec ts = { 0, 200000000 };
      nanosleep(&ts, NULL);
  }
  ```

- [ ] **Step 5: Start net thread and client after renderer init**

  ```c
  /* Networking (client mode and listen-server) */
  int net_fd = -1;
  NetThread net_thread;
  Client client;
  RemotePlayerSet remote_players;
  bool networking = !server_mode;

  if (networking) {
      net_fd = net_socket_client();
      net_thread_start(&net_thread, net_fd);

      struct sockaddr_in srv_addr = {0};
      srv_addr.sin_family      = AF_INET;
      srv_addr.sin_port        = htons(port);
      inet_pton(AF_INET, connect_ip, &srv_addr.sin_addr);

      client_init(&client, &net_thread, &srv_addr);
      remote_player_set_init(&remote_players);

      g_remote_players = &remote_players;
      client_set_snapshot_cb(&client, on_snapshot, NULL);
      client_connect(&client);
  }
  ```

  Add this file-scope callback before `main()`:
  ```c
  static RemotePlayerSet* g_remote_players = NULL;
  static void on_snapshot(const ClientPlayerSnapshot* s, void* user)
  {
      (void)user;
      if (g_remote_players)
          remote_player_push_snapshot(g_remote_players,
              s->player_id, s->x, s->y, s->z,
              s->yaw, s->pitch, s->recv_time);
  }
  ```

- [ ] **Step 6: Integrate into the game loop**

  In the main game loop, after `player_update` and before rendering:
  ```c
  /* Networking tick */
  if (networking) {
      /* Build keys bitmask. In agent mode, derive from player agent fields
       * (GLFW keys are not pressed by the agent). In normal mode, read GLFW. */
      uint8_t keys = 0;
      if (g_player.agent_mode) {
          if (g_player.agent_forward > 0)  keys |= KEY_BIT_W;
          if (g_player.agent_forward < 0)  keys |= KEY_BIT_S;
          if (g_player.agent_right   > 0)  keys |= KEY_BIT_D;
          if (g_player.agent_right   < 0)  keys |= KEY_BIT_A;
          if (g_player.agent_jump)         keys |= KEY_BIT_SPACE;
          if (g_player.agent_sprint)       keys |= KEY_BIT_SPRINT;
      } else {
          if (glfwGetKey(window, GLFW_KEY_W)            == GLFW_PRESS) keys |= KEY_BIT_W;
          if (glfwGetKey(window, GLFW_KEY_S)            == GLFW_PRESS) keys |= KEY_BIT_S;
          if (glfwGetKey(window, GLFW_KEY_A)            == GLFW_PRESS) keys |= KEY_BIT_A;
          if (glfwGetKey(window, GLFW_KEY_D)            == GLFW_PRESS) keys |= KEY_BIT_D;
          if (glfwGetKey(window, GLFW_KEY_SPACE)        == GLFW_PRESS) keys |= KEY_BIT_SPACE;
          if (glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS) keys |= KEY_BIT_SPRINT;
          if (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT)   == GLFW_PRESS) keys |= KEY_BIT_SHIFT;
      }

      client_send_input(&client, keys,
                         g_player.camera.yaw, g_player.camera.pitch,
                         &g_player);
      client_poll(&client, &g_player);
  }
  ```

  After the chunk draw call, add remote player rendering:
  ```c
  if (networking) {
      float positions[REMOTE_PLAYER_MAX][3];
      uint32_t rcount = 0;
      for (int i = 0; i < REMOTE_PLAYER_MAX; i++) {
          RemotePlayer* rp = &remote_players.players[i];
          if (!rp->active || rp->snapshot_count < 2) continue;
          vec3 pos; float yaw, pitch;
          remote_player_interpolate(rp, dt, pos, &yaw, &pitch);
          positions[rcount][0] = pos[0];
          positions[rcount][1] = pos[1];
          positions[rcount][2] = pos[2];
          rcount++;
      }
      if (rcount > 0)
          renderer_draw_remote_players(&renderer, positions, rcount,
                                        view, proj);
  }
  ```

- [ ] **Step 7: Cleanup on exit**

  Before `world_destroy`:
  ```c
  if (networking) {
      client_disconnect(&client);
      net_thread_stop(&net_thread);
      net_socket_close(net_fd);
  }
  if (host_mode)
      pt_thread_join(server_thread); /* pt_thread_join takes PT_Thread by value */
  ```

- [ ] **Step 8: Build**
  ```bash
  distrobox run -- cmake --build build 2>&1 | tail -20
  ```
  Fix any compile errors — most will be missing `#include` statements or function signature mismatches from the implementation tasks.

- [ ] **Step 9: Commit**
  ```bash
  git add src/main.c CMakeLists.txt
  git commit -m "feat: wire multiplayer into main.c (--server, --host, client)"
  ```

---

## Task 12: End-to-End Smoke Test

Verify all success criteria manually.

- [ ] **Step 1: Run a dedicated server**
  ```bash
  distrobox run -- ./build/minecraft --server &
  ```
  Expected output: `[server] listening on port 25565 (max 32 clients)`

- [ ] **Step 2: Run a listen-server client**
  ```bash
  distrobox run -- ./build/minecraft --host
  ```
  Expected: server thread starts, client window opens, `[client] connected as player N`

- [ ] **Step 3: Run a second client connecting to the listen server**

  In another terminal:
  ```bash
  distrobox run -- ./build/minecraft 127.0.0.1
  ```
  Expected: `[client] connected as player M`, placeholder box visible in first client's view near spawn.

- [ ] **Step 4: Verify disconnect and timeout**

  **Clean disconnect:** press Ctrl-C on the second client. The first client should immediately log `[client] player M left` (PKT_DISCONNECT was sent).

  **Timeout path:** relaunch the second client, then kill it with `kill -9 <pid>` (no cleanup). After 10 seconds the server log should show the player timed out and the first client should log `[client] player M left`.

- [ ] **Step 5: Run the unit test suite**
  ```bash
  distrobox run -- ctest --test-dir build -V
  ```
  Expected: all tests pass (block_physics, agent_json, net).

- [ ] **Step 6: Final commit**
  ```bash
  git add -A
  git commit -m "feat: multiplayer backbone complete — server, client, prediction, interpolation"
  ```
