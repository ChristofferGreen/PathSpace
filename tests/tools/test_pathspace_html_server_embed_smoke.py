#!/usr/bin/env python3

"""Smoke test for embedded PathSpaceHtmlServer helper."""

from __future__ import annotations

import argparse
import http.client
import json
import sys
from typing import Dict, Optional

from pathspace_serve_html_testutil import (
    find_free_port,
    load_http_transcript,
    start_server,
    terminate_process,
    wait_for_health,
)


def request(method: str,
            port: int,
            path: str,
            body: Optional[dict] = None,
            headers: Optional[Dict[str, str]] = None) -> http.client.HTTPResponse:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=6)
    payload = json.dumps(body) if body is not None else None
    request_headers = headers.copy() if headers else {}
    if payload is not None and "Content-Type" not in request_headers:
        request_headers["Content-Type"] = "application/json"
    conn.request(method, path, body=payload, headers=request_headers)
    return conn.getresponse()


def main() -> int:
    parser = argparse.ArgumentParser(description="Smoke test for embedded PathSpaceHtmlServer")
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--server", required=True)
    args = parser.parse_args()

    transcript = load_http_transcript(args.repo_root)
    port = find_free_port()
    extra_args = [
        "--seed-demo",
        "--session-timeout",
        "4",
        "--session-max-age",
        "6",
    ]
    proc = start_server(args.server, port, extra_args)

    try:
        if not wait_for_health(port, timeout=6.0):
            print("Embedded server did not pass /healthz", file=sys.stderr)
            return 1

        unauth = request("GET", port, "/apps/demo_web/gallery")
        expected_unauth = transcript.get("unauthenticated_apps", {})
        if unauth.status != expected_unauth.get("status", 401):
            body = unauth.read().decode("utf-8", errors="ignore")
            unauth.close()
            print(f"Expected 401 unauthenticated, got {unauth.status}", file=sys.stderr)
            print(body[:200], file=sys.stderr)
            return 1
        unauth.read()
        unauth.close()

        login = request("POST",
                        port,
                        "/login",
                        body={"username": "demo", "password": "demo"})
        login_body = login.read().decode("utf-8", errors="ignore")
        login_headers = {name.lower(): value for name, value in login.getheaders()}
        login.close()
        if login.status != 200:
            print(f"Login failed with {login.status}", file=sys.stderr)
            print(login_body[:200], file=sys.stderr)
            return 1
        try:
            login_json = json.loads(login_body)
        except json.JSONDecodeError as exc:
            print(f"Failed to parse login response: {exc}", file=sys.stderr)
            return 1
        if login_json.get("username") != "demo" or login_json.get("status") != "ok":
            print(f"Unexpected login payload: {login_json}", file=sys.stderr)
            return 1
        cookie = login_headers.get("set-cookie")
        if not cookie:
            print("Login did not return session cookie", file=sys.stderr)
            return 1
        cookie_header = cookie.split(";", 1)[0]

        headers = {"Cookie": cookie_header}
        session = request("GET", port, "/session", headers=headers)
        session_body = session.read().decode("utf-8", errors="ignore")
        session.close()
        if session.status != 200:
            print(f"Session endpoint failed: {session.status}", file=sys.stderr)
            print(session_body[:200], file=sys.stderr)
            return 1
        try:
            session_json = json.loads(session_body)
        except json.JSONDecodeError as exc:
            print(f"Failed to parse session payload: {exc}", file=sys.stderr)
            return 1
        if not session_json.get("authenticated"):
            print(f"Session not authenticated: {session_json}", file=sys.stderr)
            return 1

        html = request("GET", port, "/apps/demo_web/gallery", headers=headers)
        html_body = html.read().decode("utf-8", errors="ignore")
        html.close()
        if html.status != 200:
            print(f"Expected 200 from /apps, got {html.status}", file=sys.stderr)
            print(html_body[:200], file=sys.stderr)
            return 1
        if "<html" not in html_body.lower():
            print("/apps response missing HTML payload", file=sys.stderr)
            return 1
        if "pathspace-html-live" not in html_body:
            print("Live update script missing from HTML", file=sys.stderr)
            return 1

        return 0
    finally:
        terminate_process(proc)


if __name__ == "__main__":
    sys.exit(main())
