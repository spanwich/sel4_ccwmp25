/*
 * K1 <-> K0 message protocol over mk_channel (corrected design, see
 * studies/ccmp25-board/CORRECTED_DESIGN.md).
 *
 * K1 (untrusted user code) forwards the raw HTTP request bytes it read from
 * the client; K0 runs the x86 D1 gate pipeline and answers with the raw HTTP
 * response bytes. A message (<= DEMO_PAYLOAD_BYTES = 4096 B) is split into
 * fragments, because an mk_channel slot carries 512 B (ABI v2 unchanged).
 * K0 reassembles strictly in order and fails closed: any fragment that does
 * not continue the current message discards it.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>

#define GW_MSG_K0_READY 20u /* K0->K1: gate initialised, D2/D3 runs done */
#define GW_MSG_REQ_FRAG 21u /* K1->K0: request fragment */
#define GW_MSG_RSP_FRAG 22u /* K0->K1: response fragment */

#define GW_MSG_MAX 4096u /* = DEMO_PAYLOAD_BYTES */

/* Fixed little-endian layout. */
typedef struct __attribute__((packed)) {
    uint32_t msg_id; /* chosen by K1, echoed by K0 */
    uint32_t total;  /* message bytes, <= GW_MSG_MAX */
    uint32_t offset; /* of this fragment's data */
    uint16_t kind;   /* DemoMessageKind: RAW_HTTP (req) / HTTP_RESPONSE (rsp) */
    uint16_t epoch;  /* request epoch, echoed */
    /* Response only (measurement, PMU cycles on K0; zero in requests). */
    uint64_t k0_cycles;   /* whole K0 path: queue in -> response out */
    uint32_t gate_cycles; /* inside policy_gate (extract, counter, engine) */
    uint32_t auth_cycles; /* inside authenticator */
    uint32_t app_cycles;  /* inside protected_app (incl. HTTP formatting) */
    uint32_t reserved;
} gw_frag_hdr_t;

#define GW_FRAG_DATA (512u - (uint32_t)sizeof(gw_frag_hdr_t))

_Static_assert(sizeof(gw_frag_hdr_t) == 40u, "fragment header ABI");

/* Resilience test: K0's 1 s counters, sent to K1 at every K0 tick and kept
 * there for the control port ("counters"). Measurement only. */
#define GW_MSG_K0_STATS 23u

typedef struct __attribute__((packed)) {
    uint64_t tick;
    uint64_t uptime_ms;
    uint32_t busy_ppm;        /* K0 PMU cycles in the last second / cycles per second */
    uint32_t pets;            /* watchdog pets in the last second */
    uint64_t served;          /* cumulative responses sent */
    uint64_t rejected_frames; /* cumulative malformed/out-of-order frames dropped */
    uint64_t no_response;     /* cumulative requests that produced no response */
    uint32_t queue_depth;     /* K1->K0 fragments not yet consumed, at the tick */
    uint32_t send_failures;   /* cumulative K0->K1 sends refused (ring full) */
    uint64_t corrupt_dropped; /* cumulative bad-sequence slots dropped and resynced */
} gw_k0_stats_t;
