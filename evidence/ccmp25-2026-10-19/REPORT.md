# CCMP25-DVK board experiments: Cedar vs EverParse on two seL4 kernels

Date: 2026-10-19. Board: Digi CCMP25-DVK, STM32MP255CAL (2× Cortex-A35 at 1.2 GHz,
generic timer at 40 MHz), stock Digi DEY 5.0-r3 firmware. **RIF is not enabled.** The
assumption is that the kernels and DMA behave. There is **no verified-configuration
claim**: the kernels are debug builds with printing on.

Plan: `papers/drafts/access-control-as-constraints-s-and-p/studies/ccmp25-board/EXPERIMENT_PLAN.md`.
The spec and live status are in `policy_gateway/docs/implementations/aarch64-ccmp25/POLICY_GATE_BRIEF.md`
("Paper experiments").

## Setup (multikernel; the single-kernel fallback was not needed)

```
host 192.168.40.1 (enp44s0) --lab switch--> J22 ETH1
  K1 (core 1, no SMC): DWMAC driver, lwIP 2.1.2, HTTP parse -> DecisionInput
      -- mk_channel (Device-mapped SPSC rings + SGI 1) -->
  K0 (core 0): engine_decide / engine_update (none | EverParse | Cedar), backend reply
```

- **Engines** are the paper's sources, unchanged
  (`multikernel_hello/gateway/`, vendored from `experiment/http-acl-x86-sp2027` @ `e5461f3`).
  - **EverParse:** the composed gate (`GateLowGateDecide`) plus the PolicyBlob update gate,
    compiled `-O2`.
  - **Cedar:** an `aarch64-unknown-linux-musl` release staticlib (panic=abort) linked into
    K0's bare seL4 rootserver against seL4 musllibc.
- **Cedar runtime support** (no kernel, elfloader or firmware changes):
  - a syscall-free size-class heap (`k0_malloc.c`, 48 MiB arena);
  - an 8 MiB root stack (Cedar's typechecker recursion);
  - a 2^16-slot root CNode;
  - stubs for `getauxval` (A35 has no LSE), `getrandom` (deterministic benchmark-only entropy,
    as in the paper's Microkit Cedar build), `pthread_getattr_np` and `_dl_find_object`.
- **Board request model**, which differs from x86 D1:
  - There is no TLS and no login component. K1 maps `Authorization: Bearer admin|operator`
    to role/scope `ADMIN/0xffff` or `OPERATOR/0x0001`.
  - `rate_count` comes from an `X-Rate` header (default 0).
  - Paths `/api/{login,logout,status,policy,demo,reports}` map to the paper's `PATH_*` IDs.
  - The policy and requests are the x86 D1 ones: the demo policy is installed with
    `PUT /api/policy`; allow is admin `GET /api/status` (200) and deny is admin
    `GET /api/demo` (403).
- **Images:** one per engine, staged on the eMMC boot partition and booted fresh per run
  (`deploy/boot_image.sh`). Each boot also runs C2 and C1 before serving (K1 starts its
  network only after K0 reports READY). Hashes are in `images.sha256`; the kernel revision is
  in `kernel.rev`.

## Protocol

- **C0/C3:** 30 blocks. In each block every engine gets a fresh boot (NRST reset, then
  U-Boot loads the image). Engine order rotates through all 6 permutations. Scenario order
  alternates per block.
- **Per boot:** 4 warm-up plus 32 timed requests per scenario, one TCP connection per
  request, timed on the host with `perf_counter_ns`. The client is pinned to CPU 2.
- **Statistic:** median of the 30 block medians.
- **C2:** on K0, in every campaign boot. 100 warm-up plus 1,000 single-call samples for each
  of empty_bracket, allow_first, allow_last and deny_path. Inputs are those of the x86
  `timing.c` (deployment8). PMU cycles at 1.2 GHz. Statistic: median over 30 boots of the
  boot mean.
- **C1:** the paper's `test_equivalence.c` compiled into K0 (12,527 comparisons against the
  in-image oracle), on every boot.
- **C4/C6:** one fresh boot per engine for each (`c4c6/`).

Totals: 90 campaign boots, 0 retries. Everything observed is observed-not-WCET.
Comparisons with x86 D1 are side by side only, never ratios.

## C0/C3: end-to-end HTTP, host to board and back (median of block medians, µs)

| Engine | Allow | Deny | Allow block-median range | Deny block-median range | Blocks |
|---|---:|---:|---|---|---:|
| none (C0, no ACL) | 284.2 | 275.6 | 242.9–330.3 | 242.6–340.0 | 30 |
| everparse | 279.7 | 275.2 | 236.3–326.4 | 243.5–321.8 | 30 |
| cedar | 586.6 | 541.9 | 556.2–626.7 | 498.9–575.6 | 30 |

Per request inside the board (median over all timed requests):

| Engine / scenario | K0 engine µs (PMU, wall) | K1 CPU µs during the K0 call (PMU stops in WFI: K1 busy time, not wall time) |
|---|---:|---:|
| cedar/allow | 274.57 | 9.06 |
| cedar/deny | 245.45 | 8.98 |
| everparse/allow | 1.24 | 8.84 |
| everparse/deny | 1.23 | 8.89 |
| none/allow | 0.01 | 8.85 |
| none/deny | 0.01 | 8.86 |

## C2: engine_decide cost on K0 (median over boots of the boot mean; 1,000 samples after 100 warmup)

| Engine | Case | Cycles | ns @1.2 GHz | Boot-mean range (cycles) | Boots | Mismatches |
|---|---|---:|---:|---|---:|---:|
| cedar | allow_first | 321,772 | 268,143 | 321,706–321,840 | 30 | 0 |
| cedar | allow_last | 334,602 | 278,835 | 334,551–334,647 | 30 | 0 |
| cedar | deny_path | 289,832 | 241,527 | 289,770–289,892 | 30 | 0 |
| cedar | empty_bracket | 11 | 9 | 11–11 | 30 | 0 |
| everparse | allow_first | 1,023 | 852 | 1,020–1,023 | 30 | 0 |
| everparse | allow_last | 1,042 | 868 | 1,042–1,042 | 30 | 0 |
| everparse | deny_path | 1,051 | 876 | 1,051–1,052 | 30 | 0 |
| everparse | empty_bracket | 11 | 9 | 11–11 | 30 | 0 |

## C1: on-board equivalence (every campaign boot)

| Engine | Boots | EQUIVALENCE_OK (12,527 comparisons, 0 failures) |
|---|---:|---:|
| cedar | 30 | 30 |
| everparse | 30 | 30 |

## C4: policy updates over HTTP (PUT /api/policy)

| Engine | Case | HTTP | Expected | Digest unchanged on reject | Engine cycles | Pass |
|---|---|---:|---:|---|---:|---|
| everparse | valid | 200 | 200 | - | 1,856 | True |
| everparse | wrong_role | 403 | 403 | True | 1,609 | True |
| everparse | rate_ge_50 | 403 | 403 | True | 1,484 | True |
| everparse | zero_rules | 403 | 403 | True | 1,529 | True |
| everparse | nine_rules | 403 | 403 | True | 1,526 | True |
| everparse | bad_method_mask | 403 | 403 | True | 439 | True |
| everparse | bad_min_role | 403 | 403 | True | 465 | True |
| everparse | truncated_64B | 403 | 403 | True | 59 | True |
| everparse | valid_again | 200 | 200 | - | 1,464 | True |
| everparse | valid update cost, median of 32 | | | | 1,224 | |
| cedar | valid | 200 | 200 | - | 5,614,631 | True |
| cedar | wrong_role | 403 | 403 | True | 1,074 | True |
| cedar | rate_ge_50 | 403 | 403 | True | 491 | True |
| cedar | zero_rules | 403 | 403 | True | 377 | True |
| cedar | nine_rules | 403 | 403 | True | 369 | True |
| cedar | bad_method_mask | 403 | 403 | True | 397 | True |
| cedar | bad_min_role | 403 | 403 | True | 410 | True |
| cedar | truncated_64B | 403 | 403 | True | 82 | True |
| cedar | valid_again | 200 | 200 | - | 5,605,982 | True |
| cedar | valid update cost, median of 32 | | | | 5,558,880 | |

## C6: compromised-K1 probes

| Engine | Probe | Outcome |
|---|---|---|
| everparse | forged_role | `{"rc": 0, "result": 1, "note": "K0 decides on the fields K1 asserts"}` {"uptime_before": 4, "uptime_after": 4, "board_survived": true} |
| everparse | forged_scope | `{"rc": 0, "result": 0, "digest": "1c87d388f42b17aa"}` {"uptime_before": 4, "uptime_after": 4, "board_survived": true} |
| everparse | oversize | `{"rc": 0, "result": 0, "expect": "deny (0)"}` {"uptime_before": 4, "uptime_after": 4, "board_survived": true} |
| everparse | truncated | `{"rc": -1, "expect_rc": -1, "followup_rc": 0, "followup_result": 0}` {"uptime_before": 4, "uptime_after": 4, "board_survived": true} |
| everparse | bad_type | `{"rc": -1, "expect_rc": -1, "followup_rc": 0, "followup_result": 0}` {"uptime_before": 4, "uptime_after": 4, "board_survived": true} |
| everparse | k0_ram | `{"k1_untyped_covering_k0_ram": 0, "expect": 0}` {"uptime_before": 4, "uptime_after": 4, "board_survived": true} |
| everparse | sgi_flood | `{"signals": 100000, "ms": 45, "followup_rc": 0, "followup_result": 0}` {"uptime_before": 4, "uptime_after": 4, "board_survived": true} |
| everparse | silent | `{"silent_s": 40, "followup_rc": 0}` {"uptime_before": 4, "uptime_after": 44, "board_survived": true} |
| everparse | bad_sequence | `{"k0_corrupt_before": 0, "k0_corrupt_after": 2}` {"gated_request_after": 503, "uptime_after_35s": 82} |
| cedar | forged_role | `{"rc": 0, "result": 1, "note": "K0 decides on the fields K1 asserts"}` {"uptime_before": 3, "uptime_after": 3, "board_survived": true} |
| cedar | forged_scope | `{"rc": 0, "result": 0, "digest": "1c87d388f42b17aa"}` {"uptime_before": 3, "uptime_after": 3, "board_survived": true} |
| cedar | oversize | `{"rc": 0, "result": 0, "expect": "deny (0)"}` {"uptime_before": 3, "uptime_after": 3, "board_survived": true} |
| cedar | truncated | `{"rc": -1, "expect_rc": -1, "followup_rc": 0, "followup_result": 0}` {"uptime_before": 3, "uptime_after": 4, "board_survived": true} |
| cedar | bad_type | `{"rc": -1, "expect_rc": -1, "followup_rc": 0, "followup_result": 0}` {"uptime_before": 4, "uptime_after": 4, "board_survived": true} |
| cedar | k0_ram | `{"k1_untyped_covering_k0_ram": 0, "expect": 0}` {"uptime_before": 4, "uptime_after": 4, "board_survived": true} |
| cedar | sgi_flood | `{"signals": 100000, "ms": 45, "followup_rc": 0, "followup_result": 0}` {"uptime_before": 4, "uptime_after": 4, "board_survived": true} |
| cedar | silent | `{"silent_s": 40, "followup_rc": 0}` {"uptime_before": 4, "uptime_after": 44, "board_survived": true} |
| cedar | bad_sequence | `{"k0_corrupt_before": 0, "k0_corrupt_after": 1}` {"gated_request_after": 503, "uptime_after_35s": 81} |

## C5: K0 footprint (bytes)

| Engine | text+rodata | data | bss total | of which: heap arena | musl morecore | root stack | other bss | image |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| none | 128,222 | 4,824 | 9,592,264 | 0 | 1,048,576 | 8,388,608 | 155,080 | 877,760 |
| everparse | 158,062 | 4,832 | 9,592,264 | 0 | 1,048,576 | 8,388,608 | 155,080 | 906,432 |
| cedar | 3,718,460 | 217,440 | 59,924,904 | 50,331,648 | 1,048,576 | 8,388,608 | 156,072 | 4,676,288 |

## Notes and claim boundaries

- **C3:**
  - With the EverParse gate, end-to-end latency is the same as the no-ACL baseline C0
    (279.7 vs 284.2 µs allow): its ~1.2 µs decision is lost in network and stack noise.
  - Cedar adds about 300 µs per request, consistent with its ~270 µs `engine_decide` on
    the A35.
  - Block-median ranges overlap for none and EverParse.
- **C2:** EverParse costs about 1,020–1,050 cycles (0.85–0.88 µs) and Cedar about
  290k–335k cycles (0.24–0.28 ms). Variation across boots is tiny (boot-mean ranges above).
- **K1 CPU column:** the PMU cycle counter stops in WFI, so the "K1 CPU" column is K1's busy
  time during a K0 call, not wall time. The mk_channel round trip alone is measured
  separately at boot (`bench:` lines, p50 about 28 µs for 512 B after `-O2`).
- **C4:** every case returns the HTTP status that D_update predicts, and the policy digest is
  unchanged on every reject.
  - The first `valid` update changes the digest by design: demo policy replaces default.
  - Update cost: EverParse ~1.2k cycles; Cedar ~5.6 M cycles (~4.6 ms; it rebuilds its
    policy set).
- **C6 (kernels behave):**
  - **Fail closed:** oversize, truncated and bad-type frames are denied or dropped, and the
    next well-formed request is still served.
  - **SGI flood:** 100,000 signals in 45 ms leave K0 serving.
  - **Silence:** K1 silent for 40 s, past the 32 s IWDG window, and the board survives
    because K0 pets the watchdog independently.
  - **K0 RAM:** K1 holds no untyped covering K0 RAM (P1 reservation).
  - **Corrupted ring sequence:** K0 counts it (`corrupt` 0→1/2) and keeps running. The
    channel then stays blocked, so gated requests get 503 (fail closed) until reboot.
  - **Forged role/scope fields: K0 allows** what the forged attributes permit. K0 decides on
    the attributes K1 asserts, so **K1 is in the authentication TCB** in this topology. The
    ACL protects K0's resources only against requests that K1 forwards honestly.
    Authentication would have to move to K0 (e.g. K0 verifies a token K1 passes through) to
    remove that.
- **Not done:**
  - RIF/DMA confinement (built and not flashed; see `firmware/optee-rif/`).
  - Cedar heap high-water measurement: the arena size is recorded; peak use is not.
  - WCET.

## Files

- `c0c2c3/`: `manifest.json` (runs, hashes, retries), one JSON per boot (raw latencies,
  per-request engine cycles, full serial log, parsed C2 and C1), `summary.json`
  (`analyze.py`), `campaign.log`, `precheck.txt`.
- `c4c6/`: `c4-<engine>.json`, `c6-<engine>.json` (with serial logs).
- `c5/footprint.json`.
- `images.sha256`, `kernel.rev`, `TABLES.md` (`report.py`), `SHA256SUMS` (every file).
- Scripts: `multikernel_hello/exp/{campaign,c4_c6,analyze,report}.py`.
