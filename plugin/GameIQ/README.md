# Game IQ UE Plugin

The deepest extraction backend for [Unreal Game IQ](../../README.md). Editor-only.
This folder's contents map to `<YourGame>/Plugins/GameIQ/` (design §10.1) — install it
with `gameiq init`, or copy it manually.

## What it produces

Everything here emits the **ExtractorOutput** JSON contract (`packages/shared`) into
`<ProjectDir>/.gameiq/extract/*.json`, which the Game IQ core ingests.

| Producer | Tier | Editor? | Output |
|---|---|---|---|
| `GameIQExport` commandlet (C++) | 0 — identity + dependency/referencer graph | headless | `registry.json` |
| `GameIQAssets` commandlet (C++) | 1 — typed asset summaries + semantic edges + level actor inventory | headless (loads assets) | `assets.json` |
| `GameIQBlueprints` commandlet (C++) | 2 — Blueprint graphs → pseudocode, plus variables/components/interfaces | headless (loads assets) | `blueprints.json` |
| in-editor bridge *(planned)* | 1/2 live | live | pushed to index |
| `gameiq_export.py` (Python) *(legacy/optional)* | 1 — subset of the above, in-editor | in-editor | `assets.json` |

`GameIQExport` is fast and loads no assets. `GameIQAssets` loads each non-Blueprint asset
for a typed recipe (meshes → LODs/tris/materials, textures → dimensions, skeletons → bones,
data tables → row struct, levels → actor inventory + per-class counts) and emits semantic
edges (`uses-material`, `uses-skeleton`, `placed-in-level`). `GameIQBlueprints` walks each
Blueprint's graphs into pseudocode and extracts its variables, components, and interfaces —
the "my agent read my Blueprint" pass.

## Usage

Easiest — the deploy script does copy + enable + build + extract + index:

```powershell
./scripts/deploy.ps1 -Project <ProjectRoot> -All
```

Or step by step:

```bash
# Tier 0/1/2, all headless (editor closed):
UnrealEditor-Cmd <Project>.uproject -run=GameIQExport      # registry.json
UnrealEditor-Cmd <Project>.uproject -run=GameIQAssets      # assets.json
UnrealEditor-Cmd <Project>.uproject -run=GameIQBlueprints  # blueprints.json
# optional: -out=<dir>   (default <ProjectDir>/.gameiq/extract)

# then, from the project root:
gameiq index --project .
```

`gameiq index` automatically ingests any `*.json` under `.gameiq/extract/`. Run the
commandlets with the editor **closed** (they load the project themselves).

## Build

This is a source plugin: it compiles with your project (a C++ project, or one UBT
converts). Requires UE 5.x with the **Python Editor Script Plugin** enabled. Ids use
prefix-less reflection names (`PlayerCharacter`, not `APlayerCharacter`) so edges line
up with the core's C++ extractor — keep that convention if you extend the recipes.
