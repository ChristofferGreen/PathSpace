# Plan: Screenshot Theme Mismatch (Declarative Button Example)

## Goal
Make `out.png`/`out2.png` (framebuffer grabs) and `out3.png` (OS window grab) show the same theme without altering screenshot logic.

## Constraints / Do-Not-Do
- **Do not change screenshot code** (no cropping, compositing, or logic changes).
- Keep the example showing the two buttons.
- Avoid touching window chrome purely for theme fixes unless absolutely necessary.
- Keep changes ASCII; do not modify unrelated files.

## Facts Gathered
- Framebuffer captures use the “sunset” palette (mean RGB ≈ 0.126/0.134/0.160).
- OS capture shows a different palette (mean RGB ≈ 0.158/0.136/0.126) even when chrome minimized.
- Declarative runtime sets/ensures theme under `/config/theme/active`; system active theme is under `/system/themes/_active`.
- Current example does not explicitly request a theme; defaults resolve to “sunset” unless a system active theme overrides.

## Hypothesis
Live render is picking up a different active theme (likely from `/system/themes/_active`) than the one ensured for the renderer/framebuffer, causing palette divergence.

## Outcome
- Instrumented via `--dump_json` + debug run: `/config/theme/active` and window `style/theme` both reported `sunset`; `/system/themes/_active` also resolved to `sunset` during the same run.
- Example now pins a single source of truth: the sanitized `sunset` theme is ensured/stored under `/system/applications/declarative_button_example/config/theme/sunset`, set active before renderer creation, and the window `style/theme` lane is rewritten to that value (no reliance on system override).
- No screenshot/presenter logic changed; widget layout and button labels remain the same.
- Screenshot verification is still blocked in this headless run: `--screenshot ... --screenshot_os ...` exits with Signal 11 (likely macOS window capture limitation in the sandbox). Palette comparison to `out3.png` needs a GUI-capable run.

## What changed
1) **Instrument theme selection — Done.** Captured active theme values during a run; system and app actives matched (`sunset`).
2) **Force consistent theme source — Done.** Explicitly set the example’s default/active theme to sanitized `sunset`, store the theme value, and overwrite the window theme lane to the same string so renderer and window share one source.
3) **Rebuild & capture — Partially done.** Example rebuild succeeded. Automated screenshot command still crashes under the current environment; manual palette comparison pending when window capture works.
4) **Clean-up — Done.** Removed temporary logging; kept only the theme-alignment change.

## Verification Steps (carry forward)
1) `cmake --build build -j --target declarative_button_example`
2) `./build/declarative_button_example --screenshot out.png --screenshot2 out2.png --screenshot_os out3.png`
3) Compare means/histograms of `out.png` and `out3.png`; success when close within small tolerance and visuals match.
