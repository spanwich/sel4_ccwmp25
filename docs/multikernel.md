# Multikernel on STM32MP25x (CCMP25-DVK)

Status (2026-10-17): **working on hardware.** Two seL4 16.0.0 kernels, one per
Cortex-A35 core, both idle in WFI between 1 Hz ticks (0.013% busy each),
stable past several 32 s watchdog windows.

## Design

| | K0 (core 0) | K1 (core 1) |
|---|---|---|
| RAM | `0x84000000..0x98000000` | `0x98000000..0xac000000` |
| Kernel config | unicore, EL2, SMC allowed | same + `KernelArmPreparedSecondaryBoot` |
| Loaded by | U-Boot `bootm` → elfloader | K0's elfloader (bundled in its CPIO) |
| Started by | U-Boot | K0 rootserver: PSCI CPU_ON → `mk_core1_trampoline` |
| OP-TEE SMCs | IWDG pet, PSCI | none |
| Tick source | EL1 phys timer, PPI 30 | EL1 phys timer, PPI 30 (banked per core) |

`0xac000000..0xad3fffff` holds the inter-kernel channel (below);
`0xad400000..0xba7fffff` is still free for later shared memory.

Boot sequence:
1. The elfloader loads K0, then `kernel_1.elf`, `rootserver_1` and `kernel_1.dtb`
   at K1's addresses, and leaves core 1 off.
2. K0 boots. Its rootserver issues PSCI CPU_ON to the trampoline, which still lives
   in the elfloader image inside K0's RAM. Nothing has reclaimed it yet, because
   the rootserver dispatches before allocating anything.
3. The trampoline enables the EL2 MMU without set/way or global I-cache
   maintenance (`arm_enable_hyp_mmu_secondary`) and jumps to K1.

Why core 1 is started this way: an elfloader-time CPU_ON leads to a
non-secure access into TF-A BL31's SRAM bank (RISAB1, `0x0E002570`), and OP-TEE
panics. Starting core 1 from the booted K0 avoids this. The history is in
`~/sel4-multikernel/projects/sel4_ccwmp25/docs/risab_tfa_sram_investigation.md`.

## Inter-kernel channel

`channel/mk_channel.{h,c}` is ported from `projects/multikernel_qemu`
(`feature/qemu-aarch64-ikc`, ABI v2, magic "MKCH"). Each kernel owns one
10 MiB SPSC ring (16384 slots, 64 B header + 512 B payload) and maps the
peer's read-only: K0 at `0xac000000`, K1 at `0xaca00000`. Both rootservers
carve these frames out of the device untyped (the window is in neither DTB's
RAM) by halving it down to 2 MiB pages.

Differences from the QEMU original, all forced by the hardware:
- **Device (non-cacheable) mapping.** Unicore kernels map cacheable user
  pages non-shareable, so cached sharing between cores is not coherent. The
  library therefore uses only aligned 64-bit accesses (no `memcpy`/`memset`
  on shared memory), plain owner-only counter updates (no exclusives), and
  LDAR/STLR for head/tail. Header fields are read once (TOCTOU fix).
- **Doorbell is SGI 1, not SGI 8.** TF-A keeps SGIs 8-15 secure (Group 0); a
  non-secure `GICD_SGIR` write cannot raise them, so SGI 8 never arrives.
  The kernel's `seL4_IRQControl_IssueSGISignal` (upstream) sends it.
- **Boot-fresh handshake.** DRAM survives NRST, so a ring can still say READY
  from the previous boot. K0 picks a new epoch and rings the doorbell every
  10 ms; K1 trusts K0's ring only after an SGI (which can only come from this
  boot) and copies K0's epoch into its own ring; K0 attaches only when K1's
  ring shows that epoch. K1 logs a few `Received disabled IRQ: 1` while K0
  rings before K1 has bound SGI 1; this is expected.

Measured on the board (2026-10-17), 10 000 round trips of 512 B after 1 000
warm-up: min 42.8 us, p50 43.8 us, p99 44.9 us, max 45.8 us, 14 424 rt/s, no
full or corrupt slots. One ping per idle tick takes about 30 us. Both cores
still idle at 0.015-0.018% busy.

The ring is a transport only. The contract layer (policy_gate-style message
validation and admission) still sits above it; see the feasibility notes in
the session report.

## Idle

The EL2 kernel uses CNTHP (PPI 26) for its own tick, and PPI 27 is reserved for
VCPUs, which leaves the EL1 physical timer (PPI 30) free on each core. With
`KernelArmExportPCNTUser` and `KernelArmExportPTMRUser`, each rootserver:
- claims IRQ 30 and binds it to a notification;
- sets `CNTP_CVAL_EL0` to the next deadline and calls `seL4_Wait`.

While it waits, the kernel runs its idle thread (`wfi`, `src/arch/arm/64/idle.S`).
The busy figure is the PMU cycle count (which stops in WFI) divided by a
one-second busy-spin calibration (1.2 GHz). The remaining 0.013% is the
kernel's 2 ms scheduler tick.

The timer exports are marked debug-only upstream, because user code can corrupt
another thread's timer registers. That is acceptable while the rootserver is the
only thread. Once real components exist, move to a timer driver component.

## How this diverges from upstream

- **seL4 16.0.0** has an `stm32mp2` platform (`src/plat/stm32mp2`, Christian
  Bruel/ST) and we use it (`PLATFORM=stm32mp2`, `KernelARMPlatform=ccmp25-dvk`).
  Upstream has **no multikernel support**. Its SMP model is one kernel across
  all cores.
- **Our basis: Kent McLeod's multikernel prototype** (github.com/kent-mcleod,
  `kent/multicore3` in both seL4 and seL4_tools; mirrored as
  `spanwich/kent-multicore3`). The kernels stay plain unicore seL4 with no
  multikernel config option; the elfloader loads one kernel per core with
  per-core page tables. Related pieces: `kent/sgi` (the SGI API, now upstream
  as `seL4_IRQControl_IssueSGISignal`), `093bca57` (reserve DTB memory gaps so a
  kernel never hands another kernel's RAM out as device untypeds), `3ccb3d4a`
  (per-core SPI split) and the elfloader GIC distributor init. Our
  `ElfloaderMultikernel` reimplements his loader rather than descending from his
  commits, and starts core 1 from K0 instead of the elfloader (RISAB, above).
- **au-ts multikernel** (`au-ts/seL4` branch `multikernel`, 2025, Microkit
  loader) is a separate, later line. `KernelEnableMultikernelSupport` and
  `KernelMultikernelNumCPUs` are defined there (`config.cmake`), not in any of
  our or Kent's branches. It is a reference only.
- **Our kernel patches**, on `multikernel-stm32mp2-v16`:
  - `KernelArmPreparedSecondaryBoot` turns set/way cache maintenance into
    barriers for the kernel that enters through the trampoline. This is outside
    the verified configuration.
  - A `[K<n>]` per-line print prefix (diagnostic).
- **Our elfloader patches**, on `multikernel-stm32mp2-v16`:
  - `ElfloaderMultikernel`, which bundles and loads secondary kernels.
  - The core-1 trampoline and the secondary hyp-MMU helper.
  - An opt-in K1 trace (`ElfloaderMultikernelTrace`, off by default).

## Known gaps

- The channel trusts the peer for liveness: a stuck K1 hangs K0's
  `mk_channel_recv` (no timeout yet), and the watchdog then resets the board.
- Both kernels write USART2 unlocked. For now collisions are avoided by phase
  offset (K1 ticks at +0.5 s).
- The GIC distributor is shared, and each kernel initialises it at boot. That
  is fine for the per-core PPIs used here. SPIs need an explicit split.
- The trampoline region is not reserved in K0. It is safe only because K0
  dispatches before allocating any memory.
- If K0 dies, nobody pets IWDG1 and the board resets.
- Memory isolation is by convention only: each DTB lists only its own RAM, but
  the other kernel's range is not reserved (Kent's `093bca57` not ported), so a
  rootserver may receive device untypeds covering it. No RIF/RISAF partition
  is configured yet; OP-TEE can set one up on this board.
