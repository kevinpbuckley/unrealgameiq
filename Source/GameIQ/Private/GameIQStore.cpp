// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQStore.h"

#include "Dom/JsonObject.h"
#include "GameIQJson.h"
#include "GameIQTextUtil.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQStore, Log, All);

namespace
{
	// Each migration is a list of SINGLE statements: FSQLiteDatabase::Execute prepares exactly one
	// statement (sqlite3 prepare semantics), so a multi-statement string would silently run only
	// its first statement — the bug that shipped empty indexes on fresh projects.
	const TCHAR* Migration1[] = {
		TEXT("CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"),
		TEXT("CREATE TABLE entities (id TEXT PRIMARY KEY, kind TEXT NOT NULL, name TEXT NOT NULL, path TEXT NOT NULL, parent TEXT, source TEXT NOT NULL, summary TEXT, detail TEXT);"),
		TEXT("CREATE INDEX idx_entities_kind ON entities(kind);"),
		TEXT("CREATE INDEX idx_entities_name ON entities(name);"),
		TEXT("CREATE INDEX idx_entities_parent ON entities(parent);"),
		TEXT("CREATE TABLE edges (src TEXT NOT NULL, dst TEXT NOT NULL, type TEXT NOT NULL, attrs TEXT, PRIMARY KEY (src, dst, type));"),
		TEXT("CREATE INDEX idx_edges_src ON edges(src);"),
		TEXT("CREATE INDEX idx_edges_dst ON edges(dst);"),
		TEXT("CREATE TABLE chunks (id TEXT PRIMARY KEY, entity_id TEXT NOT NULL, kind TEXT NOT NULL, text TEXT NOT NULL);"),
		TEXT("CREATE INDEX idx_chunks_entity ON chunks(entity_id);"),
		TEXT("CREATE VIRTUAL TABLE chunks_fts USING fts5(chunk_id UNINDEXED, entity_id UNINDEXED, text);"),
	};

	const TCHAR* Migration2[] = {
		TEXT("ALTER TABLE entities ADD COLUMN producer TEXT;"),
		TEXT("ALTER TABLE edges ADD COLUMN producer TEXT;"),
		TEXT("ALTER TABLE chunks ADD COLUMN producer TEXT;"),
		TEXT("CREATE INDEX idx_entities_producer ON entities(producer);"),
		TEXT("CREATE INDEX idx_edges_producer ON edges(producer);"),
		TEXT("CREATE INDEX idx_chunks_producer ON chunks(producer);"),
		// The v2 FTS shape is immediately superseded by migration 4; recreating it here keeps the
		// migration chain valid for any DB that was genuinely at v1.
		TEXT("DROP TABLE IF EXISTS chunks_fts;"),
		TEXT("CREATE VIRTUAL TABLE chunks_fts USING fts5(chunk_id UNINDEXED, entity_id UNINDEXED, producer UNINDEXED, text);"),
		TEXT("INSERT INTO chunks_fts(chunk_id, entity_id, producer, text) SELECT id, entity_id, producer, text FROM chunks;"),
	};

	// Provenance/authority (issue #3): every entity is either extracted ground truth or stated design
	// intent. NULL after the ALTER; ingest re-inserts every row with a concrete value, and the query
	// layer COALESCEs any residual NULL to 'extracted-fact'.
	const TCHAR* Migration3[] = {
		TEXT("ALTER TABLE entities ADD COLUMN authority TEXT;"),
		TEXT("CREATE INDEX idx_entities_authority ON entities(authority);"),
	};

	// Retrieval upgrade: external-content FTS5 over chunks(text, aux) — porter stemming so
	// "reload" matches "Reloading", a prefix index for fast prefix queries, and an `aux` column of
	// camelCase/underscore-split identifier tokens so "player" matches BP_PlayerCharacter. Content
	// stays in `chunks` (no second copy of every chunk's text) and triggers keep FTS in sync, so
	// DeleteByProducer/DeleteSubtree no longer hand-maintain the FTS table.
	const TCHAR* Migration4[] = {
		TEXT("ALTER TABLE chunks ADD COLUMN aux TEXT;"),
		TEXT("DROP TABLE IF EXISTS chunks_fts;"),
		TEXT("CREATE VIRTUAL TABLE chunks_fts USING fts5(text, aux, content='chunks', content_rowid='rowid', tokenize='porter unicode61', prefix='2 3');"),
		TEXT("CREATE TRIGGER chunks_fts_ai AFTER INSERT ON chunks BEGIN INSERT INTO chunks_fts(rowid, text, aux) VALUES (new.rowid, new.text, COALESCE(new.aux,'')); END;"),
		TEXT("CREATE TRIGGER chunks_fts_ad AFTER DELETE ON chunks BEGIN INSERT INTO chunks_fts(chunks_fts, rowid, text, aux) VALUES ('delete', old.rowid, old.text, COALESCE(old.aux,'')); END;"),
		TEXT("CREATE TRIGGER chunks_fts_au AFTER UPDATE ON chunks BEGIN INSERT INTO chunks_fts(chunks_fts, rowid, text, aux) VALUES ('delete', old.rowid, old.text, COALESCE(old.aux,'')); INSERT INTO chunks_fts(rowid, text, aux) VALUES (new.rowid, new.text, COALESCE(new.aux,'')); END;"),
		TEXT("INSERT INTO chunks_fts(rowid, text, aux) SELECT rowid, text, COALESCE(aux,'') FROM chunks;"),
	};

	constexpr int32 SchemaMigrations = 4; // C++ store schema version (TS store retired)

	FString AsObject(const FJsonObject& O, const TCHAR* Field)
	{
		const TSharedPtr<FJsonObject>* Sub = nullptr;
		if (O.TryGetObjectField(Field, Sub) && Sub && Sub->IsValid())
		{
			FString S;
			const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&S);
			FJsonSerializer::Serialize(Sub->ToSharedRef(), Writer);
			return S;
		}
		return FString(); // empty => bind NULL
	}

	const FJsonObject* AsObjectPtr(const TSharedPtr<FJsonValue>& V)
	{
		if (V.IsValid() && V->Type == EJson::Object)
		{
			return V->AsObject().Get();
		}
		return nullptr;
	}
}

FString FGameIQStore::DefaultDbPath()
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	return FPaths::Combine(Root, TEXT(".gameiq"), TEXT("index.db"));
}

FString FGameIQStore::ReadMetaValue(const FString& Key)
{
	const FString Path = DefaultDbPath();
	if (!FPaths::FileExists(Path)) { return FString(); }
	FSQLiteDatabase Db;
	if (!Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadOnly) && !Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadWrite))
	{
		return FString();
	}
	FString Value;
	{
		FSQLitePreparedStatement Stmt(Db, TEXT("SELECT value FROM meta WHERE key = ?"));
		if (Stmt.IsValid())
		{
			Stmt.SetBindingValueByIndex(1, Key);
			if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				Stmt.GetColumnValueByName(TEXT("value"), Value);
			}
		}
	}
	Db.Close();
	return Value;
}

bool FGameIQStore::Open()
{
	return OpenInternal(DefaultDbPath(), /*bAllowRecreate=*/true);
}

bool FGameIQStore::OpenAtPath(const FString& DbPath)
{
	return OpenInternal(DbPath, /*bAllowRecreate=*/true);
}

bool FGameIQStore::OpenInternal(const FString& DbPath, bool bAllowRecreate)
{
	OpenedPath = DbPath;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(DbPath), /*Tree=*/true);
	if (!Db.Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogGameIQStore, Warning, TEXT("Game IQ: cannot open index for write at %s: %s"), *DbPath, *Db.GetLastError());
		return false;
	}
	// Rollback journal, explicitly: WAL is IMPOSSIBLE under UE's SQLiteCore — its custom VFS is
	// iVersion 1 with no shared-memory (xShm*) methods (SQLiteEmbeddedPlatform.cpp), so
	// `PRAGMA journal_mode=WAL` silently stays in legacy mode. Cross-process contention is
	// handled by busy_timeout + the BeginTransaction retry loop instead.
	Exec(TEXT("PRAGMA journal_mode=DELETE;"));
	Exec(TEXT("PRAGMA synchronous=NORMAL;"));
	Exec(TEXT("PRAGMA busy_timeout=3000;"));
	Exec(TEXT("PRAGMA foreign_keys=OFF;"));

	if (!EnsureSchema())
	{
		if (!bAllowRecreate)
		{
			Close();
			return false;
		}
		// Self-heal: the schema is broken/inconsistent (e.g. a version stamp with missing tables,
		// the July 2026 empty-index bug). Rebuild the DB file from scratch and invalidate the
		// extract hash caches so the next build re-extracts instead of "carrying forward" into
		// an empty index.
		UE_LOG(LogGameIQStore, Warning, TEXT("Game IQ: index schema at %s is inconsistent — rebuilding the DB file from scratch."), *DbPath);
		StmtCache.Empty();
		Db.Close();
		IFileManager::Get().Delete(*DbPath, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
		IFileManager::Get().Delete(*(DbPath + TEXT("-wal")), false, true, true);
		IFileManager::Get().Delete(*(DbPath + TEXT("-shm")), false, true, true);
		if (!Db.Open(*DbPath, ESQLiteDatabaseOpenMode::ReadWriteCreate))
		{
			UE_LOG(LogGameIQStore, Error, TEXT("Game IQ: could not recreate index at %s: %s"), *DbPath, *Db.GetLastError());
			return false;
		}
		Exec(TEXT("PRAGMA journal_mode=DELETE;"));
		Exec(TEXT("PRAGMA synchronous=NORMAL;"));
		Exec(TEXT("PRAGMA busy_timeout=3000;"));
		Exec(TEXT("PRAGMA foreign_keys=OFF;"));
		if (!EnsureSchema())
		{
			UE_LOG(LogGameIQStore, Error, TEXT("Game IQ: schema creation failed on a fresh DB at %s — giving up."), *DbPath);
			Close();
			return false;
		}
		InvalidateExtractCaches();
	}

	// First-ever open of this DB file: stamp a generation id the extract caches key off.
	if (GetMeta(TEXT("dbGeneration")).IsEmpty())
	{
		SetMeta(TEXT("dbGeneration"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
	}
	return true;
}

void FGameIQStore::Close()
{
	if (Db.IsValid())
	{
		StmtCache.Empty(); // must not outlive the FSQLiteDatabase they were prepared against
		Db.Close();
	}
}

bool FGameIQStore::Exec(const TCHAR* Sql)
{
	if (!Db.Execute(Sql))
	{
		UE_LOG(LogGameIQStore, Error, TEXT("Game IQ: SQL failed: %s — %s"), Sql, *Db.GetLastError());
		return false;
	}
	return true;
}

bool FGameIQStore::BeginTransaction()
{
	// busy_timeout already waits 3s inside SQLite; retry a few times on top so a save-hook write
	// colliding with a long rebuild ingest degrades to "slightly later" instead of silent loss.
	for (int32 Attempt = 0; Attempt < 5; ++Attempt)
	{
		if (Db.Execute(TEXT("BEGIN IMMEDIATE;"))) { return true; }
		UE_LOG(LogGameIQStore, Warning, TEXT("Game IQ: BEGIN IMMEDIATE failed (attempt %d/5): %s"), Attempt + 1, *Db.GetLastError());
		FPlatformProcess::Sleep(0.2f * (Attempt + 1));
	}
	UE_LOG(LogGameIQStore, Error, TEXT("Game IQ: could not start a write transaction — the index was NOT updated."));
	return false;
}

FSQLitePreparedStatement& FGameIQStore::GetCachedStmt(const TCHAR* Sql)
{
	FSQLitePreparedStatement& Stmt = StmtCache.FindOrAdd(FString(Sql));
	if (!Stmt.IsValid())
	{
		if (!Stmt.Create(Db, Sql, ESQLitePreparedStatementFlags::Persistent))
		{
			UE_LOG(LogGameIQStore, Error, TEXT("Game IQ: failed to prepare: %s — %s"), Sql, *Db.GetLastError());
		}
	}
	return Stmt;
}

bool FGameIQStore::SchemaLooksValid()
{
	static const TCHAR* Required[] = { TEXT("meta"), TEXT("entities"), TEXT("edges"), TEXT("chunks"), TEXT("chunks_fts") };
	for (const TCHAR* Table : Required)
	{
		FSQLitePreparedStatement Stmt(Db, TEXT("SELECT 1 FROM sqlite_master WHERE name = ?"));
		if (!Stmt.IsValid()) { return false; }
		Stmt.SetBindingValueByIndex(1, FString(Table));
		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row)
		{
			UE_LOG(LogGameIQStore, Warning, TEXT("Game IQ: required table '%s' is missing."), Table);
			return false;
		}
	}
	return true;
}

bool FGameIQStore::EnsureSchema()
{
	int32 Version = 0;
	Db.GetUserVersion(Version);

	// A stamped version with missing tables means a past migration silently failed — report
	// inconsistent so Open() can rebuild instead of running queries against nothing.
	if (Version >= SchemaMigrations)
	{
		return SchemaLooksValid();
	}

	auto RunMigration = [this](const TCHAR* const* Statements, int32 Count, int32 TargetVersion) -> bool
	{
		for (int32 i = 0; i < Count; ++i)
		{
			if (!Exec(Statements[i]))
			{
				UE_LOG(LogGameIQStore, Error, TEXT("Game IQ: migration to schema v%d failed at statement %d."), TargetVersion, i + 1);
				return false;
			}
		}
		return Db.SetUserVersion(TargetVersion);
	};

	if (Version < 1 && !RunMigration(Migration1, UE_ARRAY_COUNT(Migration1), 1)) { return false; }
	if (Version < 2 && !RunMigration(Migration2, UE_ARRAY_COUNT(Migration2), 2)) { return false; }
	if (Version < 3 && !RunMigration(Migration3, UE_ARRAY_COUNT(Migration3), 3)) { return false; }
	if (Version < 4 && !RunMigration(Migration4, UE_ARRAY_COUNT(Migration4), 4)) { return false; }
	return SchemaLooksValid();
}

void FGameIQStore::InvalidateExtractCaches()
{
	// The DB was rebuilt: any "unchanged, carried forward" decision keyed on the old contents is
	// now wrong. Delete the extract hash caches and stamp a fresh generation so the extractors
	// fall back to a full pass.
	const FString ExtractDir = FPaths::Combine(FPaths::GetPath(OpenedPath), TEXT("extract"));
	IFileManager::Get().Delete(*FPaths::Combine(ExtractDir, TEXT("asset-hashes.json")), false, true, true);
	IFileManager::Get().Delete(*FPaths::Combine(ExtractDir, TEXT("blueprint-hashes.json")), false, true, true);
	SetMeta(TEXT("dbGeneration"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
}

void FGameIQStore::SetMeta(const FString& Key, const FString& Value)
{
	FSQLitePreparedStatement& Stmt = GetCachedStmt(
		TEXT("INSERT INTO meta (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
	if (!Stmt.IsValid()) { return; }
	Stmt.SetBindingValueByIndex(1, Key);
	Stmt.SetBindingValueByIndex(2, Value);
	Stmt.Execute();
}

FString FGameIQStore::GetMeta(const FString& Key)
{
	FString Value;
	FSQLitePreparedStatement& Stmt = GetCachedStmt(TEXT("SELECT value FROM meta WHERE key = ?"));
	if (!Stmt.IsValid()) { return Value; }
	Stmt.Reset();
	Stmt.SetBindingValueByIndex(1, Key);
	if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
	{
		Stmt.GetColumnValueByName(TEXT("value"), Value);
	}
	Stmt.Reset();
	return Value;
}

void FGameIQStore::GetCounts(int64& OutEntities, int64& OutEdges, int64& OutChunks)
{
	auto CountOf = [this](const TCHAR* Sql) -> int64
	{
		FSQLitePreparedStatement Stmt(Db, Sql);
		int64 N = 0;
		if (Stmt.IsValid() && Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, N);
		}
		return N;
	};
	OutEntities = CountOf(TEXT("SELECT COUNT(*) FROM entities"));
	OutEdges = CountOf(TEXT("SELECT COUNT(*) FROM edges"));
	OutChunks = CountOf(TEXT("SELECT COUNT(*) FROM chunks"));
}

int64 FGameIQStore::CountChunksForProducer(const FString& Producer)
{
	FSQLitePreparedStatement Stmt(Db, TEXT("SELECT COUNT(*) FROM chunks WHERE producer = ?"));
	int64 N = 0;
	if (Stmt.IsValid())
	{
		Stmt.SetBindingValueByIndex(1, Producer);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, N);
		}
	}
	return N;
}

void FGameIQStore::UpsertEntity(const FJsonObject& E, const FString& Producer)
{
	FSQLitePreparedStatement& Stmt = GetCachedStmt(
		TEXT("INSERT INTO entities (id, kind, name, path, parent, source, summary, detail, producer, authority) "
		     "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
		     "ON CONFLICT(id) DO UPDATE SET kind=excluded.kind, name=excluded.name, path=excluded.path, "
		     "parent=excluded.parent, source=excluded.source, summary=excluded.summary, "
		     "detail=excluded.detail, producer=excluded.producer, authority=excluded.authority"));
	if (!Stmt.IsValid()) { return; }
	Stmt.SetBindingValueByIndex(1, E.GetStringField(TEXT("id")));
	Stmt.SetBindingValueByIndex(2, E.GetStringField(TEXT("kind")));
	Stmt.SetBindingValueByIndex(3, E.GetStringField(TEXT("name")));
	Stmt.SetBindingValueByIndex(4, E.GetStringField(TEXT("path")));
	FString Parent;
	if (E.TryGetStringField(TEXT("parent"), Parent) && !Parent.IsEmpty()) { Stmt.SetBindingValueByIndex(5, Parent); }
	else { Stmt.SetBindingValueByIndex(5); } // NULL
	Stmt.SetBindingValueByIndex(6, E.GetStringField(TEXT("source")));
	FString Summary;
	if (E.TryGetStringField(TEXT("summary"), Summary) && !Summary.IsEmpty()) { Stmt.SetBindingValueByIndex(7, Summary); }
	else { Stmt.SetBindingValueByIndex(7); }
	const FString Detail = AsObject(E, TEXT("detail"));
	if (!Detail.IsEmpty()) { Stmt.SetBindingValueByIndex(8, Detail); }
	else { Stmt.SetBindingValueByIndex(8); }
	Stmt.SetBindingValueByIndex(9, Producer);
	// Provenance: a producer omits `authority` for extracted ground truth; docs set 'stated-intent'.
	FString AuthorityTag;
	if (!(E.TryGetStringField(TEXT("authority"), AuthorityTag) && !AuthorityTag.IsEmpty()))
	{
		AuthorityTag = GameIQ::Authority::ExtractedFact;
	}
	Stmt.SetBindingValueByIndex(10, AuthorityTag);
	Stmt.Execute();
}

void FGameIQStore::InsertEdge(const FJsonObject& E, const FString& Producer)
{
	FSQLitePreparedStatement& Stmt = GetCachedStmt(
		TEXT("INSERT INTO edges (src, dst, type, attrs, producer) VALUES (?, ?, ?, ?, ?) "
		     "ON CONFLICT(src, dst, type) DO UPDATE SET attrs=excluded.attrs, producer=excluded.producer"));
	if (!Stmt.IsValid()) { return; }
	Stmt.SetBindingValueByIndex(1, E.GetStringField(TEXT("src")));
	Stmt.SetBindingValueByIndex(2, E.GetStringField(TEXT("dst")));
	Stmt.SetBindingValueByIndex(3, E.GetStringField(TEXT("type")));
	const FString Attrs = AsObject(E, TEXT("attrs"));
	if (!Attrs.IsEmpty()) { Stmt.SetBindingValueByIndex(4, Attrs); }
	else { Stmt.SetBindingValueByIndex(4); }
	Stmt.SetBindingValueByIndex(5, Producer);
	Stmt.Execute();
}

void FGameIQStore::InsertChunk(const FJsonObject& C, const FString& Producer)
{
	const FString ChunkId = C.GetStringField(TEXT("id"));
	const FString EntityId = C.GetStringField(TEXT("entityId"));
	const FString Text = C.GetStringField(TEXT("text"));
	// FTS stays in sync via the chunks_fts_* triggers (migration 4) — no manual mirror writes.
	FSQLitePreparedStatement& Stmt = GetCachedStmt(
		TEXT("INSERT INTO chunks (id, entity_id, kind, text, producer, aux) VALUES (?, ?, ?, ?, ?, ?) "
		     "ON CONFLICT(id) DO UPDATE SET entity_id=excluded.entity_id, kind=excluded.kind, "
		     "text=excluded.text, producer=excluded.producer, aux=excluded.aux"));
	if (!Stmt.IsValid()) { return; }
	Stmt.SetBindingValueByIndex(1, ChunkId);
	Stmt.SetBindingValueByIndex(2, EntityId);
	Stmt.SetBindingValueByIndex(3, C.GetStringField(TEXT("kind")));
	Stmt.SetBindingValueByIndex(4, Text);
	Stmt.SetBindingValueByIndex(5, Producer);
	Stmt.SetBindingValueByIndex(6, GameIQText::BuildAuxTokens(EntityId, Text));
	Stmt.Execute();
}

void FGameIQStore::DeleteByProducer(const FString& Producer)
{
	// chunks first so the FTS delete triggers still see the rows; edges/entities have no FTS.
	for (const TCHAR* Sql : {
		TEXT("DELETE FROM chunks WHERE producer = ?"),
		TEXT("DELETE FROM edges WHERE producer = ?"),
		TEXT("DELETE FROM entities WHERE producer = ?") })
	{
		FSQLitePreparedStatement& Stmt = GetCachedStmt(Sql);
		if (Stmt.IsValid()) { Stmt.SetBindingValueByIndex(1, Producer); Stmt.Execute(); }
	}
}

TArray<FString> FGameIQStore::CollectSubtreeIds(const FString& RootId)
{
	TArray<FString> Ids;
	TSet<FString> Seen;
	Ids.Add(RootId);
	Seen.Add(RootId);
	TArray<FString> Frontier = { RootId };
	while (Frontier.Num() > 0)
	{
		TArray<FString> Next;
		for (const FString& P : Frontier)
		{
			FSQLitePreparedStatement& Stmt = GetCachedStmt(TEXT("SELECT id FROM entities WHERE parent = ?"));
			if (!Stmt.IsValid()) { continue; }
			Stmt.Reset(); // manual Step() loop doesn't auto-reset like Execute() does — must rewind before reuse
			Stmt.SetBindingValueByIndex(1, P);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString Kid;
				Stmt.GetColumnValueByName(TEXT("id"), Kid);
				if (!Seen.Contains(Kid)) { Seen.Add(Kid); Ids.Add(Kid); Next.Add(Kid); }
			}
		}
		Frontier = MoveTemp(Next);
	}
	return Ids;
}

void FGameIQStore::DeleteSubtree(const FString& RootId)
{
	for (const FString& Id : CollectSubtreeIds(RootId))
	{
		for (const TCHAR* Sql : {
			TEXT("DELETE FROM chunks WHERE entity_id = ?"),
			TEXT("DELETE FROM edges WHERE src = ?"),
			TEXT("DELETE FROM entities WHERE id = ?") })
		{
			FSQLitePreparedStatement& Stmt = GetCachedStmt(Sql);
			if (Stmt.IsValid()) { Stmt.SetBindingValueByIndex(1, Id); Stmt.Execute(); }
		}
	}
}

void FGameIQStore::DeleteSubtreeForProducer(const FString& RootId, const FString& Producer)
{
	// Producer-scoped: leaves e.g. the registry's root entity + depends-on edges in place while
	// replacing only what this producer previously said about the asset.
	for (const FString& Id : CollectSubtreeIds(RootId))
	{
		for (const TCHAR* Sql : {
			TEXT("DELETE FROM chunks WHERE entity_id = ? AND producer = ?"),
			TEXT("DELETE FROM edges WHERE src = ? AND producer = ?"),
			TEXT("DELETE FROM entities WHERE id = ? AND producer = ?") })
		{
			FSQLitePreparedStatement& Stmt = GetCachedStmt(Sql);
			if (Stmt.IsValid())
			{
				Stmt.SetBindingValueByIndex(1, Id);
				Stmt.SetBindingValueByIndex(2, Producer);
				Stmt.Execute();
			}
		}
	}
}

void FGameIQStore::InsertRows(
	const TArray<TSharedPtr<FJsonValue>>& Entities,
	const TArray<TSharedPtr<FJsonValue>>& Edges,
	const TArray<TSharedPtr<FJsonValue>>& Chunks,
	const FString& Producer)
{
	for (const TSharedPtr<FJsonValue>& V : Entities) { if (const FJsonObject* E = AsObjectPtr(V)) { UpsertEntity(*E, Producer); } }
	for (const TSharedPtr<FJsonValue>& V : Edges) { if (const FJsonObject* E = AsObjectPtr(V)) { InsertEdge(*E, Producer); } }
	for (const TSharedPtr<FJsonValue>& V : Chunks) { if (const FJsonObject* C = AsObjectPtr(V)) { InsertChunk(*C, Producer); } }
}

bool FGameIQStore::Patch(
	const TArray<FString>& Replaces,
	const TArray<TSharedPtr<FJsonValue>>& Entities,
	const TArray<TSharedPtr<FJsonValue>>& Edges,
	const TArray<TSharedPtr<FJsonValue>>& Chunks,
	const FString& Producer)
{
	if (!Db.IsValid()) { return false; }

	// Roots: explicit `Replaces`, else every top-level (parent-less) entity in the delta.
	TArray<FString> Roots = Replaces;
	if (Roots.Num() == 0)
	{
		for (const TSharedPtr<FJsonValue>& V : Entities)
		{
			if (const FJsonObject* E = AsObjectPtr(V))
			{
				FString Parent;
				if (!(E->TryGetStringField(TEXT("parent"), Parent) && !Parent.IsEmpty()))
				{
					Roots.Add(E->GetStringField(TEXT("id")));
				}
			}
		}
	}

	if (!BeginTransaction()) { return false; }
	for (const FString& Root : Roots) { DeleteSubtree(Root); }
	InsertRows(Entities, Edges, Chunks, Producer);
	SetMeta(TEXT("lastIngestAtIso"), FDateTime::UtcNow().ToIso8601());
	return Exec(TEXT("COMMIT;"));
}

bool FGameIQStore::PatchProducerScoped(
	const TArray<FString>& Replaces,
	const TArray<TSharedPtr<FJsonValue>>& Entities,
	const TArray<TSharedPtr<FJsonValue>>& Edges,
	const TArray<TSharedPtr<FJsonValue>>& Chunks,
	const FString& Producer)
{
	if (!Db.IsValid()) { return false; }
	if (!BeginTransaction()) { return false; }
	for (const FString& Root : Replaces) { DeleteSubtreeForProducer(Root, Producer); }
	InsertRows(Entities, Edges, Chunks, Producer);
	SetMeta(TEXT("lastIngestAtIso"), FDateTime::UtcNow().ToIso8601());
	return Exec(TEXT("COMMIT;"));
}

bool FGameIQStore::IngestProducer(
	const FString& Producer,
	const TArray<TSharedPtr<FJsonValue>>& Entities,
	const TArray<TSharedPtr<FJsonValue>>& Edges,
	const TArray<TSharedPtr<FJsonValue>>& Chunks)
{
	if (!Db.IsValid()) { return false; }
	if (!BeginTransaction()) { return false; }
	DeleteByProducer(Producer);
	InsertRows(Entities, Edges, Chunks, Producer);
	SetMeta(TEXT("lastIngestAtIso"), FDateTime::UtcNow().ToIso8601());
	return Exec(TEXT("COMMIT;"));
}

void FGameIQStore::SetProjectMeta(const FString& Name, const FString& Root, const FString& GeneratedAtIso)
{
	if (!Db.IsValid()) { return; }
	SetMeta(TEXT("projectName"), Name);
	SetMeta(TEXT("projectRoot"), Root);
	SetMeta(TEXT("lastGeneratedAtIso"), GeneratedAtIso);
	SetMeta(TEXT("lastIngestAtIso"), FDateTime::UtcNow().ToIso8601());
}
