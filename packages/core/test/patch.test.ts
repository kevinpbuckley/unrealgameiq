import { test } from "node:test";
import assert from "node:assert/strict";
import { rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { SCHEMA_VERSION, type ExtractorOutput } from "@gameiq/shared";
import { indexProject } from "../src/index/indexer.js";
import { patchIngest } from "../src/ingest/ingest.js";
import { Store } from "../src/store/store.js";
import { QueryEngine } from "../src/query/query.js";

const here = dirname(fileURLToPath(import.meta.url));
const fixture = join(here, "fixtures", "SampleGame");
const commandletJson = join(fixture, "_commandlet", "blueprints.json");

test("patchIngest replaces one asset's subtree and preserves inbound edges", () => {
  const dbPath = join(tmpdir(), `gameiq-patch-${process.pid}-${process.hrtime.bigint()}.db`);
  rmSync(dbPath, { force: true });
  indexProject({ projectRoot: fixture, dbPath, mode: "replace", extractorJson: [commandletJson] });

  const store = new Store(dbPath);
  const engine = new QueryEngine(store);
  try {
    // baseline: HealthComponent has a member ApplyDamage and inbound references
    assert.ok(engine.getEntity("cpp:HealthComponent::ApplyDamage"), "baseline member present");
    const inboundBefore = store.incomingEdges("cpp:HealthComponent");
    assert.ok(
      inboundBefore.some((e) => e.src === "cpp:PlayerCharacter"),
      "PlayerCharacter references HealthComponent (inbound edge)",
    );
    const playerBefore = engine.getEntity("cpp:PlayerCharacter");

    // a save-hook style delta: HealthComponent changed — ApplyDamage removed, NewFunc added
    const delta: ExtractorOutput = {
      schemaVersion: SCHEMA_VERSION,
      generatedAtIso: "2026-07-01T00:00:00.000Z",
      producer: "gameiq-bridge@0.1.0",
      project: { name: "SampleGame", root: "." },
      replaces: ["cpp:HealthComponent"],
      entities: [
        {
          id: "cpp:HealthComponent",
          kind: "cpp-class",
          name: "UHealthComponent",
          path: "Source/SampleGame/HealthComponent.h",
          source: "code",
          summary: "PATCHED",
        },
        {
          id: "cpp:HealthComponent::NewFunc",
          kind: "cpp-function",
          name: "NewFunc",
          path: "Source/SampleGame/HealthComponent.h",
          parent: "cpp:HealthComponent",
          source: "code",
        },
      ],
      edges: [],
      chunks: [],
    };
    patchIngest(store, delta);

    // the replaced subtree changed
    assert.equal(engine.getEntity("cpp:HealthComponent")!.entity.summary, "PATCHED", "entity updated");
    assert.equal(
      engine.getEntity("cpp:HealthComponent::ApplyDamage"),
      undefined,
      "old child removed with the subtree",
    );
    assert.ok(engine.getEntity("cpp:HealthComponent::NewFunc"), "new child inserted");

    // inbound edges preserved (we didn't own them)
    assert.ok(
      store.incomingEdges("cpp:HealthComponent").some((e) => e.src === "cpp:PlayerCharacter"),
      "inbound reference from PlayerCharacter survived the patch",
    );

    // everything else untouched
    const playerAfter = engine.getEntity("cpp:PlayerCharacter");
    assert.deepEqual(playerAfter!.entity, playerBefore!.entity, "unrelated entity unchanged");
    assert.ok(engine.getEntity("asset:/Game/Blueprints/BP_PlayerCharacter"), "blueprint untouched");
  } finally {
    store.close();
    for (const s of ["", "-wal", "-shm"]) rmSync(dbPath + s, { force: true });
  }
});
