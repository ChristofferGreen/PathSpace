#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
PLAYWRIGHT_DIR="$ROOT_DIR/tests/inspector/playwright"
LOG_ROOT="$ROOT_DIR/test-logs/inspector_playwright"
mkdir -p "$LOG_ROOT"

LOOPS=5
NPM_CACHE_DIR="$PLAYWRIGHT_DIR/.npm-cache"
BROWSER_CACHE_DIR="$PLAYWRIGHT_DIR/.ms-playwright"

playwright_browser_ready() {
  local browser="$1"
  (
    cd "$PLAYWRIGHT_DIR"
    PLAYWRIGHT_BROWSERS_PATH="$BROWSER_CACHE_DIR" \
      node - "$browser" <<'NODE'
const browserName = process.argv[2];
try {
  const { registry } = require('playwright-core/lib/server/registry/index');
  const executable = registry.findExecutable(browserName);
  if (!executable)
    process.exit(1);
  const executablePath = executable.executablePath();
  if (!executablePath)
    process.exit(1);
  const fs = require('fs');
  fs.accessSync(executablePath, fs.constants.X_OK);
  process.exit(0);
} catch (error) {
  process.exit(1);
}
NODE
  )
}

browsers_ready() {
  local browser
  for browser in "$@"; do
    if ! playwright_browser_ready "$browser"; then
      return 1
    fi
  done
  return 0
}

install_playwright_browsers() {
  local force_flag="${1:-}"
  local args=(npx playwright install)
  if [[ "$force_flag" == "--force" ]]; then
    args+=("--force")
  fi
  args+=("chromium" "chromium-headless-shell")
  (
    cd "$PLAYWRIGHT_DIR"
    PLAYWRIGHT_BROWSERS_PATH="$BROWSER_CACHE_DIR" \
    NPM_CONFIG_CACHE="$NPM_CACHE_DIR" \
      "${args[@]}" >/dev/null
  )
}

ensure_playwright_browsers() {
  install_playwright_browsers
  if browsers_ready chromium chromium-headless-shell; then
    return
  fi

  echo "[inspector-tests] refreshing Playwright browser cache for this host"
  rm -rf "$BROWSER_CACHE_DIR"
  install_playwright_browsers --force
  if ! browsers_ready chromium chromium-headless-shell; then
    echo "error: Playwright browsers are unavailable after reinstall" >&2
    exit 1
  fi
}

pick_port() {
  python3 - <<'PY'
import socket
s = socket.socket()
try:
    s.bind(('127.0.0.1', 0))
    print(s.getsockname()[1])
finally:
    s.close()
PY
}

wait_for_port() {
  local port="$1"
  python3 - <<PY
import socket
import time
port = int(${port})
deadline = time.time() + 5.0
while time.time() < deadline:
    with socket.socket() as s:
        if s.connect_ex(('127.0.0.1', port)) == 0:
            break
    time.sleep(0.1)
else:
    raise SystemExit(1)
PY
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --loop)
      shift || { echo "Missing value for --loop" >&2; exit 1; }
      LOOPS="$1"
      ;;
    --loop=*)
      LOOPS="${1#*=}"
      ;;
    --help|-h)
      cat <<USAGE
Usage: $0 [--loop N]

Runs the inspector Playwright suite. Default loop count: 5.
USAGE
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      exit 1
      ;;
  esac
  shift
done

if ! [[ "$LOOPS" =~ ^[0-9]+$ ]] || [[ "$LOOPS" -lt 1 ]]; then
  echo "error: --loop must be a positive integer" >&2
  exit 1
fi

if [[ ! -d "$PLAYWRIGHT_DIR" ]]; then
  echo "error: Playwright directory not found: $PLAYWRIGHT_DIR" >&2
  exit 1
fi

SERVER_BIN="$ROOT_DIR/build/pathspace_inspector_server"
if [[ ! -x "$SERVER_BIN" ]]; then
  echo "error: inspector server binary missing: $SERVER_BIN" >&2
  echo "       run 'cmake --build build -j pathspace_inspector_server' first" >&2
  exit 1
fi

if [[ ! -d "$PLAYWRIGHT_DIR/node_modules" ]]; then
  echo "[inspector-tests] installing npm dependencies"
  mkdir -p "$NPM_CACHE_DIR"
  (cd "$PLAYWRIGHT_DIR" && NPM_CONFIG_CACHE="$NPM_CACHE_DIR" npm install)
fi

echo "[inspector-tests] ensuring Playwright browsers are installed"
ensure_playwright_browsers

run_loop() {
  local iteration="$1"
  local server_log="$LOG_ROOT/server_loop${iteration}.log"
  local test_log="$LOG_ROOT/tests_loop${iteration}.log"
  : > "$server_log"
  : > "$test_log"

  local chosen_port
  chosen_port=$(pick_port)
  if [[ -z "$chosen_port" ]]; then
    echo "[inspector-tests] failed to reserve a port" >&2
    return 1
  fi

  local server_cmd=("$SERVER_BIN" --host 127.0.0.1 --port "$chosen_port" --enable-test-controls)
  "${server_cmd[@]}" >"$server_log" 2>&1 &
  local server_pid=$!

  if ! wait_for_port "$chosen_port"; then
    echo "[inspector-tests] server failed to bind port $chosen_port" >&2
    kill "$server_pid" >/dev/null 2>&1 || true
    wait "$server_pid" >/dev/null 2>&1 || true
    return 1
  fi

  echo "[inspector-tests] loop $iteration/$LOOPS using port $chosen_port"
  local base_url="http://127.0.0.1:$chosen_port"
  local status=0
  set +e
  (
    set -o pipefail
    cd "$PLAYWRIGHT_DIR"
    PLAYWRIGHT_BROWSERS_PATH="$BROWSER_CACHE_DIR" \
    NPM_CONFIG_CACHE="$NPM_CACHE_DIR" \
    INSPECTOR_TEST_BASE_URL="$base_url" \
      npx playwright test --config=playwright.config.js
  ) 2>&1 | tee -a "$test_log"
  status=${PIPESTATUS[0]}
  set -e

  kill "$server_pid" >/dev/null 2>&1 || true
  wait "$server_pid" >/dev/null 2>&1 || true

  if [[ $status -ne 0 ]]; then
    echo "[inspector-tests] loop $iteration failed (see $test_log)" >&2
    return $status
  fi
  echo "[inspector-tests] loop $iteration passed"
  return 0
}

for ((i = 1; i <= LOOPS; ++i)); do
  if ! run_loop "$i"; then
    exit 1
  fi
  sleep 1
done

echo "[inspector-tests] completed $LOOPS loop(s) successfully"
