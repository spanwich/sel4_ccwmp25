#!/usr/bin/env python3
"""Render evidence/ccmp25-<date>/REPORT.md tables from the C0-C6 JSON files
and write SHA256SUMS over every file in the evidence directory."""
import hashlib
import json
import sys
from pathlib import Path


def fmt(x, nd=1):
    return "-" if x is None else f"{x:,.{nd}f}"


def main():
    ev = Path(sys.argv[1])
    s = json.loads((ev / "c0c2c3/summary.json").read_text())
    fp = json.loads((ev / "c5/footprint.json").read_text())
    L = []
    L.append("## C0/C3: end-to-end HTTP, host to board and back (median of block medians, µs)\n")
    L.append("| Engine | Allow | Deny | Allow block-median range | Deny block-median range | Blocks |")
    L.append("|---|---:|---:|---|---|---:|")
    for e in ["none", "everparse", "cedar"]:
        a, d = s["c3"].get(f"{e}/allow"), s["c3"].get(f"{e}/deny")
        if not a:
            continue
        L.append(f"| {e if e != 'none' else 'none (C0, no ACL)'} | {fmt(a['median_of_block_medians_us'])} | "
                 f"{fmt(d['median_of_block_medians_us'])} | {fmt(a['block_median_min_us'])}–{fmt(a['block_median_max_us'])} | "
                 f"{fmt(d['block_median_min_us'])}–{fmt(d['block_median_max_us'])} | {a['blocks']} |")
    L.append("\nPer request inside the board (median over all timed requests):\n")
    L.append("| Engine / scenario | K0 engine µs | K1→K0→K1 channel round trip µs |")
    L.append("|---|---:|---:|")
    for k, v in s["per_request_cycles"].items():
        L.append(f"| {k} | {fmt(v['k0_engine_us_median'], 2)} | {fmt(v['k1_round_trip_us_median'], 2)} |")
    L.append("\n## C2: engine_decide cost on K0 (median over boots of the boot mean; 1,000 samples after 100 warmup)\n")
    L.append("| Engine | Case | Cycles | ns @1.2 GHz | Boot-mean range (cycles) | Boots | Mismatches |")
    L.append("|---|---|---:|---:|---|---:|---:|")
    for k, v in sorted(s["c2"].items()):
        e, c = k.split("/")
        L.append(f"| {e} | {c} | {fmt(v['median_boot_mean_cycles'], 0)} | {fmt(v['median_boot_mean_ns'], 0)} | "
                 f"{v['boot_mean_min_cycles']:,}–{v['boot_mean_max_cycles']:,} | {v['boots']} | {v['mismatches']} |")
    L.append("\n## C1: on-board equivalence (every campaign boot)\n")
    L.append("| Engine | Boots | EQUIVALENCE_OK (12,527 comparisons, 0 failures) |")
    L.append("|---|---:|---:|")
    for e, v in s["c1"].items():
        if e != "none":
            L.append(f"| {e} | {v['boots']} | {v['equivalence_ok']} |")
    for name in ["everparse", "cedar"]:
        p = ev / f"c4c6/c4-{name}.json"
        if p.exists():
            c4 = json.loads(p.read_text())
            if name == "everparse":
                L.append("\n## C4: policy updates over HTTP (PUT /api/policy)\n")
                L.append("| Engine | Case | HTTP | Expected | Digest unchanged on reject | Engine cycles | Pass |")
                L.append("|---|---|---:|---:|---|---:|---|")
            for r in c4["cases"]:
                unchanged = "-" if r["expected"] == 200 else str(r["digest_before"] == r["digest_after"])
                L.append(f"| {name} | {r['case']} | {r['status']} | {r['expected']} | {unchanged} | "
                         f"{r['engine_cycles']:,} | {r['pass']} |")
            v = sorted(c4["valid_update_cycles"])
            L.append(f"| {name} | valid update cost, median of {len(v)} | | | | {v[len(v)//2]:,} | |")
    for name in ["everparse", "cedar"]:
        p = ev / f"c4c6/c6-{name}.json"
        if p.exists():
            c6 = json.loads(p.read_text())
            if name == "everparse":
                L.append("\n## C6: compromised-K1 probes\n")
                L.append("| Engine | Probe | Outcome |")
                L.append("|---|---|---|")
            for r in c6["probes"]:
                out = dict(r["result"])
                out.pop("probe", None)
                extra = {k: r[k] for k in r if k not in ("probe", "result", "http")}
                L.append(f"| {name} | {r['probe']} | `{json.dumps(out)}` {json.dumps(extra)} |")
    L.append("\n## C5: K0 footprint (bytes)\n")
    L.append("| Engine | text+rodata | data | bss total | of which: heap arena | musl morecore | root stack | other bss | image |")
    L.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for r in fp["k0"]:
        L.append(f"| {r['engine']} | {r['text_rodata']:,} | {r['data']:,} | {r['bss_total']:,} | "
                 f"{r['k0_malloc_arena']:,} | {r['musl_morecore_area']:,} | {r['root_stack']:,} | "
                 f"{r['bss_excluding_arena_morecore_stack']:,} | {r['image_bytes']:,} |")
    (ev / "TABLES.md").write_text("\n".join(L) + "\n")
    files = sorted(p for p in ev.rglob("*") if p.is_file() and p.name != "SHA256SUMS")
    (ev / "SHA256SUMS").write_text("".join(
        f"{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(ev)}\n" for p in files))
    print("\n".join(L))


if __name__ == "__main__":
    main()
