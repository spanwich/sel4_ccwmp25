/* K1 network stack entry points. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <stdint.h>

void net_init(void *dma_va, uint64_t dma_pa, const char *build, uint64_t (*pongs)(void));
void net_poll(void);
