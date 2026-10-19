"""Shared helpers for the corrected-design board runs (D0a-D7).

Port 80 is the gated HTTP path (K1 forwards raw bytes to K0); port 81 is
K1's local control port ("health", "stats", "probe <name>"), never gated.
Never touches host network configuration."""
import http.client
import json
import re
import socket
import subprocess
import time
from pathlib import Path

BOARD = "192.168.40.129"
ROOT = Path(__file__).resolve().parents[2]  # sel4_ccwmp25
ADMIN_LOGIN = b'{"username":"admin","password":"admin456"}'
OPERATOR_LOGIN = b'{"username":"operator","password":"oper789"}'


def request(method, path, body=None, headers=None, timeout=5):
    c = http.client.HTTPConnection(BOARD, 80, timeout=timeout)
    try:
        c.request(method, path, body=body, headers=headers or {})
        r = c.getresponse()
        return r.status, dict(r.getheaders()), r.read()
    finally:
        c.close()


def raw(data, timeout=5):
    """Send raw bytes on port 80, return all bytes until close (b'' on reset)."""
    out = bytearray()
    try:
        with socket.create_connection((BOARD, 80), timeout=timeout) as s:
            s.sendall(data)
            while True:
                chunk = s.recv(4096)
                if not chunk:
                    break
                out.extend(chunk)
    except (ConnectionResetError, BrokenPipeError) as e:
        return bytes(out) or f"<{type(e).__name__}>".encode()
    return bytes(out)


def status_of(resp):
    m = re.match(rb"HTTP/1\.1 (\d{3})", resp)
    return int(m.group(1)) if m else None


def control(cmd, timeout=90):
    with socket.create_connection((BOARD, 81), timeout=timeout) as s:
        s.sendall(cmd.encode() + b"\n")
        out = bytearray()
        while True:
            chunk = s.recv(65536)
            if not chunk:
                break
            out.extend(chunk)
    return json.loads(out)


def login(body):
    st, _, b = request("POST", "/api/login", body,
                       {"Content-Type": "application/json", "Content-Length": str(len(body))})
    if st != 200:
        raise RuntimeError(f"login HTTP {st}")
    return {"Authorization": f"Bearer {json.loads(b)['token']}"}


def serial():
    """Console text of the current boot only (after the last 'Starting kernel')."""
    s = subprocess.run(["tmux", "capture-pane", "-p", "-J", "-t", "mp255", "-S", "-8000"],
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
            return control("health", timeout=3)
        except (OSError, ValueError):
            time.sleep(1)
    raise TimeoutError("health")


def boot(engine):
    """Fresh boot of sel4-d1-<engine>.bin; returns once K1 serves (link up)."""
    subprocess.run([str(ROOT / "deploy/boot_image.sh"), f"sel4-d1-{engine}.bin"], check=True,
                   stdout=subprocess.DEVNULL)
    log = wait_serial(r"\[K0\] tick \d+ engine=\w+", 300)
    seen = set(re.findall(r"\[K0\] tick \d+ engine=(\w+)", log))
    if seen != {engine}:
        raise RuntimeError(f"wrong image booted (expected {engine}, saw {seen})")
    if re.search(r"HTTP_DEMO_FAIL|HTTP_GATE_TIMING_FAIL|EQUIVALENCE_FAIL|EXTRACTOR_FAIL", log):
        raise RuntimeError("failure marker in boot log")
    wait_serial(r"phy: link up", 60)
    return wait_health()
