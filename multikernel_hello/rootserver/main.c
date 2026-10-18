/*
 * Rootserver for the STM32MP25x multikernel (one unicore seL4 kernel per core).
 *
 * K0 starts core 1 into K1 via PSCI CPU_ON. Each kernel then:
 *   1. calibrates the PMU cycle counter (one second of spinning);
 *   2. maps the inter-kernel channel (mk_channel ABI v2) at MK_SHARED_BASE:
 *      its own 10 MiB ring read-write, the peer's read-only, both Device
 *      (non-cacheable, see channel/mk_channel.c);
 *   3. binds SGI MK_CHANNEL_SGI as the doorbell and waits for the peer ring;
 *   4. runs a ping-pong benchmark (K0 initiates, K1 echoes);
 *   5. idles on the per-core EL1 physical timer (PPI 30), waking once a second.
 *      Each K0 tick also sends one PING that K1 answers from its idle loop.
 * While a rootserver is blocked the kernel runs its idle thread (WFI).
 *
 * Only K0 makes OP-TEE SMCs (watchdog pet, PSCI); K1 makes none.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sel4/sel4.h>
#include <sel4runtime.h>

#include "mk_channel.h"

#ifndef KERNEL_ID
#define KERNEL_ID 0
#endif

#ifndef ROOTSERVER_BUILD_ID
#define ROOTSERVER_BUILD_ID "ccwmp25-mk-p1-v16"
#endif

/* EL1 physical timer: PPI 14 -> GIC IRQ 30. The EL2 kernel uses CNTHP (26). */
#define PTIMER_IRQ 30

/* Shared window, outside both kernels' RAM: K0 ring then K1 ring. */
#define MK_SHARED_BASE    0xac000000ull
#define MK_SHARED_VADDR   0x40000000ull /* local ring; peer ring follows */
#define MK_FRAME_BITS     seL4_LargePageBits
#define MK_FRAMES         (MK_CHANNEL_REGION_SIZE >> MK_FRAME_BITS)
/* Non-secure SGI: TF-A reserves SGIs 8-15 for the secure world (Group 0), and
 * a non-secure GICD_SGIR write cannot raise those. */
#define MK_CHANNEL_SGI    1u
#define MK_CHANNEL_EPOCH  0x202610170001ull
#define MK_READY_TIMEOUT  10u /* seconds */
#define MK_MSG_PING       1u
#define MK_MSG_PONG       2u
#define MK_BENCH_WARMUP   1000u
#define MK_BENCH_SAMPLES  10000u
#define MK_RECV_TIMEOUT_DIV 100u /* round-trip deadline: 1/100 s = 10 ms */
#ifndef MK_PROBE_DROP_PINGS
#define MK_PROBE_DROP_PINGS 3u   /* K1 swallows this many idle pings (P1b probe) */
#endif

/* DDR this kernel must never be able to obtain (kernel DTS "sel4,foreign-memory"). */
#define DDR_BASE        0x80000000ull
#define DDR_END         0xc0000000ull
#define K0_RAM_BASE     0x84000000ull
#define K1_RAM_BASE     0x98000000ull
#define KN_RAM_SIZE     0x14000000ull
#define OWN_RAM_BASE    (KERNEL_ID ? K1_RAM_BASE : K0_RAM_BASE)
#define PEER_RAM_BASE   (KERNEL_ID ? K0_RAM_BASE : K1_RAM_BASE)

/* Notification badges: one notification, two interrupt sources. */
#define BADGE_TIMER 1u
#define BADGE_SGI   2u

_Static_assert(MK_CHANNEL_REGION_SIZE % (1u << MK_FRAME_BITS) == 0,
               "ring must be a whole number of large pages");

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

static void halt(const char *why, uint64_t err)
{
    put_str("FATAL: ");
    put_str(why);
    put_str(" err=");
    put_dec(err);
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
#else
static void pet_watchdog(void)
{
}
#endif

/* ---- capability setup ---- */

static const seL4_BootInfo *bi;
static seL4_CPtr next_free_slot;

static seL4_CPtr alloc_slot(void)
{
    if (next_free_slot >= bi->empty.end) {
        halt("out of cslots", seL4_NotEnoughMemory);
    }
    return next_free_slot++;
}

/* Retype one kernel object from the first RAM untyped that has room. */
static seL4_CPtr alloc_object(seL4_Word type, seL4_Word size_bits, const char *what)
{
    seL4_CPtr slot = alloc_slot();

    for (seL4_CPtr ut = bi->untyped.start; ut < bi->untyped.end; ut++) {
        const seL4_UntypedDesc *d = &bi->untypedList[ut - bi->untyped.start];
        if (d->isDevice || d->sizeBits < size_bits) {
            continue;
        }
        if (seL4_Untyped_Retype(ut, type, 0, seL4_CapInitThreadCNode, 0, 0,
                                slot, 1) == seL4_NoError) {
            return slot;
        }
    }
    halt(what, seL4_NotEnoughMemory);
    return 0;
}

/*
 * P1a check: every untyped must lie in our own RAM, in the channel window, or
 * outside DDR (MMIO). Anything else (peer RAM, secure firmware, the rest of
 * DDR) means the foreign-memory reservation did not take effect.
 */
static void untyped_audit(void)
{
    unsigned n = 0, device = 0, overlap = 0, peer = 0;
    const uint64_t win_lo = MK_SHARED_BASE;
    const uint64_t win_hi = MK_SHARED_BASE + 2ull * MK_CHANNEL_REGION_SIZE;

    for (seL4_CPtr ut = bi->untyped.start; ut < bi->untyped.end; ut++) {
        const seL4_UntypedDesc *d = &bi->untypedList[ut - bi->untyped.start];
        uint64_t lo = d->paddr, hi = d->paddr + (1ull << d->sizeBits);
        n++;
        device += d->isDevice;
        if (hi <= DDR_BASE || lo >= DDR_END) {
            continue;
        }
        if (lo >= OWN_RAM_BASE && hi <= OWN_RAM_BASE + KN_RAM_SIZE) {
            continue;
        }
        if (lo >= win_lo && hi <= win_hi) {
            continue;
        }
        overlap++;
        if (lo < PEER_RAM_BASE + KN_RAM_SIZE && hi > PEER_RAM_BASE) {
            peer++;
        }
        put_str("untyped-audit: FOREIGN ");
        put_hex(lo);
        put_str("..");
        put_hex(hi);
        put_str(d->isDevice ? " device\n" : " ram\n");
    }
    put_str("untyped-audit: untypeds=");
    put_dec(n);
    put_str(" device=");
    put_dec(device);
    put_str(" overlap=");
    put_dec(overlap);
    put_str(" peer-ram=");
    put_dec(peer);
    put_str(overlap ? " FAIL\n" : " PASS\n");
}

/*
 * Turn the device-untyped span [lo, hi) into large-page frames, written to
 * frames[(paddr - lo) >> MK_FRAME_BITS]. Splits the covering untyped in halves
 * down to frame size, so frames land at exact physical addresses without an
 * allocator. Device untypeds are not zeroed by the kernel on retype.
 */
static void carve(seL4_CPtr ut, uint64_t base, unsigned bits, uint64_t lo,
                  uint64_t hi, seL4_CPtr *frames)
{
    seL4_Error err;

    if (base + (1ull << bits) <= lo || base >= hi) {
        return;
    }
    if (bits == MK_FRAME_BITS) {
        seL4_CPtr slot = alloc_slot();
        err = seL4_Untyped_Retype(ut, seL4_ARM_LargePageObject, 0,
                                  seL4_CapInitThreadCNode, 0, 0, slot, 1);
        if (err) {
            halt("retype shared frame", err);
        }
        frames[(base - lo) >> MK_FRAME_BITS] = slot;
        return;
    }

    seL4_CPtr halves = alloc_slot();
    (void)alloc_slot();
    err = seL4_Untyped_Retype(ut, seL4_UntypedObject, bits - 1,
                              seL4_CapInitThreadCNode, 0, 0, halves, 2);
    if (err) {
        halt("split device untyped", err);
    }
    carve(halves, base, bits - 1, lo, hi, frames);
    carve(halves + 1, base + (1ull << (bits - 1)), bits - 1, lo, hi, frames);
}

static void carve_window(uint64_t lo, uint64_t hi, seL4_CPtr *frames)
{
    for (seL4_CPtr ut = bi->untyped.start; ut < bi->untyped.end; ut++) {
        const seL4_UntypedDesc *d = &bi->untypedList[ut - bi->untyped.start];
        uint64_t end = d->paddr + (1ull << d->sizeBits);
        if (d->isDevice && d->paddr <= lo && hi <= end) {
            carve(ut, d->paddr, d->sizeBits, lo, hi, frames);
            return;
        }
    }
    put_str("no single device untyped covers ");
    put_hex(lo);
    put_str("\n");
    halt("carve shared window", seL4_FailedLookup);
}

/* Map a frame, creating whatever page tables are missing on the way. */
static void map_frame(seL4_CPtr frame, seL4_Word vaddr, seL4_CapRights_t rights)
{
    /* Not cacheable: unicore kernels map cacheable pages non-shareable. */
    seL4_ARM_VMAttributes attr = seL4_ARM_ExecuteNever;

    for (int level = 0; level < 4; level++) {
        seL4_Error err = seL4_ARM_Page_Map(frame, seL4_CapInitThreadVSpace,
                                           vaddr, rights, attr);
        if (err == seL4_NoError) {
            return;
        }
        if (err != seL4_FailedLookup) {
            halt("Page_Map shared frame", err);
        }
        seL4_CPtr pt = alloc_object(seL4_ARM_PageTableObject,
                                    seL4_PageTableBits, "page table");
        err = seL4_ARM_PageTable_Map(pt, seL4_CapInitThreadVSpace, vaddr,
                                     seL4_ARM_Default_VMAttributes);
        if (err) {
            halt("PageTable_Map", err);
        }
    }
    halt("Page_Map: too many levels", seL4_FailedLookup);
}

static seL4_CPtr badged(seL4_CPtr ntfn, seL4_Word badge)
{
    seL4_CPtr slot = alloc_slot();
    seL4_Error err = seL4_CNode_Mint(seL4_CapInitThreadCNode, slot, seL4_WordBits,
                                     seL4_CapInitThreadCNode, ntfn, seL4_WordBits,
                                     seL4_AllRights, badge);
    if (err) {
        halt("CNode_Mint", err);
    }
    return slot;
}

static seL4_CPtr bind_irq(seL4_Word irq_num, seL4_CPtr ntfn)
{
    seL4_CPtr irq = alloc_slot();
    seL4_Error err;

    err = seL4_IRQControl_Get(seL4_CapIRQControl, irq_num,
                              seL4_CapInitThreadCNode, irq, seL4_WordBits);
    if (err) {
        halt("IRQControl_Get", err);
    }
    err = seL4_IRQHandler_SetNotification(irq, ntfn);
    if (err) {
        halt("IRQHandler_SetNotification", err);
    }
    return irq;
}

/* ---- channel runtime ---- */

static seL4_CPtr ntfn, timer_irq, sgi_irq, sgi_signal;
static mk_channel_t channel;
static mk_channel_region_t *local_ring;
static const mk_channel_region_t *peer_ring;
static int timer_pending;
static uint64_t tick_next; /* next idle-tick deadline; 0 before the idle loop */

static void channel_notify(void *cookie)
{
    (void)cookie;
    seL4_Signal(sgi_signal);
}

/* Block until something arrives. A timer tick seen here is remembered for the
 * idle loop. Doorbells are hints: the caller always re-checks the ring. */
static int channel_wait(void *cookie)
{
    seL4_Word badge = 0;

    (void)cookie;
    seL4_Wait(ntfn, &badge);
    if (badge & BADGE_TIMER) {
        timer_pending = 1;
    }
    if (badge & BADGE_SGI) {
        return seL4_IRQHandler_Ack(sgi_irq);
    }
    return 0;
}

static uint64_t channel_now(void *cookie)
{
    (void)cookie;
    return cntpct();
}

/*
 * Bounded wait for mk_channel_recv_until. The EL1 physical timer (PPI 30) is
 * shared with the idle tick, so it is armed at whichever comes first. A tick
 * that falls due here is recorded in timer_pending for the idle loop.
 */
static int channel_wait_until(void *cookie, uint64_t deadline)
{
    seL4_Word badge = 0;

    (void)cookie;
    ptimer_arm(tick_next && tick_next < deadline ? tick_next : deadline);
    seL4_Wait(ntfn, &badge);
    if (badge & BADGE_TIMER) {
        if (tick_next && cntpct() >= tick_next) {
            timer_pending = 1;
        }
        /* A future deadline drops the level-triggered line before the ack. */
        ptimer_arm(tick_next && !timer_pending ? tick_next : UINT64_MAX);
        seL4_IRQHandler_Ack(timer_irq);
    }
    if (badge & BADGE_SGI) {
        return seL4_IRQHandler_Ack(sgi_irq);
    }
    return 0;
}

static void setup_channel(uint64_t freq)
{
    seL4_CPtr frames[2 * MK_FRAMES];
    seL4_Error err;
    const uint64_t lo = MK_SHARED_BASE;
    const uint64_t hi = MK_SHARED_BASE + 2ull * MK_CHANNEL_REGION_SIZE;
    const unsigned local_first = KERNEL_ID * MK_FRAMES;
    const unsigned peer_first = (1 - KERNEL_ID) * MK_FRAMES;

    carve_window(lo, hi, frames);
    for (unsigned i = 0; i < MK_FRAMES; i++) {
        seL4_Word off = (seL4_Word)i << MK_FRAME_BITS;
        map_frame(frames[local_first + i], MK_SHARED_VADDR + off, seL4_ReadWrite);
        map_frame(frames[peer_first + i],
                  MK_SHARED_VADDR + MK_CHANNEL_REGION_SIZE + off, seL4_CanRead);
    }
    local_ring = (mk_channel_region_t *)(uintptr_t)MK_SHARED_VADDR;
    peer_ring = (const mk_channel_region_t *)(uintptr_t)
                (MK_SHARED_VADDR + MK_CHANNEL_REGION_SIZE);

    put_str("channel: local ");
    put_hex(lo + (uint64_t)local_first * (1u << MK_FRAME_BITS));
    put_str(" rw, peer ");
    put_hex(lo + (uint64_t)peer_first * (1u << MK_FRAME_BITS));
    put_str(" ro, Device, SGI ");
    put_dec(MK_CHANNEL_SGI);
    put_str("\n");

    sgi_irq = bind_irq(MK_CHANNEL_SGI, badged(ntfn, BADGE_SGI));
    err = seL4_IRQHandler_Ack(sgi_irq);
    if (err) {
        halt("Ack SGI", err);
    }
    sgi_signal = alloc_slot();
    err = seL4_IRQControl_IssueSGISignal(seL4_CapIRQControl, MK_CHANNEL_SGI,
                                         1 - KERNEL_ID, seL4_CapInitThreadCNode,
                                         sgi_signal, seL4_WordBits);
    if (err) {
        halt("IssueSGISignal", err);
    }

    /*
     * Boot-fresh handshake. Shared DRAM survives NRST, so a ring may still say
     * READY from the previous boot. K0 picks a new epoch and rings the doorbell
     * until K1's ring carries that epoch. K1 trusts K0's ring only after an SGI,
     * which can only come from this boot, and then adopts K0's epoch.
     */
    uint64_t epoch = 0;
    uint64_t deadline = cntpct() + freq * MK_READY_TIMEOUT;
#if KERNEL_ID == 0
    epoch = (MK_CHANNEL_EPOCH << 32) ^ cntpct();
    mk_channel_region_prepare(local_ring, KERNEL_ID, epoch);
    mk_channel_region_mark_ready(local_ring);
    while (!mk_channel_region_is_ready(peer_ring, 1) ||
           ((const volatile mk_channel_control_t *)&peer_ring->control)->epoch != epoch) {
        if (cntpct() >= deadline) {
            halt("K1 ring not ready (timeout)", MK_READY_TIMEOUT);
        }
        seL4_Signal(sgi_signal);
        for (uint64_t t = cntpct(); cntpct() - t < freq / 100;) {
        }
    }
#else
    mk_channel_region_prepare(local_ring, KERNEL_ID, 0);
    for (;;) {
        seL4_Word badge = 0;
        seL4_Wait(ntfn, &badge);
        seL4_IRQHandler_Ack(sgi_irq);
        if (mk_channel_region_is_ready(peer_ring, 0)) {
            break;
        }
    }
    epoch = ((const volatile mk_channel_control_t *)&peer_ring->control)->epoch;
    ((volatile mk_channel_control_t *)&local_ring->control)->epoch = epoch;
    mk_channel_region_mark_ready(local_ring);
#endif
    (void)deadline;
    mk_channel_status_t st = mk_channel_attach(&channel, KERNEL_ID, local_ring,
                                               peer_ring, channel_notify,
                                               channel_wait, NULL);
    if (st != MK_CHANNEL_OK) {
        halt("mk_channel_attach", st);
    }
    put_str("channel: READY abi=");
    put_dec(MK_CHANNEL_ABI_VERSION);
    put_str(" epoch=");
    put_hex(epoch);
    put_str("\n");
}

static void make_payload(uint8_t payload[MK_CHANNEL_PAYLOAD_SIZE], uint64_t seq)
{
    memcpy(payload, &seq, sizeof(seq));
    for (size_t i = sizeof(seq); i < MK_CHANNEL_PAYLOAD_SIZE; i++) {
        payload[i] = (uint8_t)((seq * 131u + i) & 0xffu);
    }
}

static int payload_valid(const mk_channel_message_t *m, uint16_t type, uint64_t seq)
{
    uint8_t expected[MK_CHANNEL_PAYLOAD_SIZE];

    make_payload(expected, seq);
    return m->type == type && m->length == MK_CHANNEL_PAYLOAD_SIZE &&
           memcmp(m->payload, expected, sizeof(expected)) == 0;
}

/* K1: echo one PING as a PONG. */
static void echo(const mk_channel_message_t *m)
{
    if (m->type != MK_MSG_PING) {
        halt("echo: unexpected type", m->type);
    }
    mk_channel_status_t st = mk_channel_try_send(&channel, MK_MSG_PONG, 0,
                                                 m->payload, m->length, NULL);
    if (st != MK_CHANNEL_OK) {
        halt("echo: send", st);
    }
}

static uint64_t freq_hz;
static uint64_t stale_pongs;

/*
 * K0: one validated round trip with a 10 ms deadline. Returns its duration in
 * counter ticks, or 0 on timeout. PONGs that arrive after their own deadline
 * are recognised by sequence number and dropped.
 */
static uint64_t round_trip(uint64_t seq)
{
    uint8_t payload[MK_CHANNEL_PAYLOAD_SIZE];
    mk_channel_message_t m;

    make_payload(payload, seq);
    uint64_t t0 = cntpct();
    uint64_t deadline = t0 + freq_hz / MK_RECV_TIMEOUT_DIV;
    mk_channel_status_t st = mk_channel_try_send(&channel, MK_MSG_PING, 0,
                                                 payload, sizeof(payload), NULL);
    if (st != MK_CHANNEL_OK) {
        halt("ping: send", st);
    }
    for (;;) {
        st = mk_channel_recv_until(&channel, &m, deadline, channel_now,
                                   channel_wait_until);
        if (st == MK_CHANNEL_TIMEOUT) {
            return 0;
        }
        if (st != MK_CHANNEL_OK) {
            halt("ping: recv", st);
        }
        uint64_t got;
        memcpy(&got, m.payload, sizeof(got));
        if (m.type == MK_MSG_PONG && got < seq) {
            stale_pongs++;
            continue;
        }
        break;
    }
    uint64_t t1 = cntpct();
    if (!payload_valid(&m, MK_MSG_PONG, seq)) {
        halt("ping: bad PONG", seq);
    }
    return t1 - t0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static uint64_t ticks_to_ns(uint64_t ticks, uint64_t freq)
{
    return ticks * 1000000000ull / freq;
}

static void benchmark(uint64_t freq)
{
    const uint64_t total = (uint64_t)MK_BENCH_WARMUP + MK_BENCH_SAMPLES;

#if KERNEL_ID == 0
    static uint64_t samples[MK_BENCH_SAMPLES];
    uint64_t start = 0, sum = 0;

    for (uint64_t seq = 0; seq < total; seq++) {
        if (seq == MK_BENCH_WARMUP) {
            start = cntpct();
        }
        uint64_t dt = round_trip(seq);
        if (dt == 0) {
            halt("bench: round trip timed out", seq);
        }
        if (seq == 0) {
            put_str("bench: first PONG received\n");
        }
        if (seq >= MK_BENCH_WARMUP) {
            samples[seq - MK_BENCH_WARMUP] = dt;
            sum += dt;
        }
    }
    uint64_t elapsed = cntpct() - start;

    qsort(samples, MK_BENCH_SAMPLES, sizeof(samples[0]), cmp_u64);
    uint64_t min = samples[0], max = samples[MK_BENCH_SAMPLES - 1];
    uint64_t p50 = samples[MK_BENCH_SAMPLES / 2];
    uint64_t p99 = samples[MK_BENCH_SAMPLES * 99 / 100];

    put_str("bench: ");
    put_dec(MK_BENCH_SAMPLES);
    put_str(" round trips x ");
    put_dec(MK_CHANNEL_PAYLOAD_SIZE);
    put_str(" B: min=");
    put_dec(ticks_to_ns(min, freq));
    put_str("ns p50=");
    put_dec(ticks_to_ns(p50, freq));
    put_str("ns p99=");
    put_dec(ticks_to_ns(p99, freq));
    put_str("ns max=");
    put_dec(ticks_to_ns(max, freq));
    put_str("ns mean=");
    put_dec(ticks_to_ns(sum, freq) / MK_BENCH_SAMPLES);
    put_str("ns rate=");
    put_dec(elapsed ? MK_BENCH_SAMPLES * freq / elapsed : 0);
    put_str(" rt/s\n");
#else
    mk_channel_message_t m;
    for (uint64_t seq = 0; seq < total; seq++) {
        mk_channel_status_t st = mk_channel_recv(&channel, &m);
        if (st != MK_CHANNEL_OK) {
            halt("bench: recv", st);
        }
        if (!payload_valid(&m, MK_MSG_PING, seq)) {
            halt("bench: bad PING", seq);
        }
        if (seq == 0) {
            put_str("bench: first PING received\n");
        }
        echo(&m);
    }
    put_str("bench: echoed ");
    put_dec(total);
    put_str(" PINGs\n");
#endif
    const volatile mk_channel_control_t *lc = &local_ring->control;
    put_str("bench: sent=");
    put_dec(lc->sent);
    put_str(" received=");
    put_dec(lc->received);
    put_str(" full=");
    put_dec(lc->full);
    put_str(" corrupt=");
    put_dec(lc->corrupt);
    put_str("\n");
}

int main(void)
{
    uint64_t freq, cyc_per_sec, next, prev_cyc, tick = 0;
    uint64_t ping_seq = 1000000; /* idle-loop pings, distinct from the bench */
    uint64_t timeouts = 0;
    (void)timeouts;
#if KERNEL_ID == 1
    unsigned dropped = 0;
#endif

    bi = sel4runtime_bootinfo();
    put_str("rootserver up, build " ROOTSERVER_BUILD_ID "\n");
    untyped_audit();

#if KERNEL_ID == 0
    pet_watchdog();
    dispatch_core1();
#endif

    freq = cntfrq();
    freq_hz = freq;
    pmu_init();

    /* Calibrate before allocating anything: on K0 this also gives core 1 a
     * full second to leave the trampoline, which lives in K0's untyped RAM. */
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
    put_str(" cycles/s\n");

    next_free_slot = bi->empty.start;
    ntfn = alloc_object(seL4_NotificationObject, seL4_NotificationBits,
                        "notification");

    /* The timer also bounds channel receives, so bind it before the channel. */
    timer_irq = bind_irq(PTIMER_IRQ, badged(ntfn, BADGE_TIMER));
    ptimer_arm(UINT64_MAX);
    seL4_IRQHandler_Ack(timer_irq);

    pet_watchdog();
    setup_channel(freq);
    pet_watchdog();
    benchmark(freq);
    pet_watchdog();

    put_str("idle: blocking on PPI 30 + SGI ");
    put_dec(MK_CHANNEL_SGI);
    put_str("\n");

    /* K0 ticks on whole seconds, K1 on half seconds: no shared-UART collisions. */
    next = (cntpct() / freq + 1) * freq + (KERNEL_ID ? freq / 2 : 0);
    tick_next = next;
    ptimer_arm(next);
    seL4_IRQHandler_Ack(timer_irq);
    prev_cyc = pmccntr();

    for (;;) {
        seL4_Word badge = 0;
        uint64_t now_cyc, busy_ppm, rtt = 0;

        if (!timer_pending) {
            seL4_Wait(ntfn, &badge);
        }

        if (badge & BADGE_SGI) {
            seL4_IRQHandler_Ack(sgi_irq);
#if KERNEL_ID == 1
            mk_channel_message_t m;
            while (mk_channel_try_recv(&channel, &m) == MK_CHANNEL_OK) {
                if (dropped < MK_PROBE_DROP_PINGS) {
                    dropped++; /* P1b probe: K0 must time out, not hang */
                    continue;
                }
                echo(&m);
            }
#endif
        }
        if (!(badge & BADGE_TIMER) && !timer_pending) {
            continue;
        }
        timer_pending = 0;

        /* A future deadline drops the level-triggered line before the ack. */
        next += freq;
        tick_next = next;
        ptimer_arm(next);
        seL4_IRQHandler_Ack(timer_irq);

#if KERNEL_ID == 0
        pet_watchdog();
        rtt = round_trip(ping_seq++);
        timeouts += (rtt == 0);
#endif
        now_cyc = pmccntr();
        busy_ppm = (now_cyc - prev_cyc) * 1000000ull / cyc_per_sec;

        put_str("tick ");
        put_dec(tick++);
        put_str(" busy=");
        put_ppm_as_pct(busy_ppm);
#if KERNEL_ID == 0
        put_str(" ping=");
        put_dec(ticks_to_ns(rtt, freq));
        put_str("ns timeouts=");
        put_dec(timeouts);
        put_str(" stale=");
        put_dec(stale_pongs);
#else
        put_str(" pongs=");
        put_dec(((const volatile mk_channel_control_t *)&local_ring->control)->sent);
        put_str(" dropped=");
        put_dec(dropped);
#endif
        put_str("\n");

        /* exclude our own printing from the next sample's start point */
        prev_cyc = now_cyc;
    }

    return 0;
}
