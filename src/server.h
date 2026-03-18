#ifndef SERVER_H
#define SERVER_H

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include "reliable.h"
#include "net_thread.h"

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
    ReliableChannel    reliable;
} ServerClient;

typedef struct {
    NetThread*   net;
    ServerClient clients[SERVER_MAX_CLIENTS];
    int          max_clients;    /* runtime cap, <= SERVER_MAX_CLIENTS */
    bool         running;
} Server;

/* Blocking server loop — call from a dedicated thread or main().
 * port: UDP port to bind. max_clients: runtime cap.
 * Runs until server.running is set false (or fatal error). */
void server_run(uint16_t port, int max_clients);

#endif /* SERVER_H */
