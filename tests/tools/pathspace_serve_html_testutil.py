#!/usr/bin/env python3

"""Shared helpers for pathspace_serve_html integration tests."""

from __future__ import annotations

import http.client
import socket
import subprocess
import sys
import time
from typing import Iterable, Sequence


def find_free_port() -> int:
    """Bind to an ephemeral port on localhost and return it."""

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_health(port: int, timeout: float = 6.0) -> bool:
    """Ping /healthz until it responds with HTTP 200 or the timeout expires."""

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=1)
            conn.request("GET", "/healthz")
            resp = conn.getresponse()
            resp.read()
            conn.close()
            if resp.status == 200:
                return True
        except Exception:
            time.sleep(0.1)
    return False


def start_server(server: str,
                 port: int,
                 extra_args: Sequence[str] | None = None) -> subprocess.Popen:
    """Launch pathspace_serve_html and return the subprocess handle."""

    args: list[str] = [
        server,
        "--host",
        "127.0.0.1",
        "--port",
        str(port),
        "--apps-root",
        "/system/applications",
        "--renderer",
        "html",
    ]
    if extra_args:
        args.extend(extra_args)

    return subprocess.Popen(
        args,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.PIPE,
        text=True,
    )


def terminate_process(proc: subprocess.Popen) -> None:
    """Terminate the server process and drain stderr for diagnostics."""

    proc.terminate()
    try:
        proc.wait(timeout=2)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()

    if proc.stderr:
        stderr = proc.stderr.read()
        if stderr:
            print(stderr, file=sys.stderr)


def read_body(response: http.client.HTTPResponse) -> bytes:
    """Read the full HTTP response body and close the connection."""

    body = response.read()
    response.close()
    return body


def format_headers(headers: Iterable[tuple[str, str]]) -> str:
    return "\n".join(f"{name}: {value}" for name, value in headers)
