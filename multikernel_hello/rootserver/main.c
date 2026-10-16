/*
 * Rootserver for the STM32MP25x multikernel (one unicore seL4 kernel per core).
 *
 * K0 starts core 1 into K1 via PSCI CPU_ON, then both kernels run the same
 * idle loop: block on a notification bound to the per-core EL1 physical
 * timer (PPI 30), wake once a second, print a tick, block again. While the
 * rootserver is blocked the kernel runs its idle thread (WFI), so neither
 * core spins.
 *
 * Each tick reports the fraction of the last second the core spent awake,
 * measured with the PMU cycle counter (which stops in WFI) against a
 * one-second busy-wait calibration taken at start-up.
 *
 * Only K0 makes OP-TEE SMCs (watchdog pet, PSCI); K1 makes none.
 */

#include <stdint.h>
#include <sel4/sel4.h>
#include <sel4runtime.h>

#ifndef KERNEL_ID
#define KERNEL_ID 0
#endif

#ifndef ROOTSERVER_BUILD_ID
#define ROOTSERVER_BUILD_ID "ccwmp25-mk-idle-v16"
#endif

/* EL1 physical timer: PPI 14 -> GIC IRQ 30. The EL2 kernel uses CNTHP (26). */
#define PTIMER_IRQ 30

static void put_str(const char *s)
{
    while (*s) {
        seL4_DebugPutChar(*s++);
    }
}

static void put_dec(uint64_t n)
{
    char buf[24];
    int i = 0;

    if (n == 0) {
        seL4_DebugPutChar('0');
        return;
    }
    while (n != 0 && i < (int)sizeof(buf)) {
        buf[i++] = (char)('0' + (n % 10));
        n /= 10;
    }
    while (i > 0) {
        seL4_DebugPutChar(buf[--i]);
    }
}

static void put_hex(uint64_t n)
{
    put_str("0x");
    for (int i = 60; i >= 0; i -= 4) {
        uint64_t d = (n >> i) & 0xfu;
        seL4_DebugPutChar((char)(d < 10 ? '0' + d : 'a' + (d - 10)));
    }
}

/* Fixed-point percentage with 3 decimals, from parts-per-million. */
static void put_ppm_as_pct(uint64_t ppm)
{
    uint64_t frac = (ppm % 10000) / 10;

    put_dec(ppm / 10000);
    seL4_DebugPutChar('.');
    seL4_DebugPutChar((char)('0' + frac / 100));
    seL4_DebugPutChar((char)('0' + (frac / 10) % 10));
    seL4_DebugPutChar((char)('0' + frac % 10));
    seL4_DebugPutChar('%');
}

static void halt(const char *why, seL4_Error err)
{
    put_str("FATAL: ");
    put_str(why);
    put_str(" err=");
    put_dec((uint64_t)err);
    put_str("\n");
    for (;;) {
        seL4_TCB_Suspend(seL4_CapInitThreadTCB);
    }
}

/* ---- generic timer (exported to EL0 by KernelArmExportP{CNT,TMR}User) ---- */

static inline uint64_t cntfrq(void)
{
    uint64_t v;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(v));
    return v;
}

static inline uint64_t cntpct(void)
{
    uint64_t v;
    asm volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline void ptimer_arm(uint64_t deadline)
{
    asm volatile("msr cntp_cval_el0, %0" :: "r"(deadline));
    asm volatile("msr cntp_ctl_el0, %0; isb" :: "r"(1ull)); /* ENABLE, unmasked */
}

/* ---- PMU cycle counter (exported to EL0 by KernelArmExportPMUUser) ---- */

#define PMCR_E        (1ull << 0)
#define PMCR_LC       (1ull << 6)
#define PMCNTEN_C     (1ull << 31)
#define PMCCFILTR_NSH (1ull << 27) /* also count at EL2, where the kernel runs */

static void pmu_init(void)
{
    uint64_t pmcr;

    asm volatile("msr pmccfiltr_el0, %0" :: "r"(PMCCFILTR_NSH));
    asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
    asm volatile("msr pmcr_el0, %0" :: "r"(pmcr | PMCR_E | PMCR_LC));
    asm volatile("msr pmcntenset_el0, %0; isb" :: "r"(PMCNTEN_C));
}

static inline uint64_t pmccntr(void)
{
    uint64_t v;
    asm volatile("isb; mrs %0, pmccntr_el0" : "=r"(v));
    return v;
}

#if KERNEL_ID == 0
/* ---- K0-only OP-TEE SMCs ---- */

#ifndef MK_CORE1_TRAMPOLINE_PADDR
#error "K0 requires the build-derived MK_CORE1_TRAMPOLINE_PADDR"
#endif

#define PSCI_CPU_ON_AARCH64 0xc4000003ull
#define CORE1_MPIDR         0x1ull
#define SMCWD_FUNC_ID       0xbc000000ull
#define SMCWD_PET           3ull

static uint64_t smc(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3)
{
    seL4_ARM_SMCContext req = { .x0 = x0, .x1 = x1, .x2 = x2, .x3 = x3 };
    seL4_ARM_SMCContext resp = {0};

    seL4_ARM_SMC_Call(seL4_CapSMC, &req, &resp);
    return resp.x0;
}

static void pet_watchdog(void)
{
    smc(SMCWD_FUNC_ID, SMCWD_PET, 0, 0);
}

static void dispatch_core1(void)
{
    uint64_t ret;

    put_str("dispatch: PSCI CPU_ON core1 -> trampoline ");
    put_hex(MK_CORE1_TRAMPOLINE_PADDR);
    put_str("\n");
    ret = smc(PSCI_CPU_ON_AARCH64, CORE1_MPIDR, MK_CORE1_TRAMPOLINE_PADDR, 1);
    put_str("dispatch: PSCI CPU_ON ret=");
    put_dec(ret);
    put_str("\n");
}
#endif

/* ---- capability setup ---- */

static seL4_CPtr next_free_slot;

static seL4_CPtr alloc_slot(const seL4_BootInfo *bi)
{
    if (next_free_slot >= bi->empty.end) {
        halt("out of cslots", seL4_NotEnoughMemory);
    }
    return next_free_slot++;
}

static seL4_CPtr make_notification(const seL4_BootInfo *bi)
{
    seL4_CPtr slot = alloc_slot(bi);

    for (seL4_CPtr ut = bi->untyped.start; ut < bi->untyped.end; ut++) {
        const seL4_UntypedDesc *d = &bi->untypedList[ut - bi->untyped.start];
        if (d->isDevice || d->sizeBits < seL4_NotificationBits) {
            continue;
        }
        if (seL4_Untyped_Retype(ut, seL4_NotificationObject, 0,
                                seL4_CapInitThreadCNode, 0, 0, slot, 1) == seL4_NoError) {
            return slot;
        }
    }
    halt("no untyped for notification", seL4_NotEnoughMemory);
    return 0;
}

static seL4_CPtr bind_ptimer_irq(const seL4_BootInfo *bi, seL4_CPtr ntfn)
{
    seL4_CPtr irq = alloc_slot(bi);
    seL4_Error err;

    err = seL4_IRQControl_Get(seL4_CapIRQControl, PTIMER_IRQ,
                              seL4_CapInitThreadCNode, irq, seL4_WordBits);
    if (err) {
        halt("IRQControl_Get(30)", err);
    }
    err = seL4_IRQHandler_SetNotification(irq, ntfn);
    if (err) {
        halt("IRQHandler_SetNotification", err);
    }
    return irq;
}

int main(void)
{
    const seL4_BootInfo *bi = sel4runtime_bootinfo();
    uint64_t freq, cyc_per_sec, next, prev_cyc, tick = 0;
    seL4_CPtr ntfn, irq;

    put_str("rootserver up, build " ROOTSERVER_BUILD_ID "\n");

#if KERNEL_ID == 0
    pet_watchdog();
    dispatch_core1();
#endif

    next_free_slot = bi->empty.start;
    ntfn = make_notification(bi);
    irq = bind_ptimer_irq(bi, ntfn);

    freq = cntfrq();
    pmu_init();

    /* Calibrate: cycles in one second of pure spinning = 100% busy. */
    {
        uint64_t t0 = cntpct(), c0 = pmccntr();
        while (cntpct() - t0 < freq) {
        }
        cyc_per_sec = pmccntr() - c0;
    }
    put_str("idle: cntfrq=");
    put_dec(freq);
    put_str(" Hz, cpu=");
    put_dec(cyc_per_sec);
    put_str(" cycles/s; blocking on PPI 30\n");

    /* K0 ticks on whole seconds, K1 on half seconds: no shared-UART collisions. */
    next = (cntpct() / freq + 1) * freq + (KERNEL_ID ? freq / 2 : 0);
    ptimer_arm(next);
    seL4_IRQHandler_Ack(irq);
    prev_cyc = pmccntr();

    for (;;) {
        seL4_Word badge;
        uint64_t now_cyc, busy_ppm;

        seL4_Wait(ntfn, &badge);

        /* A future deadline drops the level-triggered line before the ack. */
        next += freq;
        ptimer_arm(next);
        seL4_IRQHandler_Ack(irq);

#if KERNEL_ID == 0
        pet_watchdog();
#endif
        now_cyc = pmccntr();
        busy_ppm = (now_cyc - prev_cyc) * 1000000ull / cyc_per_sec;

        put_str("tick ");
        put_dec(tick++);
        put_str(" busy=");
        put_ppm_as_pct(busy_ppm);
        put_str("\n");

        /* exclude our own printing from the next sample's start point */
        prev_cyc = now_cyc;
    }

    return 0;
}
