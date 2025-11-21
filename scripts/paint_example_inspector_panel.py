#!/usr/bin/env python3
"""
Serve a lightweight paint example screenshot panel for inspector prototyping.

The server proxies `pathspace_paint_screenshot_card --json` so browsers can
render severity/mean error without opening PNGs. Intended for local use.
"""

from __future__ import annotations

import argparse
import http.server
import json
import os
import subprocess
import sys
import textwrap
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

PANEL_HTML = """<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <title>Paint Example Screenshot Card</title>
  <style>
    :root {
      color-scheme: light dark;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background-color: #111;
      color: #eee;
    }
    body {
      margin: 2rem auto;
      max-width: 720px;
      padding: 0 1rem;
    }
    header {
      display: flex;
      align-items: baseline;
      gap: 1rem;
    }
    .severity-badge {
      display: inline-flex;
      align-items: center;
      padding: 0.25rem 0.75rem;
      border-radius: 999px;
      font-size: 0.9rem;
      text-transform: uppercase;
      letter-spacing: 0.05em;
      font-weight: 600;
    }
    .severity-healthy { background: #0f5132; color: #d1f7df; }
    .severity-attention { background: #842029; color: #ffd7d7; }
    .severity-missing { background: #343a40; color: #f8f9fa; }
    .severity-waiting { background: #1d2a4d; color: #d6e4ff; }
    section {
      margin-top: 1.5rem;
      padding: 1rem 1.25rem;
      border-radius: 12px;
      background: rgba(255,255,255,0.05);
      border: 1px solid rgba(255,255,255,0.08);
    }
    h2 { margin-top: 0; }
    table {
      width: 100%;
      border-collapse: collapse;
      font-size: 0.9rem;
    }
    th, td {
      border-bottom: 1px solid rgba(255,255,255,0.1);
      padding: 0.4rem 0.2rem;
      text-align: left;
    }
    .muted { opacity: 0.7; font-size: 0.9rem; }
    .error { color: #ff8d8d; font-weight: 600; }
    button {
      background: #2563eb;
      color: #fff;
      border: none;
      border-radius: 8px;
      padding: 0.5rem 1rem;
      font-size: 0.95rem;
      cursor: pointer;
    }
  </style>
</head>
<body>
  <header>
    <h1>Paint Example Screenshot</h1>
    <span id="severity" class="severity-badge severity-missing">loading</span>
  </header>
  <p id="summary" class="muted">Loading latest diagnostics…</p>
  <section>
    <h2>Manifest</h2>
    <div id="manifest"></div>
  </section>
  <section>
    <h2>Last Run</h2>
    <div id="last-run"></div>
  </section>
  <section>
    <h2>Recent Runs</h2>
    <table id="recent-runs"></table>
  </section>
  <p class="muted">
    Data source: <code id="source-path"></code>.
    <button id="refresh-btn">Refresh now</button>
  </p>
  <script>
    const ENDPOINT = "/api/cards/paint-example";
    const SSE_ENDPOINT = "/api/cards/paint-example/events";
    const SEVERITY_CLASS = {
      "healthy": "severity-healthy",
      "attention": "severity-attention",
      "missing": "severity-missing",
      "waiting": "severity-waiting",
    };

    let eventSource = null;
    let reconnectDelay = 2000;

    async function fetchCard() {
      const response = await fetch(ENDPOINT, {cache: "no-store"});
      if (!response.ok) {
        throw new Error(`Inspector request failed: ${response.status}`);
      }
      return response.json();
    }

    function connectEventStream() {
      if (eventSource) {
        eventSource.close();
      }
      eventSource = new EventSource(SSE_ENDPOINT);
      eventSource.addEventListener("card", (event) => {
        reconnectDelay = 2000;
        try {
          const payload = JSON.parse(event.data);
          updateCard(payload);
        } catch (err) {
          console.error("Failed to parse card payload", err);
        }
      });
      eventSource.addEventListener("card-error", (event) => {
        let message = "Inspector error";
        try {
          const payload = JSON.parse(event.data);
          message = payload.message || payload.error || message;
        } catch (err) {
          console.error("Failed to parse error payload", err);
        }
        document.getElementById("summary").innerHTML = `<span class="error">${message}</span>`;
      });
      eventSource.onerror = () => {
        document.getElementById("summary").innerHTML = "<span class=\"error\">SSE disconnected, retrying…</span>";
        refresh().catch(() => {});
        try {
          eventSource.close();
        } catch (err) {
          // ignore
        }
        const delay = reconnectDelay;
        reconnectDelay = Math.min(reconnectDelay * 2, 15000);
        setTimeout(connectEventStream, delay);
      };
    }

    function updateCard(card) {
      const badge = document.getElementById("severity");
      badge.textContent = card.severity || "unknown";
      badge.className = `severity-badge ${SEVERITY_CLASS[card.severity] || "severity-missing"}`;
      document.getElementById("summary").textContent = card.summary || "";
      document.getElementById("source-path").textContent = card.source || "unknown";

      const manifest = card.manifest || {};
      document.getElementById("manifest").innerHTML = `
        <div>Revision: <strong>${manifest.revision ?? "—"}</strong></div>
        <div>Tag: <strong>${manifest.tag ?? "—"}</strong></div>
        <div>Renderer: <strong>${manifest.renderer ?? "—"}</strong></div>
        <div>Frame: <strong>${manifest.width ?? "?"} × ${manifest.height ?? "?"}</strong></div>
        <div>Captured: <strong>${manifest.captured_at ?? "—"}</strong></div>
        <div>Commit: <code>${manifest.commit ?? "—"}</code></div>
        <div>Tolerance: <strong>${manifest.tolerance ?? "—"}</strong></div>
      `;

      const lastRun = card.last_run || {};
      document.getElementById("last-run").innerHTML = `
        <div>Status: <strong>${lastRun.status ?? "—"}</strong></div>
        <div>Timestamp: <strong>${lastRun.timestamp_iso ?? lastRun.timestamp_ns ?? "—"}</strong></div>
        <div>Mean Error: <strong>${lastRun.mean_error ?? "—"}</strong></div>
        <div>Max Δ: <strong>${lastRun.max_channel_delta ?? "—"}</strong></div>
        <div>Hardware Capture: <strong>${String(lastRun.hardware_capture ?? "—")}</strong></div>
        <div>Screenshot: <code>${lastRun.screenshot_path ?? "—"}</code></div>
        <div>Diff: <code>${lastRun.diff_path ?? "—"}</code></div>
      `;

      const rows = (card.recent_runs || []).map(run => `
        <tr>
          <td>${run.timestamp_iso ?? run.timestamp_ns ?? "—"}</td>
          <td>${run.status ?? "—"}</td>
          <td>${run.mean_error ?? "—"}</td>
          <td>${run.max_channel_delta ?? "—"}</td>
          <td>${run.hardware_capture ?? "—"}</td>
        </tr>
      `).join("");
      document.getElementById("recent-runs").innerHTML = `
        <thead>
          <tr><th>Timestamp</th><th>Status</th><th>Mean Error</th><th>Max Δ</th><th>Hardware</th></tr>
        </thead>
        <tbody>${rows}</tbody>
      `;
    }

    async function refresh() {
      document.getElementById("summary").textContent = "Refreshing…";
      try {
        const card = await fetchCard();
        updateCard(card);
      } catch (err) {
        document.getElementById("summary").innerHTML = `<span class="error">${err}</span>`;
      }
    }

    document.getElementById("refresh-btn").addEventListener("click", refresh);
    refresh();
    connectEventStream();
    window.addEventListener("beforeunload", () => {
      if (eventSource) {
        eventSource.close();
      }
    });
  </script>
</body>
</html>
"""

@dataclass
class CardFetchResult:
    ok: bool
    payload: Optional[dict]
    status_code: int
    error_payload: Optional[dict] = None


def parse_args(repo_root: Path) -> argparse.Namespace:
    default_metrics = repo_root / "build" / "test-logs" / "paint_example" / "diagnostics.json"
    default_cli = repo_root / "build" / "pathspace_paint_screenshot_card"
    parser = argparse.ArgumentParser(description="Serve a paint screenshot inspector panel")
    parser.add_argument("--metrics-json", type=Path, default=default_metrics,
                        help="Aggregated metrics JSON (default: %(default)s)")
    parser.add_argument("--cli", type=Path, default=default_cli,
                        help="pathspace_paint_screenshot_card binary (default: %(default)s)")
    parser.add_argument("--bind", default="127.0.0.1", help="Bind address (default: %(default)s)")
    parser.add_argument("--port", type=int, default=8765, help="Port (default: %(default)s)")
    parser.add_argument("--poll-interval", type=float, default=2.0,
                        help="Seconds between SSE polls (default: %(default)s)")
    parser.add_argument("--quiet", action="store_true", help="Reduce server logging")
    return parser.parse_args()


def fetch_card(config) -> CardFetchResult:
    cmd = [
        str(config.cli),
        "--metrics-json",
        str(config.metrics_json),
        "--json",
    ]
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError as exc:
        return CardFetchResult(
            ok=False,
            payload=None,
            status_code=500,
            error_payload={
                "error": "card_cli_not_found",
                "message": f"Failed to launch CLI: {exc}",
            },
        )

    if result.returncode != 0:
        return CardFetchResult(
            ok=False,
            payload=None,
            status_code=502,
            error_payload={
                "error": "card_cli_failure",
                "message": result.stderr.strip() or "CLI returned non-zero status",
                "returncode": result.returncode,
            },
        )

    try:
        card = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        return CardFetchResult(
            ok=False,
            payload=None,
            status_code=500,
            error_payload={
                "error": "card_cli_invalid_json",
                "message": f"CLI emitted invalid JSON: {exc}",
            },
        )

    card.setdefault("source", str(config.metrics_json))
    return CardFetchResult(ok=True, payload=card, status_code=200)


def build_handler(config):
    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, format: str, *args):
            if not config.quiet:
                super().log_message(format, *args)

        def do_GET(self):
            if self.path in ("/", "/index.html"):
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                self.wfile.write(PANEL_HTML.encode("utf-8"))
                return
            if self.path == "/api/cards/paint-example":
                self.handle_card_api()
                return
            if self.path == "/api/cards/paint-example/events":
                self.handle_card_stream()
                return
            self.send_error(404, "Not Found")

        def handle_card_api(self):
            result = fetch_card(config)
            payload = json.dumps(
                result.payload if result.ok else result.error_payload,
                indent=2,
            )
            self.send_response(result.status_code)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(payload.encode("utf-8"))

        def handle_card_stream(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()

            last_good = None
            last_error = None

            while True:
                result = fetch_card(config)
                try:
                    if result.ok and result.payload is not None:
                        serialized = json.dumps(result.payload, separators=(",", ":"))
                        if serialized != last_good:
                            self._write_sse("card", serialized)
                            last_good = serialized
                            last_error = None
                    else:
                        error_payload = result.error_payload or {
                            "error": "card_fetch_failed",
                            "message": "Unknown error",
                        }
                        serialized = json.dumps(error_payload, separators=(",", ":"))
                        if serialized != last_error:
                            self._write_sse("card-error", serialized)
                            last_error = serialized
                    time.sleep(max(0.25, float(config.poll_interval)))
                except (BrokenPipeError, ConnectionResetError):
                    break
                except Exception as exc:  # pragma: no cover - guardrail
                    if not config.quiet:
                        sys.stderr.write(f"[inspector-panel] SSE error: {exc}\n")
                    break

        def _write_sse(self, event: str, data: str) -> None:
            block = f"event: {event}\ndata: {data}\n\n".encode("utf-8")
            self.wfile.write(block)
            self.wfile.flush()

    return Handler


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    args = parse_args(repo_root)
    if not args.metrics_json.exists():
        print(f"[inspector-panel] WARN metrics file not found: {args.metrics_json}", file=sys.stderr)
    if not args.cli.exists():
        print(f"[inspector-panel] ERROR CLI binary not found: {args.cli}", file=sys.stderr)
        return 1

    Handler = build_handler(args)
    server = http.server.ThreadingHTTPServer((args.bind, args.port), Handler)
    print(textwrap.dedent(f"""
        PaintScreenshotCard panel running on http://{args.bind}:{args.port}/
        Metrics JSON: {args.metrics_json}
        Card CLI: {args.cli}
        Press Ctrl+C to exit.
    """).strip())
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[inspector-panel] Shutting down...")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
