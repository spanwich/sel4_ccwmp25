# CCMP25-DVK board, corrected design: the x86 D1 gate on two seL4 kernels (D0a–D7)

Date: 2026-10-19/20. Board: Digi CCMP25-DVK, STM32MP255CAL (2× Cortex-A35 at 1.2 GHz,
generic timer at 40 MHz), stock Digi DEY 5.0-r3 firmware. **RIF is not enabled.**
No FIP/OP-TEE changes. The kernels are debug builds with printing on, so there is
**no verified-configuration claim**. All timings are observed, not WCET.

- **Spec:** `papers/drafts/access-control-as-constraints-s-and-p/studies/ccmp25-board/CORRECTED_DESIGN.md`.
- **Supersedes** `evidence/ccmp25-2026-10-19/`, whose K1 parsed HTTP and mapped tokens to roles.
- **Live status:** `policy_gateway/docs/implementations/aarch64-ccmp25/POLICY_GATE_BRIEF.md`,
  section "Corrected design (D0a–D7)".

## Setup (multikernel; the single-kernel fallback was not needed)

```
host 192.168.40.1 (enp44s0) --lab switch--> J22 ETH1
  K1 user code (UNTRUSTED): DWMAC driver, lwIP 2.1.2, raw HTTP forwarder (:80), control port (:81)
      -- mk_channel ABI v2 (Device-mapped SPSC rings, 512 B slots, SGI 1);
         messages <= 4096 B fragmented, 40 B header + <= 472 B per slot -->
  K0 user code (TRUSTED): x86 D1 PDs, unchanged sources:
      policy_gate (F* extract_security_params -> AUTH_IDENTIFY -> RateCounter_tick_and_publish
                   -> pipeline_process_frame -> engine) <-> authenticator
      -> protected_app (HTTP response) -> back to K1
Both seL4 kernels are trusted (paper A6); the kernel boundary replaces the x86 PD boundaries.
```

**Trust mapping.** It is as in the spec.
- "K1 compromised" means K1's user code is attacker-controlled. K1's kernel still enforces K1's
  configuration, so the attacker controls only the bytes K1 puts on the channel.
- **K1 does not:**
  - parse paths or headers. It only does x86 `lwip_proxy.c`'s framing: find the header end,
    read `Content-Length`, and bound the buffer at 4096 B;
  - map tokens, set role/scope/rate, or hold policy.
- The superseded `Bearer admin|operator` mapping, the `X-Rate` header and the path table are
  deleted.

**K0 port** (`rootserver/gateway_k0.c`, `rootserver/k0shim/microkit.h`):
- **Unchanged sources.** `demo/pds/policy_gate.c`, `authenticator.c`, `protected_app.c` and
  `stage.h`, plus `common/pipeline.c`, the extracted F\* C (`demo/generated/HTTP_Extract_*.c`)
  and `verified/rate_counter/RateCounter.h`. All are compiled from byte-identical copies of
  `experiment/http-acl-x86-sp2027` @ `e5461f3` (`multikernel_hello/gateway/SHA256SUMS`).
- **Renaming.** Each PD's `init`, `notified` and config globals are renamed with `-D` flags.
  The F\* objects get `-Dwrite_u32_le=fstar_write_u32_le`, as in x86 `demo.mk`.
- **Microkit replaced.**
  - The queues stay Microkit's own `DemoQueue`/`AuthQueue` structures, in static memory.
  - `microkit_notify()` marks the peer's channel pending, using the x86 `meta.py` wiring:
    proxy→gate→app→proxy, plus gate↔authenticator.
  - A run-to-idle dispatcher delivers `notified()`.
  - A K0 "proxy" stands in for x86 `lwip_proxy.c`'s pipeline side. It reassembles K1's
    fragments, which must arrive strictly in order or the partial message is discarded. It
    builds the `DEMO_MESSAGE_RAW_HTTP` message with `length` = bytes actually received, not
    K1's claim, and fragments protected_app's HTTP response back to K1.
- **Authenticator isolation.** The authenticator is a separate component with the narrow
  x86 interface: `AuthQueue` request/response plus one channel. It shares **K0's single
  address space** with the gate; there is no separate VSpace. A separate thread or VSpace in
  the bare rootserver was not attempted. The authenticator's isolation therefore rests on K0
  being one trusted unit. On x86 D1 it is a separate PD.
- **Timing hook.** `policy_gate`'s `init()` calls `engine_init()` and then
  `gateway_timing_run()`. The x86 one is rdtsc-only, so on the board that hook runs:
  - D3 engine-only;
  - D2: the 12,527-case equivalence corpus and the F\* extractor vectors
    (`verified/test_extractor.c`);
  - a reinstall of the default policy.
- **Dashboard.** `GET /` returns the embedded dashboard (x86 `dashboard_html.h`), so
  `demo_test.py`'s dashboard checks run unchanged.

**4 KiB messages: fragmentation (chosen), not channel ABI v3.**
- ABI v2's region is 16,384 slots × 576 B = 10 MiB. Raising the payload to 4 KiB would need a
  new slot layout and count, plus changes on both sides.
- A 40-byte fragment header (`gw_proto.h`) keeps ABI v2 unchanged. A 4096 B message takes
  9 slots.
- The response header also carries K0's PMU cycle counts. This is measurement only and never
  reaches the client's HTTP bytes.

**Images** (`MK_ENGINE`). Hashes are in `images.sha256` and revisions in `kernel.rev`. K1 is
identical in all four images.

| Image | K0 behaviour |
|---|---|
| raw (D0a) | answers a fixed `200 {"status":"ok"}` to any complete message, unread: network + channel cost |
| gateonly (D0b) | full gate path (extractor, authenticator, counter, `pipeline_process_frame`), accept-all engine |
| everparse | composed gate `GateLowGateDecide` + update gate `BlobLowUpdateDecide` |
| cedar | the Cedar staticlib engine, same frames (Cedar runtime support as in the superseded run: k0_malloc 48 MiB arena, 8 MiB root stack, 2^16 root CNode, libc stubs) |

**Other board differences from x86 D1:**
- port 80 instead of 8080;
- a request that would overflow K1's 4096 B buffer is reset. x86 refuses the pbuf and the
  client stalls;
- K1 calls K0 synchronously, with a 0.5 s bound.

**Also fixed:** a lost ETH IRQ acknowledgement. When a channel wait consumed the ETH badge,
the IRQ was never acked and RX fell back to the 100 ms poll. The superseded campaign had the
same latent bug.

## Protocol

- **D4 and D0a/D0b.** 30 blocks. In each block every image gets fresh boots
  (`deploy/boot_image.sh`: NRST reset, then U-Boot `fatload` + `bootm`).
  - **Boots per block:** raw takes one boot for both scenarios. gateonly, everparse and cedar
    take one boot per scenario, because the verified rate counter is per role and never
    resets. x86 `http_benchmark.py` likewise boots per scenario.
  - **Order:** image order rotates through all 24 permutations; scenario order alternates.
  - **Per boot:** login with `POST /api/login` (raw uses the same header bytes unread), the
    x86 demo-policy install, then 4 warm-up and 32 timed requests. Allow is admin
    `GET /api/status`; deny is admin `GET /api/demo`. One TCP connection per request, timed on
    the host with `perf_counter_ns`, client pinned to CPU 2.
  - **Statistic:** median of the 30 block medians.
  - **Totals:** 210 boots. Retries: see `summary.json`.
- **D3 whole path.** For each timed request, K0's PMU cycles from queue-in to response-out
  (wall time, since K0 does not WFI while serving), split by PD. They are read from K1's
  control port after the timed loop. Statistic: median over boots of the boot mean.
- **D3 engine-only and D2.** These run on K0 at every everparse/cedar D4 boot (60 per engine).
  The inputs are x86 `timing.c` deployment8: 100 warm-up plus 1,000 samples per case.
- **D1.** One fresh boot per engine. The x86 `demo_test.py` `prepare_demo_policy()` and
  `exercise()` run unchanged through `exp/demo_test_board.py`, which only redirects
  127.0.0.1 to the board.
- **D5 and D7.** One fresh boot per engine for each (`d5d7/`).
- **D6.** Computed from the build artefacts (`d6/footprint.json`).

Comparisons with x86 D1 are side by side, never ratios. x86 is QEMU/KVM with virtio-net on a
desktop CPU; the board is a 1.2 GHz Cortex-A35 with a real NIC.

## Summary

| ID | Result |
|---|---|
| D0a | raw pipeline: 268.0 / 265.1 µs (allow/deny requests, both answered 200 unread); K0 path 10 cycles |
| D0b | gate path, accept-all engine: 288.3 / 285.0 µs; K0 path ~19.1k cycles (16 µs): policy_gate ~10.9k, authenticator ~1.5k, protected_app ~3.5k |
| D1 | **pass**: x86 `demo_test.py` (HTTP_DEMO_PASS) for both engines. The rate limit hits at request 34; the stored x86 log says 36 because it predates `prepare_demo_policy()` (2 admin requests) in the e5461f3 script |
| D2 | **pass**: 0 failures on all 120 engine boots (12,527 comparisons + 28/28 F\* extractor vectors each) |
| D3 | whole K0 path: EverParse 21.1k / 20.8k cycles (17.5 / 17.3 µs), Cedar 318k / 282k cycles (265 / 235 µs); engine-only EverParse ~1.03–1.05k cycles, Cedar 292k–337k cycles, 0 mismatches, 60 boots each |
| D4 | median of 30 block medians: EverParse 288.6 / 288.3 µs, Cedar 550.9 / 528.1 µs (allow 200 / deny 403); x86 D1: EverParse 680 / 684 µs, Cedar 715 / 716 µs (side by side, different platforms) |
| D5 | **pass**: all 12 cases per engine as D_update and the pipeline predict, digest unchanged on every reject. Valid update: policy_gate ~14.8k cycles (EverParse), ~5.66M cycles (Cedar) |
| D6 | see table; the gate additions on K0 (F\* extractor, gate, pipeline, authenticator, app, glue) total ~26 KB text excluding the engine |
| D7 | **pass**: forged identity headers, duplicate Authorization, the old `Bearer admin`, path tricks: no effect (403/401/400/404); garbage/oversize/bad-type/out-of-order/forged-SecurityParams frames dropped or answered 400, K0 kept serving (admin read 200 after each); SGI flood and 40 s K1 silence harmless; K1 has no untyped over K0 RAM; bad_sequence wedges the channel (fail closed) and K0 keeps petting the IWDG |

210 D4 boots, status complete; 26 retries, all `phy: link up` timeouts at boot before any request
(retried with a fresh boot, recorded in `d4/manifest.json`). The superseded campaign's C6
finding (forged role accepted) does not occur: K1 never asserts identity, and the
K1-forged ready-made SecurityParamsWire (`forged_secparams`) is rejected by the extractor stage (400).

## D4 / D0a / D0b: end-to-end HTTP, host to board and back (median of 30 block medians, µs)

| Image | Allow | Deny | Allow block-median range | Deny block-median range | Blocks | x86 D1 allow / deny |
|---|---:|---:|---|---|---:|---|
| raw (D0a: fixed 200, no gate) | 268.0 (200) | 265.1 (200) | 254.2–309.1 | 253.3–309.6 | 30 | - |
| gateonly (D0b: gate, accept-all) | 288.3 (200) | 285.0 (200) | 274.3–334.0 | 271.7–323.1 | 30 | - |
| EverParse | 288.6 (200) | 288.3 (403) | 270.4–315.0 | 271.2–327.9 | 30 | 680 / 684 |
| Cedar | 550.9 (200) | 528.1 (403) | 525.9–584.0 | 501.4–557.2 | 30 | 715 / 716 |

## D3: whole K0 path per request (PMU cycles, median over boots of the boot mean)

Queue in to response out on K0 (wall time; K0 does not WFI while serving). Inside: time spent in each PD's notified(). K1 busy = K1 CPU cycles during the exchange (PMU stops in WFI, so not wall time).

| Image / scenario | K0 path cycles | µs | policy_gate | authenticator | protected_app | K1 busy | Boots |
|---|---:|---:|---:|---:|---:|---:|---:|
| raw / allow | 10 | 0.01 | 0 | 0 | 0 | 9,846 | 30 |
| raw / deny | 10 | 0.01 | 0 | 0 | 0 | 9,861 | 30 |
| gateonly / allow | 19,212 | 16.01 | 10,965 | 1,466 | 3,488 | 9,958 | 30 |
| gateonly / deny | 19,084 | 15.90 | 10,812 | 1,466 | 3,517 | 10,073 | 30 |
| everparse / allow | 21,062 | 17.55 | 12,878 | 1,439 | 3,415 | 9,961 | 30 |
| everparse / deny | 20,751 | 17.29 | 12,582 | 1,441 | 3,379 | 10,387 | 30 |
| cedar / allow | 318,384 | 265.32 | 308,374 | 1,847 | 4,355 | 10,465 | 30 |
| cedar / deny | 281,879 | 234.90 | 272,188 | 1,830 | 4,074 | 10,700 | 30 |

## D3: engine_decide only (median over boots of the boot mean; 1,000 samples after 100 warmup)

| Engine | Case | Cycles | ns @1.2 GHz | Boot-mean range (cycles) | Boots | Mismatches | x86 D1 ns |
|---|---|---:|---:|---|---:|---:|---:|
| cedar | allow_first | 323,681 | 269,734 | 323,626–323,746 | 60 | 0 | 35,393.0 |
| cedar | allow_last | 336,736 | 280,613 | 336,674–336,798 | 60 | 0 | 36,508.0 |
| cedar | deny_path | 292,004 | 243,337 | 291,917–292,061 | 60 | 0 | 32,832.0 |
| cedar | empty_bracket | 11 | 9 | 11–11 | 60 | 0 | - |
| everparse | allow_first | 1,029 | 858 | 1,026–1,033 | 60 | 0 | 86.7 |
| everparse | allow_last | 1,037 | 864 | 1,037–1,040 | 60 | 0 | 78.0 |
| everparse | deny_path | 1,053 | 878 | 1,053–1,053 | 60 | 0 | 83.7 |
| everparse | empty_bracket | 11 | 9 | 11–11 | 60 | 0 | - |

## D2: on-board equivalence and F* extractor vectors (every D4 boot)

| Engine | Boots | EQUIVALENCE_OK (12,527 comparisons) | EXTRACTOR_OK (28/28 vectors) |
|---|---:|---:|---:|
| everparse | 60 | 60 | 60 |
| cedar | 60 | 60 | 60 |

## D1: x86 scripts/demo_test.py against 192.168.40.129:80

| Engine | Result | rate limit at | connection pressure |
|---|---|---:|---|
| everparse | HTTP_DEMO_PASS | 34 | completed=5,bounded_failures=3 |
| cedar | HTTP_DEMO_PASS | 34 | completed=4,bounded_failures=4 |

## D5: policy updates via PUT /api/policy (real tokens)

| Engine | Case | HTTP | Expected | Digest unchanged on reject | Pass |
|---|---|---:|---:|---|---|
| everparse | valid | 200 | 200 | - | True |
| everparse | wrong_role_operator | 403 | 403 | True | True |
| everparse | zero_rules | 403 | 403 | True | True |
| everparse | nine_rules | 403 | 403 | True | True |
| everparse | bad_method_mask | 403 | 403 | True | True |
| everparse | bad_min_role | 403 | 403 | True | True |
| everparse | truncated_64B | 403 | 403 | True | True |
| everparse | oversize_2000B | 413 | 413 | True | True |
| everparse | no_token | 401 | 401 | True | True |
| everparse | unknown_token | 403 | 403 | True | True |
| everparse | valid_again | 200 | 200 | - | True |
| everparse | rate_ge_50 | 403 | 403 | True | True |
| everparse | valid update, median of 16: policy_gate 14,807 cycles, K0 path 23,139 cycles | | | | |
| cedar | valid | 200 | 200 | - | True |
| cedar | wrong_role_operator | 403 | 403 | True | True |
| cedar | zero_rules | 403 | 403 | True | True |
| cedar | nine_rules | 403 | 403 | True | True |
| cedar | bad_method_mask | 403 | 403 | True | True |
| cedar | bad_min_role | 403 | 403 | True | True |
| cedar | truncated_64B | 403 | 403 | True | True |
| cedar | oversize_2000B | 413 | 413 | True | True |
| cedar | no_token | 401 | 401 | True | True |
| cedar | unknown_token | 403 | 403 | True | True |
| cedar | valid_again | 200 | 200 | - | True |
| cedar | rate_ge_50 | 403 | 403 | True | True |
| cedar | valid update, median of 16: policy_gate 5,655,770 cycles, K0 path 5,674,225 cycles | | | | |

## D7: compromised-K1 probes

HTTP probes (sent by the host through K1, as a compromised K1 could send them):

| Engine | Probe | HTTP | Expected | Pass |
|---|---|---:|---|---|
| everparse | baseline_operator_policy | 403 | 403 | True |
| everparse | baseline_admin_policy | 200 | 200 | True |
| everparse | forged_identity_headers_operator | 403 | 403 | True |
| everparse | forged_identity_headers_no_token | 401 | 401 | True |
| everparse | duplicate_authorization_operator_then_forged | 403 | [400, 403] | True |
| everparse | duplicate_authorization_forged_then_operator | 403 | [400, 403] | True |
| everparse | invalid_token | 403 | 403 | True |
| everparse | absent_token | 401 | 401 | True |
| everparse | empty_bearer | 401 | [400, 401, 403] | True |
| everparse | old_board_token_admin | 403 | 403 | True |
| everparse | path_trick /api/status/../policy | 400 | [400, 403, 404] | True |
| everparse | path_trick /api//policy | 404 | [400, 403, 404] | True |
| everparse | path_trick /API/POLICY | 404 | [400, 403, 404] | True |
| everparse | path_trick /api/policy%2f | 404 | [400, 403, 404] | True |
| everparse | path_trick /api/./policy | 404 | [400, 403, 404] | True |
| everparse | path_trick /api/policy;admin | 404 | [400, 403, 404] | True |
| everparse | path_trick /api/policy?role=admin | 403 | [400, 403, 404] | True |
| everparse | path_trick /api/policy#x | 404 | [400, 403, 404] | True |
| everparse | path_trick //api/policy | 404 | [400, 403, 404] | True |
| everparse | oversize_body_2000B | 413 | 413 | True |
| everparse | oversize_request_5000B | None | None | True |
| everparse | malformed_method | 400 | 400 | True |
| everparse | after_probes_operator | 403 | 403 | True |
| cedar | baseline_operator_policy | 403 | 403 | True |
| cedar | baseline_admin_policy | 200 | 200 | True |
| cedar | forged_identity_headers_operator | 403 | 403 | True |
| cedar | forged_identity_headers_no_token | 401 | 401 | True |
| cedar | duplicate_authorization_operator_then_forged | 403 | [400, 403] | True |
| cedar | duplicate_authorization_forged_then_operator | 403 | [400, 403] | True |
| cedar | invalid_token | 403 | 403 | True |
| cedar | absent_token | 401 | 401 | True |
| cedar | empty_bearer | 401 | [400, 401, 403] | True |
| cedar | old_board_token_admin | 403 | 403 | True |
| cedar | path_trick /api/status/../policy | 400 | [400, 403, 404] | True |
| cedar | path_trick /api//policy | 404 | [400, 403, 404] | True |
| cedar | path_trick /API/POLICY | 404 | [400, 403, 404] | True |
| cedar | path_trick /api/policy%2f | 404 | [400, 403, 404] | True |
| cedar | path_trick /api/./policy | 404 | [400, 403, 404] | True |
| cedar | path_trick /api/policy;admin | 404 | [400, 403, 404] | True |
| cedar | path_trick /api/policy?role=admin | 403 | [400, 403, 404] | True |
| cedar | path_trick /api/policy#x | 404 | [400, 403, 404] | True |
| cedar | path_trick //api/policy | 404 | [400, 403, 404] | True |
| cedar | oversize_body_2000B | 413 | 413 | True |
| cedar | oversize_request_5000B | None | None | True |
| cedar | malformed_method | 400 | 400 | True |
| cedar | after_probes_operator | 403 | 403 | True |

K1-side probes (K1 user code misbehaving on the channel), each followed by a gated admin `GET /api/policy` from the host:

| Engine | Probe | Result | Admin request after |
|---|---|---|---|
| everparse | garbage | `{"sent": [0, 0], "k0_replies": 0, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| everparse | oversize | `{"sent": [0, 0], "k0_replies": 0, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| everparse | bad_type | `{"sent": [0, 0], "k0_replies": 0, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| everparse | out_of_order | `{"sent": 0, "k0_replies": 0, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| everparse | forged_secparams | `{"rc": 0, "response": "HTTP/1.1 400 Bad Request", "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| everparse | k0_ram | `{"k1_untyped_covering_k0_ram": 0, "expect": 0}` | 200 |
| everparse | sgi_flood | `{"signals": 100000, "ms": 45, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| everparse | silent | `{"silent_s": 40, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| everparse | bad_sequence | `{"k0_corrupt_before": 0, "k0_corrupt_after": 1}` | <ConnectionResetError>, uptime after 35 s: 82 s |
| cedar | garbage | `{"sent": [0, 0], "k0_replies": 0, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| cedar | oversize | `{"sent": [0, 0], "k0_replies": 0, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| cedar | bad_type | `{"sent": [0, 0], "k0_replies": 0, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| cedar | out_of_order | `{"sent": 0, "k0_replies": 0, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| cedar | forged_secparams | `{"rc": 0, "response": "HTTP/1.1 400 Bad Request", "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| cedar | k0_ram | `{"k1_untyped_covering_k0_ram": 0, "expect": 0}` | 200 |
| cedar | sgi_flood | `{"signals": 100000, "ms": 45, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| cedar | silent | `{"silent_s": 40, "followup_rc": 0, "followup": "HTTP/1.1 401 Unauthorized"}` | 200 |
| cedar | bad_sequence | `{"k0_corrupt_before": 0, "k0_corrupt_after": 1}` | <ConnectionResetError>, uptime after 35 s: 82 s |

## D6: footprint (bytes)

| Image | text+rodata | data | bss total | of which K0 heap arena | musl morecore | image |
|---|---:|---:|---:|---:|---:|---:|
| K0 raw | 151,394 | 5,136 | 9,592,256 | 0 | 1,048,576 | 898,240 |
| K0 gateonly | 151,746 | 5,136 | 9,592,256 | 0 | 1,048,576 | 898,240 |
| K0 everparse | 189,522 | 5,144 | 9,592,256 | 0 | 1,048,576 | 935,104 |
| K0 cedar | 3,741,916 | 15,616 | 59,990,320 | 50,331,648 | 1,048,576 | 4,701,376 |
| K1 (all images) | 206,882 | 4,976 | 1,613,248 | - | 1,048,576 | (bundled) |

K0 bss includes the 8 MiB root stack (Sel4RuntimeRootStack).

Per component (object files before linking, EverParse build; `size -B`):

| Component | text | data | bss |
|---|---:|---:|---:|
| fstar_extractor | 6,252 | 96 | 8 |
| policy_gate (incl. dashboard, RateCounter inlined) | 9,481 | 48 | 8,228 |
| pipeline+wire+oracle | 2,288 | 0 | 0 |
| authenticator | 1,389 | 24 | 1,556 |
| protected_app | 2,308 | 24 | 4,116 |
| k0 glue (gateway_k0.c: shim, dispatcher, proxy, D3 timing) | 4,382 | 4 | 31,796 |
| d2 test code (equivalence corpus, extractor vectors) | 17,807 | 4 | 24 |
| everparse engine | 21,013 | 0 | 69 |
| Cedar engine (linked delta vs gateonly; includes cedar_libc_stubs.c, k0_malloc.c and the D2 test code) | 3,590,170 | 10,480 | - |

