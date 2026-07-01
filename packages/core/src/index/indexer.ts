import { parseExtractorOutput, type ExtractorOutput } from "@gameiq/shared";
import { ingest, type IngestSummary } from "../ingest/ingest.js";
import { extractCpp } from "../extract/cpp.js";
import { extractConfig } from "../extract/config.js";
import { findCommandletOutputs, gameiqPaths, projectInfo } from "../extract/project.js";
import { Store } from "../store/store.js";
import { readJsonFile } from "../util/readJson.js";

export interface IndexOptions {
  projectRoot: string;
  /** Override the DB path (defaults to `<root>/.gameiq/index.db`). */
  dbPath?: string;
  /** "replace" wipes and rebuilds; "merge" updates in place (incremental). */
  mode?: "replace" | "merge";
  /** Extra extractor JSON files to ingest (e.g. from a UE commandlet run). */
  extractorJson?: string[];
}

export interface IndexResult {
  projectName: string;
  dbPath: string;
  producers: Array<{ producer: string; summary: IngestSummary }>;
  totalEntities: number;
}

/**
 * Build/refresh the index for a project (design §6.2 `gameiq index`).
 * Runs the editor-less extractors (C++, config), then ingests any commandlet
 * outputs found under `.gameiq/extract/` plus any explicitly provided JSON.
 */
export function indexProject(opts: IndexOptions): IndexResult {
  const info = projectInfo(opts.projectRoot);
  const { dbPath: defaultDb } = gameiqPaths(opts.projectRoot);
  const dbPath = opts.dbPath ?? defaultDb;
  const generatedAtIso = new Date().toISOString();

  const outputs: ExtractorOutput[] = [
    extractCpp(opts.projectRoot, generatedAtIso, info.name),
    extractConfig(opts.projectRoot, generatedAtIso, info.name),
  ];

  const jsonPaths = [...findCommandletOutputs(opts.projectRoot), ...(opts.extractorJson ?? [])];
  for (const p of jsonPaths) {
    outputs.push(parseExtractorOutput(readJsonFile(p)));
  }

  const store = new Store(dbPath);
  const producers: Array<{ producer: string; summary: IngestSummary }> = [];
  try {
    outputs.forEach((output, i) => {
      // First producer of a full rebuild wipes; everything else merges.
      const mode = opts.mode === "merge" ? "merge" : i === 0 ? "replace" : "merge";
      producers.push({ producer: output.producer, summary: ingest(store, output, mode) });
    });
    const totalEntities = store.totalEntities();
    return { projectName: info.name, dbPath, producers, totalEntities };
  } finally {
    store.close();
  }
}
