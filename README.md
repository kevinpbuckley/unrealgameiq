# Unreal Game IQ

> **Knows your game.** A continuously updated, queryable index of an Unreal Engine project — assets, Blueprints, C++, config, and how they connect — exposed to AI agents as a native UE 5.8 editor toolset.

See [design.md](design.md) for the full design.

## What this repo is

A single, all-C++ Unreal Engine 5.8 editor plugin — no Node, no external runtime:

```
plugin/
  GameIQ/   # the UE plugin (contents == <Game>/Plugins/GameIQ/)
scripts/    # deploy.ps1 (copy plugin into a project) + build-index.ps1
```

The plugin does everything in-process:

- **Extract** — commandlets read the project (Asset Registry graph, per-asset recipes, Blueprint
  pseudocode, `.ini`/`.uproject` config, C++ header reflection) into `<project>/.gameiq/extract/*.json`.
- **Store & query** — a SQLite index (`.gameiq/index.db`, FTS5) via UE's `SQLiteCore`, with the query
  engine (`search`/`get_entity`/`references`/`impact`/`explain`/`project_stats`) ported to C++.
- **Serve** — `UGameIQService`, a `UToolsetDefinition` registered on UE 5.8's native `ToolsetRegistry`,
  so GameIQ's queries appear on Epic's MCP endpoint for any agent already on the built-in UE MCP or
  VibeUE — zero extra setup, answered in-process.
- **Stay fresh** — an editor save hook patches the index directly on every asset save / delete / rename.

## Quick start

### 1. Deploy the plugin into a project

```powershell
pwsh scripts/deploy.ps1 -Project <path-to-project-dir>
```

Then build the editor target (e.g. via your project's build script) so `UnrealEditor-GameIQ.dll`
is compiled. The plugin enables its dependencies (`SQLiteCore`, `ToolsetRegistry`, `EnhancedInput`).

### 2. Build the index

**Once, headless** (editor closed):

```powershell
pwsh scripts/build-index.ps1 -Project <path-to-project-dir>
```

This runs the `GameIQBuild` commandlet — every extractor plus the SQLite ingest — in a single editor
boot, writing `<project>/.gameiq/index.db`.

**From a running editor**, rebuild without closing it:

- **Tools ▸ Game IQ panel** — dockable Slate panel with index stats and two buttons: **Rebuild Index**
  (full — every extractor + ingest) and **Reindex Documents** (docs/images/external sources only, much
  faster; leaves code/asset entities untouched).
- **Console commands** — `GameIQ.Rebuild` and `GameIQ.ReindexDocs` do the same two things from the
  console, handy for scripts or an AI agent driving the editor (e.g. `unreal.SystemLibrary.execute_console_command(world, "GameIQ.Rebuild")`).

Either path spawns the commandlet as a background `UnrealEditor-Cmd` process (never in-process, so the
live editor stays responsive) and streams live progress — a per-stage editor notification, a live
stage/log readout in the panel if it's open, and the same lines in the Output Log under
`LogGameIQRunner`. Each extractor stage logs how long it took, and the Assets/Blueprints stages
(the ones doing full synchronous asset loads) log a heartbeat every ~2s while they run, so a long
stage on a big project reads as "still working" rather than "stuck". Every run also appends a
timing record — per-stage seconds plus the total — to `<project>/.gameiq/build-timings.json` (last
20 runs kept), so you can compare rebuild performance over time as the project grows.

After the first build, the in-editor **save hook** keeps the index current automatically; a full
rebuild is only needed after large out-of-editor changes (e.g. a VCS sync).

### 3. Query it from an AI agent

Open the project in the editor. GameIQ registers `GameIQ.GameIQService` on UE's MCP endpoint, so an
agent on the native UE MCP (or VibeUE) can call it with no setup:

| tool | what it answers |
|---|---|
| `Search` | hybrid FTS search across everything (optional `Kind` filter) |
| `GetEntity` | full detail: summary, chunks (BP pseudocode / recipe), edges, child entities |
| `References` | who uses this / what this uses (`Direction`, `Depth`, `EdgeType` filter) |
| `Impact` | what could break if this changes (severity-ranked dependents) |
| `Explain` | assembled context bundle for a system/topic |
| `ProjectStats` | counts, unused-asset candidates, largest dependencies |

From Python it mirrors VibeUE's services, e.g.
`unreal.GameIQService.search("reload ammo", "blueprint")`.
Example: *"which meshes use this skeleton?"* → `References` with `EdgeType: "uses-skeleton"`, `Direction: "in"`.

### Excluding directories (config)

The editor-less config/C++ walk indexes the whole project tree, including third-party plugin source.
To keep the index focused on *your* game, add a `gameiq.config.json` at the project root:

```json
{ "exclude": ["Plugins/VibeUE", "Plugins/SomeMarketplacePlugin"] }
```

Entries match a directory name (`VibeUE`) or a project-relative path (`Plugins/VibeUE`). Game IQ's own
plugin is always excluded. You can also edit the list in the editor under
**Project Settings → Plugins → Game IQ**.

## Status

v0.1 MVP, in development. See design §7 for the MVP cut and success criterion. The entire pipeline —
extract, store, query, live update — runs inside Unreal; there is no Node dependency.
