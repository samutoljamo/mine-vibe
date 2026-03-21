#ifndef CLIENT_H
#define CLIENT_H

#define CLIENT_MAX_CONNECT_ATTEMPTS 10

#include <stdint.h>
#include <stdbool.h>
#include <netinet/in.h>
#include "net.h"
#include "reliable.h"
#include "net_thread.h"

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
} Client;

void client_init(Client* c, NetThread* net,
                 const struct sockaddr_in* server_addr);
void client_destroy(Client* c);

/* Send PKT_CONNECT_REQUEST. Call once after client_init. */
void client_connect(Client* c);

/* Send current player position to the server. Call once per frame. */
void client_send_position(Client* c,
                           float x, float y, float z,
                           float yaw, float pitch);

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

void client_disconnect(Client* c);

#endif /* CLIENT_H */
