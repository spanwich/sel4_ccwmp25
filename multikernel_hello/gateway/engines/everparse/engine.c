/*
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include "gateway_common.h"
#include "gateway_engine.h"
#include "../../verified/rate_flow/RateFlow.h"
#include "RbacPolicyWrapper.h"
/* Paper spec decision 2026-09-30c: access decisions by the verified composed
 * gate (papers repo experiments/verified-combiner, copied verbatim into
 * verified_gate/). GATEWAY_VERIFIED_GATE=0 restores the monolithic validator. */
#ifndef GATEWAY_VERIFIED_GATE
#define GATEWAY_VERIFIED_GATE 1
#endif
#if GATEWAY_VERIFIED_GATE
#include "verified_gate/Gate.h"
#endif
/* Policy updates go through the verified PolicyBlob update gate
 * (paper-experiments/verified-policyblob, copied verbatim into
 * verified_policyblob/): it builds the PolicyBlob buffer, runs the generated
 * validator and decodes the rules only on acceptance. */
#include "verified_policyblob/Blob.h"

#if defined(GATEWAY_TIMING_BENCHMARK) && GATEWAY_TIMING_BENCHMARK
#include "gateway_benchmark_profile.h"
#endif

#include <string.h>

static CompactPolicy active_policy;
static bool initialized;
#if defined(GATEWAY_TIMING_BENCHMARK) && GATEWAY_TIMING_BENCHMARK
static bool benchmark_active;
static size_t benchmark_active_rules;
static PolicyRule benchmark_rules[GATEWAY_BENCHMARK_CAPACITY_SLOTS];
#endif

void RbacPolicyEverParseError(const char *struct_name, const char *field_name,
                              const char *reason)
{
    (void)struct_name;
    (void)field_name;
    (void)reason;
}

#if !GATEWAY_VERIFIED_GATE
static void encode_rules(uint8_t *buffer, size_t offset,
                         const PolicyRule *rules)
{
    for (size_t i = 0u; i < MAX_RULES; ++i) {
        const PolicyRule *rule = &rules[i];
        const size_t rule_offset = offset + i * POLICY_RULE_SIZE;
        write_u32_le(buffer, rule_offset, rule->path_id);
        buffer[rule_offset + 4u] = rule->method;
        buffer[rule_offset + 5u] = rule->min_role;
        write_u16_le(buffer, rule_offset + 6u, rule->required_scope);
    }
}
#endif

#if defined(GATEWAY_TIMING_BENCHMARK) && GATEWAY_TIMING_BENCHMARK
int engine_benchmark_configure(void)
{
    if (!initialized || GATEWAY_BENCHMARK_DOMAIN_VALID != 1 ||
        GATEWAY_BENCHMARK_ACTIVE_RULES < 1u ||
        GATEWAY_BENCHMARK_ACTIVE_RULES > GATEWAY_BENCHMARK_CAPACITY_SLOTS ||
        GATEWAY_BENCHMARK_CAPACITY_SLOTS > GATEWAY_BENCHMARK_MAX_CAPACITY ||
        (GATEWAY_BENCHMARK_PARTITIONS != 0u && GATEWAY_BENCHMARK_PARTITIONS !=
            (GATEWAY_BENCHMARK_CAPACITY_SLOTS + MAX_RULES - 1u) / MAX_RULES)) {
        return -1;
    }
    for (size_t i = 0u; i < GATEWAY_BENCHMARK_CAPACITY_SLOTS; ++i) {
        benchmark_rules[i] = i < GATEWAY_BENCHMARK_ACTIVE_RULES
            ? gateway_benchmark_rule(i)
            : (PolicyRule){
                .path_id = INACTIVE_PATH_ID,
                .method = 0u,
                .min_role = ROLE_NONE,
                .required_scope = 0u,
            };
    }
    benchmark_active_rules = GATEWAY_BENCHMARK_ACTIVE_RULES;
    benchmark_active = true;
    return 0;
}

int engine_benchmark_restore(void)
{
    if (!initialized) {
        return -1;
    }
    benchmark_active = false;
    benchmark_active_rules = 0u;
    compact_policy_init_default(&active_policy);
    return 0;
}
#endif

int engine_init(void)
{
    compact_policy_init_default(&active_policy);
    initialized = true;
    return 0;
}

DecisionResult engine_decide(const DecisionInput *input)
{
    uint8_t buffer[ACCESS_REQUEST_SIZE];

    if (!initialized || input == NULL) {
        return DECISION_INTERNAL_ERROR;
    }
    if (input->rate_count >= MAX_RATE) {
        return DECISION_RATE_LIMITED;
    }

#if defined(GATEWAY_TIMING_BENCHMARK) && GATEWAY_TIMING_BENCHMARK
    if (benchmark_active) {
        extern bool scaling_generated_decide(const PolicyRule *, const DecisionInput *);
        return scaling_generated_decide(benchmark_rules, input)
            ? DECISION_ALLOW : DECISION_DENY;
    }
#endif

#if GATEWAY_VERIFIED_GATE
    (void)buffer;
    {
        /* One partition (n = 1) holding the eight active-policy slots. The
         * rate check above keeps the distinct RATE_LIMITED result; the gate
         * re-checks c < N_max itself. 1 = accept; 0 or 2 (fail closed) = deny. */
        GATE_SPEC_RULE rules[MAX_RULES];
        for (size_t i = 0u; i < MAX_RULES; ++i) {
            const PolicyRule *rule = &active_policy.rules[i];
            rules[i] = (GATE_SPEC_RULE){ .path = rule->path_id, .mask = rule->method,
                                         .min_role = rule->min_role,
                                         .rscope = rule->required_scope };
        }
        GATE_SPEC_REQUEST q = { .rpath = input->path_id, .meth = input->method };
        GATE_SPEC_SESSION s = { .role = input->role, .sscope = input->scope,
                                .count = input->rate_count };
        uint8_t ctxt = 0u;
        return GateLowGateDecide(1u, rules, q, s, &ctxt) == 1u
            ? DECISION_ALLOW : DECISION_DENY;
    }
#else
    memset(buffer, 0, sizeof(buffer));
    buffer[0] = input->role;
    RateFlow_place_access_count(buffer, input->rate_count);
    write_u16_le(buffer, 2u, input->scope);
    encode_rules(buffer, 4u, active_policy.rules);
    write_u32_le(buffer, 68u, input->path_id);
    buffer[72] = input->method;

    return RbacPolicyCheckAccessRequest(buffer, sizeof(buffer))
        ? DECISION_ALLOW : DECISION_DENY;
#endif
}

UpdateResult engine_update(const DecisionInput *input,
                           const uint8_t *body, uint32_t body_len)
{
#if defined(GATEWAY_TIMING_BENCHMARK) && GATEWAY_TIMING_BENCHMARK
    if (benchmark_active) {
        return UPDATE_REJECTED;
    }
#endif
    if (!initialized || input == NULL || body == NULL ||
        body_len < POLICY_UPDATE_BODY_SIZE) {
        return UPDATE_REJECTED;
    }

    /* 1 = applied: the rules are exactly those the validated bytes encode;
     * 0 = rejected; 2 = rejected (fail closed). pol and nr are written only on 1. */
    BLOB_SPEC_SESSION s = { .role = input->role, .count = input->rate_count };
    BLOB_SPEC_RULE pol[MAX_RULES];
    uint8_t nr = 0u;
    uint8_t ctxt = 0u;
    if (BlobLowUpdateDecide(s, (uint8_t *)body, pol, &nr, &ctxt) != 1u) {
        return UPDATE_REJECTED;
    }

    active_policy.num_rules = nr;
    for (size_t i = 0u; i < MAX_RULES; ++i) {
        active_policy.rules[i] = (PolicyRule){ .path_id = pol[i].path,
                                               .method = pol[i].mask,
                                               .min_role = pol[i].min_role,
                                               .required_scope = pol[i].rscope };
    }
    return UPDATE_APPLIED;
}

int engine_get_policy(CompactPolicy *policy)
{
#if defined(GATEWAY_TIMING_BENCHMARK) && GATEWAY_TIMING_BENCHMARK
    if (benchmark_active) {
        return -1;
    }
#endif
    if (!initialized || policy == NULL) {
        return -1;
    }
    *policy = active_policy;
    return 0;
}

const char *engine_name(void)
{
    return "everparse";
}
