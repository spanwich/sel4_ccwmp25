#!/usr/bin/env python3
"""Summarise the corrected-design evidence directory into summary.json and
TABLES.md, and write SHA256SUMS over every file in it.

D4 (and D0a/D0b): per image x scenario, the median of block medians of the
host-observed latency (min/max of block medians too).
D3 whole path: per image x scenario, the median over boots of the boot-mean
K0 cycles (queue in -> response out; and inside policy_gate, authenticator,
protected_app). D3 engine-only: per engine x case, median over boots of the
boot mean of engine_decide cycles (C2_RESULT). D2: boots with
EQUIVALENCE_OK (12,527) and EXTRACTOR_OK. Cycle counts at 1.2 GHz.
"""
import hashlib
import json
import statistics as st
import sys
from pathlib import Path

HZ = 1.2e9
X86 = {  # papers/.../evidence/http-x86-d1/REPORT.md (matched D1, QEMU/KVM x86)
    "http_ms": {"everparse/allow": 0.680, "everparse/deny": 0.684,
                "cedar/allow": 0.715, "cedar/deny": 0.716},
    "gate_api_ns": {"everparse/allow_first": 86.7, "everparse/allow_last": 78.0,
                    "everparse/deny_path": 83.7, "cedar/allow_first": 35393.0,
                    "cedar/allow_last": 36508.0, "cedar/deny_path": 32832.0},
}
ORDER = ["raw", "gateonly", "everparse", "cedar"]
LABEL = {"raw": "raw (D0a: fixed 200, no gate)", "gateonly": "gateonly (D0b: gate, accept-all)",
         "everparse": "EverParse", "cedar": "Cedar"}


def med(x):
    return st.median(x) if x else None


def mean(x):
    return sum(x) / len(x) if x else None


def d4(ev):
    d = ev / "d4"
    man = json.loads((d / "manifest.json").read_text())
    runs = [json.loads((d / r["file"]).read_text()) for r in man["runs"]]
    out = {"status": man.get("status"), "boots": len(runs), "retries": man.get("retries", []),
           "http": {}, "k0_path": {}, "engine_only": {}, "d2": {}}
    for e in ORDER:
        er = [r for r in runs if r["engine"] == e]
        if not er:
            continue
        for scen in ["allow", "deny"]:
            blocks = {}
            k0, gate, auth, app, k1 = [], [], [], [], []
            for r in er:
                s = r["scenarios"].get(scen)
                if not s:
                    continue
                blocks.setdefault(r["block"], []).extend(s["latency_ns"])
                k0.append(mean([x["k0"] for x in s["k0"]]))
                gate.append(mean([x["gate"] for x in s["k0"]]))
                auth.append(mean([x["auth"] for x in s["k0"]]))
                app.append(mean([x["app"] for x in s["k0"]]))
                k1.append(mean([x["k1"] for x in s["k0"]]))
            bm = [med(v) for v in blocks.values()]
            out["http"][f"{e}/{scen}"] = {
                "status": er[0]["scenarios"].get(scen, er[-1]["scenarios"].get(scen, {})).get("status"),
                "blocks": len(bm), "samples": sum(len(v) for v in blocks.values()),
                "median_of_block_medians_us": med(bm) / 1e3,
                "block_median_min_us": min(bm) / 1e3, "block_median_max_us": max(bm) / 1e3}
            out["k0_path"][f"{e}/{scen}"] = {
                "boots": len(k0), "k0_cycles": med(k0), "k0_us": med(k0) / HZ * 1e6,
                "gate_cycles": med(gate), "auth_cycles": med(auth), "app_cycles": med(app),
                "k1_busy_cycles": med(k1)}
        cases = {}
        for r in er:
            for c in r["c2"]:
                if "case" in c and "mean" in c:
                    cases.setdefault(c["case"], []).append(c)
        for case, cs in cases.items():
            means = [int(c["mean"]) for c in cs]
            out["engine_only"][f"{e}/{case}"] = {
                "boots": len(cs), "mismatches": sum(int(c.get("mismatches", 0)) for c in cs),
                "median_boot_mean_cycles": med(means), "median_boot_mean_ns": med(means) / HZ * 1e9,
                "boot_mean_min_cycles": min(means), "boot_mean_max_cycles": max(means)}
        if e in ("everparse", "cedar"):
            out["d2"][e] = {
                "boots": len(er),
                "equivalence_ok": sum(any("EQUIVALENCE_OK" in x and "comparisons=12527" in x
                                          for x in r["equivalence"]) for r in er),
                "extractor_ok": sum(any("EXTRACTOR_OK" in x for x in r["extractor"]) for r in er)}
    return out


def fmt(x, nd=1):
    return "-" if x is None else f"{x:,.{nd}f}"


def tables(s, fp, d1, d5, d7):
    L = ["## D4 / D0a / D0b: end-to-end HTTP, host to board and back "
         "(median of 30 block medians, µs)\n",
         "| Image | Allow | Deny | Allow block-median range | Deny block-median range | Blocks | x86 D1 allow / deny |",
         "|---|---:|---:|---|---|---:|---|"]
    for e in ORDER:
        a, d = s["http"].get(f"{e}/allow"), s["http"].get(f"{e}/deny")
        if not a:
            continue
        x = (f"{X86['http_ms'][e + '/allow'] * 1e3:.0f} / {X86['http_ms'][e + '/deny'] * 1e3:.0f}"
             if e + "/allow" in X86["http_ms"] else "-")
        L.append(f"| {LABEL[e]} | {fmt(a['median_of_block_medians_us'])} ({a['status']}) | "
                 f"{fmt(d['median_of_block_medians_us'])} ({d['status']}) | "
                 f"{fmt(a['block_median_min_us'])}–{fmt(a['block_median_max_us'])} | "
                 f"{fmt(d['block_median_min_us'])}–{fmt(d['block_median_max_us'])} | {a['blocks']} | {x} |")
    L += ["\n## D3: whole K0 path per request (PMU cycles, median over boots of the boot mean)\n",
          "Queue in to response out on K0 (wall time; K0 does not WFI while serving). "
          "Inside: time spent in each PD's notified(). K1 busy = K1 CPU cycles during the "
          "exchange (PMU stops in WFI, so not wall time).\n",
          "| Image / scenario | K0 path cycles | µs | policy_gate | authenticator | protected_app | K1 busy | Boots |",
          "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for e in ORDER:
        for scen in ["allow", "deny"]:
            v = s["k0_path"].get(f"{e}/{scen}")
            if v:
                L.append(f"| {e} / {scen} | {fmt(v['k0_cycles'], 0)} | {fmt(v['k0_us'], 2)} | "
                         f"{fmt(v['gate_cycles'], 0)} | {fmt(v['auth_cycles'], 0)} | "
                         f"{fmt(v['app_cycles'], 0)} | {fmt(v['k1_busy_cycles'], 0)} | {v['boots']} |")
    L += ["\n## D3: engine_decide only (median over boots of the boot mean; 1,000 samples after 100 warmup)\n",
          "| Engine | Case | Cycles | ns @1.2 GHz | Boot-mean range (cycles) | Boots | Mismatches | x86 D1 ns |",
          "|---|---|---:|---:|---|---:|---:|---:|"]
    for k, v in sorted(s["engine_only"].items()):
        e, c = k.split("/")
        L.append(f"| {e} | {c} | {fmt(v['median_boot_mean_cycles'], 0)} | {fmt(v['median_boot_mean_ns'], 0)} | "
                 f"{v['boot_mean_min_cycles']:,}–{v['boot_mean_max_cycles']:,} | {v['boots']} | "
                 f"{v['mismatches']} | {fmt(X86['gate_api_ns'].get(k), 1)} |")
    L += ["\n## D2: on-board equivalence and F* extractor vectors (every D4 boot)\n",
          "| Engine | Boots | EQUIVALENCE_OK (12,527 comparisons) | EXTRACTOR_OK (28/28 vectors) |",
          "|---|---:|---:|---:|"]
    for e, v in s["d2"].items():
        L.append(f"| {e} | {v['boots']} | {v['equivalence_ok']} | {v['extractor_ok']} |")
    L += ["\n## D1: x86 scripts/demo_test.py against 192.168.40.129:80\n",
          "| Engine | Result | rate limit at | connection pressure |", "|---|---|---:|---|"]
    for e, t in d1.items():
        rl = next((l.split("limited_at=")[1].split(",")[0] for l in t.splitlines() if "rate_limit," in l), "-")
        cp = next((l.split("connection_pressure,")[1] for l in t.splitlines() if "connection_pressure," in l), "-")
        L.append(f"| {e} | {'HTTP_DEMO_PASS' if 'HTTP_DEMO_PASS' in t else 'FAIL'} | {rl} | {cp} |")
    L += ["\n## D5: policy updates via PUT /api/policy (real tokens)\n",
          "| Engine | Case | HTTP | Expected | Digest unchanged on reject | Pass |", "|---|---|---:|---:|---|---|"]
    for e, r in d5.items():
        for c in r["cases"]:
            unch = "-" if c["expected"] == 200 else str(c["digest_before"] == c["digest_after"])
            L.append(f"| {e} | {c['case']} | {c['status']} | {c['expected']} | {unch} | {c['pass']} |")
        g = sorted(x["gate"] for x in r["valid_update_stats"])
        k = sorted(x["k0"] for x in r["valid_update_stats"])
        L.append(f"| {e} | valid update, median of {len(g)}: policy_gate {g[len(g) // 2]:,} cycles, "
                 f"K0 path {k[len(k) // 2]:,} cycles | | | | |")
    L += ["\n## D7: compromised-K1 probes\n", "HTTP probes (sent by the host through K1, as a "
          "compromised K1 could send them):\n", "| Engine | Probe | HTTP | Expected | Pass |",
          "|---|---|---:|---|---|"]
    for e, r in d7.items():
        for p in r["http_probes"]:
            L.append(f"| {e} | {p['probe']} | {p['status']} | {p['expected']} | {p['pass']} |")
    L += ["\nK1-side probes (K1 user code misbehaving on the channel), each followed by a "
          "gated admin `GET /api/policy` from the host:\n",
          "| Engine | Probe | Result | Admin request after |", "|---|---|---|---|"]
    for e, r in d7.items():
        for p in r["k1_probes"]:
            res = {k: v for k, v in p["result"].items() if k != "probe"}
            after = p.get("gated_admin_policy_read_after", p.get("gated_request_after"))
            extra = f", uptime after 35 s: {p['uptime_after_35s']} s" if "uptime_after_35s" in p else ""
            L.append(f"| {e} | {p['probe']} | `{json.dumps(res)}` | {after}{extra} |")
    L += ["\n## D6: footprint (bytes)\n",
          "| Image | text+rodata | data | bss total | of which K0 heap arena | musl morecore | image |",
          "|---|---:|---:|---:|---:|---:|---:|"]
    for r in fp["k0"]:
        b = r["bss_large_symbols"]
        L.append(f"| K0 {r['engine']} | {r['text_rodata']:,} | {r['data']:,} | {r['bss_total']:,} | "
                 f"{b.get('arena', 0):,} | {b.get('morecore_area', 0):,} | {r['image_bytes']:,} |")
    k1 = fp["k1"]
    L.append(f"| K1 (all images) | {k1['text_rodata']:,} | {k1['data']:,} | {k1['bss_total']:,} | - | "
             f"{k1['bss_large_symbols'].get('morecore_area', 0):,} | (bundled) |")
    L += ["\nK0 bss includes the 8 MiB root stack (Sel4RuntimeRootStack).\n",
          "Per component (object files before linking, EverParse build; `size -B`):\n",
          "| Component | text | data | bss |", "|---|---:|---:|---:|"]
    for k, v in next(r for r in fp["k0"] if r["engine"] == "everparse")["components"].items():
        if v["objects"]:
            L.append(f"| {k} | {v['text']:,} | {v['data']:,} | {v['bss']:,} |")
    c = fp["cedar_engine_linked_delta_vs_gateonly"]
    L.append(f"| Cedar engine (linked delta vs gateonly; {c['note']}) | {c['text_rodata']:,} | {c['data']:,} | - |")
    return "\n".join(L) + "\n"


def main():
    ev = Path(sys.argv[1])
    s = d4(ev)
    (ev / "summary.json").write_text(json.dumps(s, indent=1) + "\n")
    fp = json.loads((ev / "d6/footprint.json").read_text())
    d1 = {e: (ev / f"d1/d1-{e}.log").read_text() for e in ["everparse", "cedar"]}
    d5 = {e: json.loads((ev / f"d5d7/d5-{e}.json").read_text()) for e in ["everparse", "cedar"]}
    d7 = {e: json.loads((ev / f"d5d7/d7-{e}.json").read_text()) for e in ["everparse", "cedar"]}
    t = tables(s, fp, d1, d5, d7)
    (ev / "TABLES.md").write_text(t)
    files = sorted(p for p in ev.rglob("*") if p.is_file() and p.name != "SHA256SUMS")
    (ev / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(ev)}\n" for p in files))
    print(t)


if __name__ == "__main__":
    main()
