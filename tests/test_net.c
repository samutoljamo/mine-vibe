#undef NDEBUG
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/net.h"
#include "../src/reliable.h"
#include "../src/chunkwire.h"
#include "../src/chunk_reasm.h"

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

/* ------------------------------------------------------------------ */
/*  Bounds-checked cursor codec                                         */
/* ------------------------------------------------------------------ */

/* The raw cursor never reads past the buffer and latches ok=false on the first
 * underflow. Use a heap buffer sized exactly to len so ASan catches an
 * over-read if one ever slips through. */
static void test_reader_underflow(void) {
    uint8_t* b = malloc(3);
    b[0] = 0x11; b[1] = 0x22; b[2] = 0x33;
    NetReader r = net_reader_init(b, 3);
    assert(net_reader_u8(&r) == 0x11);
    assert(net_reader_remaining(&r) == 2);
    /* A u32 needs 4 bytes but only 2 remain: must fail, return 0, not over-read. */
    assert(net_reader_u32(&r) == 0);
    assert(!net_reader_ok(&r));
    /* Once not-ok, all further reads stay 0 and remaining is 0. */
    assert(net_reader_u8(&r) == 0);
    assert(net_reader_remaining(&r) == 0);
    free(b);

    /* Exact-fit reads succeed and leave ok=true. */
    uint8_t* e = malloc(4);
    e[0]=1; e[1]=0; e[2]=0; e[3]=0;
    NetReader r2 = net_reader_init(e, 4);
    assert(net_reader_u32(&r2) == 1);
    assert(net_reader_ok(&r2));
    assert(net_reader_remaining(&r2) == 0);
    free(e);
    printf("PASS: reader_underflow\n");
}

/* The writer caps every write at capacity; an over-capacity write latches
 * ok=false and never writes past the end of a heap buffer. */
static void test_writer_overflow(void) {
    uint8_t* b = malloc(3);
    NetWriter w = net_writer_init(b, 3);
    net_writer_u8(&w, 0xAA);            /* 1 used */
    net_writer_u16(&w, 0xBBCC);         /* 3 used, exact fit */
    assert(net_writer_ok(&w));
    assert(net_writer_len(&w) == 3);
    net_writer_u8(&w, 0xFF);            /* would be byte 4: overflow */
    assert(!net_writer_ok(&w));
    assert(net_writer_len(&w) == 0);    /* len() reports 0 once overflowed */
    free(b);
    printf("PASS: writer_overflow\n");
}

/* Feed every fixed-size packet parser a buffer truncated to each length from 0
 * up to (size-1): all must report failure and never read out of bounds. The
 * exact full size must parse OK. Heap buffers sized to the truncation length
 * make ASan catch any over-read. */
static void test_parsers_reject_truncation(void) {
    /* Build one valid instance of each fixed packet into a scratch buffer, then
     * replay truncated copies of it through the safe parser. */
    uint8_t scratch[64];

    /* PKT_POSITION (32) */
    {
        PositionPacket p = {0};
        p.header.type = PKT_POSITION; p.tick = 9; p.x = 1; p.y = 2; p.z = 3;
        size_t full = net_write_position(scratch, &p);
        assert(full == 32);
        for (size_t L = 0; L < full; L++) {
            uint8_t* t = malloc(L ? L : 1);
            memcpy(t, scratch, L);
            PositionPacket q;
            assert(!net_parse_position(t, L, &q));
            free(t);
        }
        uint8_t* t = malloc(full); memcpy(t, scratch, full);
        PositionPacket q; assert(net_parse_position(t, full, &q));
        assert(q.tick == 9 && q.x == 1.0f); free(t);
    }

    /* PKT_BLOCK_BREAK (22) */
    {
        BlockBreakPacket p = {0};
        p.header.type = PKT_BLOCK_BREAK; p.x=-5; p.y=70; p.z=12; p.block=3; p.slot=2;
        size_t full = net_write_block_break(scratch, &p);
        for (size_t L = 0; L < full; L++) {
            uint8_t* t = malloc(L?L:1); memcpy(t, scratch, L);
            BlockBreakPacket q; assert(!net_parse_block_break(t, L, &q)); free(t);
        }
        uint8_t* t = malloc(full); memcpy(t, scratch, full);
        BlockBreakPacket q; assert(net_parse_block_break(t, full, &q));
        assert(q.x == -5 && q.block == 3 && q.slot == 2); free(t);
    }

    /* PKT_BLOCK_PLACE (22) */
    {
        BlockPlacePacket p = {0};
        p.header.type = PKT_BLOCK_PLACE; p.x=1; p.y=2; p.z=3; p.face=4; p.slot=1;
        size_t full = net_write_block_place(scratch, &p);
        for (size_t L = 0; L < full; L++) {
            uint8_t* t = malloc(L?L:1); memcpy(t, scratch, L);
            BlockPlacePacket q; assert(!net_parse_block_place(t, L, &q)); free(t);
        }
        uint8_t* t = malloc(full); memcpy(t, scratch, full);
        BlockPlacePacket q; assert(net_parse_block_place(t, full, &q));
        assert(q.face == 4 && q.slot == 1); free(t);
    }

    /* PKT_BLOCK_CHANGE (21) */
    {
        BlockChangePacket p = {0};
        p.header.type = PKT_BLOCK_CHANGE; p.x=9; p.y=8; p.z=7; p.block=5;
        size_t full = net_write_block_change(scratch, &p);
        for (size_t L = 0; L < full; L++) {
            uint8_t* t = malloc(L?L:1); memcpy(t, scratch, L);
            BlockChangePacket q; assert(!net_parse_block_change(t, L, &q)); free(t);
        }
        uint8_t* t = malloc(full); memcpy(t, scratch, full);
        BlockChangePacket q; assert(net_parse_block_change(t, full, &q));
        assert(q.block == 5); free(t);
    }

    /* PKT_MOB_ATTACK / PKT_CRAFT / PKT_EQUIP / PKT_CHUNK_UNLOAD */
    {
        PacketHeader h = { .type = PKT_MOB_ATTACK };
        size_t full = net_write_mob_attack(scratch, &h, 42);
        for (size_t L = 0; L < full; L++) {
            uint8_t* t = malloc(L?L:1); memcpy(t, scratch, L);
            PacketHeader rh; uint16_t id;
            assert(!net_parse_mob_attack(t, L, &rh, &id)); free(t);
        }
        uint8_t* t = malloc(full); memcpy(t, scratch, full);
        PacketHeader rh; uint16_t id; assert(net_parse_mob_attack(t, full, &rh, &id));
        assert(id == 42); free(t);
    }
    {
        PacketHeader h = { .type = PKT_CRAFT };
        size_t full = net_write_craft(scratch, &h, 7);
        for (size_t L = 0; L < full; L++) {
            uint8_t* t = malloc(L?L:1); memcpy(t, scratch, L);
            PacketHeader rh; uint16_t idx;
            assert(!net_parse_craft(t, L, &rh, &idx)); free(t);
        }
        uint8_t* t = malloc(full); memcpy(t, scratch, full);
        PacketHeader rh; uint16_t idx; assert(net_parse_craft(t, full, &rh, &idx));
        assert(idx == 7); free(t);
    }
    {
        PacketHeader h = { .type = PKT_EQUIP };
        size_t full = net_write_equip(scratch, &h, 3);
        for (size_t L = 0; L < full; L++) {
            uint8_t* t = malloc(L?L:1); memcpy(t, scratch, L);
            PacketHeader rh; uint8_t slot;
            assert(!net_parse_equip(t, L, &rh, &slot)); free(t);
        }
        uint8_t* t = malloc(full); memcpy(t, scratch, full);
        PacketHeader rh; uint8_t slot; assert(net_parse_equip(t, full, &rh, &slot));
        assert(slot == 3); free(t);
    }
    {
        PacketHeader h = { .type = PKT_CHUNK_UNLOAD };
        size_t full = net_write_chunk_unload(scratch, &h, -3, 4);
        for (size_t L = 0; L < full; L++) {
            uint8_t* t = malloc(L?L:1); memcpy(t, scratch, L);
            PacketHeader rh; int32_t cx, cz;
            assert(!net_parse_chunk_unload(t, L, &rh, &cx, &cz)); free(t);
        }
        uint8_t* t = malloc(full); memcpy(t, scratch, full);
        PacketHeader rh; int32_t cx, cz;
        assert(net_parse_chunk_unload(t, full, &rh, &cx, &cz));
        assert(cx == -3 && cz == 4); free(t);
    }

    /* PKT_PLAYER_HEALTH: < 10 bytes must fail; >= 10 must succeed. */
    {
        PacketHeader h = { .type = PKT_PLAYER_HEALTH };
        size_t full = net_write_player_health(scratch, &h, 11, 0, 5, 6);
        assert(full == 12);
        for (size_t L = 0; L < 10; L++) {
            uint8_t* t = malloc(L?L:1); memcpy(t, scratch, L);
            PacketHeader rh; uint8_t hp, fl, fd, ai;
            assert(!net_parse_player_health(t, L, &rh, &hp, &fl, &fd, &ai)); free(t);
        }
        /* Exactly 10 (legacy) parses, food/air default to max. */
        uint8_t* t10 = malloc(10); memcpy(t10, scratch, 10);
        PacketHeader rh; uint8_t hp, fl, fd, ai;
        assert(net_parse_player_health(t10, 10, &rh, &hp, &fl, &fd, &ai));
        assert(hp == 11 && fd == NET_MAX_FOOD && ai == NET_MAX_AIR); free(t10);
    }

    /* PKT_CHUNK_DATA fragment subheader (14 bytes). */
    {
        PacketHeader h = { .type = PKT_CHUNK_DATA };
        uint8_t frag[4] = {1,2,3,4};
        size_t full = net_write_chunk_data_frag(scratch, &h, 0xABCD, 2, 9, frag, 4);
        for (size_t L = 0; L < CHUNK_DATA_FRAG_PREFIX; L++) {
            uint8_t* t = malloc(L?L:1); memcpy(t, scratch, L);
            PacketHeader rh; uint16_t mid, idx, tot;
            assert(!net_parse_chunk_data_frag_hdr(t, L, &rh, &mid, &idx, &tot));
            free(t);
        }
        uint8_t* t = malloc(full); memcpy(t, scratch, full);
        PacketHeader rh; uint16_t mid, idx, tot;
        assert(net_parse_chunk_data_frag_hdr(t, full, &rh, &mid, &idx, &tot));
        assert(mid == 0xABCD && idx == 2 && tot == 9); free(t);
    }
    printf("PASS: parsers_reject_truncation\n");
}

/* PKT_INVENTORY: a declared slot_count whose implied body is not present must
 * be rejected; a well-formed snapshot round-trips. A hostile slot_count is
 * clamped, never used to over-read. */
static void test_inventory_malformed(void) {
    uint8_t buf[64];
    InventoryPacket p = {0};
    p.header.type = PKT_INVENTORY;
    p.slot_count  = INVENTORY_NET_SLOTS;
    for (int i = 0; i < INVENTORY_NET_SLOTS; i++) {
        p.slots[i].item = (uint16_t)(i + 1);
        p.slots[i].count = (uint8_t)(i + 2);
        p.slots[i].durability = (uint16_t)(i * 10);
    }
    size_t full = net_write_inventory(buf, &p);

    /* Truncations below the declared body must fail. */
    for (size_t L = 0; L < full; L++) {
        uint8_t* t = malloc(L?L:1); memcpy(t, buf, L);
        InventoryPacket q; assert(!net_parse_inventory(t, L, &q)); free(t);
    }
    /* Full parses and round-trips. */
    uint8_t* t = malloc(full); memcpy(t, buf, full);
    InventoryPacket q; assert(net_parse_inventory(t, full, &q));
    assert(q.slot_count == INVENTORY_NET_SLOTS);
    for (int i = 0; i < INVENTORY_NET_SLOTS; i++)
        assert(q.slots[i].item == (uint16_t)(i + 1));
    free(t);

    /* Hostile: header + slot_count=255 but no body. Clamp + reject (no over-read). */
    uint8_t* hostile = malloc(9);
    memset(hostile, 0, 9);
    hostile[0] = PKT_INVENTORY;
    hostile[8] = 255;
    InventoryPacket hq; assert(!net_parse_inventory(hostile, 9, &hq));
    free(hostile);
    printf("PASS: inventory_malformed\n");
}

/* chunkwire RLE decode + chunk decode reject malformed/oversized input without
 * over-reading or over-writing. */
static void test_chunkwire_malformed(void) {
    uint8_t out[64];
    size_t  n;

    /* Truncated varint (high bit set, no continuation). */
    uint8_t bad1[] = { 0x80 };
    assert(!chunkwire_rle_decode(bad1, sizeof bad1, out, sizeof out, &n));

    /* Run with no following block byte. */
    uint8_t bad2[] = { 0x03 };
    assert(!chunkwire_rle_decode(bad2, sizeof bad2, out, sizeof out, &n));

    /* Zero-length run is invalid. */
    uint8_t bad3[] = { 0x00, 0x05 };
    assert(!chunkwire_rle_decode(bad3, sizeof bad3, out, sizeof out, &n));

    /* Run that would overrun the output buffer. */
    uint8_t bad4[] = { 0x40, 0x07 };  /* run of 64 into a 10-byte out */
    assert(!chunkwire_rle_decode(bad4, sizeof bad4, out, 10, &n));

    /* chunk decode: a body shorter than the 12-byte header is rejected. */
    {
        uint8_t* hdr = malloc(11);
        memset(hdr, 0, 11);
        uint8_t blocks[CHUNKWIRE_COLUMN_BLOCKS];
        int32_t cx, cz;
        assert(!chunkwire_decode_chunk(hdr, 11, &cx, &cz, blocks, sizeof blocks));
        free(hdr);
    }
    /* chunk decode: declared rle_len longer than the actual body is rejected. */
    {
        uint8_t body[16] = {0};
        body[8] = 0xFF;  /* rle_len = 255, but only 4 body bytes follow */
        uint8_t blocks[CHUNKWIRE_COLUMN_BLOCKS];
        int32_t cx, cz;
        assert(!chunkwire_decode_chunk(body, 16, &cx, &cz, blocks, sizeof blocks));
    }
    printf("PASS: chunkwire_malformed\n");
}

/* Fuzz: random and truncated byte buffers through every parser must never
 * crash and must report failure when malformed. With ASan an over-read here
 * would abort the test. */
static void test_fuzz_parsers(void) {
    srand(1234567u);
    for (int iter = 0; iter < 200000; iter++) {
        size_t len = (size_t)(rand() % 40);   /* 0..39 bytes */
        uint8_t* b = malloc(len ? len : 1);
        for (size_t i = 0; i < len; i++) b[i] = (uint8_t)(rand() & 0xFF);

        PositionPacket pp; (void)net_parse_position(b, len, &pp);
        BlockBreakPacket bb; (void)net_parse_block_break(b, len, &bb);
        BlockPlacePacket bpl; (void)net_parse_block_place(b, len, &bpl);
        BlockChangePacket bc; (void)net_parse_block_change(b, len, &bc);
        InventoryPacket iv; (void)net_parse_inventory(b, len, &iv);
        PacketHeader h; uint16_t u16; uint8_t u8; int32_t cx, cz;
        (void)net_parse_mob_attack(b, len, &h, &u16);
        (void)net_parse_craft(b, len, &h, &u16);
        (void)net_parse_equip(b, len, &h, &u8);
        (void)net_parse_chunk_unload(b, len, &h, &cx, &cz);
        uint8_t hp, fl, fd, ai;
        (void)net_parse_player_health(b, len, &h, &hp, &fl, &fd, &ai);
        uint16_t worn[ARMOR_NET_SLOTS]; uint8_t pts;
        net_read_armor(b, len, &h, worn, &pts);   /* tolerant: must not crash */
        uint16_t mid, idx, tot;
        (void)net_parse_chunk_data_frag_hdr(b, len, &h, &mid, &idx, &tot);

        /* Counted-array readers driven by a cursor: parse a clamped count of
         * entries; underflow must be caught, never over-read. */
        {
            NetReader r = net_reader_init(b, len);
            PacketHeader rh; net_reader_header(&r, &rh);
            (void)net_reader_u32(&r);          /* bcast_seq */
            uint16_t count = net_reader_u16(&r);
            if (count > 64) count = 64;   /* MOB_MAX clamp (avoid mob.h dep) */
            for (uint16_t i = 0; i < count && net_reader_ok(&r); i++) {
                NetMobState m; net_reader_mob_state_entry(&r, &m);
            }
        }
        {
            NetReader r = net_reader_init(b, len);
            PacketHeader rh; net_reader_header(&r, &rh);
            (void)net_reader_u32(&r);
            uint8_t pc = net_reader_u8(&r);
            for (uint8_t i = 0; i < pc && net_reader_ok(&r); i++) {
                NetPlayerState ps; net_reader_player_state(&r, &ps);
            }
        }

        /* chunkwire decoders on raw fuzz. */
        uint8_t dec[64]; size_t dn;
        (void)chunkwire_rle_decode(b, len, dec, sizeof dec, &dn);
        int32_t fcx, fcz; uint8_t fblocks[CHUNKWIRE_COLUMN_BLOCKS];
        (void)chunkwire_decode_chunk(b, len, &fcx, &fcz, fblocks, sizeof fblocks);

        /* reliable reassembly on raw fuzz. */
        ReliableReassembler re; reliable_reassemble_init(&re);
        uint8_t ro[256]; size_t rl;
        (void)reliable_reassemble_feed(&re, b, len, ro, sizeof ro, &rl);

        free(b);
    }
    printf("PASS: fuzz_parsers (200000 iterations, no crash)\n");
}

/* The cursor-based entry readers must produce identical results to the legacy
 * blind readers on well-formed buffers (round-trip parity). */
static void test_cursor_roundtrip_parity(void) {
    uint8_t buf[512];
    PacketHeader hdr = { .type = PKT_MOB_STATE, .player_id = 0 };
    NetMobState in[3] = {
        { .id=1, .type=0, .x=1.5f, .y=2.5f, .z=3.5f, .yaw=0.1f, .health=20 },
        { .id=2, .type=1, .x=-4.f, .y=5.f,  .z=6.f,  .yaw=-1.f, .health=7  },
        { .id=3, .type=2, .x=9.f,  .y=8.f,  .z=7.f,  .yaw=2.f,  .health=1  },
    };
    size_t len = net_write_mob_state(buf, &hdr, 99u, in, 3);

    NetReader r = net_reader_init(buf, len);
    PacketHeader rh; net_reader_header(&r, &rh);
    assert(rh.type == PKT_MOB_STATE);
    assert(net_reader_u32(&r) == 99u);
    uint16_t count = net_reader_u16(&r);
    assert(count == 3);
    for (uint16_t i = 0; i < count; i++) {
        NetMobState m; net_reader_mob_state_entry(&r, &m);
        assert(m.id == in[i].id && m.type == in[i].type && m.health == in[i].health);
        assert(m.x == in[i].x && m.y == in[i].y && m.z == in[i].z && m.yaw == in[i].yaw);
    }
    assert(net_reader_ok(&r));
    assert(net_reader_remaining(&r) == 0);
    printf("PASS: cursor_roundtrip_parity\n");
}

/* ------------------------------------------------------------------ */
/*  Keepalive packet (protocol v9)                                      */
/* ------------------------------------------------------------------ */

/* PKT_KEEPALIVE is header-only; it round-trips through the bounds-checked codec
 * and a buffer shorter than the header is rejected without over-reading. */
static void test_keepalive_roundtrip(void) {
    uint8_t buf[HEADER_WIRE_SIZE];
    PacketHeader hdr = { .type = PKT_KEEPALIVE, .player_id = 7,
                         .seq = 0x1234, .ack = 0x5678, .ack_bits = 0x9ABC };
    size_t len = net_write_keepalive(buf, &hdr);
    assert(len == HEADER_WIRE_SIZE);

    PacketHeader h;
    assert(net_parse_keepalive(buf, len, &h));
    assert(h.type == PKT_KEEPALIVE && h.player_id == 7);
    assert(h.seq == 0x1234 && h.ack == 0x5678 && h.ack_bits == 0x9ABC);

    /* Any truncation below the header is rejected, never over-read. */
    for (size_t L = 0; L < HEADER_WIRE_SIZE; L++) {
        uint8_t* t = malloc(L ? L : 1);
        memcpy(t, buf, L);
        PacketHeader rh;
        assert(!net_parse_keepalive(t, L, &rh));
        free(t);
    }
    printf("PASS: keepalive_roundtrip\n");
}

/* ------------------------------------------------------------------ */
/*  Chunk reassembly ring (loss hardening, mine-vibe-003)               */
/* ------------------------------------------------------------------ */

/* Build a synthetic column of `total` fragments whose decoded body is a
 * deterministic function of msg_id, so two interleaved columns are
 * distinguishable. Returns the body length. */
static size_t build_column(uint16_t msg_id, uint16_t total, uint8_t* body,
                           size_t body_cap) {
    size_t n = (size_t)(total - 1) * CHUNK_DATA_FRAG_BYTES + 17; /* partial last */
    assert(n <= body_cap);
    for (size_t i = 0; i < n; i++)
        body[i] = (uint8_t)((i * 13 + msg_id * 7 + 1) & 0xFF);
    return n;
}

/* Feed fragment `index` of a built column into the ring. Returns true (and fills
 * out/out_len) when the column completes. */
static bool feed_frag(ChunkReasmRing* ring, uint16_t msg_id, uint16_t index,
                      uint16_t total, const uint8_t* body, size_t body_len,
                      uint8_t* out, size_t out_cap, size_t* out_len) {
    size_t off = (size_t)index * CHUNK_DATA_FRAG_BYTES;
    size_t flen = body_len - off;
    if (flen > CHUNK_DATA_FRAG_BYTES) flen = CHUNK_DATA_FRAG_BYTES;
    return chunk_reasm_feed(ring, msg_id, index, total, body + off, flen,
                            out, out_cap, out_len);
}

/* Two columns' fragments interleaved out of order, plus a dropped+retransmitted
 * fragment, must BOTH reassemble correctly and independently. */
static void test_reasm_interleaved(void) {
    ChunkReasmRing ring;
    chunk_reasm_init(&ring);

    enum { TOTAL = 3 };
    static uint8_t bodyA[TOTAL * CHUNK_DATA_FRAG_BYTES];
    static uint8_t bodyB[TOTAL * CHUNK_DATA_FRAG_BYTES];
    size_t lenA = build_column(100, TOTAL, bodyA, sizeof bodyA);
    size_t lenB = build_column(200, TOTAL, bodyB, sizeof bodyB);

    static uint8_t out[TOTAL * CHUNK_DATA_FRAG_BYTES];
    size_t out_len = 0;
    bool doneA = false, doneB = false;

    /* Interleave fragments of A(100) and B(200) out of order. Simulate a
     * dropped A frag 1 (skipped here) then retransmitted later. */
    assert(!feed_frag(&ring, 200, 2, TOTAL, bodyB, lenB, out, sizeof out, &out_len)); /* B last */
    assert(!feed_frag(&ring, 100, 0, TOTAL, bodyA, lenA, out, sizeof out, &out_len)); /* A first */
    assert(!feed_frag(&ring, 200, 0, TOTAL, bodyB, lenB, out, sizeof out, &out_len)); /* B first */
    assert(!feed_frag(&ring, 100, 2, TOTAL, bodyA, lenA, out, sizeof out, &out_len)); /* A last (frag1 still missing) */
    /* Both columns are still in flight concurrently. */
    assert(chunk_reasm_active_count(&ring) == 2);

    /* B frag 1 arrives -> B completes. */
    doneB = feed_frag(&ring, 200, 1, TOTAL, bodyB, lenB, out, sizeof out, &out_len);
    assert(doneB);
    assert(out_len == lenB);
    assert(memcmp(out, bodyB, lenB) == 0);
    assert(chunk_reasm_active_count(&ring) == 1);   /* only A left */

    /* The dropped A frag 1 is retransmitted (possibly duplicated) -> A completes. */
    doneA = feed_frag(&ring, 100, 1, TOTAL, bodyA, lenA, out, sizeof out, &out_len);
    assert(doneA);
    assert(out_len == lenA);
    assert(memcmp(out, bodyA, lenA) == 0);
    assert(chunk_reasm_active_count(&ring) == 0);   /* both freed */

    printf("PASS: reasm_interleaved\n");
}

/* A duplicate fragment must not double-count or corrupt; the column still
 * completes correctly exactly once. */
static void test_reasm_duplicates(void) {
    ChunkReasmRing ring;
    chunk_reasm_init(&ring);
    enum { TOTAL = 2 };
    static uint8_t body[TOTAL * CHUNK_DATA_FRAG_BYTES];
    size_t len = build_column(42, TOTAL, body, sizeof body);
    static uint8_t out[TOTAL * CHUNK_DATA_FRAG_BYTES];
    size_t out_len = 0;

    assert(!feed_frag(&ring, 42, 0, TOTAL, body, len, out, sizeof out, &out_len));
    assert(!feed_frag(&ring, 42, 0, TOTAL, body, len, out, sizeof out, &out_len)); /* dup */
    assert(chunk_reasm_active_count(&ring) == 1);
    bool done = feed_frag(&ring, 42, 1, TOTAL, body, len, out, sizeof out, &out_len);
    assert(done);
    assert(out_len == len && memcmp(out, body, len) == 0);
    printf("PASS: reasm_duplicates\n");
}

/* When more concurrent columns than slots are in flight, the OLDEST partial is
 * evicted to bound memory — without corrupting the survivors. */
static void test_reasm_overflow_evicts_oldest(void) {
    ChunkReasmRing ring;
    chunk_reasm_init(&ring);
    enum { TOTAL = 2 };
    static uint8_t out[TOTAL * CHUNK_DATA_FRAG_BYTES];
    size_t out_len = 0;

    /* Start CHUNK_REASM_SLOTS+1 distinct columns, each with only frag 0 (all
     * partial). The first started (oldest touch) must be evicted. */
    static uint8_t bodies[CHUNK_REASM_SLOTS + 1][TOTAL * CHUNK_DATA_FRAG_BYTES];
    size_t lens[CHUNK_REASM_SLOTS + 1];
    for (int i = 0; i < CHUNK_REASM_SLOTS + 1; i++) {
        lens[i] = build_column((uint16_t)(1000 + i), TOTAL, bodies[i], sizeof bodies[i]);
        bool d = feed_frag(&ring, (uint16_t)(1000 + i), 0, TOTAL,
                           bodies[i], lens[i], out, sizeof out, &out_len);
        assert(!d);
    }
    /* The ring is capped at CHUNK_REASM_SLOTS active columns. */
    assert(chunk_reasm_active_count(&ring) == CHUNK_REASM_SLOTS);

    /* The oldest column (msg_id 1000) was evicted: its surviving frag 0 is gone,
     * so completing it now requires BOTH frags again (no stale corruption). The
     * newest columns (1001..) still hold frag 0, so one more frag completes them
     * with the correct bytes. */
    bool d = feed_frag(&ring, (uint16_t)(1000 + CHUNK_REASM_SLOTS), 1, TOTAL,
                       bodies[CHUNK_REASM_SLOTS], lens[CHUNK_REASM_SLOTS],
                       out, sizeof out, &out_len);
    assert(d);
    assert(out_len == lens[CHUNK_REASM_SLOTS]);
    assert(memcmp(out, bodies[CHUNK_REASM_SLOTS], lens[CHUNK_REASM_SLOTS]) == 0);

    /* Feeding only frag 1 of the EVICTED column does not complete it (frag 0 was
     * dropped on eviction); it just opens a fresh partial slot. */
    out_len = 0;
    bool d2 = feed_frag(&ring, 1000, 1, TOTAL, bodies[0], lens[0],
                        out, sizeof out, &out_len);
    assert(!d2);
    printf("PASS: reasm_overflow_evicts_oldest\n");
}

/* Malformed fragment geometry is ignored without opening a slot or crashing. */
static void test_reasm_malformed(void) {
    ChunkReasmRing ring;
    chunk_reasm_init(&ring);
    static uint8_t body[CHUNK_DATA_FRAG_BYTES];
    static uint8_t out[CHUNK_DATA_FRAG_BYTES];
    size_t out_len = 0;
    /* total = 0 */
    assert(!chunk_reasm_feed(&ring, 1, 0, 0, body, 1, out, sizeof out, &out_len));
    /* index >= total */
    assert(!chunk_reasm_feed(&ring, 1, 5, 3, body, 1, out, sizeof out, &out_len));
    /* total > CHUNK_DATA_FRAG_MAX */
    assert(!chunk_reasm_feed(&ring, 1, 0, CHUNK_DATA_FRAG_MAX + 1, body, 1,
                             out, sizeof out, &out_len));
    /* flen too big */
    assert(!chunk_reasm_feed(&ring, 1, 0, 1, body, CHUNK_DATA_FRAG_BYTES + 1,
                             out, sizeof out, &out_len));
    assert(chunk_reasm_active_count(&ring) == 0);
    printf("PASS: reasm_malformed\n");
}

/* ------------------------------------------------------------------ */
/*  Address resolution (mine-vibe-9xu)                                   */
/* ------------------------------------------------------------------ */

/* net_resolve handles dotted-quad IPv4 literals and DNS names. "127.0.0.1"
 * must resolve to exactly 127.0.0.1 on the requested port; an IPv6-only literal
 * is reported as NO_IPV4 (the transport is IPv4-only today); garbage fails. */
static void test_resolve_addr(void) {
    struct sockaddr_in a;

    /* IPv4 literal round-trips to the exact address + port. */
    memset(&a, 0, sizeof a);
    assert(net_resolve("127.0.0.1", 25565, &a) == NET_RESOLVE_OK);
    assert(a.sin_family == AF_INET);
    assert(a.sin_port == htons(25565));
    /* 127.0.0.1 in network order. */
    assert(a.sin_addr.s_addr == htonl(0x7F000001u));

    /* "localhost" must resolve to an IPv4 loopback (most stacks list 127.0.0.1
     * among its A records). */
    memset(&a, 0, sizeof a);
    NetResolveResult lr = net_resolve("localhost", 1234, &a);
    assert(lr == NET_RESOLVE_OK || lr == NET_RESOLVE_NO_IPV4);
    if (lr == NET_RESOLVE_OK) {
        assert(a.sin_port == htons(1234));
        assert(a.sin_addr.s_addr == htonl(0x7F000001u));   /* 127.0.0.1 */
    }

    /* An IPv6 literal cannot fill an IPv4 sockaddr_in: reported, not crashed. */
    memset(&a, 0, sizeof a);
    NetResolveResult r6 = net_resolve("::1", 80, &a);
    assert(r6 == NET_RESOLVE_NO_IPV4 || r6 == NET_RESOLVE_FAILED);

    /* A syntactically invalid host fails cleanly. */
    memset(&a, 0, sizeof a);
    assert(net_resolve("this is not a host", 80, &a) == NET_RESOLVE_FAILED);

    printf("PASS: resolve_addr\n");
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
    test_reader_underflow();
    test_writer_overflow();
    test_parsers_reject_truncation();
    test_inventory_malformed();
    test_chunkwire_malformed();
    test_cursor_roundtrip_parity();
    test_fuzz_parsers();
    test_keepalive_roundtrip();
    test_reasm_interleaved();
    test_reasm_duplicates();
    test_reasm_overflow_evicts_oldest();
    test_reasm_malformed();
    test_resolve_addr();
    printf("All net tests passed.\n");
    return 0;
}
