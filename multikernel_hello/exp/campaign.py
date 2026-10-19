#!/usr/bin/env python3
"""C0/C2/C3 campaign on the CCMP25 multikernel (paper experiments).

Per block, every engine image (none, everparse, cedar) gets a fresh boot
(deploy/boot_image.sh: NRST reset + U-Boot fatload/bootm). Engine order is
balanced over blocks (all 6 permutations in rotation). Per boot:
  - serial log captured (C2_RESULT lines = decision cost, EQUIVALENCE line);
  - /health checked from the host, then the x86 D1 demo policy is installed;
  - allow (GET /api/status, admin) and deny (GET /api/demo, admin):
    4 warmup + 32 timed requests each, one TCP connection per request
    (as scripts/http_benchmark.py on x86), scenario order alternating.
Writes one JSON per boot plus manifest.json; never touches host network config.
"""
import argparse
import datetime
import hashlib
import http.client
import itertools
import json
import os
import re
import struct
import subprocess
import sys
import time
from pathlib import Path

BOARD = "192.168.40.129"
ROOT = Path(__file__).resolve().parents[2]  # sel4_ccwmp25
ENGINES = ["none", "everparse", "cedar"]
PERMS = list(itertools.permutations(ENGINES))
ADMIN = {"Authorization": "Bearer admin"}
DEMO_RULES = [(0x33333333, 0x01, 1, 0x0001), (0x22222222, 0x02, 1, 0x0000),
              (0x44444444, 0x01, 2, 0x0004)] + [(0xDEADDEAD, 0, 0, 0)] * 5


def request(method, path, body=None, headers=None, timeout=5):
    c = http.client.HTTPConnection(BOARD, 80, timeout=timeout)
    try:
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        return r.status, dict(r.getheaders()), r.read()
    finally:
        c.close()


def serial():
    """Console text of the current boot only (after the last 'Starting kernel').
    -J joins lines that tmux wrapped at the pane width."""
    s = subprocess.run(["tmux", "capture-pane", "-p", "-J", "-t", "mp255", "-S", "-5000"],
                       capture_output=True, text=True).stdout
    i = s.rfind("Starting kernel")
    return s[i:] if i >= 0 else ""


def wait_serial(pattern, timeout):
    end = time.time() + timeout
    while time.time() < end:
        s = serial()
        if re.search(pattern, s):
            return s
        time.sleep(2)
    raise TimeoutError(pattern)


def wait_health(timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        try:
            st, _, b = request("GET", "/health", timeout=2)
            if st == 200:
                return json.loads(b)
        except OSError:
            pass
        time.sleep(1)
    raise TimeoutError("health")


def run_boot(engine, scen_order, samples, warmup):
    subprocess.run([str(ROOT / "deploy/boot_image.sh"), f"sel4-{engine}.bin"], check=True,
                   stdout=subprocess.DEVNULL)
    log = wait_serial(r"\[K0\] tick \d+ engine=\w+", 300)
    seen = set(re.findall(r"\[K0\] tick \d+ engine=(\w+)", log))
    if seen != {engine}:
        raise RuntimeError(f"wrong image booted (expected {engine}, saw {seen})")
    wait_serial(r"phy: link up", 60)
    health0 = wait_health()
    blob = bytes([3]) + b"".join(struct.pack("<IBBH", *r) for r in DEMO_RULES)
    st, hdr, _ = request("PUT", "/api/policy", blob,
                         dict(ADMIN, **{"Content-Type": "application/octet-stream",
                                        "Content-Length": str(len(blob))}))
    if st != 200:
        raise RuntimeError(f"policy install HTTP {st}")
    out = {"engine": engine, "health_before": health0,
           "policy_install": {"status": st, "engine_cycles": int(hdr.get("X-Engine-Cycles", 0)),
                              "digest": hdr.get("X-Policy-Digest")},
           "scenarios": {}}
    for scen in scen_order:
        path, want = ("/api/status", 200) if scen == "allow" else ("/api/demo", 403)
        if engine == "none":
            want = 200  # C0: no ACL, the same request is passed through
        for _ in range(warmup):
            if request("GET", path, headers=ADMIN)[0] != want:
                raise RuntimeError(f"{scen} warmup status")
        ns, eng, chan = [], [], []
        for _ in range(samples):
            t = time.perf_counter_ns()
            st, h, _ = request("GET", path, headers=ADMIN)
            ns.append(time.perf_counter_ns() - t)
            if st != want:
                raise RuntimeError(f"{scen} status {st}")
            eng.append(int(h.get("X-Engine-Cycles", 0)))
            chan.append(int(h.get("X-Channel-Cycles", 0)))
        out["scenarios"][scen] = {"status": want, "latency_ns": ns,
                                  "engine_cycles": eng, "k1_channel_cycles": chan}
    out["health_after"] = wait_health()
    out["serial_log"] = serial()
    out["c2"] = [dict(f.split("=", 1) for f in l.split("C2_RESULT,", 1)[1].split(",") if "=" in f)
                 for l in out["serial_log"].splitlines() if "C2_RESULT," in l]
    out["equivalence"] = [l.split("] ", 1)[-1] for l in out["serial_log"].splitlines()
                          if "EQUIVALENCE_" in l]
    return out


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
        "topology": "two seL4 kernels: K1 NIC/lwIP/HTTP, K0 engine+backend, mk_channel",
        "client": {"host": "192.168.40.1 enp44s0", "board": BOARD,
                   "affinity": sorted(os.sched_getaffinity(0)),
                   "uname": os.uname()._asdict() if hasattr(os.uname(), "_asdict") else str(os.uname())},
        "started_utc": datetime.datetime.now(datetime.timezone.utc).isoformat(),
        "blocks": a.blocks, "samples": a.samples, "warmup": a.warmup, "runs": []}
    done = {(r["block"], r["engine"]) for r in man["runs"]}
    for block in range(a.blocks):
        order = PERMS[block % len(PERMS)]
        scen_order = ["allow", "deny"] if block % 2 == 0 else ["deny", "allow"]
        for engine in order:
            if (block, engine) in done:
                continue
            name = f"{block:02}-{engine}.json"
            for attempt in range(3):
                try:
                    r = run_boot(engine, scen_order, a.samples, a.warmup)
                    break
                except Exception as e:  # recorded, retried with a fresh boot
                    man.setdefault("retries", []).append(
                        {"block": block, "engine": engine, "attempt": attempt, "error": repr(e)})
                    man_path.write_text(json.dumps(man, indent=1))
            else:
                raise RuntimeError(f"block {block} engine {engine} failed 3 times")
            r.update({"block": block, "order": list(order), "scenario_order": scen_order,
                      "utc": datetime.datetime.now(datetime.timezone.utc).isoformat()})
            (out / name).write_text(json.dumps(r))
            man["runs"].append({"block": block, "engine": engine, "file": name,
                                "sha256": hashlib.sha256((out / name).read_bytes()).hexdigest()})
            man_path.write_text(json.dumps(man, indent=1))
            print(f"block {block} {engine} ok", flush=True)
    man["status"] = "complete"
    man["finished_utc"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    man_path.write_text(json.dumps(man, indent=1))


if __name__ == "__main__":
    main()
