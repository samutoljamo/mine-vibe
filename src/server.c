#include "server.h"
#include "net.h"
#include "net_thread.h"
#include "platform_thread.h"
#include "inventory.h"
#include "raycast.h"   /* for block_face_offset / FACE_PZ */
#include "gameplay.h"  /* for MAX_REACH */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

/* server.c is the first translation unit that pulls in both the inventory
 * model and the inventory wire format, so this is where we assert the
 * cross-header invariant. */
_Static_assert(INVENTORY_SLOTS == INVENTORY_NET_SLOTS,
    "wire format and inventory model must agree on slot count");

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
        c->active         = true;
        c->addr           = *addr;
        c->player_id      = pid;
        c->last_recv_time = net_time();
        reliable_init(&c->reliable);
        inventory_init(&c->inventory);
        return c;
    }
    return NULL;
}

static void send_reliable(Server* s, ServerClient* c,
                            const uint8_t* data, uint16_t len)
{
    uint16_t ack, bits;
    reliable_fill_ack(&c->reliable, &ack, &bits);
    uint8_t buf[RELIABLE_MAX_PAYLOAD];
    if (len > RELIABLE_MAX_PAYLOAD) return;
    memcpy(buf, data, len);
    /* Patch the wire seq (header bytes 2-3) with the value reliable_send
     * is about to assign. Packet builders here leave header.seq=0; without
     * this every reliable broadcast goes out with seq=0 and the client
     * dedupes all but the first as duplicates. */
    uint16_t seq = c->reliable.next_seq;
    buf[2] = (uint8_t)(seq & 0xFF);
    buf[3] = (uint8_t)(seq >> 8);
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
    PacketHeader h = { .type = PKT_PLAYER_JOIN, .player_id = new_client->player_id };
    net_write_header(buf, &off, &h);
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        if (!s->clients[i].active) continue;
        if (s->clients[i].player_id == new_client->player_id) continue;
        send_reliable(s, &s->clients[i], buf, (uint16_t)off);
    }
}

static void broadcast_player_leave(Server* s, uint8_t pid)
{
    uint8_t buf[HEADER_WIRE_SIZE];
    size_t off = 0;
    PacketHeader h = { .type = PKT_PLAYER_LEAVE, .player_id = pid };
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
    if (find_client_by_addr(s, addr)) return;

    ServerClient* c = alloc_client(s, addr);
    if (!c) {
        printf("[server] connection refused: server full\n");
        return;
    }
    printf("[server] player %d connected\n", c->player_id);

    uint8_t buf[HEADER_WIRE_SIZE];
    size_t off = 0;
    PacketHeader h = { .type = PKT_CONNECT_ACCEPT, .player_id = c->player_id };
    net_write_header(buf, &off, &h);
    send_reliable(s, c, buf, (uint16_t)off);

    broadcast_player_join(s, c);
}

static void handle_position(Server* s, ServerClient* c,
                              const uint8_t* data, int len)
{
    if (len < 32) return;
    PositionPacket p;
    net_read_position(data, &p);

    reliable_on_recv(&c->reliable, p.header.seq, p.header.ack, p.header.ack_bits);

    /* Drop stale/duplicate packets */
    if (p.tick <= c->last_tick && c->last_recv_time > 0) return;
    c->last_tick      = p.tick;
    c->last_recv_time = net_time();
    c->x     = p.x;
    c->y     = p.y;
    c->z     = p.z;
    c->yaw   = p.yaw;
    c->pitch = p.pitch;
    c->position_received = true;
}

/* ------------------------------------------------------------------ */
/*  Block edit helpers                                                 */
/* ------------------------------------------------------------------ */

static void server_broadcast_block_change(Server* s, int x, int y, int z, BlockID b)
{
    BlockChangePacket p = {
        .header = { .type = PKT_BLOCK_CHANGE, .player_id = 0 },
        .x = x, .y = y, .z = z, .block = (uint8_t)b,
    };
    uint8_t buf[64];
    size_t  len = net_write_block_change(buf, &p);
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        ServerClient* c = &s->clients[i];
        if (!c->active) continue;
        send_reliable(s, c, buf, (uint16_t)len);
    }
}

static void server_send_inventory(Server* s, ServerClient* c)
{
    InventoryPacket p = {
        .header = { .type = PKT_INVENTORY, .player_id = 0 },
        .slot_count = INVENTORY_NET_SLOTS,
    };
    for (int i = 0; i < INVENTORY_NET_SLOTS; i++) {
        p.slots[i].block = (uint8_t)c->inventory.slots[i].block;
        p.slots[i].count = c->inventory.slots[i].count;
    }
    uint8_t buf[64];
    size_t  len = net_write_inventory(buf, &p);
    send_reliable(s, c, buf, (uint16_t)len);
}

static void handle_block_break(Server* s, ServerClient* c,
                                const uint8_t* data, size_t len)
{
    /* Header is 8 bytes; payload is 12 (xyz) + 1 (block) = 13. */
    if (len < 8 + 13) return;
    BlockBreakPacket p;
    net_read_block_break(data, &p);

    bool is_new = reliable_on_recv(&c->reliable,
                                    p.header.seq,
                                    p.header.ack,
                                    p.header.ack_bits);
    if (!is_new) return;

    c->last_recv_time = net_time();

    if (!c->position_received) return;

    /* Reach check: distance from eye position to block centre (matches
     * client raycast origin). */
    float dx = (p.x + 0.5f) - c->x;
    float dy = (p.y + 0.5f) - (c->y + PLAYER_EYE_H);
    float dz = (p.z + 0.5f) - c->z;
    if (dx*dx + dy*dy + dz*dz > MAX_REACH * MAX_REACH) return;

    /* Server doesn't own a world to validate replaceability against, so we
     * trust the client's claimed block ID, but reject obvious nonsense. */
    BlockID b = (BlockID)p.block;
    if (b == BLOCK_AIR || b == BLOCK_WATER || b == BLOCK_BEDROCK || b >= BLOCK_COUNT) return;

    uint8_t leftover = inventory_add(&c->inventory, b, 1);
    if (leftover != 0) {
        /* No room — refuse the break. Don't broadcast, don't send inventory. */
        return;
    }
    server_broadcast_block_change(s, p.x, p.y, p.z, BLOCK_AIR);
    server_send_inventory(s, c);
}

static void handle_block_place(Server* s, ServerClient* c,
                                const uint8_t* data, size_t len)
{
    /* Header 8 + payload 12 (xyz) + 1 (face) + 1 (slot) = 22 wire bytes. */
    if (len < 8 + 14) return;
    BlockPlacePacket p;
    net_read_block_place(data, &p);

    bool is_new = reliable_on_recv(&c->reliable,
                                    p.header.seq,
                                    p.header.ack,
                                    p.header.ack_bits);
    if (!is_new) return;

    c->last_recv_time = net_time();

    if (!c->position_received) return;

    if (p.face > FACE_PZ || p.slot >= INVENTORY_SLOTS) return;
    if (c->inventory.slots[p.slot].count == 0) return;

    int dx, dy, dz;
    block_face_offset((BlockFace)p.face, &dx, &dy, &dz);
    int tx = p.x + dx, ty = p.y + dy, tz = p.z + dz;

    /* Reach check on the target cell from eye height (matches client raycast). */
    float fx = (tx + 0.5f) - c->x;
    float fy = (ty + 0.5f) - (c->y + PLAYER_EYE_H);
    float fz = (tz + 0.5f) - c->z;
    if (fx*fx + fy*fy + fz*fz > MAX_REACH * MAX_REACH) return;

    /* Don't trap the placing player inside their own block. */
    {
        float pminx = c->x - PLAYER_HALF_W, pmaxx = c->x + PLAYER_HALF_W;
        float pminy = c->y,                  pmaxy = c->y + PLAYER_HEIGHT;
        float pminz = c->z - PLAYER_HALF_W, pmaxz = c->z + PLAYER_HALF_W;
        float bminx = (float)tx,             bmaxx = (float)tx + 1.0f;
        float bminy = (float)ty,             bmaxy = (float)ty + 1.0f;
        float bminz = (float)tz,             bmaxz = (float)tz + 1.0f;
        if (pmaxx > bminx && pminx < bmaxx &&
            pmaxy > bminy && pminy < bmaxy &&
            pmaxz > bminz && pminz < bmaxz) return;
    }

    BlockID b = c->inventory.slots[p.slot].block;
    if (b == BLOCK_AIR || b >= BLOCK_COUNT) return;   /* should be impossible, but don't broadcast garbage */
    inventory_consume(&c->inventory, p.slot);
    server_broadcast_block_change(s, tx, ty, tz, b);
    server_send_inventory(s, c);
}

/* ------------------------------------------------------------------ */
/*  World anchor                                                       */
/* ------------------------------------------------------------------ */

/* Anchor = first active client's position; (0,0,0) until someone connects. */
static void server_anchor(Server* s, vec3 out) {
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        if (s->clients[i].active && s->clients[i].position_received) {
            out[0] = s->clients[i].x; out[1] = s->clients[i].y; out[2] = s->clients[i].z;
            return;
        }
    }
    out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f;
}

/* ------------------------------------------------------------------ */
/*  Server tick                                                        */
/* ------------------------------------------------------------------ */

static void server_tick(Server* s, int tick_num)
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
            if      (type == PKT_POSITION)     handle_position(s, c, msg->data, msg->len);
            else if (type == PKT_DISCONNECT)   disconnect_client(s, c);
            else if (type == PKT_BLOCK_BREAK)  handle_block_break(s, c, msg->data, (size_t)msg->len);
            else if (type == PKT_BLOCK_PLACE)  handle_block_place(s, c, msg->data, (size_t)msg->len);
        }
        free(msg);
    }

    /* Only remaining steps at 20 Hz */
    if (tick_num % 1 != 0) return; /* always true — placeholder for future rate div */

    /* Compute once per tick; shared by terrain streaming (here) and mob
     * simulation/spawning (Task 5). */
    vec3 anchor;
    server_anchor(s, anchor);

    /* Stream terrain around the anchor so mobs have ground to walk on. */
    if (s->world) {
        world_update(s->world, /*bp=*/NULL, anchor);
    }

    /* 2. Timeout detection */
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        ServerClient* c = &s->clients[i];
        if (!c->active) continue;
        if (now - c->last_recv_time > SERVER_TIMEOUT_SEC)
            disconnect_client(s, c);
    }

    /* 3. Retransmit reliable messages */
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        ServerClient* c = &s->clients[i];
        if (!c->active) continue;
        reliable_tick(&c->reliable, s->net->fd, &c->addr);
    }

    /* 4. Broadcast world state to every client */
    NetPlayerState players[SERVER_MAX_CLIENTS];
    uint8_t count = 0;
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        ServerClient* c = &s->clients[i];
        if (!c->active) continue;
        players[count].player_id = c->player_id;
        players[count].x         = c->x;
        players[count].y         = c->y;
        players[count].z         = c->z;
        players[count].yaw       = c->yaw;
        players[count].pitch     = c->pitch;
        count++;
    }

    uint8_t buf[NET_MAX_PACKET];
    PacketHeader hdr = { .type = PKT_WORLD_STATE, .player_id = 0 };
    size_t len = net_write_world_state(buf, &hdr, players, count);
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        if (!s->clients[i].active) continue;
        net_thread_push_outbound(s->net, buf, (int)len, &s->clients[i].addr);
    }
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
    s.running     = true;

    s.seed  = seed;
    s.world = world_create_headless(seed, 8 /* SERVER_MOB_RENDER_DIST */);

    printf("[server] listening on port %d (max %d clients)\n", port, s.max_clients);

    double last = net_time();
    const double tick_dt = 1.0 / SERVER_TICK_RATE;
    double accum = 0.0;
    int tick_num = 0;

    while (s.running) {
        double now = net_time();
        accum += now - last;
        last   = now;
        if (accum > 0.2) accum = 0.2;

        while (accum >= tick_dt) {
            server_tick(&s, tick_num++);
            accum -= tick_dt;
        }

        pt_sleep_ms(1);
    }

    if (s.world) world_destroy(s.world);
    net_thread_stop(&nt);
    net_socket_close(fd);
}
