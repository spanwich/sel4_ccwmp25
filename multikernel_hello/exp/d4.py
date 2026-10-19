#!/usr/bin/env python3
"""D4 (end-to-end HTTP, with D0a/D0b baselines), D2 and D3 on every boot.

Per block, every unit gets a fresh boot (deploy/boot_image.sh):
  raw       (D0a) one boot, both scenarios: K0 answers a fixed 200 unread;
  gateonly  (D0b) one boot per scenario: full gate path, accept-all engine;
  everparse, cedar  one boot per scenario.
One boot per gated scenario because the verified rate counter is per role
and never resets (x86 http_benchmark.py likewise boots per scenario:
login, demo-policy install, 4 warmup + 32 timed <= 44 < MAX_RATE).
Engine order rotates through all 24 permutations; scenario order alternates.
Requests as x86 http_benchmark.py: allow = GET /api/status, deny =
GET /api/demo, admin token from POST /api/login, one TCP connection each.
Per request, K0 PMU cycles (whole K0 path, and inside gate/auth/app) come
from K1's control port after the timed loop (outside the timed region).
Serial (C2_RESULT, EQUIVALENCE_*, D2_EXTRACTOR) is read after each boot.
"""
import argparse
import datetime
import hashlib
import itertools
import json
import os
import re
import struct
import time
from pathlib import Path

import dcommon as dc

ENGINES = ["raw", "gateonly", "everparse", "cedar"]
PERMS = list(itertools.permutations(ENGINES))
DEMO_RULES = [(0x33333333, 0x01, 1, 0x0001), (0x22222222, 0x02, 1, 0x0000),
              (0x44444444, 0x01, 2, 0x0004)] + [(0xDEADDEAD, 0, 0, 0)] * 5
PATH = {"allow": "/api/status", "deny": "/api/demo"}


def expected(engine, scen):
    return 403 if scen == "deny" and engine in ("everparse", "cedar") else 200


def run_boot(engine, scens, samples, warmup):
    health0 = dc.boot(engine)
    out = {"engine": engine, "scenarios_run": scens, "health_before": health0, "scenarios": {}}
    if engine == "raw":
        headers = {"Authorization": "Bearer admin.demo.token"}  # same bytes, unread by K0
    else:
        headers = dc.login(dc.ADMIN_LOGIN)
        blob = bytes([3]) + b"".join(struct.pack("<IBBH", *r) for r in DEMO_RULES)
        h = dict(headers, **{"Content-Type": "application/octet-stream",
                             "Content-Length": str(len(blob))})
        st = dc.request("PUT", "/api/policy", blob, h)[0]
        if st != 200:
            raise RuntimeError(f"policy install HTTP {st}")
    for scen in scens:
        want = expected(engine, scen)
        for _ in range(warmup):
            if dc.request("GET", PATH[scen], headers=headers)[0] != want:
                raise RuntimeError(f"{scen} warmup status")
        dc.control("stats")  # clear: only timed requests below
        ns = []
        for _ in range(samples):
            t = time.perf_counter_ns()
            st = dc.request("GET", PATH[scen], headers=headers)[0]
            ns.append(time.perf_counter_ns() - t)
            if st != want:
                raise RuntimeError(f"{scen} status {st}")
        stats = dc.control("stats")["requests"]
        if len(stats) != samples or any(s["status"] != want for s in stats):
            raise RuntimeError(f"{scen}: stats mismatch ({len(stats)} entries)")
        out["scenarios"][scen] = {"status": want, "latency_ns": ns, "k0": stats}
    out["health_after"] = dc.wait_health()
    log = dc.serial()
    out["serial_log"] = log
    out["c2"] = [dict(f.split("=", 1) for f in l.split("C2_RESULT,", 1)[1].split(",") if "=" in f)
                 for l in log.splitlines() if "C2_RESULT," in l]
    out["equivalence"] = [l.split("] ", 1)[-1] for l in log.splitlines() if "EQUIVALENCE_" in l]
    out["extractor"] = [l.split("] ", 1)[-1] for l in log.splitlines() if "D2_EXTRACTOR" in l]
    return out


def units(block):
    order = PERMS[block % len(PERMS)]
    scen = ["allow", "deny"] if block % 2 == 0 else ["deny", "allow"]
    for e in order:
        if e == "raw":
            yield e, scen
        else:
            for s in scen:
                yield e, [s]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True)
    ap.add_argument("--blocks", type=int, default=30)
    ap.add_argument("--samples", type=int, default=32)
    ap.add_argument("--warmup", type=int, default=4)
    a = ap.parse_args()
    out = Path(a.output)
    out.mkdir(parents=True, exist_ok=True)
    man_path = out / "manifest.json"
    man = json.loads(man_path.read_text()) if man_path.exists() else {
        "schema": 1, "claim": "host_observed_not_wcet", "board": "CCMP25-DVK STM32MP255",
        "design": "corrected: K1 raw forwarder; K0 x86 D1 gate pipeline (CORRECTED_DESIGN.md)",
        "client": {"host": "192.168.40.1 enp44s0", "board": dc.BOARD,
                   "affinity": sorted(os.sched_getaffinity(0)), "uname": list(os.uname())},
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "blocks": a.blocks, "samples": a.samples, "warmup": a.warmup, "runs": []}
    done = {(r["block"], r["engine"], tuple(r["scenarios"])) for r in man["runs"]}
    for block in range(a.blocks):
        for engine, scens in units(block):
            if (block, engine, tuple(scens)) in done:
                continue
            name = f"{block:02}-{engine}-{'+'.join(scens)}.json"
            for attempt in range(3):
                try:
                    r = run_boot(engine, scens, a.samples, a.warmup)
                    break
                except Exception as e:  # recorded, retried with a fresh boot
                    man.setdefault("retries", []).append(
                        {"block": block, "engine": engine, "scenarios": scens,
                         "attempt": attempt, "error": repr(e)})
                    man_path.write_text(json.dumps(man, indent=1))
            else:
                raise RuntimeError(f"block {block} {engine} {scens} failed 3 times")
            r.update({"block": block, "order": list(PERMS[block % len(PERMS)]),
                      "utc": datetime.datetime.now(datetime.timezone.utc).isoformat()})
            (out / name).write_text(json.dumps(r))
            man["runs"].append({"block": block, "engine": engine, "scenarios": scens, "file": name,
                                "sha256": hashlib.sha256((out / name).read_bytes()).hexdigest()})
            man_path.write_text(json.dumps(man, indent=1))
            print(f"block {block} {engine} {scens} ok", flush=True)
    man["status"] = "complete"
    man["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    man_path.write_text(json.dumps(man, indent=1))


if __name__ == "__main__":
    main()
