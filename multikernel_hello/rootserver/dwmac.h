/* K1 ETH1 (DWMAC) driver interface. SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <stdint.h>

int eth1_probe(void);                          /* N2: returns 0 if PHY found */
int eth1_link(unsigned *speed, unsigned *full); /* 1 if copper link is up */
int mdio_read(unsigned reg);
int mdio_write(unsigned reg, uint16_t val);
void *eth1_regs(void);
