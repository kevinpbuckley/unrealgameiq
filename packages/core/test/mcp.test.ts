import { test } from "node:test";
import assert from "node:assert/strict";
import { rmSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { Client } from "@modelcontextprotocol/sdk/client/index.js";
import { InMemoryTransport } from "@modelcontextprotocol/sdk/inMemory.js";
import { indexProject } from "../src/index/indexer.js";
import { Store } from "../src/store/store.js";
import { QueryEngine } from "../src/query/query.js";
import { createMcpServer } from "../src/mcp/server.js";

const here = dirname(fileURLToPath(import.meta.url));
const fixture = join(here, "fixtures", "SampleGame");
const commandletJson = join(fixture, "_commandlet", "blueprints.json");

interface ToolText {
  content: Array<{ type: string; text: string }>;
  isError?: boolean;
}

/** Call game_iq and return the parsed `result` field of the JSON payload. */
async function call(client: Client, args: Record<string, unknown>): Promise<unknown> {
  const res = (await client.callTool({ name: "game_iq", arguments: args })) as ToolText;
  assert.ok(!res.isError, `tool errored: ${res.content[0]?.text}`);
  return JSON.parse(res.content[0]!.text).result;
}

test("MCP server exposes the index over the game_iq tool", async () => {
  const dbPath = join(tmpdir(), `gameiq-mcp-${process.pid}-${process.hrtime.bigint()}.db`);
  rmSync(dbPath, { force: true });
  indexProject({ projectRoot: fixture, dbPath, mode: "replace", extractorJson: [commandletJson] });

  const store = new Store(dbPath);
  const server = createMcpServer(new QueryEngine(store));
  const [clientTransport, serverTransport] = InMemoryTransport.createLinkedPair();
  const client = new Client({ name: "test", version: "1.0.0" }, { capabilities: {} });
  await Promise.all([server.connect(serverTransport), client.connect(clientTransport)]);

  try {
    // the tool is advertised
    const tools = await client.listTools();
    assert.ok(tools.tools.some((t) => t.name === "game_iq"), "game_iq tool listed");

    // search surfaces Blueprint pseudocode grep can't see
    const hits = (await call(client, { action: "search", query: "fall damage" })) as Array<{
      entity: { id: string };
    }>;
    assert.ok(
      hits.some((h) => h.entity.id.startsWith("asset:/Game/Blueprints/BP_PlayerCharacter")),
      "search finds the Blueprint",
    );

    // get_entity returns children (the class's members)
    const detail = (await call(client, { action: "get_entity", id: "cpp:HealthComponent" })) as {
      children: Array<{ id: string }>;
    };
    assert.ok(
      detail.children.some((c) => c.id === "cpp:HealthComponent::ApplyDamage"),
      "get_entity exposes members as children",
    );

    // references with an edge-type filter (the 'what uses X via a specific relation' query)
    const refs = (await call(client, {
      action: "references",
      id: "cpp:HealthComponent",
      direction: "in",
      edgeType: "references",
    })) as Array<{ entity: { id: string }; viaType: string }>;
    assert.ok(
      refs.some((r) => r.entity.id === "cpp:PlayerCharacter" && r.viaType === "references"),
      "edge-type-filtered references works",
    );

    // impact ranks dependents
    const impact = (await call(client, { action: "impact", id: "cpp:HealthComponent" })) as Array<{
      entity: { id: string };
    }>;
    assert.ok(impact.length > 0, "impact returns dependents");
  } finally {
    await client.close();
    store.close();
    for (const s of ["", "-wal", "-shm"]) rmSync(dbPath + s, { force: true });
  }
});
