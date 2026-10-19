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
