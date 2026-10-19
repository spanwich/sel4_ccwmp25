/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Compile-time contract for benchmark-only logical policy profiles.
 */
#ifndef CEDAR_GATEWAY_BENCHMARK_PROFILE_H
#define CEDAR_GATEWAY_BENCHMARK_PROFILE_H

#include "gateway_abi.h"

#ifndef GATEWAY_BENCHMARK_PROFILE_ID
#define GATEWAY_BENCHMARK_PROFILE_ID 1
#endif
#ifndef GATEWAY_BENCHMARK_PROFILE_NAME
#define GATEWAY_BENCHMARK_PROFILE_NAME "deployment8"
#endif
#ifndef GATEWAY_BENCHMARK_ACTIVE_RULES
#define GATEWAY_BENCHMARK_ACTIVE_RULES 3u
#endif
#ifndef GATEWAY_BENCHMARK_CAPACITY_SLOTS
#define GATEWAY_BENCHMARK_CAPACITY_SLOTS 8u
#endif
#ifndef GATEWAY_BENCHMARK_PARTITIONS
#define GATEWAY_BENCHMARK_PARTITIONS 1u
#endif
#ifndef GATEWAY_BENCHMARK_LAYOUT_NAME
#define GATEWAY_BENCHMARK_LAYOUT_NAME "fixed8"
#endif
#ifndef GATEWAY_BENCHMARK_READ_ONLY
#define GATEWAY_BENCHMARK_READ_ONLY 0
#endif
#ifndef GATEWAY_BENCHMARK_DOMAIN_VALID
#define GATEWAY_BENCHMARK_DOMAIN_VALID 1
#endif
#ifndef GATEWAY_BENCHMARK_COMPARISON_AVAILABLE
#define GATEWAY_BENCHMARK_COMPARISON_AVAILABLE "true"
#endif
#ifndef GATEWAY_BENCHMARK_EVERPARSE_REASON
#define GATEWAY_BENCHMARK_EVERPARSE_REASON "none"
#endif

#define GATEWAY_BENCHMARK_MAX_CAPACITY 2048u
#define GATEWAY_BENCHMARK_PATH_BASE UINT32_C(0x50000000)
#define GATEWAY_BENCHMARK_OPNSENSE_FIRST_PATH UINT32_C(1)

#if GATEWAY_BENCHMARK_ACTIVE_RULES < 1
#error "benchmark profile must have at least one active rule"
#endif
#if GATEWAY_BENCHMARK_ACTIVE_RULES > GATEWAY_BENCHMARK_CAPACITY_SLOTS
#error "active benchmark rules exceed capacity"
#endif
#if GATEWAY_BENCHMARK_CAPACITY_SLOTS > GATEWAY_BENCHMARK_MAX_CAPACITY
#error "benchmark capacity exceeds the implemented 2048-slot bound"
#endif
#if GATEWAY_BENCHMARK_PROFILE_ID < 8 || GATEWAY_BENCHMARK_PROFILE_ID > 11
#if GATEWAY_BENCHMARK_PARTITIONS != \
    ((GATEWAY_BENCHMARK_CAPACITY_SLOTS + MAX_RULES - 1u) / MAX_RULES)
#error "benchmark partition metadata does not match configured capacity"
#endif
#endif
#if GATEWAY_BENCHMARK_READ_ONLY != 0 && GATEWAY_BENCHMARK_READ_ONLY != 1
#error "benchmark read-only flag must be zero or one"
#endif
#if GATEWAY_BENCHMARK_DOMAIN_VALID != 1
#error "benchmark profile input domain is invalid"
#endif
#if GATEWAY_BENCHMARK_READ_ONLY == 0 && \
    (GATEWAY_BENCHMARK_ACTIVE_RULES != 3u || \
     GATEWAY_BENCHMARK_CAPACITY_SLOTS != MAX_RULES)
#error "only deployment8 may use the production update path"
#endif

static inline PolicyRule gateway_benchmark_rule(size_t index)
{
    static const uint16_t scopes[8] = {
        0u, 1u, 2u, 4u, 3u, 5u, 6u, 7u
    };
    if (GATEWAY_BENCHMARK_PROFILE_ID == 1) {
        static const PolicyRule deployment_rules[MAX_RULES] = {
            { PATH_LOGOUT, METHOD_POST, ROLE_OPERATOR, UINT16_C(0) },
            { PATH_POLICY, METHOD_GET, ROLE_ADMIN, UINT16_C(4) },
            { PATH_POLICY, METHOD_PUT, ROLE_ADMIN, UINT16_C(4) },
            { INACTIVE_PATH_ID, 0u, ROLE_NONE, UINT16_C(0) },
            { INACTIVE_PATH_ID, 0u, ROLE_NONE, UINT16_C(0) },
            { INACTIVE_PATH_ID, 0u, ROLE_NONE, UINT16_C(0) },
            { INACTIVE_PATH_ID, 0u, ROLE_NONE, UINT16_C(0) },
            { INACTIVE_PATH_ID, 0u, ROLE_NONE, UINT16_C(0) },
        };
        return deployment_rules[index < MAX_RULES ? index : MAX_RULES - 1u];
    }
    if (GATEWAY_BENCHMARK_PROFILE_ID == 6) {
        return (PolicyRule){
            .path_id = GATEWAY_BENCHMARK_OPNSENSE_FIRST_PATH + (uint32_t)index,
            .method = UINT8_C(0x3f),
            .min_role = ROLE_NONE,
            .required_scope = UINT16_C(0),
        };
    }
    return (PolicyRule){
        .path_id = GATEWAY_BENCHMARK_PATH_BASE + (uint32_t)index,
        .method = (index % 2u) == 0u ? METHOD_GET : METHOD_POST,
        .min_role = (uint8_t)(index % 3u),
        .required_scope = scopes[index % 8u],
    };
}

#endif
