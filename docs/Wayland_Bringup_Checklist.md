# Linux/Wayland UI Bring-up Checklist

This document scopes the work required to run the PathSpace UI stack on Linux/Wayland. It targets the existing software renderer and presenter pipeline first, with room for future GPU uploads. Keep edits ASCII and mirror updates back into `docs/finished/Plan_SceneGraph_Finished.md` when milestones complete.

## Goals
- Ship a Wayland-compatible presenter that can display `PathSurfaceSoftware` frames with damage/progressive updates intact.
- Provide input adapters (pointer, keyboard, scroll, text input/IME) that feed the declarative widget runtime without diverging from macOS semantics.
- Make the flow repeatable for contributors: documented build flags, protocol dependencies, smoke tests, and CI hooks.

## Scope and Baseline Assumptions
- Start with software surfaces (`PATHSPACE_UI_SOFTWARE=1`) and the existing renderer buckets. Metal remains disabled (`PATHSPACE_UI_METAL=0`) on Linux.
- No Wayland-specific code exists yet; the first milestone is a functional wl_shm path using the current CPU framebuffer. GPU uploads (EGL/Vulkan/DMABUF) are follow-ups once the presenter contract is stable.
- Preserve existing diagnostics surfaces (`renderers/<rid>/targets/<tid>/output/v1/*`, `diagnostics/metrics`) so tooling and tests remain consistent.

## Environment & Dependencies
- Toolchain: GCC 13+ or Clang 17+ with C++23, Ninja, CMake ≥3.15.
- Wayland stack: `wayland-client`, `wayland-protocols`, `wayland-scanner`, `libxkbcommon`, `libseat` (seat/VT handoff), `mesa` and `wayland-egl` if experimenting with GPU uploads.
- Optional headless compositor for CI/smoke: `weston` (headless backend) or `cage` + `Xvfb` for nested runs.
- Export `PKG_CONFIG_PATH` if protocols live outside the system default; keep protocol XMLs vendored or pinned via `wayland-protocols` to avoid CI drift.

## Build Configuration (initial target)
- `-DPATHSPACE_ENABLE_UI=ON` (unchanged) — required for any UI build.
- `-DPATHSPACE_UI_SOFTWARE=ON` — keep the software path active for wl_shm blits.
- `-DPATHSPACE_UI_METAL=OFF` — avoid Apple-only sources/flags on Linux.
- Introduce `-DPATHSPACE_UI_WAYLAND=ON` (new) to gate the Wayland presenter/adapter sources once added; default to OFF until the code exists.
- Keep `BUILD_PATHSPACE_EXAMPLES=ON` so `paint_example` / `widgets_example` can drive the new presenter during smoke tests.

## Presenter & Surface Tasks
- Add `PathSurfaceWayland` (or equivalent) that wraps a wl_surface + wl_buffer pool.
  - Double-buffering with frame callbacks; match `PresentPolicy` vsync/timeout semantics.
  - wl_shm path: import `PathSurfaceSoftware` front buffer into a wl_buffer, copy only dirty rectangles when progressive tiles are present, honor stride/row padding.
  - DPI/scale: read `wl_output` scale and translate to renderer pixel ratio; ensure snapshots record the scale so HTML/tests stay deterministic.
- Window bridge: small helper to create the display/registry, bind `compositor`, `shm`, `xdg_wm_base`, and `xdg_toplevel`; expose resize/close events back to `WindowRuntime`.
- Future hooks: placeholders for DMA-BUF/EGL images so GPU uploads can reuse the same presenter interface later.

## Input Adapter Tasks
- Pointer: map `wl_pointer` enter/leave/motion/button/axis events to existing widget input ops; mirror macOS scroll deltas (pixel-based) and clamp high-rate wheels.
- Keyboard: use `xkbcommon` for keymap/compose; translate to the existing keycode enums and feed repeat/press/release into `WidgetInputRuntime`.
- Text input/IME: start with `text-input-unstable-v3` (or `input-method-v2`) for composition strings; forward commit/preedit updates to the text widgets’ composition lanes so tests stay aligned with IME flows.
- Seat changes: watch `wl_seat` capabilities and drop inputs when a seat disappears to avoid stale handles.
- Clipboard (stretch goal): wire `wl_data_device` for copy/paste parity with macOS hooks used by text widgets.

## Diagnostics & Tooling
- Mirror per-target present metrics (mode, presented age, progressive counters) into the existing diagnostics paths; confirm `scripts/ui_capture_logs.py` sees the Wayland backend identifier.
- Add a `PATHSPACE_UI_WAYLAND_VERBOSE` toggle that prints bound globals, negotiated formats, and frame callbacks to stderr for triage.
- Ensure `PATHSPACE_UI_DAMAGE_METRICS=1` still populates damage/fingerprint counters when running under Wayland.

## Testing & CI
- Smoke locally with a headless compositor:
  - Start `weston --backend=headless --socket=wayland-test --idle-time=0`.
  - Export `WAYLAND_DISPLAY=wayland-test` and run `./build/widgets_example --exit-after-render 60 --html-server` to exercise present + HTML export without a visible window.
- Add a CI job that installs Wayland deps, starts the headless compositor, configures with `-DPATHSPACE_UI_WAYLAND=ON -DPATHSPACE_UI_METAL=OFF`, builds, and runs `ctest --test-dir build --output-on-failure -j --repeat-until-fail 5 --timeout 20`.
- Capture artifacts: framebuffer dumps from `tests/ui/test_PathRenderer2D.cpp` and any Wayland-specific goldens once present.

## Open Questions / Gaps
- Which text-input protocol variant to standardize on for IME coverage (v3 vs. v1/v2) given CI availability?
- Do we support fractional scaling in the first cut or clamp to integer output scales?
- Minimum GPU story: do we require a GLES path before calling the checklist “done,” or is wl_shm sufficient for MVP parity with software-only macOS runs?
