import { existsSync, readdirSync } from "node:fs";
import { basename, join } from "node:path";

export interface ProjectInfo {
  name: string;
  root: string;
  uprojectPath?: string;
}

/** Find the first `*.uproject` directly under `root`. */
export function findUproject(root: string): string | undefined {
  let names: string[];
  try {
    names = readdirSync(root);
  } catch {
    return undefined;
  }
  const file = names.find((n) => n.toLowerCase().endsWith(".uproject"));
  return file ? join(root, file) : undefined;
}

export function projectInfo(root: string): ProjectInfo {
  const uprojectPath = findUproject(root);
  const name = uprojectPath ? basename(uprojectPath).replace(/\.uproject$/i, "") : basename(root);
  return { name, root, uprojectPath };
}

/** Canonical locations for Game IQ's per-project artifacts (design §5.2). */
export function gameiqPaths(root: string): {
  dir: string;
  dbPath: string;
  extractDir: string;
  incrementalDir: string;
} {
  const dir = join(root, ".gameiq");
  const extractDir = join(dir, "extract");
  return { dir, dbPath: join(dir, "index.db"), extractDir, incrementalDir: join(extractDir, "incremental") };
}

/** List extractor JSON files emitted by the UE commandlet / bridge under `.gameiq/extract/`. */
export function findCommandletOutputs(root: string): string[] {
  const { extractDir } = gameiqPaths(root);
  if (!existsSync(extractDir)) return [];
  try {
    return readdirSync(extractDir)
      .filter((n) => n.toLowerCase().endsWith(".json"))
      .map((n) => join(extractDir, n));
  } catch {
    return [];
  }
}
