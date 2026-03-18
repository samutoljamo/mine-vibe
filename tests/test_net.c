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

int main(void)
{
    test_serialize_position();
    test_reliable_ack();
    printf("All net tests passed.\n");
    return 0;
}
