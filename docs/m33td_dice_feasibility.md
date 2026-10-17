# M33-first boot (M33-TD) with DICE on the CCMP25-DVK: feasibility review

Date: 2026-10-18. Scope: paper review only. Nothing was built or flashed. S1, OTP
and the eMMC were not touched.

Goal: the Cortex-M33 boots first, performs DICE measured boot, then releases the
A35s (TF-A, OP-TEE, U-Boot, then the seL4 multikernel). This closes the gap
where nothing measures the seL4 boot chain today.

Tags: **[C]** confirmed (manual page, source line or URL); **[I]** inferred.

## Verdict

| Area | Verdict |
|---|---|
| 1. Boot mode and recovery | **Feasible, but needs hands on the board.** No NOR is fitted, so M33-TD must use the same eMMC. |
| 2. Firmware ownership in M33-TD | **Feasible, large porting job.** DDR init moves to the M33 and must be ported for the Digi DDR4. |
| 3. RIF impact on seL4 | **Feasible.** It does not add K0/K1 isolation, but it does centralise DMA isolation. |
| 4. DICE chain | **Feasible on paper, no existing implementation.** ROM gives no CDI; MCUboot must be layer 0. |
| 5. Software availability | **Feasible with caveats.** ST supports it on STM32MP257F-EV1/MP215F-DK only; not on CCMP25 or STM32MP255. |
| 6. OTP / secure boot | **Plan only.** Measured boot needs no fuses. Secure boot needs irreversible key-hash fusing. |

Overall: **feasible, with caveats.** No hardware blocker was found. The cost is a
board port of ST's M33-TD stack (MCUboot, TF-M, TF-A `STM32MP_M33_TDCID`,
OP-TEE `CFG_STM32_CM33TDCID`) to the CCMP25. Every experiment needs someone
physically at the board.

## 1. Boot mode and recovery

- **[C] The SOM supports M33-master boot from SD, eMMC or serial NOR.** The
  BOOT[3:0] table gives: `0111` SD, `1000` eMMC, `1011` NOR. If the boot device
  is invalid, the ROM falls back to the USB downloader (90002593, Bootstrap
  section).
- **[C] DVK S1 settings** (90002594, "System boot"):

  | Mode | S1.1–S1.4 |
  |---|---|
  | A35 eMMC (**today**) | Open, Open, Open, Open |
  | M33 eMMC | Open, Close, Open, Close |
  | M33 S-NOR | Close, Open, Open, Close |
  | UART/USB | Open, Close, Open, Open |

- **[C] No NOR flash is fitted** on either the SOM or the DVK. The only storage is
  the eMMC (up to 8 GB); the Octo-SPI pins are routed but unpopulated. So the
  low-risk option of "M33 from NOR, A35 eMMC left untouched" does not exist.
- **[C] SD boot is unavailable on wireless SOM variants.** Ours is one: SDMMC1
  goes to the Wi-Fi chip.
- **[I] M33-TD from eMMC means rewriting the eMMC boot partitions** that hold
  today's A35 FSBL. One eMMC cannot hold both an A35-TD and an M33-TD FSBL set
  and let S1 choose between them, unless ST's layout keeps them in separate
  partitions. This needs checking in ST's cm33tdcid eMMC flash layout.
- **Every M33-TD experiment needs the user at the board**, because changing S1
  is physical. The remote NRST reset only re-runs whatever is flashed.
- **Recovery path:** set S1 to UART/USB, then use STM32CubeProgrammer over USB-C
  (J25) with the `FlashLayout_emmc_*.tsv` A35-TD layout to restore today's
  eMMC. The ST wiki does flashing with all boot pins at 0 ([ST/Voxel Botics
  eMMC guide](https://voxelbotics.atlassian.net/wiki/spaces/DOC/pages/561709096/STM32MP2+EV1+Booting+Linux+from+eMMC)).
  **Prerequisite before any experiment:** do one dry run of this restore *in
  the current A35-TD mode*, so recovery is proven before it is needed. It also
  makes a backup of the eMMC boot partitions. Today's `flash_ums.sh` only
  touches partition 5 and is not a recovery path.

## 2. Who owns what in M33-TD

- **[C] TF-A built with `STM32MP_M33_TDCID=1` skips** (`plat/st/stm32mp2/bl2_plat_setup.c`):
  - DDR probe and init (:148, :350);
  - RISAB3 (SRAM1) and RISAB5 (RETRAM) setup (:363, :466);
  - backup-domain reset (:205);
  - DDR encryption key preparation (:498);
  - ICN QoS (:168).

  It reads OTP through the `stm32_otp_*` service (`stm32mp2_private.h:105-113`)
  instead of BSEC3 directly. In clocks, oscillator enable is left to the M33,
  and the M33-TD critical clocks are flagged (`drivers/st/clk/clk-stm32mp2.c:68-72, 734`).
- **[I] The M33 FSBL (MCUboot) must therefore initialise DDR4** with the Digi
  CCMP25 timings. Today they are in TF-A's `ccmp25-dvk.dts` and its DDR
  settings. That port has to land in ST's MCUboot/TF-M board support, and it is
  the largest single item.
- **[C] The A35 secure world still exists.** ST's flow wakes the A35 by SEV; the
  ROM on the A35 then loads TF-A BL2, which loads BL31, OP-TEE and U-Boot. So
  PSCI (`CPU_ON`, which K0 uses to start K1) and the OP-TEE SMC interface stay
  available to seL4.
- **[I] The watchdog may change owner.** OP-TEE starts IWDG1 today, and our
  rootserver pets it by SMC. In M33-TD, TF-M may own IWDG1 or IWDG3, so the
  K0 pet path may need a different SMC or a mailbox. This is open.
- **[C] OP-TEE has `CFG_STM32_CM33TDCID`** (`optee_os/core/arch/arm/plat-stm32mp2/conf.mk:89-108`).
  It disables SCMI, the BSEC PTA, BSEC3, provisioning and the NVMEM TA. Any of
  our SMC uses that depend on these must be re-checked
  (`docs/optee_smc_interfaces.md`).

## 3. RIF impact on the seL4 design

- **[C] Only the TDCID configures RIFSC and RISAF.** In M33-TD that is the M33
  (ST M33-TD peripherals overview). The A35 can no longer change firewalls.
  Today OP-TEE does it.
- **K0/K1 isolation: no change.** Both A35 cores still appear as CID1 on the bus,
  so RIF cannot tell the two kernels apart in either flavor (see the RIF
  discussion behind `docs/multikernel.md`, "Known gaps").
- **DMA isolation: better.** The RIMC master CIDs and the RISAF4 DDR split
  proposed earlier (K0, K1, mk_channel windows readable by CID1 only;
  per-kernel DMA windows also open to the DMA masters' CIDs) would be set once by
  the M33 before any A35 code runs. The A35, including a compromised OP-TEE,
  could not undo them. **[I]** This is a real improvement over A35-TD, where
  OP-TEE on the A35 is both the TDCID and inside the attack surface.
- **[I] The RISAB1 / TF-A SRAM issue** that forced K0 to start core 1 is a
  firewall configuration matter. In M33-TD it would be configured by the M33,
  so it might or might not recur. Keep K0-starts-K1 regardless; it works in
  both flavors.
- The new configuration must still grant the A35 non-secure world: DDR
  `0x84000000..0xba7fffff`, the GIC, USART2, ETH1/ETH2 and their clocks.
  Otherwise seL4 gets silent bus errors, as already seen with RCC
  (`docs/rcc_clock_investigation.md`).

## 4. DICE chain (paper design)

- **[C] The BootROM authenticates FSBL-M and FSBL-A** against OEM-KEY1/2 hashes
  in OTP words 144–159. **It does not measure anything or hand off a CDI** (no
  ST source says otherwise; treat this as an absence of evidence). So DICE
  layer 0 must be MCUboot.
- **UDS (the device secret) — [I] proposal:** in MCUboot, derive it from the
  SAES DHUK with a fixed label, in secure and privileged mode. Then make the key
  unreachable before jumping on: reconfigure SAES or lock the key selection.
  - [C] OP-TEE already has DHUK derivation
    (`optee_os/core/drivers/crypto/stm32/stm32_saes.c:1258`). It is the reference
    for the SAES sequence.
  - [C] DHUK is unique per device on STM32MP25xC/F, and our part is
    STM32MP255**C**.
- **Layers:**

  | Layer | Measures / derives |
  |---|---|
  | L0 MCUboot (M33) | UDS → CDI_L0; measures TF-M and the A35 FSBL (TF-A BL2) |
  | L1 TF-M (M33) | hosts DPE; holds all CDIs in M33 secure memory |
  | L2 TF-A BL2 (A35) | measures BL31, OP-TEE and U-Boot. [C] TF-A has `MEASURED_BOOT=1 DICE_PROTECTION_ENVIRONMENT=1`, built for Arm RSE ([TF-A RSE/DPE doc](https://trustedfirmware-a.readthedocs.io/en/latest/design_documents/rse.html)). |
  | L3 U-Boot or elfloader | measures K0 kernel + rootserver, K1 kernel + rootserver, both DTBs |
  | L4 seL4 rootservers | use a DPE-derived key or certificate for attestation over mk_channel |

- **[I] A35 → DPE transport:** the IPCC mailbox to the M33. It needs a new TF-A
  DPE transport for STM32MP2, because upstream only has the RSE mailbox. RIF
  must restrict that IPCC channel to A35 secure software.
- **[C] No DPE or measured-boot implementation exists for STM32MP25** in ST's
  TF-M v2.1.3, upstream TF-M or MCUboot. Everything above is new porting work.
- **seL4 side:** the elfloader is the natural measurement point; it already
  loads every image for both kernels (`ElfloaderMultikernel`).
  - Kernels and rootservers are fixed images, so their measurements are stable.
  - The elfloader runs at non-secure EL2, so it needs either an SMC to OP-TEE or
    BL31 that forwards to the DPE, or a direct IPCC path.

## 5. Software availability

- **[C] OpenSTLinux 6.1 added the `stm32mp2-m33td` machine** for STM32MP257F-EV1
  and STM32MP215F-DK. It supports M33-TD storage combinations including
  **eMMC-only** (M33 boots from eMMC, A35 runs from eMMC). It also adds M33
  protected storage on NOR and independent reboot of the A35 cores
  ([OpenSTLinux 6.1 release note](https://wiki.st.com/stm32mpu/wiki/STM32_MPU_OpenSTLinux_release_note_-_v6.1.0),
  [ST community](https://community.st.com/stm32-software-tools-165/new-openstlinux-6-1-release-154076)).
- **[C] The prebuilt M33-TD image package sat in ST's MCD Alpha space**
  (account required), and older forum posts call M33-TD alpha-only
  ([ST community thread](https://community.st.com/t5/stm32-mpus-embedded-software-and/boot-stm32mp257f-ev1-from-cortex-m33/td-p/814331)).
  It is public in 6.1, but the image package may still be gated. Check this.
- **[C] Locally we have none of it:**
  - Our TF-A is `v2.10/stm32mp/maint`, OP-TEE 4.0 (Digi `dey-5.0`) and U-Boot
    2024.04.
  - There is no TF-M, no MCUboot, and no CCMP25 cm33tdcid device tree.
  - Digi publishes no CCMP25 M33-TD variant.
- **[I] What porting to the CCMP25 needs:**
  - DDR4 timings and PMIC (STPMIC25) setup moved into MCUboot;
  - board device trees for TF-M, TF-A and OP-TEE in cm33tdcid form;
  - an eMMC flash layout;
  - a check that the STM32MP255 (not the 257 that ST targets) is supported by
    the M33-TD firmware.
  - It also likely means moving TF-A and OP-TEE to the OpenSTLinux 6.1
    baselines, a jump from our Digi 5.0 versions.
- **[C] Digi DEY 5.0 documents only the coprocessor mode, A35-TD (reviewed 2026-10-18).**
  - [Develop Cortex-M applications](https://docs.digi.com/resources/documentation/digidocs/embedded/dey/5.0/ccmp25/yocto-develop-cortex-apps_t_stm.html)
    starts the M33 *from the A35*: Linux remoteproc (`/lib/firmware`,
    `/sys/class/remoteproc/remoteproc0`), U-Boot `rproc`, or ST-LINK.
  - [Boot chain](https://docs.digi.com/resources/documentation/digidocs/embedded/dey/5.0/ccmp25/yocto-boot-chain_t_stm.html)
    lists only ROM → TF-A (FSBL) → OP-TEE → U-Boot on the A35. There are no
    M33 boot modes, no TF-M and no MCUboot.
  - [Secure boot set-up](https://docs.digi.com/resources/documentation/digidocs/embedded/dey/5.0/ccmp25/yocto-trustfence-secure-boot-set-up_t_stm.html)
    has OP-TEE authenticate and decrypt M33 firmware before remoteproc starts
    it. The keys are RPROC-FW-PKH (OTP 176..183) and RPROC-FW-ENC-KEY
    (OTP 336..343), both write-once.
  - So Digi supports "M33 boots" in the sense that the A35 boots the M33. The
    M33-master BOOT[3:0] modes appear only in the SOM hardware manual
    (90002593); no DEY software uses them.
- **[C] Source check (2026-10-18): who can start the M33 in A35-TD.** Trees
  under `~/phd/ccwmp255`.
  - **TF-A** (`v2.10/stm32mp/maint`, e22f7d898):
    - It never loads or starts the M33. Its BL2 image list
      (`plat/st/stm32mp2/plat_bl2_mem_params_desc.c`) has DDR_FW, FW_CONFIG,
      BL31, SOC_FW_CONFIG, BL32, BL32_EXTRA1, HW_CONFIG and BL33. None of
      these is an M33 image.
    - `BOOT_API_IMAGE_TYPE_M33` (`include/boot_api.h:173`) is defined but
      never used.
    - The only CPU2 control is the one that stops it. On PSCI system-off it
      clears `RCC_CPUBOOTCR.BOOT_CPU2` and asserts C2RST
      (`stm32mp2_pm.c:907-912`).
    - `stm32_pm_init` sets only `BOOT_CPU1` plus legacy boot, which lets
      only the A35 boot (`stm32mp2_pm.c:1327-1330`).
    - BL2 always hands off to BL31 → BL32 → BL33 on the A35. Its only
      M33-aware path is `STM32MP_M33_TDCID`, where the M33 already runs and
      BL2 skips its setup.
  - **U-Boot**:
    - The local tree is Digi's i.MX branch (`v2024.04/maint`, `dub-2024.04-r3.1`).
      It has no CCMP25 board, so the board's own U-Boot source is not on
      this machine.
    - Its `drivers/remoteproc/stm32_copro.c` matches only `st,stm32mp1-m4`.
      `board/st/stm32mp2` has no rproc code.
    - Digi documents `rproc` in the CCMP25 U-Boot, so that support lives
      only in Digi's STM32 branch, which is not here.
  - **OP-TEE** (`stm32_remoteproc.c`) owns the M33 start in A35-TD.
    - It matches `st,stm32mp2-m33` (`ns_loading`: U-Boot/Linux load the
      image, and hold-boot/reset are exported as SCMI reset domains) and
      `st,stm32mp2-m33-tee` (OP-TEE's remoteproc TA authenticates and loads
      the image).
    - `stm32mp2_rproc_start` (line 204) writes the boot address to
      `A35SSC_M33_INITSVTOR_CR` and, when TrustZone is enabled
      (`stm32_rproc_enable_sec_boot`), sets `A35SSC_M33_TZEN_CR`. It then
      releases hold-boot.
    - The CCMP25 DT uses the `ns_loading` variant (`ccmp25-dvk.dts:431`).
    - The RIF DT already reserves TF-M code and data (`0x80000000` and
      `0x80900000`, CID2, secure, encrypted; `ccmp25-dvk-rif.dtsi:954-964`).
  - **Conclusion:**
    - No stage of the current chain can hand control *to* the M33 instead of
      the A35. The A35 always continues to BL33.
    - The A35 chain can start the M33 alongside itself: U-Boot or Linux
      through SCMI, or OP-TEE's TA, including a secure TF-M image with TZEN.
      It always stays the TDCID while doing so.
    - Reversing who measures whom requires the ROM's M33-master boot modes,
      the M33-TD flavour above.
- **[I] An intermediate step that uses only Digi software: A35-TD plus a
  measured M33.** OP-TEE already authenticates the M33 image (RPROC-FW-PKH).
  It could also extend a measurement of that image into its own log. This does
  not move the root of trust to the M33: the M33 is measured by the A35 chain,
  not the reverse. It does need no boot-mode change and no new FSBL.

## 6. OTP plan (not executed)

- **Measured boot needs no fuses.** DICE works on an open device. The CDIs are
  just not anchored to an authenticated L0 until secure boot is on.
- Secure boot, in order, once M33-TD boots reliably:
  1. Generate the OEM key pairs and sign FSBL-M (MCUboot) and FSBL-A (TF-A BL2).
  2. Program the OEM-KEY1/KEY2 public-key-hash tables (OTP 144–159).
     **Irreversible.**
  3. Verify that signed images boot on the still-open device.
  4. Close the device (product-state OTP). **Irreversible.** After this, only
     correctly signed images boot. Make sure that includes the USB-programmer
     images used for recovery.
- Do not do either irreversible step until the recovery dry run (area 1) passes
  with signed images.

## Risks

| Risk | Recovery |
|---|---|
| M33-TD FSBL fails to boot (DDR, PMIC or clocks) | S1 to UART/USB, then STM32CubeProgrammer restores the A35-TD eMMC layout. Needs hands. |
| eMMC boot partitions overwritten without a backup | Recovery dry run and backup first (area 1). |
| RIF configured without A35 access to DDR or the GIC | Same as the first row; firewalls reset on reboot because they are not in OTP. |
| Wrong OTP key hash fused | **None; the part is bricked for secure boot.** Fuse only after signed boot passes on an open device. |
| ST M33-TD images still gated (alpha) | Request access from ST, or build from the 6.1 sources. |
| Watchdog owner changes and seL4 stops petting it | Board resets every 32 s; fix the pet path before switching the seL4 image. |

## Open questions (need ST/Digi documents or support)

1. ST's cm33tdcid **eMMC** flash layout: can the A35-TD and M33-TD FSBLs coexist
   in the boot partitions?
2. Is M33-TD supported on STM32MP255 (not just 257/215)?
3. In M33-TD, who owns IWDG1 and how should the A35 non-secure world pet it?
4. RISAF4 region count and granularity (RM0457), which limits the DDR split.
5. Can the SAES DHUK be locked from MCUboot so that later stages cannot
   re-derive the UDS?

## Staged path for a later round

1. **Read-only:** get RM0457 and the OpenSTLinux 6.1 M33-TD package and
   flash layout; answer questions 1–5.
2. **Recovery dry run (needs hands):** in today's A35-TD mode, back up the eMMC
   boot partitions and restore them with STM32CubeProgrammer over USB.
3. **Stock ST M33-TD on our SOM (needs hands):** STM32MP257F-EV1 images won't
   init our DDR. Only try them if ST's DDR settings match. Otherwise skip to
   step 4.
4. **Port:** a CCMP25 cm33tdcid board for MCUboot/TF-M, TF-A
   (`STM32MP_M33_TDCID=1`) and OP-TEE (`CFG_STM32_CM33TDCID`). Then boot our
   existing seL4 multikernel image unchanged.
5. **Move RIF to the M33:** the RISAF4/RIMC DMA-isolation split.
6. **DICE:** MCUboot L0, TF-M DPE, the TF-A DPE backend over IPCC, then
   elfloader measurements of K0/K1.
7. **Secure boot fusing** (area 6), last.
