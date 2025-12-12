#include "inspector/InspectorUiAssets.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace SP::Inspector {
namespace {

constexpr char kEmbeddedIndexHtml[] = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>PathSpace Inspector</title>
  <style>
    :root {
      color-scheme: light dark;
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
      background: #0f1119;
      color: #f3f4f6;
      --focus-ring-color: #facc15;
      --focus-ring-shadow: 0 0 0 3px rgba(250,204,21,0.35);
    }
    body {
      margin: 0;
      padding: 0;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      background: linear-gradient(180deg, rgba(15,17,25,1) 0%, rgba(14,19,32,1) 60%, rgba(9,11,17,1) 100%);
    }
    button:focus-visible,
    input:focus-visible,
    select:focus-visible,
    textarea:focus-visible,
    #tree:focus-visible,
    .remote-status-row .badge:focus-visible,
    .tree-row:focus-visible {
      outline: 2px solid var(--focus-ring-color);
      outline-offset: 2px;
      box-shadow: var(--focus-ring-shadow);
    }
    #tree:focus-visible {
      border-color: rgba(250,204,21,0.5);
    }
    header {
      padding: 1rem 2rem;
      border-bottom: 1px solid rgba(255,255,255,0.08);
      display: flex;
      justify-content: space-between;
      align-items: center;
      flex-wrap: wrap;
      gap: 1rem;
    }
    header h1 {
      margin: 0;
      font-size: 1.35rem;
    }
    main {
      padding: 1rem 2rem 2rem;
      display: flex;
      flex-direction: column;
      gap: 1rem;
      flex: 1;
    }
    .controls {
      display: flex;
      flex-wrap: wrap;
      gap: 0.75rem;
      align-items: flex-end;
    }
    .control {
      display: flex;
      flex-direction: column;
      font-size: 0.85rem;
      gap: 0.4rem;
    }
    .control input {
      min-width: 12rem;
      padding: 0.35rem 0.5rem;
      border-radius: 6px;
      border: 1px solid rgba(255,255,255,0.2);
      background: rgba(255,255,255,0.05);
      color: inherit;
    }
    .control select {
      min-width: 12rem;
      padding: 0.35rem 0.5rem;
      border-radius: 6px;
      border: 1px solid rgba(255,255,255,0.2);
      background: rgba(255,255,255,0.05);
      color: inherit;
    }
    .root-picker {
      display: flex;
      flex-wrap: wrap;
      gap: 0.5rem;
      align-items: center;
    }
    .root-hint {
      font-size: 0.75rem;
      opacity: 0.75;
    }
    .remote-status-row {
      display: flex;
      flex-wrap: wrap;
      gap: 0.4rem;
      margin: 0.5rem 0 1rem;
    }
    .remote-status-row .badge {
      cursor: pointer;
    }
    button {
      background: #2563eb;
      color: #fff;
      border: none;
      border-radius: 8px;
      padding: 0.5rem 1rem;
      font-weight: 600;
      cursor: pointer;
    }
    button.secondary {
      background: rgba(255,255,255,0.12);
    }
    button:disabled {
      opacity: 0.6;
      cursor: not-allowed;
    }
    .layout {
      display: grid;
      grid-template-columns: minmax(260px, 360px) 1fr;
      gap: 1rem;
      flex: 1;
      min-height: 0;
    }
    .panel {
      border-radius: 12px;
      border: 1px solid rgba(255,255,255,0.08);
      background: rgba(15,17,25,0.75);
      padding: 1rem;
      overflow: hidden;
      display: flex;
      flex-direction: column;
      min-height: 0;
    }
    .panel h2 {
      margin: 0 0 0.75rem;
      font-size: 1rem;
    }
    .layout.layout-secondary {
      grid-template-columns: repeat(auto-fit, minmax(280px, 1fr));
      align-items: start;
    }
    .stack {
      display: flex;
      flex-direction: column;
      gap: 0.65rem;
    }
    .button-row {
      display: flex;
      gap: 0.5rem;
      flex-wrap: wrap;
      align-items: center;
    }
    #tree {
      position: relative;
      margin: 0;
      padding: 0;
      overflow: auto;
      font-family: "SFMono-Regular", Consolas, Menlo, monospace;
      font-size: 0.85rem;
      border: 1px solid rgba(255,255,255,0.08);
      border-radius: 10px;
      min-height: 240px;
      background: rgba(0,0,0,0.25);
    }
    .tree-virtual-spacer {
      position: relative;
      width: 100%;
      height: 0;
    }
    .tree-virtual-window {
      position: absolute;
      top: 0;
      left: 0;
      right: 0;
    }
    .tree-row {
      display: flex;
      align-items: center;
      justify-content: space-between;
      gap: 0.5rem;
      height: 28px;
      padding: 0 0.5rem;
      cursor: pointer;
    }
    .tree-row:hover {
      background: rgba(37,99,235,0.35);
    }
    .tree-row.selected {
      background: rgba(79,70,229,0.9);
      color: #fff;
    }
    .tree-label {
      display: flex;
      align-items: center;
      gap: 0.35rem;
      min-width: 0;
    }
    .tree-label-name {
      font-weight: 500;
      white-space: nowrap;
      overflow: hidden;
      text-overflow: ellipsis;
    }
    .tree-meta {
      font-size: 0.75rem;
      opacity: 0.7;
      white-space: nowrap;
    }
    .tree-truncated {
      margin-left: 0.4rem;
    }
    .node-meta {
      font-size: 0.75rem;
      opacity: 0.7;
    }
    pre {
      flex: 1;
      overflow: auto;
      padding: 0.5rem;
      background: rgba(0,0,0,0.35);
      border-radius: 8px;
      margin: 0;
      font-size: 0.85rem;
    }
    .status-bar {
      font-size: 0.85rem;
      opacity: 0.85;
      display: flex;
      justify-content: space-between;
      flex-wrap: wrap;
      gap: 0.5rem;
    }
    .badge {
      padding: 0.2rem 0.6rem;
      border-radius: 999px;
      font-size: 0.75rem;
      text-transform: uppercase;
      letter-spacing: 0.05em;
    }
    .diagnostic-row {
      display: flex;
      flex-wrap: wrap;
      gap: 0.4rem;
      margin: 0.4rem 0 0.25rem;
    }
    .diagnostic-row .badge {
      font-size: 0.7rem;
    }
    .badge.ok {
      background: rgba(16,185,129,0.2);
      color: #34d399;
    }
    .badge.warn {
      background: rgba(251,191,36,0.2);
      color: #fbbf24;
    }
    .badge.err {
      background: rgba(239,68,68,0.2);
      color: #f87171;
    }
    .badge.info {
      background: rgba(59,130,246,0.2);
      color: #93c5fd;
    }
    .badge.muted {
      background: rgba(156,163,175,0.2);
      color: #d1d5db;
    }
    #paint-card {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(220px, 1fr));
      gap: 1rem;
    }
    .card {
      border-radius: 12px;
      border: 1px solid rgba(255,255,255,0.08);
      padding: 1rem;
      background: rgba(0,0,0,0.3);
    }
    code {
      font-family: "SFMono-Regular", Consolas, Menlo, monospace;
      font-size: 0.85rem;
    }
    .list {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: 0.5rem;
      max-height: 360px;
      overflow: auto;
    }
    .empty-state {
      font-size: 0.85rem;
      opacity: 0.75;
      margin: 0.25rem 0 0;
    }
    .search-meta {
      font-size: 0.8rem;
      opacity: 0.8;
      margin-bottom: 0.5rem;
    }
    .search-result {
      border: 1px solid rgba(255,255,255,0.08);
      border-radius: 10px;
      padding: 0.65rem;
      background: rgba(0,0,0,0.35);
      display: flex;
      flex-direction: column;
      gap: 0.4rem;
    }
    .search-result-title {
      display: flex;
      justify-content: space-between;
      gap: 0.5rem;
      align-items: baseline;
      flex-wrap: wrap;
    }
    .watch-entry {
      border: 1px solid rgba(255,255,255,0.08);
      border-radius: 10px;
      padding: 0.65rem;
      background: rgba(0,0,0,0.4);
      display: flex;
      flex-direction: column;
      gap: 0.35rem;
    }
    .watch-header {
      display: flex;
      justify-content: space-between;
      gap: 0.5rem;
      flex-wrap: wrap;
      align-items: center;
    }
    .watch-meta {
      font-size: 0.8rem;
      opacity: 0.8;
    }
    .watch-actions {
      display: flex;
      gap: 0.4rem;
      flex-wrap: wrap;
    }
    button.small {
      padding: 0.35rem 0.75rem;
      font-size: 0.8rem;
    }
    button.danger {
      background: rgba(239,68,68,0.12);
      color: #fca5a5;
    }
    .watch-message {
      font-size: 0.8rem;
      opacity: 0.8;
      margin: 0.35rem 0;
      display: none;
    }
    .watch-message.error {
      color: #f4a261;
    }
    .saved-watchlists {
      border-top: 1px solid rgba(255,255,255,0.1);
      margin-top: 1rem;
      padding-top: 1rem;
    }
    .saved-watchlists label {
      display: block;
      font-weight: 600;
      margin-bottom: 0.4rem;
    }
    .saved-watchlists select {
      width: 100%;
      padding: 0.45rem;
      border-radius: 6px;
      border: 1px solid rgba(255,255,255,0.2);
      background: rgba(0,0,0,0.3);
      color: inherit;
    }
    .saved-watchlists .button-row {
      margin-top: 0.5rem;
    }
    .saved-watchlists small {
      display: block;
      margin-top: 0.3rem;
      opacity: 0.75;
      font-size: 0.75rem;
    }
    .snapshot-message {
      min-height: 1.25rem;
      font-size: 0.85rem;
      margin: 0.5rem 0;
      color: rgba(255,255,255,0.85);
    }
    .snapshot-message.error {
      color: #f87171;
    }
    .snapshot-list {
      list-style: none;
      margin: 0.5rem 0 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: 0.4rem;
      max-height: 200px;
      overflow: auto;
    }
    .snapshot-list li {
      border: 1px solid rgba(255,255,255,0.1);
      border-radius: 8px;
      padding: 0.5rem 0.65rem;
      display: flex;
      flex-direction: column;
      gap: 0.2rem;
      background: rgba(255,255,255,0.02);
    }
    .snapshot-list li strong {
      font-size: 0.9rem;
    }
    .snapshot-meta {
      font-size: 0.75rem;
      opacity: 0.75;
    }
    .snapshot-select-row {
      display: flex;
      flex-wrap: wrap;
      gap: 0.4rem;
      align-items: center;
      margin: 0.4rem 0;
    }
    .snapshot-select-row select {
      min-width: 10rem;
      padding: 0.35rem;
      border-radius: 6px;
      border: 1px solid rgba(255,255,255,0.2);
      background: rgba(0,0,0,0.3);
      color: inherit;
    }
    .write-warning {
      font-size: 0.85rem;
      margin: 0.5rem 0;
      color: #fbbf24;
    }
    .write-warning.ok {
      color: #34d399;
    }
    .write-warning.error {
      color: #f87171;
    }
    .write-toggle-list {
      list-style: none;
      margin: 0;
      padding: 0;
      display: flex;
      flex-direction: column;
      gap: 0.5rem;
    }
    .write-toggle {
      border: 1px solid rgba(255,255,255,0.1);
      border-radius: 10px;
      padding: 0.65rem;
      background: rgba(0,0,0,0.35);
      display: flex;
      flex-direction: column;
      gap: 0.35rem;
    }
    .write-toggle-header {
      display: flex;
      justify-content: space-between;
      gap: 0.5rem;
      flex-wrap: wrap;
      align-items: center;
    }
    .write-toggle-path {
      font-size: 0.75rem;
      opacity: 0.75;
    }
    .write-toggle-description {
      font-size: 0.8rem;
      opacity: 0.85;
      margin: 0;
    }
    .snapshot-diff pre {
      min-height: 6rem;
    }
    .metrics-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(180px, 1fr));
      gap: 0.75rem;
    }
    .metric {
      border: 1px solid rgba(255,255,255,0.08);
      border-radius: 10px;
      padding: 0.75rem;
      background: rgba(0,0,0,0.35);
      display: flex;
      flex-direction: column;
      gap: 0.35rem;
    }
    .metric-label {
      font-size: 0.8rem;
      opacity: 0.8;
    }
    .metric-value {
      font-size: 1.35rem;
      font-weight: 600;
    }
    .metric-note {
      font-size: 0.8rem;
      opacity: 0.75;
      margin-top: 0.75rem;
    }
    .mailbox-table {
      border: 1px solid rgba(255,255,255,0.08);
      border-radius: 10px;
      overflow: hidden;
    }
    .mailbox-header,
    .mailbox-row {
      display: grid;
      grid-template-columns: 1.6fr 0.8fr 0.8fr 0.8fr 1.2fr;
      gap: 0.5rem;
      padding: 0.5rem 0.75rem;
      align-items: center;
    }
    .mailbox-header {
      background: rgba(255,255,255,0.05);
      font-size: 0.75rem;
      text-transform: uppercase;
      letter-spacing: 0.02em;
      font-weight: 700;
    }
    .mailbox-row:nth-child(odd) {
      background: rgba(255,255,255,0.02);
    }
    .mailbox-path {
      word-break: break-all;
      font-size: 0.85rem;
    }
    .mailbox-topics {
      display: flex;
      flex-wrap: wrap;
      gap: 0.35rem;
    }
    .pill {
      border-radius: 999px;
      padding: 0.2rem 0.5rem;
      background: rgba(255,255,255,0.08);
      font-size: 0.75rem;
      white-space: nowrap;
    }
    @media (max-width: 900px) {
      .layout {
        grid-template-columns: 1fr;
      }
      .control input {
        min-width: auto;
      }
    }
  </style>
</head>
<body>
  <header>
    <h1>PathSpace Inspector</h1>
    <div class="status-bar" style="width: auto;" role="status" aria-live="polite" aria-atomic="true">
      <span id="status-text">waiting for snapshot…</span>
      <span id="status-badge" class="badge warn">Idle</span>
    </div>
  </header>
  <main>
    <form class="controls" id="snapshot-form">
      <label class="control">
        Quick root
        <div class="root-picker">
          <select id="root-select">
            <option value="__custom__">Loading roots…</option>
          </select>
          <span id="root-status" class="badge muted">Local</span>
        </div>
        <span class="root-hint" id="root-hint"></span>
      </label>
      <label class="control">
        Root path
        <input type="text" id="root-input" value="/" autocomplete="off" />
      </label>
      <label class="control">
        Max depth
        <input type="number" id="depth-input" min="0" max="16" value="2" />
      </label>
      <label class="control">
        Max children
        <input type="number" id="children-input" min="1" max="256" value="64" />
      </label>
      <div class="control" style="flex-direction: row; gap: 0.5rem; align-items: center;">
        <button type="submit" id="refresh-tree">Refresh tree</button>
        <button type="button" class="secondary" id="refresh-paint">Refresh paint card</button>
      </div>
    </form>

    <div class="remote-status-row" id="remote-statuses" role="group" aria-label="Remote mount quick-select">
      <span class="badge muted">Remote mounts disabled</span>
    </div>

    <div class="layout">
      <section class="panel" data-panel-id="tree">
        <h2>Tree</h2>
        <div id="tree" class="tree-virtual-viewport" role="tree" aria-label="Path tree" aria-multiselectable="false" tabindex="0">
          <div id="tree-virtual-spacer">
            <div id="tree-virtual-window"></div>
          </div>
        </div>
        <p id="tree-empty" class="empty-state">Load a snapshot to view the tree.</p>
      </section>
      <section class="panel" data-panel-id="details">
        <h2>Node details</h2>
        <div class="status-bar" style="margin-bottom: 0.75rem;" role="status" aria-live="polite" aria-atomic="true">
          <span id="selected-path">Select a node to view details.</span>
          <span id="node-badge" class="badge warn">No node</span>
        </div>
        <pre id="node-json">{}</pre>
      </section>
    </div>

    <div class="layout layout-secondary">
      <section class="panel" data-panel-id="stream">
        <h2>Stream health</h2>
        <div class="metrics-grid">
          <div class="metric">
            <div class="metric-label">Queue depth</div>
            <div class="metric-value" id="metric-queue-depth">0</div>
            <span class="badge muted" id="metric-queue-badge">Idle</span>
          </div>
          <div class="metric">
            <div class="metric-label">Max depth seen</div>
            <div class="metric-value" id="metric-max-queue">0</div>
            <span class="badge muted" id="metric-max-badge">Tracked</span>
          </div>
          <div class="metric">
            <div class="metric-label">Dropped events</div>
            <div class="metric-value" id="metric-dropped">0</div>
            <span class="badge muted" id="metric-drop-badge">Clean</span>
          </div>
          <div class="metric">
            <div class="metric-label">Snapshot resets</div>
            <div class="metric-value" id="metric-resent">0</div>
            <span class="badge muted" id="metric-resent-badge">None</span>
          </div>
        </div>
        <div class="metrics-grid" style="margin-top:0.75rem;">
          <div class="metric">
            <div class="metric-label">Active sessions</div>
            <div class="metric-value" id="metric-active-sessions">0</div>
          </div>
          <div class="metric">
            <div class="metric-label">Total sessions</div>
            <div class="metric-value" id="metric-total-sessions">0</div>
          </div>
          <div class="metric">
            <div class="metric-label">Disconnects</div>
            <div class="metric-value" id="metric-disconnect-total">0</div>
            <span class="badge muted" id="metric-disconnect-badge">None</span>
          </div>
        </div>
        <p class="metric-note" id="metric-limit-note">Queue cap — · idle timeout —</p>
        <p class="metric-note" id="metric-disconnect-note">No disconnects recorded.</p>
      </section>
      <section class="panel" data-panel-id="mailbox">
        <h2>Capsule mailboxes</h2>
        <div class="metrics-grid">
          <div class="metric">
            <div class="metric-label">Widgets</div>
            <div class="metric-value" id="mailbox-widget-count">0</div>
            <span class="badge muted" id="mailbox-widget-badge">No mailboxes</span>
          </div>
          <div class="metric">
            <div class="metric-label">Events</div>
            <div class="metric-value" id="mailbox-events-total">0</div>
            <span class="badge muted" id="mailbox-events-badge">Idle</span>
          </div>
          <div class="metric">
            <div class="metric-label">Dispatch failures</div>
            <div class="metric-value" id="mailbox-failures-total">0</div>
            <span class="badge muted" id="mailbox-failures-badge">Clean</span>
          </div>
          <div class="metric">
            <div class="metric-label">Last event</div>
            <div class="metric-value" id="mailbox-last-event">—</div>
            <span class="badge muted" id="mailbox-last-kind">None</span>
          </div>
        </div>
        <p class="metric-note" id="mailbox-summary-note">No mailbox metrics loaded.</p>
        <div class="mailbox-table" aria-live="polite">
          <div class="mailbox-header">
            <span>Widget path</span>
            <span>Kind</span>
            <span>Events</span>
            <span>Failures</span>
            <span>Topics</span>
          </div>
          <div id="mailbox-rows"></div>
        </div>
        <p id="mailbox-empty" class="empty-state">No capsule mailboxes found.</p>
      </section>
      <section class="panel" data-panel-id="search">
        <h2>Search</h2>
        <form class="stack" id="search-form">
          <label class="control">
            Query
            <input type="text" id="search-input" placeholder="Path, type, or summary" autocomplete="off" />
          </label>
          <div class="button-row">
            <button type="submit" id="run-search">Run search</button>
            <button type="button" class="secondary" id="search-clear">Clear</button>
          </div>
        </form>
        <div class="search-meta" id="search-count">Enter a query to search the current snapshot.</div>
        <div class="diagnostic-row" id="search-metrics">
          <span class="badge muted" id="search-query-badge">0 queries</span>
          <span class="badge muted" id="search-latency-badge">No samples</span>
          <span class="badge muted" id="search-truncate-badge">No truncation</span>
        </div>
        <ul id="search-results" class="list search-results"></ul>
      </section>

      <section class="panel" data-panel-id="watchlist">
        <h2>Watchlist</h2>
        <form class="stack" id="watch-form">
          <label class="control">
            Path
            <input type="text" id="watch-input" placeholder="/app/state/path" autocomplete="off" />
          </label>
          <div class="button-row">
            <button type="submit">Add path</button>
          </div>
        </form>
        <p id="watchlist-message" class="watch-message" role="status" aria-live="polite" aria-atomic="true"></p>
        <div class="diagnostic-row" id="watch-metrics">
          <span class="badge muted" id="watch-live-badge">Live 0</span>
          <span class="badge muted" id="watch-missing-badge">Missing 0</span>
          <span class="badge muted" id="watch-truncate-badge">Truncated 0</span>
          <span class="badge muted" id="watch-scope-badge">Out of scope 0</span>
          <span class="badge muted" id="watch-unknown-badge">Unknown 0</span>
        </div>
        <div class="saved-watchlists">
          <label for="watch-saved-select">Saved watchlists</label>
          <select id="watch-saved-select" disabled>
            <option value="">No saved watchlists</option>
          </select>
          <small id="watch-saved-note">Save frequently watched paths for reuse.</small>
          <div class="button-row">
            <button type="button" class="secondary" id="watch-save">Save current</button>
            <button type="button" class="secondary" id="watch-save-new">Save as new</button>
          </div>
          <div class="button-row">
            <button type="button" class="secondary" id="watch-load">Load selection</button>
            <button type="button" class="secondary danger" id="watch-delete">Delete</button>
          </div>
          <div class="button-row">
            <button type="button" class="secondary" id="watch-export">Export</button>
            <button type="button" class="secondary" id="watch-import-button">Import</button>
            <input type="file" id="watch-import" accept="application/json" style="display:none" />
          </div>
        </div>
        <p id="watchlist-empty" class="empty-state">No watched paths yet.</p>
        <ul id="watchlist" class="list watchlist"></ul>
      </section>
      <section class="panel" data-panel-id="snapshots">
        <h2>Snapshots</h2>
        <form class="stack" id="snapshot-capture-form">
          <label class="control">
            Label
            <input type="text" id="snapshot-label" placeholder="Search baseline" autocomplete="off" />
          </label>
          <label class="control">
            Note (optional)
            <input type="text" id="snapshot-note" placeholder="Why capture this snapshot?" autocomplete="off" />
          </label>
          <div class="button-row">
            <button type="submit">Capture snapshot</button>
            <button type="button" class="secondary" id="snapshot-refresh">Refresh list</button>
          </div>
        </form>
        <p id="snapshot-message" class="snapshot-message" role="status" aria-live="polite" aria-atomic="true"></p>
        <div class="saved-watchlists">
          <label for="snapshot-select">Saved snapshots</label>
          <select id="snapshot-select" disabled>
            <option value="">No snapshots captured</option>
          </select>
          <small>Snapshots capture the current tree bounds and exporter JSON for bug reports.</small>
          <div class="button-row">
            <button type="button" class="secondary" id="snapshot-export">Download JSON</button>
            <button type="button" class="secondary danger" id="snapshot-delete">Delete</button>
          </div>
        </div>
        <p id="snapshot-empty" class="empty-state">No snapshots captured yet.</p>
        <ul id="snapshot-list" class="snapshot-list"></ul>
        <div class="snapshot-diff">
          <label>Compare snapshots</label>
          <div class="snapshot-select-row">
            <select id="snapshot-before" disabled aria-label="Compare snapshot A"></select>
            <select id="snapshot-after" disabled aria-label="Compare snapshot B"></select>
            <button type="button" class="secondary" id="snapshot-diff-button">Diff</button>
          </div>
          <pre id="snapshot-diff-output" tabindex="0" aria-label="Snapshot diff output">Select two snapshots to see differences.</pre>
        </div>
      </section>
      <section class="panel" data-panel-id="write">
        <h2>Admin write toggles</h2>
        <p id="write-warning" class="write-warning">Write controls disabled for this session.</p>
        <div class="button-row">
          <button type="button" class="secondary" id="write-refresh">Refresh</button>
          <button type="button" class="danger" id="write-enable">Enable session writes</button>
          <button type="button" class="secondary" id="write-disable" disabled>Disable</button>
        </div>
        <p id="write-note" class="write-toggle-description">Admin role required. Every action is logged to diagnostics.</p>
        <ul id="write-toggle-list" class="write-toggle-list"></ul>
      </section>
    </div>

    <section class="panel" data-panel-id="paint">
      <h2>Paint screenshot status</h2>
      <div id="paint-card" class="card-grid">
        <div class="card">
          <h3 style="margin-top:0;">Summary</h3>
          <p id="paint-summary">not loaded</p>
        </div>
        <div class="card">
          <h3 style="margin-top:0;">Manifest</h3>
          <pre id="paint-manifest">—</pre>
        </div>
        <div class="card">
          <h3 style="margin-top:0;">Last run</h3>
          <pre id="paint-last-run">—</pre>
        </div>
      </div>
    </section>
  </main>
  <script>
    (() => {
      const SEARCH_RESULT_LIMIT = 200;
      const TREE_ROW_HEIGHT = 28;
      const TREE_OVERSCAN_ROWS = 24;
      const TREE_INDENT_PX = 16;
      const TREE_ITEM_ID_PREFIX = "tree-item-";
      const LAZY_PANEL_THRESHOLD = 0.2;
      const scheduleFrame = typeof window !== "undefined" && window.requestAnimationFrame
        ? window.requestAnimationFrame.bind(window)
        : (fn) => setTimeout(fn, 16);
      const METRICS_REFRESH_MS = 5000;
      const MAILBOX_METRICS_ENDPOINT = "/inspector/metrics/mailbox";
      const SEARCH_METRICS_ENDPOINT = "/inspector/metrics/search";
      const REMOTE_STATUS_ENDPOINT = "/inspector/remotes";
      const REMOTE_STATUS_REFRESH_MS = 15000;
      const ROOT_STORAGE_KEY = "pathspace.inspector.root";
      const REMOTE_ROOT_PATH = "/remote";
      const WRITE_TOGGLE_ENDPOINT = "/inspector/actions/toggles";
      const WRITE_CONFIRM_HEADER_DEFAULT = "x-pathspace-inspector-write-confirmed";
      const WRITE_CONFIRM_TOKEN_DEFAULT = "true";
      const WRITE_ENABLE_PASSPHRASE = "ENABLE";
      const USAGE_METRICS_ENDPOINT = "/inspector/metrics/usage";
      const USAGE_FLUSH_INTERVAL_MS = 30000;
      const USAGE_PANEL_THRESHOLD = 0.35;

      const state = {
        root: "/",
        maxDepth: 2,
        maxChildren: 64,
        loadingTree: false,
        selectedPath: null,
        liveTree: null,
        treeRows: [],
        treeRowIndex: null,
        treeIndexDirty: true,
        treeRowsVersion: 0,
        treeRenderQueued: false,
        treeErrorMessage: null,
        eventSource: null,
        liveVersion: 0,
        searchQuery: "",
        watchlist: [],
        savedWatchlists: [],
        selectedWatchlistId: null,
        snapshots: [],
        selectedSnapshotId: null,
        snapshotDiffBeforeId: null,
        snapshotDiffAfterId: null,
        metricsTimer: null,
        searchMetricsTimer: null,
        searchMetrics: null,
        lastWatchMetricsPayload: null,
        remoteMounts: [],
        remoteStatusTimer: null,
        defaultRoot: "/",
        aclError: null,
        blockedStreamRoot: null,
        writeActions: [],
        writeActionsAllowed: false,
        writeSessionEnabled: false,
        writeConfirmationHeader: WRITE_CONFIRM_HEADER_DEFAULT,
        writeConfirmationToken: WRITE_CONFIRM_TOKEN_DEFAULT,
        usagePanels: new Map(),
        usageObserver: null,
        usageFlushTimer: null,
      };

      const elements = {
        tree: document.getElementById("tree"),
        treeSpacer: document.getElementById("tree-virtual-spacer"),
        treeWindow: document.getElementById("tree-virtual-window"),
        treeEmpty: document.getElementById("tree-empty"),
        nodeJson: document.getElementById("node-json"),
        selectedPath: document.getElementById("selected-path"),
        nodeBadge: document.getElementById("node-badge"),
        statusText: document.getElementById("status-text"),
        statusBadge: document.getElementById("status-badge"),
        rootSelect: document.getElementById("root-select"),
        rootStatusBadge: document.getElementById("root-status"),
        rootHint: document.getElementById("root-hint"),
        rootInput: document.getElementById("root-input"),
        depthInput: document.getElementById("depth-input"),
        childrenInput: document.getElementById("children-input"),
        remoteStatusRow: document.getElementById("remote-statuses"),
        searchForm: document.getElementById("search-form"),
        searchInput: document.getElementById("search-input"),
        searchResults: document.getElementById("search-results"),
        searchCount: document.getElementById("search-count"),
        searchClear: document.getElementById("search-clear"),
        searchQueryBadge: document.getElementById("search-query-badge"),
        searchLatencyBadge: document.getElementById("search-latency-badge"),
        searchTruncateBadge: document.getElementById("search-truncate-badge"),
        watchPanel: document.querySelector('[data-panel-id="watchlist"]'),
        watchForm: document.getElementById("watch-form"),
        watchInput: document.getElementById("watch-input"),
        watchlist: document.getElementById("watchlist"),
        watchlistEmpty: document.getElementById("watchlist-empty"),
        watchMessage: document.getElementById("watchlist-message"),
        savedSelect: document.getElementById("watch-saved-select"),
        watchImportInput: document.getElementById("watch-import"),
        watchLiveBadge: document.getElementById("watch-live-badge"),
        watchMissingBadge: document.getElementById("watch-missing-badge"),
        watchTruncateBadge: document.getElementById("watch-truncate-badge"),
        watchScopeBadge: document.getElementById("watch-scope-badge"),
        watchUnknownBadge: document.getElementById("watch-unknown-badge"),
        snapshotPanel: document.querySelector('[data-panel-id="snapshots"]'),
        snapshotCaptureForm: document.getElementById("snapshot-capture-form"),
        snapshotLabel: document.getElementById("snapshot-label"),
        snapshotNote: document.getElementById("snapshot-note"),
        snapshotMessage: document.getElementById("snapshot-message"),
        snapshotSelect: document.getElementById("snapshot-select"),
        snapshotRefresh: document.getElementById("snapshot-refresh"),
        snapshotExport: document.getElementById("snapshot-export"),
        snapshotDelete: document.getElementById("snapshot-delete"),
        snapshotList: document.getElementById("snapshot-list"),
        snapshotEmpty: document.getElementById("snapshot-empty"),
        snapshotBefore: document.getElementById("snapshot-before"),
        snapshotAfter: document.getElementById("snapshot-after"),
        snapshotDiffButton: document.getElementById("snapshot-diff-button"),
        snapshotDiffOutput: document.getElementById("snapshot-diff-output"),
        metricQueue: document.getElementById("metric-queue-depth"),
        metricQueueBadge: document.getElementById("metric-queue-badge"),
        metricMaxQueue: document.getElementById("metric-max-queue"),
        metricMaxBadge: document.getElementById("metric-max-badge"),
        metricDropped: document.getElementById("metric-dropped"),
        metricDropBadge: document.getElementById("metric-drop-badge"),
        metricResent: document.getElementById("metric-resent"),
        metricResentBadge: document.getElementById("metric-resent-badge"),
        metricActiveSessions: document.getElementById("metric-active-sessions"),
        metricTotalSessions: document.getElementById("metric-total-sessions"),
        metricDisconnectTotal: document.getElementById("metric-disconnect-total"),
        metricDisconnectBadge: document.getElementById("metric-disconnect-badge"),
        metricDisconnectNote: document.getElementById("metric-disconnect-note"),
        metricLimitNote: document.getElementById("metric-limit-note"),
        mailboxWidgetCount: document.getElementById("mailbox-widget-count"),
        mailboxWidgetBadge: document.getElementById("mailbox-widget-badge"),
        mailboxEventsTotal: document.getElementById("mailbox-events-total"),
        mailboxEventsBadge: document.getElementById("mailbox-events-badge"),
        mailboxFailuresTotal: document.getElementById("mailbox-failures-total"),
        mailboxFailuresBadge: document.getElementById("mailbox-failures-badge"),
        mailboxLastEvent: document.getElementById("mailbox-last-event"),
        mailboxLastKind: document.getElementById("mailbox-last-kind"),
        mailboxSummaryNote: document.getElementById("mailbox-summary-note"),
        mailboxRows: document.getElementById("mailbox-rows"),
        mailboxEmpty: document.getElementById("mailbox-empty"),
        writePanel: document.querySelector('[data-panel-id="write"]'),
        writeToggleList: document.getElementById("write-toggle-list"),
        writeWarning: document.getElementById("write-warning"),
        writeEnableButton: document.getElementById("write-enable"),
        writeDisableButton: document.getElementById("write-disable"),
        writeRefreshButton: document.getElementById("write-refresh"),
        writeNote: document.getElementById("write-note"),
        paintPanel: document.querySelector('[data-panel-id="paint"]'),
        summary: document.getElementById("paint-summary"),
        manifest: document.getElementById("paint-manifest"),
        lastRun: document.getElementById("paint-last-run"),
      };

      if (elements.tree) {
        if (!elements.tree.hasAttribute("tabindex")) {
          elements.tree.tabIndex = 0;
        }
        if (!elements.tree.hasAttribute("role")) {
          elements.tree.setAttribute("role", "tree");
          elements.tree.setAttribute("aria-multiselectable", "false");
        }
        elements.tree.addEventListener("keydown", handleTreeKeyDown);
      }

      const lazyPanels = new Map();
      const lazyPanelObserver = typeof IntersectionObserver !== "undefined"
        ? new IntersectionObserver(entries => {
            entries.forEach(entry => {
              if (!entry.isIntersecting) {
                return;
              }
              const id = entry.target?.dataset?.panelId;
              if (id) {
                activatePanel(id);
                lazyPanelObserver.unobserve(entry.target);
              }
            });
          }, { threshold: LAZY_PANEL_THRESHOLD })
        : null;

      let watchMessageTimeout = null;
      let snapshotMessageTimeout = null;

      function setStatus(text, badgeClass, badgeLabel) {
        elements.statusText.textContent = text;
        elements.statusBadge.className = "badge " + badgeClass;
        elements.statusBadge.textContent = badgeLabel;
      }

      function setBadge(element, badgeClass, text) {
        if (!element) {
          return;
        }
        element.className = "badge " + badgeClass;
        element.textContent = text;
      }

      function applyPostSnapshotStatus() {
        const eventSourceActive = Boolean(state.eventSource);
        const eventSourceSupported = typeof window !== "undefined" && typeof window.EventSource !== "undefined" && window.EventSource;
        if (eventSourceActive) {
          setStatus("Streaming snapshot", "ok", "Live");
        } else if (!eventSourceSupported) {
          setStatus("Manual refresh ready", "warn", "Polling");
        } else {
          setStatus("Snapshot loaded", "ok", "Ready");
        }
      }

      function safeStorageGet(key) {
        try {
          if (!window.localStorage) {
            return null;
          }
          return window.localStorage.getItem(key);
        } catch (error) {
          console.warn("localStorage read failed", error);
          return null;
        }
      }

      function safeStorageSet(key, value) {
        try {
          if (!window.localStorage) {
            return;
          }
          window.localStorage.setItem(key, value);
        } catch (error) {
          console.warn("localStorage write failed", error);
        }
      }

      function registerLazyPanel(id, element, onActivate) {
        if (!id || !element) {
          return;
        }
        if (lazyPanels.has(id)) {
          return;
        }
        lazyPanels.set(id, {
          id,
          element,
          onActivate,
          active: false,
          pending: [],
        });
        if (lazyPanelObserver) {
          lazyPanelObserver.observe(element);
        } else {
          activatePanel(id);
        }
      }

      function activatePanel(id) {
        const entry = lazyPanels.get(id);
        if (!entry || entry.active) {
          return;
        }
        entry.active = true;
        if (typeof entry.onActivate === "function") {
          try {
            entry.onActivate();
          } catch (error) {
            console.error("Lazy panel activation failed", error);
          }
        }
        if (Array.isArray(entry.pending) && entry.pending.length) {
          const pending = entry.pending.splice(0);
          pending.forEach(task => {
            try {
              task();
            } catch (error) {
              console.error("Lazy panel pending task failed", error);
            }
          });
        }
      }

      function isPanelActive(id) {
        const entry = lazyPanels.get(id);
        return !entry || entry.active;
      }

      function requestPanelWork(id, fn) {
        if (typeof fn !== "function") {
          return;
        }
        const entry = lazyPanels.get(id);
        if (!entry || entry.active) {
          fn();
          return;
        }
        entry.pending.push(fn);
      }

      registerLazyPanel("watchlist", elements.watchPanel, () => {
        loadSavedWatchlists();
        updateWatchlistStatuses();
      });
      registerLazyPanel("snapshots", elements.snapshotPanel, () => {
        loadSnapshots();
      });
      registerLazyPanel("write", elements.writePanel, () => {
        refreshWriteActions();
      });
      registerLazyPanel("paint", elements.paintPanel, () => {
        refreshPaintCard();
      });

      initUsagePanelTracking();

      function nowPerfMs() {
        if (typeof performance !== "undefined" && typeof performance.now === "function") {
          return performance.now();
        }
        return Date.now();
      }

      function initUsagePanelTracking() {
        if (typeof document === "undefined" || typeof window === "undefined") {
          return;
        }
        if (typeof IntersectionObserver === "undefined") {
          return;
        }
        const panels = document.querySelectorAll('.panel[data-panel-id]');
        if (!panels.length) {
          return;
        }
        state.usageObserver = new IntersectionObserver(handleUsageIntersections, {
          threshold: USAGE_PANEL_THRESHOLD,
        });
        panels.forEach(panel => {
          const id = panel.dataset?.panelId;
          if (!id || state.usagePanels.has(id)) {
            return;
          }
          state.usagePanels.set(id, {
            id,
            element: panel,
            visible: false,
            enteredAt: 0,
            pendingMs: 0,
            pendingEntries: 0,
          });
          state.usageObserver.observe(panel);
        });
        if (!state.usagePanels.size) {
          return;
        }
        state.usageFlushTimer = window.setInterval(() => {
          flushUsageMetrics();
        }, USAGE_FLUSH_INTERVAL_MS);
        const flushWithKeepalive = () => {
          flushUsageMetrics({ keepalive: true });
        };
        document.addEventListener("visibilitychange", () => {
          if (document.visibilityState === "hidden") {
            flushWithKeepalive();
          }
        });
        window.addEventListener("pagehide", flushWithKeepalive);
        window.addEventListener("beforeunload", flushWithKeepalive);
      }

      function handleUsageIntersections(entries) {
        if (!state.usagePanels.size) {
          return;
        }
        const now = nowPerfMs();
        entries.forEach(entry => {
          const id = entry.target?.dataset?.panelId;
          if (!id) {
            return;
          }
          const panel = state.usagePanels.get(id);
          if (!panel) {
            return;
          }
          if (entry.isIntersecting) {
            if (!panel.visible) {
              panel.visible = true;
              panel.enteredAt = now;
              panel.pendingEntries += 1;
            }
            return;
          }
          if (!panel.visible) {
            return;
          }
          const delta = now - panel.enteredAt;
          if (delta > 0) {
            panel.pendingMs += delta;
          }
          panel.visible = false;
          panel.enteredAt = 0;
        });
      }

      async function flushUsageMetrics(options = {}) {
        if (!state.usagePanels.size) {
          return;
        }
        const now = nowPerfMs();
        const timestamp = Date.now();
        const payload = [];
        state.usagePanels.forEach(panel => {
          let dwell = panel.pendingMs;
          if (panel.visible && panel.enteredAt > 0) {
            const delta = now - panel.enteredAt;
            if (delta > 0) {
              dwell += delta;
              panel.enteredAt = now;
            }
          }
          const entries = panel.pendingEntries;
          if (dwell <= 0 && entries <= 0) {
            return;
          }
          payload.push({
            id: panel.id,
            dwell_ms: Math.round(dwell),
            entries,
            timestamp_ms: timestamp,
          });
          panel.pendingMs = 0;
          panel.pendingEntries = 0;
        });
        if (!payload.length) {
          return;
        }
        try {
          await sendUsageMetrics({ timestamp_ms: timestamp, panels: payload }, options.keepalive === true);
        } catch (error) {
          console.warn("Failed to publish usage metrics", error);
        }
      }

      async function sendUsageMetrics(payload, keepalive = false) {
        if (typeof fetch === "undefined") {
          return;
        }
        const response = await fetch(USAGE_METRICS_ENDPOINT, {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify(payload),
          keepalive: keepalive === true,
        });
        if (!response.ok && response.status !== 202) {
          throw new Error(`usage metrics HTTP ${response.status}`);
        }
      }

      async function refreshStreamMetrics() {
        if (!elements.metricQueue) {
          return;
        }
        try {
          const data = await fetchJson("/inspector/metrics/stream");
          const limits = data.limits || {};
          const queueDepth = Number(data.queue_depth ?? 0);
          const maxPending = Number(limits.max_pending_events ?? 0);
          elements.metricQueue.textContent = queueDepth.toString();
          if (queueDepth === 0) {
            setBadge(elements.metricQueueBadge, "ok", "Idle");
          } else if (maxPending > 0 && queueDepth >= maxPending) {
            setBadge(elements.metricQueueBadge, "err", "Clamped");
          } else {
            setBadge(elements.metricQueueBadge, "info", "Flowing");
          }

          const maxQueueDepth = Number(data.max_queue_depth ?? 0);
          elements.metricMaxQueue.textContent = maxQueueDepth.toString();
          setBadge(elements.metricMaxBadge, maxQueueDepth > 0 ? "info" : "muted", maxQueueDepth > 0 ? "History" : "Tracked");

          const dropped = Number(data.dropped ?? 0);
          elements.metricDropped.textContent = dropped.toString();
          setBadge(elements.metricDropBadge, dropped === 0 ? "ok" : "warn", dropped === 0 ? "Clean" : "Dropped");

          const resent = Number(data.resent ?? 0);
          elements.metricResent.textContent = resent.toString();
          setBadge(elements.metricResentBadge, resent === 0 ? "ok" : "warn", resent === 0 ? "None" : "Resent");

          const activeSessions = Number(data.active_sessions ?? 0);
          const totalSessions = Number(data.total_sessions ?? 0);
          elements.metricActiveSessions.textContent = activeSessions.toString();
          elements.metricTotalSessions.textContent = totalSessions.toString();

          const disconnect = data.disconnect || {};
          const disconnectClient = Number(disconnect.client ?? 0);
          const disconnectServer = Number(disconnect.server ?? 0);
          const disconnectBackpressure = Number(disconnect.backpressure ?? 0);
          const disconnectTimeout = Number(disconnect.timeout ?? 0);
          const disconnectTotal = disconnectClient + disconnectServer + disconnectBackpressure + disconnectTimeout;
          elements.metricDisconnectTotal.textContent = disconnectTotal.toString();
          if (disconnectTotal === 0) {
            setBadge(elements.metricDisconnectBadge, "muted", "None");
          } else if (disconnectBackpressure > 0) {
            setBadge(elements.metricDisconnectBadge, "err", "Backpressure");
          } else if (disconnectTimeout > 0) {
            setBadge(elements.metricDisconnectBadge, "warn", "Timeout");
          } else if (disconnectClient > 0) {
            setBadge(elements.metricDisconnectBadge, "info", "Client");
          } else {
            setBadge(elements.metricDisconnectBadge, "info", "Server");
          }
          elements.metricDisconnectNote.textContent =
            `Client ${disconnectClient} · Server ${disconnectServer} · Backpressure ${disconnectBackpressure} · Timeout ${disconnectTimeout}`;

          const idleMs = Number(limits.idle_timeout_ms ?? 0);
          const idleText = idleMs > 0 ? `${Math.round(idleMs / 1000)}s` : "disabled";
          const capText = maxPending > 0 ? `${maxPending} events` : "—";
          elements.metricLimitNote.textContent = `Queue cap ${capText} · idle timeout ${idleText}.`;
        } catch (error) {
          elements.metricQueue.textContent = "—";
          elements.metricMaxQueue.textContent = "—";
          elements.metricDropped.textContent = "—";
          elements.metricResent.textContent = "—";
          elements.metricActiveSessions.textContent = "—";
          elements.metricTotalSessions.textContent = "—";
          elements.metricDisconnectTotal.textContent = "—";
          setBadge(elements.metricQueueBadge, "warn", "Unknown");
          setBadge(elements.metricMaxBadge, "warn", "Unknown");
          setBadge(elements.metricDropBadge, "warn", "Unknown");
          setBadge(elements.metricResentBadge, "warn", "Unknown");
          setBadge(elements.metricDisconnectBadge, "warn", "Unknown");
          elements.metricLimitNote.textContent = `Metrics unavailable: ${error.message || error}`;
          elements.metricDisconnectNote.textContent = "Failed to load metrics.";
        }
      }

      function renderMailboxRows(widgets) {
        if (!elements.mailboxRows || !elements.mailboxEmpty) {
          return;
        }

        elements.mailboxRows.innerHTML = "";
        if (!widgets || !widgets.length) {
          elements.mailboxEmpty.style.display = "block";
          return;
        }

        elements.mailboxEmpty.style.display = "none";
        widgets.forEach(widget => {
          const row = document.createElement("div");
          row.className = "mailbox-row";

          const pathSpan = document.createElement("span");
          pathSpan.className = "mailbox-path";
          pathSpan.textContent = widget.path || "—";

          const kindSpan = document.createElement("span");
          kindSpan.textContent = widget.kind || "—";

          const eventsSpan = document.createElement("span");
          eventsSpan.textContent = String(widget.events_total ?? 0);

          const failuresSpan = document.createElement("span");
          failuresSpan.textContent = String(widget.dispatch_failures_total ?? 0);

          const topicsContainer = document.createElement("div");
          topicsContainer.className = "mailbox-topics";
          const topics = Array.isArray(widget.topics) ? widget.topics : [];
          if (!topics.length) {
            const pill = document.createElement("span");
            pill.className = "pill";
            pill.textContent = "—";
            topicsContainer.appendChild(pill);
          } else {
            topics.forEach(topic => {
              const pill = document.createElement("span");
              pill.className = "pill";
              const topicName = topic.topic || "";
              const topicTotal = Number(topic.total ?? 0);
              pill.textContent = `${topicName}: ${topicTotal}`;
              topicsContainer.appendChild(pill);
            });
          }

          row.appendChild(pathSpan);
          row.appendChild(kindSpan);
          row.appendChild(eventsSpan);
          row.appendChild(failuresSpan);
          row.appendChild(topicsContainer);

          elements.mailboxRows.appendChild(row);
        });
      }

      async function refreshMailboxMetrics() {
        if (!elements.mailboxWidgetCount) {
          return;
        }

        try {
          const data = await fetchJson(MAILBOX_METRICS_ENDPOINT);
          const summary = data.summary || {};
          const widgets = Array.isArray(data.widgets) ? data.widgets : [];

          const widgetCount = Number(summary.widgets_with_mailbox ?? 0);
          elements.mailboxWidgetCount.textContent = widgetCount.toString();
          setBadge(elements.mailboxWidgetBadge,
                   widgetCount > 0 ? "info" : "muted",
                   widgetCount > 0 ? "Active" : "No mailboxes");

          const eventsTotal = Number(summary.total_events ?? 0);
          elements.mailboxEventsTotal.textContent = eventsTotal.toString();
          setBadge(elements.mailboxEventsBadge,
                   eventsTotal > 0 ? "info" : "muted",
                   eventsTotal > 0 ? "Recorded" : "Idle");

          const failuresTotal = Number(summary.total_failures ?? 0);
          elements.mailboxFailuresTotal.textContent = failuresTotal.toString();
          setBadge(elements.mailboxFailuresBadge,
                   failuresTotal > 0 ? "warn" : "ok",
                   failuresTotal > 0 ? "Failures" : "Clean");

          const lastKind = summary.last_event_kind || null;
          const lastWidget = summary.last_event_widget || null;
          const lastNs = summary.last_event_ns || null;

          elements.mailboxLastEvent.textContent = lastNs ? String(lastNs) : "—";
          setBadge(elements.mailboxLastKind,
                   lastKind ? "info" : "muted",
                   lastKind || "None");

          if (elements.mailboxSummaryNote) {
            if (!widgetCount) {
              elements.mailboxSummaryNote.textContent = "No mailbox metrics loaded.";
            } else if (lastKind) {
              const target = lastWidget ? ` @ ${lastWidget}` : "";
              elements.mailboxSummaryNote.textContent = `Last ${lastKind}${target}`;
            } else {
              elements.mailboxSummaryNote.textContent = "Mailboxes have no recent events.";
            }
          }

          renderMailboxRows(widgets);
        } catch (error) {
          console.warn("Failed to load mailbox metrics", error);
          if (elements.mailboxSummaryNote) {
            elements.mailboxSummaryNote.textContent = "Failed to load mailbox metrics.";
          }
          if (elements.mailboxWidgetBadge) {
            setBadge(elements.mailboxWidgetBadge, "warn", "Error");
          }
        }
      }

      async function refreshMetrics() {
        await Promise.allSettled([
          refreshStreamMetrics(),
          refreshMailboxMetrics(),
        ]);
      }

      async function refreshSearchDiagnostics() {
        if (!elements.searchQueryBadge) {
          return;
        }
        try {
          const data = await fetchJson(SEARCH_METRICS_ENDPOINT);
          state.searchMetrics = data;
          applySearchMetrics(data);
        } catch (error) {
          console.warn("Failed to load search metrics", error);
          setBadge(elements.searchQueryBadge, "warn", "Metrics unavailable");
          setBadge(elements.searchLatencyBadge, "warn", "—");
          setBadge(elements.searchTruncateBadge, "warn", "—");
        }
      }

      function applySearchMetrics(data) {
        if (!data) {
          return;
        }
        updateSearchMetricBadges(data.queries || {});
        updateWatchMetricBadges(data.watch || {});
      }

      function updateSearchMetricBadges(queries) {
        if (!elements.searchQueryBadge) {
          return;
        }
        const total = Number(queries.total ?? queries.total_queries ?? 0);
        const truncatedQueries = Number(queries.truncated_queries ?? 0);
        const truncatedResults = Number(queries.truncated_results_total ?? 0);
        const lastLatency = Number(queries.last_latency_ms ?? 0);
        const avgLatency = Number(queries.average_latency_ms ?? 0);
        const lastTruncated = Number(queries.last_truncated_count ?? 0);

        setBadge(elements.searchQueryBadge,
                 total > 0 ? "info" : "muted",
                 total === 1 ? "1 query" : `${total} queries`);

        const latencyParts = [];
        if (lastLatency > 0) {
          latencyParts.push(`${lastLatency} ms last`);
        }
        if (avgLatency > 0) {
          latencyParts.push(`avg ${avgLatency} ms`);
        }
        if (!latencyParts.length) {
          latencyParts.push("No samples");
        }
        const latencyClass = lastLatency > 150 ? "warn" : lastLatency > 0 ? "info" : "muted";
        setBadge(elements.searchLatencyBadge, latencyClass, latencyParts.join(" · "));

        let truncationLabel = "No truncation";
        if (truncatedQueries > 0) {
          const detail = truncatedResults > 0 ? ` (${truncatedResults} nodes)` : "";
          truncationLabel = `${truncatedQueries} queries${detail}`;
        } else if (lastTruncated > 0) {
          truncationLabel = `${lastTruncated} nodes last query`;
        }
        setBadge(elements.searchTruncateBadge,
                 truncatedQueries > 0 ? "warn" : "ok",
                 truncationLabel);
      }

      function updateWatchMetricBadges(watch) {
        if (!elements.watchLiveBadge) {
          return;
        }
        const live = Number(watch.live ?? 0);
        const missing = Number(watch.missing ?? 0);
        const truncated = Number(watch.truncated ?? 0);
        const outOfScope = Number(watch.out_of_scope ?? 0);
        const unknown = Number(watch.unknown ?? 0);

        setBadge(elements.watchLiveBadge, live > 0 ? "ok" : "muted", `Live ${live}`);
        setBadge(elements.watchMissingBadge,
                 missing > 0 ? "warn" : "muted",
                 `Missing ${missing}`);
        setBadge(elements.watchTruncateBadge,
                 truncated > 0 ? "warn" : "ok",
                 `Truncated ${truncated}`);
        setBadge(elements.watchScopeBadge,
                 outOfScope > 0 ? "err" : "ok",
                 `Out of scope ${outOfScope}`);
        setBadge(elements.watchUnknownBadge,
                 unknown > 0 ? "info" : "muted",
                 `Unknown ${unknown}`);
      }

      function setWriteWarning(message, tone = "info") {
        if (!elements.writeWarning) {
          return;
        }
        elements.writeWarning.textContent = message;
        elements.writeWarning.className = "write-warning";
        if (tone === "ok") {
          elements.writeWarning.classList.add("ok");
        } else if (tone === "error") {
          elements.writeWarning.classList.add("error");
        }
      }

      function updateWriteButtons() {
        if (elements.writeEnableButton) {
          elements.writeEnableButton.disabled = state.writeSessionEnabled;
        }
        if (elements.writeDisableButton) {
          elements.writeDisableButton.disabled = !state.writeSessionEnabled;
        }
      }

      function renderWriteActions() {
        if (!elements.writeToggleList) {
          return;
        }
        elements.writeToggleList.innerHTML = "";
        if (!state.writeActionsAllowed) {
          const empty = document.createElement("li");
          empty.className = "empty-state";
          empty.textContent = "Write toggles unavailable for this session.";
          elements.writeToggleList.appendChild(empty);
          return;
        }
        if (!state.writeActions.length) {
          const empty = document.createElement("li");
          empty.className = "empty-state";
          empty.textContent = "No write toggles configured.";
          elements.writeToggleList.appendChild(empty);
          return;
        }
        state.writeActions.forEach(action => {
          const item = document.createElement("li");
          item.className = "write-toggle";

          const header = document.createElement("div");
          header.className = "write-toggle-header";
          const title = document.createElement("strong");
          title.textContent = action.label || action.id;
          header.appendChild(title);
          const badge = document.createElement("span");
          const currentState = !!action.current_state;
          badge.className = `badge ${currentState ? "ok" : "warn"}`;
          badge.textContent = currentState ? "Enabled" : "Disabled";
          header.appendChild(badge);
          item.appendChild(header);

          if (action.description) {
            const description = document.createElement("p");
            description.className = "write-toggle-description";
            description.textContent = action.description;
            item.appendChild(description);
          }

          if (action.path) {
            const path = document.createElement("div");
            path.className = "write-toggle-path";
            path.textContent = action.path;
            item.appendChild(path);
          }

          const meta = document.createElement("div");
          meta.className = "node-meta";
          if (action.kind === "set_bool") {
            meta.textContent = action.default_state ? "Sets flag to enabled." : "Resets flag to disabled.";
          } else {
            meta.textContent = "Flips the boolean flag.";
          }
          item.appendChild(meta);

          const buttons = document.createElement("div");
          buttons.className = "button-row";
          const actionButton = document.createElement("button");
          actionButton.type = "button";
          actionButton.className = action.kind === "set_bool" ? "secondary" : "secondary";
          if (action.kind === "set_bool") {
            actionButton.textContent = action.default_state ? "Set enabled" : "Reset disabled";
          } else {
            actionButton.textContent = "Toggle";
          }
          actionButton.disabled = !state.writeSessionEnabled;
          actionButton.addEventListener("click", event => {
            event.preventDefault();
            performWriteAction(action.id, action.kind);
          });
          buttons.appendChild(actionButton);
          item.appendChild(buttons);

          elements.writeToggleList.appendChild(item);
        });
      }

      async function refreshWriteActions() {
        if (!elements.writeToggleList) {
          return;
        }
        try {
          const data = await fetchJson(WRITE_TOGGLE_ENDPOINT);
          state.writeActions = Array.isArray(data.actions) ? data.actions : [];
          state.writeActionsAllowed = true;
          if (data.confirmation_header) {
            state.writeConfirmationHeader = data.confirmation_header;
          }
          if (data.confirmation_token) {
            state.writeConfirmationToken = data.confirmation_token;
          }
          if (elements.writeNote) {
            const allowed = Array.isArray(data.allowed_roles) && data.allowed_roles.length
              ? data.allowed_roles.join(", ")
              : "admin";
            elements.writeNote.textContent = `Requires roles: ${allowed}. All actions are audited.`;
          }
          setWriteWarning(state.writeSessionEnabled
                           ? "Write controls armed. Changes are audited."
                           : "Write controls available. Enable to send changes.");
          renderWriteActions();
        } catch (error) {
          state.writeActions = [];
          state.writeActionsAllowed = false;
          const message = error && error.message ? error.message : "Write toggles unavailable.";
          const tone = error && (error.status === 403 || error.status === 404) ? "error" : "warn";
          setWriteWarning(message, tone);
          renderWriteActions();
        }
      }

      function disableWriteSession() {
        if (!state.writeSessionEnabled) {
          return;
        }
        state.writeSessionEnabled = false;
        updateWriteButtons();
        setWriteWarning("Write controls disabled for this session.");
        renderWriteActions();
      }

      function requestWriteSession() {
        if (state.writeSessionEnabled) {
          return true;
        }
        const confirmation = window.prompt(
          "Type ENABLE to arm inspector write controls for this session:",
          ""
        );
        if (!confirmation || confirmation.trim().toUpperCase() !== WRITE_ENABLE_PASSPHRASE) {
          setWriteWarning("Write controls remain disabled.", "warn");
          return false;
        }
        state.writeSessionEnabled = true;
        updateWriteButtons();
        setWriteWarning("Write controls armed. Changes are audited.", "warn");
        renderWriteActions();
        return true;
      }

      async function performWriteAction(actionId, kind) {
        if (!state.writeSessionEnabled && !requestWriteSession()) {
          return;
        }
        const payload = { id: actionId };
        payload.operation = kind === "set_bool" ? "set" : "toggle";
        const headers = { "Content-Type": "application/json" };
        if (state.writeConfirmationHeader) {
          headers[state.writeConfirmationHeader] = state.writeConfirmationToken || WRITE_CONFIRM_TOKEN_DEFAULT;
        }
        try {
          await fetchJson(WRITE_TOGGLE_ENDPOINT, {
            method: "POST",
            headers,
            body: JSON.stringify(payload),
          });
          setWriteWarning("Write toggle applied.", "ok");
          await refreshWriteActions();
        } catch (error) {
          console.error("Write toggle failed", error);
          const message = error && error.message ? error.message : "Write action failed";
          setWriteWarning(message, "error");
        }
      }

      function sendSearchMetrics(payload) {
        if (!payload) {
          return;
        }
        try {
          const body = JSON.stringify(payload);
          if (navigator.sendBeacon && typeof Blob !== "undefined") {
            const blob = new Blob([body], { type: "application/json" });
            navigator.sendBeacon(SEARCH_METRICS_ENDPOINT, blob);
            return;
          }
          fetch(SEARCH_METRICS_ENDPOINT, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body,
            keepalive: true,
          }).catch(() => {});
        } catch (error) {
          console.warn("Failed to record search metrics", error);
        }
      }

      function startMetricsPolling() {
        refreshMetrics();
        refreshSearchDiagnostics();
        if (state.metricsTimer) {
          clearInterval(state.metricsTimer);
        }
        state.metricsTimer = setInterval(refreshMetrics, METRICS_REFRESH_MS);
        if (state.searchMetricsTimer) {
          clearInterval(state.searchMetricsTimer);
        }
        state.searchMetricsTimer = setInterval(refreshSearchDiagnostics, METRICS_REFRESH_MS);
      }

      function showWatchMessage(text, isError = false) {
        if (!elements.watchMessage) {
          return;
        }
        if (watchMessageTimeout) {
          clearTimeout(watchMessageTimeout);
          watchMessageTimeout = null;
        }
        if (!text) {
          elements.watchMessage.style.display = "none";
          elements.watchMessage.textContent = "";
          elements.watchMessage.classList.remove("error");
          return;
        }
        elements.watchMessage.style.display = "block";
        elements.watchMessage.textContent = text;
        if (isError) {
          elements.watchMessage.classList.add("error");
        } else {
          elements.watchMessage.classList.remove("error");
        }
        watchMessageTimeout = setTimeout(() => {
          elements.watchMessage.style.display = "none";
          elements.watchMessage.textContent = "";
          elements.watchMessage.classList.remove("error");
          watchMessageTimeout = null;
        }, 4000);
      }

      function parentPath(path) {
        if (!path || path === "/") {
          return null;
        }
        const idx = path.lastIndexOf("/");
        if (idx <= 0) {
          return "/";
        }
        return path.slice(0, idx);
      }

      function findNode(root, path) {
        if (!root || !path) return null;
        if (root.path === path) return root;
        if (!Array.isArray(root.children)) return null;
        for (const child of root.children) {
          const match = findNode(child, path);
          if (match) return match;
        }
        return null;
      }

      function removeNode(root, path) {
        if (!root || !path || !Array.isArray(root.children)) {
          return false;
        }
        const idx = root.children.findIndex(child => child.path === path);
        if (idx >= 0) {
          root.children.splice(idx, 1);
          return true;
        }
        return root.children.some(child => removeNode(child, path));
      }

      function upsertNode(root, node) {
        if (!root || !node) return false;
        if (root.path === node.path) {
          Object.assign(root, node);
          return true;
        }
        const parent = parentPath(node.path);
        if (!parent) return false;
        const parentNode = findNode(root, parent);
        if (!parentNode) return false;
        if (!Array.isArray(parentNode.children)) {
          parentNode.children = [];
        }
        const idx = parentNode.children.findIndex(child => child.path === node.path);
        if (idx >= 0) {
          parentNode.children[idx] = node;
        } else {
          parentNode.children.push(node);
          parentNode.children.sort((a, b) => a.path.localeCompare(b.path));
        }
        return true;
      }

      function visitTree(node, fn) {
        if (!node || typeof fn !== "function") {
          return;
        }
        fn(node);
        if (Array.isArray(node.children)) {
          node.children.forEach(child => visitTree(child, fn));
        }
      }

      function normalizePath(value) {
        if (!value) {
          return null;
        }
        let normalized = value.trim();
        if (!normalized) {
          return null;
        }
        if (!normalized.startsWith("/")) {
          normalized = "/" + normalized;
        }
        while (normalized.length > 1 && normalized.endsWith("/")) {
          normalized = normalized.slice(0, -1);
        }
        return normalized;
      }

      function loadInitialParameters() {
        const params = new URLSearchParams(window.location.search || "");
        const urlRoot = normalizePath(params.get("root"));
        const storedRoot = safeStorageGet(ROOT_STORAGE_KEY);
        const resolvedRoot = urlRoot || storedRoot || state.root;
        state.root = resolvedRoot || "/";
        if (elements.rootInput) {
          elements.rootInput.value = state.root;
        }
        const depthParam = Number(params.get("depth"));
        if (!Number.isNaN(depthParam) && depthParam >= 0) {
          state.maxDepth = depthParam;
        }
        if (elements.depthInput) {
          elements.depthInput.value = state.maxDepth;
        }
        const childParam = Number(params.get("max_children"));
        if (!Number.isNaN(childParam) && childParam > 0) {
          state.maxChildren = childParam;
        }
        if (elements.childrenInput) {
          elements.childrenInput.value = state.maxChildren;
        }
        return { hadRootParam: Boolean(urlRoot) };
      }

      function updateUrlParams() {
        if (!window.history || !window.history.replaceState) {
          return;
        }
        const params = new URLSearchParams(window.location.search || "");
        params.set("root", state.root || "/");
        params.set("depth", String(state.maxDepth ?? 2));
        params.set("max_children", String(state.maxChildren ?? 64));
        const newUrl = `${window.location.pathname}?${params.toString()}`;
        window.history.replaceState({}, "", newUrl);
      }

      function updateRootSelectValue(rootValue) {
        if (!elements.rootSelect) {
          return;
        }
        const match = Array.from(elements.rootSelect.options).find(option => option.value === rootValue);
        elements.rootSelect.value = match ? rootValue : "__custom__";
      }

      function describeAccessHint(entry) {
        const details = [];
        if (entry.message) {
          details.push(entry.message);
        }
        if (entry.access_hint) {
          details.push(entry.access_hint);
        }
        return details.join(" · ");
      }

      function describeRootMeta(path) {
        if (!path || path === "/") {
          return { label: "Local root", badge: "info", hint: "" };
        }
        if (state.aclError && state.aclError.root === path) {
          const hints = [];
          if (state.aclError.message) {
            hints.push(state.aclError.message);
          }
          if (state.aclError.allowed && state.aclError.allowed.length) {
            hints.push(`Allowed roots: ${state.aclError.allowed.join(", ")}`);
          }
          return {
            label: "Access denied",
            badge: "err",
            hint: hints.join(" — ") || "Access denied",
          };
        }
        if (path === REMOTE_ROOT_PATH) {
          if (!state.remoteMounts.length) {
            return { label: "No remote mounts", badge: "muted", hint: "" };
          }
          const online = state.remoteMounts.filter(entry => entry.connected).length;
          const total = state.remoteMounts.length;
          let badge = "ok";
          if (online === 0) {
            badge = "err";
          } else if (online < total) {
            badge = "warn";
          }
          return {
            label: `${online}/${total} online`,
            badge,
            hint: "Remote mount container",
          };
        }
        const mount = state.remoteMounts.find(entry => (entry.path && entry.path === path)
            || `${REMOTE_ROOT_PATH}/${entry.alias}` === path);
        if (mount) {
          return {
            label: mount.connected ? "Online" : (mount.message || "Offline"),
            badge: mount.connected ? "ok" : "warn",
            hint: describeAccessHint(mount),
          };
        }
        return { label: "Custom root", badge: "muted", hint: "" };
      }

      function updateRootStatusDisplay() {
        const meta = describeRootMeta(state.root);
        if (elements.rootStatusBadge) {
          setBadge(elements.rootStatusBadge, meta.badge, meta.label);
        }
        if (elements.rootHint) {
          elements.rootHint.textContent = meta.hint || "";
          elements.rootHint.style.display = meta.hint ? "block" : "none";
        }
      }

      function renderRemoteStatusBadges() {
        if (!elements.remoteStatusRow) {
          return;
        }
        elements.remoteStatusRow.innerHTML = "";
        if (!state.remoteMounts.length) {
          const placeholder = document.createElement("span");
          placeholder.className = "badge muted";
          placeholder.textContent = "Remote mounts disabled";
          elements.remoteStatusRow.appendChild(placeholder);
          return;
        }
        state.remoteMounts.forEach(entry => {
          const badge = document.createElement("span");
          badge.className = `badge ${entry.connected ? "ok" : "err"}`;
          const alias = entry.alias || entry.path || "remote";
          badge.textContent = alias;
          const title = describeAccessHint(entry);
          if (title) {
            badge.title = title;
          }
          badge.tabIndex = 0;
          badge.setAttribute("role", "button");
          const targetRoot = entry.path || (entry.alias ? `${REMOTE_ROOT_PATH}/${entry.alias}` : REMOTE_ROOT_PATH);
          const statusLabel = entry.connected ? "online" : "offline";
          badge.setAttribute(
            "aria-label",
            `${alias} ${statusLabel}. Activate to inspect ${targetRoot}.`
          );
          const activate = () => {
            setRoot(targetRoot, { refresh: true });
          };
          badge.addEventListener("click", activate);
          badge.addEventListener("keydown", event => {
            if (event.key === "Enter" || event.key === " ") {
              event.preventDefault();
              activate();
            }
          });
          elements.remoteStatusRow.appendChild(badge);
        });
      }

      function createOption(value, label, title) {
        const option = document.createElement("option");
        option.value = value;
        option.textContent = label;
        if (title) {
          option.title = title;
        }
        return option;
      }

      function buildRootOptions() {
        if (!elements.rootSelect) {
          return;
        }
        const select = elements.rootSelect;
        select.innerHTML = "";
        select.appendChild(createOption("/", "Local root (/)", "Local PathSpace"));
        if (state.remoteMounts.length) {
          select.appendChild(createOption(REMOTE_ROOT_PATH, "Remote mounts (/remote)", "Container"));
          state.remoteMounts.forEach(entry => {
            const label = `${REMOTE_ROOT_PATH}/${entry.alias}`;
            const option = createOption(entry.path || label, label, describeAccessHint(entry));
            select.appendChild(option);
          });
        }
        select.appendChild(createOption("__custom__", "Custom…", "Enter manually"));
        updateRootSelectValue(state.root);
      }

      function setRoot(newRoot, options = {}) {
        const normalized = normalizePath(newRoot) || "/";
        state.root = normalized;
        if (state.aclError && state.aclError.root !== normalized) {
          state.aclError = null;
        }
        if (state.blockedStreamRoot && state.blockedStreamRoot !== normalized) {
          state.blockedStreamRoot = null;
        }
        if (elements.rootInput) {
          elements.rootInput.value = normalized;
        }
        if (options.persist !== false) {
          safeStorageSet(ROOT_STORAGE_KEY, normalized);
        }
        updateRootSelectValue(normalized);
        updateRootStatusDisplay();
        if (options.syncUrl !== false) {
          updateUrlParams();
        }
        if (options.refresh) {
          refreshTree();
          connectStream();
        }
      }

      async function refreshRemoteMetadata() {
        try {
          const data = await fetchJson(REMOTE_STATUS_ENDPOINT);
          const mounts = Array.isArray(data.mounts) ? data.mounts : [];
          state.remoteMounts = mounts.map(entry => ({
            alias: entry.alias || "",
            path: normalizePath(entry.path) || (entry.alias ? `${REMOTE_ROOT_PATH}/${entry.alias}` : REMOTE_ROOT_PATH),
            connected: Boolean(entry.connected),
            message: entry.message || "",
            access_hint: entry.access_hint || "",
          }));
          if (data.default_root) {
            state.defaultRoot = normalizePath(data.default_root) || state.defaultRoot;
          }
          buildRootOptions();
          renderRemoteStatusBadges();
          updateRootStatusDisplay();
        } catch (error) {
          console.warn("Failed to fetch remote metadata", error);
          if (!state.remoteMounts.length && elements.remoteStatusRow) {
            elements.remoteStatusRow.innerHTML = '<span class="badge warn">Remote metadata unavailable</span>';
          }
        }
      }

      function startRemoteMetadataPolling() {
        refreshRemoteMetadata();
        if (state.remoteStatusTimer) {
          clearInterval(state.remoteStatusTimer);
        }
        state.remoteStatusTimer = setInterval(refreshRemoteMetadata, REMOTE_STATUS_REFRESH_MS);
      }

      function pathWithinSnapshot(path) {
        if (!state.liveTree || !path) {
          return false;
        }
        const rootPath = state.liveTree.path || "/";
        if (rootPath === "/") {
          return true;
        }
        if (path === rootPath) {
          return true;
        }
        const prefix = rootPath.endsWith("/") ? rootPath : rootPath + "/";
        return path.startsWith(prefix);
      }

      function ancestorTruncated(path) {
        if (!state.liveTree) {
          return false;
        }
        let current = parentPath(path);
        while (current) {
          const node = findNode(state.liveTree, current);
          if (node) {
            if (node.children_truncated) {
              return true;
            }
          }
          current = parentPath(current);
        }
        return false;
      }

      function watchStatusBadge(status) {
        switch (status) {
          case "live":
            return { label: "Live", badge: "ok" };
          case "missing":
            return { label: "Missing", badge: "err" };
          case "truncated":
            return { label: "Truncated", badge: "warn" };
          case "out_of_scope":
            return { label: "Out of scope", badge: "info" };
          default:
            return { label: "Unknown", badge: "muted" };
        }
      }

      function formatRelativeTime(date) {
        if (!date) {
          return "";
        }
        const diff = Date.now() - date.getTime();
        if (diff < 5000) {
          return "just now";
        }
        if (diff < 60000) {
          return `${Math.floor(diff / 1000)}s ago`;
        }
        if (diff < 3600000) {
          return `${Math.floor(diff / 60000)}m ago`;
        }
        return `${Math.floor(diff / 3600000)}h ago`;
      }

      function formatBytes(bytes) {
        if (!Number.isFinite(bytes) || bytes <= 0) {
          return "0 B";
        }
        const units = ["B", "KB", "MB", "GB", "TB"];
        let value = bytes;
        let unitIndex = 0;
        while (value >= 1024 && unitIndex < units.length - 1) {
          value /= 1024;
          unitIndex += 1;
        }
        const precision = unitIndex === 0 ? 0 : value < 10 ? 2 : 1;
        return `${value.toFixed(precision)} ${units[unitIndex]}`;
      }

      function renderWatchlist() {
        if (!elements.watchlist) {
          return;
        }
        elements.watchlist.innerHTML = "";
        if (!state.watchlist.length) {
          elements.watchlistEmpty.style.display = "block";
          return;
        }
        elements.watchlistEmpty.style.display = "none";
        state.watchlist.forEach(entry => {
          const li = document.createElement("li");
          li.className = "watch-entry";

          const header = document.createElement("div");
          header.className = "watch-header";
          const code = document.createElement("code");
          code.textContent = entry.path;
          header.appendChild(code);
          const badge = document.createElement("span");
          const badgeInfo = watchStatusBadge(entry.status);
          badge.className = "badge " + badgeInfo.badge;
          badge.textContent = badgeInfo.label;
          header.appendChild(badge);
          li.appendChild(header);

          const meta = document.createElement("div");
          meta.className = "watch-meta";
          if (entry.status === "live" && entry.node) {
            const pieces = [entry.node.value_type || "value", `${entry.node.child_count} children`];
            if (entry.node.value_summary) {
              pieces.push(entry.node.value_summary);
            }
            if (entry.lastSeenAt) {
              pieces.push(`updated ${formatRelativeTime(entry.lastSeenAt)}`);
            }
            meta.textContent = pieces.join(" · ");
          } else if (entry.status === "truncated") {
            meta.textContent = "Beyond the current depth/child limits.";
          } else if (entry.status === "out_of_scope") {
            meta.textContent = "Outside the current snapshot root.";
          } else if (entry.status === "missing") {
            meta.textContent = "Not present in the current snapshot.";
          } else {
            meta.textContent = "Load a snapshot to resolve status.";
          }
          li.appendChild(meta);

          const actions = document.createElement("div");
          actions.className = "watch-actions";
          const openBtn = document.createElement("button");
          openBtn.type = "button";
          openBtn.className = "secondary small";
          openBtn.textContent = "Open";
          openBtn.disabled = entry.status !== "live";
          openBtn.addEventListener("click", () => selectNode(entry.path));
          actions.appendChild(openBtn);

          const removeBtn = document.createElement("button");
          removeBtn.type = "button";
          removeBtn.className = "secondary small danger";
          removeBtn.textContent = "Remove";
          removeBtn.addEventListener("click", () => removeWatch(entry.path));
          actions.appendChild(removeBtn);

          li.appendChild(actions);
          elements.watchlist.appendChild(li);
        });
      }

      function publishWatchMetrics(summary) {
        if (!summary) {
          return;
        }
        const payload = JSON.stringify(summary);
        if (state.lastWatchMetricsPayload === payload) {
          return;
        }
        state.lastWatchMetricsPayload = payload;
        sendSearchMetrics({ watch: summary });
      }

      function updateWatchlistStatuses() {
        if (!state.watchlist.length) {
          renderWatchlist();
          const emptyCounts = { live: 0, missing: 0, truncated: 0, out_of_scope: 0, unknown: 0 };
          updateWatchMetricBadges(emptyCounts);
          publishWatchMetrics(emptyCounts);
          return;
        }
        const counts = { live: 0, missing: 0, truncated: 0, out_of_scope: 0, unknown: 0 };
        state.watchlist.forEach(entry => {
          entry.node = null;
          let status = "unknown";
          if (state.liveTree) {
            if (!pathWithinSnapshot(entry.path)) {
              status = "out_of_scope";
            } else {
              const node = findNode(state.liveTree, entry.path);
              if (node) {
                status = "live";
                entry.node = node;
                entry.lastSeenAt = new Date();
                entry.lastSeenVersion = state.liveVersion;
              } else {
                status = ancestorTruncated(entry.path) ? "truncated" : "missing";
              }
            }
          }
          entry.status = status;
          if (Object.prototype.hasOwnProperty.call(counts, status)) {
            counts[status] += 1;
          } else {
            counts.unknown += 1;
          }
        });
        renderWatchlist();
        updateWatchMetricBadges(counts);
        publishWatchMetrics(counts);
      }

      function addWatch(path) {
        activatePanel("watchlist");
        const normalized = normalizePath(path);
        if (!normalized) {
          showWatchMessage("Enter a valid absolute path before adding.");
          return false;
        }
        if (state.watchlist.some(entry => entry.path === normalized)) {
          showWatchMessage("Path is already in the watchlist.");
          return false;
        }
        state.watchlist.push({
          path: normalized,
          status: state.liveTree ? "unknown" : "unknown",
        });
        elements.watchInput.value = "";
        showWatchMessage("Added watch for " + normalized);
        updateWatchlistStatuses();
        return true;
      }

      function removeWatch(path) {
        activatePanel("watchlist");
        const before = state.watchlist.length;
        state.watchlist = state.watchlist.filter(entry => entry.path !== path);
        if (state.watchlist.length !== before) {
          showWatchMessage("Removed watch for " + path);
        }
        updateWatchlistStatuses();
      }

      function findSavedWatchlist(id) {
        return state.savedWatchlists.find(entry => entry.id === id) || null;
      }

      function renderSavedWatchlists() {
        if (!elements.savedSelect) {
          return;
        }
        const select = elements.savedSelect;
        select.innerHTML = "";
        if (!state.savedWatchlists.length) {
          const option = document.createElement("option");
          option.value = "";
          option.textContent = "No saved watchlists";
          select.appendChild(option);
          select.disabled = true;
          state.selectedWatchlistId = null;
        } else {
          select.disabled = false;
          state.savedWatchlists.forEach(record => {
            const option = document.createElement("option");
            option.value = record.id;
            option.textContent = record.name || record.id;
            select.appendChild(option);
          });
          if (!state.selectedWatchlistId || !findSavedWatchlist(state.selectedWatchlistId)) {
            state.selectedWatchlistId = state.savedWatchlists[0].id;
          }
          select.value = state.selectedWatchlistId;
        }
        const note = document.getElementById("watch-saved-note");
        if (note) {
          note.textContent = state.savedWatchlists.length
            ? `${state.savedWatchlists.length} saved`
            : "No saved watchlists yet.";
        }
      }

      async function loadSavedWatchlists(nextSelectedId) {
        try {
          const data = await fetchJson("/inspector/watchlists");
          const incoming = Array.isArray(data.watchlists) ? data.watchlists : [];
          state.savedWatchlists = incoming;
          if (nextSelectedId && findSavedWatchlist(nextSelectedId)) {
            state.selectedWatchlistId = nextSelectedId;
          } else if (!findSavedWatchlist(state.selectedWatchlistId)) {
            state.selectedWatchlistId = incoming.length ? incoming[0].id : null;
          }
          renderSavedWatchlists();
        } catch (error) {
          console.error("Failed to load saved watchlists", error);
          showWatchMessage(`Failed to load saved watchlists: ${error.message || error}`, true);
        }
      }

      async function saveCurrentWatchlist(options = {}) {
        activatePanel("watchlist");
        if (!state.watchlist.length) {
          showWatchMessage("Add at least one path before saving.", true);
          return;
        }

        const activeRecord = state.selectedWatchlistId ? findSavedWatchlist(state.selectedWatchlistId) : null;
        const needsNamePrompt = options.forceNew || !activeRecord;
        let desiredName = activeRecord ? (activeRecord.name || activeRecord.id) : `Watchlist ${state.savedWatchlists.length + 1}`;
        if (needsNamePrompt) {
          const provided = window.prompt("Name for saved watchlist", desiredName);
          if (!provided) {
            showWatchMessage("Save cancelled.");
            return;
          }
          desiredName = provided.trim();
        }
        if (!desiredName) {
          showWatchMessage("Watchlist name cannot be empty.", true);
          return;
        }

        const payload = {
          name: desiredName,
          paths: state.watchlist.map(entry => entry.path),
        };

        if (!options.forceNew && activeRecord) {
          payload.id = activeRecord.id;
          payload.overwrite = true;
        }

        try {
          const result = await fetchJson("/inspector/watchlists", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(payload),
          });
          const nextId = result.watchlist && result.watchlist.id ? result.watchlist.id : payload.id;
          showWatchMessage(`Watchlist ${result.status || "saved"}.`);
          await loadSavedWatchlists(nextId);
        } catch (error) {
          showWatchMessage(`Failed to save watchlist: ${error.message || error}`, true);
        }
      }

      async function deleteSavedWatchlist() {
        activatePanel("watchlist");
        if (!state.selectedWatchlistId) {
          showWatchMessage("Select a saved watchlist to delete.");
          return;
        }
        try {
          const response = await fetch(`/inspector/watchlists?id=${encodeURIComponent(state.selectedWatchlistId)}`, {
            method: "DELETE",
          });
          if (!response.ok) {
            const text = await response.text();
            throw new Error(text || `HTTP ${response.status}`);
          }
          showWatchMessage("Deleted saved watchlist.");
          state.selectedWatchlistId = null;
          await loadSavedWatchlists();
        } catch (error) {
          showWatchMessage(`Failed to delete watchlist: ${error.message || error}`, true);
        }
      }

      function applySavedWatchlist() {
        activatePanel("watchlist");
        if (!state.selectedWatchlistId) {
          showWatchMessage("Select a saved watchlist to load.");
          return;
        }
        const record = findSavedWatchlist(state.selectedWatchlistId);
        if (!record) {
          showWatchMessage("Saved watchlist not found.", true);
          return;
        }
        state.watchlist = (record.paths || []).map(path => ({
          path,
          status: "unknown",
          node: null,
        }));
        showWatchMessage(`Loaded ${record.name || record.id}.`);
        updateWatchlistStatuses();
      }

      function exportSavedWatchlist() {
        activatePanel("watchlist");
        if (!state.selectedWatchlistId) {
          showWatchMessage("Select a saved watchlist to export.");
          return;
        }
        const record = findSavedWatchlist(state.selectedWatchlistId);
        if (!record) {
          showWatchMessage("Saved watchlist not found.", true);
          return;
        }
        const payload = {
          exported_ms: Date.now(),
          watchlists: [record],
        };
        const blob = new Blob([JSON.stringify(payload, null, 2)], { type: "application/json" });
        const url = URL.createObjectURL(blob);
        const link = document.createElement("a");
        link.href = url;
        const baseName = record.name || record.id || "watchlist";
        link.download = `${baseName.replace(/\s+/g, "-").substring(0, 32)}.watchlist.json`;
        document.body.appendChild(link);
        link.click();
        document.body.removeChild(link);
        URL.revokeObjectURL(url);
      }

      async function importWatchlistsFromFile(file) {
        activatePanel("watchlist");
        try {
          const text = await file.text();
          const json = JSON.parse(text);
          await fetchJson("/inspector/watchlists/import", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(json),
          });
          showWatchMessage("Imported watchlists.");
          await loadSavedWatchlists();
        } catch (error) {
          showWatchMessage(`Import failed: ${error.message || error}`, true);
        }
      }

      function setSnapshotMessage(message, isError = false) {
        if (!elements.snapshotMessage) {
          return;
        }
        if (snapshotMessageTimeout) {
          clearTimeout(snapshotMessageTimeout);
          snapshotMessageTimeout = null;
        }
        elements.snapshotMessage.textContent = message || "";
        elements.snapshotMessage.className = isError ? "snapshot-message error" : "snapshot-message";
        if (message) {
          snapshotMessageTimeout = window.setTimeout(() => {
            elements.snapshotMessage.textContent = "";
            elements.snapshotMessage.className = "snapshot-message";
            snapshotMessageTimeout = null;
          }, 4000);
        }
      }

      function snapshotTotalBytes(record) {
        if (!record) {
          return 0;
        }
        if (typeof record.total_bytes === "number") {
          return record.total_bytes;
        }
        const inspector = Number(record.inspector_bytes || 0);
        const exporter = Number(record.export_bytes || 0);
        return inspector + exporter;
      }

      function fillSnapshotSelect(selectElement, selectedId) {
        if (!selectElement) {
          return;
        }
        selectElement.innerHTML = "";
        state.snapshots.forEach(record => {
          const option = document.createElement("option");
          option.value = record.id;
          option.textContent = record.label || record.id;
          selectElement.appendChild(option);
        });
        if (state.snapshots.length === 0) {
          const option = document.createElement("option");
          option.value = "";
          option.textContent = "No snapshots";
          selectElement.appendChild(option);
          selectElement.value = "";
          selectElement.disabled = true;
          return;
        }
        selectElement.disabled = false;
        if (selectedId && state.snapshots.some(entry => entry.id === selectedId)) {
          selectElement.value = selectedId;
        } else {
          selectElement.value = state.snapshots[0].id;
        }
      }

      function renderSnapshots() {
        if (!elements.snapshotList) {
          return;
        }
        const hasSnapshots = state.snapshots.length > 0;
        if (elements.snapshotEmpty) {
          elements.snapshotEmpty.style.display = hasSnapshots ? "none" : "block";
        }
        elements.snapshotList.innerHTML = "";

        if (!hasSnapshots) {
          if (elements.snapshotSelect) {
            elements.snapshotSelect.innerHTML = '<option value="">No snapshots captured</option>';
            elements.snapshotSelect.disabled = true;
          }
          if (elements.snapshotBefore) {
            elements.snapshotBefore.innerHTML = '<option value="">No snapshots</option>';
            elements.snapshotBefore.disabled = true;
          }
          if (elements.snapshotAfter) {
            elements.snapshotAfter.innerHTML = '<option value="">No snapshots</option>';
            elements.snapshotAfter.disabled = true;
          }
          if (elements.snapshotDiffButton) {
            elements.snapshotDiffButton.disabled = true;
          }
          if (elements.snapshotDiffOutput) {
            elements.snapshotDiffOutput.textContent = "Select two snapshots to see differences.";
          }
          return;
        }

        if (!state.selectedSnapshotId || !state.snapshots.some(entry => entry.id === state.selectedSnapshotId)) {
          state.selectedSnapshotId = state.snapshots[0].id;
        }
        if (!state.snapshotDiffBeforeId || !state.snapshots.some(entry => entry.id === state.snapshotDiffBeforeId)) {
          state.snapshotDiffBeforeId = state.selectedSnapshotId;
        }
        if (!state.snapshotDiffAfterId || !state.snapshots.some(entry => entry.id === state.snapshotDiffAfterId)) {
          const alternate = state.snapshots.find(entry => entry.id !== state.snapshotDiffBeforeId);
          state.snapshotDiffAfterId = alternate ? alternate.id : state.snapshotDiffBeforeId;
        }

        if (elements.snapshotSelect) {
          fillSnapshotSelect(elements.snapshotSelect, state.selectedSnapshotId);
          elements.snapshotSelect.value = state.selectedSnapshotId;
        }
        fillSnapshotSelect(elements.snapshotBefore, state.snapshotDiffBeforeId);
        fillSnapshotSelect(elements.snapshotAfter, state.snapshotDiffAfterId);
        if (elements.snapshotDiffButton) {
          elements.snapshotDiffButton.disabled = state.snapshots.length === 0;
        }

        state.snapshots.forEach(record => {
          const li = document.createElement("li");
          const title = document.createElement("strong");
          title.textContent = record.label || record.id;
          li.appendChild(title);

          const created = record.created_ms ? new Date(record.created_ms).toLocaleString() : "";
          const meta = document.createElement("div");
          meta.className = "snapshot-meta";
          const options = record.options || {};
          const bounds = `${options.root || "/"} · depth ${options.max_depth ?? state.maxDepth} · children ${options.max_children ?? state.maxChildren}`;
          meta.textContent = `${created} · ${formatBytes(snapshotTotalBytes(record))} · ${bounds}`;
          li.appendChild(meta);

          if (record.note) {
            const note = document.createElement("div");
            note.className = "snapshot-meta";
            note.textContent = record.note;
            li.appendChild(note);
          }

          elements.snapshotList.appendChild(li);
        });
      }

      async function loadSnapshots(nextSelectedId) {
        try {
          const data = await fetchJson("/inspector/snapshots");
          const incoming = Array.isArray(data.snapshots) ? data.snapshots : [];
          state.snapshots = incoming;
          if (nextSelectedId && incoming.some(entry => entry.id === nextSelectedId)) {
            state.selectedSnapshotId = nextSelectedId;
          } else if (!incoming.some(entry => entry.id === state.selectedSnapshotId)) {
            state.selectedSnapshotId = incoming.length ? incoming[0].id : null;
          }
          if (!incoming.some(entry => entry.id === state.snapshotDiffBeforeId)) {
            state.snapshotDiffBeforeId = state.selectedSnapshotId;
          }
          if (!incoming.some(entry => entry.id === state.snapshotDiffAfterId)) {
            const alt = incoming.find(entry => entry.id !== state.snapshotDiffBeforeId);
            state.snapshotDiffAfterId = alt ? alt.id : state.snapshotDiffBeforeId;
          }
          renderSnapshots();
        } catch (error) {
          console.error("Failed to load snapshots", error);
          setSnapshotMessage(`Failed to load snapshots: ${error.message || error}`, true);
        }
      }

      async function captureSnapshot(event) {
        event.preventDefault();
        if (!elements.snapshotLabel) {
          return;
        }
        const labelInput = elements.snapshotLabel.value.trim();
        const noteInput = elements.snapshotNote ? elements.snapshotNote.value.trim() : "";
        if (!labelInput) {
          setSnapshotMessage("Enter a label before capturing.", true);
          return;
        }
        const payload = {
          label: labelInput,
          note: noteInput,
          options: {
            root: state.root,
            max_depth: state.maxDepth,
            max_children: state.maxChildren,
            include_values: true,
          },
        };
        try {
          const result = await fetchJson("/inspector/snapshots", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(payload),
          });
          const snapshot = result.snapshot || {};
          elements.snapshotLabel.value = "";
          if (elements.snapshotNote) {
            elements.snapshotNote.value = "";
          }
          setSnapshotMessage(`Snapshot ${snapshot.label || snapshot.id || "captured"}.`);
          await loadSnapshots(snapshot.id || null);
        } catch (error) {
          setSnapshotMessage(`Snapshot failed: ${error.message || error}`, true);
        }
      }

      async function deleteSnapshot() {
        if (!state.selectedSnapshotId) {
          setSnapshotMessage("Select a snapshot to delete.", true);
          return;
        }
        try {
          await fetchWithError(`/inspector/snapshots?id=${encodeURIComponent(state.selectedSnapshotId)}`, {
            method: "DELETE",
          });
          setSnapshotMessage("Snapshot deleted.");
          state.selectedSnapshotId = null;
          await loadSnapshots();
        } catch (error) {
          setSnapshotMessage(`Delete failed: ${error.message || error}`, true);
        }
      }

      function filenameFromDisposition(disposition, fallback) {
        if (!disposition) {
          return fallback;
        }
        const match = /filename="?([^";]+)"?/i.exec(disposition);
        return match && match[1] ? match[1] : fallback;
      }

      async function downloadSnapshot() {
        if (!state.selectedSnapshotId) {
          setSnapshotMessage("Select a snapshot to download.", true);
          return;
        }
        try {
          const response = await fetchWithError(`/inspector/snapshots/export?id=${encodeURIComponent(state.selectedSnapshotId)}`);
          const blob = await response.blob();
          const disposition = response.headers.get("Content-Disposition");
          const fallback = `${state.selectedSnapshotId}.json`;
          const filename = filenameFromDisposition(disposition, fallback);
          const url = URL.createObjectURL(blob);
          const link = document.createElement("a");
          link.href = url;
          link.download = filename;
          document.body.appendChild(link);
          link.click();
          document.body.removeChild(link);
          URL.revokeObjectURL(url);
          setSnapshotMessage("Snapshot JSON downloaded.");
        } catch (error) {
          setSnapshotMessage(`Download failed: ${error.message || error}`, true);
        }
      }

      async function diffSnapshots() {
        if (!state.snapshotDiffBeforeId || !state.snapshotDiffAfterId) {
          setSnapshotMessage("Select two snapshots to diff.", true);
          return;
        }
        try {
          const payload = { before: state.snapshotDiffBeforeId, after: state.snapshotDiffAfterId };
          const result = await fetchJson("/inspector/snapshots/diff", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(payload),
          });
          if (elements.snapshotDiffOutput) {
            elements.snapshotDiffOutput.textContent = JSON.stringify(result, null, 2);
          }
          setSnapshotMessage("Snapshot diff complete.");
        } catch (error) {
          if (elements.snapshotDiffOutput) {
            elements.snapshotDiffOutput.textContent = `Diff failed: ${error.message || error}`;
          }
          setSnapshotMessage(`Diff failed: ${error.message || error}`, true);
        }
      }

      function renderSearchMessage(message) {
        elements.searchResults.innerHTML = "";
        if (!message) {
          return;
        }
        const li = document.createElement("li");
        li.className = "empty-state";
        li.textContent = message;
        elements.searchResults.appendChild(li);
      }

      function renderSearchResults(results, totalMatches) {
        elements.searchResults.innerHTML = "";
        if (!results.length) {
          elements.searchCount.textContent = `0 matches for "${state.searchQuery}"`;
          renderSearchMessage("No nodes matched the current query.");
          return;
        }
        const summary = totalMatches > results.length
          ? `${totalMatches} matches • showing first ${results.length}`
          : `${totalMatches} matches`;
        elements.searchCount.textContent = summary;
        results.forEach(node => {
          const li = document.createElement("li");
          li.className = "search-result";

          const title = document.createElement("div");
          title.className = "search-result-title";
          const code = document.createElement("code");
          code.textContent = node.path;
          title.appendChild(code);
          const badge = document.createElement("span");
          badge.className = "badge info";
          badge.textContent = node.value_type || "value";
          title.appendChild(badge);
          li.appendChild(title);

          const meta = document.createElement("div");
          meta.className = "watch-meta";
          const pieces = [`${node.child_count} children`];
          if (node.value_summary) {
            pieces.push(node.value_summary);
          }
          meta.textContent = pieces.join(" · ");
          li.appendChild(meta);

          const actions = document.createElement("div");
          actions.className = "watch-actions";
          const openBtn = document.createElement("button");
          openBtn.type = "button";
          openBtn.className = "secondary small";
          openBtn.textContent = "Open";
          openBtn.addEventListener("click", () => selectNode(node.path));
          actions.appendChild(openBtn);

          const watchBtn = document.createElement("button");
          watchBtn.type = "button";
          watchBtn.className = "secondary small";
          watchBtn.textContent = "Watch";
          watchBtn.addEventListener("click", () => addWatch(node.path));
          actions.appendChild(watchBtn);

          li.appendChild(actions);
          elements.searchResults.appendChild(li);
        });
      }

      function runSearch(queryOverride) {
        if (typeof queryOverride === "string") {
          state.searchQuery = queryOverride.trim();
        }
        if (!state.searchQuery) {
          elements.searchCount.textContent = "Enter a query to search the current snapshot.";
          renderSearchMessage("Search is idle.");
          return;
        }
        if (!state.liveTree) {
          elements.searchCount.textContent = "Load a snapshot to enable search.";
          renderSearchMessage("No snapshot loaded yet.");
          return;
        }
        const startedAt = typeof performance !== "undefined" ? performance.now() : Date.now();
        const matches = [];
        const needle = state.searchQuery.toLowerCase();
        visitTree(state.liveTree, node => {
          const haystack = (node.path + " " + (node.value_summary || "")).toLowerCase();
          if (haystack.includes(needle)) {
            matches.push(node);
          }
        });
        const limited = matches.slice(0, SEARCH_RESULT_LIMIT);
        renderSearchResults(limited, matches.length);
        const finishedAt = typeof performance !== "undefined" ? performance.now() : Date.now();
        const latency = Math.max(0, Math.round(finishedAt - startedAt));
        sendSearchMetrics({
          query: {
            latency_ms: latency,
            match_count: matches.length,
            returned_count: limited.length,
          },
        });
      }

      function applyTreeDelta(delta) {
        if (!state.liveTree || !delta) {
          return false;
        }
        let mutated = false;
        (delta.removed || []).forEach(path => {
          if (state.liveTree && path === state.liveTree.path) {
            state.liveTree = null;
            mutated = true;
          } else if (state.liveTree && removeNode(state.liveTree, path)) {
            mutated = true;
          }
        });
        (delta.updated || []).forEach(node => {
          if (!state.liveTree) {
            return;
          }
          if (node.path === state.liveTree.path) {
            state.liveTree = node;
            mutated = true;
          } else if (upsertNode(state.liveTree, node)) {
            mutated = true;
          }
        });
        (delta.added || []).forEach(node => {
          if (!state.liveTree) {
            state.liveTree = node;
            mutated = true;
          } else if (upsertNode(state.liveTree, node)) {
            mutated = true;
          }
        });
        return mutated;
      }

      function treeItemId(path) {
        if (!path) {
          return `${TREE_ITEM_ID_PREFIX}root`;
        }
        let hash = 0;
        for (let i = 0; i < path.length; i += 1) {
          hash = (hash * 33 + path.charCodeAt(i)) >>> 0;
        }
        const safe = path.replace(/[^a-zA-Z0-9_-]/g, "-") || "node";
        return `${TREE_ITEM_ID_PREFIX}${safe}-${hash.toString(16)}`;
      }

      function createTreeRowEntry(node, depth) {
        const label = node.path === "/" ? "/" : (node.path.split("/").pop() || "/");
        const childCount = Number(node.child_count ?? (
          Array.isArray(node.children) ? node.children.length : 0
        ));
        return {
          path: node.path,
          depth: depth || 0,
          label,
          valueType: node.value_type || "value",
          childCount,
          truncated: Boolean(node.children_truncated),
        };
      }

      function buildRowsFromNode(node, depth = 0, target = []) {
        if (!node) {
          return target;
        }
        target.push(createTreeRowEntry(node, depth));
        if (Array.isArray(node.children)) {
          node.children.forEach(child => buildRowsFromNode(child, depth + 1, target));
        }
        return target;
      }

      function rebuildTreeRows() {
        state.treeRows = state.liveTree ? buildRowsFromNode(state.liveTree, 0, []) : [];
        state.treeRowIndex = null;
        state.treeIndexDirty = true;
        state.treeRowsVersion += 1;
        state.treeErrorMessage = null;
      }

      function ensureTreeRowIndex() {
        if (!state.treeRows || !state.treeRows.length) {
          state.treeRowIndex = null;
          state.treeIndexDirty = false;
          return;
        }
        if (state.treeRowIndex && !state.treeIndexDirty) {
          return;
        }
        const map = new Map();
        state.treeRows.forEach((row, idx) => {
          map.set(row.path, idx);
        });
        state.treeRowIndex = map;
        state.treeIndexDirty = false;
      }

      function updateTreeActiveDescendant() {
        if (!elements.tree) {
          return;
        }
        if (state.selectedPath) {
          elements.tree.setAttribute("aria-activedescendant", treeItemId(state.selectedPath));
        } else {
          elements.tree.removeAttribute("aria-activedescendant");
        }
      }

      function removeTreeRow(path) {
        ensureTreeRowIndex();
        if (!state.treeRowIndex || !state.treeRowIndex.has(path)) {
          return false;
        }
        const start = state.treeRowIndex.get(path);
        const depth = state.treeRows[start].depth;
        let end = start + 1;
        while (end < state.treeRows.length && state.treeRows[end].depth > depth) {
          end += 1;
        }
        state.treeRows.splice(start, end - start);
        state.treeIndexDirty = true;
        return true;
      }

      function insertTreeRow(node) {
        ensureTreeRowIndex();
        if (!node || !node.path) {
          return false;
        }
        const parent = parentPath(node.path);
        if (!parent) {
          return false;
        }
        const parentIndex = state.treeRowIndex && state.treeRowIndex.get(parent);
        if (typeof parentIndex !== "number") {
          return false;
        }
        const parentDepth = state.treeRows[parentIndex].depth;
        const rowsToInsert = buildRowsFromNode(node, parentDepth + 1, []);
        let insertPos = parentIndex + 1;
        while (insertPos < state.treeRows.length) {
          const row = state.treeRows[insertPos];
          if (row.depth <= parentDepth) {
            break;
          }
          if (row.depth === parentDepth + 1 && row.path.localeCompare(node.path) > 0) {
            break;
          }
          insertPos += 1;
        }
        state.treeRows.splice(insertPos, 0, ...rowsToInsert);
        state.treeIndexDirty = true;
        return true;
      }

      function replaceTreeRow(node) {
        if (!node || !node.path) {
          return false;
        }
        const removed = removeTreeRow(node.path);
        const inserted = insertTreeRow(node);
        return removed || inserted;
      }

      function applyTreeRowsDelta(delta) {
        if (!delta || !state.treeRows.length) {
          rebuildTreeRows();
          return true;
        }
        let mutated = false;
        const removals = delta.removed || [];
        for (const path of removals) {
          if (!removeTreeRow(path)) {
            rebuildTreeRows();
            return true;
          }
          mutated = true;
        }
        const updates = delta.updated || [];
        for (const node of updates) {
          if (!replaceTreeRow(node)) {
            rebuildTreeRows();
            return true;
          }
          mutated = true;
        }
        const additions = delta.added || [];
        for (const node of additions) {
          if (!insertTreeRow(node)) {
            rebuildTreeRows();
            return true;
          }
          mutated = true;
        }
        if (mutated) {
          state.treeRowsVersion += 1;
        }
        return mutated;
      }

      function ensureTreeSelection() {
        const rows = state.treeRows || [];
        if (!rows.length) {
          if (state.selectedPath) {
            state.selectedPath = null;
            updateTreeActiveDescendant();
          }
          return;
        }
        ensureTreeRowIndex();
        if (state.selectedPath && state.treeRowIndex && state.treeRowIndex.has(state.selectedPath)) {
          updateTreeActiveDescendant();
          return;
        }
        const first = rows[0];
        if (first && first.path) {
          selectNode(first.path);
        }
      }

      function updateTreeEmptyState(message = null) {
        if (!elements.treeEmpty) {
          return;
        }
        if (!state.treeRows.length) {
          elements.treeEmpty.style.display = "block";
          elements.treeEmpty.textContent = message || "Load a snapshot to view the tree.";
        } else {
          elements.treeEmpty.style.display = "none";
        }
      }

      function setTreeError(message) {
        state.liveTree = null;
        state.treeRows = [];
        state.treeRowIndex = null;
        state.treeIndexDirty = true;
        state.treeErrorMessage = message || null;
        state.selectedPath = null;
        updateTreeActiveDescendant();
        requestTreeRender();
        updateTreeEmptyState(state.treeErrorMessage);
        scheduleWatchlistRefresh();
      }

      function requestTreeRender() {
        if (state.treeRenderQueued) {
          return;
        }
        state.treeRenderQueued = true;
        scheduleFrame(() => {
          state.treeRenderQueued = false;
          renderVirtualTree();
        });
      }

      function renderVirtualTree() {
        if (!elements.tree || !elements.treeWindow || !elements.treeSpacer) {
          return;
        }
        const rows = state.treeRows || [];
        if (!rows.length) {
          elements.treeWindow.innerHTML = "";
          elements.treeSpacer.style.height = "0px";
          updateTreeEmptyState(state.treeErrorMessage);
          updateTreeActiveDescendant();
          return;
        }
        updateTreeEmptyState(null);
        const totalHeight = rows.length * TREE_ROW_HEIGHT;
        elements.treeSpacer.style.height = `${totalHeight}px`;
        const viewport = elements.tree;
        const scrollTop = viewport.scrollTop || 0;
        const viewportHeight = viewport.clientHeight || TREE_ROW_HEIGHT;
        const start = Math.max(0, Math.floor(scrollTop / TREE_ROW_HEIGHT) - TREE_OVERSCAN_ROWS);
        const end = Math.min(rows.length, Math.ceil((scrollTop + viewportHeight) / TREE_ROW_HEIGHT) + TREE_OVERSCAN_ROWS);
        const fragment = document.createDocumentFragment();
        for (let i = start; i < end; i += 1) {
          fragment.appendChild(createTreeRowElement(rows[i]));
        }
        elements.treeWindow.style.transform = `translateY(${start * TREE_ROW_HEIGHT}px)`;
        elements.treeWindow.innerHTML = "";
        elements.treeWindow.appendChild(fragment);
        updateTreeActiveDescendant();
      }

      function createTreeRowElement(row) {
        const div = document.createElement("div");
        div.className = "tree-row";
        if (row.path === state.selectedPath) {
          div.classList.add("selected");
        }
        div.id = treeItemId(row.path);
        div.setAttribute("role", "treeitem");
        div.setAttribute("aria-level", String(Math.max(1, (row.depth || 0) + 1)));
        div.setAttribute("aria-selected", row.path === state.selectedPath ? "true" : "false");
        if (row.childCount > 0) {
          div.setAttribute("aria-expanded", "true");
        }
        div.tabIndex = -1;
        div.dataset.path = row.path;
        div.style.paddingLeft = `${Math.max(0, row.depth) * TREE_INDENT_PX}px`;

        const label = document.createElement("div");
        label.className = "tree-label";
        const name = document.createElement("span");
        name.className = "tree-label-name";
        name.textContent = row.label;
        label.appendChild(name);
        if (row.truncated) {
          const badge = document.createElement("span");
          badge.className = "badge warn tree-truncated";
          badge.textContent = "Truncated";
          label.appendChild(badge);
        }
        div.appendChild(label);

        const meta = document.createElement("span");
        meta.className = "tree-meta";
        const suffix = row.childCount === 1 ? "child" : "children";
        meta.textContent = `${row.valueType} · ${row.childCount} ${suffix}`;
        div.appendChild(meta);

        div.addEventListener("click", () => {
          if (elements.tree && document.activeElement !== elements.tree) {
            elements.tree.focus({ preventScroll: true });
          }
          selectNode(row.path);
        });
        return div;
      }

      function getTreeRowIndex(path) {
        if (!path) {
          return -1;
        }
        ensureTreeRowIndex();
        if (!state.treeRowIndex || !state.treeRowIndex.has(path)) {
          return -1;
        }
        return state.treeRowIndex.get(path);
      }

      function scrollTreeToIndex(index) {
        if (!elements.tree || !state.treeRows || !state.treeRows.length) {
          return;
        }
        const clamped = Math.max(0, Math.min(state.treeRows.length - 1, index));
        const viewport = elements.tree;
        const rowTop = clamped * TREE_ROW_HEIGHT;
        const rowBottom = rowTop + TREE_ROW_HEIGHT;
        const scrollTop = viewport.scrollTop || 0;
        const viewportHeight = viewport.clientHeight || TREE_ROW_HEIGHT;
        if (rowTop < scrollTop) {
          viewport.scrollTop = rowTop;
        } else if (rowBottom > scrollTop + viewportHeight) {
          viewport.scrollTop = rowBottom - viewportHeight;
        }
      }

      function moveTreeSelectionTo(index) {
        const rows = state.treeRows || [];
        if (!rows.length) {
          return;
        }
        const clamped = Math.max(0, Math.min(rows.length - 1, index));
        const nextRow = rows[clamped];
        if (!nextRow || nextRow.path === state.selectedPath) {
          return;
        }
        scrollTreeToIndex(clamped);
        selectNode(nextRow.path);
      }

      function moveTreeSelectionBy(delta) {
        const rows = state.treeRows || [];
        if (!rows.length) {
          return;
        }
        const currentIndex = getTreeRowIndex(state.selectedPath);
        if (currentIndex === -1) {
          moveTreeSelectionTo(delta > 0 ? 0 : rows.length - 1);
          return;
        }
        moveTreeSelectionTo(currentIndex + delta);
      }

      function treePageStep() {
        if (!elements.tree) {
          return 10;
        }
        const height = elements.tree.clientHeight || TREE_ROW_HEIGHT;
        return Math.max(1, Math.floor(height / TREE_ROW_HEIGHT) - 1);
      }

      function handleTreeKeyDown(event) {
        if (!state.treeRows || !state.treeRows.length) {
          return;
        }
        switch (event.key) {
          case "ArrowDown":
            event.preventDefault();
            moveTreeSelectionBy(1);
            break;
          case "ArrowUp":
            event.preventDefault();
            moveTreeSelectionBy(-1);
            break;
          case "Home":
            event.preventDefault();
            moveTreeSelectionTo(0);
            break;
          case "End":
            event.preventDefault();
            moveTreeSelectionTo((state.treeRows || []).length - 1);
            break;
          case "PageDown":
            event.preventDefault();
            moveTreeSelectionBy(treePageStep());
            break;
          case "PageUp":
            event.preventDefault();
            moveTreeSelectionBy(-treePageStep());
            break;
          case "Enter":
          case " ":
            if (state.selectedPath) {
              event.preventDefault();
              const index = getTreeRowIndex(state.selectedPath);
              if (index >= 0) {
                scrollTreeToIndex(index);
              }
              selectNode(state.selectedPath);
            }
            break;
          default:
            break;
        }
      }

      function scheduleWatchlistRefresh() {
        requestPanelWork("watchlist", updateWatchlistStatuses);
      }

      function syncTreeView(options = {}) {
        if (!state.liveTree) {
          state.treeRows = [];
          state.treeRowIndex = null;
          state.treeIndexDirty = true;
          state.treeErrorMessage = options.errorMessage || null;
          requestTreeRender();
          updateTreeEmptyState(state.treeErrorMessage);
          runSearch(state.searchQuery);
          scheduleWatchlistRefresh();
          ensureTreeSelection();
          return;
        }
        const reason = options.reason || "snapshot";
        if (reason === "delta" && options.delta) {
          if (!applyTreeRowsDelta(options.delta)) {
            rebuildTreeRows();
          }
        } else {
          rebuildTreeRows();
        }
        requestTreeRender();
        updateTreeEmptyState(null);
        runSearch(state.searchQuery);
        scheduleWatchlistRefresh();
        ensureTreeSelection();
      }

      async function fetchWithError(url, fetchOptions = {}) {
        const response = await fetch(url, { cache: "no-store", ...fetchOptions });
        if (!response.ok) {
          const text = await response.text();
          let details = null;
          if (text) {
            try {
              details = JSON.parse(text);
            } catch (err) {
              console.warn("Failed to parse error payload", err);
            }
          }
          const message = (details && details.message) || text || response.statusText || "Request failed";
          const error = new Error(message);
          error.details = details;
          error.status = response.status;
          throw error;
        }
        return response;
      }

      async function fetchJson(url, fetchOptions = {}) {
        const response = await fetchWithError(url, fetchOptions);
        return response.json();
      }

      async function refreshTree() {
        if (state.loadingTree) {
          return;
        }
        state.loadingTree = true;
        setStatus("Loading tree…", "warn", "Loading");
        const params = new URLSearchParams({
          root: state.root,
          depth: state.maxDepth,
          max_children: state.maxChildren,
          include_values: "true",
        });
        try {
          const data = await fetchJson(`/inspector/tree?${params}`);
          if (data.root) {
            state.liveTree = data.root;
            state.liveVersion += 1;
            state.aclError = null;
            state.blockedStreamRoot = null;
            syncTreeView({ reason: "snapshot" });
            applyPostSnapshotStatus();
            updateRootStatusDisplay();
          } else {
            setStatus("Snapshot missing root node", "err", "Error");
            state.aclError = null;
            state.blockedStreamRoot = null;
            setTreeError("Snapshot missing root");
            updateRootStatusDisplay();
          }
        } catch (error) {
          console.error(error);
          const details = error && error.details ? error.details : null;
          if (error && error.status === 403 && details && details.error === "inspector_acl_denied") {
            const message = details.message || error.message || "Access denied";
            const allowed = Array.isArray(details.allowed_roots) ? details.allowed_roots : [];
            state.aclError = { root: state.root, message, allowed };
            state.blockedStreamRoot = state.root;
            if (state.eventSource) {
              state.eventSource.close();
              state.eventSource = null;
            }
            setStatus(message, "err", "Forbidden");
            setTreeError(message);
            updateRootStatusDisplay();
          } else {
            setStatus("Tree request failed", "err", "Error");
            setTreeError(`Request failed: ${error && error.message ? error.message : error}`);
          }
        } finally {
          state.loadingTree = false;
        }
      }

      async function selectNode(path) {
        if (!path) {
          return;
        }
        state.selectedPath = path;
        updateTreeActiveDescendant();
        requestTreeRender();
        elements.nodeBadge.className = "badge warn";
        elements.nodeBadge.textContent = "Loading";
        elements.selectedPath.textContent = path;
        if (elements.tree) {
          elements.tree.setAttribute("aria-busy", "true");
        }
        try {
          const params = new URLSearchParams({
            path,
            depth: "4",
            max_children: state.maxChildren,
            include_values: "true",
          });
          const data = await fetchJson(`/inspector/node?${params}`);
          elements.nodeJson.textContent = JSON.stringify(data, null, 2);
          elements.nodeBadge.className = "badge ok";
          elements.nodeBadge.textContent = "Loaded";
        } catch (error) {
          elements.nodeBadge.className = "badge err";
          elements.nodeBadge.textContent = "Error";
          elements.nodeJson.textContent = `Failed to load node: ${error.message || error}`;
        } finally {
          if (elements.tree) {
            elements.tree.removeAttribute("aria-busy");
          }
        }
      }

      async function refreshPaintCard() {
        elements.summary.textContent = "Loading…";
        elements.manifest.textContent = "Loading…";
        elements.lastRun.textContent = "Loading…";
        try {
          const data = await fetchJson("/inspector/cards/paint-example");
          elements.summary.textContent = data.summary || "No summary";
          elements.manifest.textContent = JSON.stringify(data.manifest ?? {}, null, 2);
          elements.lastRun.textContent = JSON.stringify(data.last_run ?? {}, null, 2);
        } catch (error) {
          elements.summary.textContent = `Failed to load paint card: ${error.message || error}`;
          elements.manifest.textContent = "—";
          elements.lastRun.textContent = "—";
        }
      }

      function connectStream() {
        if (!window.EventSource) {
          setStatus("Streaming unsupported", "warn", "Polling");
          return;
        }
        if (state.blockedStreamRoot && state.blockedStreamRoot === state.root) {
          if (state.eventSource) {
            state.eventSource.close();
            state.eventSource = null;
          }
          setStatus(state.aclError ? state.aclError.message : "Access denied", "err", "Forbidden");
          return;
        }
        if (state.eventSource) {
          state.eventSource.close();
          state.eventSource = null;
        }
        const params = new URLSearchParams({
          root: state.root,
          depth: state.maxDepth,
          max_children: state.maxChildren,
          include_values: "true",
        });
        const stream = new EventSource(`/inspector/stream?${params}`);
        state.eventSource = stream;

        stream.onopen = () => {
          setStatus("Streaming", "ok", "Live");
        };

        stream.addEventListener("snapshot", event => {
          try {
            const payload = JSON.parse(event.data);
            if (payload.root) {
              state.liveTree = payload.root;
              state.liveVersion = payload.version || state.liveVersion + 1;
              syncTreeView({ reason: "snapshot" });
              setStatus("Streaming snapshot", "ok", "Live");
            }
          } catch (err) {
            console.error("Failed to parse snapshot", err);
          }
        });

        stream.addEventListener("delta", event => {
          try {
            const payload = JSON.parse(event.data);
            if (payload.changes) {
              if (!state.liveTree) {
                refreshTree();
                return;
              }
              applyTreeDelta(payload.changes);
              state.liveVersion = payload.version || state.liveVersion + 1;
              if (!state.liveTree) {
                refreshTree();
                return;
              }
              syncTreeView({ reason: "delta", delta: payload.changes });
              setStatus("Live updates", "ok", "Streaming");
            }
          } catch (err) {
            console.error("Failed to apply delta", err);
          }
        });

        stream.addEventListener("error", () => {
          if (state.blockedStreamRoot && state.blockedStreamRoot === state.root) {
            setStatus(state.aclError ? state.aclError.message : "Access denied", "err", "Forbidden");
            stream.close();
            state.eventSource = null;
            return;
          }
          setStatus("Stream disconnected", "warn", "Retrying");
        });
      }

      if (elements.tree) {
        elements.tree.addEventListener("scroll", () => {
          requestTreeRender();
        });
      }

      if (typeof window !== "undefined") {
        window.addEventListener("resize", () => requestTreeRender());
      }

      document.getElementById("snapshot-form").addEventListener("submit", (event) => {
        event.preventDefault();
        const nextRoot = elements.rootInput ? elements.rootInput.value : state.root;
        setRoot(nextRoot || "/", { refresh: false, syncUrl: false });
        const depthValue = Number(elements.depthInput ? elements.depthInput.value : state.maxDepth);
        state.maxDepth = Number.isNaN(depthValue) ? state.maxDepth : depthValue;
        const childValue = Number(elements.childrenInput ? elements.childrenInput.value : state.maxChildren);
        state.maxChildren = Number.isNaN(childValue) || childValue <= 0 ? state.maxChildren : childValue;
        updateUrlParams();
        refreshTree();
        connectStream();
      });

      if (elements.snapshotCaptureForm) {
        elements.snapshotCaptureForm.addEventListener("submit", event => {
          event.preventDefault();
          activatePanel("snapshots");
          captureSnapshot(event);
        });
      }
      if (elements.snapshotRefresh) {
        elements.snapshotRefresh.addEventListener("click", event => {
          event.preventDefault();
          activatePanel("snapshots");
          loadSnapshots(state.selectedSnapshotId);
        });
      }
      if (elements.snapshotSelect) {
        elements.snapshotSelect.addEventListener("change", event => {
          state.selectedSnapshotId = event.target.value || null;
        });
      }
      if (elements.snapshotExport) {
        elements.snapshotExport.addEventListener("click", event => {
          event.preventDefault();
          activatePanel("snapshots");
          downloadSnapshot();
        });
      }
      if (elements.snapshotDelete) {
        elements.snapshotDelete.addEventListener("click", event => {
          event.preventDefault();
          activatePanel("snapshots");
          deleteSnapshot();
        });
      }
      if (elements.snapshotBefore) {
        elements.snapshotBefore.addEventListener("change", event => {
          state.snapshotDiffBeforeId = event.target.value || null;
        });
      }
      if (elements.snapshotAfter) {
        elements.snapshotAfter.addEventListener("change", event => {
          state.snapshotDiffAfterId = event.target.value || null;
        });
      }
      if (elements.snapshotDiffButton) {
        elements.snapshotDiffButton.addEventListener("click", event => {
          event.preventDefault();
          activatePanel("snapshots");
          diffSnapshots();
        });
      }

      if (elements.rootSelect) {
        elements.rootSelect.addEventListener("change", event => {
          const value = event.target.value;
          if (value === "__custom__") {
            updateRootStatusDisplay();
            if (elements.rootInput) {
              elements.rootInput.focus();
            }
            return;
          }
          setRoot(value, { refresh: true });
        });
      }

      document.getElementById("refresh-paint").addEventListener("click", (event) => {
        event.preventDefault();
        activatePanel("paint");
        refreshPaintCard();
      });

      elements.searchForm.addEventListener("submit", event => {
        event.preventDefault();
        runSearch(elements.searchInput.value);
      });

      elements.searchInput.addEventListener("input", event => {
        if (event.target.value.trim() === "") {
          runSearch("");
        }
      });

      elements.searchClear.addEventListener("click", event => {
        event.preventDefault();
        elements.searchInput.value = "";
        runSearch("");
      });

      elements.watchForm.addEventListener("submit", event => {
        event.preventDefault();
        addWatch(elements.watchInput.value);
      });

      if (elements.savedSelect) {
        elements.savedSelect.addEventListener("change", event => {
          state.selectedWatchlistId = event.target.value || null;
        });
      }

      updateWriteButtons();
      renderWriteActions();
      setWriteWarning("Write controls disabled for this session.");

      if (elements.writeEnableButton) {
        elements.writeEnableButton.addEventListener("click", event => {
          event.preventDefault();
          activatePanel("write");
          requestWriteSession();
        });
      }
      if (elements.writeDisableButton) {
        elements.writeDisableButton.addEventListener("click", event => {
          event.preventDefault();
          activatePanel("write");
          disableWriteSession();
        });
      }
      if (elements.writeRefreshButton) {
        elements.writeRefreshButton.addEventListener("click", event => {
          event.preventDefault();
          activatePanel("write");
          refreshWriteActions();
        });
      }

      const saveButton = document.getElementById("watch-save");
      if (saveButton) {
        saveButton.addEventListener("click", event => {
          event.preventDefault();
          saveCurrentWatchlist({ forceNew: false });
        });
      }
      const saveNewButton = document.getElementById("watch-save-new");
      if (saveNewButton) {
        saveNewButton.addEventListener("click", event => {
          event.preventDefault();
          saveCurrentWatchlist({ forceNew: true });
        });
      }
      const loadButton = document.getElementById("watch-load");
      if (loadButton) {
        loadButton.addEventListener("click", event => {
          event.preventDefault();
          applySavedWatchlist();
        });
      }
      const deleteButton = document.getElementById("watch-delete");
      if (deleteButton) {
        deleteButton.addEventListener("click", event => {
          event.preventDefault();
          deleteSavedWatchlist();
        });
      }
      const exportButton = document.getElementById("watch-export");
      if (exportButton) {
        exportButton.addEventListener("click", event => {
          event.preventDefault();
          exportSavedWatchlist();
        });
      }
      const importButton = document.getElementById("watch-import-button");
      if (importButton && elements.watchImportInput) {
        importButton.addEventListener("click", event => {
          event.preventDefault();
          elements.watchImportInput.click();
        });
        elements.watchImportInput.addEventListener("change", event => {
          const file = event.target.files && event.target.files[0];
          if (file) {
            importWatchlistsFromFile(file);
          }
          event.target.value = "";
        });
      }

      window.addEventListener("beforeunload", () => {
        if (state.eventSource) {
          state.eventSource.close();
          state.eventSource = null;
        }
        if (state.metricsTimer) {
          clearInterval(state.metricsTimer);
          state.metricsTimer = null;
        }
        if (state.searchMetricsTimer) {
          clearInterval(state.searchMetricsTimer);
          state.searchMetricsTimer = null;
        }
        if (state.remoteStatusTimer) {
          clearInterval(state.remoteStatusTimer);
          state.remoteStatusTimer = null;
        }
      });

      const initialParams = loadInitialParameters();
      buildRootOptions();
      renderRemoteStatusBadges();
      updateRootStatusDisplay();
      if (!initialParams.hadRootParam) {
        updateUrlParams();
      }

      updateTreeEmptyState();
      refreshTree();
      connectStream();
      startMetricsPolling();
      startRemoteMetadataPolling();
      runSearch("");
    })();
  </script>
</body>
</html>
)HTML";

[[nodiscard]] auto read_file(std::string const& root, std::string_view relative_path)
    -> std::optional<std::string> {
    if (root.empty()) {
        return std::nullopt;
    }

    std::filesystem::path base{root};
    std::filesystem::path full = base / std::string(relative_path);

    std::ifstream stream(full, std::ios::binary);
    if (!stream) {
        return std::nullopt;
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[nodiscard]] auto deduce_content_type(std::string_view path) -> std::string_view {
    if (path.ends_with(".js")) {
        return "application/javascript; charset=utf-8";
    }
    if (path.ends_with(".css")) {
        return "text/css; charset=utf-8";
    }
    return "text/html; charset=utf-8";
}

} // namespace

auto LoadInspectorUiAsset(std::string const& ui_root, std::string_view relative_path)
    -> InspectorUiAsset {
    auto path = relative_path;
    if (path.empty()) {
        path = "index.html";
    }

    if (auto from_disk = read_file(ui_root, path)) {
        return InspectorUiAsset{
            .content      = std::move(*from_disk),
            .content_type = std::string{deduce_content_type(path)},
        };
    }

    return InspectorUiAsset{
        .content      = std::string{kEmbeddedIndexHtml},
        .content_type = "text/html; charset=utf-8",
    };
}

} // namespace SP::Inspector
