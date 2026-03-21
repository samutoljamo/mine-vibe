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
