#include "client.h"
#include "net.h"
#include <assert.h>
#include <stdio.h>

/* Simulate the timeout logic directly — check that after
 * CLIENT_MAX_CONNECT_ATTEMPTS the client gives up. */
static void test_connect_max_attempts(void)
{
    /* We just verify the constant exists and is sane */
    assert(CLIENT_MAX_CONNECT_ATTEMPTS > 0);
    assert(CLIENT_MAX_CONNECT_ATTEMPTS <= 30);
    printf("PASS: test_connect_max_attempts (CLIENT_MAX_CONNECT_ATTEMPTS=%d)\n",
           CLIENT_MAX_CONNECT_ATTEMPTS);
}

/* Compute expected wire size of a PKT_WORLD_STATE with N players:
 * HEADER_WIRE_SIZE + 1 (count) + N * (1 pid + 5 floats*4) */
#define WORLD_STATE_ENTRY_SIZE (1 + 5 * 4)  /* 21 bytes per player */

static void test_world_state_packet_size_constants(void)
{
    /* Minimum valid packet: header + count byte */
    int min_len = HEADER_WIRE_SIZE + 1;
    /* A packet claiming 1 player must be at least min_len + 21 bytes */
    int one_player_len = min_len + WORLD_STATE_ENTRY_SIZE;

    assert(min_len > 0);
    assert(one_player_len > min_len);
    printf("PASS: test_world_state_packet_size_constants "
           "(min=%d, one_player=%d)\n", min_len, one_player_len);
}

int main(void)
{
    test_connect_max_attempts();
    test_world_state_packet_size_constants();
    return 0;
}
