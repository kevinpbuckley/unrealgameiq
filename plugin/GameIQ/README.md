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
| `GameIQBlueprints` commandlet (C++) | 2 — Blueprint graphs → pseudocode + `calls` edges | headless (loads assets) | `blueprints.json` |
| `gameiq_export.py` (Python) | 1 — typed per-asset summaries | in-editor | `assets.json` |
| in-editor bridge *(planned)* | 1/2 live | live | pushed to index |

`GameIQExport` is fast and loads no assets. `GameIQBlueprints` loads each Blueprint and
walks its event/function graphs (K2 nodes + exec pins) into greppable pseudocode — the
"my agent read my Blueprint" pass. The Python pass fills in per-asset Tier 1 detail.

## Usage

Easiest — the deploy script does copy + enable + build + extract + index:

```powershell
./scripts/deploy.ps1 -Project <ProjectRoot> -All
```

Or step by step:

```bash
# Tier 0 (fast, no asset loading) and Tier 2 (loads assets), both headless:
UnrealEditor-Cmd <Project>.uproject -run=GameIQExport
UnrealEditor-Cmd <Project>.uproject -run=GameIQBlueprints
# optional: -out=<dir>   (default <ProjectDir>/.gameiq/extract)

# Tier 1, in-editor (or headless via the python commandlet):
UnrealEditor-Cmd <Project>.uproject -run=pythonscript -script="gameiq_export.py"

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
