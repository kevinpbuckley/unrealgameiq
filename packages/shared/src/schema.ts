import { z } from "zod";

/**
 * The cross-artifact data contract for Unreal Game IQ.
 *
 * Both the TypeScript editor-less extractors (C++, config) and the UE plugin's
 * `GameIQExport` commandlet / in-editor bridge emit an {@link ExtractorOutput}.
 * The core ingests it into SQLite. Because the producer and consumer live on
 * opposite sides of a process boundary (and opposite languages), this schema is
 * validated at runtime on ingest — never trust the producer blindly.
 *
 * Bump {@link SCHEMA_VERSION} on any breaking change and migrate in the store.
 */
export const SCHEMA_VERSION = 1;

/** Provenance of a fact: `code`/`asset`/`config` are ground truth; `doc` is *intent* (design §5.4). */
export const SourceSchema = z.enum(["code", "asset", "config", "doc"]);
export type Source = z.infer<typeof SourceSchema>;

/** Structural kind of an entity. The asset's UClass (StaticMesh, Material…) lives in `detail.assetClass`. */
export const EntityKindSchema = z.enum([
  "asset",
  "blueprint",
  "bp-function",
  "bp-variable",
  "bp-component",
  "material-parameter",
  "socket",
  "level-actor",
  "cpp-class",
  "cpp-struct",
  "cpp-function",
  "cpp-property",
  "cpp-enum",
  "cpp-delegate",
  "config-section",
  "plugin",
  "doc-section",
]);
export type EntityKind = z.infer<typeof EntityKindSchema>;

/** Typed relations. Generic `references`/`depends-on` from the registry; the rest are semantic (design §5.2/§5.4). */
export const EdgeTypeSchema = z.enum([
  "references",
  "depends-on",
  "inherits",
  "calls",
  "implements",
  "uses-material",
  "uses-skeleton",
  "overrides-parameter",
  "placed-in-level",
  "plays-on",
  "casts-to",
  "describes",
  "constrains",
]);
export type EdgeType = z.infer<typeof EdgeTypeSchema>;

/** Retrieval text units (design §5.2 `chunks`). */
export const ChunkKindSchema = z.enum([
  "bp-pseudocode",
  "recipe-summary",
  "cpp-signature",
  "config-block",
  "doc-section",
]);
export type ChunkKind = z.infer<typeof ChunkKindSchema>;

export const EntitySchema = z.object({
  /** Stable, deterministic id — see ids.ts (e.g. `asset:/Game/BP_Player`, `cpp:UHealthComponent::ApplyDamage`). */
  id: z.string().min(1),
  kind: EntityKindSchema,
  name: z.string(),
  /** Package path (`/Game/...`) for assets, file path for code, doc path for docs. */
  path: z.string(),
  /** Parent entity id (e.g. a bp-function's owning blueprint), if any. */
  parent: z.string().nullish(),
  source: SourceSchema,
  /** Short human/agent-readable summary. */
  summary: z.string().nullish(),
  /** Recipe output: the raw structured detail for this entity. */
  detail: z.record(z.unknown()).nullish(),
});
export type Entity = z.infer<typeof EntitySchema>;

export const EdgeSchema = z.object({
  src: z.string().min(1),
  dst: z.string().min(1),
  type: EdgeTypeSchema,
  attrs: z.record(z.unknown()).nullish(),
});
export type Edge = z.infer<typeof EdgeSchema>;

export const ChunkSchema = z.object({
  id: z.string().min(1),
  entityId: z.string().min(1),
  kind: ChunkKindSchema,
  text: z.string(),
});
export type Chunk = z.infer<typeof ChunkSchema>;

export const ExtractorOutputSchema = z.object({
  schemaVersion: z.number().int(),
  /** ISO-8601 timestamp, stamped by the producer. */
  generatedAtIso: z.string(),
  /** Producer id for provenance/debugging, e.g. `gameiq-cpp@0.1.0`. */
  producer: z.string(),
  project: z.object({ name: z.string(), root: z.string() }),
  entities: z.array(EntitySchema),
  edges: z.array(EdgeSchema),
  chunks: z.array(ChunkSchema),
});
export type ExtractorOutput = z.infer<typeof ExtractorOutputSchema>;

/** Parse + validate an extractor output blob (throws ZodError on mismatch). */
export function parseExtractorOutput(data: unknown): ExtractorOutput {
  return ExtractorOutputSchema.parse(data);
}

/** Build an empty output for a producer to fill in. */
export function emptyOutput(
  producer: string,
  project: { name: string; root: string },
  generatedAtIso: string,
): ExtractorOutput {
  return {
    schemaVersion: SCHEMA_VERSION,
    generatedAtIso,
    producer,
    project,
    entities: [],
    edges: [],
    chunks: [],
  };
}
