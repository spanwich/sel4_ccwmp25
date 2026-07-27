/*
 * Platform VMM configuration for STM32MP25x (Digi CCMP25-DVK)
 *
 * Defines which device tree nodes and interrupts are exposed to the
 * Linux VM guest running under the seL4 hypervisor.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

/* GIC SPI interrupts start at offset 32 from hardware IRQ numbers */
#define IRQ_SPI_OFFSET 32

/* Path to the GIC node in the guest device tree */
#define GIC_NODE_PATH  "/interrupt-controller@4ac10000"

/*
 * Passthrough IRQs: hardware interrupts forwarded directly to the VM.
 * These correspond to devices passed through via untyped_mmios in devices.camkes.
 *
 * NOTE: IRQ passthrough requires IRQ caps in the capdl spec.
 * Currently empty for minimal boot — add back once IRQ cap
 * allocation is configured (via vm0.dtb or explicit CAmkES IRQ config).
 *
 * USART2: GIC_SPI 115 → hardware IRQ 115 + 32 = 147
 * DWMAC:  GIC_SPI 130 → hardware IRQ 130 + 32 = 162
 */
static const int linux_pt_irqs[] = {
    115 + IRQ_SPI_OFFSET,   /* USART2 (GIC SPI 115) */
};

/* Platform interrupts available for VMM use (not allocated to VM) */
static const int free_plat_interrupts[] = {
    50 + IRQ_SPI_OFFSET,
};

/*
 * Device tree nodes to keep in the guest DTB.
 * The VMM starts with the full DTB and removes everything except
 * nodes listed here and in plat_keep_device_and_subtree.
 */
static const char *plat_keep_devices[] = {
    "/timer",
    "/psci",
    "/aliases",
    "/fixed-clock-usart2",
};

/* Nodes to keep but mark as disabled */
static const char *plat_keep_device_and_disable[] = {};

/* Nodes to keep along with their entire subtree (e.g., GIC children) */
static const char *plat_keep_device_and_subtree[] = {
    GIC_NODE_PATH,
};

/* Nodes to keep with subtree but mark as disabled */
static const char *plat_keep_device_and_subtree_and_disable[] = {};
