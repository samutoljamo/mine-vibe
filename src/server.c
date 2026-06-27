#include "server.h"
#include "net.h"
#include "net_thread.h"
#include "platform_thread.h"
#include "inventory.h"
#include "raycast.h"   /* for block_face_offset / FACE_PZ */
#include "gameplay.h"  /* for MAX_REACH */
#include "mob.h"
#include "physics.h"
#include "player.h"      /* GRAVITY, TERMINAL_VEL */
#include "block.h"       /* block_is_solid */
#include "chunk.h"       /* CHUNK_Y */
#include "world.h"
#include <math.h>
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
        c->health = PLAYER_MAX_HEALTH;
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

/* Best-effort unreliable disconnect to an address we have no client for yet
 * (used to reject a connect before allocating a slot). */
static void reject_connect(Server* s, const struct sockaddr_in* addr,
                           uint8_t reason)
{
    uint8_t buf[HEADER_WIRE_SIZE + 1];
    size_t off = 0;
    PacketHeader h = { .type = PKT_DISCONNECT, .player_id = 0 };
    net_write_disconnect(buf, &off, &h, reason);
    net_send(s->net->fd, buf, (int)off, addr);
}

static void handle_connect_request(Server* s, const struct sockaddr_in* addr,
                                   const uint8_t* data, int len)
{
    if (find_client_by_addr(s, addr)) return;

    uint16_t version = net_read_connect_version(data, (size_t)len);
    if (version != NET_PROTOCOL_VERSION) {
        printf("[server] connection refused: protocol v%u != server v%d\n",
               version, NET_PROTOCOL_VERSION);
        reject_connect(s, addr, NET_DISCONNECT_VERSION_MISMATCH);
        return;
    }

    ServerClient* c = alloc_client(s, addr);
    if (!c) {
        printf("[server] connection refused: server full\n");
        reject_connect(s, addr, NET_DISCONNECT_SERVER_FULL);
        return;
    }
    printf("[server] player %d connected (protocol v%u)\n", c->player_id, version);

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

static void handle_mob_attack(Server* s, ServerClient* c,
                              const uint8_t* data, size_t len) {
    if (len < 10) return;
    PacketHeader h; uint16_t mob_id;
    net_read_mob_attack(data, &h, &mob_id);
    bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
    if (!is_new) return;
    c->last_recv_time = net_time();
    if (!c->position_received || !s->world) return;

    /* Rate-limit. */
    double now = net_time();
    if (now - c->last_attack_time < PLAYER_ATTACK_COOLDOWN) return;

    Mob* m = mob_set_get(&s->mobs, mob_id);
    if (!m) return;

    /* Range check from the attacker's eye to the mob centre. */
    float cx = m->position[0], cy = m->position[1] + MOB_HEIGHT * 0.5f, cz = m->position[2];
    float dx = cx - c->x, dy = cy - (c->y + PLAYER_EYE_H), dz = cz - c->z;
    if (dx*dx + dy*dy + dz*dz > (MAX_REACH + 1.0f) * (MAX_REACH + 1.0f)) return;

    c->last_attack_time = now;
    if (mob_combat_apply(&m->health, PLAYER_ATTACK_DAMAGE))
        mob_set_remove(&s->mobs, mob_id);   /* dead → vanishes next broadcast */
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
/*  Mob spawning + simulation                                          */
/* ------------------------------------------------------------------ */

/* Find the surface y at (x,z): topmost solid with two air blocks above.
 * Returns -1 if none (ungenerated/air column). */
static int server_surface_y(World* w, int x, int z) {
    for (int y = CHUNK_Y - 3; y >= 1; y--) {
        if (block_is_solid(world_get_block(w, x, y, z))
            && world_get_block(w, x, y + 1, z) == BLOCK_AIR
            && world_get_block(w, x, y + 2, z) == BLOCK_AIR)
            return y;
    }
    return -1;
}

static void server_try_spawn(Server* s, vec3 anchor) {
    int live = 0;
    for (int i = 0; i < MOB_MAX; i++) if (s->mobs.mobs[i].active) live++;
    if (live >= MOB_CAP) return;

    /* Random point in the spawn ring. rand() is fine server-side. */
    float ang = (float)rand() / (float)RAND_MAX * 6.2831853f;
    float rad = MOB_SPAWN_MIN + (float)rand() / (float)RAND_MAX * (MOB_SPAWN_MAX - MOB_SPAWN_MIN);
    int x = (int)floorf(anchor[0] + cosf(ang) * rad);
    int z = (int)floorf(anchor[2] + sinf(ang) * rad);
    int y = server_surface_y(s->world, x, z);
    if (y < 0) return;  /* terrain not ready / no valid column — try again next interval */
    Mob* m = mob_set_spawn(&s->mobs, MOB_ZOMBIE, (vec3){ (float)x + 0.5f, (float)(y + 1), (float)z + 0.5f });
    if (m) fprintf(stderr, "[server] spawned mob %u at (%d,%d)\n", m->id, x, z);
}

static void server_send_health(Server* s, ServerClient* c, uint8_t flags) {
    uint8_t buf[16];
    PacketHeader h = { .type = PKT_PLAYER_HEALTH, .player_id = 0 };
    uint8_t hp = (uint8_t)(c->health < 0 ? 0 : c->health);
    size_t len = net_write_player_health(buf, &h, hp, flags);
    send_reliable(s, c, buf, (uint16_t)len);
}

void server_damage_player(Server* s, ServerClient* c, int dmg) {
    if (c->health <= 0) return;
    c->health = (int16_t)(c->health - dmg);
    if (c->health <= 0) {
        c->health = 0;
        server_send_health(s, c, MOB_HEALTH_FLAG_DIED);
        c->health = PLAYER_MAX_HEALTH;   /* respawn refill (client teleports) */
    } else {
        server_send_health(s, c, 0);
    }
}

static void server_simulate_mobs(Server* s, float dt) {
    if (!s->world) return;

    /* Snapshot connected players for targeting. */
    MobTargetInfo players[SERVER_MAX_CLIENTS];
    int pcount = 0;
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        ServerClient* c = &s->clients[i];
        if (!c->active || !c->position_received) continue;
        players[pcount].player_id = c->player_id;
        players[pcount].position[0] = c->x;
        players[pcount].position[1] = c->y;
        players[pcount].position[2] = c->z;
        pcount++;
    }

    for (int i = 0; i < MOB_MAX; i++) {
        Mob* m = &s->mobs.mobs[i];
        if (!m->active) continue;

        if (m->attack_cooldown > 0.0f) m->attack_cooldown -= dt;

        /* 1. Target + intent */
        m->target_player = mob_acquire_target(m, players, pcount);
        float vx = 0.0f, vz = 0.0f;
        const MobTargetInfo* tgt = NULL;
        if (m->target_player != 0) {
            for (int p = 0; p < pcount; p++)
                if (players[p].player_id == m->target_player) { tgt = &players[p]; break; }
        }
        if (tgt) {
            mob_steer(m, (float*)tgt->position, &vx, &vz, &m->yaw);
        } else {
            /* Idle wander: pick a slow heading periodically. */
            m->wander_timer -= dt;
            if (m->wander_timer <= 0.0f) {
                m->wander_timer = MOB_WANDER_INTERVAL;
                m->yaw = (float)rand() / (float)RAND_MAX * 6.2831853f;
            }
            vx = cosf(m->yaw) * (MOB_SPEED * 0.3f);
            vz = sinf(m->yaw) * (MOB_SPEED * 0.3f);
        }
        m->velocity[0] = vx;
        m->velocity[2] = vz;

        /* 2. Jump over a 1-block step in the heading direction. */
        if (m->on_ground) {
            float hn = sqrtf(vx*vx + vz*vz);
            if (hn > 1e-3f) {
                int ax = (int)floorf(m->position[0] + (vx / hn) * (MOB_HALF_W + 0.3f));
                int az = (int)floorf(m->position[2] + (vz / hn) * (MOB_HALF_W + 0.3f));
                int fy = (int)floorf(m->position[1]);
                bool blocked = block_is_solid(world_get_block(s->world, ax, fy, az));
                bool clear   = world_get_block(s->world, ax, fy + 1, az) == BLOCK_AIR
                            && world_get_block(s->world, ax, fy + 2, az) == BLOCK_AIR;
                if (blocked && clear) m->velocity[1] = MOB_JUMP_SPEED;
            }
        }

        /* 3. Gravity + move (physics_move substeps collision internally). */
        m->velocity[1] -= GRAVITY * dt;
        if (m->velocity[1] < -TERMINAL_VEL) m->velocity[1] = -TERMINAL_VEL;
        PhysicsResult pr = physics_move(m->position, m->velocity,
                                        MOB_HALF_W, MOB_HEIGHT, dt,
                                        /*crouch=*/false, s->world);
        m->on_ground = pr.on_ground;

        /* 4. Contact damage to the target. */
        if (tgt && m->attack_cooldown <= 0.0f) {
            float d = glm_vec3_distance((float*)tgt->position, m->position);
            if (d <= MOB_ATTACK_RANGE) {
                for (int c = 0; c < SERVER_MAX_CLIENTS; c++) {
                    if (s->clients[c].active && s->clients[c].player_id == m->target_player) {
                        server_damage_player(s, &s->clients[c], MOB_ATTACK_DAMAGE);
                        break;
                    }
                }
                m->attack_cooldown = MOB_ATTACK_INTERVAL;
            }
        }

        /* 5. Despawn if far from every player. */
        bool near = false;
        for (int p = 0; p < pcount; p++)
            if (glm_vec3_distance((float*)players[p].position, m->position) <= MOB_DESPAWN_RANGE)
                { near = true; break; }
        if (pcount > 0 && !near) m->active = false;
    }
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
            handle_connect_request(s, &msg->addr, msg->data, msg->len);
        } else if (c) {
            if      (type == PKT_POSITION)     handle_position(s, c, msg->data, msg->len);
            else if (type == PKT_DISCONNECT)   disconnect_client(s, c);
            else if (type == PKT_BLOCK_BREAK)  handle_block_break(s, c, msg->data, (size_t)msg->len);
            else if (type == PKT_BLOCK_PLACE)  handle_block_place(s, c, msg->data, (size_t)msg->len);
            else if (type == PKT_MOB_ATTACK)   handle_mob_attack(s, c, msg->data, (size_t)msg->len);
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

    if (s->world) {
        server_simulate_mobs(s, 1.0f / SERVER_TICK_RATE);

        s->mob_spawn_timer += 1.0f / SERVER_TICK_RATE;
        if (s->mob_spawn_timer >= MOB_SPAWN_INTERVAL) {
            s->mob_spawn_timer = 0.0f;
            server_try_spawn(s, anchor);
        }
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

    {
        NetMobState mobs_wire[MOB_STATE_MAX_WIRE];
        uint16_t mc = 0;
        for (int i = 0; i < MOB_MAX && mc < MOB_STATE_MAX_WIRE; i++) {
            Mob* m = &s->mobs.mobs[i];
            if (!m->active) continue;
            mobs_wire[mc].id     = m->id;
            mobs_wire[mc].type   = (uint8_t)m->type;
            mobs_wire[mc].x      = m->position[0];
            mobs_wire[mc].y      = m->position[1];
            mobs_wire[mc].z      = m->position[2];
            mobs_wire[mc].yaw    = m->yaw;
            mobs_wire[mc].health = (uint8_t)(m->health < 0 ? 0 : m->health);
            mc++;
        }
        uint8_t mbuf[NET_MAX_PACKET];
        PacketHeader mhdr = { .type = PKT_MOB_STATE, .player_id = 0 };
        size_t mlen = net_write_mob_state(mbuf, &mhdr, mobs_wire, mc);
        for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
            if (!s->clients[i].active) continue;
            net_thread_push_outbound(s->net, mbuf, (int)mlen, &s->clients[i].addr);
        }
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
    mob_set_init(&s.mobs);

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
