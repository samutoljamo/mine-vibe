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

/* Live particle pool of the active client, published by client_init so the
 * renderer can read it via client_active_particles() without a Client pointer
 * being threaded through main.c. Cleared by client_destroy. */
static ParticleSystem* g_active_particles = NULL;

const ParticleSystem* client_active_particles(void)
{
    return g_active_particles;
}

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

/* Rate-limited malformed-packet drop log (a hostile/buggy server could spam
 * truncated datagrams; cap to one line/sec and fold the rest into a count). */
static void client_drop_malformed(const char* what)
{
    static double next_report = 0.0;
    static unsigned long suppressed = 0;
    double now = net_time();
    if (now >= next_report) {
        if (suppressed)
            fprintf(stderr, "[client] dropped malformed %s packet "
                            "(+%lu more suppressed)\n", what, suppressed);
        else
            fprintf(stderr, "[client] dropped malformed %s packet\n", what);
        suppressed = 0;
        next_report = now + 1.0;
    } else {
        suppressed++;
    }
}

/* Face -> neighbour-cell offset, matching the BlockFace enum order used on the
 * wire (FACE_NX,PX,NY,PY,NZ,PZ) and applied identically by the server when it
 * places. Inlined here (rather than #include "raycast.h", which pulls in cglm +
 * world.h) so the predicted-place SFX can target the cell that actually changes
 * without adding heavy build/test deps to client.c. */
static void client_face_offset(uint8_t face, int* dx, int* dy, int* dz)
{
    *dx = 0; *dy = 0; *dz = 0;
    switch (face) {
        case 0: *dx = -1; break;  /* FACE_NX */
        case 1: *dx =  1; break;  /* FACE_PX */
        case 2: *dy = -1; break;  /* FACE_NY */
        case 3: *dy =  1; break;  /* FACE_PY */
        case 4: *dz = -1; break;  /* FACE_NZ */
        case 5: *dz =  1; break;  /* FACE_PZ */
        default: break;           /* unknown face: predict at the hit cell */
    }
}

/* ---- Predicted block-SFX dedup (mine-vibe-5kz) --------------------------- *
 *
 * Record a predicted edit so the server's echoing PKT_BLOCK_CHANGE for our own
 * action doesn't re-play the sound we already played locally at send time. */
static void client_predict_sfx(Client* c, int x, int y, int z, uint8_t is_air)
{
    int n = c->predicted_sfx_count;
    int slots = (int)(sizeof c->predicted_sfx / sizeof c->predicted_sfx[0]);
    int i;
    if (n < slots) {
        i = n;
        c->predicted_sfx_count = n + 1;
    } else {
        /* Ring full: drop the oldest by shifting down (tiny array, cheap). */
        for (int k = 1; k < slots; k++) c->predicted_sfx[k - 1] = c->predicted_sfx[k];
        i = slots - 1;
    }
    c->predicted_sfx[i].x = x;
    c->predicted_sfx[i].y = y;
    c->predicted_sfx[i].z = z;
    c->predicted_sfx[i].is_air = is_air ? 1u : 0u;
}

/* If (x,y,z)/is_air matches a recorded prediction, consume it and return true
 * (the caller should then SKIP the SFX — we already played it locally). */
static bool client_consume_predicted_sfx(Client* c, int x, int y, int z, uint8_t is_air)
{
    for (int i = 0; i < c->predicted_sfx_count; i++) {
        if (c->predicted_sfx[i].x == x && c->predicted_sfx[i].y == y &&
            c->predicted_sfx[i].z == z &&
            c->predicted_sfx[i].is_air == (is_air ? 1u : 0u)) {
            /* Remove entry i by swapping the last one into its place. */
            c->predicted_sfx[i] = c->predicted_sfx[c->predicted_sfx_count - 1];
            c->predicted_sfx_count--;
            return true;
        }
    }
    return false;
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
    chunk_reasm_init(&c->chunk_reasm);
    c->last_server_recv_time = 0.0;
    c->last_keepalive_time   = 0.0;
    /* Start at noon so the sky is daylit before the first PKT_WORLD_STATE. */
    c->world_ticks = DAY_LENGTH_TICKS / 4;
    c->world_ticks_recv_time = 0.0;

    /* Bring up the audio engine from the client (the only place that owns the
     * server-event stream that drives SFX). Safe no-op with the null backend
     * and idempotent, so this is fine even with multiple clients in a test. */
    audio_init();
    audio_set_music(true);

    /* Particle pool: deterministic seed (the visual effect doesn't need
     * cryptographic randomness). Publish it for the renderer to read. */
    particle_system_init(&c->particles, 0xC0FFEEu);
    c->particles_last_update = net_time();
    c->have_local_pos = false;
    c->local_x = c->local_y = c->local_z = 0.0f;
    g_active_particles = &c->particles;
}

void client_destroy(Client* c) {
    if (g_active_particles == &c->particles) g_active_particles = NULL;
    audio_shutdown();
}

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

WeatherState client_get_weather(const Client* c)
{
    /* rng stays server-side; only {kind,time_left} are synced. */
    WeatherState w = { .kind = c->weather_kind,
                       .time_left = c->weather_time_left,
                       .rng = 0 };
    return w;
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
    /* Cache the local position for the rain-render step (client_poll spawns
     * weather particles centered here). Done before the connected-state guard so
     * the camera position is current even on the frame we connect. */
    c->local_x = x;
    c->local_y = y;
    c->local_z = z;
    c->have_local_pos = true;

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

    /* Predicted local SFX (mine-vibe-5kz): play the break sound immediately at
     * the edit position instead of waiting for the server's PKT_BLOCK_CHANGE
     * round-trip. Record the prediction so that confirming broadcast doesn't
     * double-play for our own action. 2D play here (listener is at the player). */
    client_predict_sfx(c, x, y, z, /*is_air=*/1);
    audio_play(SFX_BLOCK_BREAK);
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

    /* Predicted local SFX (mine-vibe-5kz): play the place sound immediately
     * rather than waiting for the server round-trip. The cell that actually
     * changes is the neighbour of (x,y,z) across `face` (same offset the server
     * applies), so predict at THAT cell to match the confirming broadcast. */
    {
        int dx = 0, dy = 0, dz = 0;
        client_face_offset(face, &dx, &dy, &dz);
        client_predict_sfx(c, x + dx, y + dy, z + dz, /*is_air=*/0);
    }
    audio_play(SFX_BLOCK_PLACE);
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

/* Send an unreliable header-only keepalive to the server. Holds the NAT pinhole
 * open and refreshes the server's per-client last_recv_time while we are idle. */
static void client_send_keepalive(Client* c) {
    if (c->state != CLIENT_CONNECTED) return;
    PacketHeader h = { .type = PKT_KEEPALIVE, .player_id = c->local_player_id };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    uint8_t buf[HEADER_WIRE_SIZE];
    size_t len = net_write_keepalive(buf, &h);
    net_thread_push_outbound(c->net, buf, (int)len, &c->server_addr);
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

void client_send_eat(Client* c, uint8_t slot) {
    if (c->state != CLIENT_CONNECTED) return;
    PacketHeader h = { .type = PKT_EAT, .player_id = c->local_player_id,
                       .seq = c->tick++ };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    uint8_t buf[16];
    size_t len = net_write_eat(buf, &h, slot);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr, buf, (uint16_t)len);
}

void client_send_container_open(Client* c, int x, int y, int z) {
    if (c->state != CLIENT_CONNECTED) return;
    PacketHeader h = { .type = PKT_CONTAINER_OPEN, .player_id = c->local_player_id,
                       .seq = c->tick++ };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    uint8_t buf[32];
    size_t len = net_write_container_open(buf, &h, x, y, z);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr, buf, (uint16_t)len);
}

void client_send_container_close(Client* c, int x, int y, int z) {
    c->container_open = false;
    if (c->state != CLIENT_CONNECTED) return;
    PacketHeader h = { .type = PKT_CONTAINER_CLOSE, .player_id = c->local_player_id,
                       .seq = c->tick++ };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    uint8_t buf[32];
    size_t len = net_write_container_close(buf, &h, x, y, z);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr, buf, (uint16_t)len);
}

void client_send_container_action(Client* c, int x, int y, int z,
                                  uint8_t slot, uint8_t dir, uint8_t count) {
    if (c->state != CLIENT_CONNECTED) return;
    PacketHeader h = { .type = PKT_CONTAINER_ACTION, .player_id = c->local_player_id,
                       .seq = c->tick++ };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    uint8_t buf[32];
    size_t len = net_write_container_action(buf, &h, x, y, z, slot, dir, count);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr, buf, (uint16_t)len);
}

int client_poll(Client* c)
{
    int state_packets = 0;
    NetMsg* msg;
    while ((msg = net_thread_pop_inbound(c->net)) != NULL) {
        if (msg->len < 1) { free(msg); continue; }
        uint8_t type = msg->data[0];

        /* Any inbound datagram from the server proves it is still alive (resets
         * the dead-server watchdog). recv_time was stamped by the net thread. */
        c->last_server_recv_time = msg->recv_time;

        if (type == PKT_CONNECT_ACCEPT && c->state == CLIENT_CONNECTING) {
            if (msg->len < HEADER_WIRE_SIZE) {
                client_drop_malformed("connect-accept"); free(msg); continue;
            }
            PacketHeader h;
            size_t off = 0;
            net_read_header(msg->data, &off, &h);
            reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            c->local_player_id = h.player_id;
            c->state           = CLIENT_CONNECTED;
            printf("[client] connected as player %d\n", c->local_player_id);

        } else if (type == PKT_WORLD_STATE && c->state == CLIENT_CONNECTED) {
            /* Parse the whole packet through a bounds-checked cursor: every
             * field (incl. each player entry and the trailing world clock) is
             * length-checked, so a truncated/hostile snapshot drops cleanly. */
            NetReader r = net_reader_init(msg->data, (size_t)msg->len);
            PacketHeader hdr;
            net_reader_header(&r, &hdr);

            /* Per-stream broadcast seq (protocol v3): drop reordered/late
             * datagrams so a stale snapshot never overwrites a newer one. */
            uint32_t bseq = net_reader_u32(&r);
            if (!net_reader_ok(&r)) {
                client_drop_malformed("world-state"); free(msg); continue;
            }
            if (c->world_state_seq_valid &&
                !seq_is_newer(bseq, c->world_state_seq)) {
                free(msg);
                continue;
            }

            uint8_t count = net_reader_u8(&r);
            if (!net_reader_ok(&r)) {
                client_drop_malformed("world-state"); free(msg); continue;
            }

            /* Stage all player entries, validating each against the cursor
             * BEFORE committing the seq, so a truncated body neither over-reads
             * nor advances world_state_seq past a packet we couldn't apply. */
            ClientPlayerSnapshot staged[256];
            int nstaged = 0;
            for (int i = 0; i < count; i++) {
                NetPlayerState ps;
                net_reader_player_state(&r, &ps);
                if (!net_reader_ok(&r)) break;
                if (ps.player_id == c->local_player_id) continue;  /* our own pos */
                staged[nstaged].player_id = ps.player_id;
                staged[nstaged].x = ps.x; staged[nstaged].y = ps.y;
                staged[nstaged].z = ps.z; staged[nstaged].yaw = ps.yaw;
                staged[nstaged].pitch = ps.pitch;
                staged[nstaged].recv_time = msg->recv_time;
                nstaged++;
            }
            /* Trailing day/night clock (protocol v3). */
            uint32_t world_ticks = net_reader_u32(&r);
            if (!net_reader_ok(&r)) {
                client_drop_malformed("world-state"); free(msg); continue;
            }

            /* Commit: this packet is being applied, so it becomes the newest. */
            c->world_state_seq       = bseq;
            c->world_state_seq_valid = true;

            if (g_snap_cb)
                for (int i = 0; i < nstaged; i++)
                    g_snap_cb(&staged[i], g_snap_user);

            /* Re-anchor every packet; the renderer extrapolates from here so
             * missed packets keep the sky moving and a u32 wrap (or backward
             * jump) simply re-anchors harmlessly. */
            c->world_ticks           = world_ticks;
            c->world_ticks_recv_time = msg->recv_time;
            state_packets++;

        } else if (type == PKT_MOB_STATE && c->state == CLIENT_CONNECTED) {
            NetReader r = net_reader_init(msg->data, (size_t)msg->len);
            PacketHeader hdr; net_reader_header(&r, &hdr);

            /* Per-stream broadcast seq (protocol v3): drop stale reordered
             * snapshots before applying (see seq_is_newer in net.h). */
            uint32_t bseq = net_reader_u32(&r);
            if (!net_reader_ok(&r)) {
                client_drop_malformed("mob-state"); free(msg); continue;
            }
            if (c->mob_state_seq_valid &&
                !seq_is_newer(bseq, c->mob_state_seq)) {
                free(msg);
                continue;
            }

            uint16_t count = net_reader_u16(&r);
            if (!net_reader_ok(&r) || count > MOB_MAX) {
                client_drop_malformed("mob-state"); free(msg); continue;
            }

            /* Stage all entries through the cursor first; only commit the seq if
             * the whole body was present (no over-read on a truncated tail). */
            ClientMobSnapshot snaps[MOB_MAX];
            for (uint16_t i = 0; i < count; i++) {
                NetMobState m; net_reader_mob_state_entry(&r, &m);
                snaps[i].id = m.id; snaps[i].type = m.type;
                snaps[i].x = m.x; snaps[i].y = m.y; snaps[i].z = m.z;
                snaps[i].yaw = m.yaw; snaps[i].health = m.health;
            }
            if (!net_reader_ok(&r)) {
                client_drop_malformed("mob-state"); free(msg); continue;
            }

            c->mob_state_seq       = bseq;
            c->mob_state_seq_valid = true;
            if (g_mobs_cb) g_mobs_cb(snaps, (int)count, msg->recv_time, g_mobs_user);

        } else if (type == PKT_PLAYER_JOIN || type == PKT_PLAYER_LEAVE) {
            if (msg->len < HEADER_WIRE_SIZE) {
                client_drop_malformed("player-join/leave"); free(msg); continue;
            }
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
            BlockChangePacket bp;
            if (!net_parse_block_change(msg->data, (size_t)msg->len, &bp)) {
                client_drop_malformed("block-change"); free(msg); continue;
            }
            PacketHeader h = bp.header;
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new) {
                /* SFX hook: a change to AIR is a break, anything else a place.
                 * Positional so distant edits by other players are quieter.
                 * For our OWN action we already played the sound locally at send
                 * time (predicted, mine-vibe-5kz); consume the prediction and
                 * skip here so we don't double-play. Other players' edits never
                 * match the ring, so they still sound. */
                if (!client_consume_predicted_sfx(c, bp.x, bp.y, bp.z,
                                                  bp.block == BLOCK_AIR)) {
                    float pos[3]      = { (float)bp.x, (float)bp.y, (float)bp.z };
                    float listener[3] = { 0.0f, 0.0f, 0.0f }; /* updated via audio_update */
                    audio_play_at(bp.block == BLOCK_AIR ? SFX_BLOCK_BREAK
                                                        : SFX_BLOCK_PLACE,
                                  pos, listener);
                }
                /* Visual particle burst at the edit cell (centered on the block).
                 * On a break (-> AIR) we can't see the OLD block id in this
                 * packet, so debris uses a neutral grey-brown tint; tinting a
                 * break by the removed block's colour needs the pre-edit world
                 * state (only known where pending changes are applied) and is
                 * deferred. On a place we DO have the new block id, so the puff
                 * is tinted by its representative colour. */
                {
                    float px = (float)bp.x + 0.5f;
                    float py = (float)bp.y + 0.5f;
                    float pz = (float)bp.z + 0.5f;
                    if (bp.block == BLOCK_AIR) {
                        particle_emit_block_break(&c->particles, px, py, pz,
                                                  0.55f, 0.50f, 0.42f);
                    } else {
                        uint8_t cr, cg, cb;
                        block_representative_color((BlockID)bp.block, &cr, &cg, &cb);
                        particle_emit_block_break(&c->particles, px, py, pz,
                                                  cr / 255.0f, cg / 255.0f,
                                                  cb / 255.0f);
                    }
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
            /* Bounds-checked parse: clamps slot_count and rejects a body that
             * doesn't cover the declared slots (no over-read on truncation). */
            InventoryPacket ip;
            if (!net_parse_inventory(msg->data, (size_t)msg->len, &ip)) {
                client_drop_malformed("inventory"); free(msg); continue;
            }
            PacketHeader h = ip.header;
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new) {
                int prev_selected = c->inventory.selected;
                inventory_init(&c->inventory);
                c->inventory.selected = prev_selected;  /* preserve focus */
                for (uint8_t i = 0; i < ip.slot_count && i < INVENTORY_SLOTS; i++) {
                    ItemId it = (ItemId)ip.slots[i].item;
                    if (!item_is_block(it) && !item_is_tool(it) &&
                        !item_is_material(it) && !item_is_armor(it) &&
                        !item_is_food(it))
                        continue;                       /* ignore garbage ids */
                    c->inventory.slots[i].item       = it;
                    c->inventory.slots[i].count      = ip.slots[i].count;
                    c->inventory.slots[i].durability = ip.slots[i].durability;
                }
            }

        } else if (type == PKT_PLAYER_HEALTH && c->state == CLIENT_CONNECTED) {
            PacketHeader h; uint8_t hp, fl, food, air;
            if (!net_parse_player_health(msg->data, (size_t)msg->len,
                                         &h, &hp, &fl, &food, &air)) {
                client_drop_malformed("player-health"); free(msg); continue;
            }
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new) {
                /* SFX hook: a drop in health means the local player took
                 * damage — play the hurt grunt and flash the screen edges red.
                 * (Death is handled below, which plays nothing extra — the hurt
                 * cue already fired here.) */
                if ((int16_t)hp < c->health) {
                    audio_play(SFX_HURT);
                    hud_trigger_hurt_flash();
                }
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

        } else if (type == PKT_KNOCKBACK && c->state == CLIENT_CONNECTED) {
            PacketHeader h; float dx, dy, dz;
            if (!net_parse_knockback(msg->data, (size_t)msg->len,
                                     &h, &dx, &dy, &dz)) {
                client_drop_malformed("knockback"); free(msg); continue;
            }
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new) {
                /* Accumulate the shove; main.c drains it into a decaying nudge on
                 * the local player position next frame. */
                c->kb_dx += dx;
                c->kb_dy += dy;
                c->kb_dz += dz;
                c->kb_pending = true;
            }

        } else if (type == PKT_WEATHER && c->state == CLIENT_CONNECTED) {
            PacketHeader h; uint8_t kind; float time_left;
            if (!net_parse_weather(msg->data, (size_t)msg->len,
                                   &h, &kind, &time_left)) {
                client_drop_malformed("weather"); free(msg); continue;
            }
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new) {
                /* Store the authoritative weather; the rain-render step (a
                 * separate ticket) reads it via client_get_weather(). */
                if (kind > (uint8_t)WEATHER_STORM) kind = (uint8_t)WEATHER_STORM;
                c->weather_kind      = (WeatherKind)kind;
                c->weather_time_left = time_left;
            }

        } else if (type == PKT_ARMOR && c->state == CLIENT_CONNECTED) {
            if (msg->len < HEADER_WIRE_SIZE) {
                client_drop_malformed("armor"); free(msg); continue;
            }
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
            PacketHeader h; uint16_t msg_id, index, total;
            if (!net_parse_chunk_data_frag_hdr(msg->data, (size_t)msg->len,
                                               &h, &msg_id, &index, &total)) {
                client_drop_malformed("chunk-data"); free(msg); continue;
            }
            /* Each fragment is an ordinary reliable packet: ack it (and dedup
             * via is_new so a retransmitted fragment isn't counted twice). */
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new) {
                size_t flen = (size_t)msg->len - CHUNK_DATA_FRAG_PREFIX;

                /* Feed the fragment into the per-msg_id reassembly ring. Several
                 * columns can be in flight at once, so an interleaved/retransmitted
                 * fragment of an older column reassembles independently of a newer
                 * one (mine-vibe-003). The ring validates geometry internally. */
                static uint8_t reasm_body[CHUNK_DATA_FRAG_MAX * CHUNK_DATA_FRAG_BYTES];
                size_t body_len = 0;
                if (chunk_reasm_feed(&c->chunk_reasm, msg_id, index, total,
                                     msg->data + CHUNK_DATA_FRAG_PREFIX, flen,
                                     reasm_body, sizeof reasm_body, &body_len)) {
                    int32_t cx, cz;
                    uint8_t blocks[16 * 256 * 16];
                    if (chunkwire_decode_chunk(reasm_body, body_len, &cx, &cz,
                                               blocks, sizeof blocks)) {
                        if (g_chunk_cb)
                            g_chunk_cb(cx, cz, blocks, sizeof blocks, g_chunk_user);
                    } else {
                        fprintf(stderr, "[client] PKT_CHUNK_DATA decode "
                                "failed (%zu body bytes)\n", body_len);
                    }
                }
            }

        } else if (type == PKT_CHUNK_UNLOAD && c->state == CLIENT_CONNECTED) {
            PacketHeader h; int32_t cx, cz;
            if (!net_parse_chunk_unload(msg->data, (size_t)msg->len, &h, &cx, &cz)) {
                client_drop_malformed("chunk-unload"); free(msg); continue;
            }
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new) {
                if (g_chunk_unload_cb) g_chunk_unload_cb(cx, cz, g_chunk_unload_user);
            }

        } else if (type == PKT_KEEPALIVE) {
            /* Liveness pong from the server. last_server_recv_time was already
             * refreshed above; nothing else to do (acks piggyback the header,
             * but keepalive is sent unreliable so we don't feed reliable_on_recv
             * its seq — it carries no reliable payload). */

        } else if (type == PKT_CONTAINER_STATE && c->state == CLIENT_CONNECTED) {
            /* Store the latest container snapshot. The full interactive
             * container UI is a separate ticket (a4s.6.4); for now we just keep
             * the state so the UI/agent can read it and nothing breaks. */
            PacketHeader h; ContainerStatePacket cs;
            if (!net_parse_container_state(msg->data, (size_t)msg->len, &h, &cs)) {
                client_drop_malformed("container-state"); free(msg); continue;
            }
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new) {
                c->container = cs;
                c->container_open = true;
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

    /* Keepalive + dead-server detection (mine-vibe-6dp). Only while connected. */
    if (c->state == CLIENT_CONNECTED) {
        double now = net_time();

        /* Seed the watchdog on the first poll after connect so a slow first
         * snapshot doesn't trip an immediate false timeout. */
        if (c->last_server_recv_time <= 0.0)
            c->last_server_recv_time = now;

        /* Periodic keepalive: holds the NAT pinhole open and keeps the server's
         * per-client last_recv_time fresh even when we send nothing else. */
        if (now - c->last_keepalive_time >= CLIENT_KEEPALIVE_INTERVAL) {
            c->last_keepalive_time = now;
            client_send_keepalive(c);
        }

        /* Dead-server watchdog: no inbound traffic for too long -> the server is
         * gone (crashed, network partition). Surface a clean disconnect instead
         * of rendering a frozen world forever. */
        if (now - c->last_server_recv_time > CLIENT_SERVER_TIMEOUT_SEC) {
            fprintf(stderr, "[client] server timed out (no traffic for %.0fs); "
                            "disconnecting\n", CLIENT_SERVER_TIMEOUT_SEC);
            c->state = CLIENT_DISCONNECTED;
        }
    }

    /* Integrate the particle pool once per poll. dt is measured against the last
     * poll's wall clock so the sim advances at real time independent of the
     * frame rate; clamp to a sane ceiling so a long stall (loading) doesn't fling
     * debris across the map in one giant step. */
    {
        double pnow = net_time();
        double dt = pnow - c->particles_last_update;
        c->particles_last_update = pnow;
        if (dt < 0.0) dt = 0.0;
        if (dt > 0.1) dt = 0.1;
        if (dt > 0.0) particle_update(&c->particles, (float)dt);

        /* Weather: while it's raining (per the server-synced state) spawn a
         * throttled batch of rain droplets centered on the local player so the
         * weather is visible and follows the camera. particle_emit_rain caps the
         * per-call count and scales it by intensity; clear weather spawns none.
         * Gated on dt so a stalled poll doesn't spam, and on have_local_pos so we
         * don't rain at the origin before the first position is known. */
        if (dt > 0.0 && c->have_local_pos) {
            WeatherState w = client_get_weather(c);
            if (weather_is_raining(&w)) {
                particle_emit_rain(&c->particles,
                                   c->local_x, c->local_y, c->local_z,
                                   weather_rain_intensity(&w));
            }
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
