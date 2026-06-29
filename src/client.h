#ifndef CLIENT_H
#define CLIENT_H

#define CLIENT_MAX_CONNECT_ATTEMPTS 10

/* Dead-server detection + keepalive (mine-vibe-6dp). The client sends a
 * header-only PKT_KEEPALIVE every CLIENT_KEEPALIVE_INTERVAL seconds whenever it
 * is connected, so an idle connection keeps the NAT pinhole open and the server
 * keeps seeing traffic (its own SERVER_TIMEOUT_SEC won't drop us). If NO inbound
 * datagram arrives from the server for CLIENT_SERVER_TIMEOUT_SEC, the client
 * treats the server as gone and disconnects cleanly. The interval is well under
 * both timeouts and the typical ~30s UDP NAT mapping lifetime. */
#define CLIENT_KEEPALIVE_INTERVAL  3.0
#define CLIENT_SERVER_TIMEOUT_SEC  10.0

#include <stdint.h>
#include <stdbool.h>
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <netinet/in.h>
#endif
#include "net.h"
#include "reliable.h"
#include "net_thread.h"
#include "inventory.h"
#include "mob.h"
#include "chunk_reasm.h"

typedef enum {
    CLIENT_DISCONNECTED,
    CLIENT_CONNECTING,   /* sent connect request, awaiting accept */
    CLIENT_CONNECTED,
} ClientState;

typedef struct {
    NetThread*   net;
    ClientState  state;
    uint8_t      local_player_id;  /* assigned by server */
    uint32_t     tick;             /* monotonic position tick counter */

    struct sockaddr_in server_addr;

    ReliableChannel reliable;

    double connect_sent_time;
    int   connect_attempts;  /* incremented on each retry; 0 = first send */

    /* True when this client is the integrated host that renders the server's
     * world in-process (host/singleplayer). Sent in PKT_CONNECT_REQUEST so the
     * server does NOT stream chunks to us (we already share the world). A true
     * remote (--client) leaves this false and receives streamed chunks. */
    bool  shared_world;
    /* Render distance (chunks) advertised to the server so it streams the disc
     * we actually render (clamped to the server's cap). 0 = let the server pick. */
    uint8_t render_distance;

    Inventory inventory;

    int16_t health;   /* last server-reported; PLAYER_MAX_HEALTH at init */
    uint8_t food;     /* 0..20 hunger, last server-reported (v3 health pkt) */
    uint8_t air;      /* 0..20 oxygen bubbles, last server-reported         */

    /* Equipped armour, mirrored from the server for the HUD (PKT_ARMOR).
     * armor[ARMOR_SLOT_*] holds the worn item id (BLOCK_AIR = empty);
     * armor_points is the server's clamped total used to size the armour bar. */
    ItemId  armor[ARMOR_SLOT_COUNT];
    uint8_t armor_points;

    /* Day/night clock, server-authoritative. world_ticks is the last value
     * received in a PKT_WORLD_STATE; world_ticks_recv_time is when (monotonic
     * net_time seconds). The renderer estimates the current time-of-day by
     * extrapolating from this anchor at SERVER_TICK_RATE so the sky animates
     * smoothly between (and through dropped) packets. Initialized to noon to
     * avoid a one-frame dawn flash before the first packet arrives. */
    uint32_t world_ticks;
    double   world_ticks_recv_time;

    /* Last-applied per-stream broadcast sequence numbers (dedup of unreliable
     * snapshots). PKT_WORLD_STATE / PKT_MOB_STATE are sent unreliable, so a
     * reordered/late datagram can carry a stale snapshot; we drop any packet
     * whose seq is not seq_is_newer than the last applied. *_seq_valid is false
     * until the first packet of that stream arrives (seq 0 is itself valid). */
    uint32_t world_state_seq;
    uint32_t mob_state_seq;
    bool     world_state_seq_valid;
    bool     mob_state_seq_valid;

    /* Block-change events received from the server, drained by main.c each
     * frame to call world_set_block + remesh on the main thread.
     * (Network thread cannot remesh — meshing is not thread-safe with the
     * renderer reading the world.) */
    struct {
        int     x, y, z;
        uint8_t block;
    } pending_block_changes[256];
    int pending_block_change_count;

    /* Reassembly ring for fragmented PKT_CHUNK_DATA columns. Each fragment is an
     * ordinary (acked + retransmitted) reliable packet carrying a
     * {msg_id,index,total} subheader; we reassemble per msg_id at our own
     * CHUNK_DATA_FRAG_BYTES stride. A few concurrent in-flight columns are kept
     * so that under loss/reorder a retransmitted fragment of an older column
     * interleaved with a newer one reassembles independently instead of being
     * dropped (mine-vibe-003). Memory is bounded; oldest partial is evicted. */
    ChunkReasmRing chunk_reasm;

    /* Dead-server detection (mine-vibe-6dp). last_server_recv_time is the
     * monotonic net_time() of the most recent inbound datagram from the server;
     * if it goes stale for CLIENT_SERVER_TIMEOUT_SEC we surface a clean
     * disconnect instead of hanging. last_keepalive_time gates the periodic
     * outbound keepalive that holds the NAT pinhole open while idle. */
    double last_server_recv_time;
    double last_keepalive_time;

    /* Last container snapshot received (PKT_CONTAINER_STATE). The full
     * interactive container UI is a separate ticket (a4s.6.4); this client just
     * stores the most recent state so the UI/agent can read it. `open` is true
     * once the local player has opened a container and not yet closed it. */
    bool                 container_open;
    ContainerStatePacket container;   /* valid when container_open */

    /* Pending knockback impulse from the server (PKT_KNOCKBACK), in blocks/s,
     * world space. The local player owns its position, so a server shove arrives
     * as its own packet and is accumulated here; main.c drains it each frame
     * (kb_pending -> false) into a short decaying positional nudge on the player.
     * Accumulated so several hits in one frame stack instead of clobbering. */
    bool  kb_pending;
    float kb_dx, kb_dy, kb_dz;
} Client;

void client_init(Client* c, NetThread* net,
                 const struct sockaddr_in* server_addr);
void client_destroy(Client* c);

/* Mark this client as the integrated shared-world host (or not). Call before
 * client_connect. Defaults to false (remote client). */
void client_set_shared_world(Client* c, bool shared);

/* Advertise this client's render distance (chunks) to the server for chunk
 * streaming. Call before client_connect. 0 (default) lets the server choose. */
void client_set_render_distance(Client* c, int rd);

/* Send PKT_CONNECT_REQUEST. Call once after client_init. */
void client_connect(Client* c);

/* Send current player position to the server. Call once per frame. */
void client_send_position(Client* c,
                           float x, float y, float z,
                           float yaw, float pitch);

/* Send a reliable break request for the cell (x,y,z). `block` is the block
 * the client believes occupies the cell — the server validates it for inventory
 * crediting. `slot` is the held hotbar slot, so the server can wear the tool. */
void client_send_break(Client* c, int x, int y, int z, uint8_t block,
                       uint8_t slot);

/* Send a reliable place request: place the inventory item at slot `slot`
 * on the face `face` of the cell (x,y,z). */
void client_send_place(Client* c, int x, int y, int z,
                        uint8_t face, uint8_t slot);

/* Send a reliable mob attack request for the given mob id. */
void client_send_mob_attack(Client* c, uint16_t mob_id);

/* Send a reliable craft request for the recipe at `recipe_index` in the shared
 * crafting table. The server validates inputs and replies with PKT_INVENTORY. */
void client_send_craft(Client* c, uint16_t recipe_index);

/* Send a reliable equip request: equip the armour item held in inventory slot
 * `slot` into its body slot. The server validates and replies with fresh
 * PKT_INVENTORY + PKT_ARMOR snapshots. */
void client_send_equip(Client* c, uint8_t slot);

/* Send a reliable eat request: eat the food item held in inventory slot `slot`.
 * The server validates it is a food item and the player isn't full, consumes
 * one, restores hunger, and replies with fresh PKT_INVENTORY + PKT_PLAYER_HEALTH
 * snapshots. */
void client_send_eat(Client* c, uint8_t slot);

/* Open the container block-entity (furnace/chest) at (x,y,z): sends a reliable
 * PKT_CONTAINER_OPEN. The server validates reach and replies with
 * PKT_CONTAINER_STATE, which client_poll stores in c->container. */
void client_send_container_open(Client* c, int x, int y, int z);

/* Stop viewing the container at (x,y,z): sends a reliable PKT_CONTAINER_CLOSE
 * and clears the local open flag. */
void client_send_container_close(Client* c, int x, int y, int z);

/* Move `count` items between the container at (x,y,z) and the player inventory.
 * `dir` is a ContainerNetDir (CONTAINER_DIR_TO_INV moves a container slot to the
 * inventory; CONTAINER_DIR_FROM_INV moves an inventory slot into the container).
 * The server validates + applies the move and resends container + inventory. */
void client_send_container_action(Client* c, int x, int y, int z,
                                  uint8_t slot, uint8_t dir, uint8_t count);

/* Process all inbound messages from the net thread.
 * Returns number of PKT_WORLD_STATE packets processed. */
int client_poll(Client* c);

/* Remote player state parsed from a world state packet */
typedef struct {
    uint8_t player_id;
    float   x, y, z;
    float   yaw, pitch;
    double  recv_time;
} ClientPlayerSnapshot;

typedef void (*ClientSnapshotCb)(const ClientPlayerSnapshot* snap, void* user);
void client_set_snapshot_cb(Client* c, ClientSnapshotCb cb, void* user);

typedef void (*ClientLeaveCb)(uint8_t player_id, void* user);
void client_set_leave_cb(Client* c, ClientLeaveCb cb, void* user);

typedef void (*ClientMobsCb)(const ClientMobSnapshot* mobs, int count,
                             double recv_time, void* user);
void client_set_mobs_cb(Client* c, ClientMobsCb cb, void* user);

typedef void (*ClientDeathCb)(void* user);
void client_set_death_cb(Client* c, ClientDeathCb cb, void* user);

/* Streamed chunk received from the server (PKT_CHUNK_DATA, reassembled +
 * RLE-decoded). `blocks` is CHUNK_BLOCKS bytes in flat x + z*16 + y*256 order;
 * valid only for the duration of the callback (copy if retained). */
typedef void (*ClientChunkCb)(int32_t cx, int32_t cz,
                              const uint8_t* blocks, size_t blocks_len,
                              void* user);
void client_set_chunk_cb(Client* c, ClientChunkCb cb, void* user);

/* Server told us to drop a now-distant chunk (PKT_CHUNK_UNLOAD). */
typedef void (*ClientChunkUnloadCb)(int32_t cx, int32_t cz, void* user);
void client_set_chunk_unload_cb(Client* c, ClientChunkUnloadCb cb, void* user);

/* Smoothed estimate of the current server world_ticks, extrapolated from the
 * last received anchor at the server tick rate. Use this (not c->world_ticks
 * directly) to drive the sky so it animates between packets and rides through
 * dropped ones. Re-anchors on every PKT_WORLD_STATE; safe across u32 wrap. */
uint32_t client_estimate_world_ticks(const Client* c);

void client_disconnect(Client* c);

#endif /* CLIENT_H */
