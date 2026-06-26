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
#include "mob.h"

#define SERVER_MAX_CLIENTS  32
#define SERVER_TICK_RATE    20     /* Hz */
#define SERVER_TIMEOUT_SEC  10.0

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
} Server;

/* Blocking server loop — call from a dedicated thread or main().
 * port: UDP port to bind. max_clients: runtime cap.
 * Runs until server.running is set false (or fatal error). */
void server_run(uint16_t port, int max_clients, int seed);

#endif /* SERVER_H */
