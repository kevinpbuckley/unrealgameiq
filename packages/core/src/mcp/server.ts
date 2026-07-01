import { McpServer } from "@modelcontextprotocol/sdk/server/mcp.js";
import { StdioServerTransport } from "@modelcontextprotocol/sdk/server/stdio.js";
import { z } from "zod";
import { QueryEngine } from "../query/query.js";
import type { Store } from "../store/store.js";
import { runAction, type GameIqArgs } from "./actions.js";

/**
 * MCP server — the product's center of gravity (design §6.1). A single
 * multi-action `game_iq` tool keeps the tool count low in agent contexts,
 * matching the unreal-engine-skills-manager consolidation pattern.
 */

const DESCRIPTION = [
  "Query the Unreal Game IQ index for this project — a queryable model of its assets,",
  "Blueprints, C++, and config and how they connect. Actions:",
  "`search` (hybrid search; needs `query`, optional `kind`),",
  "`get_entity` (full detail; needs `id`),",
  "`references` (who uses / what this uses; needs `id`, optional `direction`=in|out|both, `depth`,",
  "`edgeType` to filter e.g. uses-skeleton/uses-material/calls/placed-in-level),",
  "`impact` (what could break if this changes; needs `id`),",
  "`explain` (assembled context bundle for a system; needs `query`),",
  "`project_stats` (counts/hygiene; optional `facet`=overview|kinds|edges|unused|largest-deps).",
].join(" ");

export function createMcpServer(engine: QueryEngine): McpServer {
  const server = new McpServer({ name: "unreal-game-iq", version: "0.1.0" });

  server.tool(
    "game_iq",
    DESCRIPTION,
    {
      action: z.enum(["search", "get_entity", "references", "impact", "explain", "project_stats"]),
      query: z.string().optional().describe("search/explain text"),
      kind: z.string().optional().describe("filter search/references to an entity kind, e.g. blueprint, level-actor"),
      id: z.string().optional().describe("entity id for get_entity/references/impact"),
      direction: z.enum(["in", "out", "both"]).optional(),
      depth: z.number().int().min(1).max(8).optional(),
      edgeType: z.string().optional().describe("restrict references to one edge type, e.g. uses-skeleton"),
      facet: z.enum(["overview", "kinds", "edges", "unused", "largest-deps"]).optional(),
      limit: z.number().int().min(1).max(100).optional(),
    },
    async (args) => {
      try {
        const result = runAction(engine, args as GameIqArgs);
        const ageNote = engine.indexAgeNote();
        return {
          content: [{ type: "text", text: JSON.stringify({ result, index: ageNote }, null, 2) }],
        };
      } catch (err) {
        return {
          isError: true,
          content: [{ type: "text", text: `game_iq error: ${(err as Error).message}` }],
        };
      }
    },
  );

  return server;
}

/** Open a query engine over `store`, wire it to a stdio MCP server, and serve. */
export async function runStdio(store: Store): Promise<void> {
  const engine = new QueryEngine(store);
  const server = createMcpServer(engine);
  const transport = new StdioServerTransport();
  await server.connect(transport);
}
