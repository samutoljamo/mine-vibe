#include "client.h"
#include "net_thread.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static ClientSnapshotCb g_snap_cb   = NULL;
static void*             g_snap_user = NULL;
static ClientLeaveCb     g_leave_cb   = NULL;
static void*             g_leave_user = NULL;

void client_set_snapshot_cb(Client* c, ClientSnapshotCb cb, void* user)
{
    (void)c;
    g_snap_cb   = cb;
    g_snap_user = user;
}

void client_set_leave_cb(Client* c, ClientLeaveCb cb, void* user)
{
    (void)c;
    g_leave_cb   = cb;
    g_leave_user = user;
}

void client_init(Client* c, NetThread* net,
                  const struct sockaddr_in* server_addr)
{
    memset(c, 0, sizeof(*c));
    c->net         = net;
    c->state       = CLIENT_DISCONNECTED;
    c->server_addr = *server_addr;
    reliable_init(&c->reliable);
}

void client_destroy(Client* c) { (void)c; }

void client_connect(Client* c)
{
    uint8_t buf[HEADER_WIRE_SIZE];
    size_t off = 0;
    PacketHeader h = { .type = PKT_CONNECT_REQUEST, .player_id = 0 };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    net_write_header(buf, &off, &h);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr,
                   buf, (uint16_t)off);
    c->state            = CLIENT_CONNECTING;
    c->connect_sent_time = net_time();
    printf("[client] connecting...\n");
}

void client_send_position(Client* c,
                            float x, float y, float z,
                            float yaw, float pitch)
{
    if (c->state != CLIENT_CONNECTED) return;

    PositionPacket p = {0};
    p.header.type      = PKT_POSITION;
    p.header.player_id = c->local_player_id;
    p.header.seq       = c->tick;
    reliable_fill_ack(&c->reliable, &p.header.ack, &p.header.ack_bits);
    p.tick  = c->tick++;
    p.x     = x;
    p.y     = y;
    p.z     = z;
    p.yaw   = yaw;
    p.pitch = pitch;

    uint8_t buf[32];
    size_t len = net_write_position(buf, &p);
    net_thread_push_outbound(c->net, buf, (int)len, &c->server_addr);
}

int client_poll(Client* c)
{
    int state_packets = 0;
    NetMsg* msg;
    while ((msg = net_thread_pop_inbound(c->net)) != NULL) {
        if (msg->len < 1) { free(msg); continue; }
        uint8_t type = msg->data[0];

        if (type == PKT_CONNECT_ACCEPT && c->state == CLIENT_CONNECTING) {
            PacketHeader h;
            size_t off = 0;
            net_read_header(msg->data, &off, &h);
            reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            c->local_player_id = h.player_id;
            c->state           = CLIENT_CONNECTED;
            printf("[client] connected as player %d\n", c->local_player_id);

        } else if (type == PKT_WORLD_STATE && c->state == CLIENT_CONNECTED) {
            size_t off = 0;
            PacketHeader hdr;
            net_read_header(msg->data, &off, &hdr);
            uint8_t count = net_read_u8(msg->data, &off);
            for (int i = 0; i < count; i++) {
                uint8_t pid = net_read_u8(msg->data, &off);
                float x     = net_read_float(msg->data, &off);
                float y     = net_read_float(msg->data, &off);
                float z     = net_read_float(msg->data, &off);
                float yaw   = net_read_float(msg->data, &off);
                float pitch = net_read_float(msg->data, &off);

                /* Skip our own position — we already simulate it locally */
                if (pid == c->local_player_id) continue;

                if (g_snap_cb) {
                    ClientPlayerSnapshot snap = {
                        .player_id = pid, .x = x, .y = y, .z = z,
                        .yaw = yaw, .pitch = pitch,
                        .recv_time = msg->recv_time,
                    };
                    g_snap_cb(&snap, g_snap_user);
                }
            }
            state_packets++;

        } else if (type == PKT_PLAYER_JOIN || type == PKT_PLAYER_LEAVE) {
            PacketHeader h; size_t off = 0;
            net_read_header(msg->data, &off, &h);
            bool is_new = reliable_on_recv(&c->reliable, h.seq, h.ack, h.ack_bits);
            if (is_new) {
                if (type == PKT_PLAYER_JOIN)
                    printf("[client] player %d joined\n", h.player_id);
                else {
                    printf("[client] player %d left\n", h.player_id);
                    if (g_leave_cb) g_leave_cb(h.player_id, g_leave_user);
                }
            }

        } else if (type == PKT_DISCONNECT) {
            printf("[client] disconnected by server\n");
            c->state = CLIENT_DISCONNECTED;
        }
        free(msg);
    }

    /* Connect timeout: resend every 2s, give up after CLIENT_MAX_CONNECT_ATTEMPTS */
    if (c->state == CLIENT_CONNECTING
        && net_time() - c->connect_sent_time > 2.0) {
        if (c->connect_attempts >= CLIENT_MAX_CONNECT_ATTEMPTS) {
            fprintf(stderr, "[client] connect timed out after %d retries\n",
                    c->connect_attempts);
            c->state = CLIENT_DISCONNECTED;
        } else {
            printf("[client] retrying connect (attempt %d/%d)...\n",
                   c->connect_attempts + 1, CLIENT_MAX_CONNECT_ATTEMPTS);
            c->connect_attempts++;
            client_connect(c);
        }
    }

    return state_packets;
}

void client_disconnect(Client* c)
{
    if (c->state == CLIENT_DISCONNECTED) return;
    uint8_t buf[HEADER_WIRE_SIZE];
    size_t off = 0;
    PacketHeader h = { .type = PKT_DISCONNECT,
                        .player_id = c->local_player_id };
    reliable_fill_ack(&c->reliable, &h.ack, &h.ack_bits);
    net_write_header(buf, &off, &h);
    reliable_send(&c->reliable, c->net->fd, &c->server_addr,
                   buf, (uint16_t)off);
    c->state = CLIENT_DISCONNECTED;
}
