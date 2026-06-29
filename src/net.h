#ifndef NET_H
#define NET_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Packet types                                                       */
/* ------------------------------------------------------------------ */
typedef enum {
    PKT_CONNECT_REQUEST = 0,
    PKT_CONNECT_ACCEPT  = 1,
    PKT_DISCONNECT      = 2,
    PKT_POSITION        = 3,  /* client → server: player position */
    PKT_WORLD_STATE     = 4,
    PKT_PLAYER_JOIN     = 5,
    PKT_PLAYER_LEAVE    = 6,
    PKT_BLOCK_CHANGE    = 7,  /* server → all:    block edit broadcast      */
    PKT_BLOCK_BREAK     = 8,  /* client → server: request to break a block  */
    PKT_BLOCK_PLACE     = 9,  /* client → server: request to place a block  */
    PKT_INVENTORY       = 10, /* server → one:    full inventory snapshot   */
    PKT_MOB_STATE       = 11, /* server → all:  mob snapshot broadcast        */
    PKT_MOB_ATTACK      = 12, /* client → server: melee a mob by id           */
    PKT_PLAYER_HEALTH   = 13, /* server → one:  authoritative health + death  */
    PKT_CRAFT           = 14, /* client → server: craft a recipe by index     */
    PKT_EQUIP           = 15, /* client → server: equip the armour in a slot   */
    PKT_ARMOR           = 16, /* server → one:    equipped armour + points     */
    PKT_CHUNK_DATA      = 17, /* server → one:  RLE column (reliable+fragmented)*/
    PKT_CHUNK_UNLOAD    = 18, /* server → one:  drop a now-distant chunk        */
    PKT_KEEPALIVE       = 19, /* both ways: header-only liveness ping/pong; keeps
                               * NAT pinholes open and lets each peer detect a
                               * dead remote (no traffic for N seconds). Sent
                               * UNRELIABLE on a periodic timer (protocol v9).  */
    PKT_EAT             = 20, /* client → server: eat the food item in a hotbar
                               * slot. Server validates it is a food item and the
                               * player isn't full, then consumes 1 + restores
                               * hunger and resyncs inventory + health (v10).    */
    PKT_CONTAINER_OPEN  = 21, /* client → server: open the container block-entity
                               * (furnace/chest) at {x,y,z}. Server validates reach,
                               * marks this client a viewer, replies CONTAINER_STATE
                               * (v11).                                          */
    PKT_CONTAINER_STATE = 22, /* server → one:  full container snapshot — {pos,
                               * type} then a CHEST's 27 (item,count) slots, or a
                               * FURNACE's input/fuel/output (item,count) + burn +
                               * cook progress (v11).                            */
    PKT_CONTAINER_ACTION= 23, /* client → server: move items between a container
                               * slot and the player inventory {pos, slot, dir,
                               * count}. Server applies via the container transfer
                               * helpers + resyncs viewers/inventory (v11).      */
    PKT_CONTAINER_CLOSE = 24, /* client → server: stop viewing the container at
                               * {x,y,z} (un-marks this client as a viewer) (v11).*/
    PKT_KNOCKBACK       = 25, /* server → one: a melee/contact hit shoves the
                               * local player. Carries a velocity impulse
                               * {dx,dy,dz} (blocks/s) the client applies as a
                               * short decaying positional nudge — the local
                               * player owns its position, so server knockback
                               * has to be sent rather than ride snapshots (v13).*/
    PKT_WEATHER         = 26, /* server → all: authoritative weather state —
                               * {u8 kind, f32 time_left}. The server owns the
                               * WeatherState (rng stays server-side) and ticks
                               * it; this is broadcast on every weather KIND
                               * transition, sent once to a client shortly after
                               * it joins (late-joiner sync), and re-broadcast at
                               * a low frequency for resync. Clients store the
                               * {kind,time_left} for the rain-render step (v14). */
} PacketType;

#define NET_MAX_PLAYERS  255
#define NET_DEFAULT_PORT 25565
#define NET_MAX_PACKET   1400  /* safe below typical MTU */

/* Wire protocol version. Bump whenever any on-the-wire packet format changes
 * so mismatched client/server builds are rejected at connect time instead of
 * silently misparsing each other's bytes. A client that sends a different
 * version (or none — legacy header-only connect, read as 0) is refused with
 * NET_DISCONNECT_VERSION_MISMATCH. */
#define NET_PROTOCOL_VERSION 14   /* 14: added PKT_WEATHER (server→all authoritative weather sync) */

typedef enum {
    NET_DISCONNECT_NORMAL           = 0,
    NET_DISCONNECT_VERSION_MISMATCH = 1,
    NET_DISCONNECT_SERVER_FULL      = 2,
} NetDisconnectReason;

/* ------------------------------------------------------------------ */
/*  Bounds-checked cursor codec (untrusted/WAN input)                  */
/*                                                                     */
/*  The legacy net_read_* helpers below over-read a too-short buffer.  */
/*  For parsing packets that arrived off the network (which may be     */
/*  truncated, malformed or hostile) use a NetReader instead: every    */
/*  typed read checks the remaining length BEFORE touching the buffer  */
/*  and, on underflow, latches ok=false and returns 0 without ever     */
/*  reading past the end. After parsing, the caller checks reader_ok() */
/*  (or it never set ok=false) and DROPS the packet if parsing failed. */
/*                                                                     */
/*  The matching NetWriter caps every write against capacity so a      */
/*  serializer can never overflow its destination buffer.             */
/* ------------------------------------------------------------------ */
typedef struct {
    const uint8_t* base;
    size_t         len;
    size_t         pos;
    int            ok;     /* 1 while no underflow has occurred */
} NetReader;

static inline NetReader net_reader_init(const void* buf, size_t len)
{
    NetReader r;
    r.base = (const uint8_t*)buf;
    r.len  = len;
    r.pos  = 0;
    r.ok   = 1;
    return r;
}

static inline int    net_reader_ok(const NetReader* r)        { return r->ok; }
static inline size_t net_reader_remaining(const NetReader* r) { return r->ok ? (r->len - r->pos) : 0; }

/* True iff at least `n` more bytes are available; latches ok=false otherwise. */
static inline int net_reader_need(NetReader* r, size_t n)
{
    if (!r->ok) return 0;
    if (r->pos + n > r->len) { r->ok = 0; return 0; }
    return 1;
}

static inline uint8_t net_reader_u8(NetReader* r)
{
    if (!net_reader_need(r, 1)) return 0;
    return r->base[r->pos++];
}

static inline uint16_t net_reader_u16(NetReader* r)
{
    if (!net_reader_need(r, 2)) return 0;
    uint16_t v = (uint16_t)r->base[r->pos]
               | ((uint16_t)r->base[r->pos + 1] << 8);
    r->pos += 2;
    return v;
}

static inline uint32_t net_reader_u32(NetReader* r)
{
    if (!net_reader_need(r, 4)) return 0;
    uint32_t v = (uint32_t)r->base[r->pos]
               | ((uint32_t)r->base[r->pos + 1] << 8)
               | ((uint32_t)r->base[r->pos + 2] << 16)
               | ((uint32_t)r->base[r->pos + 3] << 24);
    r->pos += 4;
    return v;
}

static inline int32_t net_reader_i32(NetReader* r)
{
    return (int32_t)net_reader_u32(r);
}

static inline int16_t net_reader_i16(NetReader* r)
{
    return (int16_t)net_reader_u16(r);
}

static inline float net_reader_f32(NetReader* r)
{
    uint32_t bits = net_reader_u32(r);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}

/* Copy `n` bytes into `dst` (which must hold n). On underflow leaves dst
 * untouched, latches ok=false, returns 0. */
static inline int net_reader_bytes(NetReader* r, void* dst, size_t n)
{
    if (!net_reader_need(r, n)) return 0;
    memcpy(dst, r->base + r->pos, n);
    r->pos += n;
    return 1;
}

/* Skip `n` bytes (bounds-checked). */
static inline int net_reader_skip(NetReader* r, size_t n)
{
    if (!net_reader_need(r, n)) return 0;
    r->pos += n;
    return 1;
}

typedef struct {
    uint8_t* base;
    size_t   cap;
    size_t   pos;
    int      ok;     /* 1 while no overflow has occurred */
} NetWriter;

static inline NetWriter net_writer_init(void* buf, size_t cap)
{
    NetWriter w;
    w.base = (uint8_t*)buf;
    w.cap  = cap;
    w.pos  = 0;
    w.ok   = 1;
    return w;
}

static inline int    net_writer_ok(const NetWriter* w)  { return w->ok; }
static inline size_t net_writer_len(const NetWriter* w) { return w->ok ? w->pos : 0; }

static inline int net_writer_room(NetWriter* w, size_t n)
{
    if (!w->ok) return 0;
    if (w->pos + n > w->cap) { w->ok = 0; return 0; }
    return 1;
}

static inline void net_writer_u8(NetWriter* w, uint8_t v)
{
    if (!net_writer_room(w, 1)) return;
    w->base[w->pos++] = v;
}

static inline void net_writer_u16(NetWriter* w, uint16_t v)
{
    if (!net_writer_room(w, 2)) return;
    w->base[w->pos++] = (uint8_t)(v & 0xFF);
    w->base[w->pos++] = (uint8_t)(v >> 8);
}

static inline void net_writer_u32(NetWriter* w, uint32_t v)
{
    if (!net_writer_room(w, 4)) return;
    w->base[w->pos++] = (uint8_t)(v & 0xFF);
    w->base[w->pos++] = (uint8_t)((v >> 8) & 0xFF);
    w->base[w->pos++] = (uint8_t)((v >> 16) & 0xFF);
    w->base[w->pos++] = (uint8_t)(v >> 24);
}

static inline void net_writer_i32(NetWriter* w, int32_t v) { net_writer_u32(w, (uint32_t)v); }

static inline void net_writer_f32(NetWriter* w, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4);
    net_writer_u32(w, bits);
}

static inline void net_writer_bytes(NetWriter* w, const void* src, size_t n)
{
    if (!net_writer_room(w, n)) return;
    memcpy(w->base + w->pos, src, n);
    w->pos += n;
}

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

static inline void net_write_i32(uint8_t* buf, size_t* off, int32_t v)
{
    net_write_u32(buf, off, (uint32_t)v);
}

static inline int32_t net_read_i32(const uint8_t* buf, size_t* off)
{
    return (int32_t)net_read_u32(buf, off);
}

/* ------------------------------------------------------------------ */
/*  Broadcast-stream sequence numbers (dedup of unreliable state)      */
/*                                                                     */
/*  PKT_WORLD_STATE and PKT_MOB_STATE are sent unreliable, so a late   */
/*  or reordered datagram can deliver a stale snapshot after a newer   */
/*  one (rubber-banding). Each broadcast carries a per-stream u32 seq  */
/*  that the server increments once per emitted packet; the client     */
/*  remembers the highest applied seq per stream and drops anything    */
/*  not newer.                                                         */
/*                                                                     */
/*  seq_is_newer(a, b) is a PURE, wrap-aware "is a strictly newer than */
/*  b" test (RFC 1982 serial-number arithmetic over u32): true iff the */
/*  forward distance b -> a is in (0, 2^31). Equal seqs are NOT newer. */
/* ------------------------------------------------------------------ */
static inline int seq_is_newer(uint32_t a, uint32_t b)
{
    return (a != b) && ((uint32_t)(a - b) < 0x80000000u);
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

/* Bounds-checked header read. On a buffer shorter than HEADER_WIRE_SIZE the
 * reader latches ok=false; the caller checks net_reader_ok() and drops. */
static inline void net_reader_header(NetReader* r, PacketHeader* h)
{
    h->type      = net_reader_u8(r);
    h->player_id = net_reader_u8(r);
    h->seq       = net_reader_u16(r);
    h->ack       = net_reader_u16(r);
    h->ack_bits  = net_reader_u16(r);
}

/* ------------------------------------------------------------------ */
/*  PositionPacket — 32 wire bytes                                     */
/*  Client sends its authoritative position every frame.              */
/* ------------------------------------------------------------------ */
typedef struct {
    PacketHeader header;
    uint32_t     tick;
    float        x, y, z;
    float        yaw, pitch;
} PositionPacket;

static inline size_t net_write_position(uint8_t* buf, const PositionPacket* p)
{
    size_t off = 0;
    net_write_header(buf, &off, &p->header);
    net_write_u32(buf, &off, p->tick);
    net_write_float(buf, &off, p->x);
    net_write_float(buf, &off, p->y);
    net_write_float(buf, &off, p->z);
    net_write_float(buf, &off, p->yaw);
    net_write_float(buf, &off, p->pitch);
    return off; /* 32 */
}

static inline void net_read_position(const uint8_t* buf, PositionPacket* p)
{
    size_t off = 0;
    net_read_header(buf, &off, &p->header);
    p->tick  = net_read_u32(buf, &off);
    p->x     = net_read_float(buf, &off);
    p->y     = net_read_float(buf, &off);
    p->z     = net_read_float(buf, &off);
    p->yaw   = net_read_float(buf, &off);
    p->pitch = net_read_float(buf, &off);
}

/* Bounds-checked parse: returns 1 only if the full 32-byte packet was present.
 * On a short/truncated buffer returns 0 and never reads out of bounds. */
static inline int net_parse_position(const uint8_t* buf, size_t len,
                                     PositionPacket* p)
{
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, &p->header);
    p->tick  = net_reader_u32(&r);
    p->x     = net_reader_f32(&r);
    p->y     = net_reader_f32(&r);
    p->z     = net_reader_f32(&r);
    p->yaw   = net_reader_f32(&r);
    p->pitch = net_reader_f32(&r);
    return net_reader_ok(&r);
}

/* ------------------------------------------------------------------ */
/*  WorldStatePacket — 9 + 21*N + 4 wire bytes                        */
/* ------------------------------------------------------------------ */
#define WORLD_STATE_PLAYER_SIZE 21  /* 1 + 4 + 4 + 4 + 4 + 4 */

typedef struct {
    uint8_t  player_id;
    float    x, y, z;
    float    yaw, pitch;
} NetPlayerState;

/* Wire format: [header 8][bcast_seq u32][count 1][players 21*N][world_ticks u32].
 * bcast_seq is the per-stream broadcast sequence the client uses to drop stale
 * reordered datagrams (see seq_is_newer); it sits between the header and count
 * (protocol v3). world_ticks is the server's day/night clock, appended after
 * the player array. */
static inline size_t net_write_world_state(uint8_t* buf,
                                            const PacketHeader* hdr,
                                            uint32_t bcast_seq,
                                            const NetPlayerState* players,
                                            uint8_t count,
                                            uint32_t world_ticks)
{
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u32(buf, &off, bcast_seq);
    net_write_u8(buf, &off, count);
    for (int i = 0; i < count; i++) {
        net_write_u8(buf, &off, players[i].player_id);
        net_write_float(buf, &off, players[i].x);
        net_write_float(buf, &off, players[i].y);
        net_write_float(buf, &off, players[i].z);
        net_write_float(buf, &off, players[i].yaw);
        net_write_float(buf, &off, players[i].pitch);
    }
    net_write_u32(buf, &off, world_ticks);
    return off;
}

/* ------------------------------------------------------------------ */
/*  Block edit packets                                                 */
/*    BlockBreakPacket  — client → server, 21 wire bytes (8 + 12 + 1) */
/*    BlockPlacePacket  — client → server, 22 wire bytes (8 + 14)     */
/*    BlockChangePacket — server → all,    21 wire bytes (8 + 13)     */
/* ------------------------------------------------------------------ */
typedef struct {
    PacketHeader header;
    int32_t      x, y, z;
    uint8_t      block;   /* claimed block ID being broken — server validates
                            * against its own world before crediting inventory */
    uint8_t      slot;    /* hotbar slot held while mining (protocol v4); the
                            * server applies tool durability wear to it */
} BlockBreakPacket;

typedef struct {
    PacketHeader header;
    int32_t      x, y, z;
    uint8_t      face;
    uint8_t      slot;
} BlockPlacePacket;

typedef struct {
    PacketHeader header;
    int32_t      x, y, z;
    uint8_t      block;
} BlockChangePacket;

/* Must match INVENTORY_SLOTS in src/inventory.h. We deliberately don't
 * #include inventory.h here so net.h stays at the bottom of the dep
 * tree — keep this constant in sync manually. */
#define INVENTORY_NET_SLOTS 6

/* PKT_INVENTORY now carries a full item snapshot (protocol v4): per slot an
 * item id (u16: block id in the low range, tool id above BLOCK_COUNT), a stack
 * count (u8), and remaining tool durability (u16, 0 for blocks). */
typedef struct {
    PacketHeader header;
    uint8_t      slot_count;
    struct { uint16_t item; uint8_t count; uint16_t durability; }
                 slots[INVENTORY_NET_SLOTS];
} InventoryPacket;

static inline size_t net_write_block_break(uint8_t* buf,
                                            const BlockBreakPacket* p)
{
    size_t off = 0;
    net_write_header(buf, &off, &p->header);
    net_write_i32(buf, &off, p->x);
    net_write_i32(buf, &off, p->y);
    net_write_i32(buf, &off, p->z);
    net_write_u8(buf, &off, p->block);
    net_write_u8(buf, &off, p->slot);
    return off;
}

static inline void net_read_block_break(const uint8_t* buf,
                                         BlockBreakPacket* p)
{
    size_t off = 0;
    net_read_header(buf, &off, &p->header);
    p->x     = net_read_i32(buf, &off);
    p->y     = net_read_i32(buf, &off);
    p->z     = net_read_i32(buf, &off);
    p->block = net_read_u8(buf, &off);
    p->slot  = net_read_u8(buf, &off);
}

/* Bounds-checked parse (22 wire bytes). */
static inline int net_parse_block_break(const uint8_t* buf, size_t len,
                                        BlockBreakPacket* p)
{
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, &p->header);
    p->x     = net_reader_i32(&r);
    p->y     = net_reader_i32(&r);
    p->z     = net_reader_i32(&r);
    p->block = net_reader_u8(&r);
    p->slot  = net_reader_u8(&r);
    return net_reader_ok(&r);
}

static inline size_t net_write_block_place(uint8_t* buf,
                                            const BlockPlacePacket* p)
{
    size_t off = 0;
    net_write_header(buf, &off, &p->header);
    net_write_i32(buf, &off, p->x);
    net_write_i32(buf, &off, p->y);
    net_write_i32(buf, &off, p->z);
    net_write_u8(buf, &off, p->face);
    net_write_u8(buf, &off, p->slot);
    return off;
}

static inline void net_read_block_place(const uint8_t* buf,
                                         BlockPlacePacket* p)
{
    size_t off = 0;
    net_read_header(buf, &off, &p->header);
    p->x    = net_read_i32(buf, &off);
    p->y    = net_read_i32(buf, &off);
    p->z    = net_read_i32(buf, &off);
    p->face = net_read_u8(buf, &off);
    p->slot = net_read_u8(buf, &off);
}

/* Bounds-checked parse (22 wire bytes). */
static inline int net_parse_block_place(const uint8_t* buf, size_t len,
                                        BlockPlacePacket* p)
{
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, &p->header);
    p->x    = net_reader_i32(&r);
    p->y    = net_reader_i32(&r);
    p->z    = net_reader_i32(&r);
    p->face = net_reader_u8(&r);
    p->slot = net_reader_u8(&r);
    return net_reader_ok(&r);
}

static inline size_t net_write_block_change(uint8_t* buf,
                                             const BlockChangePacket* p)
{
    size_t off = 0;
    net_write_header(buf, &off, &p->header);
    net_write_i32(buf, &off, p->x);
    net_write_i32(buf, &off, p->y);
    net_write_i32(buf, &off, p->z);
    net_write_u8(buf, &off, p->block);
    return off;
}

static inline void net_read_block_change(const uint8_t* buf,
                                          BlockChangePacket* p)
{
    size_t off = 0;
    net_read_header(buf, &off, &p->header);
    p->x     = net_read_i32(buf, &off);
    p->y     = net_read_i32(buf, &off);
    p->z     = net_read_i32(buf, &off);
    p->block = net_read_u8(buf, &off);
}

/* Bounds-checked parse (21 wire bytes). */
static inline int net_parse_block_change(const uint8_t* buf, size_t len,
                                         BlockChangePacket* p)
{
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, &p->header);
    p->x     = net_reader_i32(&r);
    p->y     = net_reader_i32(&r);
    p->z     = net_reader_i32(&r);
    p->block = net_reader_u8(&r);
    return net_reader_ok(&r);
}

/* Wire body per slot: [item u16][count u8][durability u16] = 5 bytes. */
#define INVENTORY_NET_SLOT_SIZE 5

static inline size_t net_write_inventory(uint8_t* buf,
                                          const InventoryPacket* p)
{
    size_t off = 0;
    net_write_header(buf, &off, &p->header);
    net_write_u8(buf, &off, p->slot_count);
    for (uint8_t i = 0; i < p->slot_count; i++) {
        net_write_u16(buf, &off, p->slots[i].item);
        net_write_u8 (buf, &off, p->slots[i].count);
        net_write_u16(buf, &off, p->slots[i].durability);
    }
    return off;
}

static inline void net_read_inventory(const uint8_t* buf, InventoryPacket* p)
{
    size_t off = 0;
    net_read_header(buf, &off, &p->header);
    p->slot_count = net_read_u8(buf, &off);
    if (p->slot_count > INVENTORY_NET_SLOTS) p->slot_count = INVENTORY_NET_SLOTS;
    for (uint8_t i = 0; i < p->slot_count; i++) {
        p->slots[i].item       = net_read_u16(buf, &off);
        p->slots[i].count      = net_read_u8 (buf, &off);
        p->slots[i].durability = net_read_u16(buf, &off);
    }
}

/* Bounds-checked parse. The declared slot_count is clamped to the array bound
 * and every per-slot read is length-checked; a truncated body sets ok=false. */
static inline int net_parse_inventory(const uint8_t* buf, size_t len,
                                      InventoryPacket* p)
{
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, &p->header);
    uint8_t declared = net_reader_u8(&r);
    if (!net_reader_ok(&r)) return 0;
    p->slot_count = declared > INVENTORY_NET_SLOTS ? INVENTORY_NET_SLOTS
                                                   : declared;
    for (uint8_t i = 0; i < p->slot_count; i++) {
        p->slots[i].item       = net_reader_u16(&r);
        p->slots[i].count      = net_reader_u8 (&r);
        p->slots[i].durability = net_reader_u16(&r);
    }
    return net_reader_ok(&r);
}

/* ------------------------------------------------------------------ */
/*  PKT_CRAFT — client → server, 10 wire bytes (8 header + u16 recipe)  */
/*                                                                     */
/*  The client asks the server to craft the recipe at `recipe_index`    */
/*  (its position in the shared crafting table, see crafting.h). The     */
/*  server validates the player holds the inputs, consumes them, adds    */
/*  the output, and replies with a fresh PKT_INVENTORY. Sent reliably    */
/*  so a dropped request doesn't silently lose a craft.                 */
/* ------------------------------------------------------------------ */
static inline size_t net_write_craft(uint8_t* buf, const PacketHeader* hdr,
                                     uint16_t recipe_index) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u16(buf, &off, recipe_index);
    return off;
}

static inline void net_read_craft(const uint8_t* buf, PacketHeader* hdr,
                                  uint16_t* recipe_index) {
    size_t off = 0;
    net_read_header(buf, &off, hdr);
    *recipe_index = net_read_u16(buf, &off);
}

/* Bounds-checked parse (10 wire bytes). */
static inline int net_parse_craft(const uint8_t* buf, size_t len,
                                  PacketHeader* hdr, uint16_t* recipe_index) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, hdr);
    *recipe_index = net_reader_u16(&r);
    return net_reader_ok(&r);
}

/* ------------------------------------------------------------------ */
/*  Armour packets (protocol v6)                                       */
/*    PKT_EQUIP — client → server, 9 wire bytes (8 header + u8 slot).   */
/*       Asks the server to equip the armour item held in inventory     */
/*       slot `slot` into its matching armour slot (server validates).  */
/*    PKT_ARMOR — server → one,  8 + 1 + ARMOR_NET_SLOTS*2 + 1 bytes.   */
/*       Carries the four equipped armour item ids (u16 each, BLOCK_AIR  */
/*       when empty) plus the total armour points (u8) for the HUD bar.  */
/* ------------------------------------------------------------------ */
#define ARMOR_NET_SLOTS 4   /* must match ARMOR_SLOT_COUNT in item.h */

static inline size_t net_write_equip(uint8_t* buf, const PacketHeader* hdr,
                                     uint8_t slot) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u8(buf, &off, slot);
    return off;
}

static inline void net_read_equip(const uint8_t* buf, PacketHeader* hdr,
                                  uint8_t* slot) {
    size_t off = 0;
    net_read_header(buf, &off, hdr);
    *slot = net_read_u8(buf, &off);
}

/* Bounds-checked parse (9 wire bytes). */
static inline int net_parse_equip(const uint8_t* buf, size_t len,
                                  PacketHeader* hdr, uint8_t* slot) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, hdr);
    *slot = net_reader_u8(&r);
    return net_reader_ok(&r);
}

/* ------------------------------------------------------------------ */
/*  PKT_EAT — client → server, 9 wire bytes (8 header + u8 slot, v10).  */
/*                                                                     */
/*  Asks the server to eat the food item held in inventory slot `slot`. */
/*  The server validates the slot holds a food item and the player is   */
/*  not already at full hunger; on success it decrements the stack by 1, */
/*  restores hunger, and replies with fresh PKT_INVENTORY +             */
/*  PKT_PLAYER_HEALTH snapshots. Sent reliably so a dropped request      */
/*  doesn't silently lose the eat. Same wire shape as PKT_EQUIP.        */
/* ------------------------------------------------------------------ */
static inline size_t net_write_eat(uint8_t* buf, const PacketHeader* hdr,
                                   uint8_t slot) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u8(buf, &off, slot);
    return off;
}

static inline void net_read_eat(const uint8_t* buf, PacketHeader* hdr,
                                uint8_t* slot) {
    size_t off = 0;
    net_read_header(buf, &off, hdr);
    *slot = net_read_u8(buf, &off);
}

/* Bounds-checked parse (9 wire bytes). */
static inline int net_parse_eat(const uint8_t* buf, size_t len,
                                PacketHeader* hdr, uint8_t* slot) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, hdr);
    *slot = net_reader_u8(&r);
    return net_reader_ok(&r);
}

static inline size_t net_write_armor(uint8_t* buf, const PacketHeader* hdr,
                                     const uint16_t equipped[ARMOR_NET_SLOTS],
                                     uint8_t total_points) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u8(buf, &off, ARMOR_NET_SLOTS);
    for (int i = 0; i < ARMOR_NET_SLOTS; i++)
        net_write_u16(buf, &off, equipped[i]);
    net_write_u8(buf, &off, total_points);
    return off;
}

/* Bounds-checked armour parse. Tolerant by design (legacy/short packets read
 * missing fields as zero) but never over-reads: each field is taken only if the
 * cursor still has room. Always returns 1 (the packet is best-effort), with
 * absent fields defaulted. */
static inline void net_read_armor(const uint8_t* buf, size_t len,
                                  PacketHeader* hdr,
                                  uint16_t equipped[ARMOR_NET_SLOTS],
                                  uint8_t* total_points) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, hdr);
    uint8_t n = (net_reader_remaining(&r) >= 1) ? net_reader_u8(&r) : 0;
    if (n > ARMOR_NET_SLOTS) n = ARMOR_NET_SLOTS;
    for (int i = 0; i < ARMOR_NET_SLOTS; i++) equipped[i] = 0;
    for (uint8_t i = 0; i < n; i++) {
        if (net_reader_remaining(&r) < 2) break;
        equipped[i] = net_reader_u16(&r);
    }
    *total_points = (net_reader_remaining(&r) >= 1) ? net_reader_u8(&r) : 0;
}

/* ------------------------------------------------------------------ */
/*  Chunk streaming packets (protocol v7)                              */
/*    PKT_CHUNK_DATA   — server → one. The chunkwire body {cx i32,     */
/*       cz i32, rle_len u32, rle...} (see src/chunkwire.h) is split   */
/*       across one or more fragments, each an ORDINARY reliable packet */
/*       with the {msg_id,index,total} subheader below (so each frag is */
/*       independently acked + retransmitted). The client reassembles  */
/*       per msg_id at CHUNK_DATA_FRAG_BYTES stride, then RLE-decodes.  */
/*    PKT_CHUNK_UNLOAD — server → one, 16 wire bytes (8 header + 2×i32)*/
/* ------------------------------------------------------------------ */
/* PKT_CHUNK_DATA fragment wire layout (protocol v8):
 *   [header 8][msg_id u16][index u16][total u16][frag bytes...]
 * Each fragment is an ORDINARY reliable packet (so it is acked + retransmitted
 * by the existing reliable layer — we do NOT use the unacked magic-byte
 * fragmentation path). The client reassembles fragments of one msg_id in order
 * into the chunkwire body {cx,cz,rle_len,rle...}, then RLE-decodes the column.
 * msg_id distinguishes concurrent/successive columns; index/total drive
 * reassembly. The data capacity per fragment is the reliable payload minus this
 * 14-byte prefix. */
#define CHUNK_DATA_FRAG_PREFIX (HEADER_WIRE_SIZE + 6)   /* 8 + msg_id+index+total */
/* Must equal RELIABLE_MAX_PAYLOAD (reliable.h, 256). net.h sits below
 * reliable.h in the dep tree, so we restate the value here and a _Static_assert
 * in server.c (which includes both) guards the two from drifting apart. */
#define CHUNK_DATA_RELIABLE_MAX 1200
#define CHUNK_DATA_FRAG_BYTES  (CHUNK_DATA_RELIABLE_MAX - CHUNK_DATA_FRAG_PREFIX)
/* Max fragments per streamed column. Must be >= the server's stream cap
 * (SERVER_STREAM_MAX_FRAGS) and stay <= the reliable window so a whole column's
 * fragments can be in flight at once. A real RLE terrain column is ~6-8 KB ≈
 * 6-7 fragments at this size; this cap covers ~56 KB columns. */
#define CHUNK_DATA_FRAG_MAX    48

static inline size_t net_write_chunk_data_frag(uint8_t* buf,
                                               const PacketHeader* hdr,
                                               uint16_t msg_id, uint16_t index,
                                               uint16_t total,
                                               const uint8_t* frag, size_t flen)
{
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u16(buf, &off, msg_id);
    net_write_u16(buf, &off, index);
    net_write_u16(buf, &off, total);
    for (size_t i = 0; i < flen; i++) buf[off++] = frag[i];
    return off;
}

static inline void net_read_chunk_data_frag_hdr(const uint8_t* buf,
                                                uint16_t* msg_id,
                                                uint16_t* index, uint16_t* total)
{
    size_t off = HEADER_WIRE_SIZE;
    *msg_id = net_read_u16(buf, &off);
    *index  = net_read_u16(buf, &off);
    *total  = net_read_u16(buf, &off);
}

/* Bounds-checked parse of the fragment subheader (header + 3×u16 = 14 bytes).
 * Returns 1 only if the full prefix was present. */
static inline int net_parse_chunk_data_frag_hdr(const uint8_t* buf, size_t len,
                                                PacketHeader* hdr,
                                                uint16_t* msg_id,
                                                uint16_t* index, uint16_t* total)
{
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, hdr);
    *msg_id = net_reader_u16(&r);
    *index  = net_reader_u16(&r);
    *total  = net_reader_u16(&r);
    return net_reader_ok(&r);
}

static inline size_t net_write_chunk_unload(uint8_t* buf, const PacketHeader* hdr,
                                            int32_t cx, int32_t cz) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_i32(buf, &off, cx);
    net_write_i32(buf, &off, cz);
    return off;
}

static inline void net_read_chunk_unload(const uint8_t* buf,
                                         PacketHeader* hdr,
                                         int32_t* cx, int32_t* cz) {
    size_t off = 0;
    net_read_header(buf, &off, hdr);
    *cx = net_read_i32(buf, &off);
    *cz = net_read_i32(buf, &off);
}

/* Bounds-checked parse (16 wire bytes). */
static inline int net_parse_chunk_unload(const uint8_t* buf, size_t len,
                                         PacketHeader* hdr,
                                         int32_t* cx, int32_t* cz) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, hdr);
    *cx = net_reader_i32(&r);
    *cz = net_reader_i32(&r);
    return net_reader_ok(&r);
}

/* ------------------------------------------------------------------ */
/*  Mob + health packets                                               */
/*    PKT_MOB_STATE    — [header 8][bcast_seq u32][count u16][count×20] */
/*       entry: id u16 | type u8 | x f32 | y f32 | z f32 | yaw f32 | health u8
 *    PKT_MOB_ATTACK   — [header 8][mob_id u16]            = 10 bytes  */
/*    PKT_PLAYER_HEALTH— [header 8][health u8][flags u8]   = 10 bytes  */
/* ------------------------------------------------------------------ */
#define MOB_STATE_ENTRY_SIZE 20
#define MOB_HEALTH_FLAG_DIED 0x01
/* PKT_PLAYER_HEALTH v3 appends [food u8][air u8] after [health u8][flags u8].
 * food: 0..20 hunger drumsticks. air: 0..20 oxygen bubbles. Older 10-byte
 * readers still parse health+flags; the new fields sit past their length. */
#define NET_MAX_FOOD         20
#define NET_MAX_AIR          20

typedef struct {
    uint16_t id;
    uint8_t  type;
    float    x, y, z, yaw;
    uint8_t  health;
} NetMobState;

/* Wire format: [header 8][bcast_seq u32][count u16][ count × 20 ]. bcast_seq
 * (protocol v3) is the per-stream broadcast sequence for dedup; see
 * seq_is_newer / net_write_world_state. */
static inline size_t net_write_mob_state(uint8_t* buf, const PacketHeader* hdr,
                                         uint32_t bcast_seq,
                                         const NetMobState* mobs, uint16_t count) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u32(buf, &off, bcast_seq);
    net_write_u16(buf, &off, count);
    for (uint16_t i = 0; i < count; i++) {
        net_write_u16  (buf, &off, mobs[i].id);
        net_write_u8   (buf, &off, mobs[i].type);
        net_write_float(buf, &off, mobs[i].x);
        net_write_float(buf, &off, mobs[i].y);
        net_write_float(buf, &off, mobs[i].z);
        net_write_float(buf, &off, mobs[i].yaw);
        net_write_u8   (buf, &off, mobs[i].health);
    }
    return off;
}

static inline void net_read_mob_state_header(const uint8_t* buf, size_t* off,
                                             uint16_t* out_count) {
    *out_count = net_read_u16(buf, off);
}

static inline void net_read_mob_state_entry(const uint8_t* buf, size_t* off,
                                            NetMobState* m) {
    m->id     = net_read_u16(buf, off);
    m->type   = net_read_u8(buf, off);
    m->x      = net_read_float(buf, off);
    m->y      = net_read_float(buf, off);
    m->z      = net_read_float(buf, off);
    m->yaw    = net_read_float(buf, off);
    m->health = net_read_u8(buf, off);
}

/* Bounds-checked one mob entry through a cursor (20 wire bytes). */
static inline void net_reader_mob_state_entry(NetReader* r, NetMobState* m) {
    m->id     = net_reader_u16(r);
    m->type   = net_reader_u8(r);
    m->x      = net_reader_f32(r);
    m->y      = net_reader_f32(r);
    m->z      = net_reader_f32(r);
    m->yaw    = net_reader_f32(r);
    m->health = net_reader_u8(r);
}

/* Bounds-checked one player-state entry through a cursor (21 wire bytes). */
static inline void net_reader_player_state(NetReader* r, NetPlayerState* p) {
    p->player_id = net_reader_u8(r);
    p->x         = net_reader_f32(r);
    p->y         = net_reader_f32(r);
    p->z         = net_reader_f32(r);
    p->yaw       = net_reader_f32(r);
    p->pitch     = net_reader_f32(r);
}

static inline size_t net_write_mob_attack(uint8_t* buf, const PacketHeader* hdr,
                                          uint16_t mob_id) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u16(buf, &off, mob_id);
    return off;
}

static inline void net_read_mob_attack(const uint8_t* buf, PacketHeader* hdr,
                                       uint16_t* mob_id) {
    size_t off = 0;
    net_read_header(buf, &off, hdr);
    *mob_id = net_read_u16(buf, &off);
}

/* Bounds-checked parse (10 wire bytes). */
static inline int net_parse_mob_attack(const uint8_t* buf, size_t len,
                                       PacketHeader* hdr, uint16_t* mob_id) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, hdr);
    *mob_id = net_reader_u16(&r);
    return net_reader_ok(&r);
}

static inline size_t net_write_player_health(uint8_t* buf, const PacketHeader* hdr,
                                             uint8_t health, uint8_t flags,
                                             uint8_t food, uint8_t air) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u8(buf, &off, health);
    net_write_u8(buf, &off, flags);
    net_write_u8(buf, &off, food);
    net_write_u8(buf, &off, air);
    return off;
}

/* Tolerates legacy 10-byte packets: food/air default to NET_MAX_* when absent.
 * `len` is the full wire length so the reader knows whether v3 fields exist. */
static inline void net_read_player_health(const uint8_t* buf, size_t len,
                                          PacketHeader* hdr,
                                          uint8_t* health, uint8_t* flags,
                                          uint8_t* food, uint8_t* air) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, hdr);
    *health = net_reader_u8(&r);
    *flags  = net_reader_u8(&r);
    if (net_reader_remaining(&r) >= 2) {
        *food = net_reader_u8(&r);
        *air  = net_reader_u8(&r);
    } else {
        *food = NET_MAX_FOOD;
        *air  = NET_MAX_AIR;
    }
}

/* Bounds-checked parse: returns 1 only if at least health+flags were present
 * (the minimum 10-byte packet). food/air default to full when absent. */
static inline int net_parse_player_health(const uint8_t* buf, size_t len,
                                          PacketHeader* hdr,
                                          uint8_t* health, uint8_t* flags,
                                          uint8_t* food, uint8_t* air) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, hdr);
    *health = net_reader_u8(&r);
    *flags  = net_reader_u8(&r);
    if (!net_reader_ok(&r)) return 0;
    if (net_reader_remaining(&r) >= 2) {
        *food = net_reader_u8(&r);
        *air  = net_reader_u8(&r);
    } else {
        *food = NET_MAX_FOOD;
        *air  = NET_MAX_AIR;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/*  PKT_KNOCKBACK — server → one, 20 wire bytes (8 header + 3×f32).    */
/*                                                                     */
/*  A melee/contact hit shoves the local player. dx/dy/dz are a        */
/*  velocity impulse in blocks/s (world space); the client applies it  */
/*  as a short, decaying positional nudge on top of its own movement   */
/*  (the local player owns its position, so the impulse can't ride the */
/*  position snapshots the way mob knockback does). Sent reliably so a  */
/*  dropped packet doesn't silently swallow the shove.                  */
/* ------------------------------------------------------------------ */
static inline size_t net_write_knockback(uint8_t* buf, const PacketHeader* hdr,
                                         float dx, float dy, float dz) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_float(buf, &off, dx);
    net_write_float(buf, &off, dy);
    net_write_float(buf, &off, dz);
    return off;
}

/* Bounds-checked parse (20 wire bytes). Returns 1 only if all three components
 * were present; on a short buffer returns 0 and never over-reads. */
static inline int net_parse_knockback(const uint8_t* buf, size_t len,
                                      PacketHeader* hdr,
                                      float* dx, float* dy, float* dz) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, hdr);
    *dx = net_reader_f32(&r);
    *dy = net_reader_f32(&r);
    *dz = net_reader_f32(&r);
    return net_reader_ok(&r);
}

/* ------------------------------------------------------------------ */
/*  PKT_WEATHER — server → all, 13 wire bytes (8 header + u8 + f32).   */
/*                                                                     */
/*  Authoritative weather sync. The server owns a WeatherState and     */
/*  ticks it (the rng stays server-side and is NOT sent); only the     */
/*  observable {kind, time_left} crosses the wire. `kind` is a         */
/*  WeatherKind (0 clear / 1 rain / 2 storm) carried as a u8.          */
/*  Broadcast on every weather KIND transition, sent once to a late    */
/*  joiner so it starts in sync, and re-broadcast at a low frequency   */
/*  for resync. Sent reliably so a transition isn't silently lost.     */
/* ------------------------------------------------------------------ */
static inline size_t net_write_weather(uint8_t* buf, const PacketHeader* hdr,
                                       uint8_t kind, float time_left) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u8(buf, &off, kind);
    net_write_float(buf, &off, time_left);
    return off;
}

static inline void net_read_weather(const uint8_t* buf, PacketHeader* hdr,
                                    uint8_t* kind, float* time_left) {
    size_t off = 0;
    net_read_header(buf, &off, hdr);
    *kind = net_read_u8(buf, &off);
    *time_left = net_read_float(buf, &off);
}

/* Bounds-checked parse (13 wire bytes). Returns 1 only if both fields were
 * present; on a short buffer returns 0 and never over-reads. */
static inline int net_parse_weather(const uint8_t* buf, size_t len,
                                    PacketHeader* hdr,
                                    uint8_t* kind, float* time_left) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, hdr);
    *kind = net_reader_u8(&r);
    *time_left = net_reader_f32(&r);
    return net_reader_ok(&r);
}

/* ------------------------------------------------------------------ */
/*  Simple reliable-channel packets (header only, no extra payload)   */
/*  Used for: PKT_CONNECT_REQUEST, PKT_CONNECT_ACCEPT, PKT_DISCONNECT */
/*            PKT_PLAYER_JOIN, PKT_PLAYER_LEAVE                        */
/* ------------------------------------------------------------------ */
/* PKT_CONNECT_ACCEPT carries assigned player_id in header.player_id  */
/* PKT_PLAYER_JOIN/LEAVE carry the affected player_id in player_id    */

/* PKT_CONNECT_REQUEST carries a u16 protocol version after the header so the
 * server can reject incompatible clients. PKT_DISCONNECT carries a u8 reason
 * (NetDisconnectReason). Both readers tolerate legacy header-only packets:
 * a missing version reads as 0 (refused), a missing reason as NORMAL. */

/* PKT_CONNECT_REQUEST (protocol v8) appends, after the version:
 *   u8 shared_world  — 1 when the connecting client is the integrated host that
 *                      already renders the server's world in-process (so the
 *                      server must NOT stream chunks to it), 0 for a true remote
 *                      client that needs chunk streaming.
 *   u8 render_dist   — the client's render distance in chunks, so the server can
 *                      stream the disc the client actually wants (clamped to the
 *                      server's own cap). 0 means "unspecified" -> server uses
 *                      its own render distance.
 * Legacy/short reads default both fields to 0 (remote, server-rd). */
static inline void net_write_connect_request_ex(uint8_t* buf, size_t* off,
                                                const PacketHeader* h,
                                                uint8_t shared_world,
                                                uint8_t render_dist)
{
    net_write_header(buf, off, h);
    net_write_u16(buf, off, (uint16_t)NET_PROTOCOL_VERSION);
    net_write_u8(buf, off, shared_world);
    net_write_u8(buf, off, render_dist);
}

static inline void net_write_connect_request(uint8_t* buf, size_t* off,
                                             const PacketHeader* h)
{
    net_write_connect_request_ex(buf, off, h, 0, 0);
}

static inline uint16_t net_read_connect_version(const uint8_t* buf, size_t len)
{
    if (len < HEADER_WIRE_SIZE + 2) return 0;  /* legacy / malformed */
    size_t off = HEADER_WIRE_SIZE;
    return net_read_u16(buf, &off);
}

static inline uint8_t net_read_connect_shared(const uint8_t* buf, size_t len)
{
    if (len < HEADER_WIRE_SIZE + 3) return 0;  /* absent -> remote (stream) */
    size_t off = HEADER_WIRE_SIZE + 2;
    return net_read_u8(buf, &off);
}

static inline uint8_t net_read_connect_render_dist(const uint8_t* buf, size_t len)
{
    if (len < HEADER_WIRE_SIZE + 4) return 0;  /* absent -> use server's rd */
    size_t off = HEADER_WIRE_SIZE + 3;
    return net_read_u8(buf, &off);
}

static inline void net_write_disconnect(uint8_t* buf, size_t* off,
                                        const PacketHeader* h, uint8_t reason)
{
    net_write_header(buf, off, h);
    net_write_u8(buf, off, reason);
}

static inline uint8_t net_read_disconnect_reason(const uint8_t* buf, size_t len)
{
    if (len < HEADER_WIRE_SIZE + 1) return NET_DISCONNECT_NORMAL;
    size_t off = HEADER_WIRE_SIZE;
    return net_read_u8(buf, &off);
}

/* ------------------------------------------------------------------ */
/*  PKT_KEEPALIVE — header-only liveness ping/pong (protocol v9)        */
/*                                                                     */
/*  Sent UNRELIABLE in both directions on a periodic timer so that:     */
/*    - NAT/firewall pinholes stay open (a few seconds < typical ~30s   */
/*      UDP mapping timeout) for an otherwise-idle connection, and      */
/*    - each peer can notice the other has gone away (no traffic for N  */
/*      seconds) and surface a clean disconnect instead of hanging.     */
/*  It carries no payload beyond the 8-byte header; its ack/ack_bits    */
/*  still piggyback the reliable channel state like any other packet.   */
/* ------------------------------------------------------------------ */
static inline size_t net_write_keepalive(uint8_t* buf, const PacketHeader* h)
{
    size_t off = 0;
    net_write_header(buf, &off, h);
    return off;   /* 8 */
}

/* Bounds-checked parse: returns 1 only if a full 8-byte header was present. */
static inline int net_parse_keepalive(const uint8_t* buf, size_t len,
                                      PacketHeader* h)
{
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, h);
    return net_reader_ok(&r);
}

/* ------------------------------------------------------------------ */
/*  Container packets (furnace + chest block-entities, protocol v11)    */
/*                                                                     */
/*  A container block-entity lives server-side, keyed by its block      */
/*  position. The client opens it (PKT_CONTAINER_OPEN), the server       */
/*  validates reach + replies with a tagged snapshot (PKT_CONTAINER_     */
/*  STATE), the client moves items in/out (PKT_CONTAINER_ACTION) and     */
/*  finally stops viewing it (PKT_CONTAINER_CLOSE). All wire fields use  */
/*  the bounds-checked cursor codec; every parser rejects truncation.   */
/* ------------------------------------------------------------------ */

/* Container type tag on the wire (kept independent of any gameplay enum so
 * net.h stays at the bottom of the dependency tree). */
typedef enum {
    CONTAINER_NET_FURNACE = 0,
    CONTAINER_NET_CHEST   = 1,
} ContainerNetType;

/* Direction of an item move in PKT_CONTAINER_ACTION. */
typedef enum {
    CONTAINER_DIR_TO_INV   = 0,  /* container slot -> player inventory */
    CONTAINER_DIR_FROM_INV = 1,  /* player inventory slot -> container */
} ContainerNetDir;

/* Must match CHEST_SLOTS in container.h; restated here so net.h has no
 * dependency on container.h. A _Static_assert in server.c guards them. */
#define CONTAINER_NET_CHEST_SLOTS  27

/* ---- PKT_CONTAINER_OPEN — client → server, 8 + 3*i32 = 20 bytes ---- */
static inline size_t net_write_container_open(uint8_t* buf, const PacketHeader* h,
                                              int32_t x, int32_t y, int32_t z) {
    size_t off = 0;
    net_write_header(buf, &off, h);
    net_write_i32(buf, &off, x);
    net_write_i32(buf, &off, y);
    net_write_i32(buf, &off, z);
    return off;
}

static inline int net_parse_container_open(const uint8_t* buf, size_t len,
                                           PacketHeader* h,
                                           int32_t* x, int32_t* y, int32_t* z) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, h);
    *x = net_reader_i32(&r);
    *y = net_reader_i32(&r);
    *z = net_reader_i32(&r);
    return net_reader_ok(&r);
}

/* ---- PKT_CONTAINER_CLOSE — client → server, same shape as OPEN ---- */
static inline size_t net_write_container_close(uint8_t* buf, const PacketHeader* h,
                                               int32_t x, int32_t y, int32_t z) {
    return net_write_container_open(buf, h, x, y, z);
}

static inline int net_parse_container_close(const uint8_t* buf, size_t len,
                                            PacketHeader* h,
                                            int32_t* x, int32_t* y, int32_t* z) {
    return net_parse_container_open(buf, len, h, x, y, z);
}

/* ---- PKT_CONTAINER_ACTION — client → server, 8 + 3*i32 + 1 + 1 + 1 = 23 ---- */
static inline size_t net_write_container_action(uint8_t* buf, const PacketHeader* h,
                                                int32_t x, int32_t y, int32_t z,
                                                uint8_t slot, uint8_t dir,
                                                uint8_t count) {
    size_t off = 0;
    net_write_header(buf, &off, h);
    net_write_i32(buf, &off, x);
    net_write_i32(buf, &off, y);
    net_write_i32(buf, &off, z);
    net_write_u8(buf, &off, slot);
    net_write_u8(buf, &off, dir);
    net_write_u8(buf, &off, count);
    return off;
}

static inline int net_parse_container_action(const uint8_t* buf, size_t len,
                                             PacketHeader* h,
                                             int32_t* x, int32_t* y, int32_t* z,
                                             uint8_t* slot, uint8_t* dir,
                                             uint8_t* count) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, h);
    *x     = net_reader_i32(&r);
    *y     = net_reader_i32(&r);
    *z     = net_reader_i32(&r);
    *slot  = net_reader_u8(&r);
    *dir   = net_reader_u8(&r);
    *count = net_reader_u8(&r);
    return net_reader_ok(&r);
}

/* ---- PKT_CONTAINER_STATE — server → one, tagged by container type ----
 * Wire: [header 8][x i32][y i32][z i32][type u8] then, by type:
 *   FURNACE: input(item u16,count u8) fuel(u16,u8) output(u16,u8)
 *            [burn_ticks_left i32][cook_progress i32]
 *   CHEST:   27 × (item u16, count u8)
 * A single struct carries both; the writer/parser branch on `type`. */
typedef struct {
    int32_t  x, y, z;
    uint8_t  type;              /* ContainerNetType */
    /* furnace fields (valid when type == CONTAINER_NET_FURNACE) */
    uint16_t f_input;  uint8_t f_input_count;
    uint16_t f_fuel;   uint8_t f_fuel_count;
    uint16_t f_output; uint8_t f_output_count;
    int32_t  f_burn_ticks_left;
    int32_t  f_cook_progress;
    /* chest fields (valid when type == CONTAINER_NET_CHEST) */
    struct { uint16_t item; uint8_t count; } slots[CONTAINER_NET_CHEST_SLOTS];
} ContainerStatePacket;

static inline size_t net_write_container_state(uint8_t* buf,
                                               const PacketHeader* h,
                                               const ContainerStatePacket* p) {
    size_t off = 0;
    net_write_header(buf, &off, h);
    net_write_i32(buf, &off, p->x);
    net_write_i32(buf, &off, p->y);
    net_write_i32(buf, &off, p->z);
    net_write_u8(buf, &off, p->type);
    if (p->type == CONTAINER_NET_FURNACE) {
        net_write_u16(buf, &off, p->f_input);  net_write_u8(buf, &off, p->f_input_count);
        net_write_u16(buf, &off, p->f_fuel);   net_write_u8(buf, &off, p->f_fuel_count);
        net_write_u16(buf, &off, p->f_output); net_write_u8(buf, &off, p->f_output_count);
        net_write_i32(buf, &off, p->f_burn_ticks_left);
        net_write_i32(buf, &off, p->f_cook_progress);
    } else {
        for (int i = 0; i < CONTAINER_NET_CHEST_SLOTS; i++) {
            net_write_u16(buf, &off, p->slots[i].item);
            net_write_u8 (buf, &off, p->slots[i].count);
        }
    }
    return off;
}

static inline int net_parse_container_state(const uint8_t* buf, size_t len,
                                            PacketHeader* h,
                                            ContainerStatePacket* p) {
    NetReader r = net_reader_init(buf, len);
    net_reader_header(&r, h);
    p->x    = net_reader_i32(&r);
    p->y    = net_reader_i32(&r);
    p->z    = net_reader_i32(&r);
    p->type = net_reader_u8(&r);
    if (!net_reader_ok(&r)) return 0;
    if (p->type == CONTAINER_NET_FURNACE) {
        p->f_input  = net_reader_u16(&r); p->f_input_count  = net_reader_u8(&r);
        p->f_fuel   = net_reader_u16(&r); p->f_fuel_count   = net_reader_u8(&r);
        p->f_output = net_reader_u16(&r); p->f_output_count = net_reader_u8(&r);
        p->f_burn_ticks_left = net_reader_i32(&r);
        p->f_cook_progress   = net_reader_i32(&r);
    } else if (p->type == CONTAINER_NET_CHEST) {
        for (int i = 0; i < CONTAINER_NET_CHEST_SLOTS; i++) {
            p->slots[i].item  = net_reader_u16(&r);
            p->slots[i].count = net_reader_u8(&r);
        }
    } else {
        return 0;   /* unknown type tag */
    }
    return net_reader_ok(&r);
}

/* ------------------------------------------------------------------ */
/*  UDP socket helpers                                                 */
/* ------------------------------------------------------------------ */
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <netinet/in.h>
#endif

/* Returns socket fd (non-blocking), or -1 on error.
 * Server: binds to 0.0.0.0:port. Client: unbound, use sendto. */
int  net_socket_server(uint16_t port);
int  net_socket_client(void);
void net_socket_close(int fd);

/* ------------------------------------------------------------------ */
/*  Address resolution (DNS hostnames + IP literals)                   */
/* ------------------------------------------------------------------ */
/* Resolve `host` (a DNS hostname, a dotted-quad IPv4 literal, or an IPv6
 * literal) and `port` via getaddrinfo. On success fills *out with the resolved
 * IPv4 socket address (in network byte order, ready for net_send) and returns
 * NET_RESOLVE_OK. This replaces the old inet_pton path so --client accepts
 * names like "host.example.com", not just "1.2.3.4".
 *
 * The current UDP transport is IPv4 (struct sockaddr_in) end-to-end, so an
 * address that resolves ONLY to IPv6 is reported as NET_RESOLVE_NO_IPV4 rather
 * than silently failing — the caller can surface a clear message. A name with
 * both A and AAAA records resolves to its IPv4 (A) address and works today.
 *
 * Pure apart from the DNS lookup itself (no globals, no sockets created), so it
 * is unit-testable against loopback literals ("127.0.0.1"). */
typedef enum {
    NET_RESOLVE_OK        = 0,
    NET_RESOLVE_FAILED    = 1,  /* unknown host / lookup error */
    NET_RESOLVE_NO_IPV4   = 2,  /* resolved, but only IPv6 addresses available */
} NetResolveResult;

NetResolveResult net_resolve(const char* host, uint16_t port,
                             struct sockaddr_in* out);

/* Returns bytes sent/received, 0 on EAGAIN/EWOULDBLOCK, -1 on error. */
int  net_send(int fd, const void* buf, int len,
              const struct sockaddr_in* addr);
int  net_recv(int fd, void* buf, int len,
              struct sockaddr_in* from);

/* Monotonic clock in seconds — valid in both server and client modes */
double net_time(void);

#endif /* NET_H */
