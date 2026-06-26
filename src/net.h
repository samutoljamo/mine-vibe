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
/*  WorldStatePacket — 9 + 21*N wire bytes                            */
/* ------------------------------------------------------------------ */
#define WORLD_STATE_PLAYER_SIZE 21  /* 1 + 4 + 4 + 4 + 4 + 4 */

typedef struct {
    uint8_t  player_id;
    float    x, y, z;
    float    yaw, pitch;
} NetPlayerState;

/* Wire format: [header 8][count 1][players 21*N] */
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
/*  Block edit packets                                                 */
/*    BlockBreakPacket  — client → server, 21 wire bytes (8 + 12 + 1) */
/*    BlockPlacePacket  — client → server, 22 wire bytes (8 + 14)     */
/*    BlockChangePacket — server → all,    21 wire bytes (8 + 13)     */
/* ------------------------------------------------------------------ */
typedef struct {
    PacketHeader header;
    int32_t      x, y, z;
    uint8_t      block;   /* claimed block ID being broken — server checks
                            * it isn't AIR/WATER/BEDROCK before crediting
                            * inventory. Server has no world today, so it
                            * trusts the client on the actual cell content. */
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

typedef struct {
    PacketHeader header;
    uint8_t      slot_count;
    struct { uint8_t block; uint8_t count; } slots[INVENTORY_NET_SLOTS];
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

static inline size_t net_write_inventory(uint8_t* buf,
                                          const InventoryPacket* p)
{
    size_t off = 0;
    net_write_header(buf, &off, &p->header);
    net_write_u8(buf, &off, p->slot_count);
    for (uint8_t i = 0; i < p->slot_count; i++) {
        net_write_u8(buf, &off, p->slots[i].block);
        net_write_u8(buf, &off, p->slots[i].count);
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
        p->slots[i].block = net_read_u8(buf, &off);
        p->slots[i].count = net_read_u8(buf, &off);
    }
}

/* ------------------------------------------------------------------ */
/*  Mob + health packets                                               */
/*    PKT_MOB_STATE    — [header 8][count u16][ count × 20 ]           */
/*       entry: id u16 | type u8 | x f32 | y f32 | z f32 | yaw f32 | health u8
 *    PKT_MOB_ATTACK   — [header 8][mob_id u16]            = 10 bytes  */
/*    PKT_PLAYER_HEALTH— [header 8][health u8][flags u8]   = 10 bytes  */
/* ------------------------------------------------------------------ */
#define MOB_STATE_ENTRY_SIZE 20
#define MOB_HEALTH_FLAG_DIED 0x01

typedef struct {
    uint16_t id;
    uint8_t  type;
    float    x, y, z, yaw;
    uint8_t  health;
} NetMobState;

static inline size_t net_write_mob_state(uint8_t* buf, const PacketHeader* hdr,
                                         const NetMobState* mobs, uint16_t count) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
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
                                             uint8_t health, uint8_t flags) {
    size_t off = 0;
    net_write_header(buf, &off, hdr);
    net_write_u8(buf, &off, health);
    net_write_u8(buf, &off, flags);
    return off;
}

static inline void net_read_player_health(const uint8_t* buf, PacketHeader* hdr,
                                          uint8_t* health, uint8_t* flags) {
    size_t off = 0;
    net_read_header(buf, &off, hdr);
    *health = net_read_u8(buf, &off);
    *flags  = net_read_u8(buf, &off);
}

/* ------------------------------------------------------------------ */
/*  Simple reliable-channel packets (header only, no extra payload)   */
/*  Used for: PKT_CONNECT_REQUEST, PKT_CONNECT_ACCEPT, PKT_DISCONNECT */
/*            PKT_PLAYER_JOIN, PKT_PLAYER_LEAVE                        */
/* ------------------------------------------------------------------ */
/* PKT_CONNECT_ACCEPT carries assigned player_id in header.player_id  */
/* PKT_PLAYER_JOIN/LEAVE carry the affected player_id in player_id    */

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
