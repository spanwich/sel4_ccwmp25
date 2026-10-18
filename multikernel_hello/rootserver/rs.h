/*
 * Helpers shared by the multikernel rootserver's source files.
 * SPDX-License-Identifier: BSD-2-Clause
 */
#pragma once

#include <stdint.h>
#include <sel4/sel4.h>

extern const seL4_BootInfo *bi;

void put_str(const char *s);
void put_dec(uint64_t n);
void put_hex(uint64_t n);
void halt(const char *why, uint64_t err);

seL4_CPtr alloc_slot(void);
seL4_CPtr alloc_object(seL4_Word type, seL4_Word size_bits, const char *what);
void map_frame_attr(seL4_CPtr frame, seL4_Word vaddr, seL4_CapRights_t rights,
                    seL4_ARM_VMAttributes attr);
void *map_device_page(uint64_t paddr, seL4_Word vaddr);

static inline uint64_t rs_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline uint64_t rs_cntfrq(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline void rs_udelay(uint64_t us)
{
    uint64_t t0 = rs_cntpct(), ticks = us * rs_cntfrq() / 1000000u;
    while (rs_cntpct() - t0 < ticks) {
    }
}

/* Fixed virtual addresses for driver mappings (below MK_SHARED_VADDR). */
#define RS_VADDR_OPTEE_SHM  0x30000000ul
#define RS_VADDR_RCC        0x30010000ul
#define RS_VADDR_ETH1       0x30020000ul
#define RS_VADDR_GPIO_BASE  0x30030000ul /* + port * 0x1000 */
#define RS_VADDR_SYSCFG     0x30040000ul

#if KERNEL_ID == 0
void smc_call(const seL4_ARM_SMCContext *req, seL4_ARM_SMCContext *resp);
/* N1: ETH1 clocks via SCMI over OP-TEE, then RCC_ETH1CFGR gates. */
void eth1_clocks_enable(void);
#endif
