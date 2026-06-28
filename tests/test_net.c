#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/net.h"
#include "../src/reliable.h"

static void test_serialize_position(void)
{
    PositionPacket p = {0};
    p.header.type      = PKT_POSITION;
    p.header.player_id = 3;
    p.header.seq       = 1000;
    p.header.ack       = 999;
    p.header.ack_bits  = 0xF0F0;
    p.tick  = 12345;
    p.x     = 1.0f;
    p.y     = 64.0f;
    p.z     = -5.5f;
    p.yaw   = 1.57f;
    p.pitch = -0.3f;

    uint8_t buf[64];
    size_t len = net_write_position(buf, &p);
    assert(len == 32);

    PositionPacket q = {0};
    net_read_position(buf, &q);
    assert(q.header.type      == PKT_POSITION);
    assert(q.header.player_id == 3);
    assert(q.header.seq       == 1000);
    assert(q.tick             == 12345);
    assert(q.x     == 1.0f);
    assert(q.y     == 64.0f);
    assert(q.z     == -5.5f);
    assert(q.yaw   == 1.57f);
    assert(q.pitch == -0.3f);
    printf("test_serialize_position: PASS\n");
}

static void test_reliable_ack(void)
{
    ReliableChannel ch;
    reliable_init(&ch);
    /* Simulate receiving seqs 0, 1, 2 */
    reliable_on_recv(&ch, 0, 0, 0);
    reliable_on_recv(&ch, 1, 0, 0);
    reliable_on_recv(&ch, 2, 0, 0);
    uint16_t ack, bits;
    reliable_fill_ack(&ch, &ack, &bits);
    assert(ack == 2);
    /* bits: seq 1 = diff 1, bit 0; seq 0 = diff 2, bit 1 */
    assert(bits & (1 << 0)); /* seq 1 received */
    assert(bits & (1 << 1)); /* seq 0 received */
    printf("test_reliable_ack: PASS\n");
}

static void test_mob_state_roundtrip(void) {
    uint8_t buf[512];
    PacketHeader hdr = { .type = PKT_MOB_STATE, .player_id = 0 };
    NetMobState in[2] = {
        { .id = 7, .type = 0, .x = 1.5f, .y = 64.0f, .z = -3.25f, .yaw = 1.0f, .health = 20 },
        { .id = 9, .type = 0, .x = 9.0f, .y = 65.0f, .z =  2.00f, .yaw = -2.0f, .health = 5 },
    };
    size_t len = net_write_mob_state(buf, &hdr, 12345u, in, 2);
    assert(len == 8 + 4 + 2 + 2 * 20);  /* header + bcast_seq + count + entries */

    PacketHeader h; size_t off = 0; net_read_header(buf, &off, &h);
    assert(h.type == PKT_MOB_STATE);
    uint32_t bseq = net_read_u32(buf, &off);
    assert(bseq == 12345u);
    uint16_t count; net_read_mob_state_header(buf, &off, &count);
    assert(count == 2);
    for (int i = 0; i < 2; i++) {
        NetMobState m; net_read_mob_state_entry(buf, &off, &m);
        assert(m.id == in[i].id && m.health == in[i].health);
        assert(m.x == in[i].x && m.y == in[i].y && m.z == in[i].z && m.yaw == in[i].yaw);
    }
    printf("PASS: mob_state_roundtrip\n");
}

static void test_world_state_roundtrip(void) {
    uint8_t buf[512];
    PacketHeader hdr = { .type = PKT_WORLD_STATE, .player_id = 0 };
    NetPlayerState in[2] = {
        { .player_id = 1, .x = 1.0f, .y = 64.0f, .z = -2.0f, .yaw = 0.5f, .pitch = -0.1f },
        { .player_id = 2, .x = 8.0f, .y = 70.0f, .z =  3.0f, .yaw = 1.5f, .pitch =  0.2f },
    };
    size_t len = net_write_world_state(buf, &hdr, 777u, in, 2, 4242u);
    assert(len == 8 + 4 + 1 + 2 * WORLD_STATE_PLAYER_SIZE + 4);

    PacketHeader h; size_t off = 0; net_read_header(buf, &off, &h);
    assert(h.type == PKT_WORLD_STATE);
    uint32_t bseq = net_read_u32(buf, &off);
    assert(bseq == 777u);
    uint8_t count = net_read_u8(buf, &off);
    assert(count == 2);
    for (int i = 0; i < 2; i++) {
        uint8_t pid = net_read_u8(buf, &off);
        float x = net_read_float(buf, &off);
        float y = net_read_float(buf, &off);
        float z = net_read_float(buf, &off);
        float yaw = net_read_float(buf, &off);
        float pitch = net_read_float(buf, &off);
        assert(pid == in[i].player_id);
        assert(x == in[i].x && y == in[i].y && z == in[i].z);
        assert(yaw == in[i].yaw && pitch == in[i].pitch);
    }
    uint32_t world_ticks = net_read_u32(buf, &off);
    assert(world_ticks == 4242u);
    printf("PASS: world_state_roundtrip\n");
}

static void test_seq_is_newer(void) {
    /* Strict ordering: equal is never newer. */
    assert(!seq_is_newer(5, 5));
    assert( seq_is_newer(6, 5));
    assert(!seq_is_newer(5, 6));

    /* Far apart in the small range. */
    assert( seq_is_newer(1000, 0));
    assert(!seq_is_newer(0, 1000));

    /* Wrap-around: small values are "newer" than values just below 2^32. */
    assert( seq_is_newer(0, 0xFFFFFFFFu));
    assert(!seq_is_newer(0xFFFFFFFFu, 0));
    assert( seq_is_newer(5, 0xFFFFFFF0u));
    assert(!seq_is_newer(0xFFFFFFF0u, 5));

    /* Exactly half the range: not newer (ambiguous boundary, treated as old). */
    assert(!seq_is_newer(0x80000000u, 0));
    /* Just under half the range forward: newer. */
    assert( seq_is_newer(0x7FFFFFFFu, 0));

    printf("PASS: seq_is_newer\n");
}

static void test_mob_attack_roundtrip(void) {
    uint8_t buf[64];
    PacketHeader hdr = { .type = PKT_MOB_ATTACK, .player_id = 3 };
    size_t len = net_write_mob_attack(buf, &hdr, 42);
    assert(len == 10);
    PacketHeader h; uint16_t id;
    net_read_mob_attack(buf, &h, &id);
    assert(h.type == PKT_MOB_ATTACK && h.player_id == 3 && id == 42);
    printf("PASS: mob_attack_roundtrip\n");
}

static void test_player_health_roundtrip(void) {
    uint8_t buf[64];
    PacketHeader hdr = { .type = PKT_PLAYER_HEALTH, .player_id = 0 };
    size_t len = net_write_player_health(buf, &hdr, 12, MOB_HEALTH_FLAG_DIED, 17, 8);
    assert(len == 12);
    PacketHeader h; uint8_t hp, fl, food, air;
    net_read_player_health(buf, len, &h, &hp, &fl, &food, &air);
    assert(h.type == PKT_PLAYER_HEALTH && hp == 12 && fl == MOB_HEALTH_FLAG_DIED);
    assert(food == 17 && air == 8);

    /* Legacy 10-byte packet: food/air default to full. */
    PacketHeader h2; uint8_t hp2, fl2, food2, air2;
    net_read_player_health(buf, 10, &h2, &hp2, &fl2, &food2, &air2);
    assert(hp2 == 12 && food2 == NET_MAX_FOOD && air2 == NET_MAX_AIR);
    printf("PASS: player_health_roundtrip\n");
}

static void test_connect_request_carries_version(void) {
    uint8_t buf[64];
    size_t off = 0;
    PacketHeader hdr = { .type = PKT_CONNECT_REQUEST, .player_id = 0 };
    net_write_connect_request(buf, &off, &hdr);
    /* protocol v8: header + version(u16) + shared_world(u8) + render_dist(u8). */
    assert(off == HEADER_WIRE_SIZE + 4);

    PacketHeader h; size_t roff = 0; net_read_header(buf, &roff, &h);
    assert(h.type == PKT_CONNECT_REQUEST);
    assert(net_read_connect_version(buf, off) == NET_PROTOCOL_VERSION);
    /* Default convenience writer marks a remote client with unspecified rd. */
    assert(net_read_connect_shared(buf, off) == 0);
    assert(net_read_connect_render_dist(buf, off) == 0);

    /* Explicit shared-world host flag + render distance round-trip. */
    size_t off2 = 0;
    net_write_connect_request_ex(buf, &off2, &hdr, 1, 12);
    assert(off2 == HEADER_WIRE_SIZE + 4);
    assert(net_read_connect_version(buf, off2) == NET_PROTOCOL_VERSION);
    assert(net_read_connect_shared(buf, off2) == 1);
    assert(net_read_connect_render_dist(buf, off2) == 12);

    /* A request missing the trailing fields reads them as 0 (remote, server-rd). */
    assert(net_read_connect_shared(buf, HEADER_WIRE_SIZE + 2) == 0);
    assert(net_read_connect_render_dist(buf, HEADER_WIRE_SIZE + 3) == 0);
    printf("PASS: connect_request_carries_version\n");
}

static void test_legacy_connect_request_reads_version_zero(void) {
    /* An old header-only connect request (no version field) must read as
     * version 0 so the server can reject it rather than misparse. */
    uint8_t buf[HEADER_WIRE_SIZE];
    size_t off = 0;
    PacketHeader hdr = { .type = PKT_CONNECT_REQUEST };
    net_write_header(buf, &off, &hdr);
    assert(net_read_connect_version(buf, off) == 0);
    printf("PASS: legacy_connect_request_reads_version_zero\n");
}

static void test_craft_roundtrip(void) {
    uint8_t buf[64];
    PacketHeader hdr = { .type = PKT_CRAFT, .player_id = 7 };
    size_t off = net_write_craft(buf, &hdr, 5);
    assert(off == HEADER_WIRE_SIZE + 2);

    PacketHeader h; uint16_t idx;
    net_read_craft(buf, &h, &idx);
    assert(h.type == PKT_CRAFT && h.player_id == 7);
    assert(idx == 5);
    printf("PASS: craft_roundtrip\n");
}

static void test_disconnect_reason_roundtrip(void) {
    uint8_t buf[64];
    size_t off = 0;
    PacketHeader hdr = { .type = PKT_DISCONNECT, .player_id = 0 };
    net_write_disconnect(buf, &off, &hdr, NET_DISCONNECT_VERSION_MISMATCH);
    assert(off == HEADER_WIRE_SIZE + 1);
    assert(net_read_disconnect_reason(buf, off) == NET_DISCONNECT_VERSION_MISMATCH);
    /* Legacy header-only disconnect reads as NORMAL. */
    size_t loff = 0; net_write_header(buf, &loff, &hdr);
    assert(net_read_disconnect_reason(buf, loff) == NET_DISCONNECT_NORMAL);
    printf("PASS: disconnect_reason_roundtrip\n");
}

/* ------------------------------------------------------------------ */
/*  Reliable message fragmentation                                     */
/* ------------------------------------------------------------------ */

/* A payload larger than RELIABLE_MAX_PAYLOAD splits into multiple fragment
 * packets, each of which fits within RELIABLE_MAX_PAYLOAD, and reassembles
 * back to the exact original bytes. */
static void test_fragment_roundtrip(void) {
    /* Larger than RELIABLE_FRAG_CHUNK so it actually splits into >1 fragment. */
    uint8_t payload[3000];
    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 31 + 7);

    uint16_t total = reliable_fragment_count(sizeof(payload));
    assert(total > 1); /* must actually fragment */

    ReliableReassembler re;
    reliable_reassemble_init(&re);

    uint8_t out[4096];
    size_t out_len = 0;
    bool done = false;
    for (uint16_t i = 0; i < total; i++) {
        uint8_t frag[RELIABLE_MAX_PAYLOAD];
        size_t flen = reliable_fragment_build(frag, /*msg_id*/ 5, i, total,
                                              payload, sizeof(payload));
        assert(flen <= RELIABLE_MAX_PAYLOAD);
        assert(reliable_packet_is_fragment(frag, flen));
        done = reliable_reassemble_feed(&re, frag, flen,
                                        out, sizeof(out), &out_len);
    }
    assert(done);
    assert(out_len == sizeof(payload));
    assert(memcmp(out, payload, sizeof(payload)) == 0);
    printf("PASS: fragment_roundtrip\n");
}

/* Fragments arriving out of order must still reassemble correctly. */
static void test_fragment_out_of_order(void) {
    /* >= 3 fragments at the current RELIABLE_FRAG_CHUNK size. */
    uint8_t payload[4000];
    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(255 - (i % 251));

    uint16_t total = reliable_fragment_count(sizeof(payload));
    assert(total >= 3);

    /* Build all fragments up front. */
    uint8_t frags[16][RELIABLE_MAX_PAYLOAD];
    size_t  flens[16];
    assert(total <= 16);
    for (uint16_t i = 0; i < total; i++)
        flens[i] = reliable_fragment_build(frags[i], 9, i, total,
                                           payload, sizeof(payload));

    /* Feed in reverse order. */
    ReliableReassembler re;
    reliable_reassemble_init(&re);
    uint8_t out[8192];
    size_t out_len = 0;
    bool done = false;
    for (int i = (int)total - 1; i >= 0; i--)
        done = reliable_reassemble_feed(&re, frags[i], flens[i],
                                        out, sizeof(out), &out_len);
    assert(done);
    assert(out_len == sizeof(payload));
    assert(memcmp(out, payload, sizeof(payload)) == 0);

    /* A duplicate fragment after completion must not corrupt anything. */
    done = reliable_reassemble_feed(&re, frags[0], flens[0],
                                    out, sizeof(out), &out_len);
    assert(out_len == sizeof(payload));
    assert(memcmp(out, payload, sizeof(payload)) == 0);
    printf("PASS: fragment_out_of_order\n");
}

/* A small message (<= RELIABLE_MAX_PAYLOAD) needs exactly one fragment and is
 * NOT recognized as a fragment packet on the wire — it goes through the plain
 * unfragmented reliable path untouched. */
static void test_small_message_not_fragmented(void) {
    /* not "small": it is a legacy typedef macro (char) in <windows.h>/rpcndr.h */
    uint8_t buf[32];
    for (size_t i = 0; i < sizeof(buf); i++) buf[i] = (uint8_t)i;

    assert(reliable_fragment_count(sizeof(buf)) == 1);
    /* The raw small payload must not be mistaken for a fragment packet. */
    assert(!reliable_packet_is_fragment(buf, sizeof(buf)));

    /* Even a zero-length payload reports one fragment. */
    assert(reliable_fragment_count(0) == 1);
    printf("PASS: small_message_not_fragmented\n");
}

/* PKT_CHUNK_DATA fragment subheader + PKT_CHUNK_UNLOAD round-trip. The
 * fragment data capacity must leave room for the 14-byte prefix in one reliable
 * packet, and the multi-fragment reassembly stride must be CHUNK_DATA_FRAG_BYTES
 * (a value < the whole reliable payload). */
static void test_chunk_packets_roundtrip(void) {
    uint8_t buf[CHUNK_DATA_RELIABLE_MAX];
    PacketHeader h = { .type = PKT_CHUNK_DATA, .player_id = 0 };
    uint8_t frag[CHUNK_DATA_FRAG_BYTES];
    for (size_t i = 0; i < sizeof(frag); i++) frag[i] = (uint8_t)(i * 7 + 1);

    size_t len = net_write_chunk_data_frag(buf, &h, 0xABCD, 3, 9,
                                           frag, sizeof frag);
    assert(len == CHUNK_DATA_FRAG_PREFIX + sizeof(frag));
    assert(len <= CHUNK_DATA_RELIABLE_MAX);   /* fits one reliable packet */

    uint16_t msg_id, index, total;
    net_read_chunk_data_frag_hdr(buf, &msg_id, &index, &total);
    assert(msg_id == 0xABCD && index == 3 && total == 9);
    assert(memcmp(buf + CHUNK_DATA_FRAG_PREFIX, frag, sizeof frag) == 0);

    /* The prefix is header(8) + 3×u16(6) = 14; data capacity is the remainder. */
    assert(CHUNK_DATA_FRAG_PREFIX == HEADER_WIRE_SIZE + 6);
    assert(CHUNK_DATA_FRAG_BYTES == CHUNK_DATA_RELIABLE_MAX - CHUNK_DATA_FRAG_PREFIX);

    /* Unload packet. */
    uint8_t ubuf[HEADER_WIRE_SIZE + 8];
    PacketHeader uh = { .type = PKT_CHUNK_UNLOAD, .player_id = 0 };
    size_t ulen = net_write_chunk_unload(ubuf, &uh, -12345, 6789);
    assert(ulen == HEADER_WIRE_SIZE + 8);
    PacketHeader rh; int32_t cx, cz;
    net_read_chunk_unload(ubuf, &rh, &cx, &cz);
    assert(rh.type == PKT_CHUNK_UNLOAD && cx == -12345 && cz == 6789);
    printf("PASS: chunk_packets_roundtrip\n");
}

int main(void)
{
    test_serialize_position();
    test_reliable_ack();
    test_mob_state_roundtrip();
    test_world_state_roundtrip();
    test_seq_is_newer();
    test_mob_attack_roundtrip();
    test_player_health_roundtrip();
    test_connect_request_carries_version();
    test_legacy_connect_request_reads_version_zero();
    test_craft_roundtrip();
    test_disconnect_reason_roundtrip();
    test_fragment_roundtrip();
    test_fragment_out_of_order();
    test_small_message_not_fragmented();
    test_chunk_packets_roundtrip();
    printf("All net tests passed.\n");
    return 0;
}
