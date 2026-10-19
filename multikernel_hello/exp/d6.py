#!/usr/bin/env python3
"""D6: footprint of K0 per image (raw, gateonly, everparse, cedar) and of K1.

Linked rootserver ELF: text+rodata (incl. eh_frame), data, bss, with the
large bss reservations named (K0 heap arena, musl morecore, root stack).
Per component: aarch64 `size` of the object files that make it up, before
linking (the F* extractor, gate, authenticator, protected app, D2 test
code, engine). Cedar's engine is the linked delta vs. the gateonly image
(its staticlib is gc-section linked)."""
import json
import re
import subprocess
import sys
from pathlib import Path

WS = Path(__file__).resolve().parents[4]  # camkes-vm-examples
SIZE, NM = "aarch64-linux-gnu-size", "aarch64-linux-gnu-nm"
IMAGES = ["raw", "gateonly", "everparse", "cedar"]


def sections(elf):
    out = subprocess.run([SIZE, "-A", str(elf)], capture_output=True, text=True, check=True).stdout
    sec = {}
    for line in out.splitlines()[2:]:
        p = line.split()
        if len(p) >= 2 and p[1].isdigit():
            sec[p[0]] = int(p[1])
    text = sum(v for k, v in sec.items() if k in (".init", ".text", ".fini", ".rodata", ".eh_frame",
                                                    ".note.gnu.build-id"))
    data = sum(v for k, v in sec.items() if k in (".data", ".got", ".got.plt", ".init_array",
                                                    ".fini_array", ".auth_config", ".demo_config",
                                                    "__vsyscall", "_ps_irqchips", ".tbss"))
    return {"text_rodata": text, "data": data, "bss_total": sec.get(".bss", 0)}


def big_bss(elf):
    out = subprocess.run([NM, "-S", "--size-sort", str(elf)], capture_output=True, text=True).stdout
    big = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in "bB" and int(p[1], 16) >= 1 << 20:
            big[p[3]] = int(p[1], 16)
    return big


def objsize(objs):
    if not objs:
        return {"text": 0, "data": 0, "bss": 0, "objects": 0}
    out = subprocess.run([SIZE, "-B", "--totals"] + [str(o) for o in objs],
                         capture_output=True, text=True, check=True).stdout
    t = out.strip().splitlines()[-1].split()
    return {"text": int(t[0]), "data": int(t[1]), "bss": int(t[2]), "objects": len(objs)}


def components(build):
    d = build / "rootserver/CMakeFiles"
    g = lambda pat: sorted(d.glob(pat))
    return {
        "fstar_extractor": objsize(g("rootserver_hello.dir/__/gateway/demo/generated/*.obj")),
        "policy_gate (incl. dashboard, RateCounter inlined)":
            objsize(g("rootserver_hello.dir/__/gateway/demo/pds/policy_gate.c.obj")),
        "pipeline+wire+oracle": objsize(g("rootserver_hello.dir/__/gateway/common/*.obj")),
        "authenticator": objsize(g("rootserver_hello.dir/__/gateway/demo/pds/authenticator.c.obj")),
        "protected_app": objsize(g("rootserver_hello.dir/__/gateway/demo/pds/protected_app.c.obj")),
        "k0 glue (gateway_k0.c: shim, dispatcher, proxy, D3 timing)":
            objsize(g("rootserver_hello.dir/gateway_k0.c.obj")),
        "d2 test code (equivalence corpus, extractor vectors)": objsize(g("gw_common.dir/**/*.obj")),
        "everparse engine": objsize(g("gw_engine.dir/**/*.obj")),
        "accept_all engine": objsize(g("rootserver_hello.dir/engine_accept_all.c.obj")),
    }


def main():
    out = {"k0": [], "k1": None}
    for e in IMAGES:
        b = WS / f"build-ccwmp25-mk-hello-k0-{e}"
        elf = b / "rootserver/rootserver_hello"
        img = b / "images/rootserver_hello-image-arm-stm32mp2"
        r = {"engine": e, **sections(elf), "bss_large_symbols": big_bss(elf),
             "image_bytes": img.stat().st_size, "components": components(b)}
        out["k0"].append(r)
    go = next(r for r in out["k0"] if r["engine"] == "gateonly")
    ce = next(r for r in out["k0"] if r["engine"] == "cedar")
    out["cedar_engine_linked_delta_vs_gateonly"] = {
        "text_rodata": ce["text_rodata"] - go["text_rodata"], "data": ce["data"] - go["data"],
        "note": "includes cedar_libc_stubs.c, k0_malloc.c and the D2 test code"}
    k1 = WS / "build-ccwmp25-mk-hello-k1/rootserver/rootserver_hello"
    out["k1"] = {**sections(k1), "bss_large_symbols": big_bss(k1)}
    Path(sys.argv[1]).write_text(json.dumps(out, indent=1) + "\n")
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
