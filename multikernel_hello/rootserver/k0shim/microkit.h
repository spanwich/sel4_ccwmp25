/*
 * K0 stand-in for <microkit.h>, used only to compile the unchanged x86 D1
 * gate PDs (policy_gate.c, authenticator.c, protected_app.c) into K0's bare
 * rootserver. Microkit's shared-memory queues stay as they are (plain
 * memory in one address space); microkit_notify() marks the peer channel
 * pending and gateway_k0.c's run-to-idle dispatcher delivers notified().
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once
#include <stdint.h>

typedef unsigned int microkit_channel;

void microkit_notify(microkit_channel ch);
void microkit_dbg_puts(const char *s);
void microkit_dbg_putc(int c);
void microkit_dbg_put32(uint32_t v);
