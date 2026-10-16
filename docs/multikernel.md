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

`0xac000000..0xba7fffff` is reserved for future inter-kernel shared memory.

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
- **au-ts multikernel** (https://github.com/au-ts/multikernel-manifest,
  `multikernel` branches of seL4 and Microkit) is the closest upstream effort.
  Like us, it uses a per-kernel RAM region and a fixed kernel virtual address.
  It differs from our work in these ways:
  - Its loader is the Microkit loader. Ours is the seL4_tools elfloader.
  - Its loader releases the secondaries. We start core 1 from K0's rootserver,
    for the RISAB reason above.
  - It has `KernelEnableMultikernelSupport` with cross-core SGI notifications.
    We have no inter-kernel IPC yet.
  - It supports QEMU, OdroidC4 and MaaXboard, not STM32MP2.
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

- Both kernels write USART2 unlocked. For now collisions are avoided by phase
  offset (K1 ticks at +0.5 s).
- The GIC distributor is shared, and each kernel initialises it at boot. That
  is fine for the per-core PPIs used here. SPIs need an explicit split.
- The trampoline region is not reserved in K0. It is safe only because K0
  dispatches before allocating any memory.
- If K0 dies, nobody pets IWDG1 and the board resets.
