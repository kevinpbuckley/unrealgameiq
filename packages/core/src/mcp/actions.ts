import type { EdgeType, EntityKind } from "@gameiq/shared";
import type { Direction, QueryEngine } from "../query/query.js";

/** Parsed arguments for the single multi-action `game_iq` tool (design §6.1: few tools, rich params). */
export interface GameIqArgs {
  action: "search" | "get_entity" | "references" | "impact" | "explain" | "project_stats";
  query?: string;
  kind?: string;
  id?: string;
  direction?: Direction;
  depth?: number;
  edgeType?: string;
  facet?: "overview" | "kinds" | "edges" | "unused" | "largest-deps";
  limit?: number;
}

/** Dispatch one action against the query engine. Shared by the MCP server and the CLI. */
export function runAction(engine: QueryEngine, args: GameIqArgs): unknown {
  switch (args.action) {
    case "search":
      return engine.searchProject(args.query ?? "", args.kind as EntityKind | undefined, args.limit ?? 20);
    case "get_entity": {
      if (!args.id) throw new Error("get_entity requires `id`");
      const detail = engine.getEntity(args.id);
      if (!detail) return { error: `entity not found: ${args.id}` };
      return detail;
    }
    case "references": {
      if (!args.id) throw new Error("references requires `id`");
      return engine.references(
        args.id,
        args.direction ?? "both",
        args.depth ?? 1,
        args.edgeType as EdgeType | undefined,
      );
    }
    case "impact": {
      if (!args.id) throw new Error("impact requires `id`");
      return engine.impact(args.id);
    }
    case "explain":
      return engine.explain(args.query ?? "", args.limit ?? 8);
    case "project_stats":
      return engine.projectStats(args.facet ?? "overview");
    default:
      throw new Error(`unknown action: ${String((args as { action: string }).action)}`);
  }
}
