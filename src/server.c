#include "server.h"
#include "net.h"
#include "net_thread.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

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
}

/* ------------------------------------------------------------------ */
/*  Packet handling                                                    */
/* ------------------------------------------------------------------ */

static void handle_connect_request(Server* s, const struct sockaddr_in* addr)
{
    if (find_client_by_addr(s, addr)) return;

    ServerClient* c = alloc_client(s, addr);
    if (!c) {
        printf("[server] connection refused: server full\n");
        return;
    }
    printf("[server] player %d connected\n", c->player_id);

    uint8_t buf[HEADER_WIRE_SIZE];
    size_t off = 0;
    PacketHeader h = { .type = PKT_CONNECT_ACCEPT, .player_id = c->player_id };
    net_write_header(buf, &off, &h);
    send_reliable(s, c, buf, (uint16_t)off);

    broadcast_player_join(s, c);
}

static void handle_position(Server* s, ServerClient* c,
                              const uint8_t* data, int len)
{
    if (len < 32) return;
    PositionPacket p;
    net_read_position(data, &p);

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
            handle_connect_request(s, &msg->addr);
        } else if (c) {
            if      (type == PKT_POSITION)   handle_position(s, c, msg->data, msg->len);
            else if (type == PKT_DISCONNECT) disconnect_client(s, c);
        }
        free(msg);
    }

    /* Only remaining steps at 20 Hz */
    if (tick_num % 1 != 0) return; /* always true — placeholder for future rate div */

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

    uint8_t buf[NET_MAX_PACKET];
    PacketHeader hdr = { .type = PKT_WORLD_STATE, .player_id = 0 };
    size_t len = net_write_world_state(buf, &hdr, players, count);
    for (int i = 0; i < SERVER_MAX_CLIENTS; i++) {
        if (!s->clients[i].active) continue;
        net_thread_push_outbound(s->net, buf, (int)len, &s->clients[i].addr);
    }
}

/* ------------------------------------------------------------------ */
/*  Entry point                                                        */
/* ------------------------------------------------------------------ */

void server_run(uint16_t port, int max_clients)
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

    printf("[server] listening on port %d (max %d clients)\n", port, s.max_clients);

    double last = net_time();
    const double tick_dt = 1.0 / SERVER_TICK_RATE;
    double accum = 0.0;
    int tick_num = 0;

    while (s.running) {
        double now = net_time();
        accum += now - last;
        last   = now;
        if (accum > 0.2) accum = 0.2;

        while (accum >= tick_dt) {
            server_tick(&s, tick_num++);
            accum -= tick_dt;
        }

        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
    }

    net_thread_stop(&nt);
    net_socket_close(fd);
}
