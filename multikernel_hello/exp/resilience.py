#!/usr/bin/env python3
"""Overload resilience run (studies/ccmp25-board/RESILIENCE_TEST.md).

Per image, per attack (R-A/R-B/R-C), per offered rate: a fresh boot, log in
for operator+admin tokens, fill the request templates, then for each rate
run loadgen (open-loop, one connection per request) while polling the board
counters on control port :81 once a second. A legitimate admin client runs
throughout loadgen (--legit-req). Per step: `dur` s of load then `recovery`
s idle, with the legitimate client still running during recovery.

SAFETY: without --direct-link this refuses any rate above 1000 req/s and
keeps loadgen's own ceilings (it will not exceed --max-mbps, and stops
within --dead-ms of the board going quiet). The lab-switch dry runs use
--max-rate 1000. High rates need the direct cable and --direct-link.
"""
import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

import dcommon as dc

HERE = Path(__file__).resolve().parent
LOAD = HERE / "load"
REQ = LOAD / "requests"
ATTACKS = {
    "R-A": {"req": "r_a_anonymous.http", "expect": 401, "desc": "anonymous, no token"},
    "R-A-inv": {"req": "r_a_invalid.http", "expect": 403, "desc": "invalid token"},
    "R-B": {"tmpl": "r_b_operator.tmpl", "token": "operator", "expect": 403,
            "desc": "operator token, engine-denied /api/reports"},
    "R-C": {"req": "r_c_malformed.http", "expect": 400, "desc": "malformed method"},
}


def fill(tmpl, token, out):
    out.write_bytes(REQ.joinpath(tmpl).read_bytes().replace(b"TOKEN", token.encode()))


def tokens():
    a = dc.login(dc.ADMIN_LOGIN)["Authorization"].split()[1]
    o = dc.login(dc.OPERATOR_LOGIN)["Authorization"].split()[1]
    return a, o


def run_rate(a, attack, rate, dur, recovery, work, direct, max_mbps, threads):
    reqfile = work / f"req-{attack}.http"
    legitfile = work / "req-legit.http"
    cmd = [str(LOAD / "loadgen"), "--host", dc.BOARD, "--port", "80",
           "--req", str(reqfile), "--expect", str(ATTACKS[attack]["expect"]),
           "--rate", str(rate), "--dur", str(dur), "--threads", str(threads),
           "--legit-req", str(legitfile), "--legit-expect", "200",
           "--max-mbps", str(max_mbps), "--dead-ms", "1000"]
    if direct:
        cmd.append("--direct-link")
    ctr = []
    t_end = time.time() + dur + recovery + 3
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    while time.time() < t_end:
        try:
            ctr.append({"wall": time.time(), **dc.control("counters", timeout=3)})
        except (OSError, ValueError):
            ctr.append({"wall": time.time(), "error": True})
        if proc.poll() is not None and time.time() > t_end - recovery:
            pass
        time.sleep(1)
        if proc.poll() is not None and time.time() - (t_end - recovery - dur) > dur + recovery:
            break
    try:
        out, err = proc.communicate(timeout=30)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
    digest = dc.request("GET", "/api/policy", headers={"Authorization": "Bearer " + a})
    return {"rate": rate, "cmd": cmd, "stderr": err,
            "loadgen": json.loads(out) if out.strip().startswith("{") else {"raw": out},
            "board_counters": ctr, "policy_status_after": digest[0],
            "policy_digest_after": __import__("hashlib").sha256(digest[2]).hexdigest()[:16]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--output", required=True)
    ap.add_argument("--images", nargs="+", default=["raw", "gateonly", "everparse", "cedar"])
    ap.add_argument("--attacks", nargs="+", default=list(ATTACKS))
    ap.add_argument("--rates", nargs="+", type=int, default=[500, 1000])
    ap.add_argument("--dur", type=int, default=60)
    ap.add_argument("--recovery", type=int, default=30)
    ap.add_argument("--threads", type=int, default=16)
    ap.add_argument("--max-mbps", type=float, default=40.0)
    ap.add_argument("--max-rate", type=int, default=1000, help="hard cap enforced here too")
    ap.add_argument("--direct-link", action="store_true")
    a = ap.parse_args()
    if not a.direct_link and max(a.rates) > a.max_rate:
        sys.exit(f"refusing rates above {a.max_rate} without --direct-link (shared lab LAN)")
    out = Path(a.output)
    out.mkdir(parents=True, exist_ok=True)
    work = out / "work"
    work.mkdir(exist_ok=True)
    manifest = {"images": a.images, "attacks": a.attacks, "rates": a.rates, "dur": a.dur,
                "recovery": a.recovery, "direct_link": a.direct_link, "runs": []}
    for image in a.images:
        dc.boot(image)
        admin, operator = tokens()
        if image != "raw":
            dc.install_demo_policy = True
            import struct
            rules = [(0x33333333, 0x01, 1, 0x0001), (0x22222222, 0x02, 1, 0x0000),
                     (0x44444444, 0x01, 2, 0x0004)] + [(0xDEADDEAD, 0, 0, 0)] * 5
            blob = bytes([3]) + b"".join(struct.pack("<IBBH", *r) for r in rules)
            st = dc.request("PUT", "/api/policy", blob,
                            {"Authorization": "Bearer " + admin,
                             "Content-Type": "application/octet-stream",
                             "Content-Length": str(len(blob))})[0]
            if st != 200:
                raise RuntimeError(f"policy install HTTP {st}")
        fill("legit_admin.tmpl", admin, work / "req-legit.http")
        for attack in a.attacks:
            spec = ATTACKS[attack]
            if "tmpl" in spec:
                tok = operator if spec["token"] == "operator" else admin
                fill(spec["tmpl"], tok, work / f"req-{attack}.http")
            else:
                (work / f"req-{attack}.http").write_bytes(REQ.joinpath(spec["req"]).read_bytes())
            for rate in a.rates:
                r = run_rate(admin, attack, rate, a.dur, a.recovery, work,
                             a.direct_link, a.max_mbps, a.threads)
                name = f"{image}-{attack}-{rate}.json"
                (out / name).write_text(json.dumps(r, indent=1))
                tot = r["loadgen"].get("totals", {})
                print(f"{image} {attack} {rate}/s: completed={tot.get('completed')} "
                      f"wrong_allow={tot.get('wrong_allow_2xx')} "
                      f"policy={r['policy_digest_after']}", flush=True)
                manifest["runs"].append({"image": image, "attack": attack, "rate": rate,
                                         "file": name})
                (out / "manifest.json").write_text(json.dumps(manifest, indent=1))
    manifest["status"] = "complete"
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1))


if __name__ == "__main__":
    main()
