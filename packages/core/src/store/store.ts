import { mkdirSync } from "node:fs";
import { dirname } from "node:path";
import Database from "better-sqlite3";
import {
  SCHEMA_VERSION,
  type Chunk,
  type Edge,
  type Entity,
  type EntityKind,
} from "@gameiq/shared";

/**
 * Low-level persistence over SQLite (design §5.2). The store owns the schema and
 * raw data access; query semantics live in ../query. FTS5 backs full-text search;
 * embeddings (sqlite-vec) are deliberately out of the MVP (design §7).
 */

/** Incremental DDL migrations, applied in order. Never edit a shipped migration — append a new one. */
const MIGRATIONS: string[] = [
  // 1 — initial schema
  `
  CREATE TABLE meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
  );

  CREATE TABLE entities (
    id      TEXT PRIMARY KEY,
    kind    TEXT NOT NULL,
    name    TEXT NOT NULL,
    path    TEXT NOT NULL,
    parent  TEXT,
    source  TEXT NOT NULL,
    summary TEXT,
    detail  TEXT
  );
  CREATE INDEX idx_entities_kind   ON entities(kind);
  CREATE INDEX idx_entities_name   ON entities(name);
  CREATE INDEX idx_entities_parent ON entities(parent);

  CREATE TABLE edges (
    src   TEXT NOT NULL,
    dst   TEXT NOT NULL,
    type  TEXT NOT NULL,
    attrs TEXT,
    PRIMARY KEY (src, dst, type)
  );
  CREATE INDEX idx_edges_src ON edges(src);
  CREATE INDEX idx_edges_dst ON edges(dst);

  CREATE TABLE chunks (
    id        TEXT PRIMARY KEY,
    entity_id TEXT NOT NULL,
    kind      TEXT NOT NULL,
    text      TEXT NOT NULL
  );
  CREATE INDEX idx_chunks_entity ON chunks(entity_id);

  -- Standalone FTS5 (stores text + unindexed ids); maintained manually on ingest.
  CREATE VIRTUAL TABLE chunks_fts USING fts5(
    chunk_id UNINDEXED,
    entity_id UNINDEXED,
    text
  );
  `,
  // 2 — producer-scoped ingest: tag every row with its producer so re-ingesting a
  // producer replaces only its own rows (no cross-producer clobbering of chunks/edges).
  `
  ALTER TABLE entities ADD COLUMN producer TEXT;
  ALTER TABLE edges    ADD COLUMN producer TEXT;
  ALTER TABLE chunks   ADD COLUMN producer TEXT;
  CREATE INDEX idx_entities_producer ON entities(producer);
  CREATE INDEX idx_edges_producer    ON edges(producer);
  CREATE INDEX idx_chunks_producer   ON chunks(producer);

  DROP TABLE chunks_fts;
  CREATE VIRTUAL TABLE chunks_fts USING fts5(
    chunk_id UNINDEXED,
    entity_id UNINDEXED,
    producer UNINDEXED,
    text
  );
  INSERT INTO chunks_fts(chunk_id, entity_id, producer, text)
    SELECT id, entity_id, producer, text FROM chunks;
  `,
];

export interface SearchHit {
  chunkId: string;
  entityId: string;
  kind: string;
  score: number; // bm25 — lower is better
  snippet: string;
}

interface EntityRow {
  id: string;
  kind: string;
  name: string;
  path: string;
  parent: string | null;
  source: string;
  summary: string | null;
  detail: string | null;
}

interface EdgeRow {
  src: string;
  dst: string;
  type: string;
  attrs: string | null;
}

function rowToEntity(r: EntityRow): Entity {
  return {
    id: r.id,
    kind: r.kind as EntityKind,
    name: r.name,
    path: r.path,
    parent: r.parent,
    source: r.source as Entity["source"],
    summary: r.summary,
    detail: r.detail ? (JSON.parse(r.detail) as Record<string, unknown>) : null,
  };
}

function rowToEdge(r: EdgeRow): Edge {
  return {
    src: r.src,
    dst: r.dst,
    type: r.type as Edge["type"],
    attrs: r.attrs ? (JSON.parse(r.attrs) as Record<string, unknown>) : null,
  };
}

export class Store {
  readonly db: Database.Database;

  constructor(dbPath: string) {
    if (dbPath !== ":memory:") mkdirSync(dirname(dbPath), { recursive: true });
    this.db = new Database(dbPath);
    this.db.pragma("journal_mode = WAL");
    this.db.pragma("synchronous = NORMAL");
    this.db.pragma("foreign_keys = OFF"); // edges may point at not-yet-extracted entities
    this.#migrate();
  }

  #migrate(): void {
    const current = this.db.pragma("user_version", { simple: true }) as number;
    const tx = this.db.transaction(() => {
      for (let v = current; v < MIGRATIONS.length; v++) {
        this.db.exec(MIGRATIONS[v]!);
      }
      this.db.pragma(`user_version = ${MIGRATIONS.length}`);
    });
    tx();
    this.setMeta("schemaVersion", String(SCHEMA_VERSION));
  }

  close(): void {
    this.db.close();
  }

  /** Run `fn` inside a single transaction. */
  transaction<T>(fn: () => T): T {
    return this.db.transaction(fn)();
  }

  // ---- meta ----------------------------------------------------------------

  setMeta(key: string, value: string): void {
    this.db
      .prepare(`INSERT INTO meta(key, value) VALUES (?, ?)
                ON CONFLICT(key) DO UPDATE SET value = excluded.value`)
      .run(key, value);
  }

  getMeta(key: string): string | undefined {
    const row = this.db.prepare(`SELECT value FROM meta WHERE key = ?`).get(key) as
      | { value: string }
      | undefined;
    return row?.value;
  }

  // ---- writes (used by ingest) --------------------------------------------

  clearAll(): void {
    this.db.exec(`DELETE FROM entities; DELETE FROM edges; DELETE FROM chunks; DELETE FROM chunks_fts;`);
  }

  /** Remove every row contributed by one producer (producer-scoped replace). */
  deleteByProducer(producer: string): void {
    this.db.prepare(`DELETE FROM chunks_fts WHERE producer = ?`).run(producer);
    this.db.prepare(`DELETE FROM chunks WHERE producer = ?`).run(producer);
    this.db.prepare(`DELETE FROM edges WHERE producer = ?`).run(producer);
    this.db.prepare(`DELETE FROM entities WHERE producer = ?`).run(producer);
  }

  upsertEntity(e: Entity, producer: string): void {
    this.db
      .prepare(
        `INSERT INTO entities (id, kind, name, path, parent, source, summary, detail, producer)
         VALUES (@id, @kind, @name, @path, @parent, @source, @summary, @detail, @producer)
         ON CONFLICT(id) DO UPDATE SET
           kind=excluded.kind, name=excluded.name, path=excluded.path,
           parent=excluded.parent, source=excluded.source,
           summary=excluded.summary, detail=excluded.detail, producer=excluded.producer`,
      )
      .run({
        id: e.id,
        kind: e.kind,
        name: e.name,
        path: e.path,
        parent: e.parent ?? null,
        source: e.source,
        summary: e.summary ?? null,
        detail: e.detail ? JSON.stringify(e.detail) : null,
        producer,
      });
  }

  insertEdge(edge: Edge, producer: string): void {
    this.db
      .prepare(
        `INSERT INTO edges (src, dst, type, attrs, producer) VALUES (?, ?, ?, ?, ?)
         ON CONFLICT(src, dst, type) DO UPDATE SET attrs = excluded.attrs, producer = excluded.producer`,
      )
      .run(edge.src, edge.dst, edge.type, edge.attrs ? JSON.stringify(edge.attrs) : null, producer);
  }

  insertChunk(c: Chunk, producer: string): void {
    this.db
      .prepare(`INSERT INTO chunks (id, entity_id, kind, text, producer) VALUES (?, ?, ?, ?, ?)
                ON CONFLICT(id) DO UPDATE SET entity_id=excluded.entity_id, kind=excluded.kind,
                  text=excluded.text, producer=excluded.producer`)
      .run(c.id, c.entityId, c.kind, c.text, producer);
    // keep FTS in sync (delete any prior row for this chunk id, then insert)
    this.db.prepare(`DELETE FROM chunks_fts WHERE chunk_id = ?`).run(c.id);
    this.db
      .prepare(`INSERT INTO chunks_fts (chunk_id, entity_id, producer, text) VALUES (?, ?, ?, ?)`)
      .run(c.id, c.entityId, producer, c.text);
  }

  // ---- reads (used by query engine) ---------------------------------------

  getEntity(id: string): Entity | undefined {
    const row = this.db.prepare(`SELECT * FROM entities WHERE id = ?`).get(id) as EntityRow | undefined;
    return row ? rowToEntity(row) : undefined;
  }

  getEntitiesByKind(kind: EntityKind, limit = 1000): Entity[] {
    const rows = this.db
      .prepare(`SELECT * FROM entities WHERE kind = ? LIMIT ?`)
      .all(kind, limit) as EntityRow[];
    return rows.map(rowToEntity);
  }

  /** Child entities (those whose parent is `id`) — e.g. a blueprint's variables/components/functions. */
  childrenOf(id: string, limit = 500): Entity[] {
    const rows = this.db
      .prepare(`SELECT * FROM entities WHERE parent = ? LIMIT ?`)
      .all(id, limit) as EntityRow[];
    return rows.map(rowToEntity);
  }

  /** Outgoing edges (this entity depends on / uses dst). */
  outgoingEdges(id: string): Edge[] {
    const rows = this.db.prepare(`SELECT * FROM edges WHERE src = ?`).all(id) as EdgeRow[];
    return rows.map(rowToEdge);
  }

  /** Incoming edges (other entities reference this id). */
  incomingEdges(id: string): Edge[] {
    const rows = this.db.prepare(`SELECT * FROM edges WHERE dst = ?`).all(id) as EdgeRow[];
    return rows.map(rowToEdge);
  }

  /** Full-text search over chunk text; returns hits ordered best-first. */
  searchChunks(ftsQuery: string, limit = 20): SearchHit[] {
    const rows = this.db
      .prepare(
        `SELECT f.chunk_id AS chunkId, f.entity_id AS entityId, c.kind AS kind,
                bm25(chunks_fts) AS score,
                snippet(chunks_fts, 3, '[', ']', ' … ', 12) AS snippet
         FROM chunks_fts f
         JOIN chunks c ON c.id = f.chunk_id
         WHERE chunks_fts MATCH ?
         ORDER BY score
         LIMIT ?`,
      )
      .all(ftsQuery, limit) as SearchHit[];
    return rows;
  }

  countByKind(): Array<{ kind: string; count: number }> {
    return this.db
      .prepare(`SELECT kind, COUNT(*) AS count FROM entities GROUP BY kind ORDER BY count DESC`)
      .all() as Array<{ kind: string; count: number }>;
  }

  countByEdgeType(): Array<{ type: string; count: number }> {
    return this.db
      .prepare(`SELECT type, COUNT(*) AS count FROM edges GROUP BY type ORDER BY count DESC`)
      .all() as Array<{ type: string; count: number }>;
  }

  totalEntities(): number {
    return (this.db.prepare(`SELECT COUNT(*) AS n FROM entities`).get() as { n: number }).n;
  }
}
