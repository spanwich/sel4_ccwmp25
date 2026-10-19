#!/usr/bin/env python3
"""Summarise a CCMP25 campaign directory (campaign.py output) into summary.json.

C0/C3: per engine x scenario, the median of block medians (and the min/max of
block medians), host-observed ns. C2: per engine x case, the median over boots
of the boot-level mean (and p50) engine_decide cycles, converted at 1.2 GHz.
C1 on every boot: count of EQUIVALENCE_OK lines. Also K0 engine cycles per
HTTP request (X-Engine-Cycles) and K1 channel round trip (X-Channel-Cycles).
"""
import json
import statistics as st
import sys
from pathlib import Path

CPU_HZ = 1_200_000_000


def med(xs):
    return st.median(xs) if xs else None


def main():
    d = Path(sys.argv[1])
    man = json.loads((d / "manifest.json").read_text())
    runs = [json.loads((d / r["file"]).read_text()) for r in man["runs"]]
    out = {"campaign": str(d), "status": man.get("status"), "boots": len(runs),
           "retries": man.get("retries", []), "c3": {}, "c2": {}, "c1": {},
           "per_request_cycles": {}}
    engines = sorted({r["engine"] for r in runs})
    for e in engines:
        er = [r for r in runs if r["engine"] == e]
        out["c1"][e] = {"boots": len(er),
                        "equivalence_ok": sum(any("EQUIVALENCE_OK" in x and "comparisons=12527" in x
                                                  for x in r["equivalence"]) for r in er)}
        for scen in ["allow", "deny"]:
            bm = [med(r["scenarios"][scen]["latency_ns"]) for r in er]
            allv = [v for r in er for v in r["scenarios"][scen]["latency_ns"]]
            out["c3"][f"{e}/{scen}"] = {
                "blocks": len(bm), "samples": len(allv),
                "median_of_block_medians_us": med(bm) / 1e3,
                "block_median_min_us": min(bm) / 1e3, "block_median_max_us": max(bm) / 1e3,
                "status": er[0]["scenarios"][scen]["status"]}
            ec = [c for r in er for c in r["scenarios"][scen]["engine_cycles"]]
            kc = [c for r in er for c in r["scenarios"][scen]["k1_channel_cycles"]]
            out["per_request_cycles"][f"{e}/{scen}"] = {
                "k0_engine_cycles_median": med(ec), "k1_round_trip_cycles_median": med(kc),
                "k0_engine_us_median": med(ec) / CPU_HZ * 1e6 if ec else None,
                "k1_round_trip_us_median": med(kc) / CPU_HZ * 1e6 if kc else None}
        cases = {}
        for r in er:
            for c in r["c2"]:
                if "case" in c and "mean" in c:
                    cases.setdefault(c["case"], []).append(c)
        for case, cs in cases.items():
            means = [int(c["mean"]) for c in cs]
            p50s = [int(c["p50"]) for c in cs if "p50" in c]
            out["c2"][f"{e}/{case}"] = {
                "boots": len(cs), "mismatches": sum(int(c.get("mismatches", 0)) for c in cs),
                "median_boot_mean_cycles": med(means),
                "median_boot_mean_ns": med(means) / CPU_HZ * 1e9,
                "median_boot_p50_cycles": med(p50s),
                "boot_mean_min_cycles": min(means), "boot_mean_max_cycles": max(means)}
    (d / "summary.json").write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
