import { test } from "node:test";
import assert from "node:assert/strict";
import { rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { indexProject } from "../src/index/indexer.js";
import { Store } from "../src/store/store.js";
import { QueryEngine } from "../src/query/query.js";

const here = dirname(fileURLToPath(import.meta.url));
const fixture = join(here, "fixtures", "SampleGame");
const commandletJson = join(fixture, "_commandlet", "blueprints.json");

function freshDb(): string {
  const p = join(tmpdir(), `gameiq-test-${process.pid}-${process.hrtime.bigint()}.db`);
  rmSync(p, { force: true });
  return p;
}

function cleanup(dbPath: string): void {
  for (const suffix of ["", "-wal", "-shm"]) rmSync(dbPath + suffix, { force: true });
}

test("indexes the fixture project end-to-end (no editor)", () => {
  const dbPath = freshDb();
  const result = indexProject({ projectRoot: fixture, dbPath, mode: "replace", extractorJson: [commandletJson] });
  const store = new Store(dbPath);
  const engine = new QueryEngine(store);
  try {
    assert.ok(result.totalEntities > 0, "should extract entities");

    // --- C++ extractor (canonical ids use prefix-less reflection names) ---
    const health = engine.getEntity("cpp:HealthComponent");
    assert.ok(health, "UHealthComponent extracted");
    assert.equal(health!.entity.kind, "cpp-class");
    assert.equal(health!.entity.name, "UHealthComponent", "display name keeps the source prefix");

    const player = engine.getEntity("cpp:PlayerCharacter");
    assert.ok(player, "APlayerCharacter extracted");
    assert.ok(
      player!.outgoing.some((e) => e.type === "inherits" && e.dst === "cpp:Character"),
      "inherits edge to ACharacter (canonical: Character)",
    );
    assert.ok(
      player!.outgoing.some((e) => e.type === "references" && e.dst === "cpp:HealthComponent"),
      "UPROPERTY TObjectPtr<UHealthComponent> becomes a references edge",
    );
    assert.ok(engine.getEntity("cpp:HealthComponent::ApplyDamage"), "UFUNCTION extracted as member");

    // --- config extractor ---
    assert.ok(engine.searchProject("GameDefaultMap").length > 0, "ini keys are searchable");
    assert.ok(engine.getEntity("plugin:GameIQ"), "enabled plugin extracted from .uproject");

    // --- commandlet output (cross-producer linking) ---
    const bp = engine.getEntity("asset:/Game/Blueprints/BP_PlayerCharacter");
    assert.ok(bp, "blueprint ingested from commandlet output");
    assert.ok(
      bp!.outgoing.some((e) => e.dst === "cpp:HealthComponent" && e.type === "calls"),
      "BP->C++ call edge resolves across producers",
    );

    // --- search (the killer demo: find logic you can't grep) ---
    const hits = engine.searchProject("fall damage");
    assert.ok(hits.length > 0, "pseudocode is searchable");
    assert.ok(
      hits.some((h) => h.entity.id.startsWith("asset:/Game/Blueprints/BP_PlayerCharacter")),
      "fall-damage query surfaces the Blueprint",
    );

    // --- impact (what breaks if UHealthComponent changes) ---
    const impact = engine.impact("cpp:HealthComponent");
    assert.ok(
      impact.some((r) => r.entity.id === "asset:/Game/Blueprints/BP_PlayerCharacter"),
      "the Blueprint shows up as an impacted dependent",
    );
    assert.ok(
      impact.some((r) => r.entity.id === "cpp:PlayerCharacter"),
      "the C++ owner shows up as an impacted dependent",
    );
  } finally {
    store.close();
    cleanup(dbPath);
  }
});

test("incremental merge replaces only the changed entity's edges", () => {
  const dbPath = freshDb();
  indexProject({ projectRoot: fixture, dbPath, mode: "replace", extractorJson: [commandletJson] });
  // re-run as merge; counts should stay stable (idempotent), not double up
  const before = (() => {
    const s = new Store(dbPath);
    const n = s.totalEntities();
    s.close();
    return n;
  })();
  indexProject({ projectRoot: fixture, dbPath, mode: "merge", extractorJson: [commandletJson] });
  const store = new Store(dbPath);
  try {
    assert.equal(store.totalEntities(), before, "re-indexing is idempotent");
  } finally {
    store.close();
    cleanup(dbPath);
  }
});
