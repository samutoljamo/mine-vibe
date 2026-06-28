#include "client.h"
#include "net_thread.h"
#include "gameplay.h"
#include "daynight.h"   /* DAY_LENGTH_TICKS for noon init */
#include "ui/hud.h"     /* hud_set_survival — latch food/air for the HUD */
#include "audio.h"      /* audio_init/play/shutdown — client-side SFX hooks */
#include "block.h"      /* BLOCK_AIR — distinguish break vs place events */
#include "chunkwire.h"  /* decode streamed RLE columns (PKT_CHUNK_DATA) */
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
static ClientChunkCb        g_chunk_cb        = NULL;
static void*                g_chunk_user      = NULL;
static ClientChunkUnloadCb  g_chunk_unload_cb = NULL;
static void*                g_chunk_unload_user = NULL;

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

void client_set_chunk_cb(Client* c, ClientChunkCb cb, void* user)
{
    (void)c; g_chunk_cb = cb; g_chunk_user = user;
}

void client_set_chunk_unload_cb(Client* c, ClientChunkUnloadCb cb, void* user)
{
    (void)c; g_chunk_unload_cb = cb; g_chunk_unload_user = user;
}

void client_set_shared_world(Client* c, bool shared) { c->shared_world = shared; }

void client_set_render_distance(Client* c, int rd) {
    if (rd < 0) rd = 0;
    if (rd > 255) rd = 255;
    c->render_distance = (uint8_t)rd;
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
    c->food   = NET_MAX_FOOD;
    c->air    = NET_MAX_AIR;
    for (int i = 0; i < ARMOR_SLOT_COUNT; i++) c->armor[i] = (ItemId)BLOCK_AIR;
    c->armor_points = 0;
    c->pending_block_change_count = 0;
    c->shared_world = false;
    /* memset(c,0) above already cleared chunk_reasm (active=false). */
    /* Start at noon so the sky is daylit before the first PKT_WORLD_STATE. */
    c->world_ticks = DAY_LENGTH_TICKS / 4;
    c->world_ticks_recv_time = 0.0;

    /* Bring up the audio engine from the client (the only place that owns the
     * server-event stream that drives SFX). Safe no-op with the null backend
     * and idempotent, so this is fine even with multiple clients in a test. */
    audio_init();
    audio_set_music(true);
}

void client_destroy(Client* c) { (void)c; audio_shutdown(); }

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
    uint8_t buf[HEADER_WIRE_SIZE + 4];
    size_t off = 0;
    PacketHeader h = { .type = PKT_CONNECT_REQUEST, .player_id = 0 };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    net_write_connect_request_ex(buf, &off, &h, c->shared_world ? 1 : 0,
                                 c->render_distance);
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

void client_send_break(Client* c, int x, int y, int z, uint8_t block,
                       uint8_t slot)
{
    if (c->state != CLIENT_CONNECTED) return;
    BlockBreakPacket p = {
        .header = {
            .type      = PKT_BLOCK_BREAK,
            .player_id = c->local_player_id,
            .seq       = c->tick++,
        },
        .x = x, .y = y, .z = z, .block = block, .slot = slot,
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

void client_send_craft(Client* c, uint16_t recipe_index) {
    if (c->state != CLIENT_CONNECTED) return;
    PacketHeader h = { .type = PKT_CRAFT, .player_id = c->local_player_id,
                       .seq = c->tick++ };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    uint8_t buf[16];
    size_t len = net_write_craft(buf, &h, recipe_index);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr, buf, (uint16_t)len);
}

void client_send_equip(Client* c, uint8_t slot) {
    if (c->state != CLIENT_CONNECTED) return;
    PacketHeader h = { .type = PKT_EQUIP, .player_id = c->local_player_id,
                       .seq = c->tick++ };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    uint8_t buf[16];
    size_t len = net_write_equip(buf, &h, slot);
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
                /* SFX hook: a change to AIR is a break, anything else a place.
                 * Positional so distant edits by other players are quieter. */
                {
                    float pos[3]      = { (float)bp.x, (float)bp.y, (float)bp.z };
                    float listener[3] = { 0.0f, 0.0f, 0.0f }; /* updated via audio_update */
                    audio_play_at(bp.block == BLOCK_AIR ? SFX_BLOCK_BREAK
                                                        : SFX_BLOCK_PLACE,
                                  pos, listener);
                }
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
                 * the implied body (INVENTORY_NET_SLOT_SIZE bytes per slot). */
                uint8_t declared_slots = ((const uint8_t*)msg->data)[8];
                if (declared_slots > INVENTORY_NET_SLOTS) declared_slots = INVENTORY_NET_SLOTS;
                if ((size_t)msg->len >=
                        (size_t)(8 + 1 + declared_slots * INVENTORY_NET_SLOT_SIZE)) {
                    InventoryPacket ip;
                    net_read_inventory(msg->data, &ip);
                    int prev_selected = c->inventory.selected;
                    inventory_init(&c->inventory);
                    c->inventory.selected = prev_selected;  /* preserve focus */
                    for (uint8_t i = 0; i < ip.slot_count && i < INVENTORY_SLOTS; i++) {
                        ItemId it = (ItemId)ip.slots[i].item;
                        if (!item_is_block(it) && !item_is_tool(it) &&
                            !item_is_material(it) && !item_is_armor(it))
                            continue;                       /* ignore garbage ids */
                        c->inventory.slots[i].item       = it;
                        c->inventory.slots[i].count      = ip.slots[i].count;
                        c->inventory.slots[i].durability = ip.slots[i].durability;
                    }
                }
            }

        } else if (type == PKT_PLAYER_HEALTH && c->state == CLIENT_CONNECTED) {
            PacketHeader h; size_t off = 0; net_read_header(msg->data, &off, &h);
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new && (size_t)msg->len >= 10) {
                uint8_t hp, fl, food, air;
                net_read_player_health(msg->data, (size_t)msg->len,
                                       &h, &hp, &fl, &food, &air);
                /* SFX hook: a drop in health means the local player took
                 * damage. (Death is handled below, which also plays nothing
                 * extra — the hurt cue already fired here.) */
                if ((int16_t)hp < c->health)
                    audio_play(SFX_HURT);
                c->health = (int16_t)hp;
                c->food   = food;
                c->air    = air;
                hud_set_survival(food, air);
                if ((fl & MOB_HEALTH_FLAG_DIED) && g_death_cb) {
                    c->health = PLAYER_MAX_HEALTH;
                    c->food   = NET_MAX_FOOD;
                    c->air    = NET_MAX_AIR;
                    hud_set_survival(NET_MAX_FOOD, NET_MAX_AIR);
                    g_death_cb(g_death_user);
                }
            }

        } else if (type == PKT_ARMOR && c->state == CLIENT_CONNECTED) {
            PacketHeader h; size_t off = 0; net_read_header(msg->data, &off, &h);
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new) {
                uint16_t worn[ARMOR_NET_SLOTS]; uint8_t pts;
                net_read_armor(msg->data, (size_t)msg->len, &h, worn, &pts);
                for (int i = 0; i < ARMOR_SLOT_COUNT; i++)
                    c->armor[i] = (ItemId)worn[i];
                c->armor_points = pts;
                hud_set_armor((const ItemId*)c->armor, pts);
            }

        } else if (type == PKT_CHUNK_DATA && c->state == CLIENT_CONNECTED) {
            PacketHeader h; size_t off = 0; net_read_header(msg->data, &off, &h);
            /* Each fragment is an ordinary reliable packet: ack it (and dedup
             * via is_new so a retransmitted fragment isn't counted twice). */
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new && (size_t)msg->len >= CHUNK_DATA_FRAG_PREFIX) {
                uint16_t msg_id, index, total;
                net_read_chunk_data_frag_hdr(msg->data, &msg_id, &index, &total);
                size_t flen = (size_t)msg->len - CHUNK_DATA_FRAG_PREFIX;

                if (total >= 1 && total <= CHUNK_DATA_FRAG_MAX
                    && index < total && flen <= CHUNK_DATA_FRAG_BYTES) {
                    /* New column (or first fragment) resets the reassembly. */
                    if (!c->chunk_reasm.active || c->chunk_reasm.msg_id != msg_id
                        || c->chunk_reasm.total != total) {
                        memset(&c->chunk_reasm, 0, sizeof(c->chunk_reasm));
                        c->chunk_reasm.active = true;
                        c->chunk_reasm.msg_id = msg_id;
                        c->chunk_reasm.total  = total;
                    }
                    size_t doff = (size_t)index * CHUNK_DATA_FRAG_BYTES;
                    if (doff + flen <= sizeof(c->chunk_reasm.data)) {
                        memcpy(c->chunk_reasm.data + doff,
                               msg->data + CHUNK_DATA_FRAG_PREFIX, flen);
                        if (!c->chunk_reasm.got[index]) {
                            c->chunk_reasm.got[index] = 1;
                            c->chunk_reasm.received++;
                        }
                        /* Last fragment fixes the total length (earlier ones are
                         * full CHUNK_DATA_FRAG_BYTES). */
                        if (index == (uint16_t)(total - 1))
                            c->chunk_reasm.total_len = doff + flen;

                        if (c->chunk_reasm.received == c->chunk_reasm.total) {
                            int32_t cx, cz;
                            uint8_t blocks[16 * 256 * 16];
                            if (chunkwire_decode_chunk(c->chunk_reasm.data,
                                                       c->chunk_reasm.total_len,
                                                       &cx, &cz,
                                                       blocks, sizeof blocks)) {
                                if (g_chunk_cb)
                                    g_chunk_cb(cx, cz, blocks, sizeof blocks,
                                               g_chunk_user);
                            } else {
                                fprintf(stderr, "[client] PKT_CHUNK_DATA decode "
                                        "failed (%zu body bytes)\n",
                                        c->chunk_reasm.total_len);
                            }
                            c->chunk_reasm.active = false;
                        }
                    }
                }
            }

        } else if (type == PKT_CHUNK_UNLOAD && c->state == CLIENT_CONNECTED) {
            PacketHeader h; size_t off = 0; net_read_header(msg->data, &off, &h);
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new && (size_t)msg->len >= HEADER_WIRE_SIZE + 8) {
                int32_t cx, cz;
                net_read_chunk_unload(msg->data, &h, &cx, &cz);
                if (g_chunk_unload_cb) g_chunk_unload_cb(cx, cz, g_chunk_unload_user);
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

    /* Pump the audio engine once per frame (client_poll is the per-frame drain
     * point). Null backend makes this a cheap no-op; a real backend refills its
     * device buffer and advances the procedural music here. */
    audio_update(NULL);

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
