#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
from pathlib import Path
import argparse
import http.client
import json
import queue
import re
import socket
import struct
import subprocess
import threading
import time


def qemu_command(image, port):
    image = Path(image).resolve()
    return [
        "taskset", "-c", "5", "qemu-system-x86_64", "-machine", "q35", "-accel", "kvm",
        "-cpu", "host", "-smp", "1", "-m", "2G", "-display", "none",
        "-serial", "mon:stdio", "-no-reboot",
        "-kernel", str(image.parent / "sel4_32.elf"), "-initrd", str(image),
        "-netdev", f"user,id=netdev0,hostfwd=tcp::{port}-10.0.2.15:8080",
        "-device", "virtio-net-pci,netdev=netdev0,addr=0x2.0",
    ]


def request(port, method, path, body=None, headers=None, timeout=10):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    try:
        connection.request(method, path, body=body, headers=headers or {})
        response = connection.getresponse()
        return response.status, response.read()
    finally:
        connection.close()


def require(actual, expected, description):
    if actual != expected:
        raise AssertionError(f"{description}: expected {expected}, got {actual}")
    print(f"DEMO_TEST_PASS,name={description},status={actual}")


def policy_blob(rule_count=3):
    rules = [
        (0x33333333, 0x01, 1, 0x0001),
        (0x22222222, 0x02, 1, 0x0000),
        (0x44444444, 0x01, 2, 0x0004),
        (0x55555555, 0x01, 1, 0x0001),
        (0x66666666, 0x01, 2, 0x0004),
    ]
    rules = rules[:rule_count]
    rules += [(0xDEADDEAD, 0, 0, 0)] * (8 - len(rules))
    output = bytearray([rule_count])
    for path, method, role, scope in rules:
        output.extend(struct.pack("<IBBH", path, method, role, scope))
    assert len(output) == 65
    return bytes(output)


def require_policy(body, declared_rules, paths, description):
    policy = json.loads(body)
    if policy.get("declared_rules") != declared_rules:
        raise AssertionError(
            f"{description}: expected declared_rules={declared_rules}, "
            f"got {policy.get('declared_rules')}"
        )
    slots = policy.get("slots")
    if not isinstance(slots, list) or len(slots) != 8:
        raise AssertionError(f"{description}: expected all eight policy slots")
    observed = {slot.get("path") for slot in slots}
    if not paths.issubset(observed):
        raise AssertionError(
            f"{description}: missing paths {sorted(paths - observed)}"
        )
    print(
        f"DEMO_TEST_PASS,name={description},declared_rules={declared_rules},"
        "slots=8"
    )


def reconnect_request(port, method, path, timeout=5):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            return request(port, method, path, timeout=2)
        except (OSError, http.client.HTTPException) as error:
            last_error = error
            time.sleep(0.02)
    raise AssertionError(
        f"disconnect/reconnect: server did not recover: {last_error}"
    )


def abandon_request(port, payload):
    stream = socket.create_connection(("127.0.0.1", port), timeout=5)
    try:
        stream.setsockopt(
            socket.SOL_SOCKET,
            socket.SO_LINGER,
            struct.pack("ii", 1, 0),
        )
        stream.sendall(payload)
    finally:
        stream.close()


def disconnect_reconnect_test(port):
    status_request = (
        b"GET /api/status HTTP/1.1\r\n"
        b"Host: localhost\r\n"
        b"Authorization: Bearer invalid_token_here\r\n\r\n"
    )
    dashboard_request = b"GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"
    for _ in range(4):
        abandon_request(port, status_request)
        status, body = reconnect_request(port, "GET", "/")
        if status != 200 or b"AArch64 Policy Gateway" not in body:
            raise AssertionError(
                "disconnect/reconnect: stale API response reached dashboard "
                f"connection: status={status}"
            )

        abandon_request(port, dashboard_request)
        status, _ = reconnect_request(port, "GET", "/api/status")
        if status != 401:
            raise AssertionError(
                "disconnect/reconnect: stale dashboard response reached API "
                f"connection: status={status}"
            )
    print("DEMO_TEST_PASS,name=disconnect_reconnect_epoch,rounds=4")


def malformed_test(port):
    with socket.create_connection(("127.0.0.1", port), timeout=10) as stream:
        stream.sendall(b"BREW /api/status HTTP/1.1\r\nHost: localhost\r\n\r\n")
        response = bytearray()
        while True:
            chunk = stream.recv(4096)
            if not chunk:
                break
            response.extend(chunk)
    if not response.startswith(b"HTTP/1.1 400 "):
        raise AssertionError(f"malformed request: unexpected response {response[:80]!r}")
    print("DEMO_TEST_PASS,name=malformed_request,status=400")


def pressure_test(port):
    results = []
    errors = []

    def worker():
        try:
            results.append(request(port, "GET", "/api/status", timeout=15)[0])
        except Exception as error:  # Recovery below is the admission criterion.
            errors.append(type(error).__name__)

    workers = [threading.Thread(target=worker) for _ in range(8)]
    for worker in workers:
        worker.start()
    for worker in workers:
        worker.join()
    recovery, _ = request(port, "GET", "/api/status")
    require(recovery, 401, "connection_pressure_recovery")
    if not results or any(status != 401 for status in results):
        raise AssertionError(
            f"connection pressure: statuses={results}, errors={errors}"
        )
    print(
        "DEMO_TEST_PASS,name=connection_pressure," +
        f"completed={len(results)},bounded_failures={len(errors)}"
    )


def prepare_demo_policy(port):
    admin_body = b'{"username":"admin","password":"admin456"}'
    status, body = request(port, "POST", "/api/login", admin_body, {
        "Content-Type": "application/json",
        "Content-Length": str(len(admin_body)),
    })
    require(status, 200, "fresh_boot_admin_login")
    admin_token = json.loads(body)["token"]
    admin_headers = {"Authorization": f"Bearer {admin_token}"}

    operator_body = b'{"username":"operator","password":"oper789"}'
    status, body = request(port, "POST", "/api/login", operator_body, {
        "Content-Type": "application/json",
        "Content-Length": str(len(operator_body)),
    })
    require(status, 200, "fresh_boot_operator_login")
    operator_headers = {"Authorization": f"Bearer {json.loads(body)['token']}"}
    status, _ = request(port, "GET", "/api/status", headers=operator_headers)
    require(status, 403, "fresh_boot_operator_status_denied")

    status, body = request(port, "GET", "/api/policy", headers=admin_headers)
    require(status, 200, "fresh_boot_admin_policy_read")
    require_policy(body, 3, {"/api/logout", "/api/policy"},
                   "fresh_boot_restrictive_shape")

    blob = policy_blob(3)
    update_headers = dict(admin_headers)
    update_headers.update({
        "Content-Type": "application/octet-stream",
        "Content-Length": str(len(blob)),
    })
    status, _ = request(port, "PUT", "/api/policy", blob, update_headers)
    require(status, 200, "fresh_boot_admin_install_demo_policy")


def exercise(port):
    status, body = request(port, "GET", "/")
    require(status, 200, "dashboard")
    if b"AArch64 Policy Gateway" not in body:
        raise AssertionError("dashboard: expected embedded AArch64 UI marker")
    print("DEMO_TEST_PASS,name=dashboard_content,marker=AArch64_Policy_Gateway")

    status, _ = request(port, "GET", "/api/status")
    require(status, 401, "status_without_token")
    status, _ = request(port, "GET", "/api/not-configured")
    require(status, 404, "unknown_path")
    status, _ = request(port, "GET", "/api/demo")
    require(status, 401, "catalogued_path_without_token")
    malformed_test(port)
    disconnect_reconnect_test(port)
    pressure_test(port)

    admin_body = b'{"username":"admin","password":"admin456"}'
    status, body = request(port, "POST", "/api/login", admin_body, {
        "Content-Type": "application/json",
        "Content-Length": str(len(admin_body)),
    })
    require(status, 200, "admin_login")
    admin_token = json.loads(body)["token"]
    admin_headers = {"Authorization": f"Bearer {admin_token}"}

    status, _ = request(port, "GET", "/api/status", headers=admin_headers)
    require(status, 200, "status_with_token")
    status, _ = request(
        port,
        "GET",
        "/api/status?x=hello%20world%26more",
        headers=admin_headers,
    )
    require(status, 200, "status_with_encoded_query")
    status, _ = request(port, "GET", "/api/status", headers={
        "Authorization": "Bearer invalid_token_here"
    })
    require(status, 403, "status_with_bad_token")

    operator_body = b'{"username":"operator","password":"oper789"}'
    status, body = request(port, "POST", "/api/login", operator_body, {
        "Content-Type": "application/json",
        "Content-Length": str(len(operator_body)),
    })
    require(status, 200, "operator_login")
    operator_token = json.loads(body)["token"]
    operator_headers = {"Authorization": f"Bearer {operator_token}"}

    status, body = request(
        port, "GET", "/api/policy", headers=admin_headers
    )
    require(status, 200, "policy_read_default")
    require_policy(
        body,
        3,
        {"/api/status", "/api/logout", "/api/policy"},
        "policy_read_default_shape",
    )
    status, _ = request(port, "GET", "/api/demo", headers=operator_headers)
    require(status, 403, "demo_denied_before_preset")
    status, _ = request(port, "GET", "/api/reports", headers=admin_headers)
    require(status, 403, "reports_denied_before_preset")
    status, _ = request(port, "POST", "/api/logout")
    require(status, 401, "logout_without_token")
    status, _ = request(port, "GET", "/api/logout", headers=admin_headers)
    require(status, 403, "logout_wrong_method")

    blob = policy_blob(5)
    update_headers = dict(admin_headers)
    update_headers.update({
        "Content-Type": "application/octet-stream",
        "Content-Length": str(len(blob)),
    })
    status, _ = request(port, "PUT", "/api/policy", blob, update_headers)
    require(status, 200, "install_demo_preset")

    status, body = request(
        port, "GET", "/api/policy", headers=admin_headers
    )
    require(status, 200, "policy_read_demo_preset")
    require_policy(
        body,
        5,
        {"/api/demo", "/api/reports"},
        "policy_read_demo_shape",
    )
    status, _ = request(port, "GET", "/api/demo", headers=operator_headers)
    require(status, 200, "operator_demo_allowed")
    status, _ = request(
        port, "GET", "/api/reports", headers=operator_headers
    )
    require(status, 403, "operator_reports_denied")
    status, _ = request(port, "GET", "/api/reports", headers=admin_headers)
    require(status, 200, "admin_reports_allowed")

    invalid_blob = bytearray(blob)
    invalid_blob[1 + 4] = 0x40
    status, _ = request(
        port, "PUT", "/api/policy", bytes(invalid_blob), update_headers
    )
    require(status, 403, "invalid_policy_rejected")
    status, body = request(
        port, "GET", "/api/policy", headers=admin_headers
    )
    require(status, 200, "policy_read_after_rejection")
    require_policy(
        body,
        5,
        {"/api/demo", "/api/reports"},
        "invalid_update_preserves_policy",
    )

    reset_blob = policy_blob(3)
    status, _ = request(
        port, "PUT", "/api/policy", reset_blob, update_headers
    )
    require(status, 200, "reset_default_policy")
    status, body = request(
        port, "GET", "/api/policy", headers=admin_headers
    )
    require(status, 200, "policy_read_after_reset")
    require_policy(
        body,
        3,
        {"/api/status", "/api/logout", "/api/policy"},
        "reset_policy_shape",
    )
    status, _ = request(port, "GET", "/api/demo", headers=operator_headers)
    require(status, 403, "demo_denied_after_reset")
    status, _ = request(port, "GET", "/api/reports", headers=admin_headers)
    require(status, 403, "reports_denied_after_reset")
    status, _ = request(port, "POST", "/api/logout", headers=admin_headers)
    require(status, 200, "logout_with_token")

    limited_at = None
    for index in range(1, 56):
        status, _ = request(port, "GET", "/api/status", headers=admin_headers)
        if status == 429:
            limited_at = index
            break
        require(status, 200, f"sustained_request_{index}")
    if limited_at is None:
        raise AssertionError("rate limit not observed within 55 requests")
    print(f"DEMO_TEST_PASS,name=rate_limit,limited_at={limited_at},status=429")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", required=True)
    parser.add_argument("--serial-log")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--boot-timeout", type=int, default=30)
    args = parser.parse_args()

    process = subprocess.Popen(
        qemu_command(args.image, args.port),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
        bufsize=1,
    )
    lines = []
    messages = queue.Queue()

    def capture():
        assert process.stdout is not None
        for line in process.stdout:
            lines.append(line)
            messages.put(line)

    thread = threading.Thread(target=capture, daemon=True)
    thread.start()
    deadline = time.monotonic() + args.boot_timeout
    ready_text = ""
    try:
        while time.monotonic() < deadline:
            if process.poll() is not None:
                raise RuntimeError(f"QEMU exited with status {process.returncode}")
            try:
                line = messages.get(timeout=0.25)
            except queue.Empty:
                continue
            if "HTTP_DEMO_FAIL" in line or "HTTP_GATE_TIMING_FAIL" in line or "MICROKIT_CEDAR_HEAP_EXHAUSTED" in line or "cedar gate: panic" in line or "MON|ERROR" in line:
                raise RuntimeError(line.strip())
            ready_text = (ready_text + line)[-8192:]
            cleaned = re.sub(
                r"D1_CONTROL,hz=\d+,releases=\d+,missed=\d+,worst_late_ns=\d+\n",
                "", ready_text,
            )
            if "HTTP_DEMO_READY" in cleaned:
                break
        else:
            raise TimeoutError("demo did not obtain a DHCP address")

        prepare_demo_policy(args.port)
        exercise(args.port)
        if any("HTTP_DEMO_FAIL" in line or "HTTP_GATE_TIMING_FAIL" in line or "MICROKIT_CEDAR_HEAP_EXHAUSTED" in line or "cedar gate: panic" in line or "MON|ERROR" in line
               for line in lines):
            raise RuntimeError("failure marker appeared in the QEMU log")
        print("HTTP_DEMO_PASS,scenarios=25,transport=plaintext")
    except Exception:
        print("".join(lines[-80:]))
        raise
    finally:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)
        thread.join(timeout=1)
        if args.serial_log:
            Path(args.serial_log).write_text("".join(lines))


if __name__ == "__main__":
    main()
