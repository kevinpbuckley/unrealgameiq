export { Store, type SearchHit } from "./store/store.js";
export * from "./query/query.js";
export * from "./ingest/ingest.js";
export * from "./index/indexer.js";
export { drainIncremental, type DrainResult } from "./index/incremental.js";
export * from "./extract/project.js";
export { extractCpp, CPP_PRODUCER } from "./extract/cpp.js";
export { extractConfig, CONFIG_PRODUCER } from "./extract/config.js";
export { runAction, type GameIqArgs } from "./mcp/actions.js";
export { createMcpServer, runStdio } from "./mcp/server.js";
export {
  loadConfig,
  effectiveExcludes,
  CONFIG_FILENAME,
  ALWAYS_EXCLUDE,
  type GameIqConfig,
} from "./config.js";
