#!/usr/bin/env python3
"""D1: run the x86 scripts/demo_test.py scenarios (vendored unchanged in
gateway/scripts/) against the board instead of a local QEMU: every
connection to 127.0.0.1:<port> goes to BOARD:80. Only the QEMU launch and
the boot wait are skipped; prepare_demo_policy() and exercise() run as-is.
Expects a freshly booted board (the x86 test also starts from a fresh boot)."""
import argparse
import http.client
import socket
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gateway/scripts"))
import demo_test  # noqa: E402

BOARD = "192.168.40.129"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default=BOARD)
    ap.add_argument("--port", type=int, default=80)
    a = ap.parse_args()

    real_conn = http.client.HTTPConnection.__init__
    real_create = socket.create_connection

    def conn_init(self, host, port=None, *args, **kw):
        if host == "127.0.0.1":
            host, port = a.host, a.port
        real_conn(self, host, port, *args, **kw)

    def create(address, *args, **kw):
        if address[0] == "127.0.0.1":
            address = (a.host, a.port)
        return real_create(address, *args, **kw)

    http.client.HTTPConnection.__init__ = conn_init
    socket.create_connection = create
    demo_test.prepare_demo_policy(a.port)
    demo_test.exercise(a.port)
    print("HTTP_DEMO_PASS,scenarios=25,transport=plaintext,target=ccmp25-board")


if __name__ == "__main__":
    main()
