#include "server.h"
#include "net.h"
#include "net_thread.h"
#include "platform_thread.h"
#include "inventory.h"
#include "item.h"      /* ItemId, tool_break_time, starter tools */
#include "crafting.h"  /* recipe table + pure matching for PKT_CRAFT */
#include "combat.h"    /* weapon_damage: weapon-scaled melee vs mobs (pure) */
#include "raycast.h"   /* for block_face_offset / FACE_PZ */
#include "gameplay.h"  /* for MAX_REACH */
#include "mob.h"
#include "mob_ai.h"      /* passive spawn eligibility + loot tables (pure) */
#include "physics.h"
#include "player.h"      /* GRAVITY, TERMINAL_VEL */
#include "block.h"       /* block_is_solid */
#include "block_physics.h" /* WATER_SOURCE_LEVEL */
#include "chunk.h"       /* CHUNK_Y, CHUNK_BLOCKS */
#include "chunkwire.h"   /* RLE column (de)serialize for PKT_CHUNK_DATA */
#include "chunk_stream.h"/* pure streaming-policy diff */
#include "reliable.h"    /* RELIABLE_MAX_PAYLOAD (chunk fragment sizing) */
#include "world.h"
#include "daynight.h"    /* world clock + darkness gate for spawning */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/* Cross-thread shutdown signal. The integrated server runs server_run() on its
 * own thread (host mode); the main thread calls server_request_stop() before
 * joining so the loop terminates instead of hanging the join forever. */
static atomic_bool g_server_stop = false;

void server_request_stop(void) { atomic_store(&g_server_stop, true); }

/* Cross-thread "flush the world now" signal. The UI's Save / Save & Quit
 * buttons set this; the loop forces an overlay save next tick and clears it.
 * Mirrors g_server_stop so server.h / net.h stay untouched. */
static atomic_bool g_server_save_req = false;

void server_request_save(void) { atomic_store(&g_server_save_req, true); }

/* Live authoritative world, published once server_run_ex creates it so the
 * host's main/render thread can render the SAME world instead of generating a
 * second copy (host shared-world unification). NULL before creation and after
 * teardown. _Atomic so the host can poll it from another thread. */
static _Atomic(World*) g_server_world = NULL;

World* server_get_world(void) {
    return atomic_load_explicit(&g_server_world, memory_order_acquire);
}

/* server.c is the first translation unit that pulls in both the inventory
 * model and the inventory wire format, so this is where we assert the
 * cross-header invariant. */
_Static_assert(INVENTORY_SLOTS == INVENTORY_NET_SLOTS,
    "wire format and inventory model must agree on slot count");
_Static_assert(ARMOR_SLOT_COUNT == ARMOR_NET_SLOTS,
    "wire format and armour model must agree on armour slot count");
_Static_assert(CHUNK_DATA_RELIABLE_MAX == RELIABLE_MAX_PAYLOAD,
    "chunk-data fragment sizing in net.h must match the reliable payload cap");

/* Chunk streaming tunables. Budget caps how many NEW columns we encode+send per
 * client per tick so a fresh joiner doesn't flood the link (and the reliable
 * send window) in one tick; the rest stream over subsequent ticks. */
#define SERVER_STREAM_BUDGET     4
/* Upper bound on the render distance we will stream around any client, so a
 * malicious/huge advertised value can't make us generate an unbounded disc. */
#define SERVER_STREAM_MAX_RD     32
/* Safety cap on fragments for one column. Real terrain RLE-compresses to 1-3
 * fragments; only an adversarial/incompressible column approaches this. We skip
 * (don't stream) a column that would exceed it rather than overflow the 32-slot
 * reliable window. */
#define SERVER_STREAM_MAX_FRAGS  48
_Static_assert(SERVER_STREAM_MAX_FRAGS <= CHUNK_DATA_FRAG_MAX,
    "server must not send more fragments than the client can reassemble");
_Static_assert(SERVER_STREAM_MAX_FRAGS <= RELIABLE_WINDOW,
    "a column's fragments must fit in the reliable send window at once");

/* Edible item: with no dedicated FOOD item in the block model, we designate an
 * existing, naturally-obtainable block as food ("foraged berries from leaves").
 * Right-clicking while holding it eats one to restore hunger. Each leaf restores
 * a few drumsticks plus a little saturation. */
#define SERVER_FOOD_BLOCK    BLOCK_LEAVES
#define SERVER_FOOD_RESTORE  4.0f
#define SERVER_FOOD_SAT      2.4f

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Forward decl: armour snapshot is sent on connect (before its definition). */
static void server_send_armor(Server* s, ServerClient* c);

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

/* Give a fresh player a usable starter set of tools (one of each kind at the
 * lowest tier). Tools are unstackable, so each lands in its own slot; with
 * INVENTORY_SLOTS == 6 this leaves 3 slots free for mined blocks. */
static void server_give_starter_kit(ServerClient* c) {
    inventory_add_item(&c->inventory, ITEM_WOOD_PICKAXE, 1);
    inventory_add_item(&c->inventory, ITEM_WOOD_AXE,     1);
    inventory_add_item(&c->inventory, ITEM_WOOD_SHOVEL,  1);
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
        server_give_starter_kit(c);
        c->health = PLAYER_MAX_HEALTH;
        survival_init(&c->survival);
        c->prev_pos_valid = false;
        c->falling        = false;
        c->respawn_grace  = 0.0f;
        c->needs_health_sync = false;
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
    /* Release the per-client streamed-chunk set (alloc_client memsets the slot
     * on reuse, which would otherwise leak this array). */
    free(c->streamed);
    c->streamed       = NULL;
    c->streamed_count = 0;
    c->streamed_cap   = 0;
}

/* ------------------------------------------------------------------ */
/*  Malformed-packet drop logging (rate-limited)                       */
/* ------------------------------------------------------------------ */
/* A hostile peer can fire a stream of truncated/garbage datagrams; logging one
 * line per drop would itself be a log-flood DoS. Emit at most one line per
 * second and fold the rest into a count. */
static void server_drop_malformed(const char* what)
{
    static double next_report = 0.0;
    static unsigned long suppressed = 0;
    double now = net_time();
    if (now >= next_report) {
        if (suppressed)
            fprintf(stderr, "[server] dropped malformed %s packet "
                            "(+%lu more suppressed)\n", what, suppressed);
        else
            fprintf(stderr, "[server] dropped malformed %s packet\n", what);
        suppressed = 0;
        next_report = now + 1.0;
    } else {
        suppressed++;
    }
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
    /* Shared-world host (renders the server world in-process) must NOT be
     * streamed chunks; a true remote client must. */
    c->shares_world = net_read_connect_shared(data, (size_t)len) != 0;
    /* Stream the disc the client actually renders, clamped to a sane server cap
     * (on-demand gen can serve any chunk, so we are not bound by the server
     * world's small mob render distance). 0 advertised -> fall back to the
     * server world's render distance. */
    {
        int crd = (int)net_read_connect_render_dist(data, (size_t)len);
        if (crd <= 0) crd = world_get_render_distance(s->world);
        if (crd > SERVER_STREAM_MAX_RD) crd = SERVER_STREAM_MAX_RD;
        c->stream_rd = crd;
    }
    printf("[server] player %d connected (protocol v%u%s)\n", c->player_id, version,
           c->shares_world ? ", shared-world host" : ", remote");

    uint8_t buf[HEADER_WIRE_SIZE];
    size_t off = 0;
    PacketHeader h = { .type = PKT_CONNECT_ACCEPT, .player_id = c->player_id };
    net_write_header(buf, &off, &h);
    send_reliable(s, c, buf, (uint16_t)off);

    broadcast_player_join(s, c);

    /* Initial state snapshots for the new client (armour starts empty). */
    server_send_armor(s, c);
}

/* Keepalive ping from a client: refresh its liveness timer (so SERVER_TIMEOUT
 * never drops an otherwise-idle but connected client) and pong back so the
 * client's own dead-server watchdog stays satisfied and its NAT pinhole open. */
static void handle_keepalive(Server* s, ServerClient* c,
                             const uint8_t* data, int len)
{
    PacketHeader h;
    if (!net_parse_keepalive(data, (size_t)len, &h)) {
        server_drop_malformed("keepalive");
        return;
    }
    c->last_recv_time = net_time();

    /* Unreliable pong (carries fresh ack/ack_bits like any packet). */
    PacketHeader ph = { .type = PKT_KEEPALIVE, .player_id = 0 };
    reliable_fill_ack(&c->reliable, &ph.ack, &ph.ack_bits);
    uint8_t buf[HEADER_WIRE_SIZE];
    size_t plen = net_write_keepalive(buf, &ph);
    net_thread_push_outbound(s->net, buf, (int)plen, &c->addr);
}

static void handle_position(Server* s, ServerClient* c,
                              const uint8_t* data, int len)
{
    PositionPacket p;
    if (!net_parse_position(data, (size_t)len, &p)) {
        server_drop_malformed("position");
        return;
    }

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

/* Persist a block edit: record it in the overlay (so it survives restarts and
 * is replayed onto newly generated chunks) and apply it to the live headless
 * world (so mob terrain/collision stays consistent). */
static void server_persist_edit(Server* s, int x, int y, int z, BlockID b)
{
    if (s->overlay_active) {
        overlay_record(&s->overlay, x, y, z, b);
        s->overlay_dirty = true;
    }
    if (s->world) {
        world_set_block(s->world, x, y, z, b);
        /* Player-placed water must be a full source block, else water_tick
         * reads meta 0 and dissipates it next tick. In host shared-world mode
         * the client renders this same world, so the server (not the client)
         * owns setting the source meta. Harmless for the headless/dedicated
         * server too. */
        if (b == BLOCK_WATER)
            world_set_meta(s->world, x, y, z, WATER_SOURCE_LEVEL);
    }
}

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
        p.slots[i].item       = (uint16_t)c->inventory.slots[i].item;
        p.slots[i].count      = c->inventory.slots[i].count;
        p.slots[i].durability = c->inventory.slots[i].durability;
    }
    uint8_t buf[64];
    size_t  len = net_write_inventory(buf, &p);
    send_reliable(s, c, buf, (uint16_t)len);
}

/* Total armour points from the player's worn set (clamped pure helper). */
static int server_armor_points(const ServerClient* c) {
    return armor_points_total(c->armor);
}

static void server_send_armor(Server* s, ServerClient* c) {
    uint16_t worn[ARMOR_NET_SLOTS];
    for (int i = 0; i < ARMOR_SLOT_COUNT; i++) worn[i] = (uint16_t)c->armor[i];
    PacketHeader h = { .type = PKT_ARMOR, .player_id = 0 };
    uint8_t buf[32];
    size_t  len = net_write_armor(buf, &h, worn,
                                  (uint8_t)server_armor_points(c));
    send_reliable(s, c, buf, (uint16_t)len);
}

/* Equip the armour item held in inventory slot `slot` into its body slot.
 * Server-authoritative: any non-armour item or empty/garbage slot is a no-op.
 * Swaps any previously-worn piece back into the inventory slot. */
static void handle_equip(Server* s, ServerClient* c,
                         const uint8_t* data, size_t len) {
    PacketHeader h; uint8_t slot;
    if (!net_parse_equip(data, len, &h, &slot)) {
        server_drop_malformed("equip");
        return;
    }
    bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
    if (!is_new) return;
    c->last_recv_time = net_time();

    if (slot >= INVENTORY_SLOTS) return;
    InventorySlot* inv = &c->inventory.slots[slot];
    if (inv->count == 0) return;
    ItemId item = inv->item;
    ArmorSlot as = item_armor_slot(item);
    if (as == ARMOR_SLOT_NONE) return;     /* not armour: ignore */

    /* Swap: remember what was worn, place the new piece, return the old to the
     * vacated inventory slot (armour is unstackable so the slot held one). */
    ItemId   prev_item = c->armor[as];
    uint16_t prev_dur  = c->armor_dur[as];

    c->armor[as]     = item;
    c->armor_dur[as] = inv->durability ? inv->durability
                                       : item_get_def(item)->max_durability;

    if (prev_item != BLOCK_AIR && item_is_armor(prev_item)) {
        inv->item       = prev_item;
        inv->count      = 1;
        inv->durability = prev_dur;
    } else {
        inv->item       = BLOCK_AIR;
        inv->count      = 0;
        inv->durability = 0;
    }

    server_send_inventory(s, c);
    server_send_armor(s, c);
}

/* Server-authoritative eating (PKT_EAT): the player asks to eat the food item
 * held in hotbar `slot`. Validated via the pure survival_can_eat rule — the slot
 * must hold a food item AND the player must not already be at full hunger (so a
 * food item is never wasted). On success: consume one from the stack, restore
 * hunger by the item's item_hunger_restore, and resync the inventory + health
 * (which carries the new hunger to the HUD). Any failure is a silent no-op so a
 * stale/malicious client can't desync state. */
static void handle_eat(Server* s, ServerClient* c,
                       const uint8_t* data, size_t len) {
    PacketHeader h; uint8_t slot;
    if (!net_parse_eat(data, len, &h, &slot)) {
        server_drop_malformed("eat");
        return;
    }
    bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
    if (!is_new) return;
    c->last_recv_time = net_time();

    if (slot >= INVENTORY_SLOTS) return;
    InventorySlot* inv = &c->inventory.slots[slot];
    if (inv->count == 0) return;

    if (!survival_can_eat(item_is_food(inv->item), c->survival.food,
                          (float)SURVIVAL_MAX_FOOD))
        return;                                  /* not food, or already full */

    if (!survival_apply_food(&c->survival, item_hunger_restore(inv->item)))
        return;                                  /* nothing restored (no-op) */

    inventory_consume(&c->inventory, slot);
    c->needs_health_sync = true;                 /* push updated hunger this tick */
    server_send_inventory(s, c);
}

/* --- Pure block-edit validation predicates (no world/server state) --- */

/* A block can be broken (and dropped into inventory) if it is a real, solid
 * block that isn't the unbreakable bedrock floor. Air/water are not solid;
 * bedrock is solid but flagged unbreakable. Both checks are side-effect free. */
static inline bool server_block_breakable(BlockID id)
{
    if (id == BLOCK_AIR || id >= BLOCK_COUNT) return false;
    if (id == BLOCK_BEDROCK) return false;
    if (!block_is_solid(id)) return false;                 /* excludes water */
    if (block_break_time(id) >= BLOCK_BREAK_UNBREAKABLE) return false;
    return true;
}

/* A cell currently holding `target` may have a new block placed into it if the
 * cell is empty (air) or a replaceable fluid (water). Solid blocks block
 * placement; the client should have aimed at an adjacent empty cell. */
static inline bool server_block_replaceable(BlockID target)
{
    return target == BLOCK_AIR || target == BLOCK_WATER;
}

/* Does an axis-aligned unit block at (bx,by,bz) overlap any active player's
 * AABB? Used to refuse placements that would intersect a player (including the
 * placer, already covered separately, but cheap to include). */
static bool server_block_intersects_player(Server* s, int bx, int by, int bz)
{
    float bminx = (float)bx,        bmaxx = (float)bx + 1.0f;
    float bminy = (float)by,        bmaxy = (float)by + 1.0f;
    float bminz = (float)bz,        bmaxz = (float)bz + 1.0f;
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        ServerClient* o = &s->clients[i];
        if (!o->active || !o->position_received) continue;
        float pminx = o->x - PLAYER_HALF_W, pmaxx = o->x + PLAYER_HALF_W;
        float pminy = o->y,                 pmaxy = o->y + PLAYER_HEIGHT;
        float pminz = o->z - PLAYER_HALF_W, pmaxz = o->z + PLAYER_HALF_W;
        if (pmaxx > bminx && pminx < bmaxx &&
            pmaxy > bminy && pminy < bmaxy &&
            pmaxz > bminz && pminz < bmaxz) return true;
    }
    return false;
}

static void handle_block_break(Server* s, ServerClient* c,
                                const uint8_t* data, size_t len)
{
    /* Header 8 + payload 12 (xyz) + 1 (block) + 1 (slot) = 22 wire bytes. */
    BlockBreakPacket p;
    if (!net_parse_block_break(data, len, &p)) {
        server_drop_malformed("block-break");
        return;
    }

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

    /* Consult the authoritative server world (which already reflects the
     * persistence overlay's prior edits) instead of trusting the client's
     * claimed block. Reject the break unless the world actually holds a solid,
     * breakable block at the requested cell. The credited drop is the server's
     * block, never the client-supplied one. */
    if (!s->world) return;
    /* Ensure the target column is really generated on the server (applying the
     * persistence overlay) before reading it, so validation consults the TRUE
     * block instead of a fail-open AIR. world_ensure_chunk is synchronous and
     * thread-safe; world_get_chunk_col_block reads through it. In the rare host-
     * mode window where the chunk is mid-generation by a worker, ensure returns
     * without a forced regen and the read may still be AIR — that simply refuses
     * the break (the client retries), which is correct and never credits a
     * block that isn't there. */
    int bcx = (p.x < 0) ? (p.x - 15) / 16 : p.x / 16;
    int bcz = (p.z < 0) ? (p.z - 15) / 16 : p.z / 16;
    world_ensure_chunk(s->world, bcx, bcz);
    BlockID actual = world_get_block(s->world, p.x, p.y, p.z);
    if (!server_block_breakable(actual)) return;

    uint8_t leftover = inventory_add(&c->inventory, actual, 1);
    if (leftover != 0) {
        /* No room — refuse the break. Don't broadcast, don't send inventory. */
        return;
    }
    /* Successful break: wear the held tool (no-op if the slot isn't a tool).
     * If it breaks, inventory_damage_tool empties the slot; the snapshot below
     * carries the change to the client. */
    if (p.slot < INVENTORY_SLOTS)
        inventory_damage_tool(&c->inventory, p.slot);
    server_persist_edit(s, p.x, p.y, p.z, BLOCK_AIR);
    server_broadcast_block_change(s, p.x, p.y, p.z, BLOCK_AIR);
    server_send_inventory(s, c);
}

static void handle_block_place(Server* s, ServerClient* c,
                                const uint8_t* data, size_t len)
{
    /* Header 8 + payload 12 (xyz) + 1 (face) + 1 (slot) = 22 wire bytes. */
    BlockPlacePacket p;
    if (!net_parse_block_place(data, len, &p)) {
        server_drop_malformed("block-place");
        return;
    }

    bool is_new = reliable_on_recv(&c->reliable,
                                    p.header.seq,
                                    p.header.ack,
                                    p.header.ack_bits);
    if (!is_new) return;

    c->last_recv_time = net_time();

    if (!c->position_received) return;

    if (p.face > FACE_PZ || p.slot >= INVENTORY_SLOTS) return;
    if (c->inventory.slots[p.slot].count == 0) return;

    /* Edible item: right-clicking with the designated food block consumes one
     * to restore hunger instead of placing it (only when not already full).
     * Reuses the block/item model — no new wire packet or client input path. */
    if (c->inventory.slots[p.slot].item == SERVER_FOOD_BLOCK) {
        if (survival_eat(&c->survival.food, &c->survival.saturation,
                         SERVER_FOOD_RESTORE, SERVER_FOOD_SAT)) {
            inventory_consume(&c->inventory, p.slot);
            c->needs_health_sync = true;   /* push updated hunger this tick */
            server_send_inventory(s, c);
            return;
        }
        /* Full: fall through and place it like a normal block. */
    }

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

    ItemId  held = c->inventory.slots[p.slot].item;
    if (!item_is_block(held)) return;                 /* tools aren't placeable */
    BlockID b = item_as_block(held);
    if (b == BLOCK_AIR || b >= BLOCK_COUNT) return;   /* should be impossible, but don't broadcast garbage */

    /* Validate against the authoritative server world (reflects the overlay's
     * prior edits): the target cell must currently be empty/replaceable, and
     * the new block must not intersect any player. Ensure the target column is
     * generated first so placement isn't wrongly allowed/refused against a
     * fail-open AIR read of an ungenerated chunk. */
    if (!s->world) return;
    {
        int tcx = (tx < 0) ? (tx - 15) / 16 : tx / 16;
        int tcz = (tz < 0) ? (tz - 15) / 16 : tz / 16;
        world_ensure_chunk(s->world, tcx, tcz);
    }
    BlockID target = world_get_block(s->world, tx, ty, tz);
    if (!server_block_replaceable(target)) return;
    if (server_block_intersects_player(s, tx, ty, tz)) return;

    inventory_consume(&c->inventory, p.slot);
    server_persist_edit(s, tx, ty, tz, b);
    server_broadcast_block_change(s, tx, ty, tz, b);
    server_send_inventory(s, c);
}

/* Server-authoritative crafting: validate the player holds every ingredient of
 * the requested recipe, consume them, add the output, and push a fresh
 * inventory snapshot. Any failure (bad index, can't afford, no room) is a
 * silent no-op so a malicious or stale client can't desync the inventory. */
static void handle_craft(Server* s, ServerClient* c,
                         const uint8_t* data, size_t len) {
    PacketHeader h; uint16_t recipe_index;
    if (!net_parse_craft(data, len, &h, &recipe_index)) {
        server_drop_malformed("craft");
        return;
    }
    bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
    if (!is_new) return;
    c->last_recv_time = net_time();

    const Recipe* r = crafting_recipe((int)recipe_index);
    if (!r) return;

    ItemCounts counts;
    crafting_counts_from_inventory(&c->inventory, &counts);
    if (!crafting_can_make(r, &counts)) return;

    /* Dry-run the output add on a copy so we never consume inputs we can't
     * deposit the result for (atomic: either the whole craft applies or none). */
    Inventory trial = c->inventory;
    for (int k = 0; k < r->input_count; k++) {
        if (!inventory_remove(&trial, r->inputs[k].item, r->inputs[k].count))
            return;   /* should not happen after can_make, but stay safe */
    }
    if (inventory_add_item(&trial, r->output.item, r->output.count) != 0)
        return;       /* no room for the output — refuse the craft */

    c->inventory = trial;
    server_send_inventory(s, c);
}

/* Award a slain mob's loot to its killer's inventory (server-authoritative) and
 * push the updated inventory if anything was actually picked up. Drops whose
 * item is not yet modelled produce an empty table (see mob_loot), so this is a
 * no-op for those types until the items exist. */
static void award_mob_loot(Server* s, ServerClient* c, MobType type) {
    MobLootDrop drops[MOB_LOOT_MAX];
    int n = mob_loot(type, drops);
    bool gained = false;
    for (int i = 0; i < n; i++) {
        uint8_t leftover = inventory_add_item(&c->inventory, drops[i].item, drops[i].count);
        if (leftover < drops[i].count) gained = true;   /* at least one picked up */
    }
    if (gained) server_send_inventory(s, c);
}

/* Melee damage the attacker deals to a mob this swing, in hit-points.
 *
 * Ideally this would use the player's *selected* hotbar item, but the
 * PKT_MOB_ATTACK wire form carries only the mob id and the server never learns
 * the client's selected slot (inventory is server->client only). So, staying
 * server-authoritative, we use the strongest weapon the attacker is carrying as
 * the swing weapon, falling back to the bare-hand (fist) baseline when they hold
 * no weapon. weapon_damage() returns COMBAT_FIST_DAMAGE for non-weapon items, so
 * an empty/block-only inventory naturally yields a fist punch. Result >= 1. */
static int server_player_mob_damage(const ServerClient* c) {
    float best = COMBAT_FIST_DAMAGE;
    for (int i = 0; i < INVENTORY_SLOTS; i++) {
        if (c->inventory.slots[i].count == 0) continue;
        float d = weapon_damage(c->inventory.slots[i].item);
        if (d > best) best = d;
    }
    int dmg = (int)(best + 0.5f);   /* round to whole hit-points */
    return dmg < 1 ? 1 : dmg;
}

static void handle_mob_attack(Server* s, ServerClient* c,
                              const uint8_t* data, size_t len) {
    PacketHeader h; uint16_t mob_id;
    if (!net_parse_mob_attack(data, len, &h, &mob_id)) {
        server_drop_malformed("mob-attack");
        return;
    }
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

    /* Subtract the attacker's weapon damage from the mob's health pool; the mob
     * only dies once health hits 0. Per-type max health (mob_max_health) means
     * tanky hostiles take several hits while frail animals drop fast. */
    int dmg = server_player_mob_damage(c);
    if (mob_combat_apply(&m->health, dmg)) {
        MobType killed = m->type;           /* m is invalidated by removal below */
        award_mob_loot(s, c, killed);       /* type-appropriate drops to killer */
        mob_set_remove(&s->mobs, mob_id);   /* dead → vanishes next broadcast */
    }
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

/* Hostile types only (zombie/skeleton/creeper occupy the first slots of the
 * MobType enum). Passive animals are handled by the daytime herd path below. */
#define HOSTILE_TYPE_COUNT 3

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
    MobType type = (MobType)(rand() % HOSTILE_TYPE_COUNT);   /* hostile only */
    Mob* m = mob_set_spawn(&s->mobs, type, (vec3){ (float)x + 0.5f, (float)(y + 1), (float)z + 0.5f });
    if (m) fprintf(stderr, "[server] spawned mob %u (type %d) at (%d,%d)\n", m->id, (int)type, x, z);
}

/* Count currently-live passive animals (for the passive cap). */
static int server_count_passive(const Server* s) {
    int n = 0;
    for (int i = 0; i < MOB_MAX; i++) {
        const Mob* m = &s->mobs.mobs[i];
        if (m->active && mob_ai_is_passive_spawn_type(m->type)) n++;
    }
    return n;
}

/* Distance from (x,z) at surface height to the nearest connected player (XZ
 * plane). Returns a large sentinel when no players are connected. */
static float server_nearest_player_dist_xz(const Server* s, float x, float z) {
    float best = 1.0e30f;
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        const ServerClient* c = &s->clients[i];
        if (!c->active || !c->position_received) continue;
        float dx = c->x - x, dz = c->z - z;
        float d = sqrtf(dx*dx + dz*dz);
        if (d < best) best = d;
    }
    return best;
}

/* Daytime passive herd spawning. Picks a candidate column in a ring around the
 * anchor, then defers to the pure eligibility check (daytime, grass surface,
 * passive cap, min distance from players) before placing a small same-type herd
 * clustered around the seed. Server-authoritative; runs on a separate timer and
 * budget from the hostile path. */
static void server_try_spawn_passive(Server* s, vec3 anchor) {
    int live_passive = server_count_passive(s);

    /* Candidate seed column in the passive spawn ring. */
    float ang = (float)rand() / (float)RAND_MAX * 6.2831853f;
    float rad = PASSIVE_SPAWN_MIN
              + (float)rand() / (float)RAND_MAX * (PASSIVE_SPAWN_MAX - PASSIVE_SPAWN_MIN);
    int sx = (int)floorf(anchor[0] + cosf(ang) * rad);
    int sz = (int)floorf(anchor[2] + sinf(ang) * rad);
    int sy = server_surface_y(s->world, sx, sz);
    if (sy < 0) return;   /* terrain not ready */

    bool is_day   = !daynight_is_dark(s->world_ticks);
    bool on_grass = world_get_block(s->world, sx, sy, sz) == BLOCK_GRASS;
    float pdist   = server_nearest_player_dist_xz(s, (float)sx + 0.5f, (float)sz + 0.5f);

    if (!mob_ai_passive_spawn_ok(is_day, on_grass, live_passive, pdist)) return;

    int herd = mob_ai_herd_size(live_passive, (uint32_t)rand());
    if (herd <= 0) return;
    MobType type = mob_ai_herd_type((uint32_t)rand());

    int spawned = 0;
    for (int i = 0; i < herd; i++) {
        /* Cluster members within PASSIVE_HERD_SPREAD blocks of the seed. */
        int ox = (int)floorf(((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * PASSIVE_HERD_SPREAD);
        int oz = (int)floorf(((float)rand() / (float)RAND_MAX * 2.0f - 1.0f) * PASSIVE_HERD_SPREAD);
        int mx = sx + ox, mz = sz + oz;
        int my = server_surface_y(s->world, mx, mz);
        if (my < 0) { my = sy; mx = sx; mz = sz; }   /* fall back to the seed column */
        Mob* m = mob_set_spawn(&s->mobs, type,
                               (vec3){ (float)mx + 0.5f, (float)(my + 1), (float)mz + 0.5f });
        if (m) spawned++;
    }
    if (spawned)
        fprintf(stderr, "[server] spawned passive herd: %d x type %d at (%d,%d)\n",
                spawned, (int)type, sx, sz);
}

/* Convert survival air seconds (0..MAX_AIR_SEC) to 0..20 bubbles for the HUD. */
static uint8_t server_air_bubbles(const ServerClient* c) {
    float frac = c->survival.air / SURVIVAL_MAX_AIR_SEC;
    if (frac < 0.0f) frac = 0.0f;
    if (frac > 1.0f) frac = 1.0f;
    return (uint8_t)(frac * NET_MAX_AIR + 0.5f);
}

static void server_send_health(Server* s, ServerClient* c, uint8_t flags) {
    uint8_t buf[16];
    PacketHeader h = { .type = PKT_PLAYER_HEALTH, .player_id = 0 };
    uint8_t hp = (uint8_t)(c->health < 0 ? 0 : c->health);
    uint8_t food = (uint8_t)(c->survival.food < 0.0f ? 0 : (int)(c->survival.food + 0.5f));
    if (food > NET_MAX_FOOD) food = NET_MAX_FOOD;
    size_t len = net_write_player_health(buf, &h, hp, flags, food,
                                         server_air_bubbles(c));
    send_reliable(s, c, buf, (uint16_t)len);
}

/* On death: clear the inventory (vanilla drops items; with no item-entity
 * system we simply clear them), refill health/hunger/air, and start a brief
 * invulnerability window. The DIED flag tells the client to play its death
 * state and teleport to spawn. */
static void server_kill_player(Server* s, ServerClient* c) {
    inventory_init(&c->inventory);
    server_give_starter_kit(c);   /* keep tools usable after respawn */
    server_send_inventory(s, c);

    /* Worn armour is lost on death (no item-entity drops yet). */
    for (int i = 0; i < ARMOR_SLOT_COUNT; i++) {
        c->armor[i]     = (ItemId)BLOCK_AIR;
        c->armor_dur[i] = 0;
    }
    server_send_armor(s, c);

    c->health = 0;
    server_send_health(s, c, MOB_HEALTH_FLAG_DIED);

    /* Respawn refill (client teleports to spawn on the DIED flag). */
    c->health = PLAYER_MAX_HEALTH;
    survival_init(&c->survival);
    c->respawn_grace  = SURVIVAL_RESPAWN_GRACE_SEC;
    c->falling        = false;
    c->prev_pos_valid = false;
    c->needs_health_sync = true;
}

/* Apply one point of armour wear to every worn piece on a hit; a piece that
 * reaches 0 durability breaks (slot emptied). Flags an armour sync if anything
 * changed so the client HUD updates. Matches the per-hit wear tested for the
 * pure durability math. */
static void server_wear_armor(Server* s, ServerClient* c) {
    bool changed = false;
    for (int i = 0; i < ARMOR_SLOT_COUNT; i++) {
        if (c->armor[i] == BLOCK_AIR || !item_is_armor(c->armor[i])) continue;
        if (c->armor_dur[i] > 0) c->armor_dur[i]--;
        if (c->armor_dur[i] == 0) {            /* broke */
            c->armor[i] = (ItemId)BLOCK_AIR;
            changed = true;
        }
    }
    if (changed) server_send_armor(s, c);
}

void server_damage_player(Server* s, ServerClient* c, int dmg) {
    if (c->health <= 0) return;
    if (c->respawn_grace > 0.0f) return;   /* invulnerable after respawn */
    if (dmg <= 0) return;

    /* Armour reduces incoming damage (4%/point, capped) and takes wear. */
    int reduced = damage_after_armor(dmg, server_armor_points(c));
    if (server_armor_points(c) > 0) server_wear_armor(s, c);

    c->health = (int16_t)(c->health - reduced);
    if (c->health <= 0) {
        server_kill_player(s, c);
    } else {
        server_send_health(s, c, 0);
    }
}

/* Is the block at (x,y,z) lava? No BLOCK_LAVA exists in the world model yet,
 * so this is always false today; the survival lava path and its pure math are
 * fully wired and tested, ready for a lava block to be added later. */
static bool server_block_is_lava(BlockID b) {
    (void)b;
#ifdef BLOCK_LAVA
    return b == BLOCK_LAVA;
#else
    return false;
#endif
}

/* Per-tick survival simulation for one client: hunger decay from movement,
 * fall damage on landing, drowning when the head is underwater, lava contact,
 * starvation, and natural regen. Server-authoritative; mutates health/hunger
 * and flags a health sync when anything visible changes. */
static void server_simulate_survival(Server* s, ServerClient* c, float dt) {
    if (!c->position_received) return;

    SurvivalState* sv = &c->survival;
    int16_t hp_before   = c->health;
    float   food_before = sv->food;
    uint8_t air_before  = server_air_bubbles(c);

    if (c->respawn_grace > 0.0f) {
        c->respawn_grace -= dt;
        if (c->respawn_grace < 0.0f) c->respawn_grace = 0.0f;
    }

    /* --- Movement-driven exhaustion (horizontal distance) --- */
    if (c->prev_pos_valid) {
        float ddx = c->x - c->prev_x;
        float ddz = c->z - c->prev_z;
        float hdist = sqrtf(ddx*ddx + ddz*ddz);
        bool sprinting = hdist > (PLAYER_SPRINT_SPEED * 0.5f) * dt; /* rough */
        survival_apply_exhaustion(&sv->food, &sv->saturation, &sv->exhaustion,
                                  survival_exhaustion_move(hdist, sprinting));
    }
    /* Idle trickle so a stationary player still grows hungry over time. */
    survival_apply_exhaustion(&sv->food, &sv->saturation, &sv->exhaustion,
                              SURVIVAL_EXH_IDLE_PER_SEC * dt);

    /* --- Fall damage: track descent, apply on landing --- */
    if (c->prev_pos_valid && s->world) {
        bool grounded = block_is_solid(
            world_get_block(s->world, (int)floorf(c->x),
                            (int)floorf(c->y - 0.1f), (int)floorf(c->z)));
        bool descending = c->y < c->prev_y - 1e-4f;
        if (descending) {
            if (!c->falling) { c->falling = true; c->fall_start_y = c->prev_y; }
            else if (c->prev_y > c->fall_start_y) c->fall_start_y = c->prev_y;
        }
        if (c->falling && grounded) {
            float dist = c->fall_start_y - c->y;
            int dmg = survival_fall_damage(dist);
            c->falling = false;
            if (dmg > 0) server_damage_player(s, c, dmg);
        }
        /* Reset the fall tracker if we end up rising (jump/swim) while not
         * mid-descent, so a jump doesn't pre-arm fall damage. */
        if (!descending && grounded) c->falling = false;
    }

    /* --- Drowning: is the head block water? --- */
    bool head_water = false;
    if (s->world) {
        int hx = (int)floorf(c->x);
        int hy = (int)floorf(c->y + PLAYER_EYE_H);
        int hz = (int)floorf(c->z);
        head_water = (world_get_block(s->world, hx, hy, hz) == BLOCK_WATER);
    }
    {
        int dmg = survival_drown_step(head_water, &sv->air, &sv->drown_timer, dt);
        if (dmg > 0) server_damage_player(s, c, dmg);
    }

    /* --- Lava contact (feet block) --- */
    bool in_lava = false;
    if (s->world) {
        in_lava = server_block_is_lava(
            world_get_block(s->world, (int)floorf(c->x),
                            (int)floorf(c->y), (int)floorf(c->z)));
    }
    {
        int dmg = survival_lava_step(in_lava, &sv->lava_timer, dt);
        if (dmg > 0) server_damage_player(s, c, dmg);
    }

    /* --- Starvation + regen (only if still alive after the above) --- */
    if (c->health > 0) {
        int starve = survival_starve_step(sv->food, c->health, &sv->starve_timer, dt);
        if (starve > 0) server_damage_player(s, c, starve);

        int heal = survival_regen_tick(sv, c->health, dt);
        if (heal > 0) {
            c->health = (int16_t)(c->health + heal);
            if (c->health > PLAYER_MAX_HEALTH) c->health = PLAYER_MAX_HEALTH;
        }
    }

    /* Remember this tick's position for next-tick diffing. */
    c->prev_x = c->x; c->prev_y = c->y; c->prev_z = c->z;
    c->prev_pos_valid = true;

    /* Push a health packet if anything the client renders changed. server_*
     * damage already sent packets on hits; this catches hunger/air/regen. */
    if (c->health != hp_before
        || (int)(sv->food + 0.5f) != (int)(food_before + 0.5f)
        || server_air_bubbles(c) != air_before
        || c->needs_health_sync) {
        c->needs_health_sync = false;
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
        MobStats st = mob_stats(m->type);
        float tdist = tgt ? glm_vec3_distance((float*)tgt->position, m->position) : 0.0f;
        if (tgt) {
            mob_steer(m, (float*)tgt->position, &vx, &vz, &m->yaw);
            /* Skeleton keeps its distance: reverse the chase vector when too close. */
            if (m->type == MOB_SKELETON && skeleton_wants_to_retreat(tdist)) {
                vx = -vx; vz = -vz;   /* face stays toward target via m->yaw */
            }
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

        /* 4. Per-type attack against the target. */
        if (tgt) {
            ServerClient* tc = NULL;
            for (int c = 0; c < SERVER_MAX_CLIENTS; c++)
                if (s->clients[c].active && s->clients[c].player_id == m->target_player)
                    { tc = &s->clients[c]; break; }

            if (m->type == MOB_CREEPER) {
                /* Arm the fuse near the player; detonate when it burns out. */
                if (!m->fuse_lit && creeper_should_arm_fuse(tdist)) {
                    m->fuse_lit = true;
                    m->fuse_timer = CREEPER_FUSE_TIME;
                }
                if (m->fuse_lit) {
                    m->fuse_timer -= dt;
                    /* Walk back out of range -> defuse. */
                    if (tdist > CREEPER_FUSE_RANGE * 1.5f) {
                        m->fuse_lit = false;
                    } else if (creeper_should_detonate(tdist, m->fuse_timer)) {
                        if (tc) server_damage_player(s, tc, CREEPER_BLAST_DAMAGE);
                        m->active = false;  /* self-destruct (block damage = follow-up) */
                        continue;
                    }
                }
            } else if (m->type == MOB_SKELETON) {
                /* Ranged hitscan shot on a cooldown. v1: no projectile travel. */
                if (m->attack_cooldown <= 0.0f && skeleton_in_shoot_range(tdist)) {
                    if (tc) server_damage_player(s, tc, st.attack_damage);
                    m->attack_cooldown = st.attack_interval;
                }
            } else if (m->attack_cooldown <= 0.0f && tdist <= st.attack_range) {
                /* Zombie melee contact. */
                if (tc) server_damage_player(s, tc, st.attack_damage);
                m->attack_cooldown = st.attack_interval;
            }
        }

        /* 5. Despawn if far from every player. */
        bool any_near = false;  /* not "near": it is a legacy macro in <windows.h> */
        for (int p = 0; p < pcount; p++)
            if (glm_vec3_distance((float*)players[p].position, m->position) <= MOB_DESPAWN_RANGE)
                { any_near = true; break; }
        if (pcount > 0 && !any_near) m->active = false;
    }
}

/* ------------------------------------------------------------------ */
/*  Persistence flush                                                   */
/* ------------------------------------------------------------------ */

/* Write the overlay to disk if there are unsaved edits. overlay_save
 * snapshots under the overlay's lock then does the file write outside it,
 * so this is safe to call from the tick loop. */
static void server_flush_world(Server* s, bool force)
{
    if (!s->overlay_active) return;
    if (!force && !s->overlay_dirty) return;
    if (overlay_save(&s->overlay, s->save_path)) {
        s->overlay_dirty = false;
        fprintf(stderr, "[server] world saved (%zu edits) -> %s\n",
                overlay_count(&s->overlay), s->save_path);
    } else {
        fprintf(stderr, "[server] WARNING: failed to save world to %s\n",
                s->save_path);
    }
}

/* ------------------------------------------------------------------ */
/*  Chunk streaming (remote clients)                                   */
/* ------------------------------------------------------------------ */

/* Append a coord to a client's streamed set (grows the array as needed). */
static void streamed_add(ServerClient* c, ChunkCoord cc) {
    if (c->streamed_count == c->streamed_cap) {
        size_t ncap = c->streamed_cap ? c->streamed_cap * 2 : 256;
        ChunkCoord* n = realloc(c->streamed, ncap * sizeof(ChunkCoord));
        if (!n) return;   /* OOM: skip recording; the chunk simply restreams */
        c->streamed = n;
        c->streamed_cap = ncap;
    }
    c->streamed[c->streamed_count++] = cc;
}

/* Remove a coord from a client's streamed set (swap-with-last; order in the set
 * is irrelevant to chunk_stream_diff). */
static void streamed_remove(ServerClient* c, int32_t cx, int32_t cz) {
    for (size_t i = 0; i < c->streamed_count; i++) {
        if (c->streamed[i].cx == cx && c->streamed[i].cz == cz) {
            c->streamed[i] = c->streamed[--c->streamed_count];
            return;
        }
    }
}

/* Encode column (cx,cz) and send it to client c as fragmented PKT_CHUNK_DATA on
 * the reliable channel. Returns true if streamed (and should be recorded), false
 * if skipped (oversized incompressible column — real terrain never hits this).  */
static bool server_send_chunk(Server* s, ServerClient* c, int32_t cx, int32_t cz) {
    uint8_t blocks[CHUNK_BLOCKS];
    if (!world_copy_chunk_blocks(s->world, cx, cz, blocks, sizeof blocks))
        return false;

    /* Worst-case encoded body (chunkwire_encode_bound()): 12-byte header +
     * 2*CHUNK_BLOCKS + slack. Static, so it never lands on the stack. The
     * function is the source of truth; this buffer is sized to its known upper
     * bound and asserted at runtime. */
    static uint8_t body[12 + 2 * CHUNK_BLOCKS + 16];
    size_t body_len = chunkwire_encode_chunk(cx, cz, blocks, body, sizeof body);
    if (body_len == 0) return false;

    uint16_t total = (uint16_t)((body_len + CHUNK_DATA_FRAG_BYTES - 1)
                                / CHUNK_DATA_FRAG_BYTES);
    if (total == 0) total = 1;
    if (total > SERVER_STREAM_MAX_FRAGS) {
        fprintf(stderr, "[server] chunk (%d,%d) encodes to %zu bytes (%u frags > %d); "
                        "skipping stream (incompressible column)\n",
                cx, cz, body_len, total, SERVER_STREAM_MAX_FRAGS);
        return false;
    }

    uint16_t msg_id = c->chunk_msg_id++;
    for (uint16_t i = 0; i < total; i++) {
        size_t off = (size_t)i * CHUNK_DATA_FRAG_BYTES;
        size_t flen = body_len - off;
        if (flen > CHUNK_DATA_FRAG_BYTES) flen = CHUNK_DATA_FRAG_BYTES;

        uint8_t pkt[RELIABLE_MAX_PAYLOAD];
        PacketHeader h = { .type = PKT_CHUNK_DATA, .player_id = 0 };
        size_t plen = net_write_chunk_data_frag(pkt, &h, msg_id, i, total,
                                                body + off, flen);
        send_reliable(s, c, pkt, (uint16_t)plen);
    }
    return true;
}

/* Per-tick chunk streaming for one REMOTE client: diff the client's render disc
 * against what we've already sent, on-demand generate + send new columns (budget
 * capped), and unload columns that left range. No-op for the shared-world host
 * (it renders the server world directly) and before the first position. */
static void server_stream_chunks(Server* s, ServerClient* c) {
    if (!s->world) return;
    if (c->shares_world) return;        /* host renders the world in-process */
    if (!c->position_received) return;

    int rd = c->stream_rd > 0 ? c->stream_rd
                              : world_get_render_distance(s->world);
    /* Match world_update's chunk-of-position floor division exactly. */
    int ccx = (int)floorf(c->x / 16.0f);
    int ccz = (int)floorf(c->z / 16.0f);

    /* Bound the per-tick work: at most SERVER_STREAM_BUDGET sends, plus any
     * number of (cheap) unloads. */
    ChunkCoord to_send[SERVER_STREAM_BUDGET];
    static ChunkCoord to_unload[4096];
    size_t ns = 0, nu = 0;
    chunk_stream_diff(ccx, ccz, rd,
                      c->streamed, c->streamed_count,
                      to_send, SERVER_STREAM_BUDGET, &ns, SERVER_STREAM_BUDGET,
                      to_unload, sizeof(to_unload) / sizeof(to_unload[0]), &nu);

    /* Unload departed chunks first (frees the client's memory + our set entry). */
    for (size_t i = 0; i < nu; i++) {
        uint8_t buf[HEADER_WIRE_SIZE + 8];
        PacketHeader h = { .type = PKT_CHUNK_UNLOAD, .player_id = 0 };
        size_t len = net_write_chunk_unload(buf, &h, to_unload[i].cx, to_unload[i].cz);
        send_reliable(s, c, buf, (uint16_t)len);
        streamed_remove(c, to_unload[i].cx, to_unload[i].cz);
    }

    /* Send new in-range columns (nearest-first), recording each as streamed. */
    for (size_t i = 0; i < ns; i++) {
        if (server_send_chunk(s, c, to_send[i].cx, to_send[i].cz))
            streamed_add(c, to_send[i]);
        /* If skipped (oversized), don't record it — but to avoid retrying it
         * every tick forever, record it anyway as "handled"; the client just
         * won't have that one rare column. We choose to record so the stream
         * makes forward progress and doesn't wedge on a pathological column. */
        else
            streamed_add(c, to_send[i]);
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
            else if (type == PKT_KEEPALIVE)    handle_keepalive(s, c, msg->data, msg->len);
            else if (type == PKT_DISCONNECT)   disconnect_client(s, c);
            else if (type == PKT_BLOCK_BREAK)  handle_block_break(s, c, msg->data, (size_t)msg->len);
            else if (type == PKT_BLOCK_PLACE)  handle_block_place(s, c, msg->data, (size_t)msg->len);
            else if (type == PKT_MOB_ATTACK)   handle_mob_attack(s, c, msg->data, (size_t)msg->len);
            else if (type == PKT_CRAFT)        handle_craft(s, c, msg->data, (size_t)msg->len);
            else if (type == PKT_EQUIP)        handle_equip(s, c, msg->data, (size_t)msg->len);
            else if (type == PKT_EAT)          handle_eat(s, c, msg->data, (size_t)msg->len);
        }
        free(msg);
    }

    /* Only remaining steps at 20 Hz */
    if (tick_num % 1 != 0) return; /* always true — placeholder for future rate div */

    /* Advance the day/night clock. u32 wrap is harmless: daynight_phase01
     * takes it modulo DAY_LENGTH_TICKS. */
    s->world_ticks++;

    /* Compute once per tick; shared by terrain streaming (here) and mob
     * simulation/spawning (Task 5). */
    vec3 anchor;
    server_anchor(s, anchor);

    /* Stream terrain around the anchor so mobs have ground to walk on.
     * In host shared-world mode the renderer's main thread drives the chunk
     * pipeline (world_update) on this same world — exactly one thread may do
     * that — so we skip it here and only read blocks (mob/collision queries).
     * The main thread anchors world_update on the player's position, which is
     * the same anchor, so terrain is loaded around the player either way. */
    if (s->world && s->drives_world_update) {
        world_update(s->world, /*bp=*/NULL, anchor);
    }

    /* Survival: hunger, fall/drown/lava damage, regen — per connected client. */
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        ServerClient* c = &s->clients[i];
        if (!c->active) continue;
        server_simulate_survival(s, c, 1.0f / SERVER_TICK_RATE);
    }

    /* Chunk streaming: push terrain to each REMOTE client around its position
     * (budget-capped per tick), unloading chunks that left range. No-op for the
     * shared-world host. Runs every tick; the budget bounds the work. */
    if (s->world) {
        for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
            ServerClient* c = &s->clients[i];
            if (!c->active) continue;
            server_stream_chunks(s, c);
        }
    }

    if (s->world) {
        server_simulate_mobs(s, 1.0f / SERVER_TICK_RATE);

        /* Hostile mobs only spawn in darkness. Only accumulate/attempt the
         * spawn timer while dark so a dusk transition doesn't immediately
         * dump a burst of mobs from time built up during the day. Only
         * MOB_ZOMBIE exists today (all hostile), so gating the whole path
         * is correct; gate per-type once passive mobs arrive. */
        if (daynight_is_dark(s->world_ticks)) {
            s->mob_spawn_timer += 1.0f / SERVER_TICK_RATE;
            if (s->mob_spawn_timer >= MOB_SPAWN_INTERVAL) {
                s->mob_spawn_timer = 0.0f;
                server_try_spawn(s, anchor);
            }
        }

        /* Passive farm animals spawn in daylight, on a separate timer/budget so
         * they don't compete with the hostile path. Only accumulate while it is
         * day, so a dawn transition doesn't dump a burst built up overnight. */
        if (!daynight_is_dark(s->world_ticks)) {
            s->passive_spawn_timer += 1.0f / SERVER_TICK_RATE;
            if (s->passive_spawn_timer >= PASSIVE_SPAWN_INTERVAL) {
                s->passive_spawn_timer = 0.0f;
                server_try_spawn_passive(s, anchor);
            }
        }
    }

    /* Periodic world flush. */
    s->save_timer += 1.0f / SERVER_TICK_RATE;
    if (s->save_timer >= SERVER_SAVE_INTERVAL) {
        s->save_timer = 0.0f;
        server_flush_world(s, /*force=*/false);
    }

    /* On-demand flush requested from the UI thread (Save / Save & Quit). Force
     * the write even if nothing changed so the player gets clear feedback. */
    if (atomic_exchange(&g_server_save_req, false)) {
        s->save_timer = 0.0f;
        server_flush_world(s, /*force=*/true);
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

    /* Per-stream broadcast sequence numbers for unreliable-snapshot dedup.
     * Incremented once per emitted packet so the client can drop stale
     * reordered datagrams (see seq_is_newer in net.h). One server runs per
     * process, so process-static counters suffice and keep server.h untouched. */
    static uint32_t world_state_bseq = 0;
    static uint32_t mob_state_bseq   = 0;

    uint8_t buf[NET_MAX_PACKET];
    PacketHeader hdr = { .type = PKT_WORLD_STATE, .player_id = 0 };
    size_t len = net_write_world_state(buf, &hdr, world_state_bseq++,
                                       players, count, s->world_ticks);
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
        size_t mlen = net_write_mob_state(mbuf, &mhdr, mob_state_bseq++,
                                          mobs_wire, mc);
        for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
            if (!s->clients[i].active) continue;
            net_thread_push_outbound(s->net, mbuf, (int)mlen, &s->clients[i].addr);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

void server_run(uint16_t port, int max_clients, int seed, const char* save_path)
{
    server_run_ex(port, max_clients, seed, save_path, NULL, 0);
}

void server_run_ex(uint16_t port, int max_clients, int seed,
                   const char* save_path, Renderer* renderer,
                   int render_distance)
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
    atomic_store(&g_server_stop, false);   /* reset for a clean (re)start */
    atomic_store(&g_server_save_req, false);

    s.seed  = seed;

    /* Start the world at noon (phase 0.25) so a fresh server begins in
     * full daylight rather than mid-transition. */
    s.world_ticks = DAY_LENGTH_TICKS / 4;

    /* ---- World persistence: load (or start) the block-delta overlay ----
     * Use the caller-chosen world path (saves/<name>/world.dat) when given;
     * fall back to the legacy single-world file otherwise. */
    snprintf(s.save_path, sizeof(s.save_path), "%s",
             (save_path && save_path[0]) ? save_path : SERVER_SAVE_FILE);
    s.overlay_active = true;
    if (overlay_load(&s.overlay, s.save_path)) {
        if (overlay_seed(&s.overlay) != seed) {
            /* Saved world is for a different seed — keep the saved edits but
             * warn; replaying deltas from another seed is still well-defined
             * (they overwrite by coordinate), just possibly surprising. */
            fprintf(stderr, "[server] WARNING: %s seed %d != requested seed %d; "
                            "using saved edits anyway\n",
                    s.save_path, overlay_seed(&s.overlay), seed);
        }
        fprintf(stderr, "[server] loaded world: %zu persisted edits from %s\n",
                overlay_count(&s.overlay), s.save_path);
    } else {
        overlay_init(&s.overlay, seed);
        fprintf(stderr, "[server] no saved world at %s; starting fresh\n",
                s.save_path);
    }

    /* Host shared-world: when the caller hands us a renderer we create the
     * world WITH it (so chunk meshes upload to the GPU) at the client's render
     * distance, and the host's main/render thread will drive the chunk
     * pipeline — so we must NOT also call world_update from this thread.
     * Dedicated/headless (renderer == NULL) keeps the legacy behaviour: we own
     * the pipeline at the small mob render distance. */
    s.drives_world_update = (renderer == NULL);
    int rd = (renderer != NULL && render_distance > 0)
           ? render_distance : 8 /* SERVER_MOB_RENDER_DIST */;

    World* w = world_create(renderer, seed, rd);
    /* Attach the persistence overlay BEFORE publishing the world, so that
     * every chunk the generation workers produce has the player's saved edits
     * replayed during WORK_GENERATE (overlay_apply_chunk) — before the chunk
     * advances to GENERATED/LIT/MESHING. The host renderer only ever meshes a
     * chunk once it reaches LIT, which is strictly after generation+overlay, so
     * there is no window where the client could mesh pre-overlay terrain. */
    world_set_overlay(w, &s.overlay);
    s.world = w;
    atomic_store_explicit(&g_server_world, w, memory_order_release);
    mob_set_init(&s.mobs);

    printf("[server] listening on port %d (max %d clients)\n", port, s.max_clients);

    double last = net_time();
    const double tick_dt = 1.0 / SERVER_TICK_RATE;
    double accum = 0.0;
    int tick_num = 0;

    while (s.running && !atomic_load(&g_server_stop)) {
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

    /* Release any per-client streamed-chunk sets still allocated (clients that
     * never disconnected before the loop exited). */
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        free(s.clients[i].streamed);
        s.clients[i].streamed = NULL;
    }

    /* Final save before teardown. Detach the overlay from the world first so
     * no worker can read it while we free it (workers stop in world_destroy). */
    server_flush_world(&s, /*force=*/true);
    /* Unpublish before destroying so a late server_get_world() can't hand the
     * host a dangling pointer. In host mode the main thread has already stopped
     * touching the world by the time it joins this thread (see main.c
     * shutdown order). */
    atomic_store_explicit(&g_server_world, NULL, memory_order_release);
    if (s.world) {
        world_set_overlay(s.world, NULL);
        world_destroy(s.world);
    }
    if (s.overlay_active) overlay_free(&s.overlay);
    net_thread_stop(&nt);
    net_socket_close(fd);
}
