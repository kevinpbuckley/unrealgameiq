// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQStore.h"

#include "Dom/JsonObject.h"
#include "GameIQJson.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQStore, Log, All);

namespace
{
	// Schema in lockstep with packages/core/src/store/store.ts MIGRATIONS. Applied by user_version.
	const TCHAR* Migration1 = TEXT(
		"CREATE TABLE meta (key TEXT PRIMARY KEY, value TEXT NOT NULL);"
		"CREATE TABLE entities (id TEXT PRIMARY KEY, kind TEXT NOT NULL, name TEXT NOT NULL, path TEXT NOT NULL, parent TEXT, source TEXT NOT NULL, summary TEXT, detail TEXT);"
		"CREATE INDEX idx_entities_kind ON entities(kind);"
		"CREATE INDEX idx_entities_name ON entities(name);"
		"CREATE INDEX idx_entities_parent ON entities(parent);"
		"CREATE TABLE edges (src TEXT NOT NULL, dst TEXT NOT NULL, type TEXT NOT NULL, attrs TEXT, PRIMARY KEY (src, dst, type));"
		"CREATE INDEX idx_edges_src ON edges(src);"
		"CREATE INDEX idx_edges_dst ON edges(dst);"
		"CREATE TABLE chunks (id TEXT PRIMARY KEY, entity_id TEXT NOT NULL, kind TEXT NOT NULL, text TEXT NOT NULL);"
		"CREATE INDEX idx_chunks_entity ON chunks(entity_id);"
		"CREATE VIRTUAL TABLE chunks_fts USING fts5(chunk_id UNINDEXED, entity_id UNINDEXED, text);");

	const TCHAR* Migration2 = TEXT(
		"ALTER TABLE entities ADD COLUMN producer TEXT;"
		"ALTER TABLE edges ADD COLUMN producer TEXT;"
		"ALTER TABLE chunks ADD COLUMN producer TEXT;"
		"CREATE INDEX idx_entities_producer ON entities(producer);"
		"CREATE INDEX idx_edges_producer ON edges(producer);"
		"CREATE INDEX idx_chunks_producer ON chunks(producer);"
		"DROP TABLE chunks_fts;"
		"CREATE VIRTUAL TABLE chunks_fts USING fts5(chunk_id UNINDEXED, entity_id UNINDEXED, producer UNINDEXED, text);"
		"INSERT INTO chunks_fts(chunk_id, entity_id, producer, text) SELECT id, entity_id, producer, text FROM chunks;");

	// Provenance/authority (issue #3): every entity is either extracted ground truth or stated design
	// intent. NULL after the ALTER; ingest re-inserts every row with a concrete value, and the query
	// layer COALESCEs any residual NULL to 'extracted-fact'.
	const TCHAR* Migration3 = TEXT(
		"ALTER TABLE entities ADD COLUMN authority TEXT;"
		"CREATE INDEX idx_entities_authority ON entities(authority);");

	constexpr int32 SchemaMigrations = 3; // C++ store schema version (TS store retired)

	FString StoreDbPath()
	{
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		return FPaths::Combine(Root, TEXT(".gameiq"), TEXT("index.db"));
	}

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

bool FGameIQStore::Open()
{
	const FString Path = StoreDbPath();
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
	if (!Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadWriteCreate))
	{
		UE_LOG(LogGameIQStore, Warning, TEXT("Game IQ: cannot open index for write at %s: %s"), *Path, *Db.GetLastError());
		return false;
	}
	// Match the TS store: rollback journal (no WAL -shm, so cross-library readers coexist on Windows).
	Db.Execute(TEXT("PRAGMA journal_mode=DELETE;"));
	Db.Execute(TEXT("PRAGMA synchronous=NORMAL;"));
	Db.Execute(TEXT("PRAGMA busy_timeout=3000;"));
	Db.Execute(TEXT("PRAGMA foreign_keys=OFF;"));
	EnsureSchema();
	return true;
}

void FGameIQStore::Close()
{
	if (Db.IsValid())
	{
		Db.Close();
	}
}

void FGameIQStore::EnsureSchema()
{
	int32 Version = 0;
	Db.GetUserVersion(Version);
	if (Version < 1) { Db.Execute(Migration1); }
	if (Version < 2) { Db.Execute(Migration2); }
	if (Version < 3) { Db.Execute(Migration3); }
	if (Version < SchemaMigrations) { Db.SetUserVersion(SchemaMigrations); }
}

void FGameIQStore::SetMeta(const FString& Key, const FString& Value)
{
	FSQLitePreparedStatement Stmt = Db.PrepareStatement(
		TEXT("INSERT INTO meta (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value"));
	if (!Stmt.IsValid()) { return; }
	Stmt.SetBindingValueByIndex(1, Key);
	Stmt.SetBindingValueByIndex(2, Value);
	Stmt.Execute();
}

void FGameIQStore::UpsertEntity(const FJsonObject& E, const FString& Producer)
{
	FSQLitePreparedStatement Stmt = Db.PrepareStatement(
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
	FSQLitePreparedStatement Stmt = Db.PrepareStatement(
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
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(
			TEXT("INSERT INTO chunks (id, entity_id, kind, text, producer) VALUES (?, ?, ?, ?, ?) "
			     "ON CONFLICT(id) DO UPDATE SET entity_id=excluded.entity_id, kind=excluded.kind, "
			     "text=excluded.text, producer=excluded.producer"));
		if (!Stmt.IsValid()) { return; }
		Stmt.SetBindingValueByIndex(1, ChunkId);
		Stmt.SetBindingValueByIndex(2, EntityId);
		Stmt.SetBindingValueByIndex(3, C.GetStringField(TEXT("kind")));
		Stmt.SetBindingValueByIndex(4, C.GetStringField(TEXT("text")));
		Stmt.SetBindingValueByIndex(5, Producer);
		Stmt.Execute();
	}
	// keep FTS in sync (delete any prior row for this chunk id, then insert)
	{
		FSQLitePreparedStatement Del = Db.PrepareStatement(TEXT("DELETE FROM chunks_fts WHERE chunk_id = ?"));
		if (Del.IsValid()) { Del.SetBindingValueByIndex(1, ChunkId); Del.Execute(); }
	}
	{
		FSQLitePreparedStatement Ins = Db.PrepareStatement(
			TEXT("INSERT INTO chunks_fts (chunk_id, entity_id, producer, text) VALUES (?, ?, ?, ?)"));
		if (Ins.IsValid())
		{
			Ins.SetBindingValueByIndex(1, ChunkId);
			Ins.SetBindingValueByIndex(2, EntityId);
			Ins.SetBindingValueByIndex(3, Producer);
			Ins.SetBindingValueByIndex(4, C.GetStringField(TEXT("text")));
			Ins.Execute();
		}
	}
}

void FGameIQStore::DeleteByProducer(const FString& Producer)
{
	for (const TCHAR* Sql : {
		TEXT("DELETE FROM chunks_fts WHERE producer = ?"),
		TEXT("DELETE FROM chunks WHERE producer = ?"),
		TEXT("DELETE FROM edges WHERE producer = ?"),
		TEXT("DELETE FROM entities WHERE producer = ?") })
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(Sql);
		if (Stmt.IsValid()) { Stmt.SetBindingValueByIndex(1, Producer); Stmt.Execute(); }
	}
}

void FGameIQStore::DeleteSubtree(const FString& RootId)
{
	// Collect the root + all descendants (by parent), then delete their rows + owned edges/chunks.
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
			FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("SELECT id FROM entities WHERE parent = ?"));
			if (!Stmt.IsValid()) { continue; }
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

	for (const FString& Id : Ids)
	{
		for (const TCHAR* Sql : {
			TEXT("DELETE FROM chunks_fts WHERE entity_id = ?"),
			TEXT("DELETE FROM chunks WHERE entity_id = ?"),
			TEXT("DELETE FROM edges WHERE src = ?"),
			TEXT("DELETE FROM entities WHERE id = ?") })
		{
			FSQLitePreparedStatement Stmt = Db.PrepareStatement(Sql);
			if (Stmt.IsValid()) { Stmt.SetBindingValueByIndex(1, Id); Stmt.Execute(); }
		}
	}
}

void FGameIQStore::Patch(
	const TArray<FString>& Replaces,
	const TArray<TSharedPtr<FJsonValue>>& Entities,
	const TArray<TSharedPtr<FJsonValue>>& Edges,
	const TArray<TSharedPtr<FJsonValue>>& Chunks,
	const FString& Producer)
{
	if (!Db.IsValid()) { return; }

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

	Db.Execute(TEXT("BEGIN IMMEDIATE;"));
	for (const FString& Root : Roots) { DeleteSubtree(Root); }
	for (const TSharedPtr<FJsonValue>& V : Entities) { if (const FJsonObject* E = AsObjectPtr(V)) { UpsertEntity(*E, Producer); } }
	for (const TSharedPtr<FJsonValue>& V : Edges) { if (const FJsonObject* E = AsObjectPtr(V)) { InsertEdge(*E, Producer); } }
	for (const TSharedPtr<FJsonValue>& V : Chunks) { if (const FJsonObject* C = AsObjectPtr(V)) { InsertChunk(*C, Producer); } }
	SetMeta(TEXT("lastIngestAtIso"), FDateTime::UtcNow().ToIso8601());
	Db.Execute(TEXT("COMMIT;"));
}

void FGameIQStore::IngestProducer(
	const FString& Producer,
	const TArray<TSharedPtr<FJsonValue>>& Entities,
	const TArray<TSharedPtr<FJsonValue>>& Edges,
	const TArray<TSharedPtr<FJsonValue>>& Chunks)
{
	if (!Db.IsValid()) { return; }
	Db.Execute(TEXT("BEGIN IMMEDIATE;"));
	DeleteByProducer(Producer);
	for (const TSharedPtr<FJsonValue>& V : Entities) { if (const FJsonObject* E = AsObjectPtr(V)) { UpsertEntity(*E, Producer); } }
	for (const TSharedPtr<FJsonValue>& V : Edges) { if (const FJsonObject* E = AsObjectPtr(V)) { InsertEdge(*E, Producer); } }
	for (const TSharedPtr<FJsonValue>& V : Chunks) { if (const FJsonObject* C = AsObjectPtr(V)) { InsertChunk(*C, Producer); } }
	SetMeta(TEXT("lastIngestAtIso"), FDateTime::UtcNow().ToIso8601());
	Db.Execute(TEXT("COMMIT;"));
}

void FGameIQStore::SetProjectMeta(const FString& Name, const FString& Root, const FString& GeneratedAtIso)
{
	if (!Db.IsValid()) { return; }
	SetMeta(TEXT("projectName"), Name);
	SetMeta(TEXT("projectRoot"), Root);
	SetMeta(TEXT("lastGeneratedAtIso"), GeneratedAtIso);
	SetMeta(TEXT("lastIngestAtIso"), FDateTime::UtcNow().ToIso8601());
}
