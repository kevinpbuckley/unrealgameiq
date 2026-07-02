// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQQuery.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SQLiteDatabase.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQQuery, Log, All);

namespace
{
	FString IndexDbPath()
	{
		// ProjectDir() is relative to the engine binary at runtime; sqlite resolves against the
		// process cwd, so hand it an absolute path.
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		return FPaths::Combine(Root, TEXT(".gameiq"), TEXT("index.db"));
	}

	/** Open the index read-only-in-spirit. ReadWrite (not Create) copes with WAL's shared-memory
	 *  file better than a pure read-only handle while Node holds a writer; we only issue SELECTs.
	 *  On failure, `OutError` explains why (missing file vs open failure) for the JSON response. */
	bool OpenIndex(FSQLiteDatabase& Db, FString& OutError)
	{
		const FString Path = IndexDbPath();
		if (!FPaths::FileExists(Path))
		{
			OutError = FString::Printf(TEXT("index not found at %s — run `gameiq index` first"), *Path);
			UE_LOG(LogGameIQQuery, Warning, TEXT("Game IQ: %s"), *OutError);
			return false;
		}
		// The DB is WAL-mode and may be held by the Node MCP server; try read-only first (correct for
		// our SELECT-only use), then read-write. Surface the sqlite error if both fail.
		if (Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadOnly)) { return true; }
		const FString RoErr = Db.GetLastError();
		if (Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadWrite)) { return true; }
		OutError = FString::Printf(TEXT("failed to open index %s (ro: %s; rw: %s)"),
			*Path, *RoErr, *Db.GetLastError());
		UE_LOG(LogGameIQQuery, Warning, TEXT("Game IQ: %s"), *OutError);
		return false;
	}

	FString ColStr(const FSQLitePreparedStatement& Stmt, const TCHAR* Name)
	{
		FString V;
		Stmt.GetColumnValueByName(Name, V);
		return V;
	}

	/** A JSON string column is stored as text; re-parse it to an object, or null / raw string on failure. */
	TSharedPtr<FJsonValue> ParseJsonColumn(const FString& Raw)
	{
		if (Raw.IsEmpty())
		{
			return MakeShared<FJsonValueNull>();
		}
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid())
		{
			return MakeShared<FJsonValueObject>(Obj);
		}
		return MakeShared<FJsonValueString>(Raw);
	}

	TSharedPtr<FJsonValue> NullOrString(const FString& S)
	{
		return S.IsEmpty() ? StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueNull>())
		                   : StaticCastSharedRef<FJsonValue>(MakeShared<FJsonValueString>(S));
	}

	/** Build the entity object from a statement positioned on an `entities` row (SELECT *). */
	TSharedRef<FJsonObject> EntityJson(const FSQLitePreparedStatement& Stmt)
	{
		TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
		E->SetStringField(TEXT("id"), ColStr(Stmt, TEXT("id")));
		E->SetStringField(TEXT("kind"), ColStr(Stmt, TEXT("kind")));
		E->SetStringField(TEXT("name"), ColStr(Stmt, TEXT("name")));
		E->SetStringField(TEXT("path"), ColStr(Stmt, TEXT("path")));
		E->SetField(TEXT("parent"), NullOrString(ColStr(Stmt, TEXT("parent"))));
		E->SetStringField(TEXT("source"), ColStr(Stmt, TEXT("source")));
		E->SetField(TEXT("summary"), NullOrString(ColStr(Stmt, TEXT("summary"))));
		E->SetField(TEXT("detail"), ParseJsonColumn(ColStr(Stmt, TEXT("detail"))));
		return E;
	}

	int32 CountByIdColumn(FSQLiteDatabase& Db, const TCHAR* Sql, const FString& Id)
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(Sql);
		if (!Stmt.IsValid()) { return 0; }
		Stmt.SetBindingValueByIndex(1, Id);
		int32 N = 0;
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByName(TEXT("n"), N);
		}
		return N;
	}

	/** Index-age stamp (design §8), mirrored from the meta table so agents know staleness. */
	TSharedRef<FJsonObject> IndexNote(FSQLiteDatabase& Db)
	{
		TSharedRef<FJsonObject> Note = MakeShared<FJsonObject>();
		auto Meta = [&Db](const TCHAR* Key) -> FString
		{
			FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("SELECT value FROM meta WHERE key = ?"));
			if (!Stmt.IsValid()) { return FString(); }
			Stmt.SetBindingValueByIndex(1, FString(Key));
			FString V;
			if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row) { Stmt.GetColumnValueByName(TEXT("value"), V); }
			return V;
		};
		Note->SetField(TEXT("lastIngestAtIso"), NullOrString(Meta(TEXT("lastIngestAtIso"))));
		Note->SetField(TEXT("lastGeneratedAtIso"), NullOrString(Meta(TEXT("lastGeneratedAtIso"))));
		Note->SetField(TEXT("projectName"), NullOrString(Meta(TEXT("projectName"))));
		return Note;
	}

	FString Serialize(const TSharedRef<FJsonObject>& Root)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return Out;
	}

	/** Wrap a payload as the MCP server does: {"result": payload, "index": ageNote}. */
	FString Envelope(FSQLiteDatabase& Db, const TSharedRef<FJsonValue>& Payload)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetField(TEXT("result"), Payload);
		Root->SetObjectField(TEXT("index"), IndexNote(Db));
		return Serialize(Root);
	}

	FString ErrorJson(const FString& Message)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("error"), Message);
		return Serialize(Root);
	}

	/** Mirror of query/fts.ts toFtsQuery: word tokens, each quoted, OR-joined. Empty if none. */
	FString BuildFtsQuery(const FString& Input)
	{
		FString Out;
		FString Cur;
		auto Flush = [&Out, &Cur]()
		{
			if (Cur.Len() > 0)
			{
				if (Out.Len() > 0) { Out += TEXT(" OR "); }
				Out += TEXT("\"") + Cur + TEXT("\"");
				Cur.Reset();
			}
		};
		for (const TCHAR C : Input)
		{
			if (FChar::IsAlnum(C) || C == TEXT('_')) { Cur.AppendChar(C); }
			else { Flush(); }
		}
		Flush();
		return Out;
	}

	/** Load one entity by id into a JSON object, or null if absent. */
	TSharedPtr<FJsonObject> LoadEntity(FSQLiteDatabase& Db, const FString& Id)
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("SELECT * FROM entities WHERE id = ?"));
		if (!Stmt.IsValid()) { return nullptr; }
		Stmt.SetBindingValueByIndex(1, Id);
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			return EntityJson(Stmt);
		}
		return nullptr;
	}
}

FString GameIQQuery::Search(const FString& Query, const FString& Kind, int32 Limit)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); }; // FSQLiteDatabase asserts if destroyed while open

	const FString Fts = BuildFtsQuery(Query);
	TArray<TSharedPtr<FJsonValue>> Results;
	if (!Fts.IsEmpty())
	{
		// Best hit per entity, ranked by bm25 (lower is better) — mirrors QueryEngine.searchProject.
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(
			TEXT("SELECT f.chunk_id AS chunkId, f.entity_id AS entityId, c.kind AS kind, "
			     "bm25(chunks_fts) AS score, snippet(chunks_fts, 3, '[', ']', ' \xE2\x80\xA6 ', 12) AS snippet "
			     "FROM chunks_fts f JOIN chunks c ON c.id = f.chunk_id "
			     "WHERE chunks_fts MATCH ? ORDER BY score LIMIT ?"));
		if (Stmt.IsValid())
		{
			Stmt.SetBindingValueByIndex(1, Fts);
			Stmt.SetBindingValueByIndex(2, Limit * 5); // over-fetch so dedupe/kind-filter still fills the page

			struct FHit { TSharedRef<FJsonObject> Entity; double Score; FString Snippet; FString ChunkKind; };
			TMap<FString, FHit> BestByEntity;
			TArray<FString> Order;

			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				const FString EntityId = ColStr(Stmt, TEXT("entityId"));
				double Score = 0.0;
				Stmt.GetColumnValueByName(TEXT("score"), Score);
				const FString Snippet = ColStr(Stmt, TEXT("snippet"));
				const FString ChunkKind = ColStr(Stmt, TEXT("kind"));

				const FHit* Existing = BestByEntity.Find(EntityId);
				if (Existing && Existing->Score <= Score) { continue; }

				TSharedPtr<FJsonObject> Entity = LoadEntity(Db, EntityId);
				if (!Entity.IsValid()) { continue; }
				if (!Kind.IsEmpty() && Entity->GetStringField(TEXT("kind")) != Kind) { continue; }

				if (!Existing) { Order.Add(EntityId); }
				BestByEntity.Add(EntityId, FHit{ Entity.ToSharedRef(), Score, Snippet, ChunkKind });
			}

			Order.Sort([&BestByEntity](const FString& A, const FString& B)
			{
				return BestByEntity[A].Score < BestByEntity[B].Score;
			});

			for (const FString& EntityId : Order)
			{
				if (Results.Num() >= Limit) { break; }
				const FHit& H = BestByEntity[EntityId];
				TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
				R->SetObjectField(TEXT("entity"), H.Entity);
				R->SetNumberField(TEXT("score"), H.Score);
				R->SetStringField(TEXT("snippet"), H.Snippet);
				R->SetStringField(TEXT("matchedChunkKind"), H.ChunkKind);
				Results.Add(MakeShared<FJsonValueObject>(R));
			}
		}
	}

	return Envelope(Db, MakeShared<FJsonValueArray>(Results));
}

FString GameIQQuery::GetEntity(const FString& Id, int32 Cap)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); }; // FSQLiteDatabase asserts if destroyed while open

	TSharedPtr<FJsonObject> Entity = LoadEntity(Db, Id);
	if (!Entity.IsValid())
	{
		TSharedRef<FJsonObject> NotFound = MakeShared<FJsonObject>();
		NotFound->SetStringField(TEXT("error"), FString::Printf(TEXT("entity not found: %s"), *Id));
		return Envelope(Db, MakeShared<FJsonValueObject>(NotFound));
	}

	// chunks (capped)
	TArray<TSharedPtr<FJsonValue>> Chunks;
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("SELECT id, kind, text FROM chunks WHERE entity_id = ? LIMIT ?"));
		if (Stmt.IsValid())
		{
			Stmt.SetBindingValueByIndex(1, Id);
			Stmt.SetBindingValueByIndex(2, Cap);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
				C->SetStringField(TEXT("id"), ColStr(Stmt, TEXT("id")));
				C->SetStringField(TEXT("kind"), ColStr(Stmt, TEXT("kind")));
				C->SetStringField(TEXT("text"), ColStr(Stmt, TEXT("text")));
				Chunks.Add(MakeShared<FJsonValueObject>(C));
			}
		}
	}

	// edges (capped) — helper to read src/dst/type/attrs for a bound column
	auto ReadEdges = [&Db, &Id, Cap](const TCHAR* Sql) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(Sql);
		if (Stmt.IsValid())
		{
			Stmt.SetBindingValueByIndex(1, Id);
			Stmt.SetBindingValueByIndex(2, Cap);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				TSharedRef<FJsonObject> Ed = MakeShared<FJsonObject>();
				Ed->SetStringField(TEXT("src"), ColStr(Stmt, TEXT("src")));
				Ed->SetStringField(TEXT("dst"), ColStr(Stmt, TEXT("dst")));
				Ed->SetStringField(TEXT("type"), ColStr(Stmt, TEXT("type")));
				Ed->SetField(TEXT("attrs"), ParseJsonColumn(ColStr(Stmt, TEXT("attrs"))));
				Out.Add(MakeShared<FJsonValueObject>(Ed));
			}
		}
		return Out;
	};
	TArray<TSharedPtr<FJsonValue>> Outgoing = ReadEdges(TEXT("SELECT * FROM edges WHERE src = ? LIMIT ?"));
	TArray<TSharedPtr<FJsonValue>> Incoming = ReadEdges(TEXT("SELECT * FROM edges WHERE dst = ? LIMIT ?"));

	// children (capped)
	TArray<TSharedPtr<FJsonValue>> Children;
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("SELECT * FROM entities WHERE parent = ? LIMIT ?"));
		if (Stmt.IsValid())
		{
			Stmt.SetBindingValueByIndex(1, Id);
			Stmt.SetBindingValueByIndex(2, Cap);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				Children.Add(MakeShared<FJsonValueObject>(EntityJson(Stmt)));
			}
		}
	}

	const int32 OutCount = CountByIdColumn(Db, TEXT("SELECT COUNT(*) AS n FROM edges WHERE src = ?"), Id);
	const int32 InCount = CountByIdColumn(Db, TEXT("SELECT COUNT(*) AS n FROM edges WHERE dst = ?"), Id);
	const int32 ChildCount = CountByIdColumn(Db, TEXT("SELECT COUNT(*) AS n FROM entities WHERE parent = ?"), Id);
	const int32 ChunkCount = CountByIdColumn(Db, TEXT("SELECT COUNT(*) AS n FROM chunks WHERE entity_id = ?"), Id);

	TSharedRef<FJsonObject> Counts = MakeShared<FJsonObject>();
	Counts->SetNumberField(TEXT("outgoing"), OutCount);
	Counts->SetNumberField(TEXT("incoming"), InCount);
	Counts->SetNumberField(TEXT("children"), ChildCount);
	Counts->SetNumberField(TEXT("chunks"), ChunkCount);

	TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
	Detail->SetObjectField(TEXT("entity"), Entity.ToSharedRef());
	Detail->SetArrayField(TEXT("outgoing"), Outgoing);
	Detail->SetArrayField(TEXT("incoming"), Incoming);
	Detail->SetArrayField(TEXT("children"), Children);
	Detail->SetArrayField(TEXT("chunks"), Chunks);
	Detail->SetObjectField(TEXT("counts"), Counts);
	Detail->SetBoolField(TEXT("truncated"),
		OutCount > Cap || InCount > Cap || ChildCount > Cap || ChunkCount > Cap);

	return Envelope(Db, MakeShared<FJsonValueObject>(Detail));
}
