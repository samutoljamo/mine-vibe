#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include <stdbool.h>
#ifdef _WIN32
#  include <winsock2.h>
#else
#  include <netinet/in.h>
#endif
#include "reliable.h"
#include "net_thread.h"
#include "inventory.h"
#include "world.h"
#include "world_persist.h"
#include "mob.h"
#include "survival.h"
#include "chunk_stream.h"   /* ChunkCoord + streaming-policy diff */
#include "worldsave.h"      /* GameMode + gamemode_allows_damage (read-only) */

typedef struct Renderer Renderer;

#define SERVER_MAX_CLIENTS  32
#define SERVER_TICK_RATE    20     /* Hz */
#define SERVER_TIMEOUT_SEC  10.0
#define SERVER_SAVE_INTERVAL 30.0f /* seconds between periodic world flushes  */
#define SERVER_SAVE_FILE    "world.dat"

typedef struct {
    bool               active;
    struct sockaddr_in addr;
    uint8_t            player_id;   /* 1–255 */
    double             last_recv_time;
    uint32_t           last_tick;
    float              x, y, z;
    float              yaw, pitch;
    bool               position_received;   /* set true on first PKT_POSITION; gates break/place */
    ReliableChannel    reliable;
    Inventory          inventory;
    ItemId             armor[ARMOR_SLOT_COUNT]; /* worn armour per slot; BLOCK_AIR = empty */
    uint16_t           armor_dur[ARMOR_SLOT_COUNT]; /* remaining durability of each worn piece */
    int16_t            health;
    double             last_attack_time;    /* rate-limit player→mob attacks (Task 8) */

    /* --- Survival mechanics (hunger, environment, death/respawn) --- */
    SurvivalState      survival;
    bool               prev_pos_valid;      /* have a previous position to diff */
    float              prev_x, prev_y, prev_z;
    float              fall_start_y;        /* y where the current fall began    */
    bool               falling;             /* tracking a descent for fall dmg   */
    float              respawn_grace;       /* seconds of post-respawn invuln     */
    bool               needs_health_sync;   /* hunger/health changed; push packet */

    /* --- Chunk streaming (remote clients only) --- */
    bool               shares_world;        /* integrated host: renders the server
                                             * world in-process, so we DON'T stream
                                             * chunks to it (set from the connect
                                             * handshake's shared_world flag).      */
    int                stream_rd;           /* render distance to stream around this
                                             * client (from the connect handshake;
                                             * clamped). 0 until first connect.     */
    ChunkCoord*        streamed;            /* coords already streamed to this client */
    size_t             streamed_count;
    size_t             streamed_cap;
    uint16_t           chunk_msg_id;        /* next PKT_CHUNK_DATA reassembly id    */
} ServerClient;

/* A container block-entity (furnace or chest) lives at a fixed block position
 * and owns its contents server-side. The full struct (which embeds the
 * smelting/container models) is defined privately in server.c, so server.h stays
 * free of those headers — and of their `ItemStack` typedef, which would clash
 * with crafting.h's identically-named one in any TU that includes both. The
 * Server here only holds an opaque pointer to a dynamic array of them. */
typedef struct BlockEntity BlockEntity;

typedef struct {
    NetThread*   net;
    ServerClient clients[SERVER_MAX_CLIENTS];
    int          max_clients;    /* runtime cap, <= SERVER_MAX_CLIENTS */
    bool         running;
    World*       world;          /* headless; mob terrain + collision      */
    int          seed;
    GameMode     gamemode;       /* per-world mode; gates ALL player damage.
                                  * Defaults to GAMEMODE_SURVIVAL (zero-init)
                                  * so current behaviour is unchanged until
                                  * main.c calls server_set_gamemode() after
                                  * loading WorldMeta. */
    MobSet       mobs;           /* (added/used in Task 5)                  */
    float        mob_spawn_timer; /* accumulates time toward MOB_SPAWN_INTERVAL */
    float        passive_spawn_timer; /* accumulates toward PASSIVE_SPAWN_INTERVAL */
    uint32_t     world_ticks;    /* day/night clock; advances once per tick  */

    /* Host shared-world mode: when a renderer is attached (host/singleplayer),
     * the renderer's main thread owns the chunk pipeline (it must own GPU
     * uploads anyway), so the server thread does NOT call world_update — it
     * only reads/writes blocks. false for dedicated servers, which drive the
     * pipeline themselves. */
    bool         drives_world_update;

    /* World persistence: block-delta overlay + periodic-flush bookkeeping. */
    BlockOverlay overlay;
    bool         overlay_active;  /* false if persistence is disabled        */
    bool         overlay_dirty;   /* edits since last flush                   */
    float        save_timer;      /* accumulates time toward SERVER_SAVE_INTERVAL */
    char         save_path[1024];

    /* Container block-entities (furnaces + chests). Created when a
     * BLOCK_FURNACE/BLOCK_CHEST is placed, destroyed (contents returned to the
     * breaker) when broken. Dynamic array, searched by block position. */
    BlockEntity* block_entities;
    size_t       block_entity_count;
    size_t       block_entity_cap;
} Server;

/* Blocking server loop — call from a dedicated thread or main().
 * port: UDP port to bind. max_clients: runtime cap. seed: worldgen seed.
 * save_path: overlay file to load/flush; if NULL, falls back to
 *   SERVER_SAVE_FILE (single hardcoded world, legacy behaviour).
 * Runs until server.running is set false (or fatal error). */
void server_run(uint16_t port, int max_clients, int seed, const char* save_path);

/* Host shared-world variant. When `renderer` is non-NULL, the server creates
 * its world WITH that renderer (so chunk meshes upload to the GPU) at
 * `render_distance`, and does NOT drive the chunk pipeline itself — the host's
 * main/render thread pumps world_update on the shared world. The host obtains
 * the live World* via server_get_world() once it is created. With
 * renderer == NULL this behaves exactly like server_run (headless, server
 * drives its own pipeline). */
void server_run_ex(uint16_t port, int max_clients, int seed,
                   const char* save_path, Renderer* renderer,
                   int render_distance);

/* Returns the server's authoritative World* once the server loop has created
 * it, or NULL before that (and after teardown). Thread-safe; the host spins on
 * this briefly after starting the server thread. The returned world is owned
 * by the server — the caller must NOT destroy it. */
World* server_get_world(void);

/* Set the world's game mode. In GAMEMODE_CREATIVE the server applies zero
 * player damage from every source (mob/fall/drown/lava/starve). main.c should
 * call this after loading WorldMeta, before/while the server loop runs. */
void server_set_gamemode(Server* s, GameMode gm);

/* Signal the running server loop (on its own thread in host mode) to exit, so
 * the main thread can join it cleanly on shutdown. Thread-safe. */
void server_request_stop(void);

/* Request a mid-game world flush from another thread (the UI's Save button).
 * The server loop notices the flag and forces an overlay save next tick.
 * Thread-safe; mirrors server_request_stop. */
void server_request_save(void);

#endif /* SERVER_H */
