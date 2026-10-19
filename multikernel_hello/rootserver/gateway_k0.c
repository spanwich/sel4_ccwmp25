/*
 * K0 side of the paper experiments: the access-control engine (none |
 * EverParse | Cedar, selected at build time by MK_ENGINE), the in-image
 * equivalence run (C1), the decision-cost run (C2), and the request handler
 * that serves K1 over mk_channel (C0/C3/C4/C6).
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>

#include "rs.h"
#include "gateway_k0.h"
#include "gw_proto.h"

#if KERNEL_ID == 0

#include "gateway_abi.h"
#include "gateway_common.h"
#ifndef MK_ENGINE_none
#include "gateway_engine.h"
#include "gateway_benchmark_profile.h"
int equivalence_main(void);
#endif

#define C2_WARMUP  100u
#define C2_SAMPLES 1000u

static uint64_t served, rejected_frames;

const char *gw_engine_label(void)
{
    return MK_ENGINE_NAME;
}

static DecisionResult decide(const DecisionInput *in)
{
#ifdef MK_ENGINE_none
    (void)in;
    return DECISION_ALLOW; /* C0: pass-through baseline, no ACL */
#else
    return engine_decide(in);
#endif
}

static UpdateResult update(const DecisionInput *in, const uint8_t *body, uint32_t len)
{
#ifdef MK_ENGINE_none
    (void)in;
    (void)body;
    (void)len;
    return UPDATE_APPLIED;
#else
    return engine_update(in, body, len);
#endif
}

static uint64_t policy_digest(void)
{
#ifdef MK_ENGINE_none
    return 0;
#else
    CompactPolicy p;
    memset(&p, 0, sizeof(p));
    if (engine_get_policy(&p) != 0) {
        return 0;
    }
    uint64_t h = 0xcbf29ce484222325ull;
    uint8_t b[1 + MAX_RULES * POLICY_RULE_SIZE];
    b[0] = p.num_rules;
    for (unsigned i = 0; i < MAX_RULES; i++) {
        uint8_t *r = b + 1 + i * POLICY_RULE_SIZE;
        write_u32_le(r, 0, p.rules[i].path_id);
        r[4] = p.rules[i].method;
        r[5] = p.rules[i].min_role;
        write_u16_le(r, 6, p.rules[i].required_scope);
    }
    for (unsigned i = 0; i < sizeof(b); i++) {
        h = (h ^ b[i]) * 0x100000001b3ull;
    }
    return h;
#endif
}

static void install_default_policy(void)
{
#ifndef MK_ENGINE_none
    CompactPolicy p;
    uint8_t body[POLICY_UPDATE_BODY_SIZE];
    const DecisionInput admin = { PATH_POLICY, METHOD_PUT, ROLE_ADMIN, UINT16_MAX, 0u };
    compact_policy_init_default(&p);
    memset(body, 0, sizeof(body));
    body[0] = p.num_rules;
    for (unsigned i = 0; i < MAX_RULES; i++) {
        uint8_t *r = body + 1 + i * POLICY_RULE_SIZE;
        write_u32_le(r, 0, p.rules[i].path_id);
        r[4] = p.rules[i].method;
        r[5] = p.rules[i].min_role;
        write_u16_le(r, 6, p.rules[i].required_scope);
    }
    UpdateResult u = engine_update(&admin, body, sizeof(body));
    put_str(u == UPDATE_APPLIED ? "gw: default policy installed\n"
                                : "gw: default policy install FAILED\n");
#endif
}

void gw_init(void)
{
#ifndef MK_ENGINE_none
    if (engine_init() != 0) {
        halt("engine_init", 0);
    }
#endif
    put_str("gw: engine=" MK_ENGINE_NAME " ready, digest=");
    put_hex(policy_digest());
    put_str("\n");
}

/* C1: returns 0 on EQUIVALENCE_OK. Engine none has nothing to compare. */
int gw_equivalence(void)
{
#ifdef MK_ENGINE_none
    put_str("c1: engine none, skipped\n");
    return 0;
#else
    uint64_t t0 = rs_cntpct();
    put_str("c1: start engine=" MK_ENGINE_NAME "\n");
    int rc = equivalence_main();
    fflush(stdout);
    fflush(stderr);
    put_str("c1: done rc=");
    put_dec((uint64_t)rc);
    put_str(" ms=");
    put_dec((rs_cntpct() - t0) * 1000u / rs_cntfrq());
    put_str("\n");
    install_default_policy(); /* C1 leaves a randomized policy behind */
    return rc;
#endif
}

#ifndef MK_ENGINE_none
static int cmp64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void c2_case(const char *name, const DecisionInput *in, int expected, int empty)
{
    static uint64_t cyc[C2_SAMPLES];
    volatile DecisionResult sink = DECISION_DENY;
    uint64_t mismatches = 0, sum = 0;

    for (unsigned i = 0; i < C2_WARMUP; i++) {
        if (!empty) {
            sink = engine_decide(in);
        }
    }
    uint64_t t0 = rs_cntpct();
    for (unsigned i = 0; i < C2_SAMPLES; i++) {
        uint64_t s = rs_pmccntr();
        if (!empty) {
            sink = engine_decide(in);
        }
        uint64_t e = rs_pmccntr();
        cyc[i] = e - s;
        sum += cyc[i];
        if (!empty && (int)sink != expected) {
            mismatches++;
        }
    }
    uint64_t ticks = rs_cntpct() - t0;
    qsort(cyc, C2_SAMPLES, sizeof(cyc[0]), cmp64);
    printf("C2_RESULT,engine=%s,case=%s,samples=%u,warmup=%u,unit=cycles,cpu_hz=1200000000,"
           "min=%llu,p50=%llu,p90=%llu,p99=%llu,max=%llu,mean=%llu,"
           "loop_cntpct_ticks=%llu,cntfrq=%llu,mismatches=%llu\n",
           MK_ENGINE_NAME, name, C2_SAMPLES, C2_WARMUP,
           (unsigned long long)cyc[0], (unsigned long long)cyc[C2_SAMPLES / 2 - 1],
           (unsigned long long)cyc[C2_SAMPLES * 9 / 10 - 1],
           (unsigned long long)cyc[C2_SAMPLES * 99 / 100 - 1],
           (unsigned long long)cyc[C2_SAMPLES - 1],
           (unsigned long long)(sum / C2_SAMPLES), (unsigned long long)ticks,
           (unsigned long long)rs_cntfrq(), (unsigned long long)mismatches);
}
#endif

/* C2: decision cost on K0, same inputs as the x86 timing.c (deployment8). */
void gw_timing(void)
{
#ifndef MK_ENGINE_none
    const PolicyRule first = gateway_benchmark_rule(0u);
    const PolicyRule last = gateway_benchmark_rule(GATEWAY_BENCHMARK_ACTIVE_RULES - 1u);
    const DecisionInput allow_first = {
        first.path_id, (uint8_t)(first.method & (uint8_t)(0u - first.method)),
        ROLE_ADMIN, UINT16_MAX, 0u };
    const DecisionInput allow_last = {
        last.path_id, (uint8_t)(last.method & (uint8_t)(0u - last.method)),
        ROLE_ADMIN, UINT16_MAX, 0u };
    const DecisionInput deny_path = {
        UINT32_C(0xfeedbeef), METHOD_GET, ROLE_ADMIN, UINT16_MAX, 0u };

    install_default_policy();
    c2_case("empty_bracket", &deny_path, DECISION_DENY, 1);
    c2_case("allow_first", &allow_first, DECISION_ALLOW, 0);
    c2_case("allow_last", &allow_last, DECISION_ALLOW, 0);
    c2_case("deny_path", &deny_path, DECISION_DENY, 0);
    fflush(stdout);
#endif
}

/* Serve one K1 request. Returns the reply message type (0 = drop). */
uint16_t gw_handle(uint16_t type, const uint8_t *payload, uint32_t len, gw_rsp_t *rsp)
{
    gw_req_t req;

    memset(rsp, 0, sizeof(*rsp));
    /* Fail closed on anything malformed: K1 is not trusted for the ACL. */
    if ((type != GW_MSG_DECIDE && type != GW_MSG_UPDATE) ||
        len < offsetof(gw_req_t, body)) {
        rejected_frames++;
        return 0;
    }
    memset(&req, 0, sizeof(req));
    memcpy(&req, payload, len < sizeof(req) ? len : sizeof(req));
    rsp->req_id = req.req_id;
    if (req.body_len > GW_BODY_MAX || offsetof(gw_req_t, body) + req.body_len > len) {
        rejected_frames++;
        rsp->result = type == GW_MSG_DECIDE ? DECISION_DENY : UPDATE_REJECTED;
        rsp->policy_digest = policy_digest();
        return type == GW_MSG_DECIDE ? GW_MSG_VERDICT : GW_MSG_UPDATE_RESULT;
    }
    const DecisionInput in = { req.path_id, req.method, req.role, req.scope, req.rate_count };
    served++;
    if (type == GW_MSG_UPDATE) {
        uint64_t s = rs_pmccntr();
        UpdateResult u = update(&in, req.body, req.body_len);
        rsp->engine_cycles = rs_pmccntr() - s;
        rsp->result = u;
        rsp->policy_digest = policy_digest();
        return GW_MSG_UPDATE_RESULT;
    }
    uint64_t s = rs_pmccntr();
    DecisionResult d = decide(&in);
    rsp->engine_cycles = rs_pmccntr() - s;
    rsp->result = d;
    if (d == DECISION_ALLOW) {
        /* Backend: a small fixed JSON document per path. */
        int n = snprintf(rsp->body, sizeof(rsp->body),
                         "{\"backend\":\"k0\",\"path\":\"0x%08x\",\"served\":%llu}\n",
                         (unsigned)req.path_id, (unsigned long long)served);
        rsp->body_len = n > 0 ? (uint32_t)n : 0;
        if (in.path_id == PATH_POLICY && in.method == METHOD_GET) {
            rsp->policy_digest = policy_digest();
        }
    }
    return GW_MSG_VERDICT;
}

uint64_t gw_rejected_frames(void)
{
    return rejected_frames;
}

#endif /* KERNEL_ID == 0 */
