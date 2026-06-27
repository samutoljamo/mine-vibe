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
} ServerClient;

typedef struct {
    NetThread*   net;
    ServerClient clients[SERVER_MAX_CLIENTS];
    int          max_clients;    /* runtime cap, <= SERVER_MAX_CLIENTS */
    bool         running;
    World*       world;          /* headless; mob terrain + collision      */
    int          seed;
    MobSet       mobs;           /* (added/used in Task 5)                  */
    float        mob_spawn_timer; /* accumulates time toward MOB_SPAWN_INTERVAL */
    uint32_t     world_ticks;    /* day/night clock; advances once per tick  */

    /* World persistence: block-delta overlay + periodic-flush bookkeeping. */
    BlockOverlay overlay;
    bool         overlay_active;  /* false if persistence is disabled        */
    bool         overlay_dirty;   /* edits since last flush                   */
    float        save_timer;      /* accumulates time toward SERVER_SAVE_INTERVAL */
    char         save_path[1024];
} Server;

/* Blocking server loop — call from a dedicated thread or main().
 * port: UDP port to bind. max_clients: runtime cap.
 * Runs until server.running is set false (or fatal error). */
void server_run(uint16_t port, int max_clients, int seed);

/* Signal the running server loop (on its own thread in host mode) to exit, so
 * the main thread can join it cleanly on shutdown. Thread-safe. */
void server_request_stop(void);

#endif /* SERVER_H */
