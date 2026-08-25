# Path B Phase 1 — Two Independent seL4 Kernels

Status on 2026-08-12: implementation-complete; STM32MP25x hardware acceptance pending.

Phase 1 is the smallest asymmetric-multiprocessing proof for this project: two independent unicore seL4 kernels, one per Cortex-A35 core, boot from one image and remain observably alive. It does not include the distributed capability layer or production cross-kernel communication.

## Definition of done

| Gate | State | Evidence |
|---|---|---|
| v16 source builds for both per-core kernels | PASS | K0 and K1 compile from the local v16 dependency tree |
| ARM-QEMU boots two independent kernels | PASS | Both kernels reach user space and both rootservers continue ticking |
| x86 regression and shared-memory handshake | PASS | K0/K1 identify as slots 0/1 and complete the cafe/babe round trip |
| STM bundle has a valid K1 handoff address | PASS | The build derives the trampoline symbol, recompiles K0, relinks, and requires address convergence |
| STM K1 uses prepared-secondary mode only | PASS | The build checks K1 has the generated config symbol and K0 does not |
| CCMP25-DVK runs K0 and K1 for at least 60 seconds | PENDING | Requires a connected Digi board and UART log |
| K0 keeps the OP-TEE-armed watchdog alive with no RISAB panic | PENDING | Requires the same hardware run |

Phase 1 is not closed until the two pending hardware gates pass. The workstation currently exposes an STM32 ST-LINK for a different board, not a Digi CCMP25-DVK, so no hardware result is claimed.

## 1. Scope

Phase 1 includes:

- one independently built seL4 kernel and rootserver per physical core;
- disjoint physical RAM ownership;
- one elfloader image containing both kernel/rootserver/DTB tuples;
- platform-appropriate secondary-core dispatch;
- direct, prefixed UART output from both kernels;
- watchdog petting by K0 only;
- reproducible QEMU smoke tests.

Phase 1 does not include:

- K0-exclusive UART with a K1 shared-memory logging ring (Phase 1.1);
- IPI capability signalling or production dataports;
- SemperKernel capability enforcement;
- CAmkES or CapDL integration for the multikernel bootstrap;
- more than two kernels on the Phase 1 targets.

## 2. STM32MP25x memory partition

| Region | Range | Size | Owner |
|---|---|---:|---|
| K0 RAM | 0x84000000–0x97ffffff | 320 MiB | Core 0 seL4 |
| K1 RAM | 0x98000000–0xabffffff | 320 MiB | Core 1 seL4 |
| Reserved shared pool | 0xac000000–0xba7fffff | about 232 MiB | Later phases |

Each kernel DTB declares only its own RAM. USART2 at 0x400e0000 is present in both DTBs for the Phase 1 proof. Concurrent bytes may interleave; the K0/K1 prefixes are the diagnostic contract.

## 3. Boot contracts

### 3.1 ARM-QEMU Stage A

1. Build K1 at RAM base 0x70000000 and K0 at 0x60000000.
2. Bundle the stripped K1 kernel, rootserver, and DTB in K0's elfloader archive.
3. The elfloader loads both tuples and calls PSCI CPU_ON for core 1 with multikernel_secondary_startup.
4. The generic trampoline enables the prepared boot page tables, restores the K1 entry arguments, and branches to K1.
5. The boot core enters K0. Both rootservers print ticks.

ARM-QEMU must use the generic trampoline. STM-only late-dispatch and RISAB tracing are guarded by CONFIG_PLAT_STM32MP2.

### 3.2 STM32MP25x Stage B

The STM secure-firmware path needs a later handoff than QEMU:

1. Build K1 first with KernelArmPreparedSecondaryBoot enabled.
2. Build K0 and bundle K1. The elfloader loads and cleans the K1 kernel, rootserver, DTB, entry metadata, and boot page-table state.
3. On STM only, the elfloader leaves core 1 parked and enters K0.
4. The K0 rootserver invokes PSCI CPU_ON through seL4_CapSMC.
5. Core 1 enters mk_core1_trampoline at EL2 with the MMU off, enables the secondary MMU path, restores x0–x5, and branches to K1.
6. K0 alone pets the OP-TEE watchdog through the watchdog SMC. K1 never calls the watchdog service.

The trampoline address is not a source constant. build-stm32mp25x.sh performs a bounded link fixpoint:

1. link a bootstrap image;
2. read mk_core1_trampoline from that exact elfloader with aarch64-linux-gnu-nm;
3. rebuild K0 with MK_CORE1_TRAMPOLINE_PADDR;
4. relink and reject the image unless the linked and compiled addresses match.

For the current v16 build the verified address is 0x880050a0. It is evidence from this build, not an ABI promise.

## 4. Implementation layout

- projects/sel4_ccwmp25/multikernel_hello/ — STM rootserver, DTBs, settings, and two-pass build orchestration.
- projects/multikernel_qemu/ — AArch64 QEMU K0/K1 build and smoke test.
- projects/multikernel_qemu_x86_64/ — x86 K0/K1 build and shared-memory regression test.
- tools/seL4/elfloader-tool/src/arch-arm/multikernel.c — secondary tuple loading and platform dispatch.
- tools/seL4/elfloader-tool/src/arch-arm/64/multikernel_trampoline.S — generic ARM secondary trampoline.
- kernel/src/arch/arm/ — prepared-secondary kernel option and cache-maintenance path.

The native single-kernel and CAmkES gateway paths remain opt-in and separate from this bootstrap.

## 5. Reproducible local validation

Run from the v16 staging source root:

    cd .v16-staging/source
    ./projects/multikernel_qemu/build-mk.sh
    ./projects/multikernel_qemu/test-mk.sh
    ./projects/multikernel_qemu_x86_64/build.sh both
    ./projects/multikernel_qemu_x86_64/test.sh
    ./projects/sel4_ccwmp25/multikernel_hello/build-stm32mp25x.sh

The expected STM artifact is discovered by platform prefix because v16 names it rootserver_hello-image-arm-stm32mp2 while older trees used stm32mp25x. The current artifact is under build-ccwmp25-mk-hello-k0/images/.

## 6. Hardware acceptance procedure

Use a Digi CCMP25-DVK, not another ST-LINK target.

1. Deploy the exact generated uImage with deploy/usb-deploy.sh --image PATH, or the established SD/eMMC install flow.
2. Capture USART2 at 115200 baud from the board's serial device.
3. Keep the board running for at least 60 seconds.
4. Require all of the following evidence:

   - K0 prints ROOTSERVER_BUILD: ccwmp25-mk-phase1-v16-01;
   - K0 prints the derived trampoline address and PSCI return 0;
   - K1 prints the same build marker and reaches tick output;
   - K0 and K1 ticks continue for at least 60 seconds;
   - there is no watchdog reset, OP-TEE RISAB/IAC panic, or EL2 trampoline exception.

Save the complete UART log beside the hardware investigation notes. A compile-only result cannot close this gate.

## 7. Verification boundary

Each instance is configured as a unicore seL4 kernel. KernelArmPreparedSecondaryBoot changes low-level AArch64 cache-maintenance behavior and is not covered by the stock verified configuration. The loader/trampoline owns address-scoped cleaning to the point of coherency before K1 entry.

Phase 1 establishes boot isolation through static RAM partitioning. It does not yet demonstrate runtime authority transfer or seL4-enforced cross-kernel capabilities.

## 8. Next phase

After the hardware gate passes, Phase 1.1 can make K0 the exclusive UART owner and carry K1 logs through a shared device frame. That work must not be folded back into the Phase 1 boot acceptance result.
