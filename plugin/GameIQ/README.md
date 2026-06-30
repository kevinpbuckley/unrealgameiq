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
| `gameiq_export.py` (Python) | 1 — typed per-asset summaries | in-editor | `assets.json` |
| in-editor bridge *(planned)* | 1/2 live + Tier 2 pseudocode via `ExportNodesToText` | live | pushed to index |

The C++ commandlet is fast and loads no assets. The Python pass fills in per-asset
detail. **Tier 2** (Blueprint/material logic → pseudocode) is deliberately a C++
`FEdGraphUtilities::ExportNodesToText` job (design §5.1), not Python — tracked for the
in-editor bridge.

## Usage

```bash
# Tier 0, headless — no interactive editor:
UnrealEditor-Cmd <Project>.uproject -run=GameIQExport
# optional: -out=<dir>   (default <ProjectDir>/.gameiq/extract)

# Tier 1, in-editor (or headless via the python commandlet):
UnrealEditor-Cmd <Project>.uproject -run=pythonscript -script="gameiq_export.py"

# then, from the project root:
gameiq index --project .
```

`gameiq index` automatically ingests any `*.json` under `.gameiq/extract/`.

## Build

This is a source plugin: it compiles with your project (a C++ project, or one UBT
converts). Requires UE 5.x with the **Python Editor Script Plugin** enabled. Ids use
prefix-less reflection names (`PlayerCharacter`, not `APlayerCharacter`) so edges line
up with the core's C++ extractor — keep that convention if you extend the recipes.
