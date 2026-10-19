/*
 * D0a/D0b baseline engine for K0 (images "raw" and "gateonly"): the full
 * gate pipeline runs (F* extractor, authenticator, rate counter,
 * pipeline_process_frame), but every decision is ALLOW and every update is
 * APPLIED. Not a policy engine; it isolates the cost of the gate path.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <string.h>

#include "gateway_common.h"
#include "gateway_engine.h"

static CompactPolicy policy;

int engine_init(void)
{
    compact_policy_init_default(&policy);
    return 0;
}

DecisionResult engine_decide(const DecisionInput *input)
{
    (void)input;
    return DECISION_ALLOW;
}

UpdateResult engine_update(const DecisionInput *input, const uint8_t *body, uint32_t body_len)
{
    (void)input;
    (void)body;
    (void)body_len;
    return UPDATE_APPLIED;
}

int engine_get_policy(CompactPolicy *out)
{
    memcpy(out, &policy, sizeof(*out));
    return 0;
}

const char *engine_name(void)
{
    return "accept_all";
}
