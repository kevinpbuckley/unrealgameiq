import { readFileSync } from "node:fs";
import { relative } from "node:path";
import {
  cppClassId,
  cppMemberId,
  emptyOutput,
  type Chunk,
  type Edge,
  type Entity,
  type EntityKind,
  type ExtractorOutput,
} from "@gameiq/shared";
import { walkFiles } from "../util/walk.js";

/**
 * Editor-less C++ reflection-surface extractor (design §5.1). MVP is a
 * lightweight header parse — UE's reflection macros make headers regular enough
 * that regex + brace-matching gets UCLASS/USTRUCT/UENUM/UFUNCTION/UPROPERTY,
 * the class hierarchy, and property→type references. A clangd/tree-sitter
 * backend (for call graphs) is a later upgrade; this runs with no editor.
 */

export const CPP_PRODUCER = "gameiq-cpp@0.1.0";

const REFLECTED = /\b(UCLASS|USTRUCT|UINTERFACE)\s*\(/g;

/** Find the `{ ... }` block starting at/after `from`; returns [openIdx, closeIdx] or null. */
function findBlock(text: string, from: number): [number, number] | null {
  const open = text.indexOf("{", from);
  if (open < 0) return null;
  let depth = 0;
  for (let i = open; i < text.length; i++) {
    const ch = text[i];
    if (ch === "{") depth++;
    else if (ch === "}") {
      depth--;
      if (depth === 0) return [open, i];
    }
  }
  return null;
}

/** Reduce a C++ type expression to its inner UE type identifier (e.g. TObjectPtr<UFoo> → UFoo). */
function innerTypeName(type: string): string | null {
  const tmpl = type.match(/<\s*([A-Za-z_]\w*)/);
  const base = tmpl ? tmpl[1]! : type.replace(/[*&]/g, "");
  const id = base.trim().match(/[A-Za-z_]\w*$/);
  return id ? id[0] : null;
}

/**
 * Canonical reflection name: strip the UE source prefix (A/U/F/I/S/T/E followed by
 * an uppercase letter) so ids match what the Asset Registry / reflection system
 * uses (`APlayerCharacter` → `PlayerCharacter`). This is what makes BP→C++ edges
 * emitted by the UE commandlet resolve to entities found by this header parse.
 */
function reflectionName(ident: string): string {
  const m = ident.match(/^[AUFISTE]([A-Z]\w*)$/);
  return m ? m[1]! : ident;
}

interface ParsedClass {
  macro: string;
  name: string;
  base: string | null;
  bodyStart: number;
  bodyEnd: number;
  file: string;
  functions: string[];
  properties: Array<{ name: string; type: string }>;
}

function parseFile(text: string, file: string): ParsedClass[] {
  const classes: ParsedClass[] = [];
  REFLECTED.lastIndex = 0;
  let m: RegExpExecArray | null;
  while ((m = REFLECTED.exec(text))) {
    const macro = m[1]!;
    // class/struct decl follows the macro's closing paren
    const after = text.slice(m.index, m.index + 4000);
    const decl = after.match(
      /\)\s*(?:class|struct)\s+(?:[A-Z0-9_]+_API\s+)?([A-Za-z_]\w*)\b(?:\s*:\s*public\s+([A-Za-z_]\w*))?/,
    );
    if (!decl) continue;
    const declAbs = m.index + (decl.index ?? 0);
    const block = findBlock(text, declAbs);
    if (!block) continue;
    const body = text.slice(block[0], block[1]);

    const functions: string[] = [];
    const fnRe = /UFUNCTION\s*\([\s\S]*?\)\s*[\s\S]*?\b([A-Za-z_]\w*)\s*\(/g;
    let fm: RegExpExecArray | null;
    while ((fm = fnRe.exec(body))) functions.push(fm[1]!);

    const properties: Array<{ name: string; type: string }> = [];
    const propRe = /UPROPERTY\s*\([\s\S]*?\)\s*([A-Za-z_][\w:<>,\s\*&]*?)\b([A-Za-z_]\w*)\s*(?:;|=|\{)/g;
    let pm: RegExpExecArray | null;
    while ((pm = propRe.exec(body))) {
      const type = pm[1]!.trim();
      const name = pm[2]!;
      if (type.length === 0) continue;
      properties.push({ name, type });
    }

    classes.push({
      macro,
      name: decl[1]!,
      base: decl[2] ?? null,
      bodyStart: block[0],
      bodyEnd: block[1],
      file,
      functions,
      properties,
    });
  }
  return classes;
}

function kindForMacro(macro: string): EntityKind {
  if (macro === "USTRUCT") return "cpp-struct";
  return "cpp-class"; // UCLASS, UINTERFACE
}

export function extractCpp(
  projectRoot: string,
  generatedAtIso: string,
  projectName: string,
  excludes: string[] = [],
): ExtractorOutput {
  const out = emptyOutput(CPP_PRODUCER, { name: projectName, root: projectRoot }, generatedAtIso);
  const headers = walkFiles(projectRoot, [".h"], excludes);

  const allClasses: ParsedClass[] = [];
  for (const file of headers) {
    let text: string;
    try {
      text = readFileSync(file, "utf8");
    } catch {
      continue;
    }
    if (!text.includes("UCLASS") && !text.includes("USTRUCT") && !text.includes("UINTERFACE")) continue;
    allClasses.push(...parseFile(text, file));
  }

  // Canonical ids use the prefix-less reflection name; the prefixed source name is kept for display.
  const known = new Set(allClasses.map((c) => reflectionName(c.name)));
  const entities: Entity[] = [];
  const edges: Edge[] = [];
  const chunks: Chunk[] = [];

  for (const c of allClasses) {
    const rel = relative(projectRoot, c.file).replaceAll("\\", "/");
    const canonical = reflectionName(c.name);
    const id = cppClassId(canonical);
    entities.push({
      id,
      kind: kindForMacro(c.macro),
      name: c.name,
      path: rel,
      source: "code",
      summary: `${c.macro} ${c.name}${c.base ? ` : public ${c.base}` : ""}`,
      detail: {
        sourceName: c.name,
        base: c.base,
        functions: c.functions,
        properties: c.properties,
        file: rel,
      },
    });

    if (c.base) edges.push({ src: id, dst: cppClassId(reflectionName(c.base)), type: "inherits" });

    for (const fn of c.functions) {
      const fid = cppMemberId(canonical, fn);
      entities.push({
        id: fid,
        kind: "cpp-function",
        name: fn,
        path: rel,
        parent: id,
        source: "code",
        summary: `${c.name}::${fn}()`,
      });
      chunks.push({ id: `${fid}#sig`, entityId: fid, kind: "cpp-signature", text: `${c.name}::${fn}()` });
    }

    for (const p of c.properties) {
      const pid = cppMemberId(canonical, p.name);
      entities.push({
        id: pid,
        kind: "cpp-property",
        name: p.name,
        path: rel,
        parent: id,
        source: "code",
        summary: `${p.type} ${p.name}`,
        detail: { type: p.type },
      });
      const refRaw = innerTypeName(p.type);
      const ref = refRaw ? reflectionName(refRaw) : null;
      if (ref && ref !== canonical && known.has(ref)) {
        edges.push({ src: id, dst: cppClassId(ref), type: "references", attrs: { via: p.name } });
      }
    }

    const memberLines = [
      ...c.functions.map((f) => `  ${f}()`),
      ...c.properties.map((p) => `  ${p.type} ${p.name}`),
    ].join("\n");
    chunks.push({
      id: `${id}#sig`,
      entityId: id,
      kind: "cpp-signature",
      text: `${c.macro} ${c.name}${c.base ? ` : public ${c.base}` : ""}\n${memberLines}`,
    });
  }

  out.entities = entities;
  out.edges = edges;
  out.chunks = chunks;
  return out;
}
