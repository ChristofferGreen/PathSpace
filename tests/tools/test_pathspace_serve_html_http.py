#!/usr/bin/env python3

"""Integration test for pathspace_serve_html HTML fetch + auth gating."""

from __future__ import annotations

import argparse
import http.client
import json
import sys
import time
from typing import Dict, Optional

from pathspace_serve_html_testutil import (
    find_free_port,
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
    parser = argparse.ArgumentParser(description="HTML fetch regression for pathspace_serve_html")
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--server", required=True)
    args = parser.parse_args()

    port = find_free_port()
    extra_args = [
        "--seed-demo",
        "--session-timeout",
        "4",
        "--session-max-age",
        "6",
        "--rate-limit-ip-per-minute",
        "240",
        "--rate-limit-ip-burst",
        "200",
        "--rate-limit-session-per-minute",
        "60",
        "--rate-limit-session-burst",
        "20",
    ]
    proc = start_server(args.server, port, extra_args)

    try:
        if not wait_for_health(port, timeout=6.0):
            print("Server did not pass /healthz", file=sys.stderr)
            return 1

        # Unauthenticated access should be rejected.
        unauth = request("GET", port, "/apps/demo_web/gallery")
        body = unauth.read().decode("utf-8", errors="ignore")
        unauth.close()
        if unauth.status != 401:
            print(f"Expected 401 for unauthenticated request, got {unauth.status}", file=sys.stderr)
            print(body, file=sys.stderr)
            return 1

        # Authenticate using the seeded demo credentials.
        login = request("POST",
                        port,
                        "/login",
                        body={"username": "demo", "password": "demo"})
        if login.status != 200:
            print(f"Login failed with {login.status}", file=sys.stderr)
            print(login.read().decode("utf-8", errors="ignore"), file=sys.stderr)
            login.close()
            return 1
        cookie_header = login.getheader("Set-Cookie")
        login.read()
        login.close()
        if not cookie_header:
            print("Server did not return a session cookie", file=sys.stderr)
            return 1
        cookie = cookie_header.split(";", 1)[0]

        headers = {"Cookie": cookie}
        html_resp = request("GET", port, "/apps/demo_web/gallery", headers=headers)
        status = html_resp.status
        cache_control = html_resp.getheader("Cache-Control")
        etag = html_resp.getheader("ETag")
        html_body = html_resp.read().decode("utf-8", errors="ignore")
        html_resp.close()

        if status != 200:
            print(f"Expected 200 from /apps, got {status}", file=sys.stderr)
            print(html_body[:200], file=sys.stderr)
            return 1
        if cache_control != "no-store":
            print(f"Unexpected Cache-Control: {cache_control}", file=sys.stderr)
            return 1
        if not etag:
            print("Missing ETag header on HTML response", file=sys.stderr)
            return 1
        if "<html" not in html_body.lower():
            print("Response did not look like HTML", file=sys.stderr)
            return 1
        if "pathspace-html-live" not in html_body:
            print("Live update script missing from HTML body", file=sys.stderr)
            return 1

        json_resp = request("GET",
                             port,
                             "/apps/demo_web/gallery?format=json",
                             headers=headers)
        json_body = json_resp.read().decode("utf-8", errors="ignore")
        json_resp.close()
        if json_resp.status != 200:
            print(f"Expected 200 from JSON endpoint, got {json_resp.status}", file=sys.stderr)
            print(json_body[:200], file=sys.stderr)
            return 1
        try:
            payload = json.loads(json_body)
        except json.JSONDecodeError as exc:
            print(f"Failed to decode JSON payload: {exc}", file=sys.stderr)
            return 1
        if not isinstance(payload.get("revision"), (int, float)):
            print("JSON payload missing numeric revision", file=sys.stderr)
            return 1
        dom_value = payload.get("dom")
        if not isinstance(dom_value, str) or not dom_value.strip():
            print("JSON payload did not include DOM content", file=sys.stderr)
            return 1
        if "css" not in payload or "commands" not in payload:
            print("JSON payload missing css/commands keys", file=sys.stderr)
            return 1

        asset_resp = request("GET",
                              port,
                              "/assets/demo_web/images/demo-badge.txt",
                              headers=headers)
        asset_body = asset_resp.read()
        cache_header = asset_resp.getheader("Cache-Control")
        etag_header = asset_resp.getheader("ETag")
        content_type = asset_resp.getheader("Content-Type")
        asset_resp.close()
        if asset_resp.status != 200:
            print(f"Expected 200 from asset route, got {asset_resp.status}", file=sys.stderr)
            return 1
        if cache_header != "public, max-age=31536000, immutable":
            print(f"Unexpected asset Cache-Control: {cache_header}", file=sys.stderr)
            return 1
        if not etag_header:
            print("Asset response missing ETag", file=sys.stderr)
            return 1
        if content_type != "text/plain":
            print(f"Unexpected asset Content-Type: {content_type}", file=sys.stderr)
            return 1
        if asset_body != b"PathSpace demo asset":
            print(f"Asset body mismatch: {asset_body!r}", file=sys.stderr)
            return 1
        cache_headers = headers.copy()
        cache_headers["If-None-Match"] = etag_header
        cached_resp = request("GET",
                               port,
                               "/assets/demo_web/images/demo-badge.txt",
                               headers=cache_headers)
        cached_body = cached_resp.read()
        cached_resp.close()
        if cached_resp.status != 304:
            print(f"Expected 304 from cached asset, got {cached_resp.status}", file=sys.stderr)
            return 1
        if cached_body:
            print("Expected empty body for 304 response", file=sys.stderr)
            return 1

        ops_resp = request(
            "POST",
            port,
            "/api/ops/demo_refresh",
            body={
                "app": "demo_web",
                "schema": "demo.refresh.v1",
                "payload": {"source": "integration_test", "nonce": 1},
            },
            headers=headers,
        )
        ops_body = ops_resp.read().decode("utf-8", errors="ignore")
        ops_resp.close()
        if ops_resp.status != 202:
            print(f"Expected 202 from /api/ops, got {ops_resp.status}", file=sys.stderr)
            print(ops_body[:200], file=sys.stderr)
            return 1
        try:
            ack = json.loads(ops_body)
        except json.JSONDecodeError as exc:
            print(f"Failed to parse ops response: {exc}", file=sys.stderr)
            return 1
        if ack.get("status") != "enqueued":
            print(f"Unexpected ops status: {ack}", file=sys.stderr)
            return 1
        if ack.get("app") != "demo_web" or ack.get("op") != "demo_refresh":
            print(f"Ops response did not echo app/op: {ack}", file=sys.stderr)
            return 1
        expected_queue = "/system/applications/demo_web/ops/demo_refresh/inbox/queue"
        if ack.get("queue") != expected_queue:
            print(f"Unexpected queue path: {ack.get('queue')}", file=sys.stderr)
            return 1

        metrics_resp = request("GET", port, "/metrics", headers=headers)
        metrics_body = metrics_resp.read().decode("utf-8", errors="ignore")
        metrics_resp.close()
        if metrics_resp.status != 200:
            print(f"/metrics returned {metrics_resp.status}", file=sys.stderr)
            print(metrics_body[:200], file=sys.stderr)
            return 1
        if "pathspace_serve_html_requests_total" not in metrics_body:
            print("/metrics payload missing request counters", file=sys.stderr)
            return 1

        # Exceed the per-session rate limit to ensure the guard responds with 429.
        rate_limit_triggered = False
        for attempt in range(25):
            rl_resp = request("GET", port, "/session", headers=headers)
            rl_body = rl_resp.read().decode("utf-8", errors="ignore")
            status = rl_resp.status
            rl_resp.close()
            if status == 429:
                try:
                    payload = json.loads(rl_body)
                except json.JSONDecodeError as exc:
                    print(f"Failed to parse rate limit payload: {exc}", file=sys.stderr)
                    return 1
                if payload.get("error") != "rate_limited":
                    print(f"Unexpected rate limit payload: {payload}", file=sys.stderr)
                    return 1
                rate_limit_triggered = True
                break
        if not rate_limit_triggered:
            print("Expected per-session rate limit to trigger", file=sys.stderr)
            return 1

        time.sleep(5)
        expired_resp = request("GET", port, "/apps/demo_web/gallery", headers=headers)
        expired_body = expired_resp.read().decode("utf-8", errors="ignore")
        expired_cookie = expired_resp.getheader("Set-Cookie")
        expired_resp.close()
        if expired_resp.status != 401:
            print(f"Expected 401 after session expiry, got {expired_resp.status}", file=sys.stderr)
            print(expired_body[:200], file=sys.stderr)
            return 1
        if not expired_cookie or "Max-Age=0" not in expired_cookie:
            print("Expired session should clear cookie", file=sys.stderr)
            return 1

        return 0
    finally:
        terminate_process(proc)


if __name__ == "__main__":
    sys.exit(main())
