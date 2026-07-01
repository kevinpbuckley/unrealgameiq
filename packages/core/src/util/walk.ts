import { readdirSync, type Dirent } from "node:fs";
import { join, relative } from "node:path";

const DEFAULT_SKIP = ["Intermediate", "Binaries", "Saved", "DerivedDataCache", ".git", "node_modules"];

/**
 * Recursively list files under `root` matching one of `exts` (lowercase, with dot).
 *
 * Always skips build dirs (Intermediate/Binaries/…). `excludes` are user-supplied and
 * match either a directory *name* (`VibeUE`) or a project-relative *path* (`Plugins/VibeUE`) —
 * so a project can keep third-party plugin source out of the index (see loadConfig).
 */
export function walkFiles(root: string, exts: string[], excludes: string[] = []): string[] {
  const skipNames = new Set(DEFAULT_SKIP);
  const excl = excludes.map((e) => e.replaceAll("\\", "/").replace(/\/+$/, "")).filter((e) => e.length > 0);
  const out: string[] = [];

  const isExcluded = (name: string, relPath: string): boolean =>
    excl.some((e) => name === e || relPath === e || relPath.startsWith(e + "/"));

  const visit = (dir: string): void => {
    let entries: Dirent[];
    try {
      entries = readdirSync(dir, { withFileTypes: true });
    } catch {
      return;
    }
    for (const d of entries) {
      if (d.isDirectory()) {
        if (skipNames.has(d.name)) continue;
        const rel = relative(root, join(dir, d.name)).replaceAll("\\", "/");
        if (isExcluded(d.name, rel)) continue;
        visit(join(dir, d.name));
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
