/* K1 network stack entry points. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>
#include "mk_channel.h"
#include "gw_proto.h"

void net_init(void *dma_va, uint64_t dma_pa, const char *build);
void net_poll(void);

/* One request/response exchange with K0 (gw_proto.h fragments): sends
 * len bytes as message kind, reassembles K0's response into rsp. meta (may
 * be NULL) receives the response's fragment header. 0 on success, -1 on
 * timeout or a malformed reply. */
int k1_exchange(uint16_t kind, uint16_t epoch, const uint8_t *req, uint32_t len,
                uint8_t *rsp, uint32_t cap, uint32_t *rsp_len, gw_frag_hdr_t *meta);

/* Resilience counters: K0's 1 s stats frame, and K1's own tick. */
void k1_on_k0_stats(const void *payload, uint32_t len);
void net_tick(uint64_t tick, uint32_t busy_ppm);

/* Provided by main.c (K1). */
int k1_send_frame(uint16_t type, const void *payload, uint32_t len);
int k1_recv_frame(mk_channel_message_t *m, uint64_t deadline);
uint64_t k1_now(void);
uint64_t k1_freq(void);
/* D7 compromised-K1 probes; writes JSON to out, returns length or -1. */
int k1_probe(const char *name, char *out, unsigned out_len);
