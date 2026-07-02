import { readdirSync, rmSync } from "node:fs";
import { join } from "node:path";
import { parseExtractorOutput } from "@gameiq/shared";
import { patchIngest } from "../ingest/ingest.js";
import { gameiqPaths } from "../extract/project.js";
import type { Store } from "../store/store.js";
import { readJsonFile } from "../util/readJson.js";

export interface DrainResult {
  files: number;
  entities: number;
  edges: number;
  chunks: number;
}

/**
 * Apply every pending delta in `<project>/.gameiq/extract/incremental/` once, then return
 * (design §8). This is what the in-editor save hook spawns after a debounce: the plugin
 * extracts each saved asset in-memory, writes a delta JSON here, then fires one detached
 * `gameiq drain` — no long-running watcher. A delta that fails to parse (a producer
 * mid-write) is left for the next drain; one that parses but fails to apply is dropped.
 */
export function drainIncremental(
  projectRoot: string,
  store: Store,
  onPatch?: (info: { file: string; entities: number; edges: number; chunks: number }) => void,
): DrainResult {
  const { incrementalDir } = gameiqPaths(projectRoot);
  const res: DrainResult = { files: 0, entities: 0, edges: 0, chunks: 0 };
  let files: string[];
  try {
    files = readdirSync(incrementalDir)
      .filter((f) => f.toLowerCase().endsWith(".json"))
      .sort();
  } catch {
    return res;
  }
  for (const f of files) {
    const full = join(incrementalDir, f);
    let output;
    try {
      output = parseExtractorOutput(readJsonFile(full));
    } catch {
      continue; // partial write / not valid yet — leave for a later drain
    }
    try {
      const summary = patchIngest(store, output);
      rmSync(full, { force: true });
      res.files += 1;
      res.entities += summary.entities;
      res.edges += summary.edges;
      res.chunks += summary.chunks;
      onPatch?.({ file: f, entities: summary.entities, edges: summary.edges, chunks: summary.chunks });
    } catch (e) {
      rmSync(full, { force: true }); // drop a poison delta rather than fail every future drain
      process.stderr.write(`[gameiq] failed to apply ${f}: ${(e as Error).message}\n`);
    }
  }
  return res;
}
