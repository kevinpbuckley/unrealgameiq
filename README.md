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

The **core** is a standalone Node app installed once and run across many projects; it writes
each project's index to `<project>/.gameiq/`. It never opens Unreal for the editor-less tiers.
The **plugin** is the deepest extraction backend (Tier 1/2), invoked as a headless commandlet or
talked to live via the in-editor bridge.

## Quick start (development)

```bash
npm install
npm run build
npm test

# index a project (editor-less tiers + ingest of any extractor JSON)
npm run gameiq -- index --project <path-to-uproject-dir>

# run the MCP server over stdio
npm run gameiq -- mcp --project <path-to-uproject-dir>
```

## Status

v0.1 MVP, in development. See design §7 for the MVP cut and success criterion.
