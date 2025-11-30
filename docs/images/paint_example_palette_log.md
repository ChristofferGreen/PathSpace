# Paint Example Palette Log

This log records every intentional palette or screenshot baseline update for
`examples/paint_example`. The `scripts/paint_example_monitor.py` guard now fails
when the manifest revision lacks a matching entry in this file, so always update
the log while refreshing screenshots.

## Update Checklist
1. Capture fresh artifacts via `scripts/paint_example_capture.py --tags 1280x800 paint_720 paint_600`.
2. Update `docs/images/paint_example_baselines.json` and
   `docs/images/paint_example_manifest_revision.txt` together.
3. Re-run `scripts/paint_example_monitor.py --tags 1280x800 paint_720 paint_600` to confirm the new
   revision, then append a section below with the date, commit, intent, and SHA table.
4. Mention the change in `docs/Memory.md` and any relevant plan/status docs.

---

## Revision 8 — 2025-11-30
- Commit: `ab960930665b32c8a5c69b3cf8b2263e563c10a9`
- Reason: Widget theme simplification palette refresh.
- Notes: Metal captures only; see `docs/finished/Plan_WidgetThemeSimplification_Finished.md` Phase 4.
- SHA-256 values:
  - `1280x800`: `7e3b75e8ca9c1268084d7a3c9361f3550dbb621e468397179caaeb6590500215`
  - `paint_720`: `79c1bcd292cfbb59cf3676d371d2c6f4a6dcb812f5f26680f0ba8e957f522454`
  - `paint_600`: `4dcfe4e95725fb47d07aaac54952d37d6c33642d784fe51b59082b2291eb39ad`
