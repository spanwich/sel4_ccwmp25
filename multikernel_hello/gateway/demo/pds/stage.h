/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef POLICY_GATEWAY_DEMO_STAGE_H
#define POLICY_GATEWAY_DEMO_STAGE_H

#include "demo_wire.h"

#include <microkit.h>

extern DemoConfig demo_config;

typedef void (*DemoStageTransform)(DemoMessage *message);

static DemoMessage pending_message;
static bool message_pending;

static inline bool demo_stage_flush(void)
{
    if (!message_pending) {
        return true;
    }
    if (!demo_queue_send(demo_output_queue(&demo_config),
                         &pending_message)) {
        return false;
    }
    message_pending = false;
    microkit_notify(demo_config.output_channel);
    return true;
}

static inline void demo_stage_forward(const DemoMessage *message)
{
    if (demo_queue_send(demo_output_queue(&demo_config), message)) {
        microkit_notify(demo_config.output_channel);
        return;
    }
    pending_message = *message;
    message_pending = true;
}

static inline void demo_stage_service(DemoStageTransform transform)
{
    if (!demo_stage_flush()) {
        return;
    }
    DemoMessage message;
    if (!demo_queue_receive(demo_input_queue(&demo_config), &message)) {
        return;
    }
    microkit_notify(demo_config.input_channel);
    if (transform != NULL) {
        transform(&message);
    }
    demo_stage_forward(&message);
}

#endif
