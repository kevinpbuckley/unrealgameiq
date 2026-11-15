---
name: gameiq
display_name: Game IQ project index
description: Query the Game IQ index of THIS Unreal project (assets, Blueprints with pseudocode, C++ classes and function bodies, config, design docs, level actors, Level Sequences, DataTable rows, and the reference graph between them) instead of grepping binary .uasset files or reading the tree file by file. Use for "where is X used / who references X", "is this asset unused", "what loads this by path", "which sequences play this animation", "biggest textures", "where is this line of dialogue", "what does this Blueprint do", "what changed since commit X", "what breaks if I rename X", and doc search. Works in the editor (16 GameIQ.GameIQService tools) and without it (scripts/gameiq_query.py, read-only).
keywords:
  - gameiq
  - index
  - references
  - unused assets
  - dependency graph
  - impact
  - blueprint pseudocode
  - search
  - design docs
  - what changed
  - asset size
---

# Game IQ: ask the index before you grep

Game IQ keeps `<Project>/.gameiq/index.db` (SQLite + FTS5) in step with the project: an editor save
hook patches it on every asset save, delete and rename; C++ and design docs reindex themselves when
their files change; assets copied in without a save are caught up when the editor starts. A query
answers in milliseconds what a grep over `Content/` cannot answer at all (`.uasset` is binary) and
what reading the tree file by file answers slowly.

## Query it instead of grepping when the question is about the project

| Question | Call |
|---|---|
| Where is X implemented / anything about X | `Search("reload ammo")`, then `GetEntity(id)` |
| Who uses / references this asset or class | `References(id, "in")` (add `EdgeType`, e.g. `uses-skeleton`, `plays-anim`, `loads-by-path`) |
| What breaks if I delete or rename it | `Impact(id)` |
| Which assets are unused | `ProjectStats("unused")` — `unused` plus `possiblyLoadedByPath` (named only by a C++ `/Game/...` literal) |
| Biggest assets / what bloats the build | `ProjectStats("largest")`, `ProjectStats("largest:Texture2D")` |
| What does this Blueprint do | `GetBlueprintContext("/Game/Path/BP_X")` (pseudocode per graph) |
| Which sequences play this animation or sound, who is on screen at 14 s | `References(animId, "in", EdgeType="plays-anim")`; the sequence's `timeline` chunk via `GetEntity` |
| Where is this line of dialogue / this DataTable row | `Search("I was in the makeup trailer", Kind="datatable-row")` |
| What changed since a commit, a snapshot or a time | `Changes("HEAD~3")`, `Changes("2026-11-14T18:00:00Z")` |
| Everything about a system in one bounded pack | `Explain("combat damage", "standard", 20000, "", 1, "markdown")` |
| Architecture diagram | `ExportGraph(topicOrId, "mermaid")` |
| What does the design doc say, and does the build match | `Search(..., Kind="doc-section")`, `Drift()`, `Coverage()` |
| Results look empty, stale or wrong | `Doctor()` first (freshness per source, assets on disk but not indexed) |

Grep is still right for exact C++ flow tracing after the index points you at the files: the C++
`calls` graph is best-effort (no dynamic dispatch, delegates or calls through locals), and responses
on `cpp:` ids say so.

## How to call it

**In the editor, Python** (static methods on `unreal.GameIQService`, each returns a JSON string with
the payload under `"result"`):

```python
import json
hits = json.loads(unreal.GameIQService.search("player input", "blueprint"))["result"]
ent = json.loads(unreal.GameIQService.get_entity(hits[0]["entity"]["id"]))["result"]
```

**In a UE 5.9 `execute_python` script** (the native AI toolset runner): `call_tool` is injected and
awaited; it returns the tool's value (here the JSON string) directly. `unreal.ToolsetRegistry.execute_tool`
is rejected by the 5.9 runner — do not use it.

```python
import json
hits = json.loads(await call_tool("GameIQ.GameIQService.Search", query="player input", kind=""))["result"]
```

**Over MCP** `call_tool`: `toolset_name="GameIQ.GameIQService"`, `tool_name="Search"`,
`arguments={"query": "player input", "kind": ""}`. The answer arrives as `{"returnValue": "<json string>"}`:
parse twice. Parameter names are the C++ ones in camelCase (`query`, `kind`, `pathPrefix`,
`edgeType`, `sinceSnapshot`).

**Without the editor** (another agent holds it, Codex/Cursor, a subagent, CI): the read-only CLI
opens the same database with stock Python and prints the same envelope.

```
python Plugins/GameIQ/scripts/gameiq_query.py search "reload ammo" --kind blueprint
python Plugins/GameIQ/scripts/gameiq_query.py references asset:/Game/Audio/VO_REX_01 --direction in
python Plugins/GameIQ/scripts/gameiq_query.py stats unused
python Plugins/GameIQ/scripts/gameiq_query.py changes HEAD~3
python Plugins/GameIQ/scripts/gameiq_query.py doctor
```

Never `grep -a` the database file.

## The 16 tools (toolset `GameIQ.GameIQService`)

`Search(query, kind, limit, offset, pathPrefix)`, `GetEntity(id)`, `Children(id, classFilter, limit, offset)`,
`References(id, direction, depth, edgeType, kind, offset)`, `Impact(id, limit)`,
`Explain(topic, detailLevel, maxChars, kinds, depth, format)`, `GetBlueprintContext(idOrPath, detailLevel, maxChars, format)`,
`ExportGraph(topicOrId, format, direction, depth, edgeTypes, kinds, maxNodes)`,
`CreateSnapshot(name, metadataJson)`, `ListSnapshots(limit)`, `DeleteSnapshot(id)`,
`Changes(sinceSnapshot, untilSnapshot, kind, pathPrefix, limit, offset)`, `ProjectStats(facet)`,
`Drift()`, `Coverage(docType)`, `Doctor()`.

`ProjectStats` facets: `overview`, `kinds`, `edges`, `unused`, `largest`, `largest:<AssetClass>`,
`largest-deps`, `authority`, `doc-types`.

## Ids, kinds and edges

- Ids: `asset:/Game/Path/Name` (assets and Blueprints), `asset:/Game/Map::actor::Name` (placed
  actors), `asset:/Game/DT_X::row::RowName` (DataTable rows), `cpp:Class` and `cpp:Class::Function`
  (reflection prefix stripped: `cpp:CaseSubsystem`, not `cpp:UCaseSubsystem`; prefixed spellings are
  aliased), `cpp-file:Source/...` (asset paths outside parsed functions), `doc:Docs/File.md#anchor`.
  Copy ids from search results; an unknown id returns `error` + `didYouMean`, never a silent empty.
- Kinds: `asset`, `blueprint`, `level-actor`, `datatable-row`, `bp-function`, `bp-component`,
  `bp-variable`, `cpp-class`, `cpp-struct`, `cpp-function`, `cpp-property`, `cpp-file`,
  `config-section`, `plugin`, `document`, `doc-section`, `image`. A wrong kind filter returns an
  empty list: pass `""` when unsure.
- Edge types: `depends-on` (registry), `references` (`attrs.via` = property or column),
  `uses-material`, `uses-texture`, `uses-skeleton`, `placed-in-level`, `instance-of`, `inherits`,
  `implements`, `calls`, `binds-input`, `loads-by-path` (C++ `/Game/...` literal; patterns such as
  `asset-pattern:/Game/Anim/A_Idle_*` come from `%s` format strings and folder literals),
  `plays-anim` / `plays-audio` / `plays-sequence` (Level Sequences; `attrs.sections` = binding, start
  and end seconds), `describes` / `illustrates` (design docs).
- Every row carries `authority`: `extracted-fact` (from the build) or `stated-intent` (a design doc).
  A doc is what someone said, not what the game does.

## Freshness

Every response has `index.bySource` with the last ingest time per source (`cpp`, `assets`,
`blueprints`, `registry`, `docs`, `config`, …). `index.lastIngestAtIso` moves on any write, so read
the per-source time before trusting an old source. `Doctor()` also lists assets on disk that are not
in the index (by folder), docs changed since their last extraction, and missing agent-guide blocks.
Refresh by hand with the console commands `GameIQ.Rebuild` (all), `GameIQ.ReindexDocs`,
`GameIQ.ReindexCode`, or headless with `scripts/build-index.ps1 -Project <dir>`.

## Gotchas

- `ProjectStats("unused")` counts an asset as used only through a real inbound edge (not a self-edge,
  a design-doc mention, or its own child entities). Maps, startup assets and assets loaded through
  config or the Asset Manager can still appear: confirm with `References` and a grep of `Config/`
  before deleting. Anything in `possiblyLoadedByPath` is loaded by name from code.
- Sizes are uncooked on-disk package sizes, not cooked sizes.
- Snapshots: the editor takes one automatically when git HEAD moves (kept: newest 20). `Changes`
  with a ref that has no snapshot uses the newest snapshot taken before that commit and says so in
  `since.resolvedFrom`.
- Level actors are capped at 500 per map as entities; full per-class counts are always in the map's
  summary chunk. `Children(mapId, "Light")` pages one class.
