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

	/** Open the index read-only-in-spirit. The DB is a rollback-journal file (not WAL), so a
	 *  read-only handle from UE's SQLite coexists with the Node writer. Falls back to read-write.
	 *  On failure, `OutError` explains why for the JSON response. */
	bool OpenIndex(FSQLiteDatabase& Db, FString& OutError)
	{
		const FString Path = IndexDbPath();
		if (!FPaths::FileExists(Path))
		{
			OutError = FString::Printf(TEXT("index not found at %s — run `gameiq index` first"), *Path);
			UE_LOG(LogGameIQQuery, Warning, TEXT("Game IQ: %s"), *OutError);
			return false;
		}
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
		// Provenance (issue #3): always present so an agent reading only the JSON can tell stated
		// design intent from extracted ground truth. Residual NULLs (pre-migration rows) read as fact.
		const FString AuthorityTag = ColStr(Stmt, TEXT("authority"));
		E->SetStringField(TEXT("authority"), AuthorityTag.IsEmpty() ? TEXT("extracted-fact") : AuthorityTag);
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

	FString MetaValue(FSQLiteDatabase& Db, const TCHAR* Key)
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("SELECT value FROM meta WHERE key = ?"));
		if (!Stmt.IsValid()) { return FString(); }
		Stmt.SetBindingValueByIndex(1, FString(Key));
		FString V;
		if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row) { Stmt.GetColumnValueByName(TEXT("value"), V); }
		return V;
	}

	/** Index-age stamp (design §8), mirrored from the meta table so agents know staleness. */
	TSharedRef<FJsonObject> IndexNote(FSQLiteDatabase& Db)
	{
		TSharedRef<FJsonObject> Note = MakeShared<FJsonObject>();
		Note->SetField(TEXT("lastIngestAtIso"), NullOrString(MetaValue(Db, TEXT("lastIngestAtIso"))));
		Note->SetField(TEXT("lastGeneratedAtIso"), NullOrString(MetaValue(Db, TEXT("lastGeneratedAtIso"))));
		Note->SetField(TEXT("projectName"), NullOrString(MetaValue(Db, TEXT("projectName"))));
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

	bool EntityExists(FSQLiteDatabase& Db, const FString& Id)
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("SELECT 1 FROM entities WHERE id = ?"));
		if (!Stmt.IsValid()) { return false; }
		Stmt.SetBindingValueByIndex(1, Id);
		return Stmt.Step() == ESQLitePreparedStatementStepResult::Row;
	}

	struct FRawEdge { FString Src; FString Dst; FString Type; };

	/** Raw edges for traversal (src/dst/type only). bOutgoing => WHERE src=Id, else WHERE dst=Id. */
	TArray<FRawEdge> ReadRawEdges(FSQLiteDatabase& Db, const FString& Id, bool bOutgoing)
	{
		TArray<FRawEdge> Out;
		const TCHAR* Sql = bOutgoing
			? TEXT("SELECT src, dst, type FROM edges WHERE src = ?")
			: TEXT("SELECT src, dst, type FROM edges WHERE dst = ?");
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(Sql);
		if (!Stmt.IsValid()) { return Out; }
		Stmt.SetBindingValueByIndex(1, Id);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Out.Add(FRawEdge{ ColStr(Stmt, TEXT("src")), ColStr(Stmt, TEXT("dst")), ColStr(Stmt, TEXT("type")) });
		}
		return Out;
	}

	/** Severity per edge type (mirror of query.ts SEVERITY): heavier = worse to break. */
	int32 EdgeSeverity(const FString& Type)
	{
		static const TMap<FString, int32> Severity = {
			{ TEXT("inherits"), 10 }, { TEXT("implements"), 9 }, { TEXT("calls"), 8 },
			{ TEXT("uses-skeleton"), 8 }, { TEXT("uses-material"), 7 }, { TEXT("uses-texture"), 5 },
			{ TEXT("overrides-parameter"), 6 }, { TEXT("casts-to"), 6 }, { TEXT("placed-in-level"), 5 },
			{ TEXT("plays-on"), 4 }, { TEXT("depends-on"), 4 }, { TEXT("references"), 3 },
			{ TEXT("describes"), 2 }, { TEXT("constrains"), 2 }, { TEXT("illustrates"), 2 },
		};
		const int32* Found = Severity.Find(Type);
		return Found ? *Found : 1;
	}

	// ---- structured builders (shared so `explain` can reuse search + references) --------------

	struct FSearchHit { TSharedRef<FJsonObject> Entity; double Score; FString Snippet; FString ChunkKind; };

	TArray<FSearchHit> BuildSearchHits(FSQLiteDatabase& Db, const FString& Query, const FString& Kind, int32 Limit)
	{
		TArray<FSearchHit> Ordered;
		const FString Fts = BuildFtsQuery(Query);
		if (Fts.IsEmpty()) { return Ordered; }

		FSQLitePreparedStatement Stmt = Db.PrepareStatement(
			TEXT("SELECT f.chunk_id AS chunkId, f.entity_id AS entityId, c.kind AS kind, "
			     "bm25(chunks_fts) AS score, snippet(chunks_fts, 3, '[', ']', ' \xE2\x80\xA6 ', 12) AS snippet "
			     "FROM chunks_fts f JOIN chunks c ON c.id = f.chunk_id "
			     "WHERE chunks_fts MATCH ? ORDER BY score LIMIT ?"));
		if (!Stmt.IsValid()) { return Ordered; }
		Stmt.SetBindingValueByIndex(1, Fts);
		Stmt.SetBindingValueByIndex(2, Limit * 5); // over-fetch so dedupe/kind-filter still fills the page

		TMap<FString, int32> IndexByEntity; // entityId -> position in Ordered
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			const FString EntityId = ColStr(Stmt, TEXT("entityId"));
			double Score = 0.0;
			Stmt.GetColumnValueByName(TEXT("score"), Score);

			if (const int32* Pos = IndexByEntity.Find(EntityId))
			{
				if (Ordered[*Pos].Score <= Score) { continue; } // keep best (lowest) bm25
			}
			TSharedPtr<FJsonObject> Entity = LoadEntity(Db, EntityId);
			if (!Entity.IsValid()) { continue; }
			if (!Kind.IsEmpty() && Entity->GetStringField(TEXT("kind")) != Kind) { continue; }

			FSearchHit Hit{ Entity.ToSharedRef(), Score, ColStr(Stmt, TEXT("snippet")), ColStr(Stmt, TEXT("kind")) };
			if (const int32* Pos = IndexByEntity.Find(EntityId)) { Ordered[*Pos] = Hit; }
			else { IndexByEntity.Add(EntityId, Ordered.Num()); Ordered.Add(Hit); }
		}

		Ordered.Sort([](const FSearchHit& A, const FSearchHit& B) { return A.Score < B.Score; });
		if (Ordered.Num() > Limit) { Ordered.SetNum(Limit); }
		return Ordered;
	}

	TSharedRef<FJsonObject> SearchHitJson(const FSearchHit& H)
	{
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetObjectField(TEXT("entity"), H.Entity);
		R->SetNumberField(TEXT("score"), H.Score);
		R->SetStringField(TEXT("snippet"), H.Snippet);
		R->SetStringField(TEXT("matchedChunkKind"), H.ChunkKind);
		return R;
	}

	struct FRefRow { TSharedRef<FJsonObject> Entity; FString ViaType; int32 Depth; FString Direction; };

	TArray<FRefRow> BuildReferences(FSQLiteDatabase& Db, const FString& Id, const FString& Direction,
		int32 Depth, const FString& EdgeType, const FString& Kind, int32 Limit)
	{
		TArray<FRefRow> Results;
		if (!EntityExists(Db, Id)) { return Results; }

		TArray<FString> Dirs;
		if (Direction == TEXT("both")) { Dirs = { TEXT("in"), TEXT("out") }; }
		else { Dirs = { Direction.IsEmpty() ? TEXT("both") : Direction }; }

		TSet<FString> Seen;
		Seen.Add(Id);
		for (const FString& Dir : Dirs)
		{
			const bool bOut = Dir == TEXT("out");
			TArray<FString> Frontier = { Id };
			for (int32 D = 1; D <= Depth; ++D)
			{
				TArray<FString> Next;
				for (const FString& Node : Frontier)
				{
					for (const FRawEdge& E : ReadRawEdges(Db, Node, bOut))
					{
						if (!EdgeType.IsEmpty() && E.Type != EdgeType) { continue; }
						const FString Other = bOut ? E.Dst : E.Src;
						if (Seen.Contains(Other)) { continue; }
						Seen.Add(Other);
						TSharedPtr<FJsonObject> Entity = LoadEntity(Db, Other);
						if (!Entity.IsValid()) { continue; } // edge to an unextracted entity
						Next.Add(Other); // still traverse through it even if filtered from results
						if (!Kind.IsEmpty() && Entity->GetStringField(TEXT("kind")) != Kind) { continue; }
						Results.Add(FRefRow{ Entity.ToSharedRef(), E.Type, D, Dir });
						if (Results.Num() >= Limit) { return Results; }
					}
				}
				Frontier = MoveTemp(Next);
				if (Frontier.Num() == 0) { break; }
			}
		}
		return Results;
	}

	TSharedRef<FJsonObject> RefRowJson(const FRefRow& R)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetObjectField(TEXT("entity"), R.Entity);
		O->SetStringField(TEXT("viaType"), R.ViaType);
		O->SetNumberField(TEXT("depth"), R.Depth);
		O->SetStringField(TEXT("direction"), R.Direction);
		return O;
	}

	/** Rows of a simple SELECT as an array of objects with the given string columns. */
	TArray<TSharedPtr<FJsonValue>> SelectRows(FSQLiteDatabase& Db, const TCHAR* Sql,
		const TArray<FString>& StringCols, const TArray<FString>& IntCols)
	{
		TArray<TSharedPtr<FJsonValue>> Rows;
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(Sql);
		if (!Stmt.IsValid()) { return Rows; }
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			for (const FString& C : StringCols) { Row->SetStringField(C, ColStr(Stmt, *C)); }
			for (const FString& C : IntCols)
			{
				int32 N = 0;
				Stmt.GetColumnValueByName(*C, N);
				Row->SetNumberField(C, N);
			}
			Rows.Add(MakeShared<FJsonValueObject>(Row));
		}
		return Rows;
	}
}

FString GameIQQuery::Search(const FString& Query, const FString& Kind, int32 Limit)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); }; // FSQLiteDatabase asserts if destroyed while open

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FSearchHit& H : BuildSearchHits(Db, Query, Kind, Limit))
	{
		Results.Add(MakeShared<FJsonValueObject>(SearchHitJson(H)));
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

	// edges (capped) — full detail incl. attrs
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

FString GameIQQuery::References(const FString& Id, const FString& Direction, int32 Depth,
	const FString& EdgeType, const FString& Kind, int32 Limit)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); };

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FRefRow& R : BuildReferences(Db, Id, Direction, Depth <= 0 ? 1 : Depth, EdgeType, Kind, Limit <= 0 ? 200 : Limit))
	{
		Results.Add(MakeShared<FJsonValueObject>(RefRowJson(R)));
	}
	return Envelope(Db, MakeShared<FJsonValueArray>(Results));
}

FString GameIQQuery::Impact(const FString& Id, int32 MaxDepth)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); };

	TArray<TSharedPtr<FJsonValue>> Results;
	if (EntityExists(Db, Id))
	{
		struct FImpact { TSharedRef<FJsonObject> Entity; FString ViaType; int32 Depth; double Severity; };
		TMap<FString, FImpact> Best;
		TArray<FString> Order;
		TSet<FString> Seen;
		Seen.Add(Id);
		TArray<FString> Frontier = { Id };
		const int32 Cap = MaxDepth <= 0 ? 4 : MaxDepth;
		for (int32 D = 1; D <= Cap; ++D)
		{
			TArray<FString> Next;
			for (const FString& Node : Frontier)
			{
				for (const FRawEdge& E : ReadRawEdges(Db, Node, /*bOutgoing=*/false))
				{
					const FString Dependent = E.Src;
					if (Seen.Contains(Dependent)) { continue; }
					Seen.Add(Dependent);
					TSharedPtr<FJsonObject> Entity = LoadEntity(Db, Dependent);
					if (!Entity.IsValid()) { continue; }
					const double Severity = static_cast<double>(EdgeSeverity(E.Type)) / D; // closer + heavier = higher
					if (!Best.Contains(Dependent)) { Order.Add(Dependent); }
					Best.Add(Dependent, FImpact{ Entity.ToSharedRef(), E.Type, D, Severity });
					Next.Add(Dependent);
				}
			}
			Frontier = MoveTemp(Next);
			if (Frontier.Num() == 0) { break; }
		}
		Order.Sort([&Best](const FString& A, const FString& B) { return Best[A].Severity > Best[B].Severity; });
		for (const FString& Key : Order)
		{
			const FImpact& I = Best[Key];
			TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetObjectField(TEXT("entity"), I.Entity);
			O->SetStringField(TEXT("viaType"), I.ViaType);
			O->SetNumberField(TEXT("depth"), I.Depth);
			O->SetNumberField(TEXT("severity"), I.Severity);
			Results.Add(MakeShared<FJsonValueObject>(O));
		}
	}
	return Envelope(Db, MakeShared<FJsonValueArray>(Results));
}

FString GameIQQuery::Explain(const FString& Topic, int32 Limit)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); };

	const int32 Seeds = Limit <= 0 ? 8 : Limit;
	TArray<FSearchHit> Hits = BuildSearchHits(Db, Topic, FString(), Seeds);

	TArray<TSharedPtr<FJsonValue>> SeedJson;
	TSet<FString> SeedIds;
	for (const FSearchHit& H : Hits)
	{
		SeedJson.Add(MakeShared<FJsonValueObject>(SearchHitJson(H)));
		SeedIds.Add(H.Entity->GetStringField(TEXT("id")));
	}

	TArray<TSharedPtr<FJsonValue>> Related;
	TSet<FString> RelatedSeen = SeedIds;
	for (const FSearchHit& H : Hits)
	{
		const FString SeedId = H.Entity->GetStringField(TEXT("id"));
		for (const FRefRow& R : BuildReferences(Db, SeedId, TEXT("both"), 1, FString(), FString(), 200))
		{
			const FString Rid = R.Entity->GetStringField(TEXT("id"));
			if (RelatedSeen.Contains(Rid)) { continue; }
			RelatedSeen.Add(Rid);
			Related.Add(MakeShared<FJsonValueObject>(RefRowJson(R)));
		}
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("topic"), Topic);
	Payload->SetArrayField(TEXT("seeds"), SeedJson);
	Payload->SetArrayField(TEXT("related"), Related);
	return Envelope(Db, MakeShared<FJsonValueObject>(Payload));
}

FString GameIQQuery::ProjectStats(const FString& Facet)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); };

	const FString F = Facet.IsEmpty() ? TEXT("overview") : Facet;

	if (F == TEXT("kinds"))
	{
		return Envelope(Db, MakeShared<FJsonValueArray>(SelectRows(Db,
			TEXT("SELECT kind, COUNT(*) AS count FROM entities GROUP BY kind ORDER BY count DESC"),
			{ TEXT("kind") }, { TEXT("count") })));
	}
	if (F == TEXT("edges"))
	{
		return Envelope(Db, MakeShared<FJsonValueArray>(SelectRows(Db,
			TEXT("SELECT type, COUNT(*) AS count FROM edges GROUP BY type ORDER BY count DESC"),
			{ TEXT("type") }, { TEXT("count") })));
	}
	if (F == TEXT("unused"))
	{
		return Envelope(Db, MakeShared<FJsonValueArray>(SelectRows(Db,
			TEXT("SELECT e.id AS id, e.kind AS kind, e.name AS name, e.path AS path FROM entities e "
			     "WHERE e.kind IN ('asset','blueprint') "
			     "AND NOT EXISTS (SELECT 1 FROM edges x WHERE x.dst = e.id) LIMIT 500"),
			{ TEXT("id"), TEXT("kind"), TEXT("name"), TEXT("path") }, {})));
	}
	if (F == TEXT("largest-deps"))
	{
		return Envelope(Db, MakeShared<FJsonValueArray>(SelectRows(Db,
			TEXT("SELECT e.id AS id, e.name AS name, e.kind AS kind, COUNT(x.dst) AS outDeps "
			     "FROM entities e JOIN edges x ON x.src = e.id GROUP BY e.id ORDER BY outDeps DESC LIMIT 50"),
			{ TEXT("id"), TEXT("name"), TEXT("kind") }, { TEXT("outDeps") })));
	}

	if (F == TEXT("authority"))
	{
		return Envelope(Db, MakeShared<FJsonValueArray>(SelectRows(Db,
			TEXT("SELECT COALESCE(authority,'extracted-fact') AS authority, COUNT(*) AS count "
			     "FROM entities GROUP BY authority ORDER BY count DESC"),
			{ TEXT("authority") }, { TEXT("count") })));
	}
	if (F == TEXT("doc-types"))
	{
		// docType lives in each document's detail JSON; count via json_extract (SQLiteCore has JSON1).
		return Envelope(Db, MakeShared<FJsonValueArray>(SelectRows(Db,
			TEXT("SELECT json_extract(detail,'$.docType') AS docType, COUNT(*) AS count "
			     "FROM entities WHERE kind='document' GROUP BY docType ORDER BY count DESC"),
			{ TEXT("docType") }, { TEXT("count") })));
	}

	// overview (default)
	int32 Total = 0;
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("SELECT COUNT(*) AS n FROM entities"));
		if (Stmt.IsValid() && Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByName(TEXT("n"), Total);
		}
	}
	TSharedRef<FJsonObject> Overview = MakeShared<FJsonObject>();
	Overview->SetNumberField(TEXT("totalEntities"), Total);
	Overview->SetArrayField(TEXT("byKind"), SelectRows(Db,
		TEXT("SELECT kind, COUNT(*) AS count FROM entities GROUP BY kind ORDER BY count DESC"),
		{ TEXT("kind") }, { TEXT("count") }));
	Overview->SetArrayField(TEXT("byEdgeType"), SelectRows(Db,
		TEXT("SELECT type, COUNT(*) AS count FROM edges GROUP BY type ORDER BY count DESC"),
		{ TEXT("type") }, { TEXT("count") }));
	Overview->SetField(TEXT("projectName"), NullOrString(MetaValue(Db, TEXT("projectName"))));
	Overview->SetField(TEXT("lastIngestAtIso"), NullOrString(MetaValue(Db, TEXT("lastIngestAtIso"))));
	Overview->SetField(TEXT("lastGeneratedAtIso"), NullOrString(MetaValue(Db, TEXT("lastGeneratedAtIso"))));
	return Envelope(Db, MakeShared<FJsonValueObject>(Overview));
}

namespace
{
	/** Parse a stored detail JSON string into an object (or null). */
	TSharedPtr<FJsonObject> ParseDetail(const FString& Raw)
	{
		if (Raw.IsEmpty()) { return nullptr; }
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Raw);
		return (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid()) ? Obj : nullptr;
	}

	/** A scalar JSON value → display string; empty for objects/arrays/null. */
	FString ScalarToString(const TSharedPtr<FJsonValue>& V)
	{
		if (!V.IsValid()) { return FString(); }
		switch (V->Type)
		{
		case EJson::String: return V->AsString();
		case EJson::Boolean: return V->AsBool() ? TEXT("true") : TEXT("false");
		case EJson::Number:
		{
			const double D = V->AsNumber();
			if (FMath::IsNearlyEqual(D, FMath::RoundToDouble(D))) { return FString::Printf(TEXT("%lld"), (int64)FMath::RoundToDouble(D)); }
			return FString::Printf(TEXT("%g"), D);
		}
		default: return FString();
		}
	}

	/** Flatten a detail object's top-level scalar fields to key→string. */
	TMap<FString, FString> FlattenScalars(const TSharedPtr<FJsonObject>& Obj)
	{
		TMap<FString, FString> Out;
		if (Obj.IsValid())
		{
			for (const auto& Pair : Obj->Values)
			{
				const FString S = ScalarToString(Pair.Value);
				if (!S.IsEmpty()) { Out.Add(FString(Pair.Key), S); }
			}
		}
		return Out;
	}

	/** If `Text` states `Key <=|:> value`, return the stated value (else empty). Case-insensitive key. */
	FString StatedValueFor(const FString& Text, const FString& Key)
	{
		if (Key.Len() < 3) { return FString(); }
		const FString Lower = Text.ToLower();
		const FString KeyLower = Key.ToLower();
		int32 From = 0;
		while (true)
		{
			const int32 At = Lower.Find(KeyLower, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
			if (At == INDEX_NONE) { return FString(); }
			int32 i = At + Key.Len();
			while (i < Text.Len() && (Text[i] == TEXT(' ') || Text[i] == TEXT('\t'))) { ++i; }
			if (i < Text.Len() && (Text[i] == TEXT('=') || Text[i] == TEXT(':')))
			{
				++i;
				while (i < Text.Len() && (Text[i] == TEXT(' ') || Text[i] == TEXT('\t'))) { ++i; }
				FString Val;
				while (i < Text.Len())
				{
					const TCHAR C = Text[i];
					if (FChar::IsAlnum(C) || C == TEXT('.') || C == TEXT('_') || C == TEXT('#') || C == TEXT('-')) { Val.AppendChar(C); ++i; }
					else { break; }
				}
				if (Val.Len() > 0) { return Val; }
			}
			From = At + Key.Len();
		}
	}

	bool ValuesDiffer(const FString& A, const FString& B)
	{
		if (A.IsNumeric() && B.IsNumeric())
		{
			return !FMath::IsNearlyEqual(FCString::Atod(*A), FCString::Atod(*B), 1e-6);
		}
		return !A.Equals(B, ESearchCase::IgnoreCase);
	}
}

FString GameIQQuery::Coverage(const FString& DocType, int32 Limit)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); };

	// Document id → title.
	TMap<FString, FString> DocTitle;
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("SELECT id, name FROM entities WHERE kind='document'"));
		if (Stmt.IsValid())
		{
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				DocTitle.Add(ColStr(Stmt, TEXT("id")), ColStr(Stmt, TEXT("name")));
			}
		}
	}

	struct FDocCov { FString Title; FString DocType; int32 Total = 0; int32 Linked = 0; TArray<FString> Unlinked; };
	TMap<FString, FDocCov> Docs;
	int32 SecTotal = 0, SecLinked = 0;

	FSQLitePreparedStatement Stmt = Db.PrepareStatement(
		TEXT("SELECT e.id AS id, e.name AS name, e.parent AS parent, e.detail AS detail, "
		     "(SELECT COUNT(*) FROM edges x WHERE x.src = e.id AND x.type='describes') AS links "
		     "FROM entities e WHERE e.kind='doc-section'"));
	if (Stmt.IsValid())
	{
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			const FString Parent = ColStr(Stmt, TEXT("parent"));
			const FString Name = ColStr(Stmt, TEXT("name"));
			const TSharedPtr<FJsonObject> D = ParseDetail(ColStr(Stmt, TEXT("detail")));
			const FString ThisType = D.IsValid() ? D->GetStringField(TEXT("docType")) : FString();
			if (!DocType.IsEmpty() && ThisType != DocType) { continue; }

			int32 Links = 0; Stmt.GetColumnValueByName(TEXT("links"), Links);
			FDocCov& Cov = Docs.FindOrAdd(Parent);
			if (Cov.Title.IsEmpty()) { Cov.Title = DocTitle.FindRef(Parent); Cov.DocType = ThisType; }
			++Cov.Total; ++SecTotal;
			if (Links > 0) { ++Cov.Linked; ++SecLinked; }
			else { Cov.Unlinked.Add(Name); }
		}
	}

	TArray<TSharedPtr<FJsonValue>> DocRows;
	for (const TPair<FString, FDocCov>& P : Docs)
	{
		if (DocRows.Num() >= Limit) { break; }
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetStringField(TEXT("doc"), P.Key);
		R->SetStringField(TEXT("title"), P.Value.Title);
		R->SetStringField(TEXT("docType"), P.Value.DocType);
		R->SetNumberField(TEXT("sections"), P.Value.Total);
		R->SetNumberField(TEXT("implemented"), P.Value.Linked);
		TArray<TSharedPtr<FJsonValue>> Un;
		for (const FString& N : P.Value.Unlinked) { Un.Add(MakeShared<FJsonValueString>(N)); }
		R->SetArrayField(TEXT("unimplemented"), Un);
		DocRows.Add(MakeShared<FJsonValueObject>(R));
	}

	TSharedRef<FJsonObject> Summary = MakeShared<FJsonObject>();
	Summary->SetNumberField(TEXT("documents"), Docs.Num());
	Summary->SetNumberField(TEXT("sections"), SecTotal);
	Summary->SetNumberField(TEXT("sectionsImplemented"), SecLinked);
	Summary->SetNumberField(TEXT("sectionsWithoutImplementation"), SecTotal - SecLinked);

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetField(TEXT("filterDocType"), NullOrString(DocType));
	Payload->SetObjectField(TEXT("summary"), Summary);
	Payload->SetArrayField(TEXT("docs"), DocRows);
	Payload->SetStringField(TEXT("note"),
		TEXT("Coverage compares stated design intent (documents) against extracted implementation via "
		     "'describes' links. 'unimplemented' sections have no matching implementation in the index."));
	return Envelope(Db, MakeShared<FJsonValueObject>(Payload));
}

FString GameIQQuery::Drift(int32 Limit)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); };

	TArray<TSharedPtr<FJsonValue>> Rows;

	// Each describes edge: does the doc's stated key=value contradict the entity's extracted value?
	FSQLitePreparedStatement Edge = Db.PrepareStatement(TEXT("SELECT src, dst FROM edges WHERE type='describes'"));
	if (Edge.IsValid())
	{
		while (Edge.Step() == ESQLitePreparedStatementStepResult::Row && Rows.Num() < Limit)
		{
			const FString Src = ColStr(Edge, TEXT("src"));
			const FString Dst = ColStr(Edge, TEXT("dst"));

			// section text
			FString Text;
			{
				FSQLitePreparedStatement T = Db.PrepareStatement(
					TEXT("SELECT text FROM chunks WHERE entity_id = ? AND kind='doc-section' LIMIT 1"));
				if (T.IsValid()) { T.SetBindingValueByIndex(1, Src); if (T.Step() == ESQLitePreparedStatementStepResult::Row) { T.GetColumnValueByName(TEXT("text"), Text); } }
			}
			if (Text.IsEmpty()) { continue; }

			// target entity name + detail
			FString TargetName, DetailRaw;
			{
				FSQLitePreparedStatement E = Db.PrepareStatement(TEXT("SELECT name, detail FROM entities WHERE id = ? LIMIT 1"));
				if (E.IsValid()) { E.SetBindingValueByIndex(1, Dst); if (E.Step() == ESQLitePreparedStatementStepResult::Row) { E.GetColumnValueByName(TEXT("name"), TargetName); E.GetColumnValueByName(TEXT("detail"), DetailRaw); } }
			}
			const TMap<FString, FString> Facts = FlattenScalars(ParseDetail(DetailRaw));
			for (const TPair<FString, FString>& Fact : Facts)
			{
				const FString Stated = StatedValueFor(Text, Fact.Key);
				if (!Stated.IsEmpty() && ValuesDiffer(Stated, Fact.Value))
				{
					TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
					R->SetStringField(TEXT("docSection"), Src);
					R->SetStringField(TEXT("entity"), Dst);
					R->SetStringField(TEXT("entityName"), TargetName);
					R->SetStringField(TEXT("property"), Fact.Key);
					R->SetStringField(TEXT("statedInDoc"), Stated);
					R->SetStringField(TEXT("actualInIndex"), Fact.Value);
					Rows.Add(MakeShared<FJsonValueObject>(R));
					if (Rows.Num() >= Limit) { break; }
				}
			}
		}
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("drift"), Rows);
	Payload->SetStringField(TEXT("note"),
		TEXT("Drift = a doc section states 'property = value' that contradicts the value extracted from "
		     "the implementation it describes. Stated intent is not ground truth; verify before acting."));
	return Envelope(Db, MakeShared<FJsonValueObject>(Payload));
}
