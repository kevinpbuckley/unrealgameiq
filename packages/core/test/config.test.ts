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

test("gameiq.config.json excludes directories from the editor-less index", () => {
  const dbPath = join(tmpdir(), `gameiq-config-${process.pid}-${process.hrtime.bigint()}.db`);
  rmSync(dbPath, { force: true });
  indexProject({ projectRoot: fixture, dbPath, mode: "replace" });

  const store = new Store(dbPath);
  const engine = new QueryEngine(store);
  try {
    // gameiq.config.json excludes Plugins/ThirdPartyPlugin
    assert.equal(
      engine.getEntity("cpp:ThirdPartyActor"),
      undefined,
      "C++ under an excluded plugin dir must not be indexed",
    );
    // a sibling plugin that is NOT excluded is still indexed
    assert.ok(engine.getEntity("cpp:IncludedActor"), "non-excluded plugin C++ is still indexed");
    // and the game's own Source is unaffected
    assert.ok(engine.getEntity("cpp:HealthComponent"), "game C++ is still indexed");
  } finally {
    store.close();
    for (const s of ["", "-wal", "-shm"]) rmSync(dbPath + s, { force: true });
  }
});

test("--exclude override also works", () => {
  const dbPath = join(tmpdir(), `gameiq-config2-${process.pid}-${process.hrtime.bigint()}.db`);
  rmSync(dbPath, { force: true });
  indexProject({ projectRoot: fixture, dbPath, mode: "replace", exclude: ["Plugins/IncludedPlugin"] });

  const store = new Store(dbPath);
  const engine = new QueryEngine(store);
  try {
    // now both plugins are excluded (config + override)
    assert.equal(engine.getEntity("cpp:ThirdPartyActor"), undefined);
    assert.equal(engine.getEntity("cpp:IncludedActor"), undefined, "override exclusion applies");
    assert.ok(engine.getEntity("cpp:HealthComponent"), "game C++ still indexed");
  } finally {
    store.close();
    for (const s of ["", "-wal", "-shm"]) rmSync(dbPath + s, { force: true });
  }
});
