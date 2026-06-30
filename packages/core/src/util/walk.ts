import { readdirSync, type Dirent } from "node:fs";
import { join } from "node:path";

const DEFAULT_SKIP = ["Intermediate", "Binaries", "Saved", "DerivedDataCache", ".git", "node_modules"];

/** Recursively list files under `root` matching one of `exts` (lowercase, with dot), skipping build dirs. */
export function walkFiles(root: string, exts: string[], skipDirs: string[] = DEFAULT_SKIP): string[] {
  const skip = new Set(skipDirs);
  const out: string[] = [];

  const visit = (dir: string): void => {
    let entries: Dirent[];
    try {
      entries = readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const d of entries) {
      if (d.isDirectory()) {
        if (!skip.has(d.name)) visit(join(dir, d.name));
        continue;
      }
      if (!d.isFile()) continue;
      const dot = d.name.lastIndexOf(".");
      if (dot < 0) continue;
      if (exts.includes(d.name.slice(dot).toLowerCase())) out.push(join(dir, d.name));
    }
  };

  visit(root);
  return out;
}
