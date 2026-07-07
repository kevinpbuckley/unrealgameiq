// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQQuery.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameIQTextUtil.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SQLiteDatabase.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQQuery, Log, All);

namespace
{
	FString GDbPathOverride; // tests only — see SetDbPathOverrideForTests

	FString IndexDbPath()
	{
		if (!GDbPathOverride.IsEmpty()) { return GDbPathOverride; }
		// ProjectDir() is relative to the engine binary at runtime; sqlite resolves against the
		// process cwd, so hand it an absolute path.
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
		return FPaths::Combine(Root, TEXT(".gameiq"), TEXT("index.db"));
	}

	/** Open the index read-only-in-spirit; falls back to read-write (WAL readers may need to
	 *  create the -shm file). On failure, `OutError` explains why for the JSON response. */
	bool OpenIndex(FSQLiteDatabase& Db, FString& OutError)
	{
		const FString Path = IndexDbPath();
		if (!FPaths::FileExists(Path))
		{
			OutError = FString::Printf(TEXT("index not found at %s — run GameIQ.Rebuild first"), *Path);
			UE_LOG(LogGameIQQuery, Warning, TEXT("Game IQ: %s"), *OutError);
			return false;
		}
		if (Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadOnly))
		{
			Db.Execute(TEXT("PRAGMA busy_timeout=2000;"));
			return true;
		}
		const FString RoErr = Db.GetLastError();
		if (Db.Open(*Path, ESQLiteDatabaseOpenMode::ReadWrite))
		{
			Db.Execute(TEXT("PRAGMA busy_timeout=2000;"));
			return true;
		}
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

	/** Build the entity object from a statement positioned on a row that includes the `entities`
	 *  columns (SELECT e.* or SELECT *). */
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

	/** Severity per edge type (mirror of query.ts SEVERITY): heavier = worse to break. */
	int32 EdgeSeverity(const FString& Type)
	{
		static const TMap<FString, int32> Severity = {
			{ TEXT("inherits"), 10 }, { TEXT("implements"), 9 }, { TEXT("calls"), 8 },
			{ TEXT("binds-input"), 7 }, // breaking an input binding breaks player controls
			{ TEXT("uses-skeleton"), 8 }, { TEXT("uses-material"), 7 }, { TEXT("uses-texture"), 5 },
			{ TEXT("overrides-parameter"), 6 }, { TEXT("casts-to"), 6 }, { TEXT("placed-in-level"), 5 },
			{ TEXT("instance-of"), 5 }, // placed actor → its Blueprint/C++ class
			{ TEXT("plays-on"), 4 }, { TEXT("depends-on"), 4 }, { TEXT("references"), 3 },
			{ TEXT("describes"), 2 }, { TEXT("constrains"), 2 }, { TEXT("illustrates"), 2 },
		};
		const int32* Found = Severity.Find(Type);
		return Found ? *Found : 1;
	}

	// ---- structured builders (shared so `explain` can reuse search + references) --------------

	struct FSearchHit { TSharedRef<FJsonObject> Entity; double Score; FString Snippet; FString ChunkKind; int32 InboundRefs = 0; };

	/**
	 * One FTS pass: dedupe to each entity's best-scoring chunk (window function), push the kind
	 * filter and pagination into SQL, and rank text hits above aux (split-identifier) hits via
	 * bm25 column weights.
	 *
	 * bm25 is negative (more negative = better), so a multiplier < 1 demotes and > 1 boosts.
	 * Kind weights fix the "player controls" failure mode: placed-actor property dumps are the
	 * largest, keyword-densest chunks in the index ("Player Camera Location" × every weather
	 * actor) and drowned the real input/blueprint/code entities on intent-shaped queries.
	 * cpp-body chunks are demoted below summaries for the same reason: a long function body
	 * mentioning a term shouldn't outrank the entity that IS that term.
	 */
	TArray<FSearchHit> RunFtsQuery(FSQLiteDatabase& Db, const FString& Match, const FString& Kind,
		const FString& PathPrefix, int32 Limit, int32 Offset)
	{
		TArray<FSearchHit> Out;
		// The extra 0.5x CASE demotes bulk asset classes (textures, material instances/functions):
		// they exist in the thousands, their names echo gameplay words ("teeth_normal_map" matches
		// "mapping"), and they are almost never the answer to an intent-shaped question. A query
		// that really wants them still finds them via kind filter or their exact name.
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT(
			"WITH hits AS ("
			"  SELECT c.id AS chunkId, c.entity_id AS entityId, c.kind AS chunkKind,"
			"         bm25(chunks_fts, 1.0, 0.6)"
			"           * (CASE ce.kind WHEN 'level-actor' THEN 0.35 ELSE 1.0 END)"
			"           * (CASE c.kind WHEN 'cpp-body' THEN 0.7 ELSE 1.0 END)"
			"           * (CASE WHEN json_extract(ce.detail,'$.assetClass') IN"
			"               ('Texture2D','TextureCube','Texture2DArray','VirtualTexture2D',"
			"                'MaterialInstanceConstant','MaterialFunction') THEN 0.5 ELSE 1.0 END) AS score,"
			"         snippet(chunks_fts, 0, '[', ']', ' ... ', 12) AS snip"
			"  FROM chunks_fts JOIN chunks c ON c.rowid = chunks_fts.rowid"
			"  JOIN entities ce ON ce.id = c.entity_id"
			"  WHERE chunks_fts MATCH ?1"
			"), best AS ("
			"  SELECT *, ROW_NUMBER() OVER (PARTITION BY entityId ORDER BY score) AS rn FROM hits"
			") "
			"SELECT b.chunkId, b.chunkKind, b.score, b.snip,"
			"       (SELECT COUNT(*) FROM edges x WHERE x.dst = e.id) AS inRefs, e.* "
			"FROM best b JOIN entities e ON e.id = b.entityId "
			"WHERE b.rn = 1 AND (?2 = '' OR e.kind = ?2) "
			"AND (?5 = '' OR e.id LIKE 'asset:' || ?5 || '%' OR e.path LIKE ?5 || '%') "
			"ORDER BY b.score LIMIT ?3 OFFSET ?4"));
		if (!Stmt.IsValid())
		{
			UE_LOG(LogGameIQQuery, Warning, TEXT("Game IQ: search statement failed to prepare: %s"), *Db.GetLastError());
			return Out;
		}
		Stmt.SetBindingValueByIndex(1, Match);
		Stmt.SetBindingValueByIndex(2, Kind);
		Stmt.SetBindingValueByIndex(3, Limit);
		Stmt.SetBindingValueByIndex(4, Offset);
		Stmt.SetBindingValueByIndex(5, PathPrefix);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			double Score = 0.0;
			Stmt.GetColumnValueByName(TEXT("score"), Score);
			int32 InRefs = 0;
			Stmt.GetColumnValueByName(TEXT("inRefs"), InRefs);
			Out.Add(FSearchHit{ EntityJson(Stmt), Score, ColStr(Stmt, TEXT("snip")), ColStr(Stmt, TEXT("chunkKind")), InRefs });
		}
		return Out;
	}

	/**
	 * Post-fetch re-rank, two signals bm25 can't see (scores are negative — more negative =
	 * better — so boosting multiplies UP):
	 * - Name match: bm25 scores chunk BODIES, so an entity whose NAME is the query
	 *   ("IMC_SenseisRevenge") can lose to a big chunk that merely mentions the words. Boost by
	 *   the fraction of query tokens in the name (camel/underscore-split, case-insensitive).
	 * - Reference weight: between comparable text matches, the entity the project actually leans
	 *   on should win — the IMC eleven hero blueprints bind over the one the bird pawn uses.
	 *   Log-scale (11 refs ≈ 1.43x, 100 ≈ 1.8x) so hub assets can't steamroll text relevance.
	 */
	void RerankHits(const FString& Query, TArray<FSearchHit>& Hits)
	{
		TArray<FString> Tokens;
		for (const FString& T : GameIQText::WordTokens(Query))
		{
			if (T.Len() >= 3) { Tokens.Add(T.ToLower()); }
		}
		for (FSearchHit& H : Hits)
		{
			if (Tokens.Num() > 0)
			{
				const FString Name = H.Entity->GetStringField(TEXT("name"));
				const FString Hay = (Name + TEXT(" ") + GameIQText::SplitCamel(Name.Replace(TEXT("_"), TEXT(" ")))).ToLower();
				int32 Matched = 0;
				for (const FString& T : Tokens)
				{
					if (Hay.Contains(T)) { ++Matched; }
				}
				H.Score *= 1.0 + 0.8 * (double)Matched / (double)Tokens.Num();
			}
			H.Score *= 1.0 + 0.12 * FMath::Log2(1.0 + (double)FMath::Max(0, H.InboundRefs));
		}
		Hits.StableSort([](const FSearchHit& A, const FSearchHit& B) { return A.Score < B.Score; });
	}

	/** AND-first (precision), then OR (recall), then prefix-OR (partial words) — first tier
	 *  with any hits wins, so "reload logic" doesn't return everything containing "logic".
	 *  Overfetches, re-ranks by name match, then applies Offset/Limit. */
	TArray<FSearchHit> BuildSearchHits(FSQLiteDatabase& Db, const FString& Query, const FString& Kind,
		int32 Limit, int32 Offset, const FString& PathPrefix = TEXT(""))
	{
		const int32 Fetch = FMath::Min((Offset + Limit) * 3, 300);
		auto RunTier = [&](const FString& Match) -> TArray<FSearchHit>
		{
			TArray<FSearchHit> Hits = RunFtsQuery(Db, Match, Kind, PathPrefix, Fetch, 0);
			RerankHits(Query, Hits);
			if (Offset > 0) { Hits.RemoveAt(0, FMath::Min(Offset, Hits.Num())); }
			if (Hits.Num() > Limit) { Hits.SetNum(Limit); }
			return Hits;
		};

		const FString AndQ = GameIQText::BuildFtsQuery(Query, TEXT("AND"));
		if (AndQ.IsEmpty()) { return {}; }
		TArray<FSearchHit> Hits = RunTier(AndQ);
		if (Hits.Num() > 0) { return Hits; }

		const FString OrQ = GameIQText::BuildFtsQuery(Query, TEXT("OR"));
		if (OrQ != AndQ)
		{
			Hits = RunTier(OrQ);
			if (Hits.Num() > 0) { return Hits; }
		}
		return RunTier(GameIQText::BuildFtsQuery(Query, TEXT("OR"), /*bPrefix=*/true));
	}

	TSharedRef<FJsonObject> SearchHitJson(const FSearchHit& H)
	{
		TSharedRef<FJsonObject> R = MakeShared<FJsonObject>();
		R->SetObjectField(TEXT("entity"), H.Entity);
		R->SetNumberField(TEXT("score"), H.Score);
		R->SetStringField(TEXT("snippet"), H.Snippet);
		R->SetStringField(TEXT("matchedChunkKind"), H.ChunkKind);
		R->SetNumberField(TEXT("inboundRefs"), H.InboundRefs);
		return R;
	}

	struct FRefRow { TSharedRef<FJsonObject> Entity; FString ViaType; int32 Depth; FString Direction; };

	/**
	 * One-direction transitive walk as a single recursive CTE — the previous C++ BFS prepared and
	 * ran one edge query per visited node plus one entity load per neighbor (thousands of
	 * statements on hub assets). The window function keeps each entity's shallowest hop.
	 */
	void WalkDirection(FSQLiteDatabase& Db, const FString& Id, bool bOutgoing, int32 Depth,
		const FString& EdgeType, const FString& Kind, int32 MaxRows, const TCHAR* DirLabel,
		TSet<FString>& SeenIds, TArray<FRefRow>& Results)
	{
		const TCHAR* Sql = bOutgoing
			? TEXT(
				"WITH RECURSIVE walk(id, via, depth) AS ("
				"  SELECT e.dst, e.type, 1 FROM edges e WHERE e.src = ?1 AND (?2 = '' OR e.type = ?2)"
				"  UNION"
				"  SELECT e.dst, e.type, w.depth + 1 FROM edges e JOIN walk w ON e.src = w.id"
				"  WHERE w.depth < ?3 AND (?2 = '' OR e.type = ?2)"
				"), best AS ("
				"  SELECT id, via, depth, ROW_NUMBER() OVER (PARTITION BY id ORDER BY depth) AS rn FROM walk"
				") "
				"SELECT b.via AS viaType, b.depth AS hopDepth, e.* "
				"FROM best b JOIN entities e ON e.id = b.id "
				"WHERE b.rn = 1 AND b.id <> ?1 AND (?4 = '' OR e.kind = ?4) "
				"ORDER BY b.depth, e.id LIMIT ?5")
			: TEXT(
				"WITH RECURSIVE walk(id, via, depth) AS ("
				"  SELECT e.src, e.type, 1 FROM edges e WHERE e.dst = ?1 AND (?2 = '' OR e.type = ?2)"
				"  UNION"
				"  SELECT e.src, e.type, w.depth + 1 FROM edges e JOIN walk w ON e.dst = w.id"
				"  WHERE w.depth < ?3 AND (?2 = '' OR e.type = ?2)"
				"), best AS ("
				"  SELECT id, via, depth, ROW_NUMBER() OVER (PARTITION BY id ORDER BY depth) AS rn FROM walk"
				") "
				"SELECT b.via AS viaType, b.depth AS hopDepth, e.* "
				"FROM best b JOIN entities e ON e.id = b.id "
				"WHERE b.rn = 1 AND b.id <> ?1 AND (?4 = '' OR e.kind = ?4) "
				"ORDER BY b.depth, e.id LIMIT ?5");

		FSQLitePreparedStatement Stmt = Db.PrepareStatement(Sql);
		if (!Stmt.IsValid())
		{
			UE_LOG(LogGameIQQuery, Warning, TEXT("Game IQ: traversal statement failed to prepare: %s"), *Db.GetLastError());
			return;
		}
		Stmt.SetBindingValueByIndex(1, Id);
		Stmt.SetBindingValueByIndex(2, EdgeType);
		Stmt.SetBindingValueByIndex(3, Depth);
		Stmt.SetBindingValueByIndex(4, Kind);
		Stmt.SetBindingValueByIndex(5, MaxRows);
		while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			const FString EntId = ColStr(Stmt, TEXT("id"));
			if (SeenIds.Contains(EntId)) { continue; }
			SeenIds.Add(EntId);
			int32 HopDepth = 0;
			Stmt.GetColumnValueByName(TEXT("hopDepth"), HopDepth);
			Results.Add(FRefRow{ EntityJson(Stmt), ColStr(Stmt, TEXT("viaType")), HopDepth, DirLabel });
		}
	}

	TArray<FRefRow> BuildReferences(FSQLiteDatabase& Db, const FString& Id, const FString& Direction,
		int32 Depth, const FString& EdgeType, const FString& Kind, int32 Limit, int32 Offset)
	{
		TArray<FRefRow> Results;
		if (!EntityExists(Db, Id)) { return Results; }

		const FString Dir = Direction.IsEmpty() ? TEXT("both") : Direction;
		const int32 MaxRows = Offset + Limit;
		TSet<FString> SeenIds;
		SeenIds.Add(Id);
		if (Dir == TEXT("in") || Dir == TEXT("both"))
		{
			WalkDirection(Db, Id, /*bOutgoing=*/false, Depth, EdgeType, Kind, MaxRows, TEXT("in"), SeenIds, Results);
		}
		if (Dir == TEXT("out") || Dir == TEXT("both"))
		{
			WalkDirection(Db, Id, /*bOutgoing=*/true, Depth, EdgeType, Kind, MaxRows, TEXT("out"), SeenIds, Results);
		}

		Results.StableSort([](const FRefRow& A, const FRefRow& B) { return A.Depth < B.Depth; });
		if (Offset > 0)
		{
			Results.RemoveAt(0, FMath::Min(Offset, Results.Num()));
		}
		if (Results.Num() > Limit) { Results.SetNum(Limit); }
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

	int64 ScalarInt(FSQLiteDatabase& Db, const TCHAR* Sql)
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(Sql);
		int64 N = 0;
		if (Stmt.IsValid() && Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			Stmt.GetColumnValueByIndex(0, N);
		}
		return N;
	}
}

void GameIQQuery::SetDbPathOverrideForTests(const FString& Path)
{
	GDbPathOverride = Path;
}

FString GameIQQuery::Search(const FString& Query, const FString& Kind, int32 Limit, int32 Offset,
	const FString& PathPrefix)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); }; // FSQLiteDatabase asserts if destroyed while open

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FSearchHit& H : BuildSearchHits(Db, Query, Kind, Limit <= 0 ? 20 : Limit, FMath::Max(0, Offset), PathPrefix))
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

	// children (capped) — round-robin by class (one of each class first, rare classes leading),
	// so a 50-row window on a 500-actor level still shows every class instead of 50 rocks.
	TArray<TSharedPtr<FJsonValue>> Children;
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT(
			"SELECT * FROM ("
			"  SELECT e.*,"
			"    ROW_NUMBER() OVER (PARTITION BY COALESCE(json_extract(e.detail,'$.class'), e.kind) ORDER BY e.id) AS rr,"
			"    COUNT(*) OVER (PARTITION BY COALESCE(json_extract(e.detail,'$.class'), e.kind)) AS clsN"
			"  FROM entities e WHERE e.parent = ?"
			") ORDER BY rr, clsN, id LIMIT ?"));
		if (!Stmt.IsValid())
		{
			Stmt = Db.PrepareStatement(TEXT("SELECT * FROM entities WHERE parent = ? LIMIT ?"));
		}
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

	// class → count rollup over ALL children (not just the capped window): the cheap answer to
	// "what's in this level" without paging anything.
	TArray<TSharedPtr<FJsonValue>> ChildrenByClass;
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT(
			"SELECT COALESCE(json_extract(detail,'$.class'), kind) AS class, COUNT(*) AS count "
			"FROM entities WHERE parent = ? GROUP BY class ORDER BY count DESC"));
		if (Stmt.IsValid())
		{
			Stmt.SetBindingValueByIndex(1, Id);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				Row->SetStringField(TEXT("class"), ColStr(Stmt, TEXT("class")));
				int32 N = 0;
				Stmt.GetColumnValueByName(TEXT("count"), N);
				Row->SetNumberField(TEXT("count"), N);
				ChildrenByClass.Add(MakeShared<FJsonValueObject>(Row));
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
	if (ChildrenByClass.Num() > 0)
	{
		Detail->SetArrayField(TEXT("childrenByClass"), ChildrenByClass);
	}
	Detail->SetArrayField(TEXT("chunks"), Chunks);
	Detail->SetObjectField(TEXT("counts"), Counts);
	const bool bTruncated = OutCount > Cap || InCount > Cap || ChildCount > Cap || ChunkCount > Cap;
	Detail->SetBoolField(TEXT("truncated"), bTruncated);
	if (bTruncated)
	{
		Detail->SetStringField(TEXT("note"), FString::Printf(
			TEXT("arrays capped at %d (full totals in counts); children are round-robin by class — use Children(id, classFilter, offset) to page the rest"), Cap));
	}

	return Envelope(Db, MakeShared<FJsonValueObject>(Detail));
}

FString GameIQQuery::Children(const FString& Id, const FString& ClassFilter, int32 Limit, int32 Offset)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); }; // FSQLiteDatabase asserts if destroyed while open

	const int32 UseLimit = Limit <= 0 ? 50 : Limit;
	const int32 UseOffset = FMath::Max(0, Offset);

	int32 Total = 0;
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT(
			"SELECT COUNT(*) AS n FROM entities "
			"WHERE parent = ?1 AND (?2 = '' OR COALESCE(json_extract(detail,'$.class'), kind) LIKE '%' || ?2 || '%')"));
		if (Stmt.IsValid())
		{
			Stmt.SetBindingValueByIndex(1, Id);
			Stmt.SetBindingValueByIndex(2, ClassFilter);
			if (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				Stmt.GetColumnValueByName(TEXT("n"), Total);
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> Rows;
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT(
			"SELECT * FROM entities "
			"WHERE parent = ?1 AND (?2 = '' OR COALESCE(json_extract(detail,'$.class'), kind) LIKE '%' || ?2 || '%') "
			"ORDER BY id LIMIT ?3 OFFSET ?4"));
		if (Stmt.IsValid())
		{
			Stmt.SetBindingValueByIndex(1, Id);
			Stmt.SetBindingValueByIndex(2, ClassFilter);
			Stmt.SetBindingValueByIndex(3, UseLimit);
			Stmt.SetBindingValueByIndex(4, UseOffset);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				Rows.Add(MakeShared<FJsonValueObject>(EntityJson(Stmt)));
			}
		}
	}

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetStringField(TEXT("parent"), Id);
	Payload->SetStringField(TEXT("classFilter"), ClassFilter);
	Payload->SetNumberField(TEXT("total"), Total);
	Payload->SetNumberField(TEXT("offset"), UseOffset);
	Payload->SetArrayField(TEXT("children"), Rows);
	return Envelope(Db, MakeShared<FJsonValueObject>(Payload));
}

FString GameIQQuery::References(const FString& Id, const FString& Direction, int32 Depth,
	const FString& EdgeType, const FString& Kind, int32 Limit, int32 Offset)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); };

	TArray<TSharedPtr<FJsonValue>> Results;
	for (const FRefRow& R : BuildReferences(Db, Id, Direction, Depth <= 0 ? 1 : Depth, EdgeType, Kind,
		Limit <= 0 ? 200 : Limit, FMath::Max(0, Offset)))
	{
		Results.Add(MakeShared<FJsonValueObject>(RefRowJson(R)));
	}
	return Envelope(Db, MakeShared<FJsonValueArray>(Results));
}

FString GameIQQuery::Impact(const FString& Id, int32 MaxDepth, int32 Limit)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); };

	const int32 DepthCap = MaxDepth <= 0 ? 4 : MaxDepth;
	const int32 OutCap = Limit <= 0 ? 200 : Limit;
	constexpr int32 InternalCap = 5000; // bound the walk on hub entities; byKind still reflects what was fetched

	struct FImpactRow { TSharedRef<FJsonObject> Entity; FString ViaType; int32 Depth; double Severity; };
	TArray<FImpactRow> Rows;
	TMap<FString, int32> ByKind;

	if (EntityExists(Db, Id))
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT(
			"WITH RECURSIVE walk(id, via, depth) AS ("
			"  SELECT e.src, e.type, 1 FROM edges e WHERE e.dst = ?1"
			"  UNION"
			"  SELECT e.src, e.type, w.depth + 1 FROM edges e JOIN walk w ON e.dst = w.id WHERE w.depth < ?2"
			"), best AS ("
			"  SELECT id, via, depth, ROW_NUMBER() OVER (PARTITION BY id ORDER BY depth) AS rn FROM walk"
			") "
			"SELECT b.via AS viaType, b.depth AS hopDepth, e.* "
			"FROM best b JOIN entities e ON e.id = b.id "
			"WHERE b.rn = 1 AND b.id <> ?1 LIMIT ?3"));
		if (Stmt.IsValid())
		{
			Stmt.SetBindingValueByIndex(1, Id);
			Stmt.SetBindingValueByIndex(2, DepthCap);
			Stmt.SetBindingValueByIndex(3, InternalCap);
			while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				int32 HopDepth = 1;
				Stmt.GetColumnValueByName(TEXT("hopDepth"), HopDepth);
				const FString Via = ColStr(Stmt, TEXT("viaType"));
				const double Severity = static_cast<double>(EdgeSeverity(Via)) / FMath::Max(1, HopDepth); // closer + heavier = higher
				TSharedRef<FJsonObject> Entity = EntityJson(Stmt);
				ByKind.FindOrAdd(Entity->GetStringField(TEXT("kind")))++;
				Rows.Add(FImpactRow{ Entity, Via, HopDepth, Severity });
			}
		}
		else
		{
			UE_LOG(LogGameIQQuery, Warning, TEXT("Game IQ: impact statement failed to prepare: %s"), *Db.GetLastError());
		}
	}

	Rows.StableSort([](const FImpactRow& A, const FImpactRow& B) { return A.Severity > B.Severity; });

	const int32 TotalDependents = Rows.Num();
	TArray<TSharedPtr<FJsonValue>> ResultRows;
	for (int32 i = 0; i < FMath::Min(TotalDependents, OutCap); ++i)
	{
		const FImpactRow& I = Rows[i];
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetObjectField(TEXT("entity"), I.Entity);
		O->SetStringField(TEXT("viaType"), I.ViaType);
		O->SetNumberField(TEXT("depth"), I.Depth);
		O->SetNumberField(TEXT("severity"), I.Severity);
		ResultRows.Add(MakeShared<FJsonValueObject>(O));
	}

	ByKind.ValueSort([](int32 A, int32 B) { return A > B; });
	TSharedRef<FJsonObject> ByKindJson = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& P : ByKind) { ByKindJson->SetNumberField(P.Key, P.Value); }

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	Payload->SetArrayField(TEXT("impacted"), ResultRows);
	Payload->SetNumberField(TEXT("totalDependents"), TotalDependents);
	Payload->SetObjectField(TEXT("byKind"), ByKindJson);
	Payload->SetBoolField(TEXT("truncated"), TotalDependents > OutCap || TotalDependents >= InternalCap);
	return Envelope(Db, MakeShared<FJsonValueObject>(Payload));
}

FString GameIQQuery::Explain(const FString& Topic, int32 Limit)
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); };

	const int32 Seeds = Limit <= 0 ? 8 : Limit;

	// Overfetch, then cap seeds per entity kind: on intent-shaped topics one chatty kind
	// (e.g. dozens of placed actors) would otherwise fill every seed slot and the bundle
	// never reaches the blueprint/code/asset entities that actually explain the system.
	// Score order is preserved within the cap; leftover slots backfill from the overflow.
	TArray<FSearchHit> Ranked = BuildSearchHits(Db, Topic, FString(), Seeds * 3, 0);
	const int32 MaxPerKind = FMath::Max(2, Seeds / 3);
	TArray<FSearchHit> Hits, Overflow;
	TMap<FString, int32> PerKind;
	for (const FSearchHit& H : Ranked)
	{
		if (Hits.Num() >= Seeds) { break; }
		int32& N = PerKind.FindOrAdd(H.Entity->GetStringField(TEXT("kind")));
		if (N < MaxPerKind) { ++N; Hits.Add(H); }
		else { Overflow.Add(H); }
	}
	for (const FSearchHit& H : Overflow)
	{
		if (Hits.Num() >= Seeds) { break; }
		Hits.Add(H);
	}

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
		for (const FRefRow& R : BuildReferences(Db, SeedId, TEXT("both"), 1, FString(), FString(), 200, 0))
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
	TSharedRef<FJsonObject> Overview = MakeShared<FJsonObject>();
	Overview->SetNumberField(TEXT("totalEntities"), (double)ScalarInt(Db, TEXT("SELECT COUNT(*) FROM entities")));
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

FString GameIQQuery::Doctor()
{
	FSQLiteDatabase Db;
	FString OpenErr;
	if (!OpenIndex(Db, OpenErr)) { return ErrorJson(OpenErr); }
	ON_SCOPE_EXIT { Db.Close(); };

	TSharedRef<FJsonObject> Payload = MakeShared<FJsonObject>();
	TArray<FString> Problems;

	// Schema: version stamp AND the tables it implies (a stamp with missing tables was exactly
	// the failure mode that shipped empty indexes — distinguish "empty project" from "broken DB").
	int32 UserVersion = 0;
	Db.GetUserVersion(UserVersion);
	Payload->SetNumberField(TEXT("schemaVersion"), UserVersion);

	TArray<FString> MissingTables;
	for (const TCHAR* Table : { TEXT("meta"), TEXT("entities"), TEXT("edges"), TEXT("chunks"), TEXT("chunks_fts") })
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("SELECT 1 FROM sqlite_master WHERE name = ?"));
		if (!Stmt.IsValid()) { MissingTables.Add(Table); continue; }
		Stmt.SetBindingValueByIndex(1, FString(Table));
		if (Stmt.Step() != ESQLitePreparedStatementStepResult::Row) { MissingTables.Add(Table); }
	}
	if (MissingTables.Num() > 0)
	{
		Problems.Add(FString::Printf(TEXT("missing tables: %s — the index is broken; run GameIQ.Rebuild (the store will self-heal the schema)"),
			*FString::Join(MissingTables, TEXT(", "))));
	}

	// Journal mode (expected: delete — WAL is unavailable under UE's SQLite VFS, no xShm support).
	{
		FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT("PRAGMA journal_mode"));
		if (Stmt.IsValid() && Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
		{
			FString Mode;
			Stmt.GetColumnValueByIndex(0, Mode);
			Payload->SetStringField(TEXT("journalMode"), Mode);
		}
	}

	if (MissingTables.Num() == 0)
	{
		const int64 Entities = ScalarInt(Db, TEXT("SELECT COUNT(*) FROM entities"));
		const int64 Edges = ScalarInt(Db, TEXT("SELECT COUNT(*) FROM edges"));
		const int64 Chunks = ScalarInt(Db, TEXT("SELECT COUNT(*) FROM chunks"));
		Payload->SetNumberField(TEXT("entities"), (double)Entities);
		Payload->SetNumberField(TEXT("edges"), (double)Edges);
		Payload->SetNumberField(TEXT("chunks"), (double)Chunks);
		if (Entities == 0)
		{
			Problems.Add(TEXT("the index has zero entities — run GameIQ.Rebuild"));
		}

		// FTS consistency: external-content FTS must track the chunks table 1:1 via triggers.
		const int64 FtsRows = ScalarInt(Db, TEXT("SELECT COUNT(*) FROM chunks_fts"));
		Payload->SetNumberField(TEXT("ftsRows"), (double)FtsRows);
		if (FtsRows != Chunks)
		{
			Problems.Add(FString::Printf(TEXT("FTS index out of sync (%lld fts rows vs %lld chunks) — run GameIQ.Rebuild full"), FtsRows, Chunks));
		}

		Payload->SetArrayField(TEXT("byProducer"), SelectRows(Db,
			TEXT("SELECT COALESCE(producer,'(none)') AS producer, COUNT(*) AS count FROM entities GROUP BY producer ORDER BY count DESC"),
			{ TEXT("producer") }, { TEXT("count") }));

		// Level coverage: actor totals stated in each level's summary chunk (assets stage) vs the
		// level-actor entities actually indexed under that map. Listing fewer than total is by
		// design (the entity cap), but ZERO indexed actors on a populated map means the map's deep
		// extraction never ran — the exact failure that silently hid a level's lighting. "healthy"
		// must not paper over that.
		{
			TArray<TSharedPtr<FJsonValue>> Levels;
			FSQLitePreparedStatement Stmt = Db.PrepareStatement(TEXT(
				"SELECT c.entity_id AS id, c.text AS text,"
				" (SELECT COUNT(*) FROM entities k WHERE k.parent = c.entity_id) AS indexed"
				" FROM chunks c WHERE c.kind = 'recipe-summary' AND c.text LIKE '%(Level)%Actors:%'"));
			if (Stmt.IsValid())
			{
				while (Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
				{
					const FString MapId = ColStr(Stmt, TEXT("id"));
					const FString Text = ColStr(Stmt, TEXT("text"));
					int32 Indexed = 0;
					Stmt.GetColumnValueByName(TEXT("indexed"), Indexed);

					int32 Actors = 0;
					const int32 At = Text.Find(TEXT("Actors: "));
					if (At != INDEX_NONE) { Actors = FCString::Atoi(*Text.Mid(At + 8)); }

					TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
					Row->SetStringField(TEXT("map"), MapId);
					Row->SetNumberField(TEXT("actors"), Actors);
					Row->SetNumberField(TEXT("indexedActors"), Indexed);
					Levels.Add(MakeShared<FJsonValueObject>(Row));
					if (Actors > 0 && Indexed == 0)
					{
						Problems.Add(FString::Printf(
							TEXT("%s reports %d actors but has no indexed level-actor entities — run GameIQ.Rebuild full"),
							*MapId, Actors));
					}
				}
			}
			if (Levels.Num() > 0)
			{
				Payload->SetArrayField(TEXT("levels"), Levels);
			}
		}
	}

	Payload->SetField(TEXT("dbGeneration"), NullOrString(MetaValue(Db, TEXT("dbGeneration"))));
	Payload->SetField(TEXT("lastIngestAtIso"), NullOrString(MetaValue(Db, TEXT("lastIngestAtIso"))));
	Payload->SetField(TEXT("lastGeneratedAtIso"), NullOrString(MetaValue(Db, TEXT("lastGeneratedAtIso"))));

	Payload->SetBoolField(TEXT("healthy"), Problems.Num() == 0);
	TArray<TSharedPtr<FJsonValue>> ProblemsJson;
	for (const FString& P : Problems) { ProblemsJson.Add(MakeShared<FJsonValueString>(P)); }
	Payload->SetArrayField(TEXT("problems"), ProblemsJson);
	return Envelope(Db, MakeShared<FJsonValueObject>(Payload));
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
