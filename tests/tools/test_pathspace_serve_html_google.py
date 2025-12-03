#!/usr/bin/env python3

"""Integration test for Google Sign-In flow."""

from __future__ import annotations

import argparse
import http.client
import http.server
import json
import sys
import threading
import urllib.parse
from typing import Dict, Optional

from pathspace_serve_html_testutil import (
    find_free_port,
    start_server,
    terminate_process,
    wait_for_health,
)

ID_TOKEN = (
    "eyJhbGciOiJSUzI1NiIsImtpZCI6InRlc3Qta2V5LTEiLCJ0eXAiOiJKV1QifQ."
    "eyJpc3MiOiJodHRwczovL2FjY291bnRzLmdvb2dsZS5jb20iLCJhdWQiOiJ0ZXN0LWNsaWVudCIs"
    "InN1YiI6Imdvb2dsZS11c2VyLTEyMyIsImVtYWlsIjoiZGVtby51c2VyQGV4YW1wbGUuY29tIiwi"
    "ZW1haWxfdmVyaWZpZWQiOnRydWUsImV4cCI6MTg5MzQ1NjAwMCwiaWF0IjoxODkzNDUwMDAwLCJo"
    "ZCI6ImV4YW1wbGUuY29tIn0.2pt_be-kAiQEOqL2TYnO307hrQb-HS_5KX7uDJAFi-7Sk21xCQwm9"
    "qUmgqK3fyOJTn6SpaKLEpow8ckMbHx0iFZ2C9fo3oEvlPrlTwHPMvVh0GQTrxp4I8EcxG8miZLmJ2"
    "CNyzUPtZnsZ12gjxtdehatHz47F81GloQMp9ml5APxj5Kfce5AVj5oLSuSZE__Mny_dKJ5C2y2l-f"
    "A5GwH3mPiO0pKMpOWvLQ0WYEtsSK9GTkaBWsZGuFTABOgqD6yXgLS1I8RAYxmnwZkmt-AVrFju7r7"
    "9tKMKkP8aflN-LcrgwR0DSBcunD_CDoujxoJ_BgPkhIHqRJLiybQTyhNfA"
)

JWKS_RESPONSE = {
    "keys": [
        {
            "kty": "RSA",
            "kid": "test-key-1",
            "use": "sig",
            "alg": "RS256",
            "n": "8ixyeV2fEW5HoYFslD-NbP48japsoEoY4t27neXTI_lY6Vx-8v1hHAT0ShG-QLXtPmh6gfmVR-RDBYIRtoh1A-jMWjFKB6y__Ps8edJ-dRrgXVNO2NniMh_vtXP43l--HDLhrgAExF7NZ-MdN0dZqt9kInuO2gGYdscKyfAGSA0S7Flmhp9sfnBS3xvzxqIBZgOxtYwXxDAFEvJEURjAeKwFwcUvWfbMNE7grJniHHPPWXLAabEIBE5PDEE0HF7s0lYJCk8l3NL2V-kgXSidIfSAExvW0yaWLg6SBbDPEFX65AZQYGb50-o9c9fL2_8vzbJQqvSYz_1bMOAsTi6rmQ",
            "e": "AQAB",
        }
    ]
}


def make_stub_handler(stub: "GoogleStubServer") -> type[http.server.BaseHTTPRequestHandler]:
    class Handler(http.server.BaseHTTPRequestHandler):
        def do_POST(self) -> None:  # noqa: N802
            if self.path != "/google/token":
                self.send_error(404)
                return

            length = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(length).decode("utf-8", errors="ignore")
            stub.request_bodies.append(body)
            response = {
                "access_token": "stub",
                "expires_in": 3600,
                "token_type": "Bearer",
                "id_token": ID_TOKEN,
            }
            payload = json.dumps(response).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_GET(self) -> None:  # noqa: N802
            if self.path == "/google/jwks":
                payload = json.dumps(JWKS_RESPONSE).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(payload)))
                self.end_headers()
                self.wfile.write(payload)
                return
            if self.path.startswith("/google/auth"):
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.end_headers()
                self.wfile.write(b"stub auth endpoint")
                return
            self.send_error(404)

        def log_message(self, format: str, *args: object) -> None:  # noqa: A003
            return

    return Handler


class GoogleStubServer(threading.Thread):
    def __init__(self, port: int) -> None:
        super().__init__(daemon=True)
        self._port = port
        self._httpd = http.server.HTTPServer(("127.0.0.1", port), make_stub_handler(self))
        self.request_bodies: list[str] = []

    @property
    def port(self) -> int:
        return self._port

    def run(self) -> None:
        self._httpd.serve_forever()

    def stop(self) -> None:
        self._httpd.shutdown()
        self._httpd.server_close()


def request(method: str,
            port: int,
            path: str,
            headers: Optional[Dict[str, str]] = None) -> http.client.HTTPResponse:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
    conn.request(method, path, body=None, headers=headers or {})
    return conn.getresponse()


def main() -> int:
    parser = argparse.ArgumentParser(description="Google Sign-In regression for pathspace_serve_html")
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--server", required=True)
    args = parser.parse_args()

    stub_port = find_free_port()
    stub = GoogleStubServer(stub_port)
    stub.start()

    server_port = find_free_port()
    redirect_uri = f"http://127.0.0.1:{server_port}/login/google/callback"
    extra_args = [
        "--seed-demo",
        "--google-client-id",
        "test-client",
        "--google-client-secret",
        "test-secret",
        "--google-redirect-uri",
        redirect_uri,
        "--google-auth-endpoint",
        f"http://127.0.0.1:{stub_port}/google/auth",
        "--google-token-endpoint",
        f"http://127.0.0.1:{stub_port}/google/token",
        "--google-jwks-endpoint",
        f"http://127.0.0.1:{stub_port}/google/jwks",
    ]
    proc = start_server(args.server, server_port, extra_args)

    try:
        if not wait_for_health(server_port, timeout=6.0):
            print("Server did not become healthy", file=sys.stderr)
            return 1

        login_resp = request(
            "GET",
            server_port,
            "/login/google?redirect=/apps/demo_web/gallery",
        )
        if login_resp.status != 302:
            body = login_resp.read().decode("utf-8", errors="ignore")
            print(f"Expected 302 from /login/google, got {login_resp.status}", file=sys.stderr)
            print(body, file=sys.stderr)
            login_resp.close()
            return 1
        location = login_resp.getheader("Location") or ""
        login_resp.close()
        parsed = urllib.parse.urlparse(location)
        if parsed.port != stub_port or parsed.path != "/google/auth":
            print(f"Unexpected redirect target: {location}", file=sys.stderr)
            return 1
        params = urllib.parse.parse_qs(parsed.query)
        required = {"client_id", "redirect_uri", "response_type", "scope", "state", "code_challenge"}
        if not required.issubset(params.keys()):
            print(f"Missing params in redirect: {params}", file=sys.stderr)
            return 1
        state = params["state"][0]

        callback_resp = request(
            "GET",
            server_port,
            f"/login/google/callback?state={urllib.parse.quote(state)}&code=test_code",
        )
        if callback_resp.status != 302:
            body = callback_resp.read().decode("utf-8", errors="ignore")
            print(f"Expected 302 from callback, got {callback_resp.status}", file=sys.stderr)
            print(body, file=sys.stderr)
            callback_resp.close()
            return 1
        cookie_header = callback_resp.getheader("Set-Cookie")
        location = callback_resp.getheader("Location")
        callback_resp.read()
        callback_resp.close()
        if not cookie_header or not location:
            print("Callback missing Set-Cookie or Location", file=sys.stderr)
            return 1
        if location != "/apps/demo_web/gallery":
            print(f"Unexpected callback redirect target: {location}", file=sys.stderr)
            return 1
        cookie = cookie_header.split(";", 1)[0]

        headers = {"Cookie": cookie}
        html_resp = request("GET", server_port, "/apps/demo_web/gallery", headers)
        body = html_resp.read().decode("utf-8", errors="ignore")
        html_resp.close()
        if html_resp.status != 200 or "<html" not in body.lower():
            print(f"/apps request failed: {html_resp.status}", file=sys.stderr)
            print(body[:200], file=sys.stderr)
            return 1

        if not stub.request_bodies:
            print("Token endpoint was not invoked", file=sys.stderr)
            return 1
        token_params = urllib.parse.parse_qs(stub.request_bodies[0])
        expected_fields = {"code", "client_id", "client_secret", "redirect_uri", "grant_type", "code_verifier"}
        if not expected_fields.issubset(token_params.keys()):
            print(f"Token request missing fields: {token_params}", file=sys.stderr)
            return 1
        if token_params["code"][0] != "test_code":
            print(f"Token request contained wrong code: {token_params['code'][0]}", file=sys.stderr)
            return 1
        if not token_params["code_verifier"][0]:
            print("PKCE code verifier missing", file=sys.stderr)
            return 1

        return 0
    finally:
        terminate_process(proc)
        stub.stop()
        stub.join()


if __name__ == "__main__":
    sys.exit(main())
