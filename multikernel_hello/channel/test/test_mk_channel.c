/*
 * Host unit test for mk_channel: round trip, full ring, corrupt header and
 * the bounded receive (mk_channel_recv_until) with a fake clock.
 *
 *   cc -O2 -Wall -Wextra -I.. ../mk_channel.c test_mk_channel.c -o t && ./t
 */
#include "mk_channel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static uint64_t fake_now;
static unsigned waits;

static uint64_t now_fn(void *cookie) { (void)cookie; return fake_now; }

/* Each wait advances the clock by 3 ticks, like a timer interrupt would. */
static int wait_until_fn(void *cookie, uint64_t deadline)
{
    (void)cookie;
    (void)deadline;
    waits++;
    fake_now += 3;
    return 0;
}

static int wait_fail_fn(void *cookie, uint64_t deadline)
{
    (void)cookie;
    (void)deadline;
    return -1;
}

int main(void)
{
    mk_channel_region_t *r0 = aligned_alloc(64, sizeof(*r0));
    mk_channel_region_t *r1 = aligned_alloc(64, sizeof(*r1));
    mk_channel_t c0, c1;
    mk_channel_message_t m;
    uint8_t buf[MK_CHANNEL_PAYLOAD_SIZE];

    CHECK(r0 && r1);
    mk_channel_region_prepare(r0, 0, 42);
    mk_channel_region_prepare(r1, 1, 42);
    mk_channel_region_mark_ready(r0);
    mk_channel_region_mark_ready(r1);
    CHECK(mk_channel_attach(&c0, 0, r0, r1, NULL, NULL, NULL) == MK_CHANNEL_OK);
    CHECK(mk_channel_attach(&c1, 1, r1, r0, NULL, NULL, NULL) == MK_CHANNEL_OK);

    /* empty ring: times out at the deadline, after a bounded number of waits */
    fake_now = 100;
    waits = 0;
    CHECK(mk_channel_recv_until(&c1, &m, 110, now_fn, wait_until_fn) == MK_CHANNEL_TIMEOUT);
    CHECK(fake_now >= 110 && waits == 4);

    /* deadline already past: no wait at all */
    waits = 0;
    CHECK(mk_channel_recv_until(&c1, &m, 0, now_fn, wait_until_fn) == MK_CHANNEL_TIMEOUT);
    CHECK(waits == 0);

    /* wait failure propagates */
    CHECK(mk_channel_recv_until(&c1, &m, ~0ull, now_fn, wait_fail_fn) == MK_CHANNEL_WAIT_FAILED);
    CHECK(mk_channel_recv_until(&c1, &m, ~0ull, NULL, wait_until_fn) == MK_CHANNEL_INVALID_ARGUMENT);

    /* a pending message is delivered even when the deadline has passed */
    for (size_t i = 0; i < sizeof(buf); i++) {
        buf[i] = (uint8_t)i;
    }
    CHECK(mk_channel_try_send(&c0, 7, 1, buf, 13, NULL) == MK_CHANNEL_OK);
    CHECK(mk_channel_recv_until(&c1, &m, 0, now_fn, wait_until_fn) == MK_CHANNEL_OK);
    CHECK(m.type == 7 && m.flags == 1 && m.length == 13 && m.sequence == 1);
    CHECK(memcmp(m.payload, buf, 13) == 0);

    /* full ring */
    unsigned sent = 0;
    while (mk_channel_try_send(&c0, 1, 0, buf, sizeof(buf), NULL) == MK_CHANNEL_OK) {
        sent++;
    }
    CHECK(sent == MK_CHANNEL_SLOT_COUNT);
    CHECK(r0->control.full == 1);

    /* corrupt header: oversize length is rejected and counted */
    r0->slots[r1->control.rx_tail & (MK_CHANNEL_SLOT_COUNT - 1u)].length =
        MK_CHANNEL_PAYLOAD_SIZE + 1;
    CHECK(mk_channel_try_recv(&c1, &m) == MK_CHANNEL_CORRUPT);
    CHECK(r1->control.corrupt == 1);

    printf("%s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures != 0;
}
