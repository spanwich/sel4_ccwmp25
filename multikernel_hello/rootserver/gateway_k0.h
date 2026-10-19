/* K0 paper-experiment entry points. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>
#include "gw_proto.h"

const char *gw_engine_label(void);
void gw_init(void);
void gw_timing(void);
int gw_equivalence(void);
uint16_t gw_handle(uint16_t type, const uint8_t *payload, uint32_t len, gw_rsp_t *rsp);
uint64_t gw_rejected_frames(void);
