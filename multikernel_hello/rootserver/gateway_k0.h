/* K0 gate pipeline entry points (corrected design). SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>
#include "gw_proto.h"

const char *gw_engine_label(void);
/* Wire the D1 PDs, run their init() (engine_init, D3 engine-only, D2). */
void gw_init(void);
/* One mk_channel message from K1 (request fragment or garbage). */
void gw_on_frame(uint16_t type, const uint8_t *payload, uint32_t len);
uint64_t gw_rejected_frames(void);
uint64_t gw_requests_served(void);
uint64_t gw_no_response(void);
uint64_t gw_send_failures(void);
void gw_note_corrupt_dropped(void);
uint64_t gw_corrupt_dropped(void);

/* Provided by main.c (K0). */
int k0_send(uint16_t type, const void *payload, uint32_t len);
void pet_watchdog_k0(void);
