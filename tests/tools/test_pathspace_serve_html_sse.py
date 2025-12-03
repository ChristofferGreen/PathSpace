#!/usr/bin/env python3

"""Headless verification for pathspace_serve_html SSE streaming."""

import argparse
import http.client
import json
import sys
import time

from pathspace_serve_html_testutil import (
    find_free_port,
    start_server,
    terminate_process,
    wait_for_health,
)


def parse_sse_block(block: str):
    event_type = None
    data_lines = []
    for line in block.split("\n"):
        if not line:
            continue
        if line.startswith(":"):
            continue
        if line.startswith("event:"):
            event_type = line[len("event:") :].strip()
        elif line.startswith("data:"):
            data_lines.append(line[len("data:") :].strip())
    if not event_type or not data_lines:
        return None
    try:
        payload = json.loads("\n".join(data_lines))
    except json.JSONDecodeError:
        return None
    return {"event": event_type, "data": payload}


def stream_sse_events(response, timeout: float):
    buffer = ""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            chunk = response.read(1)
        except Exception:
            break
        if not chunk:
            time.sleep(0.05)
            continue
        buffer += chunk.decode("utf-8", errors="ignore")
        while "\n\n" in buffer:
            raw, buffer = buffer.split("\n\n", 1)
            event = parse_sse_block(raw)
            if event:
                yield event


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate SSE events from pathspace_serve_html")
    parser.add_argument("--repo-root", required=True)
    parser.add_argument("--server", required=True)
    args = parser.parse_args()

    port = find_free_port()
    extra_args = [
        "--allow-unauthenticated",
        "--seed-demo",
        "--demo-refresh-interval-ms",
        "200",
    ]
    proc = start_server(args.server, port, extra_args)

    try:
        if not wait_for_health(port, timeout=6.0):
            print("Server failed health check", file=sys.stderr)
            return 1

        conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        conn.request("GET", "/apps/demo_web/gallery/events", headers={"Accept": "text/event-stream"})
        resp = conn.getresponse()
        if resp.status != 200:
            body = resp.read().decode("utf-8", errors="ignore")
            print(f"Unexpected SSE status {resp.status}: {body}", file=sys.stderr)
            conn.close()
            return 1

        frames = []
        diagnostics = []
        deadline = time.monotonic() + 6.0
        for event in stream_sse_events(resp, timeout=6.0):
            if event["event"] == "frame":
                frames.append(event["data"])
            elif event["event"] == "diagnostic":
                diagnostics.append(event["data"])
            if len(frames) >= 2 and diagnostics:
                break
            if time.monotonic() >= deadline:
                break

        conn.close()

        if len(frames) < 2:
            print("Expected at least two frame events", file=sys.stderr)
            return 1
        if not diagnostics:
            print("Did not receive any diagnostic events", file=sys.stderr)
            return 1
        if frames[-1].get("revision", 0) <= frames[0].get("revision", 0):
            print("Frame revisions did not advance", file=sys.stderr)
            return 1

        resume_revision = frames[-1].get("revision", 0)
        time.sleep(0.3)

        reconnect_conn = http.client.HTTPConnection("127.0.0.1", port, timeout=5)
        reconnect_conn.request(
            "GET",
            "/apps/demo_web/gallery/events",
            headers={"Accept": "text/event-stream", "Last-Event-ID": str(resume_revision)},
        )
        reconnect_resp = reconnect_conn.getresponse()
        if reconnect_resp.status != 200:
            body = reconnect_resp.read().decode("utf-8", errors="ignore")
            print(f"Reconnect SSE status {reconnect_resp.status}: {body}", file=sys.stderr)
            reconnect_conn.close()
            return 1

        resumed_frame = None
        for event in stream_sse_events(reconnect_resp, timeout=4.0):
            if event["event"] == "frame":
                resumed_frame = event["data"]
                break
        reconnect_conn.close()
        if resumed_frame is None:
            print("Reconnect stream did not deliver a frame", file=sys.stderr)
            return 1
        if resumed_frame.get("revision", 0) <= resume_revision:
            print("Reconnect frame revision did not advance beyond resume id", file=sys.stderr)
            return 1

        return 0
    finally:
        terminate_process(proc)


if __name__ == "__main__":
    sys.exit(main())
