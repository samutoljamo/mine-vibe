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
} PacketType;

#define NET_MAX_PLAYERS  255
#define NET_DEFAULT_PORT 25565
#define NET_MAX_PACKET   1400  /* safe below typical MTU */

/* Wire protocol version. Bump whenever any on-the-wire packet format changes
 * so mismatched client/server builds are rejected at connect time instead of
 * silently misparsing each other's bytes. A client that sends a different
 * version (or none — legacy header-only connect, read as 0) is refused with
 * NET_DISCONNECT_VERSION_MISMATCH. */
#define NET_PROTOCOL_VERSION 4

typedef enum {
    NET_DISCONNECT_NORMAL           = 0,
    NET_DISCONNECT_VERSION_MISMATCH = 1,
    NET_DISCONNECT_SERVER_FULL      = 2,
} NetDisconnectReason;

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
    size_t off = 0;
    net_read_header(buf, &off, hdr);
    *health = net_read_u8(buf, &off);
    *flags  = net_read_u8(buf, &off);
    if (len >= HEADER_WIRE_SIZE + 4) {
        *food = net_read_u8(buf, &off);
        *air  = net_read_u8(buf, &off);
    } else {
        *food = NET_MAX_FOOD;
        *air  = NET_MAX_AIR;
    }
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

static inline void net_write_connect_request(uint8_t* buf, size_t* off,
                                             const PacketHeader* h)
{
    net_write_header(buf, off, h);
    net_write_u16(buf, off, (uint16_t)NET_PROTOCOL_VERSION);
}

static inline uint16_t net_read_connect_version(const uint8_t* buf, size_t len)
{
    if (len < HEADER_WIRE_SIZE + 2) return 0;  /* legacy / malformed */
    size_t off = HEADER_WIRE_SIZE;
    return net_read_u16(buf, &off);
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

/* Returns bytes sent/received, 0 on EAGAIN/EWOULDBLOCK, -1 on error. */
int  net_send(int fd, const void* buf, int len,
              const struct sockaddr_in* addr);
int  net_recv(int fd, void* buf, int len,
              struct sockaddr_in* from);

/* Monotonic clock in seconds — valid in both server and client modes */
double net_time(void);

#endif /* NET_H */
