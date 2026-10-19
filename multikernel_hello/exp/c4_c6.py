#!/usr/bin/env python3
"""C4 (policy updates) and C6 (compromised-K1 probes) on the CCMP25 multikernel.

One fresh boot per engine for C4 and another for C6 (bad_sequence wedges the
channel, so it runs last). Expected update outcomes follow the paper's
D_update (validate_policy_update_fields + body checks in test_equivalence.c).
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import time
from pathlib import Path

import campaign as cp

VALID = [(0x33333333, 0x01, 1, 0x0001), (0x22222222, 0x02, 1, 0x0000),
         (0x44444444, 0x01, 2, 0x0004)] + [(0xDEADDEAD, 0, 0, 0)] * 5


def body(n_rules=3, rules=VALID, length=None):
    b = bytes([n_rules]) + b"".join(struct.pack("<IBBH", *r) for r in rules)
    return b if length is None else b[:length]


def mutate(rule_index, field_offset, value):
    b = bytearray(body())
    b[1 + rule_index * 8 + field_offset] = value
    return bytes(b)


C4_CASES = [
    ("valid", "admin", 0, body(), 200),
    ("wrong_role", "operator", 0, body(), 403),
    ("rate_ge_50", "admin", 50, body(), 403),
    ("zero_rules", "admin", 0, body(0), 403),
    ("nine_rules", "admin", 0, body(9), 403),
    ("bad_method_mask", "admin", 0, mutate(0, 4, 0x40), 403),
    ("bad_min_role", "admin", 0, mutate(0, 5, 3), 403),
    ("truncated_64B", "admin", 0, body(length=64), 403),
    ("valid_again", "admin", 0, body(), 200),
]


def put_policy(role, rate, blob):
    h = {"Authorization": f"Bearer {role}", "Content-Type": "application/octet-stream",
         "Content-Length": str(len(blob))}
    if rate:
        h["X-Rate"] = str(rate)
    st, hdr, _ = cp.request("PUT", "/api/policy", blob, h)
    return st, int(hdr.get("X-Engine-Cycles", 0)), hdr.get("X-Policy-Digest")


def digest():
    st, hdr, _ = cp.request("GET", "/api/policy", headers=cp.ADMIN)
    return st, hdr.get("X-Policy-Digest")


def boot(engine):
    subprocess.run([str(cp.ROOT / "deploy/boot_image.sh"), f"sel4-{engine}.bin"], check=True,
                   stdout=subprocess.DEVNULL)
    log = cp.wait_serial(r"\[K0\] tick \d+ engine=\w+", 300)
    assert set(re.findall(r"\[K0\] tick \d+ engine=(\w+)", log)) == {engine}
    cp.wait_serial(r"phy: link up", 60)
    cp.wait_health()


def c4(engine, update_samples):
    boot(engine)
    rows = []
    for name, role, rate, blob, want in C4_CASES:
        before = digest()[1]
        st, cyc, dig = put_policy(role, rate, blob)
        after = digest()[1]
        rows.append({"case": name, "role": role, "rate": rate, "body_len": len(blob),
                     "status": st, "expected": want, "engine_cycles": cyc,
                     "digest_before": before, "digest_after": after,
                     "pass": st == want and ((before == after) if want != 200 else True)})
    cost = [put_policy("admin", 0, body())[1] for _ in range(update_samples)]
    return {"engine": engine, "cases": rows, "valid_update_cycles": cost,
            "serial_log": cp.serial()}


def c6(engine):
    boot(engine)
    res = []
    for name in ["forged_role", "forged_scope", "oversize", "truncated", "bad_type",
                 "k0_ram", "sgi_flood", "silent"]:
        up0 = cp.wait_health()["uptime_s"]
        st, _, b = cp.request("GET", f"/probe/{name}", timeout=90)
        h = cp.wait_health()
        res.append({"probe": name, "http": st, "result": json.loads(b),
                    "uptime_before": up0, "uptime_after": h["uptime_s"],
                    "board_survived": h["uptime_s"] >= up0})
    st, _, b = cp.request("GET", "/probe/bad_sequence", timeout=30)
    time.sleep(2)
    after = cp.request("GET", "/api/status", headers=cp.ADMIN, timeout=10)[0]
    time.sleep(35)  # past the 32 s IWDG window: K0 must still be petting it
    h = cp.wait_health()
    res.append({"probe": "bad_sequence", "http": st, "result": json.loads(b),
                "gated_request_after": after, "uptime_after_35s": h["uptime_s"]})
    return {"engine": engine, "probes": res, "serial_log": cp.serial()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True)
    ap.add_argument("--update-samples", type=int, default=32)
    a = ap.parse_args()
    out = Path(a.output)
    out.mkdir(parents=True, exist_ok=True)
    for engine in ["everparse", "cedar"]:
        for kind, fn in (("c4", lambda e: c4(e, a.update_samples)), ("c6", c6)):
            r = fn(engine)
            p = out / f"{kind}-{engine}.json"
            p.write_text(json.dumps(r, indent=1))
            print(kind, engine, hashlib.sha256(p.read_bytes()).hexdigest()[:16], flush=True)


if __name__ == "__main__":
    main()
