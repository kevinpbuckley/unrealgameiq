# Unreal Game IQ

> **Knows your game.** A continuously updated, queryable index of an Unreal Engine project — assets, Blueprints, C++, config, and how they connect — exposed to you and to AI agents.

See [design.md](design.md) for the full design.

## What this repo is

A monorepo with two distributable artifacts (see design §10.1):

```
packages/
  shared/   # the entity/edge/chunk + extractor-output schema (the cross-artifact contract)
  core/     # TS: CLI, MCP server, SQLite index, query engine  → publishes to npm
plugin/
  GameIQ/   # the UE plugin (contents == <Game>/Plugins/GameIQ/) — deep extraction backend
```

Two layers with opposite runtime needs (design §6.5):

- **Extraction** *builds* the index — it loads assets, so it needs the UE binary (the plugin's
  commandlets run headless via `UnrealEditor-Cmd`, no UI, but not UE-free).
- **Serving** *answers* from the index — it only reads `<project>/.gameiq/index.db` (SQLite+FTS),
  so it needs **no UE at all**. This is the Node core (CLI + MCP + query engine), and it's why
  the MCP lives outside Unreal: it answers with the editor — and often UE entirely — closed.

## Quick start (development)

```bash
npm install
npm run build
npm test
```

### Index a real project

The plugin's three commandlets (Tier 0 registry, Tier 1 assets, Tier 2 Blueprints) write JSON
into `<project>/.gameiq/extract/`, which the core ingests. The deploy script does it all:

```bash
# copy the plugin into the project, build the editor target, run the commandlets, index:
pwsh scripts/deploy.ps1 -Project <path-to-uproject-dir> -All

# or just re-index after extraction (editor-less):
npm run gameiq -- index --project <path-to-uproject-dir>
```

### Use it from an AI agent (MCP)

The core serves the index over the Model Context Protocol. Register it with any MCP client
(here, a project `.mcp.json` for Claude Code):

```json
{
  "mcpServers": {
    "game-iq": {
      "command": "npx",
      "args": ["-y", "@gameiq/core", "mcp", "--project", "."]
    }
  }
}
```

Until `@gameiq/core` is published, point at the local build instead:
`"command": "node", "args": ["<repo>/packages/core/dist/cli/index.js", "mcp", "--project", "."]`.

The server exposes one multi-action `game_iq` tool (few-tools pattern, design §6.1):

| action | what it answers |
|---|---|
| `search` | hybrid search across everything (optional `kind` filter) |
| `get_entity` | full detail: summary, chunks (BP pseudocode / recipe), edges, and child entities |
| `references` | who uses this / what this uses (`direction`, `depth`, `edgeType` filter) |
| `impact` | what could break if this changes (severity-ranked dependents) |
| `explain` | assembled context bundle for a system/topic |
| `project_stats` | counts, unused-asset candidates, largest dependencies |

Example: *"which meshes use this skeleton?"* → `references` with `edgeType: "uses-skeleton"`, `direction: "in"`.

## Status

v0.1 MVP, in development. See design §7 for the MVP cut and success criterion.
