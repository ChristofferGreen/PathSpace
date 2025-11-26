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
    }
    body {
      margin: 0;
      padding: 0;
      min-height: 100vh;
      display: flex;
      flex-direction: column;
      background: linear-gradient(180deg, rgba(15,17,25,1) 0%, rgba(14,19,32,1) 60%, rgba(9,11,17,1) 100%);
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
    #tree {
      list-style: none;
      margin: 0;
      padding: 0;
      overflow: auto;
      font-family: "SFMono-Regular", Consolas, Menlo, monospace;
      font-size: 0.85rem;
    }
    #tree li {
      margin-bottom: 0.25rem;
    }
    .tree-node {
      display: inline-flex;
      gap: 0.4rem;
      align-items: center;
      padding: 0.15rem 0.35rem;
      border-radius: 6px;
      cursor: pointer;
    }
    .tree-node:hover {
      background: rgba(37,99,235,0.35);
    }
    .tree-node.selected {
      background: rgba(79,70,229,0.8);
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
    <div class="status-bar" style="width: auto;">
      <span id="status-text">waiting for snapshot…</span>
      <span id="status-badge" class="badge warn">Idle</span>
    </div>
  </header>
  <main>
    <form class="controls" id="snapshot-form">
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

    <div class="layout">
      <section class="panel">
        <h2>Tree</h2>
        <ul id="tree"></ul>
      </section>
      <section class="panel">
        <h2>Node details</h2>
        <div class="status-bar" style="margin-bottom: 0.75rem;">
          <span id="selected-path">Select a node to view details.</span>
          <span id="node-badge" class="badge warn">No node</span>
        </div>
        <pre id="node-json">{}</pre>
      </section>
    </div>

    <section class="panel">
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
      const state = {
        root: "/",
        maxDepth: 2,
        maxChildren: 64,
        loadingTree: false,
        selectedPath: null,
      };

      const elements = {
        tree: document.getElementById("tree"),
        nodeJson: document.getElementById("node-json"),
        selectedPath: document.getElementById("selected-path"),
        nodeBadge: document.getElementById("node-badge"),
        statusText: document.getElementById("status-text"),
        statusBadge: document.getElementById("status-badge"),
        summary: document.getElementById("paint-summary"),
        manifest: document.getElementById("paint-manifest"),
        lastRun: document.getElementById("paint-last-run"),
      };

      function setStatus(text, badgeClass, badgeLabel) {
        elements.statusText.textContent = text;
        elements.statusBadge.className = "badge " + badgeClass;
        elements.statusBadge.textContent = badgeLabel;
      }

      async function fetchJson(url) {
        const response = await fetch(url, { cache: "no-store" });
        if (!response.ok) {
          throw new Error(await response.text() || response.statusText);
        }
        return response.json();
      }

      function renderTree(node, depth = 0) {
        const li = document.createElement("li");
        const button = document.createElement("span");
        button.className = "tree-node";
        button.dataset.path = node.path;
        button.textContent = node.path.split("/").pop() || "/";

        const meta = document.createElement("span");
        meta.className = "node-meta";
        meta.textContent = `${node.value_type || "value"} · ${node.child_count} children`;
        button.appendChild(meta);

        button.addEventListener("click", () => {
          selectNode(node.path);
        });

        li.appendChild(button);

        if (node.children && node.children.length > 0) {
          const ul = document.createElement("ul");
          ul.style.listStyle = "none";
          ul.style.margin = "0 0 0.25rem 0.75rem";
          ul.style.padding = "0";
          node.children.forEach(child => {
            ul.appendChild(renderTree(child, depth + 1));
          });
          li.appendChild(ul);
        } else if (node.children_truncated) {
          const span = document.createElement("div");
          span.className = "node-meta";
          span.style.marginLeft = "0.75rem";
          span.textContent = "…additional children truncated";
          li.appendChild(span);
        }

        return li;
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
          elements.tree.innerHTML = "";
          if (data.root) {
            elements.tree.appendChild(renderTree(data.root));
            setStatus("Snapshot loaded", "ok", "Ready");
          } else {
            setStatus("Snapshot missing root node", "err", "Error");
          }
        } catch (error) {
          console.error(error);
          setStatus("Tree request failed", "err", "Error");
          elements.tree.innerHTML = `<li class="node-meta">Request failed: ${error.message || error}</li>`;
        } finally {
          state.loadingTree = false;
        }
      }

      async function selectNode(path) {
        state.selectedPath = path;
        elements.nodeBadge.className = "badge warn";
        elements.nodeBadge.textContent = "Loading";
        elements.selectedPath.textContent = path;
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
          document.querySelectorAll(".tree-node").forEach(node => {
            if (node.dataset.path === path) {
              node.classList.add("selected");
            } else {
              node.classList.remove("selected");
            }
          });
        } catch (error) {
          elements.nodeBadge.className = "badge err";
          elements.nodeBadge.textContent = "Error";
          elements.nodeJson.textContent = `Failed to load node: ${error.message || error}`;
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

      document.getElementById("snapshot-form").addEventListener("submit", (event) => {
        event.preventDefault();
        state.root = document.getElementById("root-input").value || "/";
        state.maxDepth = Number(document.getElementById("depth-input").value || 2);
        state.maxChildren = Number(document.getElementById("children-input").value || 64);
        refreshTree();
      });

      document.getElementById("refresh-paint").addEventListener("click", (event) => {
        event.preventDefault();
        refreshPaintCard();
      });

      refreshTree();
      refreshPaintCard();
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
