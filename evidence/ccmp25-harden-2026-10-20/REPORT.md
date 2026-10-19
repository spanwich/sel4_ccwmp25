# CCMP25-DVK: per-kernel peripheral ownership and cheap hardening

Date: 2026-10-20. Board: Digi CCMP25-DVK, STM32MP255 (2× Cortex-A35, GICv2, unicore
seL4 per core). Builds on the corrected design (`evidence/ccmp25-d-2026-10-19/`,
sel4_ccwmp25 `5291bb4`). RIF not enabled; no FIP/OP-TEE change. Observed, not WCET.

## Motivation
`untyped_audit` previously skipped everything outside DDR, so **both** kernels received
all 39 device untypeds over all peripheral MMIO. K1's untrusted network code (its
rootserver holds every cap) could therefore map RCC, IWDG, GPIO and other registers K0
depends on. This round closes that and two cheap channel/GIC items.

## 1. Per-kernel peripheral ownership (device-tree allow-list + in-image audit)

**Ownership, verified against the rootserver's `map_device_page` callers** (the only
path to device MMIO):
- **K0** maps only RCC `0x44200000` (one register, 64 KiB page block). OP-TEE and the
  IWDG are reached by SMC; the GIC and console UART are kernel devices.
- **K1** maps ETH1 `0x482c0000` (16 KiB), SYSCFG ETH1CR `0x44233000`, and GPIO banks
  A/B/C/F/H (`0x44240000 + 0x10000·n` for n∈{0,1,2,5,7}).
- No other MMIO is mapped by either rootserver.

**Device tree (allow-list).** Each kernel's DTS now reserves every non-owned MMIO range
as `sel4,foreign-memory`, leaving only its owned pages as gaps. Because the kernel's
`create_untypeds` fills only the gaps between reserved regions, the owned pages are the
sole peripheral device untypeds created; everything else (including the whole
`0x0..0x40000000` low window and the `0xc0000000..0x10000000000` high window) is removed.
The GIC band `0x4ac00000..0x4ad00000` is left to the kernel, which already reserves the
GIC's live register pages; only unused fragments around them remain.

**Audit.** `untyped_audit` now prints every device untyped outside DDR and FAILs unless
each lies in this kernel's owned allow-list (the GIC-area fragments are tolerated and
labelled `gic-area`). On the board both kernels report `unowned-mmio=0 PASS`:

```
[K0] untyped-audit: MMIO 0x44200000..0x44210000 owned RCC
[K0] untyped-audit: untypeds=43 device=20 overlap=0 peer-ram=0 unowned-mmio=0 PASS
[K1] untyped-audit: MMIO 0x44233000..0x44234000 owned SYSCFG
[K1] untyped-audit: MMIO 0x44240000..0x44241000 owned GPIOA ... GPIOH
[K1] untyped-audit: MMIO 0x482c0000..0x482c4000 owned ETH1
[K1] untyped-audit: untypeds=58 device=27 overlap=0 peer-ram=0 unowned-mmio=0 PASS
```
(Full log: `audit.log`.) Device untypeds dropped from 39 to 20 (K0) / 27 (K1); the
remainder are owned pages, the ETH1 DMA window (K1), the channel window and GIC fragments.

## 2a. Channel bad-sequence recovery

A slot with a wrong sequence number made `mk_channel_try_recv` return `CORRUPT` without
advancing `rx_tail`, wedging K0's channel until reboot. Added `mk_channel_drop_one`: on
`CORRUPT`, `k0_serve` drops the bad slot and resynchronises (the bad slot already advanced
the peer `tx_head`, so dropping realigns both cursors) and keeps serving. A new
`corrupt_dropped` counter is in the K0 1 s stats frame (control port `:81`, `counters`).
The `bad_sequence` D7 probe now reports `expect:"recovered"` and its follow-up request is
served (see §3).

## 2b. GIC distributor split — NOT done, recorded as a trust assumption

Kent McLeod's `3ccb3d4a` ("multikernel: Assign SPIs in device tree") plus the
elfloader-distributor-init commits would stop each kernel from reconfiguring the shared
GIC distributor. They do **not** port in ~half a day here: `3ccb3d4a` targets a 2023
GICv2 tree, while this branch is seL4 16.0.0 with a restructured driver; a clean port
needs coordinated changes to the elfloader, the DTS (`seL4,spi-irqs`), the hardware tool
(`fdt.py`/`c_header.py`), the kernel GICv2 driver and `init_irqs`, a full kernel+elfloader
rebuild and re-test, and it interacts with the documented TF-A RISAB constraint on
elfloader-time secondary dispatch. Per the task's rule this is stopped and recorded.

**Trust assumption (GIC distributor).** Both kernels run `dist_init()` on the shared GICv2
distributor at boot; K1's boot disables the distributor and retargets all SPIs to K1. This
is benign today only because K0 owns no SPIs (its only interrupts are the per-CPU-banked
SGI 1 doorbell and the PPI 30 timer, which the distributor-wide reset does not touch). A
future multikernel kernel should assign SPIs per core and keep the secondary off the
shared distributor. K1 user code cannot reach the distributor regardless: the GIC is a
kernel device, not an untyped (confirmed by the ownership audit).

## Not fixed (listed as trust assumptions / future work)
- **Shared L2 cache maintenance by set/way** — not isolated between kernels.
- **Shared console UART** in these debug builds — K0 and K1 both print; removed in a
  release build.
- **Bus contention and timing channels** between the cores — out of scope.
- **Ethernet DMA without RIF** — K1's DMA-capable ETH1 is not firewalled by RIF/RIMC, so a
  compromised K1 could program ETH DMA to addresses outside its window. Closing this needs
  RIF firmware (skipped by project decision).

## 3. Re-run on the hardened images (all four engines)

Images (`images.sha256`), K1 identical across engines; built from sel4_ccwmp25 at the
hardening commits (`kernel.rev`).

- **D1** (`d1/`): x86 `demo_test.py` → `HTTP_DEMO_PASS` for EverParse and Cedar.
- **D2** (in every boot's serial): `EQUIVALENCE_OK` (12,527) and `D2_EXTRACTOR,EXTRACTOR_OK`
  (28/28) on every everparse/cedar boot.
- **D5** (`d5d7/d5-*.json`): all policy-update cases match `D_update`; digest unchanged on
  every reject; both engines. 0 failures.
- **D7** (`d5d7/d7-*.json`): both engines, every probe passes and a gated admin
  `GET /api/policy` returns 200 after each.
  - **Ownership probes (new):** K1 finds **no** device untyped covering RCC
    (`0x44200000`), IWDG (`0x44020000`), an unowned peripheral (`0x40000000`), or K0 RAM
    — `k1_untyped_covering = 0` in every case.
  - **bad_sequence (now recovers):** `k0_corrupt_before=0, after=1` (K0 counted the bad
    slot), the follow-up request is served (`401`), `expect:"recovered"`. No reboot.
  - Forged identity headers, duplicate `Authorization`, the old `Bearer admin`, path
    tricks, oversize, garbage/oversize/bad-type/out-of-order frames, the K1-forged
    `SecurityParamsWire` (→ 400), SGI flood, 40 s silence: no effect.
  - Uptime monotonic; `gated_admin_after_35s = 200` (watchdog kept petting past its 32 s
    window), board never rebooted.
- **D4 sanity** (`d4-sanity/`, 3 blocks, 0 retries): end-to-end medians within the earlier
  30-block ranges, so the hardening did not change timing:

  | image / scen | sanity median µs | sanity range | prior 30-block range |
  |---|---:|---|---|
  | raw / allow | 282.9 | 262.6–284.2 | 254.2–309.1 |
  | raw / deny | 254.3 | 254.2–270.7 | 253.3–309.6 |
  | gateonly / allow | 283.0 | 273.9–285.5 | 274.3–334.0 |
  | gateonly / deny | 281.8 | 279.5–282.0 | 271.7–323.1 |
  | everparse / allow | 288.8 | 282.0–340.2 | 270.4–315.0 |
  | everparse / deny | 316.8 | 298.4–325.0 | 271.2–327.9 |
  | cedar / allow | 556.6 | 540.7–578.0 | 525.9–584.0 |
  | cedar / deny | 542.9 | 532.9–557.1 | 501.4–557.2 |

## D6 footprint (bytes)

| image | text+rodata | bss total | image |
|---|---:|---:|---:|
| K0 raw | 152,274 | 9,592,256 | 904,384 |
| K0 gateonly | 152,634 | 9,592,256 | 904,384 |
| K0 everparse | 194,490 | 9,592,256 | 945,344 |
| K0 cedar | 3,742,772 | 59,990,320 | 4,707,520 |
| K1 (all) | 209,722 | 1,678,784 | — |

The ownership and bad-sequence changes add a few hundred bytes (the audit allow-list and
`mk_channel_drop_one`); the per-image totals are essentially unchanged from
`evidence/ccmp25-d-2026-10-19`.

## Note
The intermittent `phy: link up` negotiation seen in the earlier campaign recurs; the test
harness now gates readiness on the control port answering rather than on the serial string.
This is a link-negotiation flake, unrelated to the hardening, and the data path works once
up.
