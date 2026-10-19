/* K1 network stack entry points. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

void net_init(void *dma_va, uint64_t dma_pa, const char *build);
void net_poll(void);

/* Provided by main.c (K1): synchronous request to K0 over mk_channel.
 * Returns 0 on reply, -1 on timeout or channel error. */
int k1_call_k0(uint16_t type, const void *req, uint32_t len, void *rsp, uint32_t rsp_len);
/* C6 compromised-K1 probes; writes JSON to out, returns length or -1. */
int k1_probe(const char *name, char *out, unsigned out_len);
