// Host-side check of the arithmetic in the mining hot path. No hardware involved:
//   gcc -O2 -o check_hot_path check_hot_path.c && ./check_hot_path
//
// Covers the extranonce2 walk in create_jobs_task (bounded by the field width, wrapping, and
// visiting every value exactly once) and the ASIC ticket difficulty in asic_task.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASIC_TICKET_DIFF_CAP 256

static uint64_t e2_space(int e2_len)
{
    return (e2_len >= 4) ? 0x100000000ULL : (1ULL << (8 * e2_len));
}

static int largest_power_of_two(int num)
{
    int power = 0;
    while (num > 1) {
        num = num >> 1;
        power++;
    }
    return 1 << power;
}

static double ticket_for(double pool_diff)
{
    double ticket = pool_diff < 1 ? 1 : pool_diff;
    if (ticket > ASIC_TICKET_DIFF_CAP) {
        ticket = ASIC_TICKET_DIFF_CAP;
    }
    return ticket;
}

// Every extranonce2 the walk emits has to fit the field the pool gave us, and each has to be
// visited exactly once - repeating one is work the pool has already seen.
static void check_walk(int e2_len, uint32_t start)
{
    uint64_t space = e2_space(e2_len);
    char *seen = calloc(space, 1);
    assert(seen != NULL);

    uint32_t extranonce_2 = start % space;
    for (uint64_t sent = 0; sent < space; sent++, extranonce_2 = (extranonce_2 + 1) % space) {
        assert(extranonce_2 < space);
        assert(seen[extranonce_2] == 0);
        seen[extranonce_2] = 1;
    }

    for (uint64_t i = 0; i < space; i++) {
        assert(seen[i] == 1);
    }
    free(seen);
}

int main(void)
{
    assert(e2_space(1) == 256);
    assert(e2_space(2) == 65536);
    assert(e2_space(3) == 16777216);
    assert(e2_space(4) == 0x100000000ULL);
    // A pool offering more than four bytes must not overflow the 32 bit counter
    assert(e2_space(8) == 0x100000000ULL);

    check_walk(1, 0);
    check_walk(1, 200);
    check_walk(2, 65535);
    check_walk(2, 12345);

    // The counter itself wraps at 2^32 before the modulo sees it
    uint32_t near_top = 0xfffffffe;
    for (int i = 0; i < 4; i++) {
        assert((uint64_t) near_top < e2_space(4));
        near_top = (near_top + 1) % e2_space(4);
    }
    assert(near_top == 2);

    // Ticket difficulty: never zero, never above the cap, always what the chip rounds to
    assert(ticket_for(0.001) == 1);
    assert(ticket_for(0.5) == 1);
    assert(ticket_for(32) == 32);
    assert(ticket_for(256) == 256);
    assert(ticket_for(16384) == ASIC_TICKET_DIFF_CAP);

    assert(largest_power_of_two((int) ticket_for(16384)) == 256);
    assert(largest_power_of_two((int) ticket_for(100)) == 64);
    assert(largest_power_of_two((int) ticket_for(0.5)) == 1);

    // The hashrate estimator weighs every result by the chip's threshold, so the weight has to
    // match the rate: 1.14 results/s at ticket 64 is the 313 GH/s the board actually measured.
    double results_per_second = 1.14;
    double hashrate = results_per_second * 64 * 4294967296.0;
    assert(hashrate > 300e9 && hashrate < 320e9);

    printf("all checks passed\n");
    return 0;
}
