# Unreal Game IQ — Design Document

**Status:** Draft v0.1 · June 2026
**Owner:** Buckley Builds LLC
**Related products:** [VibeUE](https://www.vibeue.com/) (acts on a live editor), [Unreal Engine Skills](https://www.unrealengineskills.com/) (engine knowledge for agents)

---

## 1. One-line pitch

**Unreal Game IQ knows your game.** It builds a continuously updated, queryable model of an Unreal Engine project — assets, Blueprints, C++, config, and how they connect and change over time — and exposes that model to you and to AI agents. Ask a real question about your game and get a grounded, cited answer; point an agent at your project and it stops guessing.

## 2. The problem

An Unreal project is the worst-indexed codebase in software:

1. **Half the logic is invisible to text tools.** Blueprints, DataTables, Behavior Trees, materials, and levels are binary `.uasset` files. `grep` returns nothing. An AI agent pointed at the repo literally cannot read half the game. This is the single biggest reason agents underperform on UE projects.
2. **The connective tissue is implicit.** "What breaks if I delete this texture?" "Who calls this event?" "Which levels use this Blueprint?" The answers exist only inside the editor's Asset Registry and reference viewer — slow to open, impossible for an agent to use, and gone the moment the editor closes.
3. **Knowledge lives in heads.** "How does the damage system work?" spans a C++ component, two Blueprints, a DataTable, and a config file. Nobody documented it; the one person who knew left the team or forgot.
4. **Change is opaque.** A `.uasset` diff in source control is a binary blob. "What actually changed in combat since the last milestone?" is unanswerable without replaying history in the editor.

Existing tools each see a slice: the editor sees everything but only interactively; IDEs see C++ only; source control sees files, not meaning. Nothing holds the whole game in one queryable place.

## 3. Product pillars (matches the site)

| Pillar | Meaning |
|---|---|
| **Knows your game** | A persistent index of every asset, Blueprint, class, and the dependency/reference graph between them — kept fresh as the project changes, with history. |
| **Answers, not searches** | Natural-language questions get grounded, cited answers ("Fall damage is applied in `BP_PlayerCharacter` → `OnLanded`, which calls `UHealthComponent::ApplyDamage` defined in `HealthComponent.cpp:142`"), not a list of 200 search hits. |
| **Built for AI workflows** | The index is exposed as an MCP server. Any agent (Claude Code, Copilot, Cursor) gets project context as tools — the missing third leg next to engine knowledge (UE Skills) and editor control (VibeUE). |

## 4. Who it's for

- **Primary: solo devs and small teams using AI agents.** They want the agent to stop guessing about their project. Game IQ is what makes "fix the bug in my reload logic" work when the reload logic is a Blueprint.
- **Secondary: the humans on those teams.** Onboarding ("explain the inventory system"), hygiene ("what's unused?", "why is the build 40 GB?"), and impact analysis before risky changes.
- **Later: mid-size studios** with Perforce, build farms, and stricter IP requirements. Local-first design (see §9) is what keeps this door open.

## 5. How it works — the loop

```
                ┌──────────────────────────────────────────────┐
                │                YOUR PROJECT                  │
                │  .uproject  Source/  Content/  Config/  VCS  │
                └──────┬───────────────────────────┬───────────┘
                       │ extract                   │ watch
                       ▼                           ▼
            ┌─────────────────┐          ┌──────────────────┐
            │   EXTRACTORS    │          │  CHANGE WATCHER  │
            │ asset registry  │◄─────────│ fs events + VCS  │
            │ BP graph dump   │ re-index │ commits          │
            │ C++ reflection  │  deltas  └──────────────────┘
            │ config/ini      │
            └────────┬────────┘
                     ▼
            ┌─────────────────┐
            │  KNOWLEDGE BASE │   entities + edges + text + vectors
            │  (local SQLite) │   snapshots over time
            └────────┬────────┘
                     ▼
            ┌─────────────────┐
            │   QUERY ENGINE  │   graph queries · hybrid search · synthesis
            └────────┬────────┘
                     ▼
        ┌────────────┴────────────┐
        ▼                         ▼
  ┌───────────┐            ┌────────────┐
  │ MCP SERVER│            │  CLI / UI  │
  │ (agents)  │            │  (humans)  │
  └───────────┘            └────────────┘
```

### 5.1 Extraction (the hard, defensible part)

Extraction must cover **every `.uasset` in the project — not just Blueprints**. A question like "which meshes still use the old skeleton?" or "what's driving this material parameter?" is exactly the kind of thing Game IQ exists to answer. To make full coverage tractable, extraction is a small *framework* rather than a fixed list of extractors: every asset gets shallow coverage for free **from UE's own reflection/export machinery** (so "an extractor per asset type" is largely a myth — see below), and high-value asset classes get bespoke deep extractors ("recipes") that are mostly curation on top of that free baseline. Three depth tiers:

- **Tier 0 — Registry (every asset, free).** From the Asset Registry: identity, class, package path, tags/metadata, and the **dependency/referencer graph**. Covers all assets of every type with zero per-type code, and alone answers "what uses X?".
- **Tier 1 — Typed summary (per-class recipes).** Structured extraction of what each asset *is*: its shape, settings, and outgoing semantic links.
- **Tier 2 — Logic rendering (the killer feature).** Graph-bearing assets rendered as **pseudocode/structured text** ("On BeginPlay → if HasWeapon → Spawn BP_Muzzle…") so their logic is greppable, embeddable, and quotable in answers. The engine hands us the raw graph as faithful text via `FEdGraphUtilities::ExportNodesToText` (the BP-node clipboard format) — **no bytecode decompilation** — so the only custom code is the per-graph-family renderer (K2 Blueprint, material expressions, anim state machines, Behavior Trees) that turns that node/pin text into pseudocode.

Initial recipe set, covering the asset classes that dominate real questions:

| Asset class | Tier 1 summary | Tier 2 logic |
|---|---|---|
| Blueprint (Actor/Component/Widget/etc.) | parent class, variables, components, functions, dispatchers, interfaces | event/function graphs as pseudocode |
| Material / Material Function | shading model, blend mode, parameters, texture refs, usage flags | expression graph as pseudocode |
| Material Instance | parent chain + **only the overridden parameters** | — |
| Skeleton | bone hierarchy, sockets, slots, curve names | — |
| Skeletal Mesh | skeleton link, LODs, material slots, morph targets, vert/tri counts, physics asset | — |
| Static Mesh | LODs, material slots, UV channels, collision setup, sockets, tri counts | — |
| Texture | dimensions, format, compression, sRGB, mip/LOD settings, on-disk size | — |
| Anim Sequence / Montage / BlendSpace | skeleton, length, curves, notifies, slots | — |
| Anim Blueprint | target skeleton, anim graph structure | state machines + transition rules as text |
| Behavior Tree / Blackboard | composites, decorators, services, tasks, blackboard keys | tree as indented text |
| Niagara System / Emitter | emitters, modules, exposed parameters | module stack summary |
| Level / World | actor inventory (label, class, transform); per-actor detail — lights: intensity/color/mobility, static-mesh actors: mesh; per-class counts; each actor a searchable chunk | — |
| Input Mapping Context / Input Action | key→action mappings (`IA_Jump ← SpaceBar…`), action value types, `references`(via `maps`) edges | — |
| DataTable / CurveTable | row struct + row count *(full row data: planned)* | — |
| Sound Cue / MetaSound | *(generic reflection fallback today)* | graph summary *(planned)* |
| Physics Asset, GameplayTags, DataAssets, everything else | typed **property bag via reflection fallback** (`ExportTextItem` over every UPROPERTY) + object-ref edges | — |

**Reflection export is the foundation, not a fallback — no asset is invisible.** UE serializes any loaded UObject's `UPROPERTY`s to structured text with *zero per-type code* — via `FJsonObjectConverter` (UStruct→JSON), the lower-level `FProperty::ExportTextItem`, or the generic `ObjectExporterT3D`; for Levels/Actors, `LevelExporterT3D`/`ActorExporterT3D` (the editor's own copy/paste format) yield the actor inventory and per-asset overrides directly. This property-bag export runs for *every* asset class — plugin types, marketplace systems, new engine features — guaranteeing 100% coverage. A Tier 1 recipe is then a thin **curation** pass over that free baseline (pick the ~10 fields that matter, name the semantic edges), plus a small garnish of **computed** values that aren't stored as properties (tri counts, LOD screen sizes, texture dimensions, on-disk size — small accessor calls). Recipes are versioned and pluggable so users/plugins can register their own for custom asset types. *(All of this needs the UObject loaded — i.e. the commandlet/in-editor path; it does nothing for the editor-less path, which still relies on raw file parsing — see §11.)*

Alongside the asset framework, two non-asset extractors:

- **C++ extractor.** Parses `Source/` for the reflection surface (UCLASS / USTRUCT / UFUNCTION / UPROPERTY / delegates) plus class hierarchy and file locations. MVP uses a lightweight header parse (the reflection macros make UE headers unusually regular); a clangd-index backend can come later for call graphs. Cross-links everywhere: a BP node calling a `UFUNCTION`, a mesh referencing a C++ component class, a DataTable using a USTRUCT row type — all become edges.
- **Config/project extractor.** `.ini` files, `.uproject`/`.uplugin`, enabled plugins, maps list, input actions, GameplayTags. Small, but answers a disproportionate number of real questions.

**Editor-optional is a core principle** — with a boundary the implementation makes precise. Truly editor-less today: the **C++ header parse** and **config/`.ini`/`.uproject` parse** (pure TypeScript, no UE). Everything that reads a binary `.uasset` — including Tier 0's Asset Registry graph — currently runs through the UE binary as a **headless commandlet** (no interactive editor, but not UE-free). As shipped there are **three commandlets**: `GameIQExport` (Tier 0 registry graph), `GameIQAssets` (Tier 1 typed recipes + level actor inventory), and `GameIQBlueprints` (Tier 2 pseudocode + variables/components/interfaces). The design's *editor-less* Tier 0 (parsing the on-disk Asset Registry / `.uasset` files without UE, UAssetAPI-style, §11) is a deferred fallback tier — until it lands, a machine that has never built the project gets only the C++/config layers. The live in-editor bridge (instant freshness, unsaved state) remains planned.

### 5.2 Knowledge base

Local-first **SQLite** in `<project>/.gameiq/` (gitignored by default; optionally committed as a team-shared cache).

Data model — deliberately boring:

- **`entities`** — one row per thing: any asset (mesh, material, texture, skeleton, level…), Blueprint, BP function/variable, material parameter, socket, level actor, C++ class/function/property, config section, plugin, **design-doc section**. Columns: id, kind, name, path, parent, **`source` (code / asset / config / doc — fact vs. intent)**, summary, raw JSON detail (the recipe output).
- **`edges`** — typed relations: generic `references`/`depends-on` from the registry, plus semantic types from recipes: `inherits`, `calls`, `implements`, `uses-material`, `uses-texture`, `uses-skeleton`, `overrides-parameter`, `placed-in-level`, `plays-on`, `casts-to`. Typed edges are what turn "what references X" into "which *meshes* use this *skeleton*."
- **`chunks`** — text units for retrieval (BP pseudocode per function, recipe summaries per asset, C++ signatures + doc comments, config blocks, ingested design-doc sections) with **FTS5** full-text index and an **embedding vector** (small local model by default; pluggable).
- **`snapshots`** — index state per VCS revision (or timestamp), so "what changed since X" is a diff of two snapshots, with binary `.uasset` changes rendered as *semantic* diffs ("`BP_Enemy`: function `TakeDamage` modified — 3 nodes added; variable `Armor` default 50 → 75"). *(planned)*

**Producer-scoped ingest.** Every entity/edge/chunk is tagged with the `producer` that emitted it (`gameiq-cpp`, `gameiq-ue-registry`, `gameiq-ue-assets`, `gameiq-ue-bp`, `gameiq-config`, …). Re-ingesting a producer replaces only *its own* rows, so producers compose without ordering assumptions or cross-clobbering — e.g. the registry owns a Blueprint's entity while the blueprints producer adds a structure chunk to it, and either can re-run independently. This is what makes the multi-producer, incrementally-refreshed model safe.

### 5.3 Query engine

Three tiers, cheapest first:

1. **Graph queries** — exact, instant: lookups, reference traversal, impact analysis (transitive closure over `edges`), stats. No LLM involved.
2. **Hybrid retrieval** — FTS5 + vector search over `chunks`, fused and reranked. Powers "find anything about reloading."
3. **Synthesis (`ask`)** — retrieval feeds an LLM that composes an answer **with citations** (entity ids → asset paths / `file:line`). Every claim must trace to an entity; uncited claims are dropped. When invoked via MCP, the *calling agent* is the synthesizer — Game IQ returns assembled, cited context. Game IQ only needs its own LLM (bring-your-own-key Claude) for the human-facing CLI/UI `ask`.

### 5.4 Design-intent ingestion (read-only)

Beyond the project's own files, Game IQ ingests **external design documents** — Game Design Doc, Level Design Docs, brand/style guidelines — as a first-class knowledge source. **Scope is ingestion, not management: Game IQ reads and indexes these docs; it is never a place to author, edit, or store them.** They stay in whatever the team already uses (Google Drive, Notion, Confluence, local markdown); Game IQ pulls, chunks, and embeds them into the same store.

Two phases, deliberately:

- **Phase 1 — ingest as chunks; let the agent connect (the near-term plan).** Doc sections land as ordinary `chunks` (FTS5 + embeddings), retrievable side-by-side with Blueprint pseudocode, C++ signatures, and asset summaries. There is **no explicit doc→code edge graph** — when an agent asks "is the stamina system implemented?", hybrid retrieval surfaces both the GDD section and the relevant code/assets in one bundle, and the **agent does the intent→implementation reasoning itself**, exactly as it already reasons over mixed context. Cheap, ships early, and sidesteps the hard linking problem entirely.
- **Phase 2 — explicit links + conformance (later).** Once Phase 1 proves useful, add typed edges (`GDD§"Damage" —describes→ UHealthComponent`, `BrandGuide —constrains→ M_UI_Button`) to power deterministic **intent-vs-implementation conformance / drift** queries that don't rely on the agent connecting the dots each time:
  - "Brand guide primary color is `#1B9AAA` — which materials/widgets violate it?"
  - "GDD describes a stamina system — is any of it implemented, or still just a doc?"
  - "Combat changed since the milestone (the §5.2 semantic diffs) — does the GDD still match?"

Two rules hold across both phases:

- **Local-first holds (§9).** Docs are pulled and indexed *locally*; game content is never pushed to a doc service. A cloud doc source is a *read* source, not an exfiltration path.
- **Provenance is tagged.** Every entity/chunk records its `source` (code / asset / config / doc). Code is fact; a doc is *intent*. An answer never presents "the GDD says…" as ground truth — even in Phase 1, where the agent is doing the connecting, the provenance tag keeps the two sides distinct.

Connectors are pluggable in the same spirit as recipes (§5.1): start with local markdown + Google Drive, add Notion/Confluence as demand shows. **v0.3+** — the chunk model already accommodates Phase 1; Phase 2 reuses the existing entity/edge machinery once the core technical index is proven.

### 5.5 Scoping the index (config)

"Knows *your* game" means the index shouldn't be dominated by third-party plugin internals. By default the editor-less walk sweeps the whole tree, so a project-root **`gameiq.config.json`** (committed) lists directories to exclude — matched by name (`VibeUE`) or project-relative path (`Plugins/VibeUE`); Game IQ's own plugin is always excluded. Same list is editable in the editor under **Project Settings → Plugins → Game IQ** (`UGameIQSettings`), which writes the JSON; `gameiq index --exclude <dir…>` overrides ad hoc. (On ThirdPerson58, excluding `Plugins/VibeUE` dropped ~2,400 vendored-plugin C++ entities from the index.)

## 6. Surfaces

### 6.1 MCP server (the product's center of gravity)

`gameiq mcp` (stdio, local; same shape as the rest of the ecosystem). Following the UE Skills MCP v2 consolidation — **few tools, rich parameters** — this shipped as **one `game_iq` multi-action tool** (matching `unreal-engine-skills-manager`), keeping the tool count at one in agent contexts. Actions:

| Action | What it does |
|---|---|
| `search(query, kind?)` | Hybrid search (FTS5 today) across all chunks; ranked entities with a snippet. |
| `get_entity(id)` | Full detail: summary, chunks (BP pseudocode / recipe), edges, and child entities. **Bounded** — arrays capped with full `counts` + a `truncated` flag so a level with hundreds of actors stays usable. |
| `references(id, direction, depth?, edgeType?, kind?)` | Who uses this / what it uses. `edgeType` filters the relation (`uses-skeleton`, `placed-in-level`, …); `kind` filters the result entity kind. |
| `impact(id)` | Transitive "what could break if I change or delete this," severity-ranked by edge type. |
| `explain(topic)` | Retrieval bundle for a system: seed hits + immediate neighborhood, pre-assembled context. |
| `project_stats(facet)` | Counts, unused-asset candidates, largest dependencies — the hygiene queries. |

Every response carries an index-age stamp (design §8). `changes(since)` (semantic diff) is **planned** — pending snapshots (§5.2). The server is stdio and spawned on demand by the MCP client (`claude mcp add <name> -- node …/cli mcp --project <dir>`); user-scoped registrations should use an absolute `--project` path.

### 6.2 CLI

`gameiq index` (build/refresh), `gameiq watch`, `gameiq ask "..."`, `gameiq impact <asset>`, `gameiq changes --since <rev>`, `gameiq stats`. The CLI is also the CI entry point (index on the build machine; fail PRs that delete still-referenced assets).

### 6.3 The ecosystem story

This is the moat — three products, one loop, all under Buckley Builds:

- **Unreal Engine Skills** = how the *engine* works (universal knowledge).
- **Unreal Game IQ** = how *your game* works (project knowledge). ← this product
- **VibeUE** = hands on the *live editor* (action).

An agent task like "add fall damage" becomes: Game IQ `explain("damage")` → grounded plan → UE Skills for correct engine APIs → edit C++ / drive VibeUE for Blueprint changes → Game IQ re-indexes and verifies the new edges exist. Each product is useful alone; together they're a complete agent stack for UE. Cross-promote accordingly.

### 6.4 Why not just VibeUE? (probe vs. index)

VibeUE can already read a Blueprint, its references, and live diffs — so the fair challenge is "why is Game IQ not redundant?" The answer is that those are **point inspections against a live editor**: VibeUE is a *probe* (editor open, project loaded, you point it at one thing you already know). Game IQ is *persistent memory* — a precomputed graph + searchable corpus + history that answers with the editor closed. Same primitive ("read this asset"); opposite system. VibeUE inspects; Game IQ remembers and searches.

Game IQ earns its existence **only** on the four axes a live probe is structurally bad at — and at least one must be in the MVP demo or the product has no reason to exist next to VibeUE:

- **Scale** — "which of my 4,000 meshes use the deprecated skeleton?" is one indexed graph query, not thousands of sequential live reads.
- **Availability** — answers at 2am, in CI, in a git hook, on a machine that never opened the editor.
- **Discovery** — "where is reload handled?" when you *don't* know the asset name (hybrid search over a corpus VibeUE doesn't have).
- **History** — "what changed in combat since the milestone?" diffed against any VCS revision, offline.

Relationship, not rivalry: Game IQ does **not** reimplement VibeUE's live reading. VibeUE is the best in-editor extraction surface; Game IQ writes those reads into the persistent index so the same knowledge is queryable whole-project, offline, by any agent, across time. (Game IQ ships its own UE plugin rather than bundling into VibeUE — see §10.1 — but the bridge patterns are shared.)

### 6.5 Epic Developer Assistant / Toolset Registry (UE 5.8)

UE 5.8 ships (Experimental, `EditorOnly`, `NoRedist`, off by default) the **`AIAssistant`** plugin ("Epic Developer Assistant" / EDA) and a **`ToolsetRegistry`** framework: you register a `UToolsetDefinition` with static `UFUNCTION`s marked `meta=(AICallable)`, and the in-editor assistant can call them. This is the "AI Editor Toolset" path.

**Decision: the standalone MCP server stays the product; the Toolset Registry is a *fourth surface*, not a replacement.** The Toolset path is `EditorOnly`, in-process, and reaches only Epic's EDA — it contradicts Game IQ's two non-negotiables (editor-optional, agent-agnostic) and rests on an experimental, Epic-controlled API. So:

- Keep the MCP/CLI core (§6.1–6.2) as center of gravity.
- Add a thin `UToolsetDefinition` adapter that exposes `search_project`/`impact`/`explain` to the EDA — cheap, and it puts Game IQ *inside* Epic's assistant with live **dock context** (the EDA exposes the focused asset/graph/selected nodes) the headless extractor can't see. A later, optional surface.
- Read it competitively too: EDA + its `AgentSkill` concept + project/user context prompts is Epic encroaching on all three Buckley Builds products at once (engine knowledge, project context, in-editor action) — a reason to own the cross-agent, editor-optional, historical layer Epic won't build, not to nest inside their assistant.

## 7. MVP (v0.1) — cut ruthlessly

**In:**
- Tier 0 for **all** assets (Asset Registry, editor-less) + C++ header reflection parse + config parse.
- The extraction commandlet with the **generic property-bag fallback** (so every asset type has *some* depth from day one) and Tier 1 recipes for the highest-traffic classes: Blueprint, Material/Material Instance, Skeleton, Skeletal/Static Mesh, Texture, Anim assets, DataTable, Level.
- SQLite store, FTS5 search (embeddings behind a flag), graph queries with typed edges.
- MCP server with `search_project`, `get_entity`, `references`, `impact`, `project_stats`.
- CLI: `gameiq index`, `gameiq mcp`. Windows first.

**The one hard MVP feature:** Tier 2 pseudocode for Blueprints (headless commandlet) — the demo that makes people gasp ("my agent just read my Blueprint"). Other Tier 2 renderings (materials, Anim BP state machines, Behavior Trees) follow in v0.2 on the same machinery.

**Out (v0.2+):** live watcher, snapshots/`changes`, semantic uasset diffing, `explain` bundles, embeddings-by-default, Perforce, dashboard UI, team sync (Turso — same infra as the MCP usage tracking), Mac/Linux, design-doc ingestion + intent-vs-implementation conformance (§5.4, v0.3+).

**Success criterion:** on a real project, an agent with Game IQ correctly answers "where is X handled and what references it?" for Blueprint-implemented logic that the same agent without Game IQ cannot answer at all.

## 8. Freshness model

Freshness has two separable jobs, and conflating them is the common mistake: **detection** (what changed) is cheap and editor-free; **extraction** (reading the changed thing) is not, because a binary `.uasset` must be loaded into a UE process to render Tier 1/2 — detecting the change doesn't read it.

- **Detection** — a filesystem watcher on `Content/` + `Source/` (a `.uasset` only hits disk on *save*, which is the right granularity) plus a VCS hook. Editor-free, and it naturally covers both "I saved" and "I synced/pulled." Text artifacts (C++, config) and the Tier 0 graph are *also* extracted here with no editor.
- **Extraction of changed binary assets** — needs a live UE process. Three modes, increasing freshness:
  1. **`gameiq index`** (MVP) — explicit full or `--changed` incremental run via the headless commandlet; the Asset Registry makes incremental cheap.
  2. **Watcher → debounced commandlet** (v0.2) — the watcher batches changed packages and fires a headless `UnrealEditor-Cmd -run=GameIQExport`. No interactive editor; freshness in *seconds* (bounded by UE process cold-start, so batch — don't run per-save).
  3. **In-editor bridge** (the plugin) — when the editor is already open during active work, the asset is already loaded in memory: sub-second freshness, and it's the only mode that can see *unsaved* in-editor edits. This is what live agent workflows need.
- Every query response carries an index-age stamp so agents (and humans) know how stale the answer might be.

The plugin is therefore **in scope** (not merely an optional nicety): the watcher+commandlet path can't see unsaved state and pays cold-start, so the live mode is the bridge. See §10.1 for how the plugin is delivered.

## 9. Privacy & IP

Non-negotiable for adoption: **the index never leaves the developer's machine by default.** No cloud ingestion of game content. Embeddings computed locally. Telemetry limited to anonymous usage counts (opt-out), same convention as the skills MCP usage tracking. Team-sync of the index (later) goes through infrastructure the team controls or an explicit opt-in cloud tier. This is both the right thing and a sales weapon against any cloud-first competitor.

## 10. Tech stack (proposed)

Game IQ is **a standalone Node/TypeScript app, not a UE plugin** — the plugin is one (deepest) extraction backend, not the product. The core installs once via `npx gameiq` and runs across many projects, writing each project's index to its `.gameiq/`. It never opens Unreal for the editor-less tiers; it invokes the commandlet / talks to the in-editor bridge only for deep binary extraction.

- **Core / CLI / MCP:** TypeScript on Node — matches the team's existing MCP servers and tooling, mature MCP SDK, easy distribution (`npx gameiq`). Performance-critical parsing can drop to a native module later if profiling demands it.
- **Extractors in UE:** a small **all-C++** editor plugin exposing three commandlets (`GameIQExport`/`GameIQAssets`/`GameIQBlueprints`); ships as its own plugin (not bundled with VibeUE, though it shares the bridge patterns). Module deps are deliberately minimal (`AssetRegistry`, `Json`, `EnhancedInput`, `DeveloperSettings`, and editor modules for Tier 2). *(An earlier Python Tier 1 extractor was removed — the C++ `GameIQAssets` superseded it. Python remains a candidate for the future live in-editor bridge and user-pluggable recipes, not for batch extraction.)*
- **Store:** SQLite (better-sqlite3) + FTS5; sqlite-vec for vectors. Turso/libSQL enters only for the future team-sync tier — same stack already used for vibeue usage data.
- **Licensing/business (placeholder):** free core for indie/solo, paid team tier (sync, CI, dashboard). Decide before public beta.

### 10.1 Repository & distribution layout

**One monorepo, two distributable artifacts.** The decisive reason for a monorepo over split repos: the plugin *produces* the extraction output and the core *consumes* it — every Tier 2 recipe changes both sides, so they share a schema (`packages/shared`) and must version in lockstep. One atomic commit, no cross-repo skew.

```
unreal-game-iq/
  packages/
    core/        # TS: CLI, MCP server, query engine, SQLite  → publishes to npm
    shared/      # the entity/edge/export schema both sides import
  plugin/
    GameIQ/      # contents == <Game>/Plugins/GameIQ/ ; .uplugin + Source/ (all C++)
  scripts/       # deploy.ps1 — copy plugin → build → extract → index for a target project
  design.md
```

- **The core lives nowhere inside the game** — it's project-agnostic, installed once, indexes into each project's `.gameiq/`.
- **Only `plugin/GameIQ/` maps into a game**, at `<Game>/Plugins/GameIQ/`. It stays a clean plugin folder (nothing but `.uplugin`, `Source/`, `Content/`, `Resources/`) so no Node files leak into a packaged game.
- **Delivery: CLI-managed.** `gameiq init` detects the `.uproject`, copies/symlinks `plugin/GameIQ` into `Plugins/`, registers it, and gitignores it by default — mirroring the `.gameiq/` index convention (§5.2: gitignored by default, optionally committed for hermetic team builds). Submodule / marketplace zip are fallbacks.

## 11. Prior art & building blocks

Techniques and libraries worth studying or reusing:

- **[UAssetAPI](https://github.com/atenfyr/UAssetAPI)/UAssetGUI** (MIT, .NET) — reads/writes uncooked `.uasset` incl. raw Kismet (Blueprint) bytecode, JSON serialization. **Proves editor-less Blueprint reading is feasible.** Caveat: it sees compiled bytecode, not the node graph — pseudocode from bytecode is decompilation (harder, lossier). Likely conclusion: the editor/commandlet path renders full-fidelity graphs from `FEdGraphUtilities::ExportNodesToText` (the actual node graph as text, not bytecode — §5.1), relegating UAssetAPI-style bytecode parsing to the no-editor fallback tier only.
- **[CUE4Parse](https://github.com/FabianFG/CUE4Parse)** (Apache-2.0, FModel team) — battle-tested C# parser for *cooked* packages; relevant later for packaged-build size/stats analysis, plus `kismet-analyzer` for bytecode analysis.
- **[BPMerge](https://forums.unrealengine.com/t/open-source-bpmerge-deterministic-3-way-merge-for-unreal-engine-blueprints/2722265)** (open source) — extracts a **semantic IR from `.uasset`** for 3-way Blueprint merge, writes back via editor Python. Direct validation of our snapshot/semantic-diff design (§5.2); study its IR before inventing ours.
- **Asset diff ecosystem** — Epic's own [asset text-export diffing](https://www.unrealengine.com/en-US/blog/diffing-unreal-assets) and the in-editor Blueprint diff tool, plus community tools ([uasset-diff-tool](https://github.com/theqoqqi/uasset-diff-tool), BPDiffer). Confirms the pain around binary diffs is real and unsolved outside the editor.
- **tree-sitter (C++ grammar)** — proven approach for fast, robust parsing of UE's reflection macros without a full compiler; the right base for our C++ extractor.
- **Generic code-AI indexers** (Cursor, Sourcegraph, Greptile) — index text only; Blueprints are invisible to all of them. They set UX expectations ("my agent knows my codebase") that UE projects can't meet today — that expectation gap is our market.

**Net read:** nobody ships a **persistent, editor-optional, agent-agnostic project intelligence layer** for UE as a standalone product. The specific shape (open MCP + local-first + history) is unclaimed — and the window won't stay open forever. Ship the MVP fast.

## 12. Open questions

1. **Pseudocode fidelity:** how lossy can the Blueprint rendering be before agents draw wrong conclusions? Needs an eval set (reuse the skill-eval harness approach from the UES repo).
2. **Index time/size budget:** target = full index of a 50 GB project in minutes, incremental updates in seconds. Validate on a real mid-size project early.
3. **One multi-action MCP tool vs. several:** follow the UES v2 consolidation? (Leaning yes.)
4. **Commit the index?** Shared `.gameiq/` cache in VCS vs. everyone indexing locally. Probably: local by default, exportable bundle for teams.
5. **Trademark check:** clear "Game IQ" before launch.
6. **UEFN/Verse:** out of scope for now, but the entity/edge model shouldn't preclude it.
7. **Level extraction at scale:** a 10k-actor open-world map could explode index size if every actor becomes an entity with full overrides. Need an aggregation strategy (per-class counts + only actors with meaningful overrides?) and a real-world benchmark.
8. **Recipe priority order beyond MVP:** which Tier 1/2 recipes matter most after the initial set — Niagara? MetaSounds? PCG graphs? Decide from beta-user query logs rather than guessing.
9. **Design-doc ingestion (§5.4):** Phase 1 just makes doc sections retrievable and lets the agent connect intent to implementation — the open question is whether agent-side connecting is *good enough* in practice, or whether the explicit doc→code edges of Phase 2 are needed sooner than expected. The hard Phase 2 problem is *how* to link a prose section to the right entities (heading/naming heuristics vs. LLM-assisted matching). Which connectors first (local markdown + Drive likely)?
10. **Plugin delivery — source vs. precompiled (§10.1):** a source plugin needs the game to be a C++ project and compiles per engine version (5.7 vs 5.8…); precompiled binaries avoid that but mean maintaining a per-version build matrix (and a Fab/marketplace listing). Decide before beta — it gates BP-only projects.
11. **EDA / Toolset Registry investment (§6.5):** how much to build against an Experimental, `NoRedist` 5.8 API while it churns — ship the thin adapter early for the dock-context win, or wait until it stabilizes?
12. **World Partition level actors:** the Level recipe reads `PersistentLevel->Actors`, which is correct for non-WP maps but **misses actors in World Partition maps** (they live as One-File-Per-Actor `.uasset`s under `__ExternalActors__/`, streamed/unloaded). A real WP game's main level would extract with ~no placed actors. Fix needs either loading the WP grid or reading external-actor asset data. (Surfaced on ThirdPerson58, whose `Lvl_ThirdPerson` map was deleted but left 68 orphaned external actors — itself a hygiene signal Game IQ can flag.)

## 13. Trademark note

All public materials follow the existing convention: Unreal® is a trademark of Epic Games, Inc.; Unreal Game IQ is an independent Buckley Builds LLC product, not affiliated with or endorsed by Epic Games. Keep marketing version-agnostic ("Unreal Engine", not "UE5.x").

## 14. Implementation status (v0.1)

Snapshot of shipped vs. planned so the vision above doesn't read as done. Verified end-to-end on **ThirdPerson58 (UE 5.8)**: extract → index → MCP → agent.

**Shipped**
- Monorepo: `packages/shared` (schema), `packages/core` (SQLite+FTS5 store, producer-scoped ingest, query engine, MCP server, CLI), `plugin/GameIQ` (all-C++ editor plugin), `scripts/deploy.ps1`.
- **Editor-less:** C++ header reflection parse, config/`.ini`/`.uproject` parse, directory-exclude config (§5.5).
- **Commandlets:** `GameIQExport` (Tier 0 registry graph), `GameIQAssets` (Tier 1: static/skeletal mesh, texture, skeleton, data table, material, **Input Mapping Context/Action**, **level actor inventory with light/mesh detail**, generic reflection fallback for the rest), `GameIQBlueprints` (Tier 2 pseudocode + variables/components/interfaces).
- **Edges:** `depends-on`, `references`, `inherits`, `implements`, `calls`, `uses-material`, `uses-texture`, `uses-skeleton`, `placed-in-level`.
- **MCP:** one `game_iq` multi-action tool (§6.1), verified over stdio; `claude mcp add` registration.
- Cross-language robustness: BOM-tolerant JSON reader, UTF-8 output, bounded `get_entity`.

**Not yet built** — editor-less Tier 0 (registry-file parsing, still commandlet-only); in-editor live bridge + filesystem watcher (§8); snapshots / `changes` semantic diff; embeddings (FTS5 only); `ask` synthesis; design-doc ingestion (§5.4); Tier 2 for materials/AnimBP/Behavior Trees; bespoke recipes for Niagara/Sound/Physics (generic fallback today); World Partition actors (§12.12); Mac/Linux; team sync.

**Known gaps** — World Partition level actors (§12.12); base-material texture refs only via Tier 0 `depends-on` (headless `GetUsedTextures` returns empty; MI overrides are typed); level per-actor entities capped at 500 (full class counts always reported, §12.7).
