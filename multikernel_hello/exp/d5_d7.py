#!/usr/bin/env python3
"""D5 (policy updates through the gate) and D7 (compromised-K1 probes).

One fresh boot per engine for D5 and another for D7. Every request carries
real tokens obtained by POST /api/login; K0 authenticates, K1 only forwards.

D5 expectations follow D_update (update gate) plus the pipeline's own
status mapping: applied -> 200, rejected -> 403, extractor body bound -> 413,
no token -> 401, unknown token -> 403. The admin rate counter is consumed by
every authenticated request, so the rate >= 50 case runs last.
"""
import argparse
import hashlib
import json
import struct
import time
from pathlib import Path

import dcommon as dc

RULES = [(0x33333333, 0x01, 1, 0x0001), (0x22222222, 0x02, 1, 0x0000),
         (0x44444444, 0x01, 2, 0x0004)] + [(0xDEADDEAD, 0, 0, 0)] * 5


def blob(n_rules=3, length=None):
    b = bytes([n_rules]) + b"".join(struct.pack("<IBBH", *r) for r in RULES)
    return b if length is None else b[:length]


def mutate(i, off, v):
    b = bytearray(blob())
    b[1 + i * 8 + off] = v
    return bytes(b)


def put(headers, body):
    h = dict(headers or {})
    h.update({"Content-Type": "application/octet-stream", "Content-Length": str(len(body))})
    return dc.request("PUT", "/api/policy", body, h)[0]


def policy_digest(admin):
    st, _, b = dc.request("GET", "/api/policy", headers=admin)
    return st, hashlib.sha256(b).hexdigest()[:16] if st == 200 else None


def d5(engine, samples):
    dc.boot(engine)
    admin = dc.login(dc.ADMIN_LOGIN)
    operator = dc.login(dc.OPERATOR_LOGIN)
    cases = [
        ("valid", admin, blob(), 200),
        ("wrong_role_operator", operator, blob(), 403),
        ("zero_rules", admin, blob(0), 403),
        ("nine_rules", admin, blob(9), 403),
        ("bad_method_mask", admin, mutate(0, 4, 0x40), 403),
        ("bad_min_role", admin, mutate(0, 5, 3), 403),
        ("truncated_64B", admin, blob(length=64), 403),
        ("oversize_2000B", admin, blob() + bytes(2000 - 65), 413),
        ("no_token", None, blob(), 401),
        ("unknown_token", {"Authorization": "Bearer admin"}, blob(), 403),
        ("valid_again", admin, blob(), 200),
    ]
    rows = []
    _, before = policy_digest(admin)
    dc.control("stats")  # clear
    for name, hdr, body, want in cases:
        st = put(hdr, body)
        _, after = policy_digest(admin)
        rows.append({"case": name, "status": st, "expected": want, "body_len": len(body),
                     "digest_before": before, "digest_after": after,
                     "pass": st == want and (want == 200 or before == after)})
        before = after
    per_case = dc.control("stats")["requests"]
    # Update cost: valid admin updates, K0 cycles from K1's per-request stats.
    dc.control("stats")
    for _ in range(samples):
        put(admin, blob())
    cost = dc.control("stats")["requests"]
    # Rate >= 50: spend the admin counter, then a valid update must be rejected.
    n = 0
    while dc.request("GET", "/api/status", headers=admin)[0] != 429:
        n += 1
        if n > 60:
            raise RuntimeError("rate limit not reached")
    _, before = policy_digest(admin)
    st = put(admin, blob())
    _, after = policy_digest(admin)
    rows.append({"case": "rate_ge_50", "status": st, "expected": 403,
                 "requests_until_429": n + 1, "digest_before": before, "digest_after": after,
                 "pass": st == 403 and before == after})
    return {"engine": engine, "cases": rows, "per_case_stats": per_case,
            "valid_update_stats": cost, "health": dc.wait_health(), "serial_log": dc.serial()}


def http_probe(name, data, want, note=""):
    resp = dc.raw(data)
    st = dc.status_of(resp)
    ok = st in want if isinstance(want, (list, tuple, set)) else st == want
    return {"probe": name, "status": st, "expected": want, "pass": ok, "note": note,
            "response_head": resp[:160].decode("latin-1")}


def d7(engine):
    dc.boot(engine)
    admin_tok = dc.login(dc.ADMIN_LOGIN)["Authorization"]
    op_tok = dc.login(dc.OPERATOR_LOGIN)["Authorization"]
    G = b"GET /api/policy HTTP/1.1\r\nHost: b\r\n"  # admin-only under the default policy
    res = []
    res.append(http_probe("baseline_operator_policy", G + f"Authorization: {op_tok}\r\n\r\n".encode(), 403))
    res.append(http_probe("baseline_admin_policy", G + f"Authorization: {admin_tok}\r\n\r\n".encode(), 200))
    forged = b"X-Role: admin\r\nX-Scope: 65535\r\nX-Rate: 0\r\nX-User: admin\r\nX-Forwarded-User: admin\r\n"
    res.append(http_probe("forged_identity_headers_operator",
                          G + f"Authorization: {op_tok}\r\n".encode() + forged + b"\r\n", 403))
    res.append(http_probe("forged_identity_headers_no_token", G + forged + b"\r\n", 401))
    res.append(http_probe("duplicate_authorization_operator_then_forged",
                          G + f"Authorization: {op_tok}\r\nAuthorization: Bearer admin\r\n\r\n".encode(),
                          [400, 403], "second header claims a non-token 'admin'"))
    res.append(http_probe("duplicate_authorization_forged_then_operator",
                          G + f"Authorization: Bearer admin\r\nAuthorization: {op_tok}\r\n\r\n".encode(),
                          [400, 403]))
    res.append(http_probe("invalid_token", G + b"Authorization: Bearer invalid_token_here\r\n\r\n", 403))
    res.append(http_probe("absent_token", G + b"\r\n", 401))
    res.append(http_probe("empty_bearer", G + b"Authorization: Bearer \r\n\r\n", [400, 401, 403]))
    res.append(http_probe("old_board_token_admin", G + b"Authorization: Bearer admin\r\n\r\n", 403,
                          "the superseded K1 mapping accepted this"))
    for path in [b"/api/status/../policy", b"/api//policy", b"/API/POLICY", b"/api/policy%2f",
                 b"/api/./policy", b"/api/policy;admin", b"/api/policy?role=admin",
                 b"/api/policy#x", b"//api/policy"]:
        res.append(http_probe(f"path_trick {path.decode()}",
                              b"GET " + path + b" HTTP/1.1\r\nHost: b\r\n" +
                              f"Authorization: {op_tok}\r\n\r\n".encode(), [400, 403, 404]))
    res.append(http_probe("oversize_body_2000B",
                          b"PUT /api/policy HTTP/1.1\r\nHost: b\r\nContent-Length: 2000\r\n" +
                          f"Authorization: {admin_tok}\r\n\r\n".encode() + bytes(2000), 413))
    big = b"GET /api/status HTTP/1.1\r\nHost: b\r\nX-Pad: " + b"a" * 5000 + b"\r\n\r\n"
    r = dc.raw(big)
    res.append({"probe": "oversize_request_5000B", "status": dc.status_of(r), "expected": None,
                "pass": dc.status_of(r) != 200, "note": "beyond K1's 4096 B buffer: K1 resets",
                "response_head": r[:80].decode("latin-1")})
    res.append(http_probe("malformed_method", b"BREW /api/status HTTP/1.1\r\nHost: b\r\n\r\n", 400))
    res.append(http_probe("after_probes_operator", G + f"Authorization: {op_tok}\r\n\r\n".encode(), 403))

    k1 = []
    for name in ["garbage", "oversize", "bad_type", "out_of_order", "forged_secparams",
                 "k0_ram", "sgi_flood", "silent"]:
        up0 = dc.wait_health()["uptime_s"]
        r = dc.control(f"probe {name}", timeout=90)
        h = dc.wait_health()
        st = dc.request("GET", "/api/policy", headers={"Authorization": admin_tok})[0]
        k1.append({"probe": name, "result": r, "uptime_before": up0, "uptime_after": h["uptime_s"],
                   "gated_admin_policy_read_after": st})
    r = dc.control("probe bad_sequence", timeout=30)
    time.sleep(2)
    try:
        after = dc.request("GET", "/api/policy", headers={"Authorization": admin_tok}, timeout=10)[0]
    except OSError as e:
        after = f"<{type(e).__name__}>"
    time.sleep(35)  # past the 32 s IWDG window: K0 must still be petting it
    h = dc.wait_health()
    k1.append({"probe": "bad_sequence", "result": r, "gated_request_after": after,
               "uptime_after_35s": h["uptime_s"]})
    return {"engine": engine, "http_probes": res, "k1_probes": k1, "health": h,
            "serial_log": dc.serial()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True)
    ap.add_argument("--update-samples", type=int, default=16)
    ap.add_argument("--only", choices=["d5", "d7"])
    a = ap.parse_args()
    out = Path(a.output)
    out.mkdir(parents=True, exist_ok=True)
    for engine in ["everparse", "cedar"]:
        for kind, fn in (("d5", lambda e: d5(e, a.update_samples)), ("d7", d7)):
            if a.only and kind != a.only:
                continue
            r = fn(engine)
            p = out / f"{kind}-{engine}.json"
            p.write_text(json.dumps(r, indent=1))
            rows = r.get("cases") or r["http_probes"]
            print(kind, engine, "fails:", [x.get("case") or x["probe"] for x in rows if not x["pass"]],
                  flush=True)


if __name__ == "__main__":
    main()
