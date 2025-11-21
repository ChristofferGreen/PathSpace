# PathSpace Terminal Plan

> **Drafted:** November 21, 2025 — concept for a Carta Linea-aware terminal that launches apps and visualizes their returned PathSpaces.

## Motivation
- Developers and power users want a textual/command-driven way to launch PathSpace applications beyond the GUI dock.
- Carta Linea already models applications as cards; pairing it with a terminal creates a bridge between textual commands and the card/timeline UI.
- Existing demos print text results only; we want richer visualizers that can render anything a returned PathSpace describes (text, 3D widgets, etc.).

## Goals
1. Provide a terminal emulator UI built on the declarative widget stack (scrollback, prompt, input history).
2. Treat each command as a launcher: commands resolve to application descriptors, spawn the app, and receive a PathSpace handle representing the app’s output.
3. Visualize returned PathSpaces generically — plain text output, structured tables, or embedded 3D scenes — and register each result as a Carta Linea card for later browsing.
4. Keep a live mapping between terminal sessions and Carta Linea decks; closing a terminal card archives it in the timeline.
5. Support programmable commands/scripts (e.g., `.run widgets_example --headless`, `.inspect /system/...`).

## Dependencies
- `docs/Plan_CartaLinea.md` — terminal cards integrate directly with the Carta Linea index/decks.
- Declarative widget runtime for the terminal UI, scrollback, and embedded viewers.
- Window manager/dock (future) for launching the terminal itself.

## Architecture Overview
```
Terminal App (PathSpace) -----------------------------------------------
| widgets/<terminal>/state/input
| widgets/<terminal>/log/lines
| commands queue --> Launcher service --> App starts --> PathSpace handle
| visualizer registry ---------+-> text renderer
                               +-> table/grid visualizer
                               +-> 3D widget embedder
Carta Linea index <----------- publishes card per command result
```
- Terminal UI writes commands to `terminal/ops/command/queue`.
- Launcher service resolves command → application manifest (PathSpace path), starts the app, and captures the returned PathSpace root.
- Visualizer registry matches the returned PathSpace metadata (declared kind, schema hints) to an appropriate renderer.
- Each result publishes into Carta Linea (`index/builds/<rev>`), tagging the originating command, timestamps, and visualization info.

## Command Semantics
- Commands resemble shell invocations (`paint --screenshot`, `inspect /system/io/log/error`).
- Built-in commands manage sessions (`.cards`, `.clear`, `.help`).
- Apps can declare CLI manifests (name, args, output type, visualizer hints).
- Errors are surfaced both as text output and structured diagnostic entries.

## Visualization Strategy
- **Text**: default fallback, pretty-print JSON/PathSpace diffs.
- **Structured data**: table/grid widgets for tabular nodes, tree viewers for hierarchical data.
- **Interactive scenes**: embed scenes returned by the app (e.g., 3D widget, mini canvas) inside the terminal scrollback; click-to-expand to full Carta Linea card.
- Visualizers register under `/system/terminal/visualizers/<kind>` so new apps can opt-in without modifying the core terminal.

## Carta Linea Integration
- Each command result becomes a card under `/users/<user>/system/applications/carta/index/cards/terminal/<id>`.
- Cards reference the terminal session, command string, app metadata, and the visualizer used.
- Timeline entries capture when commands were issued and completed.

## Roadmap
1. **Schema & manifest**
   - Define `terminal/*` nodes (input, output, status) and CLI manifest format for apps.
   - Extend Carta Linea plan with terminal card types and visualizer hooks.
2. **Terminal UI MVP**
   - Basic prompt, scrollback, history navigation, text output.
   - Commands launch stub apps that return text PathSpaces.
3. **Launcher service**
   - Resolve CLI args → app descriptors, manage lifecycles, capture returned PathSpaces.
   - Error handling + timeout policies.
4. **Visualizer registry**
   - Implement text + table + generic scene visualizers.
   - Register visualization metadata with Carta Linea when publishing cards.
5. **Advanced outputs**
   - Support embedded 3D widgets and interactive scenes inside the terminal buffer.
   - Allow expanding a result into a dedicated window/card view.
6. **Scripting & automation**
   - Command scripting, macros, piping outputs between commands.
   - Optional integration with Carta Linea decks for saved command workflows.

## Open Questions
- How do we sandbox commands/apps to prevent runaway processes or huge PathSpaces?
- Should command history and cards sync across devices (requires distributed mounts)?
- How do we version visualizer manifests so older terminals can still render newer app outputs?
- Do we expose terminal sessions via the inspector/web adapter for remote access?
