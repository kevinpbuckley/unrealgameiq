import { readFileSync } from "node:fs";
import { relative } from "node:path";
import {
  configSectionId,
  emptyOutput,
  pluginId,
  type Chunk,
  type Edge,
  type Entity,
  type ExtractorOutput,
} from "@gameiq/shared";
import { walkFiles } from "../util/walk.js";

/**
 * Editor-less config/project extractor (design §5.1): `.ini` sections,
 * `.uproject`/`.uplugin` enabled plugins. Small, but answers a disproportionate
 * number of real questions. Runs with no editor.
 */

export const CONFIG_PRODUCER = "gameiq-config@0.1.0";

interface IniSection {
  name: string;
  keys: Array<{ key: string; value: string }>;
}

function parseIni(text: string): IniSection[] {
  const sections: IniSection[] = [];
  let current: IniSection | null = null;
  for (const raw of text.split(/\r?\n/)) {
    const line = raw.trim();
    if (line.length === 0 || line.startsWith(";") || line.startsWith("#")) continue;
    const header = line.match(/^\[(.+)\]$/);
    if (header) {
      current = { name: header[1]!, keys: [] };
      sections.push(current);
      continue;
    }
    const eq = line.indexOf("=");
    if (eq > 0 && current) {
      current.keys.push({ key: line.slice(0, eq).trim(), value: line.slice(eq + 1).trim() });
    }
  }
  return sections;
}

export function extractConfig(
  projectRoot: string,
  generatedAtIso: string,
  projectName: string,
  excludes: string[] = [],
): ExtractorOutput {
  const out = emptyOutput(CONFIG_PRODUCER, { name: projectName, root: projectRoot }, generatedAtIso);
  const entities: Entity[] = [];
  const edges: Edge[] = [];
  const chunks: Chunk[] = [];

  // --- .ini sections ---
  for (const file of walkFiles(projectRoot, [".ini"], excludes)) {
    let text: string;
    try {
      text = readFileSync(file, "utf8");
    } catch {
      continue;
    }
    const rel = relative(projectRoot, file).replaceAll("\\", "/");
    for (const section of parseIni(text)) {
      const id = configSectionId(rel, section.name);
      const body = section.keys.map((k) => `${k.key}=${k.value}`).join("\n");
      entities.push({
        id,
        kind: "config-section",
        name: section.name,
        path: rel,
        source: "config",
        summary: `[${section.name}] in ${rel} (${section.keys.length} keys)`,
        detail: { file: rel, keys: section.keys },
      });
      chunks.push({
        id: `${id}#block`,
        entityId: id,
        kind: "config-block",
        text: `[${section.name}] (${rel})\n${body}`,
      });
    }
  }

  // --- .uproject / .uplugin enabled plugins ---
  for (const file of walkFiles(projectRoot, [".uproject", ".uplugin"], excludes)) {
    let json: { Plugins?: Array<{ Name?: string; Enabled?: boolean }>; Modules?: Array<{ Name?: string }> };
    const rel = relative(projectRoot, file).replaceAll("\\", "/");
    try {
      json = JSON.parse(readFileSync(file, "utf8"));
    } catch {
      continue;
    }
    for (const p of json.Plugins ?? []) {
      if (!p.Name) continue;
      const id = pluginId(p.Name);
      entities.push({
        id,
        kind: "plugin",
        name: p.Name,
        path: rel,
        source: "config",
        summary: `Plugin ${p.Name} (${p.Enabled === false ? "disabled" : "enabled"}) — declared in ${rel}`,
        detail: { enabled: p.Enabled !== false, declaredIn: rel },
      });
      chunks.push({
        id: `${id}#decl`,
        entityId: id,
        kind: "config-block",
        text: `Plugin ${p.Name} enabled=${p.Enabled !== false} in ${rel}`,
      });
    }
  }

  out.entities = entities;
  out.edges = edges;
  out.chunks = chunks;
  return out;
}
