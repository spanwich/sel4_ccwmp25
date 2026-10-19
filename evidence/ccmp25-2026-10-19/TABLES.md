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
