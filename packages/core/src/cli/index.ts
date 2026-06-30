#!/usr/bin/env node
import { cpSync, existsSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import { Command } from "commander";
import { indexProject } from "../index/indexer.js";
import { gameiqPaths, projectInfo } from "../extract/project.js";
import { Store } from "../store/store.js";
import { QueryEngine } from "../query/query.js";
import { runStdio } from "../mcp/server.js";

const moduleDir = dirname(fileURLToPath(import.meta.url));

function resolveDb(projectRoot: string, dbOpt?: string): string {
  return dbOpt ? resolve(dbOpt) : gameiqPaths(resolve(projectRoot)).dbPath;
}

function openEngine(projectRoot: string, dbOpt?: string): { store: Store; engine: QueryEngine } {
  const dbPath = resolveDb(projectRoot, dbOpt);
  if (!existsSync(dbPath)) {
    throw new Error(`no index at ${dbPath}. Run \`gameiq index --project ${projectRoot}\` first.`);
  }
  const store = new Store(dbPath);
  return { store, engine: new QueryEngine(store) };
}

/** Walk up from the installed module to find the bundled `plugin/GameIQ` source. */
function findPluginSource(): string | undefined {
  let dir = moduleDir;
  for (let i = 0; i < 8; i++) {
    const candidate = join(dir, "plugin", "GameIQ");
    if (existsSync(join(candidate, "GameIQ.uplugin"))) return candidate;
    const parent = dirname(dir);
    if (parent === dir) break;
    dir = parent;
  }
  return undefined;
}

const program = new Command();
program.name("gameiq").description("Unreal Game IQ — queryable index of an Unreal Engine project").version("0.1.0");

program
  .command("index")
  .description("build or refresh the index for a project (editor-less tiers + any commandlet output)")
  .requiredOption("-p, --project <dir>", "path to the project root (folder containing the .uproject)")
  .option("--db <path>", "override index DB path")
  .option("--changed", "incremental merge instead of full rebuild")
  .action((opts: { project: string; db?: string; changed?: boolean }) => {
    const root = resolve(opts.project);
    const result = indexProject({
      projectRoot: root,
      dbPath: opts.db ? resolve(opts.db) : undefined,
      mode: opts.changed ? "merge" : "replace",
    });
    process.stdout.write(`Indexed "${result.projectName}" → ${result.dbPath}\n`);
    for (const p of result.producers) {
      process.stdout.write(
        `  ${p.producer}: ${p.summary.entities} entities, ${p.summary.edges} edges, ${p.summary.chunks} chunks (${p.summary.mode})\n`,
      );
    }
    process.stdout.write(`  total entities: ${result.totalEntities}\n`);
  });

program
  .command("mcp")
  .description("serve the index over the Model Context Protocol (stdio)")
  .requiredOption("-p, --project <dir>", "path to the project root")
  .option("--db <path>", "override index DB path")
  .action(async (opts: { project: string; db?: string }) => {
    const { store } = openEngine(opts.project, opts.db);
    process.stderr.write(`[gameiq] MCP server ready for ${resolve(opts.project)}\n`);
    await runStdio(store);
  });

program
  .command("search")
  .description("hybrid search across the index")
  .argument("<query...>", "search text")
  .requiredOption("-p, --project <dir>", "path to the project root")
  .option("--db <path>", "override index DB path")
  .option("-k, --kind <kind>", "filter to an entity kind")
  .option("-n, --limit <n>", "max results", "10")
  .action((queryParts: string[], opts: { project: string; db?: string; kind?: string; limit: string }) => {
    const { store, engine } = openEngine(opts.project, opts.db);
    try {
      const hits = engine.searchProject(queryParts.join(" "), opts.kind as never, Number(opts.limit));
      for (const h of hits) {
        process.stdout.write(`${h.entity.id}  [${h.entity.kind}]  ${h.entity.name}\n    ${h.snippet}\n`);
      }
      if (hits.length === 0) process.stdout.write("(no matches)\n");
    } finally {
      store.close();
    }
  });

program
  .command("impact")
  .description("what could break if this entity changes")
  .argument("<id>", "entity id")
  .requiredOption("-p, --project <dir>", "path to the project root")
  .option("--db <path>", "override index DB path")
  .action((id: string, opts: { project: string; db?: string }) => {
    const { store, engine } = openEngine(opts.project, opts.db);
    try {
      const rows = engine.impact(id);
      for (const r of rows) {
        process.stdout.write(`${r.severity.toFixed(1)}  ${r.entity.id}  via ${r.viaType} (depth ${r.depth})\n`);
      }
      if (rows.length === 0) process.stdout.write("(nothing depends on this, or id not found)\n");
    } finally {
      store.close();
    }
  });

program
  .command("stats")
  .description("project statistics")
  .requiredOption("-p, --project <dir>", "path to the project root")
  .option("--db <path>", "override index DB path")
  .option("-f, --facet <facet>", "overview|kinds|edges|unused|largest-deps", "overview")
  .action((opts: { project: string; db?: string; facet: string }) => {
    const { store, engine } = openEngine(opts.project, opts.db);
    try {
      process.stdout.write(JSON.stringify(engine.projectStats(opts.facet as never), null, 2) + "\n");
    } finally {
      store.close();
    }
  });

program
  .command("init")
  .description("install the GameIQ UE plugin into a project's Plugins/ folder")
  .requiredOption("-p, --project <dir>", "path to the project root")
  .action((opts: { project: string }) => {
    const root = resolve(opts.project);
    const info = projectInfo(root);
    if (!info.uprojectPath) {
      process.stderr.write(`[gameiq] warning: no .uproject found in ${root}\n`);
    }
    const src = findPluginSource();
    if (!src) {
      process.stderr.write("[gameiq] could not locate bundled plugin/GameIQ source; install manually.\n");
      process.exitCode = 1;
      return;
    }
    const dest = join(root, "Plugins", "GameIQ");
    cpSync(src, dest, { recursive: true });
    process.stdout.write(`Installed GameIQ plugin → ${dest}\n`);

    // gitignore the plugin by default (design §10.1: managed dependency, not vendored)
    const giPath = join(root, ".gitignore");
    const line = "/Plugins/GameIQ/";
    let gi = existsSync(giPath) ? readFileSync(giPath, "utf8") : "";
    if (!gi.split(/\r?\n/).includes(line)) {
      gi += (gi.endsWith("\n") || gi.length === 0 ? "" : "\n") + line + "\n";
      writeFileSync(giPath, gi);
      process.stdout.write(`Added ${line} to .gitignore\n`);
    }
    process.stdout.write("Next: enable the plugin in the editor (or .uproject), then `gameiq index`.\n");
  });

program.parseAsync(process.argv).catch((err: unknown) => {
  process.stderr.write(`gameiq: ${(err as Error).message}\n`);
  process.exitCode = 1;
});
