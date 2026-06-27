#include "client.h"
#include "net_thread.h"
#include "gameplay.h"
#include "daynight.h"   /* DAY_LENGTH_TICKS for noon init */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

_Static_assert(INVENTORY_SLOTS == INVENTORY_NET_SLOTS,
    "wire format and inventory model must agree on slot count");

static ClientSnapshotCb g_snap_cb   = NULL;
static void*             g_snap_user = NULL;
static ClientLeaveCb     g_leave_cb   = NULL;
static void*             g_leave_user = NULL;
static ClientMobsCb      g_mobs_cb   = NULL;
static void*             g_mobs_user = NULL;
static ClientDeathCb     g_death_cb  = NULL;
static void*             g_death_user = NULL;

void client_set_snapshot_cb(Client* c, ClientSnapshotCb cb, void* user)
{
    (void)c;
    g_snap_cb   = cb;
    g_snap_user = user;
}

void client_set_leave_cb(Client* c, ClientLeaveCb cb, void* user)
{
    (void)c;
    g_leave_cb   = cb;
    g_leave_user = user;
}

void client_set_mobs_cb(Client* c, ClientMobsCb cb, void* user)
{
    (void)c;
    g_mobs_cb   = cb;
    g_mobs_user = user;
}

void client_set_death_cb(Client* c, ClientDeathCb cb, void* user)
{
    (void)c; g_death_cb = cb; g_death_user = user;
}

void client_init(Client* c, NetThread* net,
                  const struct sockaddr_in* server_addr)
{
    memset(c, 0, sizeof(*c));
    c->net         = net;
    c->state       = CLIENT_DISCONNECTED;
    c->server_addr = *server_addr;
    reliable_init(&c->reliable);
    inventory_init(&c->inventory);
    c->health = PLAYER_MAX_HEALTH;
    c->pending_block_change_count = 0;
    /* Start at noon so the sky is daylit before the first PKT_WORLD_STATE. */
    c->world_ticks = DAY_LENGTH_TICKS / 4;
    c->world_ticks_recv_time = 0.0;
}

void client_destroy(Client* c) { (void)c; }

/* Server runs the world clock at 20 Hz; mirror that here for extrapolation.
 * Kept local so client.c doesn't need to pull in server.h. */
#define CLIENT_SERVER_TICK_RATE 20.0

uint32_t client_estimate_world_ticks(const Client* c)
{
    /* Before the first packet, world_ticks_recv_time is 0: just use the
     * noon seed set in client_init (no extrapolation). */
    if (c->world_ticks_recv_time <= 0.0)
        return c->world_ticks;
    double elapsed = net_time() - c->world_ticks_recv_time;
    if (elapsed < 0.0) elapsed = 0.0;
    double advanced = elapsed * CLIENT_SERVER_TICK_RATE;
    /* u32 add wraps modulo 2^32, which is exactly what the day phase math
     * expects; cast through double carefully to avoid UB on huge elapsed. */
    uint32_t delta = (uint32_t)advanced;
    return c->world_ticks + delta;
}

void client_connect(Client* c)
{
    uint8_t buf[HEADER_WIRE_SIZE + 2];
    size_t off = 0;
    PacketHeader h = { .type = PKT_CONNECT_REQUEST, .player_id = 0 };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    net_write_connect_request(buf, &off, &h);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr,
                   buf, (uint16_t)off);
    c->state            = CLIENT_CONNECTING;
    c->connect_sent_time = net_time();
    printf("[client] connecting...\n");
}

void client_send_position(Client* c,
                            float x, float y, float z,
                            float yaw, float pitch)
{
    if (c->state != CLIENT_CONNECTED) return;

    PositionPacket p = {0};
    p.header.type      = PKT_POSITION;
    p.header.player_id = c->local_player_id;
    p.header.seq       = c->tick;
    reliable_fill_ack(&c->reliable, &p.header.ack, &p.header.ack_bits);
    p.tick  = c->tick++;
    p.x     = x;
    p.y     = y;
    p.z     = z;
    p.yaw   = yaw;
    p.pitch = pitch;

    uint8_t buf[32];
    size_t len = net_write_position(buf, &p);
    net_thread_push_outbound(c->net, buf, (int)len, &c->server_addr);
}

void client_send_break(Client* c, int x, int y, int z, uint8_t block)
{
    if (c->state != CLIENT_CONNECTED) return;
    BlockBreakPacket p = {
        .header = {
            .type      = PKT_BLOCK_BREAK,
            .player_id = c->local_player_id,
            .seq       = c->tick++,
        },
        .x = x, .y = y, .z = z, .block = block,
    };
    reliable_fill_ack(&c->reliable, &p.header.ack, &p.header.ack_bits);
    uint8_t buf[64];
    size_t  len = net_write_block_break(buf, &p);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr,
                   buf, (uint16_t)len);
}

void client_send_place(Client* c, int x, int y, int z,
                        uint8_t face, uint8_t slot)
{
    if (c->state != CLIENT_CONNECTED) return;
    BlockPlacePacket p = {
        .header = {
            .type      = PKT_BLOCK_PLACE,
            .player_id = c->local_player_id,
            .seq       = c->tick++,
        },
        .x = x, .y = y, .z = z, .face = face, .slot = slot,
    };
    reliable_fill_ack(&c->reliable, &p.header.ack, &p.header.ack_bits);
    uint8_t buf[64];
    size_t  len = net_write_block_place(buf, &p);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr,
                   buf, (uint16_t)len);
}

void client_send_mob_attack(Client* c, uint16_t mob_id) {
    if (c->state != CLIENT_CONNECTED) return;
    PacketHeader h = { .type = PKT_MOB_ATTACK, .player_id = c->local_player_id,
                       .seq = c->tick++ };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    uint8_t buf[16];
    size_t len = net_write_mob_attack(buf, &h, mob_id);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr, buf, (uint16_t)len);
}

int client_poll(Client* c)
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

            /* Per-stream broadcast seq (protocol v3): drop reordered/late
             * datagrams so a stale snapshot never overwrites a newer one. */
            if (msg->len < (int)off + 4) { free(msg); continue; }
            uint32_t bseq = net_read_u32(msg->data, &off);
            if (c->world_state_seq_valid &&
                !seq_is_newer(bseq, c->world_state_seq)) {
                free(msg);
                continue;
            }

            uint8_t count = net_read_u8(msg->data, &off);

            /* Validate: packet must contain count * (1+5*4) player bytes plus
             * a trailing u32 world_ticks (protocol v3), after header+seq+count. */
            int required = (int)off + count * (1 + 5 * 4) + 4;
            if (required > msg->len) {
                fprintf(stderr, "[client] PKT_WORLD_STATE truncated "
                        "(need %d bytes, have %d)\n", required, msg->len);
                free(msg);
                continue;
            }

            /* Commit: this packet is being applied, so it becomes the newest. */
            c->world_state_seq       = bseq;
            c->world_state_seq_valid = true;

            for (int i = 0; i < count; i++) {
                uint8_t pid = net_read_u8(msg->data, &off);
                float x     = net_read_float(msg->data, &off);
                float y     = net_read_float(msg->data, &off);
                float z     = net_read_float(msg->data, &off);
                float yaw   = net_read_float(msg->data, &off);
                float pitch = net_read_float(msg->data, &off);

                /* Skip our own position — we already simulate it locally */
                if (pid == c->local_player_id) continue;

                if (g_snap_cb) {
                    ClientPlayerSnapshot snap = {
                        .player_id = pid, .x = x, .y = y, .z = z,
                        .yaw = yaw, .pitch = pitch,
                        .recv_time = msg->recv_time,
                    };
                    g_snap_cb(&snap, g_snap_user);
                }
            }

            /* Trailing day/night clock. Re-anchor every packet; the renderer
             * extrapolates from here so missed packets keep the sky moving and
             * a u32 wrap (or backward jump) simply re-anchors harmlessly. */
            c->world_ticks           = net_read_u32(msg->data, &off);
            c->world_ticks_recv_time = msg->recv_time;
            state_packets++;

        } else if (type == PKT_MOB_STATE && c->state == CLIENT_CONNECTED) {
            size_t off = 0; PacketHeader hdr; net_read_header(msg->data, &off, &hdr);

            /* Per-stream broadcast seq (protocol v3): drop stale reordered
             * snapshots before applying (see seq_is_newer in net.h). */
            if (msg->len < (int)off + 4) { free(msg); continue; }
            uint32_t bseq = net_read_u32(msg->data, &off);
            if (c->mob_state_seq_valid &&
                !seq_is_newer(bseq, c->mob_state_seq)) {
                free(msg);
                continue;
            }

            uint16_t count; net_read_mob_state_header(msg->data, &off, &count);
            int required = (int)off + (int)count * MOB_STATE_ENTRY_SIZE;
            if (count > MOB_MAX || required > msg->len) { free(msg); continue; }

            c->mob_state_seq       = bseq;
            c->mob_state_seq_valid = true;
            ClientMobSnapshot snaps[MOB_MAX];
            for (uint16_t i = 0; i < count; i++) {
                NetMobState m; net_read_mob_state_entry(msg->data, &off, &m);
                snaps[i].id = m.id; snaps[i].type = m.type;
                snaps[i].x = m.x; snaps[i].y = m.y; snaps[i].z = m.z;
                snaps[i].yaw = m.yaw; snaps[i].health = m.health;
            }
            if (g_mobs_cb) g_mobs_cb(snaps, (int)count, msg->recv_time, g_mobs_user);

        } else if (type == PKT_PLAYER_JOIN || type == PKT_PLAYER_LEAVE) {
            PacketHeader h; size_t off = 0;
            net_read_header(msg->data, &off, &h);
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new) {
                if (type == PKT_PLAYER_JOIN)
                    printf("[client] player %d joined\n", h.player_id);
                else {
                    printf("[client] player %d left\n", h.player_id);
                    if (g_leave_cb) g_leave_cb(h.player_id, g_leave_user);
                }
            }

        } else if (type == PKT_BLOCK_CHANGE && c->state == CLIENT_CONNECTED) {
            PacketHeader h; size_t off = 0;
            net_read_header(msg->data, &off, &h);
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new && (size_t)msg->len >= 8 + 13) {
                BlockChangePacket bp;
                net_read_block_change(msg->data, &bp);
                if (c->pending_block_change_count < 256) {
                    int i = c->pending_block_change_count++;
                    c->pending_block_changes[i].x     = bp.x;
                    c->pending_block_changes[i].y     = bp.y;
                    c->pending_block_changes[i].z     = bp.z;
                    c->pending_block_changes[i].block = bp.block;
                } else {
                    fprintf(stderr, "[client] pending_block_changes full (%d), dropping edit (%d,%d,%d block=%u); world will diverge until reload\n",
                            c->pending_block_change_count, bp.x, bp.y, bp.z, bp.block);
                }
            }

        } else if (type == PKT_INVENTORY && c->state == CLIENT_CONNECTED) {
            PacketHeader h; size_t off = 0;
            net_read_header(msg->data, &off, &h);
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new && (size_t)msg->len >= 8 + 1) {
                /* Peek slot_count from the wire and verify total length covers
                 * the implied body (1B per block + 1B per count per slot). */
                uint8_t declared_slots = ((const uint8_t*)msg->data)[8];
                if (declared_slots > INVENTORY_NET_SLOTS) declared_slots = INVENTORY_NET_SLOTS;
                if ((size_t)msg->len >= (size_t)(8 + 1 + declared_slots * 2)) {
                    InventoryPacket ip;
                    net_read_inventory(msg->data, &ip);
                    int prev_selected = c->inventory.selected;
                    inventory_init(&c->inventory);
                    c->inventory.selected = prev_selected;  /* preserve focus */
                    for (uint8_t i = 0; i < ip.slot_count && i < INVENTORY_SLOTS; i++) {
                        BlockID b = (BlockID)ip.slots[i].block;
                        if ((unsigned)b >= BLOCK_COUNT) continue;   /* range-check; ignore garbage IDs */
                        c->inventory.slots[i].block = b;
                        c->inventory.slots[i].count = ip.slots[i].count;
                    }
                }
            }

        } else if (type == PKT_PLAYER_HEALTH && c->state == CLIENT_CONNECTED) {
            PacketHeader h; size_t off = 0; net_read_header(msg->data, &off, &h);
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new && (size_t)msg->len >= 10) {
                uint8_t hp, fl; net_read_player_health(msg->data, &h, &hp, &fl);
                c->health = (int16_t)hp;
                if ((fl & MOB_HEALTH_FLAG_DIED) && g_death_cb) {
                    c->health = PLAYER_MAX_HEALTH;
                    g_death_cb(g_death_user);
                }
            }

        } else if (type == PKT_DISCONNECT) {
            uint8_t reason = net_read_disconnect_reason(msg->data, (size_t)msg->len);
            switch (reason) {
                case NET_DISCONNECT_VERSION_MISMATCH:
                    fprintf(stderr, "[client] rejected: protocol version mismatch "
                                    "(client v%d). Update client/server to match.\n",
                            NET_PROTOCOL_VERSION);
                    break;
                case NET_DISCONNECT_SERVER_FULL:
                    fprintf(stderr, "[client] rejected: server full\n");
                    break;
                default:
                    printf("[client] disconnected by server\n");
                    break;
            }
            c->state = CLIENT_DISCONNECTED;
        }
        free(msg);
    }

    /* Connect timeout: resend every 2s, give up after CLIENT_MAX_CONNECT_ATTEMPTS */
    if (c->state == CLIENT_CONNECTING
        && net_time() - c->connect_sent_time > 2.0) {
        if (c->connect_attempts >= CLIENT_MAX_CONNECT_ATTEMPTS) {
            fprintf(stderr, "[client] connect timed out after %d retries\n",
                    c->connect_attempts);
            c->state = CLIENT_DISCONNECTED;
        } else {
            printf("[client] retrying connect (attempt %d/%d)...\n",
                   c->connect_attempts + 1, CLIENT_MAX_CONNECT_ATTEMPTS);
            c->connect_attempts++;
            client_connect(c);
        }
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
