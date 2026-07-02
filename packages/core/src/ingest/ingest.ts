import { SCHEMA_VERSION, type ExtractorOutput } from "@gameiq/shared";
import type { Store } from "../store/store.js";

export type IngestMode =
  | "replace" // full rebuild: wipe everything, then insert (gameiq index)
  | "merge"; // incremental: replace only the entities in this output (gameiq index --changed)

export interface IngestSummary {
  entities: number;
  edges: number;
  chunks: number;
  mode: IngestMode;
}

/**
 * Ingest one extractor output into the store (design §5.2). Multiple producers
 * (C++ extractor, config extractor, UE commandlet) each emit an output; ingest
 * them in turn with mode "merge" to build a combined index, or "replace" for the
 * first/full producer of a clean rebuild.
 */
export function ingest(store: Store, output: ExtractorOutput, mode: IngestMode): IngestSummary {
  if (output.schemaVersion !== SCHEMA_VERSION) {
    throw new Error(
      `extractor output schemaVersion ${output.schemaVersion} != core SCHEMA_VERSION ${SCHEMA_VERSION} ` +
        `(producer: ${output.producer}). Rebuild the producer or migrate.`,
    );
  }

  store.transaction(() => {
    if (mode === "replace") {
      store.clearAll();
    } else {
      // producer-scoped replace: drop only THIS producer's prior rows, then re-insert.
      // Keeps other producers' contributions (e.g. the registry owns a blueprint entity
      // while the blueprints producer adds a structure chunk to it) intact.
      store.deleteByProducer(output.producer);
    }

    for (const e of output.entities) store.upsertEntity(e, output.producer);
    for (const edge of output.edges) store.insertEdge(edge, output.producer);
    for (const c of output.chunks) store.insertChunk(c, output.producer);

    store.setMeta("projectName", output.project.name);
    store.setMeta("projectRoot", output.project.root);
    store.setMeta("lastProducer", output.producer);
    store.setMeta("lastGeneratedAtIso", output.generatedAtIso);
    store.setMeta("lastIngestAtIso", new Date().toISOString());
  });

  return {
    entities: output.entities.length,
    edges: output.edges.length,
    chunks: output.chunks.length,
    mode,
  };
}

/**
 * Incremental ingest (design §8): apply a small delta for a few changed assets without
 * touching the rest of the index. For each root in `output.replaces` (or every top-level
 * entity if absent), the root's subtree + its owned edges/chunks are deleted, then the
 * delta is inserted. Inbound edges are preserved. This is what the in-editor save hook
 * feeds so the index tracks saves in real time.
 */
export function patchIngest(store: Store, output: ExtractorOutput): IngestSummary {
  if (output.schemaVersion !== SCHEMA_VERSION) {
    throw new Error(
      `delta schemaVersion ${output.schemaVersion} != core SCHEMA_VERSION ${SCHEMA_VERSION} (producer: ${output.producer})`,
    );
  }
  const roots =
    output.replaces && output.replaces.length > 0
      ? output.replaces
      : output.entities.filter((e) => !e.parent).map((e) => e.id);

  store.transaction(() => {
    for (const root of roots) store.deleteSubtree(root);
    for (const e of output.entities) store.upsertEntity(e, output.producer);
    for (const edge of output.edges) store.insertEdge(edge, output.producer);
    for (const c of output.chunks) store.insertChunk(c, output.producer);
    store.setMeta("lastIngestAtIso", new Date().toISOString());
  });

  return {
    entities: output.entities.length,
    edges: output.edges.length,
    chunks: output.chunks.length,
    mode: "merge",
  };
}
