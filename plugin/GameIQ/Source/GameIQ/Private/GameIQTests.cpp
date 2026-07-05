// Copyright Buckley Builds LLC. All Rights Reserved.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameIQStore.h"
#include "GameIQTextUtil.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "SQLiteDatabase.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace GameIQTestUtil
{
	FString TempDbPath(const TCHAR* Name)
	{
		const FString Dir = FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("GameIQTests"));
		IFileManager::Get().MakeDirectory(*Dir, /*Tree=*/true);
		const FString Path = FPaths::Combine(Dir, Name);
		IFileManager::Get().Delete(*Path, false, true, true);
		IFileManager::Get().Delete(*(Path + TEXT("-wal")), false, true, true);
		IFileManager::Get().Delete(*(Path + TEXT("-shm")), false, true, true);
		return Path;
	}

	TSharedPtr<FJsonValue> Entity(const TCHAR* Id, const TCHAR* Kind, const TCHAR* Name, const TCHAR* Parent = nullptr)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), Id);
		O->SetStringField(TEXT("kind"), Kind);
		O->SetStringField(TEXT("name"), Name);
		O->SetStringField(TEXT("path"), TEXT("/Game/Test"));
		O->SetStringField(TEXT("source"), TEXT("asset"));
		if (Parent) { O->SetStringField(TEXT("parent"), Parent); }
		return MakeShared<FJsonValueObject>(O);
	}

	TSharedPtr<FJsonValue> Edge(const TCHAR* Src, const TCHAR* Dst, const TCHAR* Type)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("src"), Src);
		O->SetStringField(TEXT("dst"), Dst);
		O->SetStringField(TEXT("type"), Type);
		return MakeShared<FJsonValueObject>(O);
	}

	TSharedPtr<FJsonValue> Chunk(const TCHAR* Id, const TCHAR* EntityId, const TCHAR* Text)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), Id);
		O->SetStringField(TEXT("entityId"), EntityId);
		O->SetStringField(TEXT("kind"), TEXT("recipe-summary"));
		O->SetStringField(TEXT("text"), Text);
		return MakeShared<FJsonValueObject>(O);
	}

	int64 CountRows(const FString& DbPath, const TCHAR* Sql)
	{
		FSQLiteDatabase Db;
		if (!Db.Open(*DbPath, ESQLiteDatabaseOpenMode::ReadOnly)) { return -1; }
		int64 N = -1;
		{
			FSQLitePreparedStatement Stmt(Db, Sql);
			if (Stmt.IsValid() && Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				Stmt.GetColumnValueByIndex(0, N);
			}
		}
		Db.Close();
		return N;
	}
}

/**
 * Fresh-DB schema creation. This is the regression test for the July 2026 empty-index bug:
 * FSQLiteDatabase::Execute runs only the FIRST statement of a multi-statement string, so the
 * old multi-statement migration created just `meta`, stamped user_version anyway, and every
 * insert/query silently failed. Verifies every table (incl. FTS) really exists on a new file.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameIQStoreSchemaTest, "GameIQ.Store.SchemaCreation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGameIQStoreSchemaTest::RunTest(const FString& Parameters)
{
	const FString DbPath = GameIQTestUtil::TempDbPath(TEXT("schema.db"));

	FGameIQStore Store;
	TestTrue(TEXT("fresh DB opens"), Store.OpenAtPath(DbPath));
	TestFalse(TEXT("dbGeneration stamped"), Store.GetMeta(TEXT("dbGeneration")).IsEmpty());
	Store.Close();

	for (const TCHAR* Table : { TEXT("meta"), TEXT("entities"), TEXT("edges"), TEXT("chunks"), TEXT("chunks_fts") })
	{
		const FString Sql = FString::Printf(TEXT("SELECT COUNT(*) FROM sqlite_master WHERE name = '%s'"), Table);
		TestEqual(FString::Printf(TEXT("table %s exists"), Table), GameIQTestUtil::CountRows(DbPath, *Sql), (int64)1);
	}

	// Re-open is idempotent (schema already valid, no self-heal churn) and the generation persists.
	FGameIQStore Store2;
	TestTrue(TEXT("re-open succeeds"), Store2.OpenAtPath(DbPath));
	const FString Gen = Store2.GetMeta(TEXT("dbGeneration"));
	Store2.Close();
	TestFalse(TEXT("generation stable across reopen"), Gen.IsEmpty());
	return true;
}

/** Ingest round-trip: rows land, producer replace doesn't duplicate, FTS triggers stay in sync
 *  and porter stemming matches inflected query terms. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameIQStoreIngestTest, "GameIQ.Store.IngestRoundtrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGameIQStoreIngestTest::RunTest(const FString& Parameters)
{
	using namespace GameIQTestUtil;
	const FString DbPath = TempDbPath(TEXT("ingest.db"));

	FGameIQStore Store;
	if (!TestTrue(TEXT("open"), Store.OpenAtPath(DbPath))) { return false; }

	const TArray<TSharedPtr<FJsonValue>> Entities = {
		Entity(TEXT("asset:/Game/Test/BP_PlayerCharacter"), TEXT("blueprint"), TEXT("BP_PlayerCharacter")),
		Entity(TEXT("asset:/Game/Test/BP_PlayerCharacter::var::Health"), TEXT("bp-variable"), TEXT("Health"), TEXT("asset:/Game/Test/BP_PlayerCharacter")),
	};
	const TArray<TSharedPtr<FJsonValue>> Edges = {
		Edge(TEXT("asset:/Game/Test/BP_PlayerCharacter"), TEXT("cpp:Character"), TEXT("inherits")),
	};
	const TArray<TSharedPtr<FJsonValue>> Chunks = {
		Chunk(TEXT("c1"), TEXT("asset:/Game/Test/BP_PlayerCharacter"), TEXT("BP_PlayerCharacter handles reloading the weapon")),
		Chunk(TEXT("c2"), TEXT("asset:/Game/Test/BP_PlayerCharacter::var::Health"), TEXT("float Health = 100")),
	};
	TestTrue(TEXT("ingest succeeds"), Store.IngestProducer(TEXT("test-producer"), Entities, Edges, Chunks));

	int64 E = 0, Ed = 0, C = 0;
	Store.GetCounts(E, Ed, C);
	TestEqual(TEXT("entities"), E, (int64)2);
	TestEqual(TEXT("edges"), Ed, (int64)1);
	TestEqual(TEXT("chunks"), C, (int64)2);

	// Re-ingesting the same producer replaces, not duplicates.
	TestTrue(TEXT("re-ingest succeeds"), Store.IngestProducer(TEXT("test-producer"), Entities, Edges, Chunks));
	Store.GetCounts(E, Ed, C);
	TestEqual(TEXT("entities unchanged after re-ingest"), E, (int64)2);
	TestEqual(TEXT("chunks unchanged after re-ingest"), C, (int64)2);
	Store.Close();

	// FTS triggers: index tracks the chunks table 1:1.
	TestEqual(TEXT("fts rows"), CountRows(DbPath, TEXT("SELECT COUNT(*) FROM chunks_fts")), (int64)2);
	// Porter stemming: "reload" matches "reloading".
	TestEqual(TEXT("stemmed match"), CountRows(DbPath,
		TEXT("SELECT COUNT(*) FROM chunks_fts WHERE chunks_fts MATCH '\"reload\"'")), (int64)1);
	// Aux tokens: "player" matches BP_PlayerCharacter via camel/underscore splitting — both chunks,
	// because the variable chunk's entity id also carries the owner's identifier.
	TestEqual(TEXT("aux identifier match"), CountRows(DbPath,
		TEXT("SELECT COUNT(*) FROM chunks_fts WHERE chunks_fts MATCH '\"player\"'")), (int64)2);
	return true;
}

/** Producer-scoped delta: replacing one producer's subtree rows must leave other producers'
 *  rows on the same entities untouched (registry root entity survives an assets delta). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameIQStorePatchTest, "GameIQ.Store.ProducerScopedPatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGameIQStorePatchTest::RunTest(const FString& Parameters)
{
	using namespace GameIQTestUtil;
	const FString DbPath = TempDbPath(TEXT("patch.db"));

	FGameIQStore Store;
	if (!TestTrue(TEXT("open"), Store.OpenAtPath(DbPath))) { return false; }

	// Registry owns the root entity + a depends-on edge; assets owns a chunk + a child entity.
	Store.IngestProducer(TEXT("registry"),
		{ Entity(TEXT("asset:/Game/Test/Mesh"), TEXT("asset"), TEXT("Mesh")) },
		{ Edge(TEXT("asset:/Game/Test/Mesh"), TEXT("asset:/Game/Test/Mat"), TEXT("depends-on")) },
		{});
	Store.IngestProducer(TEXT("assets"),
		{ Entity(TEXT("asset:/Game/Test/Mesh::sub"), TEXT("level-actor"), TEXT("Sub"), TEXT("asset:/Game/Test/Mesh")) },
		{ Edge(TEXT("asset:/Game/Test/Mesh"), TEXT("asset:/Game/Test/Skel"), TEXT("uses-skeleton")) },
		{ Chunk(TEXT("mesh#summary"), TEXT("asset:/Game/Test/Mesh"), TEXT("old summary")) });

	// Assets delta replaces the Mesh subtree with a new chunk (no child this time).
	TestTrue(TEXT("delta patch succeeds"), Store.PatchProducerScoped(
		{ TEXT("asset:/Game/Test/Mesh") },
		{},
		{},
		{ Chunk(TEXT("mesh#summary"), TEXT("asset:/Game/Test/Mesh"), TEXT("new summary")) },
		TEXT("assets")));
	Store.Close();

	// Registry's rows survive; assets' old child + edge are gone; new chunk is in.
	TestEqual(TEXT("registry root entity survives"), CountRows(DbPath,
		TEXT("SELECT COUNT(*) FROM entities WHERE id = 'asset:/Game/Test/Mesh'")), (int64)1);
	TestEqual(TEXT("registry edge survives"), CountRows(DbPath,
		TEXT("SELECT COUNT(*) FROM edges WHERE type = 'depends-on'")), (int64)1);
	TestEqual(TEXT("assets child entity replaced away"), CountRows(DbPath,
		TEXT("SELECT COUNT(*) FROM entities WHERE id = 'asset:/Game/Test/Mesh::sub'")), (int64)0);
	TestEqual(TEXT("assets edge replaced away"), CountRows(DbPath,
		TEXT("SELECT COUNT(*) FROM edges WHERE type = 'uses-skeleton'")), (int64)0);
	TestEqual(TEXT("new chunk text"), CountRows(DbPath,
		TEXT("SELECT COUNT(*) FROM chunks WHERE text = 'new summary'")), (int64)1);

	// Full (non-scoped) patch with an empty delta drops the whole subtree — the delete path.
	FGameIQStore Store2;
	if (!TestTrue(TEXT("reopen"), Store2.OpenAtPath(DbPath))) { return false; }
	TestTrue(TEXT("delete patch succeeds"), Store2.Patch({ TEXT("asset:/Game/Test/Mesh") }, {}, {}, {}, TEXT("assets")));
	Store2.Close();
	TestEqual(TEXT("entity fully removed"), CountRows(DbPath,
		TEXT("SELECT COUNT(*) FROM entities WHERE id = 'asset:/Game/Test/Mesh'")), (int64)0);
	TestEqual(TEXT("no orphan chunks"), CountRows(DbPath,
		TEXT("SELECT COUNT(*) FROM chunks WHERE entity_id = 'asset:/Game/Test/Mesh'")), (int64)0);
	return true;
}

/** Text utilities: camel splitting, FTS query building, aux token generation. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameIQTextUtilTest, "GameIQ.Text.Utilities",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGameIQTextUtilTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("SplitCamel basic"), GameIQText::SplitCamel(TEXT("DirectionalLight")), FString(TEXT("Directional Light")));
	TestEqual(TEXT("SplitCamel acronym run kept"), GameIQText::SplitCamel(TEXT("HUDWidget")), FString(TEXT("HUDWidget")));
	TestEqual(TEXT("SplitCamel plain word"), GameIQText::SplitCamel(TEXT("light")), FString(TEXT("light")));

	TestEqual(TEXT("AND query"), GameIQText::BuildFtsQuery(TEXT("reload logic"), TEXT("AND")),
		FString(TEXT("\"reload\" AND \"logic\"")));
	TestEqual(TEXT("OR query"), GameIQText::BuildFtsQuery(TEXT("reload logic"), TEXT("OR")),
		FString(TEXT("\"reload\" OR \"logic\"")));
	TestEqual(TEXT("prefix query"), GameIQText::BuildFtsQuery(TEXT("relo"), TEXT("OR"), true),
		FString(TEXT("\"relo\"*")));
	TestEqual(TEXT("metacharacters neutralized"), GameIQText::BuildFtsQuery(TEXT("a-b (c)"), TEXT("AND")),
		FString(TEXT("\"a\" AND \"b\" AND \"c\"")));
	TestTrue(TEXT("empty input"), GameIQText::BuildFtsQuery(TEXT("!!!"), TEXT("AND")).IsEmpty());

	const FString Aux = GameIQText::BuildAuxTokens(
		TEXT("asset:/Game/Test/BP_PlayerCharacter"), TEXT("BP_PlayerCharacter (Blueprint)"));
	TestTrue(TEXT("aux splits identifier"), Aux.Contains(TEXT("Player Character")));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
