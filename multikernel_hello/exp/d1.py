#!/usr/bin/env python3
"""D1: per engine, a fresh boot, then the x86 demo_test.py scenarios
(demo_test_board.py) against 192.168.40.129:80. Writes d1-<engine>.log
(test output) and d1-<engine>.serial.log."""
import subprocess
import sys
from pathlib import Path

import dcommon as dc

out = Path(sys.argv[1])
out.mkdir(parents=True, exist_ok=True)
engines = sys.argv[2:] or ["everparse", "cedar"]
ok = True
for e in engines:
    health = dc.boot(e)
    r = subprocess.run([sys.executable, str(Path(__file__).with_name("demo_test_board.py"))],
                       capture_output=True, text=True, timeout=600)
    (out / f"d1-{e}.log").write_text(r.stdout + r.stderr)
    (out / f"d1-{e}.serial.log").write_text(dc.serial())
    passed = r.returncode == 0 and "HTTP_DEMO_PASS" in r.stdout
    ok &= passed
    print(f"D1 {e}: {'PASS' if passed else 'FAIL'} health={health}", flush=True)
sys.exit(0 if ok else 1)
