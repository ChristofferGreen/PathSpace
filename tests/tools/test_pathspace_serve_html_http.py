#!/usr/bin/env python3

"""Integration test for pathspace_serve_html HTML fetch + auth gating."""

from __future__ import annotations

import argparse
import http.client
import json
import sys
import time
from typing import Any, Dict, Mapping, Optional

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


def validate_http_response(check_name: str,
                           response: http.client.HTTPResponse,
                           transcript: Mapping[str, Any]) -> Optional[Dict[str, Any]]:
    expected = transcript.get(check_name)
    if not expected:
        print(f"Transcript missing check '{check_name}'", file=sys.stderr)
        response.read()
        response.close()
        return None

    headers = {name.lower(): value for name, value in response.getheaders()}
    body_bytes = response.read()
    response.close()
    body_text = body_bytes.decode("utf-8", errors="ignore")
    status = response.status

    expected_status = expected.get("status")
    if expected_status is not None and status != expected_status:
        print(f"[{check_name}] Expected status {expected_status}, got {status}", file=sys.stderr)
        print(body_text[:200], file=sys.stderr)
        return None

    expected_headers = expected.get("headers", {})
    for key, value in expected_headers.items():
        key_lower = key.lower()
        actual_value = headers.get(key_lower)
        if actual_value != value:
            print(
                f"[{check_name}] Header mismatch for {key}: expected '{value}', got '{actual_value}'",
                file=sys.stderr,
            )
            return None

    for header_name in expected.get("required_headers", []):
        header_lower = header_name.lower()
        if header_lower not in headers:
            print(f"[{check_name}] Missing required header: {header_name}", file=sys.stderr)
            return None

    for header_name, needle in expected.get("header_substrings", {}).items():
        header_lower = header_name.lower()
        header_value = headers.get(header_lower, "")
        if needle not in header_value:
            print(
                f"[{check_name}] Header {header_name} missing substring '{needle}': {header_value}",
                file=sys.stderr,
            )
            return None

    json_payload = None
    if "json" in expected:
        try:
            json_payload = json.loads(body_text)
        except json.JSONDecodeError as exc:
            print(f"[{check_name}] Failed to decode JSON payload: {exc}", file=sys.stderr)
            return None
        for key, value in expected["json"].items():
            if json_payload.get(key) != value:
                print(
                    f"[{check_name}] JSON mismatch for '{key}': expected '{value}', got '{json_payload.get(key)}'",
                    file=sys.stderr,
                )
                return None

    return {"headers": headers, "body": body_text, "json": json_payload}


def main() -> int:
    parser = argparse.ArgumentParser(description="HTML fetch regression for pathspace_serve_html")
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--server", required=True)
    args = parser.parse_args()

    http_transcript = load_http_transcript(args.repo_root)
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
        if validate_http_response("unauthenticated_apps", unauth, http_transcript) is None:
            return 1

        # Authenticate using the seeded demo credentials.
        login = request("POST",
                        port,
                        "/login",
                        body={"username": "demo", "password": "demo"})
        login_result = validate_http_response("login_success", login, http_transcript)
        if login_result is None:
            return 1
        cookie_header = login_result["headers"].get("set-cookie")
        if not cookie_header:
            print("Server did not return a session cookie", file=sys.stderr)
            return 1
        cookie = cookie_header.split(";", 1)[0]

        headers = {"Cookie": cookie}

        session_resp = request("GET", port, "/session", headers=headers)
        if validate_http_response("session_authenticated", session_resp, http_transcript) is None:
            return 1

        cookie_name = cookie.split("=", 1)[0]
        invalid_cookie = f"{cookie_name}=invalid-session"
        invalid_headers = {"Cookie": invalid_cookie}
        invalid_resp = request("GET", port, "/apps/demo_web/gallery", headers=invalid_headers)
        if validate_http_response("invalid_cookie_apps", invalid_resp, http_transcript) is None:
            return 1
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
            status = rl_resp.status
            if status == 429:
                if validate_http_response("rate_limit_session", rl_resp, http_transcript) is None:
                    return 1
                rate_limit_triggered = True
                break
            rl_resp.read()
            rl_resp.close()
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

        # Fresh login to validate the explicit logout/session endpoints using transcript guardrails.
        second_login = request(
            "POST",
            port,
            "/login",
            body={"username": "demo", "password": "demo"},
        )
        second_login_result = validate_http_response("login_success", second_login, http_transcript)
        if second_login_result is None:
            return 1
        second_cookie_header = second_login_result["headers"].get("set-cookie")
        if not second_cookie_header:
            print("Second login did not return a session cookie", file=sys.stderr)
            return 1
        second_cookie = second_cookie_header.split(";", 1)[0]
        logout_headers = {"Cookie": second_cookie}

        logout_resp = request("POST", port, "/logout", headers=logout_headers)
        if validate_http_response("logout_success", logout_resp, http_transcript) is None:
            return 1

        session_after_logout = request("GET", port, "/session")
        if validate_http_response("session_after_logout", session_after_logout, http_transcript) is None:
            return 1

        return 0
    finally:
        terminate_process(proc)


if __name__ == "__main__":
    sys.exit(main())
