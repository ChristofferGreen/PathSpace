# PathSpace

Typed, hierarchical, thread-safe paths for data and computations.

PathSpace is a C++ library inspired by tuplespaces (e.g., Linda) and reactive/dataflow ideas. You insert values or executions at paths, then read or take them—optionally blocking with timeouts. Paths look like a filesystem, and inserts can target multiple nodes using simple glob patterns.

- Typed values (fundamental types, std::string, and most STL containers/structs via Alpaca serialization)
- Executions (lambdas, std::function, function pointers) with Lazy or Immediate scheduling
- Blocking reads with timeouts and non-blocking peeks
- Globbed insert to fan-out to multiple existing subtrees
- Thread-safe from the ground up

Links:
- docs/AI_Architecture.md (design and internals)
- docs/AI_Paths.md (canonical namespace/layout conventions)
- docs/Plan_Overview.md (index of active plan documents and priorities)
- docs/Plan_Distributed_PathSpace.md (distributed mounting/network plan)
- docs/WidgetDeclarativeAPI.md (workflow for the declarative widget runtime; required reading for UI work)
- docs/WidgetDeclarativeFeatureParity.md (migration tracker comparing legacy and declarative widgets)
- docs/WidgetDeclarativeMigrationTracker.md (live status board for inspector/web/consumer adoption of the declarative runtime)
- docs/finished/Plan_PathSpace_Inspector_Finished.md (live PathSpace inspector roadmap; historical record)
- examples/devices_example.cpp (experimental device IO example)
- examples/paint_example.cpp (declarative paint demo + screenshot harness)
- build/docs/html/index.html (Doxygen API Reference)

## Architecture at a Glance
- **Trie-backed space** — `PathSpace` stores data in a concurrent trie of `Node` objects. Each node owns serialized `NodeData` payloads plus queued `Task` executions; inserts append, reads copy, and takes pop without global locks. See “Internal Data” in `docs/AI_Architecture.md`.
- **Paths & globbing** — Strongly-typed `ConcretePath`/`GlobPath` wrappers provide component iterators, pattern matching (`*`, `**`, ranges), and validation (`src/pathspace/path/`).
- **Wait/notify** — Blocking reads register waiters in concrete/glob registries and wake via a `NotificationSink` token; timeouts surface as `Error::Timeout` (`Wait/notify` + `Blocking` in the architecture doc).
- **Layers & PathIO** — Permission-checked views (`PathView`), alias mounts (`PathAlias`), and OS/event bridges live in `src/pathspace/layer/`. Enable macOS backends with `-DENABLE_PATHIO_MACOS=ON` and review the PathIO guidance near the end of `docs/AI_Architecture.md`.
- **Declarative widget runtime** — Declarative helpers in `include/pathspace/ui/declarative/**` mount widgets entirely through PathSpace state. `SP::System::LaunchStandard`, `App::Create`, `Window::Create`, and `Scene::Create` bootstrap the runtime; widgets use `Button::Create`, `List::Create`, etc., instead of hand-built buckets. Legacy imperative builders are compatibility-only and will be removed once downstream consumers migrate (see `docs/WidgetDeclarativeAPI.md`).
- **Canonical namespaces** — `docs/AI_Paths.md` defines system/app/render targets; renderer and presenter plans live in `docs/Plan_SceneGraph_Renderer.md`.

## Quick start

1) Prerequisites
- A C++23 compiler (Clang or GCC)
- CMake ≥ 3.15
- macOS/Linux/Windows

2) Configure & build
```bash
git clone <this-repo>
cd PathSpace
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

3) Run tests (recommended)
```bash
ctest --test-dir build --output-on-failure -j --repeat-until-fail 5 --timeout 20
```
or use the helper that wraps the loop/timeout policy from `docs/AI_Architecture.md`:
```bash
./scripts/compile.sh --loop=5 --timeout=20
```

4) Use in your project
- Link against the `PathSpace` target (installed or via add_subdirectory)
- Include headers from `src/pathspace`:
```cpp
#include <pathspace/PathSpace.hpp>
```

Build options:
- ENABLE_ADDRESS_SANITIZER=ON|OFF
- ENABLE_THREAD_SANITIZER=ON|OFF
- ENABLE_UNDEFINED_SANITIZER=ON|OFF
- ENABLE_PATHIO_MACOS=ON|OFF (compile-time defines for experimental macOS PathIO)
- BUILD_PATHSPACE_EXAMPLES=ON|OFF

Tip: Enable sanitizers when debugging concurrency/path issues, and pair `ENABLE_PATHIO_MACOS=ON` with the PathIO section in `docs/AI_Architecture.md` if you need native device backends.

Scripts:
- ./scripts/compile.sh
- ./scripts/update_compile_commands.sh (keeps compile_commands.json in repo root)
- ./build/pathspace_serve_html [--seed-demo] (serves `/apps/<app>/<view>` plus `/assets/<app>/<relative>` for HTML adapter targets; see docs/serve_html/README.md for the module map and docs/finished/Plan_WebServer_Adapter_Finished.md for deployment details.)
- ./build/paint_example --serve-html / ./build/widgets_example --serve-html (attach HTML mirror targets, seed demo credentials, and launch an embedded `pathspace_serve_html` thread so you can open `http://127.0.0.1:8080/apps/<app>/<view>` in a browser while the native window is running. Use the ServeHtml module guide when extending these flows.)

## HTML bundle export

The declarative examples can emit standalone HTML bundles that mirror
`renderers/<renderer>/targets/html/<view>/output/v1/html/*`. Run either example with
`--export-html <output_dir>` to capture the DOM, CSS, commands, metadata, and asset blobs without
starting the LocalWindow loop:

```
./build/paint_example --export-html out/html/paint
./build/widgets_example --export-html out/html/widgets
```

Each export rewrites the target directory with:

- `dom.html` — the rendered DOM fragment
- `styles.css` — the adapter’s CSS output (may be empty when commands drive the UI)
- `commands.json` — Canvas replay commands used when DOM fallback isn’t possible
- `metadata.txt` — renderer/target/view identifiers, revision, mode, fallback flag, asset count
- `assets_manifest.txt` plus `assets/` — fingerprinted binary payloads (fonts, images, etc.)

`--export-html` implies headless mode and cannot be combined with screenshot/GPU smoke flags. The
files above are what `pathspace_serve_html` and future web adapters expect; copy them into a staging
directory or serve them directly from the prototype server while the distributed PathSpace plumbing
is still under construction.

## Inspector quick start

Once you have built the tree, you can launch the bundled inspector server against any running PathSpace (local or distributed mount) and open it in a browser:

```bash
./build/pathspace_inspector_server \
  --host 0.0.0.0 \
  --port 0 \
  --root /app \
  --max-depth 3 \
  --max-children 64 \
  --diagnostics-root /diagnostics/ui/paint_example \
  --ui-root out/inspector-ui \
  --no-demo
```

- Point `--root` (or the embedded `InspectorHttpServer::Options::snapshot.root`) at the subtree you want to expose.
- When your process already embeds `InspectorHttpServer`, just set the options, call `start()`, and visit `http://<host>:<port>/`—the SPA is served from the same binary.
- Distributed deployments should add the remote mount aliases to `InspectorHttpServer::Options::remote_mounts` (or the CLI equivalent) so the SPA’s remote picker lists each mounted PathSpace together with its live health metrics.

See `docs/finished/Plan_PathSpace_Inspector_Finished.md` for the full endpoint catalog (tree, node, stream, metrics, watchlists, actions) and `docs/AI_Debugging_Playbook.md` §8 for the day-to-day inspector workflow.

## Documentation (Doxygen)
You can generate HTML API documentation into build/docs/html:

- Using the helper script (requires doxygen):
  ```bash
  ./scripts/compile.sh --docs
  ```
  Output: [build/docs/html/index.html](build/docs/html/index.html)

- Using CMake directly:
  ```bash
  cmake -S . -B build -DENABLE_DOXYGEN=ON
  cmake --build build --target docs -j
  ```
  Output: [build/docs/html/index.html](build/docs/html/index.html)

## API at a glance
All operations below are member functions on a PathSpace instance. Assume `PathSpace ps;`.


- Insert a typed value or execution:
  - `ps.insert<"/a/b">(42)`
  - `ps.insert<"/a/f">([](){ return 123; })`
  - `ps.insert<"/a/f">([](){ return 123; })` with compile-time path validation
- Read (copy, non-destructive) or take (destructive):
  - `auto v = ps.read<int>("/a/b");`
  - `auto v = ps.take<int>("/a/b", Block{500ms});`
- Blocking and timeouts:
  - `ps.read<T>(path, Block{timeout})` blocks until available or times out
- Execution scheduling category:
  - Lazy (run on first read) or Immediate (run when inserted)
  - `ps.insert<"/exec">(fn, Lazy{})` or `ps.insert<"/exec">(fn, Immediate{})`
- Peek a future (execution result handle) via the unified read API:
  - `auto fut = ps.read<FutureAny>("/exec");` returns a type-erased FutureAny
- Globbed insert (fan-out to existing subtrees):
  - `ps.insert<"/sensors/*/status">(1)` writes to all matching, already-existing paths

Notes:
- Read/take expect concrete (non-glob) paths.
- Insert supports glob patterns to target multiple existing matches.

## Simple examples

Hello, PathSpace:
```cpp
#include <pathspace/PathSpace.hpp>
using namespace SP;

int main() {
    PathSpace ps;

    // Insert then read
    ps.insert<"/numbers/answer">(42);
    auto v = ps.read<int>("/numbers/answer");
    if (v) {
        // 42
        (void)v.value();
    }

    // Take removes the value
    auto first = ps.take<int>("/numbers/answer");
    auto none  = ps.read<int>("/numbers/answer"); // no value now
}
```

Blocking read with timeout:
```cpp
#include <pathspace/PathSpace.hpp>
#include <chrono>
using namespace SP;
using namespace std::chrono_literals;

int main() {
    PathSpace ps;

    // Times out because nothing is present at the path
    auto v = ps.read<int>("/missing/value", Block{100ms});
    if (!v) {
        // Timed out or not found
    }

    // Insert later from another thread, then read with a larger timeout
    ps.insert<"/ready/value">(7);
    auto r = ps.read<int>("/ready/value", Block{500ms}); // returns 7
}
```

Globbed insert (fan-out to existing matches):
```cpp
#include <pathspace/PathSpace.hpp>
using namespace SP;

int main() {
    PathSpace ps;
    ps.insert<"/sensors/a">(0);
    ps.insert<"/sensors/b">(0);

    // Fan-out to existing children under /sensors/*
    // This will write the value 1 into all concrete children that exist.
    ps.insert<"/sensors/*">(1);

    auto a0 = ps.take<int>("/sensors/a"); // 0
    auto a1 = ps.read<int>("/sensors/a"); // 1
    auto b0 = ps.take<int>("/sensors/b"); // 0
    auto b1 = ps.read<int>("/sensors/b"); // 1
}
```

## Executions (lambdas, std::function, function pointers)

Insert computations that produce values. Read triggers evaluation (Lazy) or retrieves the already-running (Immediate) result.

Lazy execution (runs on first read):
```cpp
#include <pathspace/PathSpace.hpp>
using namespace SP;

int main() {
    PathSpace ps;

    ps.insert<"/compute/lazy">([]() -> int { return 100; }, Lazy{});

    // Triggers the lambda to run, then copies the result
    auto v = ps.read<int>("/compute/lazy", Block{});
    // v.value() == 100
}
```

Immediate execution (scheduled at insert time):
```cpp
#include <pathspace/PathSpace.hpp>
using namespace SP;

int main() {
    PathSpace ps;

    ps.insert<"/compute/imm">([]() -> int { return 1 + 1; }, Immediate{});

    // The computation likely finished already; read returns the result.
    auto v = ps.read<int>("/compute/imm", Block{});
    // v.value() == 2
}
```

Chained computations:
```cpp
#include <pathspace/PathSpace.hpp>
using namespace SP;

int main() {
    PathSpace ps;

    // f3 produces 100
    ps.insert<"/f3">([]{ return 100; });

    // f2 reads f3 and returns +10
    ps.insert<"/f2">([&]{
        return ps.read<int>("/f3", Block{}).value() + 10;
    });

    // f1 reads f2 and returns +1
    ps.insert<"/f1">([&]{
        return ps.read<int>("/f2", Block{}).value() + 1;
    });

    auto v = ps.read<int>("/f1", Block{});
    // v.value() == 111
}
```

Advanced: peek the future for an execution result
```cpp
#include <pathspace/PathSpace.hpp>
using namespace SP;

int main() {
    PathSpace ps;

    ps.insert<"/exec">([]{ return 42; }, Immediate{});

    // Non-blocking peek via unified read; returns Expected<FutureAny>
    auto futExp = ps.read<FutureAny>("/exec");
    if (futExp) {
        auto fut = futExp.value();
        // Wait and copy typed result (caller must know the type)
        int out = 0;
        fut.copy_to(&out);
        // out == 42
    }
}
```

## Data types and serialization

- Fundamental types (int, double, etc.) are serialized directly
- std::string and many STL containers and user-defined structs are serialized via Alpaca (see src/pathspace/type/serialization.hpp)
- For custom structs, Alpaca can usually serialize them by value; see Alpaca's docs for constraints and attributes
- Executions (functions/lambdas) are not serialized; they’re run within the process and their results are stored/served

If you need to take results repeatedly, prefer read (copy) over take (pop). Use take to consume streams or queues where you only want the front element once.

## Threading model

- Reads/writes are thread-safe
- Blocking reads use a wait/notify mechanism; you can pass timeouts with `Block{...}`
- Immediate executions are scheduled on the internal TaskPool when inserted
- Lazy executions are scheduled on first read

Tip: For high-throughput patterns, write with Immediate and read<FutureAny> to coordinate downstream consumers.

## Experimental IO providers (PathIO)

The repository includes experimental providers under `src/pathspace/layer/io` and examples in `examples/devices_example.cpp` and `examples/paint_example.cpp`. These mount path-agnostic IO providers (e.g., mouse, keyboard) into a `PathSpace` tree and serve typed event streams using the canonical `/system/devices/in/*` namespace; see `docs/Plan_SceneGraph_Renderer.md` for app/device path conventions. The paint example combines those providers with the software renderer to offer a minimal mouse-driven canvas.

Sketch:
```cpp
#include <pathspace/PathSpace.hpp>
#include <pathspace/layer/io/PathIOMouse.hpp>
#include <pathspace/layer/io/PathIOKeyboard.hpp>
using namespace SP;

int main() {
    PathSpace ps;

    auto mouse    = std::make_unique<PathIOMouse>(PathIOMouse::BackendMode::Auto);
    auto keyboard = std::make_unique<PathIOKeyboard>(PathIOKeyboard::BackendMode::Auto);

    // Mount providers at app-chosen paths
    auto mret = ps.insert<"/system/devices/in/pointer/default">(std::move(mouse));
    auto kret = ps.insert<"/system/devices/in/text/default">(std::move(keyboard));

    // Read typed events (blocking with timeout)
    auto me = ps.read<SP::PathIOMouse::Event>("/system/devices/in/pointer/default/events", Block{});
    auto ke = ps.read<SP::PathIOKeyboard::Event>("/system/devices/in/text/default/events", Block{});
}
```

After mounting a provider, opt-in to push delivery (and throttling/telemetry) via the shared config nodes:
```cpp
ps.insert("/system/devices/in/pointer/default/config/push/enabled", true);
ps.insert("/system/devices/in/pointer/default/config/push/rate_limit_hz", static_cast<std::uint32_t>(480));
ps.insert("/system/devices/in/pointer/default/config/push/subscribers/your_app", true);
```
Normalized IO Trellis structs live in `<pathspace/io/IoEvents.hpp>` for consumers that want canonical `PointerEvent`, `ButtonEvent`, or `TextEvent` payloads.

Notes:
- These providers are evolving; platform-specific backends may require flags (ENABLE_PATHIO_MACOS)
- The example simulates events if no OS backend is active

See:
- examples/devices_example.cpp
- src/pathspace/layer/io/*

## CMake integration

As a subproject:
```cmake
# In your CMakeLists.txt
add_subdirectory(PathSpace)
target_link_libraries(your_app PRIVATE PathSpace)
target_include_directories(your_app PRIVATE ${CMAKE_SOURCE_DIR}/PathSpace/src) # if needed
```

As an installed library:
```cmake
find_package(PathSpace REQUIRED) # if you export a config in your environment
target_link_libraries(your_app PRIVATE PathSpace)
```

Options:
- ENABLE_ADDRESS_SANITIZER, ENABLE_THREAD_SANITIZER, ENABLE_UNDEFINED_SANITIZER (default OFF)
- ENABLE_PATHIO_MACOS (default OFF)
- BUILD_PATHSPACE_EXAMPLES (default OFF)

## Troubleshooting

- Path must be concrete for read/take; insert can use globs to target existing matches
- Use `Block{...}` for blocking reads; without it, reads are non-blocking and may return NoObjectFound
- When using Immediate executions, ensure you are reading the correct type at the path
- For timeouts or readiness issues, `readFuture` lets you explicitly coordinate on completion

## License

See LICENSE for details.

## Contributing

- Keep docs and code consistent; if you change core behavior or APIs (paths, NodeData, WaitMap, TaskPool, serialization), update docs/AI_Architecture.md
- Run tests: `./scripts/compile.sh && ctest --test-dir build -j`
- PRs should include a short “Purpose” and an “AI Change Log” when applicable
