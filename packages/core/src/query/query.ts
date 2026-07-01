import type { Edge, Entity, EntityKind, EdgeType } from "@gameiq/shared";
import type { Store } from "../store/store.js";
import { toFtsQuery } from "./fts.js";

/**
 * Query engine (design §5.3 / §6.1). Three tiers: graph queries (exact),
 * hybrid retrieval (FTS5 now; vectors later), and assembled bundles for agents.
 * Everything here is editor-free and runs against the SQLite index.
 */

export type Direction = "in" | "out" | "both";

export interface SearchResult {
  entity: Entity;
  score: number; // bm25, lower is better
  snippet: string;
  matchedChunkKind: string;
}

export interface EntityDetail {
  entity: Entity;
  outgoing: Edge[];
  incoming: Edge[];
  children: Entity[];
  chunks: Array<{ id: string; kind: string; text: string }>;
}

export interface RefResult {
  entity: Entity;
  viaType: EdgeType;
  depth: number;
  direction: "in" | "out";
}

export interface ImpactResult {
  entity: Entity;
  viaType: EdgeType;
  depth: number;
  severity: number;
}

/** How dangerous it is for a dependent of this edge type if the target changes/disappears. */
const SEVERITY: Record<EdgeType, number> = {
  inherits: 10,
  implements: 9,
  calls: 8,
  "uses-skeleton": 8,
  "uses-material": 7,
  "uses-texture": 5,
  "overrides-parameter": 6,
  "casts-to": 6,
  "placed-in-level": 5,
  "plays-on": 4,
  "depends-on": 4,
  references: 3,
  describes: 2,
  constrains: 2,
};

export class QueryEngine {
  constructor(private readonly store: Store) {}

  /** Hybrid search across everything; optionally filter to one entity kind. */
  searchProject(query: string, kind?: EntityKind, limit = 20): SearchResult[] {
    const fts = toFtsQuery(query);
    if (!fts) return [];
    // over-fetch so a kind filter / dedupe still fills the page
    const hits = this.store.searchChunks(fts, limit * 5);
    const bestByEntity = new Map<string, SearchResult>();
    for (const h of hits) {
      const entity = this.store.getEntity(h.entityId);
      if (!entity) continue;
      if (kind && entity.kind !== kind) continue;
      const existing = bestByEntity.get(entity.id);
      if (!existing || h.score < existing.score) {
        bestByEntity.set(entity.id, {
          entity,
          score: h.score,
          snippet: h.snippet,
          matchedChunkKind: h.kind,
        });
      }
    }
    return [...bestByEntity.values()].sort((a, b) => a.score - b.score).slice(0, limit);
  }

  /** Index-age stamp attached to every response so agents know how stale an answer is (design §8). */
  indexAgeNote(): { lastIngestAtIso: string | null; lastGeneratedAtIso: string | null; projectName: string | null } {
    return {
      lastIngestAtIso: this.store.getMeta("lastIngestAtIso") ?? null,
      lastGeneratedAtIso: this.store.getMeta("lastGeneratedAtIso") ?? null,
      projectName: this.store.getMeta("projectName") ?? null,
    };
  }

  getEntity(id: string): EntityDetail | undefined {
    const entity = this.store.getEntity(id);
    if (!entity) return undefined;
    const chunks = this.store.db
      .prepare(`SELECT id, kind, text FROM chunks WHERE entity_id = ?`)
      .all(id) as Array<{ id: string; kind: string; text: string }>;
    return {
      entity,
      outgoing: this.store.outgoingEdges(id),
      incoming: this.store.incomingEdges(id),
      children: this.store.childrenOf(id),
      chunks,
    };
  }

  /**
   * BFS over the edge graph from `id`, up to `depth` hops. Optionally restrict to one
   * edge type — e.g. references(skeletonId, "in", 1, "uses-skeleton") answers
   * "which meshes use this skeleton?".
   */
  references(id: string, direction: Direction, depth = 1, edgeType?: EdgeType): RefResult[] {
    if (!this.store.getEntity(id)) return [];
    const results: RefResult[] = [];
    const seen = new Set<string>([id]);
    const dirs: Array<"in" | "out"> = direction === "both" ? ["in", "out"] : [direction];

    for (const dir of dirs) {
      let frontier: string[] = [id];
      for (let d = 1; d <= depth; d++) {
        const next: string[] = [];
        for (const node of frontier) {
          const edges = dir === "out" ? this.store.outgoingEdges(node) : this.store.incomingEdges(node);
          for (const e of edges) {
            if (edgeType && e.type !== edgeType) continue;
            const other = dir === "out" ? e.dst : e.src;
            if (seen.has(other)) continue;
            seen.add(other);
            const entity = this.store.getEntity(other);
            if (!entity) continue; // edge to an entity we haven't extracted; skip in results
            results.push({ entity, viaType: e.type, depth: d, direction: dir });
            next.push(other);
          }
        }
        frontier = next;
        if (frontier.length === 0) break;
      }
    }
    return results;
  }

  /** "What could break if I change/delete this" — transitive inbound, ranked by severity. */
  impact(id: string, maxDepth = 4): ImpactResult[] {
    if (!this.store.getEntity(id)) return [];
    const best = new Map<string, ImpactResult>();
    let frontier: string[] = [id];
    const seen = new Set<string>([id]);
    for (let d = 1; d <= maxDepth; d++) {
      const next: string[] = [];
      for (const node of frontier) {
        for (const e of this.store.incomingEdges(node)) {
          const dependent = e.src;
          if (seen.has(dependent)) continue;
          seen.add(dependent);
          const entity = this.store.getEntity(dependent);
          if (!entity) continue;
          const severity = (SEVERITY[e.type] ?? 1) / d; // closer + heavier = higher
          best.set(dependent, { entity, viaType: e.type, depth: d, severity });
          next.push(dependent);
        }
      }
      frontier = next;
      if (frontier.length === 0) break;
    }
    return [...best.values()].sort((a, b) => b.severity - a.severity);
  }

  projectStats(facet: "overview" | "kinds" | "edges" | "unused" | "largest-deps"): unknown {
    switch (facet) {
      case "kinds":
        return this.store.countByKind();
      case "edges":
        return this.store.countByEdgeType();
      case "unused": {
        // asset/blueprint entities that nothing references — deletion candidates
        const rows = this.store.db
          .prepare(
            `SELECT e.id, e.kind, e.name, e.path FROM entities e
             WHERE e.kind IN ('asset','blueprint')
               AND NOT EXISTS (SELECT 1 FROM edges x WHERE x.dst = e.id)
             LIMIT 500`,
          )
          .all();
        return rows;
      }
      case "largest-deps": {
        const rows = this.store.db
          .prepare(
            `SELECT e.id, e.name, e.kind, COUNT(x.dst) AS outDeps
             FROM entities e JOIN edges x ON x.src = e.id
             GROUP BY e.id ORDER BY outDeps DESC LIMIT 50`,
          )
          .all();
        return rows;
      }
      case "overview":
      default:
        return {
          totalEntities: this.store.totalEntities(),
          byKind: this.store.countByKind(),
          byEdgeType: this.store.countByEdgeType(),
          projectName: this.store.getMeta("projectName") ?? null,
          lastIngestAtIso: this.store.getMeta("lastIngestAtIso") ?? null,
          lastGeneratedAtIso: this.store.getMeta("lastGeneratedAtIso") ?? null,
        };
    }
  }

  /**
   * Assemble a retrieval bundle for a system/topic (design §6.1 `explain`).
   * Basic in MVP: top search hits + their immediate neighborhood + pseudocode chunks.
   */
  explain(topic: string, limit = 8): {
    topic: string;
    seeds: SearchResult[];
    related: RefResult[];
  } {
    const seeds = this.searchProject(topic, undefined, limit);
    const related: RefResult[] = [];
    const seen = new Set(seeds.map((s) => s.entity.id));
    for (const s of seeds) {
      for (const r of this.references(s.entity.id, "both", 1)) {
        if (seen.has(r.entity.id)) continue;
        seen.add(r.entity.id);
        related.push(r);
      }
    }
    return { topic, seeds, related };
  }
}
