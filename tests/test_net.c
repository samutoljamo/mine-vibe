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
    size_t len = net_write_mob_state(buf, &hdr, in, 2);
    assert(len == 8 + 2 + 2 * 20);

    PacketHeader h; size_t off = 0; net_read_header(buf, &off, &h);
    assert(h.type == PKT_MOB_STATE);
    uint16_t count; net_read_mob_state_header(buf, &off, &count);
    assert(count == 2);
    for (int i = 0; i < 2; i++) {
        NetMobState m; net_read_mob_state_entry(buf, &off, &m);
        assert(m.id == in[i].id && m.health == in[i].health);
        assert(m.x == in[i].x && m.y == in[i].y && m.z == in[i].z && m.yaw == in[i].yaw);
    }
    printf("PASS: mob_state_roundtrip\n");
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
    size_t len = net_write_player_health(buf, &hdr, 12, MOB_HEALTH_FLAG_DIED);
    assert(len == 10);
    PacketHeader h; uint8_t hp, fl;
    net_read_player_health(buf, &h, &hp, &fl);
    assert(h.type == PKT_PLAYER_HEALTH && hp == 12 && fl == MOB_HEALTH_FLAG_DIED);
    printf("PASS: player_health_roundtrip\n");
}

static void test_connect_request_carries_version(void) {
    uint8_t buf[64];
    size_t off = 0;
    PacketHeader hdr = { .type = PKT_CONNECT_REQUEST, .player_id = 0 };
    net_write_connect_request(buf, &off, &hdr);
    assert(off == HEADER_WIRE_SIZE + 2);

    PacketHeader h; size_t roff = 0; net_read_header(buf, &roff, &h);
    assert(h.type == PKT_CONNECT_REQUEST);
    assert(net_read_connect_version(buf, off) == NET_PROTOCOL_VERSION);
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
    uint8_t payload[1000];
    for (size_t i = 0; i < sizeof(payload); i++)
        payload[i] = (uint8_t)(i * 31 + 7);

    uint16_t total = reliable_fragment_count(sizeof(payload));
    assert(total > 1); /* must actually fragment */

    ReliableReassembler re;
    reliable_reassemble_init(&re);

    uint8_t out[2048];
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
    uint8_t payload[600];
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
    uint8_t out[2048];
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
    uint8_t small[32];
    for (size_t i = 0; i < sizeof(small); i++) small[i] = (uint8_t)i;

    assert(reliable_fragment_count(sizeof(small)) == 1);
    /* The raw small payload must not be mistaken for a fragment packet. */
    assert(!reliable_packet_is_fragment(small, sizeof(small)));

    /* Even a zero-length payload reports one fragment. */
    assert(reliable_fragment_count(0) == 1);
    printf("PASS: small_message_not_fragmented\n");
}

int main(void)
{
    test_serialize_position();
    test_reliable_ack();
    test_mob_state_roundtrip();
    test_mob_attack_roundtrip();
    test_player_health_roundtrip();
    test_connect_request_carries_version();
    test_legacy_connect_request_reads_version_zero();
    test_disconnect_reason_roundtrip();
    test_fragment_roundtrip();
    test_fragment_out_of_order();
    test_small_message_not_fragmented();
    printf("All net tests passed.\n");
    return 0;
}
