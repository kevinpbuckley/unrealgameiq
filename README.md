# Unreal Game IQ

> **Knows your game.** A continuously updated, queryable index of an Unreal Engine project — assets, Blueprints, C++, config, and how they connect — exposed to AI agents as a native UE 5.8 editor toolset.

See [design.md](design.md) for the full design.

## What this repo is

A single, all-C++ Unreal Engine 5.8 editor plugin — no Node, no external runtime:

```
plugin/
  GameIQ/   # the UE plugin (contents == <Game>/Plugins/GameIQ/)
scripts/    # deploy.ps1 (copy plugin into a project), build-index.ps1, reindex-docs.ps1
```

The plugin does everything in-process:

- **Extract** — commandlets read the project (Asset Registry graph, per-asset recipes, Blueprint
  pseudocode, `.ini`/`.uproject` config, C++ headers *and* function bodies, design docs
  (.md/.txt/.pdf/.docx), images, and exported external docs) into `<project>/.gameiq/extract/*.json`.
- **Store & query** — a SQLite index (`.gameiq/index.db`, FTS5) via UE's `SQLiteCore`, with the query
  engine (`search`/`get_entity`/`children`/`references`/`impact`/`explain`/`project_stats`/`drift`/
  `coverage`/`doctor`) ported to C++.
- **Serve** — `UGameIQService`, a `UToolsetDefinition` registered on UE 5.8's native `ToolsetRegistry`,
  so GameIQ's queries appear on Epic's MCP endpoint for any agent already on the built-in UE MCP or
  VibeUE — zero extra setup, answered in-process.
- **Stay fresh** — an editor save hook patches the index on every asset save / delete / rename, and a
  source-tree fingerprint check reindexes C++ automatically on editor startup and after Live Coding
  patches. No manual rebuilds in the normal edit loop.
- **Teach the agent** — `GameIQ.GenerateAgentConfig` writes a ready-made usage guide into the project
  root (`CLAUDE.md` / `GEMINI.md` / `AGENTS.md` / Copilot instructions) so any coding agent knows how
  to call the index.

## Quick start

### 1. Install the plugin

Clone this repo into your project's `Plugins/` folder:

```powershell
cd <YourProject>/Plugins
git clone https://github.com/kevinpbuckley/unrealgameiq.git
```

Unreal discovers the nested `.uplugin` automatically — no need to move anything. Enable
**Unreal Game IQ** in Edit → Plugins if needed, then build the editor target so
`UnrealEditor-GameIQ.dll` is compiled. The plugin enables its dependencies (`SQLiteCore`,
`ToolsetRegistry`, `EnhancedInput`). (Alternative: `pwsh scripts/deploy.ps1 -Project <path>`
copies just `plugin/GameIQ/` into the project without the git nesting.)

### 2. Build the index

**Once, headless** (editor closed), from your project root:

```powershell
pwsh Plugins/unrealgameiq/scripts/build-index.ps1 -Project .
```

This runs the `GameIQBuild` commandlet — every extractor plus the SQLite ingest — in a single editor
boot, writing `<project>/.gameiq/index.db`.

**From a running editor**, rebuild without closing it:

- **Tools ▸ Game IQ panel** — dockable Slate panel with index stats and two buttons: **Rebuild Index**
  (full — every extractor + ingest) and **Reindex Documents** (docs/images/external sources only, much
  faster; leaves code/asset entities untouched).
- **Console commands** — `GameIQ.Rebuild`, `GameIQ.ReindexDocs`, and `GameIQ.ReindexCode` (C++ only)
  do the same from the console, handy for scripts or an AI agent driving the editor (e.g.
  `unreal.SystemLibrary.execute_console_command(world, "GameIQ.Rebuild")`).

Either path spawns the commandlet as a background `UnrealEditor-Cmd` process (never in-process, so the
live editor stays responsive) and streams live progress — a per-stage editor notification, a live
stage/log readout in the panel if it's open, and the same lines in the Output Log under
`LogGameIQRunner`. Each extractor stage logs how long it took, and the Assets/Blueprints stages
(the ones doing full synchronous asset loads) log a heartbeat every ~2s while they run, so a long
stage on a big project reads as "still working" rather than "stuck". Every run also appends a
timing record — per-stage seconds plus the total — to `<project>/.gameiq/build-timings.json` (last
20 runs kept), so you can compare rebuild performance over time as the project grows.

After the first build the index maintains itself: the in-editor **save hook** patches it on every
asset save / delete / rename, and the **C++ freshness check** reindexes code automatically when the
editor starts (or a Live Coding patch lands) with changed sources — toggle under
**Project Settings → Plugins → Game IQ → Auto Reindex Cpp**. A full rebuild is only needed after
large out-of-editor changes (e.g. a VCS sync).

### 3. Query it from an AI agent

Open the project in the editor. GameIQ registers `GameIQ.GameIQService` on UE's MCP endpoint, so an
agent on the native UE MCP (or VibeUE) can call it with no setup:

| tool | what it answers |
|---|---|
| `Search` | hybrid FTS search across everything (optional `Kind` / path-prefix filter) |
| `GetEntity` | full detail: summary, chunks (BP pseudocode / recipe / C++ bodies), edges, children |
| `Children` | page an entity's children with a class filter (e.g. every Light in a level) |
| `References` | who uses this / what this uses (`Direction`, `Depth`, `EdgeType` filter) |
| `Impact` | what could break if this changes (severity-ranked dependents) |
| `Explain` | assembled context bundle for a system/topic |
| `ProjectStats` | counts, unused-asset candidates, largest dependencies, authority ranking |
| `Drift` | doc-stated `property = value` claims that contradict the implementation |
| `Coverage` | which stated design intent is backed by a real implementation |
| `Doctor` | index health — distinguishes "empty project" from "broken index" |

From Python it mirrors VibeUE's services, e.g.
`unreal.GameIQService.search("reload ammo", "blueprint")`.
Example: *"which meshes use this skeleton?"* → `References` with `EdgeType: "uses-skeleton"`, `Direction: "in"`.

### Excluding directories (config)

The editor-less config/C++ walk indexes the whole project tree, including third-party plugin source.
To keep the index focused on *your* game, add a `gameiq.config.json` at the project root:

```json
{ "exclude": ["Plugins/unrealgameiq", "Plugins/SomeMarketplacePlugin"] }
```

Entries match a directory name (`VibeUE`) or a project-relative path (`Plugins/VibeUE`). Add
`Plugins/unrealgameiq` (or all of `Plugins`) so Game IQ's own source stays out of your index.
You can also edit the list in the editor under **Project Settings → Plugins → Game IQ**.

## Status

v0.1 beta. Requires **UE 5.8** (the serve layer sits on 5.8's experimental `ToolsetRegistry`);
developed and tested on **Windows** — the core is portable C++ but Mac/Linux are unverified and the
helper scripts are PowerShell. The entire pipeline — extract, store, query, live update — runs inside
Unreal; there is no Node dependency. See design §7 for the MVP cut and success criterion, and
`GameIQTests.cpp` for the automation suite (`Automation RunTests GameIQ`).
