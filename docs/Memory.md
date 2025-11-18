## Repository Memory
This file is for remembering architecture details about the PathSPace project so it becomes easier to get up to speed about details later.
No need to store details about a worklog of dates when things where added.

- 2025-11-18: WidgetEventTrellis mutates declarative widget state (button hover/press, toggle checked, slider value/dragging, list hover/selection, tree hover/expanded) and flips `render/dirty` before emitting each `WidgetOp`, so declarative apps stay in sync without handlers touching `widgets/<id>/state/*`.
