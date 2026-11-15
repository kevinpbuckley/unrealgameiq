# Unreal Game IQ

> **Knows your game.** A continuously updated, queryable index of an Unreal Engine project — assets, Blueprints, C++, config, and how they connect — exposed to AI agents as a native UE 5.9 editor toolset and a read-only command-line query tool.

See [design.md](design.md) for the full design.

## What this repo is

A single, all-C++ Unreal Engine 5.9 editor plugin — no Node, no external runtime. The repository
root is also the deployable Unreal plugin root:

```
GameIQ.uplugin
Source/GameIQ/
Content/
Skills/gameiq/     # agent skill, discovered natively by UE 5.9 (/Skills/GameIQ/gameiq)
scripts/           # deployment and index helpers, plus the read-only query CLI
```

The plugin does everything in-process:

- **Extract** — commandlets read the project (Asset Registry graph, per-asset recipes, Blueprint
  pseudocode, `.ini`/`.uproject` config, C++ headers *and* function bodies, design docs
  (`.md`/`.txt`/`.html`/`.htm`/`.pdf`/`.docx`), images, and exported external docs) into
  `<project>/.gameiq/extract/*.json`. HTML headings become searchable document sections; scripts,
  styles, comments, and markup are omitted from the indexed text.
- **Store & query** — a SQLite index (`.gameiq/index.db`, FTS5) via UE's `SQLiteCore`, with the query
  engine (`search`/`get_entity`/`children`/`references`/`impact`/`explain`/`get_blueprint_context`/
  `export_graph`/`snapshots`/`changes`/`project_stats`/`drift`/`coverage`/`doctor`) ported to C++.
- **Serve** — `UGameIQService`, a `UToolsetDefinition` registered on UE 5.9's native `ToolsetRegistry`,
  so GameIQ's queries appear on Epic's MCP endpoint for any agent already on the built-in UE MCP or
  VibeUE — zero extra setup, answered in-process.
- **Stay fresh** — an editor save hook patches the index on every asset save / delete / rename; a
  source-tree fingerprint check reindexes C++ automatically on editor startup and after Live Coding
  patches; the docs folder is watched and design docs reindex when they change (a background
  `UnrealEditor-Cmd` run, at most one every 5 minutes by default); and a catch-up scan on
  editor start adds assets that arrived without a save (marketplace packs, a VCS sync). Every response
  says how fresh each source is (`index.bySource`). No manual rebuilds in the normal edit loop.
- **Query without the editor** — `scripts/gameiq_query.py` opens `.gameiq/index.db` read-only with stock
  Python and answers search / entity / references / children / impact / stats / changes / doctor, for
  agents that can't hold the editor (another agent has it, Codex, Cursor, subagents, CI).
- **Teach the agent** — the plugin ships `Skills/gameiq/SKILL.md` (UE 5.9 lists it in `ListSkills` with no
  setup), and `GameIQ.GenerateAgentConfig` writes a ready-made usage guide into the project root
  (`CLAUDE.md` / `GEMINI.md` / `AGENTS.md` / Copilot instructions) and adds `.gameiq/` to `.gitignore`.
  Doctor, the Tools ▸ Game IQ panel and the editor log warn while no agent file carries the guide.

## Quick start

### 1. Install the plugin

Clone this repo into your project's `Plugins/` folder:

```powershell
cd <YourProject>/Plugins
git clone https://github.com/kevinpbuckley/unrealgameiq.git GameIQ
```

This produces the conventional `<YourProject>/Plugins/GameIQ/GameIQ.uplugin` layout. Enable
**Unreal Game IQ** in Edit → Plugins if needed, then build the editor target so
`UnrealEditor-GameIQ.dll` is compiled. The plugin enables its dependencies (`SQLiteCore`,
`ToolsetRegistry`, `EnhancedInput`). (Alternative: `pwsh scripts/deploy.ps1 -Project <path>`
copies the plugin payload into another project without its Git metadata or development files.)

### 2. Build the index

**Once, headless** (editor closed), from your project root:

```powershell
pwsh Plugins/GameIQ/scripts/build-index.ps1 -Project .
```

The script finds the project's `*.uproject` (its name need not match the folder) and the engine from the
`.uproject`'s `EngineAssociation`; pass `-Engine <engine root>` to override.

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

### 3. Tell your agents, and keep the index out of git

In the editor console run `GameIQ.GenerateAgentConfig` (or click **Write Agent Guide** in Tools ▸ Game IQ).
It writes the usage block into `CLAUDE.md`, `GEMINI.md` and `AGENTS.md` and adds `.gameiq/` to `.gitignore`.
An installed but unannounced index goes unused: in one 48-hour project agents made 2 index queries and
about 1,200 greps.

### 4. Query it from an AI agent

Open the project in the editor. GameIQ registers `GameIQ.GameIQService` on UE's MCP endpoint, so an
agent on the native UE MCP (or VibeUE) can call it with no setup:

| tool | what it answers |
|---|---|
| `Search` | hybrid FTS search across everything (optional `Kind` / path-prefix filter) |
| `GetEntity` | full detail: summary, chunks (BP pseudocode / recipe / C++ bodies), edges, children |
| `Children` | page an entity's children with a class filter (e.g. every Light in a level) |
| `References` | who uses this / what this uses (`Direction`, `Depth`, `EdgeType` filter) |
| `Impact` | what could break if this changes (severity-ranked dependents) |
| `Explain` | character-budgeted context bundle; detail, kind, depth, and JSON/Markdown controls |
| `GetBlueprintContext` | compact Blueprint identity and prioritized pseudocode/recipe chunks by id or path |
| `ExportGraph` | deterministic bounded graph in Mermaid, Graphviz DOT, or structured JSON |
| `CreateSnapshot` / `ListSnapshots` / `DeleteSnapshot` | manage named, consistent local index snapshots |
| `Changes` | semantic entity, Blueprint logic, and relationship changes since a snapshot, a git ref (`HEAD~3`) or a time |
| `ProjectStats` | counts, unused assets (and assets only C++ path literals load), largest assets by size, largest dependencies, authority |
| `Drift` | doc-stated `property = value` claims that contradict the implementation |
| `Coverage` | which stated design intent is backed by a real implementation |
| `Doctor` | index health — "empty project" vs "broken index", freshness per source, assets on disk but not indexed, stale docs |

From Python it mirrors VibeUE's services, e.g.
`unreal.GameIQService.search("reload ammo", "blueprint")`. Inside a UE 5.9 `execute_python` script use
the injected `call_tool`: `await call_tool("GameIQ.GameIQService.Search", query="reload ammo", kind="")`
(it returns the JSON string; `unreal.ToolsetRegistry.execute_tool` is rejected by the 5.9 runner).

With the editor closed or held by someone else, the same queries run from the command line:

```powershell
python Plugins/GameIQ/scripts/gameiq_query.py search "reload ammo" --kind blueprint
python Plugins/GameIQ/scripts/gameiq_query.py references asset:/Game/Audio/VO_Line --direction in
python Plugins/GameIQ/scripts/gameiq_query.py stats unused
python Plugins/GameIQ/scripts/gameiq_query.py changes HEAD~3
python Plugins/GameIQ/scripts/gameiq_query.py doctor
```
Example: *"which meshes use this skeleton?"* → `References` with `EdgeType: "uses-skeleton"`, `Direction: "in"`.

Use a context pack when an agent needs portable system or Blueprint context without repeated calls:

```python
import unreal
context = unreal.GameIQService.explain("combat damage", "standard", 20000, "blueprint,cpp-class", 2, "markdown")
blueprint = unreal.GameIQService.get_blueprint_context("/Game/Combat/BP_Damage", "logic", 12000, "json")
diagram = unreal.GameIQService.export_graph("combat damage", "mermaid", "both", 2, "", "", 75)
```

For reviewable agent work, snapshot before the mutation and compare after GameIQ's normal save-hook
freshness boundary:

```python
import json, unreal
before = json.loads(unreal.GameIQService.create_snapshot("before combat task", '{"run":"combat-42"}'))
snapshot_id = before["result"]["id"]
# ...perform, save, compile, and verify changes...
changes = unreal.GameIQService.changes(snapshot_id, "current", "", "/Game/Combat", 200, 0)
```

Snapshots stay local under `.gameiq/snapshots`; `Changes` reports full counts, paging/truncation,
timestamps, and semantic provenance. VibeUE's run journal can attach the snapshot id and change report.

Snapshots are also tied to git: every manifest records `gitHead`/`gitBranch`, and while the editor is open
it snapshots the index each time HEAD moves (Project Settings ▸ Plugins ▸ Game IQ ▸ Snapshot Per Commit;
the newest 20 are kept). So `Changes("HEAD~3")`, `Changes("<sha>")` or `Changes("2026-11-14T18:00:00Z")`
answers "what changed in assets, code and docs since then" without a hand-made snapshot.

### Excluding directories (config)

The editor-less config/C++ walk indexes the whole project tree, including third-party plugin source.
To keep the index focused on *your* game, add a `gameiq.config.json` at the project root:

```json
{ "exclude": ["Plugins/GameIQ", "Plugins/SomeMarketplacePlugin"] }
```

Entries match a directory name (`VibeUE`), a project-relative path (`Plugins/VibeUE`), or a glob over
either (`Docs/Archive*`, `*.bak.md`). Add `Plugins/GameIQ` (or all of `Plugins`) so Game IQ's own source
stays out of your index. Every dot-directory (`.git`, `.claude`, `.vs`, …) and every linked git
worktree (a folder whose `.git` file points into `worktrees/`, i.e. a second copy of the project) is
always skipped; submodules and nested clones are kept.
You can also edit the list in the editor under **Project Settings → Plugins → Game IQ**.

## Status

v0.1 beta. Requires **UE 5.9** on this branch (the serve layer sits on the experimental `ToolsetRegistry`;
5.9 also needs the empty-string bind fix in `GameIQSqlBind.h`);
developed and tested on **Windows** — the core is portable C++ but Mac/Linux are unverified and the
helper scripts are PowerShell. The entire pipeline — extract, store, query, live update — runs inside
Unreal; there is no Node dependency. See design §7 for the MVP cut and success criterion, and
`GameIQTests.cpp` for the automation suite (`Automation RunTests GameIQ`).
