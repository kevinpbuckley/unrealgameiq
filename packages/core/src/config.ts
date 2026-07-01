import { existsSync, readFileSync } from "node:fs";
import { join } from "node:path";

/**
 * Per-project config, read from `<projectRoot>/gameiq.config.json` (committed, user-editable).
 * Keep it small and forgiving — a malformed file degrades to defaults rather than failing a run.
 */
export interface GameIqConfig {
  /**
   * Directories to keep out of the editor-less index. Each entry matches a directory *name*
   * (`VibeUE`) or a project-relative *path* (`Plugins/VibeUE`). Use this to exclude third-party
   * plugin source/content so the index reflects *your* game, not vendored plugins.
   */
  exclude: string[];
}

export const CONFIG_FILENAME = "gameiq.config.json";

/** Game IQ's own plugin is never worth indexing into a project — always excluded. */
export const ALWAYS_EXCLUDE = ["Plugins/GameIQ"];

export function loadConfig(projectRoot: string): GameIqConfig {
  const path = join(projectRoot, CONFIG_FILENAME);
  if (!existsSync(path)) return { exclude: [] };
  try {
    const raw = JSON.parse(readFileSync(path, "utf8")) as { exclude?: unknown };
    const exclude = Array.isArray(raw.exclude)
      ? raw.exclude.filter((x): x is string => typeof x === "string")
      : [];
    return { exclude };
  } catch {
    return { exclude: [] };
  }
}

/** The effective exclude list for a project: config excludes + always-excluded + any overrides. */
export function effectiveExcludes(projectRoot: string, overrides: string[] = []): string[] {
  return [...new Set([...loadConfig(projectRoot).exclude, ...ALWAYS_EXCLUDE, ...overrides])];
}
