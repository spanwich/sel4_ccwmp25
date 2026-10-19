/*
 * K1 <-> K0 request protocol over mk_channel (paper experiments).
 * K1 (network-facing, untrusted for the ACL) turns an HTTP request into a
 * DecisionInput; K0 decides (and applies policy updates) and replies.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>

#define GW_MSG_DECIDE        16u /* K1->K0: gw_req_t */
#define GW_MSG_VERDICT       17u /* K0->K1: gw_rsp_t */
#define GW_MSG_UPDATE        18u /* K1->K0: gw_req_t + body */
#define GW_MSG_UPDATE_RESULT 19u /* K0->K1: gw_rsp_t */

#define GW_BODY_MAX 256u

/* Fixed little-endian layout, no reliance on struct padding across kernels. */
typedef struct __attribute__((packed)) {
    uint32_t req_id;
    uint32_t path_id;
    uint8_t method;
    uint8_t role;
    uint16_t scope;
    uint8_t rate_count;
    uint8_t pad[3];
    uint32_t body_len;
    uint8_t body[GW_BODY_MAX];
} gw_req_t;

typedef struct __attribute__((packed)) {
    uint32_t req_id;
    uint32_t result;        /* DecisionResult or UpdateResult */
    uint64_t engine_cycles; /* PMU cycles in engine_decide/engine_update */
    uint64_t policy_digest; /* FNV-1a over the active CompactPolicy */
    uint32_t body_len;      /* backend reply body */
    char body[192];
} gw_rsp_t;
