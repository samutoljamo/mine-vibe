#ifndef CLIENT_H
#define CLIENT_H

#define CLIENT_MAX_CONNECT_ATTEMPTS 10

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

    Inventory inventory;

    /* Block-change events received from the server, drained by main.c each
     * frame to call world_set_block + remesh on the main thread.
     * (Network thread cannot remesh — meshing is not thread-safe with the
     * renderer reading the world.) */
    struct {
        int     x, y, z;
        uint8_t block;
    } pending_block_changes[256];
    int pending_block_change_count;
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

/* Send a reliable break request for the cell (x,y,z). `block` is the block
 * the client believes occupies the cell — the server uses it for inventory
 * crediting. */
void client_send_break(Client* c, int x, int y, int z, uint8_t block);

/* Send a reliable place request: place the inventory item at slot `slot`
 * on the face `face` of the cell (x,y,z). */
void client_send_place(Client* c, int x, int y, int z,
                        uint8_t face, uint8_t slot);

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

void client_disconnect(Client* c);

#endif /* CLIENT_H */
