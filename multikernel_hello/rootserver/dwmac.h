/* K1 ETH1 (DWMAC) driver interface. SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#include <stdint.h>

int eth1_probe(void);                          /* N2: returns 0 if PHY found */
int eth1_link(unsigned *speed, unsigned *full); /* 1 if copper link is up */
int mdio_read(unsigned reg);
int mdio_write(unsigned reg, uint16_t val);
void *eth1_regs(void);

struct eth1_stats {
    uint64_t rx, tx, rx_err, tx_err, tx_full, dma_err;
};
extern struct eth1_stats eth1_stats;

void eth1_dma_init(void *va, uint64_t pa, const uint8_t mac[6]);
void eth1_mac_link(unsigned speed, unsigned full);
int eth1_tx(const uint8_t *frame, unsigned len);
unsigned eth1_rx(void (*cb)(const uint8_t *frame, unsigned len));
