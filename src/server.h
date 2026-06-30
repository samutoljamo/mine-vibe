#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <stdatomic.h>
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
#include "weather.h"        /* server-authoritative WeatherState */
#include "platform_thread.h" /* PT_Mutex for the deferred test-backdoor queue */

/* TEST-ONLY: a single deferred backdoor op (give/tp/spawn/set_*). The headless
 * harness runs on a DIFFERENT thread than the server loop; applying these
 * directly (the old behaviour) raced the server thread's use of the same
 * authoritative state + reliable send channel, intermittently corrupting the
 * reliable window so inventory/snapshot packets were lost — the scenario
 * flakiness. Instead the harness ENQUEUES an op and the server thread drains +
 * applies it at the top of server_tick, so all authoritative mutation and
 * reliable sends happen on one thread. */
typedef enum {
    SBD_NONE = 0,
    SBD_GIVE, SBD_TP, SBD_SPAWN_MOB,
    SBD_SET_TIME, SBD_SET_WEATHER, SBD_SET_FOOD, SBD_SET_HEALTH
} ServerBackdoorKind;

typedef struct {
    ServerBackdoorKind kind;
    int   ia, ib;        /* generic int args (item/count, type, ticks, kind, value) */
    float fx, fy, fz;    /* generic float args (positions)                          */
} ServerBackdoorOp;

#define SERVER_BACKDOOR_QUEUE_CAP 64

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

    /* Monotonic server-tick counter, incremented exactly once per server_tick
     * and NEVER reset/mutated by test backdoors (unlike world_ticks, which
     * server_test_set_time clobbers). The headless harness reads this via
     * server_current_tick() to step synchronously on the server thread's real
     * progress instead of racing a fixed wall-clock sleep. Atomic so the
     * harness (client thread) and server thread can read/write it concurrently;
     * a relaxed read is fine — it's a one-way barrier, not a lock. */
    atomic_uint_least64_t tick_count;

    /* TEST-ONLY deferred backdoor queue (see ServerBackdoorOp). Mutex-guarded
     * ring; producer = harness thread (enqueue), consumer = server thread
     * (drain at top of server_tick). Empty/unused in production. */
    PT_Mutex          backdoor_lock;
    ServerBackdoorOp  backdoor_queue[SERVER_BACKDOOR_QUEUE_CAP];
    int               backdoor_head;   /* next slot to write */
    int               backdoor_count;  /* pending ops        */
    bool              backdoor_inited;  /* mutex initialized  */

    /* Server-authoritative weather. Seeded from the world seed at startup and
     * ticked once per server tick (the rng stays here, never sent). Only the
     * observable {kind,time_left} is broadcast (PKT_WEATHER): on every KIND
     * transition, once to each late-joiner, and periodically for resync. */
    WeatherState weather;
    WeatherKind  weather_last_kind;    /* last broadcast kind; transition detect */
    float        weather_resync_timer; /* accumulates toward a low-freq resync   */

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
                   int render_distance, GameMode gamemode);

/* Returns the server's authoritative World* once the server loop has created
 * it, or NULL before that (and after teardown). Thread-safe; the host spins on
 * this briefly after starting the server thread. The returned world is owned
 * by the server — the caller must NOT destroy it. */
World* server_get_world(void);

/* TEST-ONLY BACKDOOR. Returns the live in-process Server* once server_run_ex has
 * created it (NULL before/after). The integrated headless harness uses this to
 * apply test helpers (give/tp/spawn_mob/set_time/set_weather) directly to the
 * authoritative state — there is no wire packet for these, and the server thread
 * is in the same process. NOT for gameplay; production code must go through the
 * client->server packet path. Thread-safe publish; the harness owns the timing. */
Server* server_get_instance(void);

/* Monotonic server-tick count: increments exactly once per server_tick on the
 * server thread and never decreases (test backdoors do NOT touch it). The
 * headless harness uses this as a synchronization barrier — it records T0, then
 * blocks until server_current_tick() >= T0 + N — so a `step N` reflects the
 * server having genuinely processed N ticks rather than a guessed sleep.
 * Thread-safe (relaxed atomic read). Returns 0 if s is NULL. */
uint64_t server_current_tick(const Server* s);

/* The actual UDP port the server bound to (published once the socket is bound).
 * When server_run_ex is started with port 0 the OS assigns an ephemeral port —
 * the headless harness uses this so back-to-back / parallel runs never collide
 * on a fixed port or a lingering TIME_WAIT socket. The in-process harness client
 * spins on this to learn where to connect. Returns 0 before bind / after
 * teardown. Thread-safe (acquire atomic). */
uint16_t server_get_port(void);

/* TEST-ONLY backdoor mutators. Apply directly to the authoritative state of the
 * (single) integrated client / world and request the same client snapshots a
 * real action would. Used only by the headless harness; there is no wire packet
 * for these. All return false (or 0) if no server/client is present. */
bool server_test_give(Server* s, int item, int count);
bool server_test_tp(Server* s, float x, float y, float z);
int  server_test_spawn_mob(Server* s, int type, float x, float y, float z);
bool server_test_set_time(Server* s, uint32_t ticks);
bool server_test_set_weather(Server* s, int kind);
bool server_test_set_food(Server* s, int food);
bool server_test_set_health(Server* s, int hp);

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
