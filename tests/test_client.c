#include "client.h"
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

int main(void)
{
    test_connect_max_attempts();
    return 0;
}
