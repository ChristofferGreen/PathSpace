## Repository Memory

- November 17, 2025 — Reinforced coding preference that PathSpace code should avoid C++ exceptions; use explicit status/error returns instead unless wrapping an unavoidable third-party API.
- November 17, 2025 — Declarative runtime mirrors every `WidgetAction` into `widgets/<id>/events/inbox/queue` plus per-event queues and reports enqueue/drop telemetry under `/system/widgets/runtime/input/metrics/{events_enqueued_total,events_dropped_total}`.
