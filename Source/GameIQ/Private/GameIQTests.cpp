// Copyright Buckley Builds LLC. All Rights Reserved.

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameIQCppParse.h"
#include "GameIQDocText.h"
#include "GameIQQuery.h"
#include "GameIQStore.h"
#include "GameIQTextUtil.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
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

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameIQHtmlDocParseTest, "GameIQ.Docs.HtmlParsing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FGameIQHtmlDocParseTest::RunTest(const FString& Parameters)
{
	const FString Html = TEXT("<!doctype html><html><head><title>Combat &amp; Abilities</title>")
		TEXT("<style>.hidden{display:none}</style><script>ignoreMe()</script></head><body>")
		TEXT("<section><h2>Damage Model</h2><p>Armor reduces damage.</p></section>")
		TEXT("<h3>Status Effects</h3><ul><li>Burn</li><li>Freeze</li></ul></body></html>");
	const GameIQDocText::FParsed Parsed = GameIQDocText::Parse(Html, TEXT("html"));

	TestTrue(TEXT("HTML is supported"), Parsed.bSupported);
	TestEqual(TEXT("format is HTML"), Parsed.Format, FString(TEXT("html")));
	TestEqual(TEXT("title falls back to the HTML title element"), Parsed.Title, FString(TEXT("Combat & Abilities")));
	TestEqual(TEXT("headings create sections"), Parsed.Sections.Num(), 2);
	if (Parsed.Sections.Num() == 2)
	{
		TestEqual(TEXT("H2 heading is retained"), Parsed.Sections[0].Heading, FString(TEXT("Damage Model")));
		TestTrue(TEXT("paragraph text is searchable"), Parsed.Sections[0].Body.Contains(TEXT("Armor reduces damage.")));
		TestEqual(TEXT("H3 heading is retained"), Parsed.Sections[1].Heading, FString(TEXT("Status Effects")));
		TestTrue(TEXT("list text is searchable"), Parsed.Sections[1].Body.Contains(TEXT("Burn")) && Parsed.Sections[1].Body.Contains(TEXT("Freeze")));
	}
	const FString CombinedText = GameIQDocText::HtmlToText(Html);
	TestFalse(TEXT("scripts are omitted"), CombinedText.Contains(TEXT("ignoreMe")));
	TestFalse(TEXT("styles are omitted"), CombinedText.Contains(TEXT("display:none")));
	return true;
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

namespace GameIQTestUtil
{
	/** Parse a query-layer JSON response and return its "result" value (nullptr on failure). */
	TSharedPtr<FJsonValue> ParseResult(const FString& Json)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid()) { return nullptr; }
		return Obj->TryGetField(TEXT("result"));
	}

	FString HitEntityId(const TSharedPtr<FJsonValue>& Hit)
	{
		const TSharedPtr<FJsonObject>* O = nullptr;
		if (!Hit.IsValid() || !Hit->TryGetObject(O)) { return FString(); }
		const TSharedPtr<FJsonObject>* E = nullptr;
		if (!(*O)->TryGetObjectField(TEXT("entity"), E)) { return FString(); }
		return (*E)->GetStringField(TEXT("id"));
	}
}

/**
 * Kind-weighted ranking: a placed actor whose property dump repeats the query term must not
 * outrank a real asset/blueprint entity that matches it once. This is the regression test for
 * the "player controls" failure: Ultra_Dynamic_Weather's "Player Camera Location" dump used to
 * beat every input asset on raw bm25.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameIQQueryRankingTest, "GameIQ.Query.KindWeightedRanking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGameIQQueryRankingTest::RunTest(const FString& Parameters)
{
	using namespace GameIQTestUtil;
	const FString DbPath = TempDbPath(TEXT("ranking.db"));

	FGameIQStore Store;
	if (!TestTrue(TEXT("open"), Store.OpenAtPath(DbPath))) { return false; }
	Store.IngestProducer(TEXT("test"),
		{
			Entity(TEXT("asset:/Game/Maps/L::actor::Weather"), TEXT("level-actor"), TEXT("Weather")),
			Entity(TEXT("asset:/Game/Input/IMC_Controls"), TEXT("asset"), TEXT("IMC_Controls")),
		},
		{},
		{
			// bm25 alone would rank the term-spamming actor dump first.
			Chunk(TEXT("a1"), TEXT("asset:/Game/Maps/L::actor::Weather"),
				TEXT("player camera location player camera rotation player weather player position")),
			Chunk(TEXT("a2"), TEXT("asset:/Game/Input/IMC_Controls"),
				TEXT("player input mapping context")),
		});
	Store.Close();

	GameIQQuery::SetDbPathOverrideForTests(DbPath);
	ON_SCOPE_EXIT { GameIQQuery::SetDbPathOverrideForTests(FString()); };

	const TSharedPtr<FJsonValue> Result = ParseResult(GameIQQuery::Search(TEXT("player"), FString(), 10, 0));
	const TArray<TSharedPtr<FJsonValue>>* Hits = nullptr;
	if (!TestTrue(TEXT("search returns array"), Result.IsValid() && Result->TryGetArray(Hits))) { return false; }
	if (!TestEqual(TEXT("both entities hit"), Hits->Num(), 2)) { return false; }
	TestEqual(TEXT("asset outranks level-actor dump"),
		HitEntityId((*Hits)[0]), FString(TEXT("asset:/Game/Input/IMC_Controls")));
	return true;
}

/** Explain seed diversity: one chatty entity kind must not fill every seed slot. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameIQQueryExplainDiversityTest, "GameIQ.Query.ExplainSeedDiversity",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGameIQQueryExplainDiversityTest::RunTest(const FString& Parameters)
{
	using namespace GameIQTestUtil;
	const FString DbPath = TempDbPath(TEXT("diversity.db"));

	FGameIQStore Store;
	if (!TestTrue(TEXT("open"), Store.OpenAtPath(DbPath))) { return false; }
	TArray<TSharedPtr<FJsonValue>> Entities, Chunks;
	for (int32 i = 0; i < 10; ++i)
	{
		const FString Id = FString::Printf(TEXT("asset:/Game/Maps/L::actor::A%d"), i);
		Entities.Add(Entity(*Id, TEXT("level-actor"), *FString::Printf(TEXT("A%d"), i)));
		// Repeat the term so every actor outscores the blueprint on raw bm25.
		Chunks.Add(Chunk(*FString::Printf(TEXT("c%d"), i), *Id,
			TEXT("player camera player location player rotation")));
	}
	Entities.Add(Entity(TEXT("asset:/Game/BP_Hero"), TEXT("blueprint"), TEXT("BP_Hero")));
	Chunks.Add(Chunk(TEXT("hero"), TEXT("asset:/Game/BP_Hero"), TEXT("hero blueprint with player input")));
	Store.IngestProducer(TEXT("test"), Entities, {}, Chunks);
	Store.Close();

	GameIQQuery::SetDbPathOverrideForTests(DbPath);
	ON_SCOPE_EXIT { GameIQQuery::SetDbPathOverrideForTests(FString()); };

	const TSharedPtr<FJsonValue> Result = ParseResult(GameIQQuery::Explain(TEXT("player")));
	const TSharedPtr<FJsonObject>* Payload = nullptr;
	if (!TestTrue(TEXT("explain returns object"), Result.IsValid() && Result->TryGetObject(Payload))) { return false; }
	const TArray<TSharedPtr<FJsonValue>>* Seeds = nullptr;
	if (!TestTrue(TEXT("seeds array"), (*Payload)->TryGetArrayField(TEXT("seeds"), Seeds))) { return false; }

	bool bHasBlueprint = false;
	for (const TSharedPtr<FJsonValue>& S : *Seeds)
	{
		if (HitEntityId(S) == TEXT("asset:/Game/BP_Hero")) { bHasBlueprint = true; }
	}
	TestTrue(TEXT("blueprint seeded despite 10 stronger level-actor hits"), bHasBlueprint);
	return true;
}

/** .cpp parsing: definitions (incl. ctor init lists) yield bodies, call sites don't, and
 *  Enhanced Input BindAction wiring is extracted with property/trigger/handler intact. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameIQCppParseTest, "GameIQ.Cpp.BodyAndBindingParse",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGameIQCppParseTest::RunTest(const FString& Parameters)
{
	const FString Cpp = TEXT(R"cpp(
#include "MyCharacter.h"

AMyCharacter::AMyCharacter()
	: Super(FObjectInitializer::Get())
{
	PrimaryActorTick.bCanEverTick = true;
}

void AMyCharacter::BeginPlay()
{
	Super::BeginPlay();
	const bool B = bFlag ? AMyCharacter::StaticHelper(1) : false;
}

void AMyCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	if (UEnhancedInputComponent* Input = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		Input->BindAction(MovementAction, ETriggerEvent::Triggered, this, &AMyCharacter::Move);
		Input->BindAction(JumpAction, ETriggerEvent::Started, this, &AMyCharacter::Jump);
		Input->BindAction(JumpAction, ETriggerEvent::Completed, this, &AMyCharacter::StopJump);
	}
}

void AMyCharacter::Move(const FInputActionValue& Value)
{
	AddMovementInput(GetActorForwardVector(), Value.Get<float>());
}
)cpp");

	TSet<FString> Known;
	Known.Add(TEXT("AMyCharacter"));
	TArray<GameIQCpp::FCppBody> Bodies;
	TArray<GameIQCpp::FInputBindingSite> Bindings;
	GameIQCpp::ParseCppBodies(Cpp, Known, Bodies, Bindings);

	TArray<FString> Names;
	for (const GameIQCpp::FCppBody& B : Bodies) { Names.Add(B.Func); }
	TestTrue(TEXT("ctor body found (init list)"), Names.Contains(TEXT("AMyCharacter")));
	TestTrue(TEXT("BeginPlay body found"), Names.Contains(TEXT("BeginPlay")));
	TestTrue(TEXT("Move body found"), Names.Contains(TEXT("Move")));
	TestFalse(TEXT("ternary call site is not a definition"), Names.Contains(TEXT("StaticHelper")));
	TestEqual(TEXT("definition count"), Bodies.Num(), 4); // ctor, BeginPlay, SetupPlayerInputComponent, Move

	for (const GameIQCpp::FCppBody& B : Bodies)
	{
		if (B.Func == TEXT("Move"))
		{
			TestTrue(TEXT("body text captured"), B.Body.Contains(TEXT("AddMovementInput")));
		}
	}

	if (!TestEqual(TEXT("three bindings"), Bindings.Num(), 3)) { return false; }
	TestEqual(TEXT("bind prop"), Bindings[0].Prop, FString(TEXT("MovementAction")));
	TestEqual(TEXT("bind trigger"), Bindings[0].Trigger, FString(TEXT("Triggered")));
	TestEqual(TEXT("bind class"), Bindings[0].ClassName, FString(TEXT("AMyCharacter")));
	TestEqual(TEXT("bind handler"), Bindings[0].Handler, FString(TEXT("Move")));
	TestEqual(TEXT("completed pair kept as separate call site"), Bindings[2].Handler, FString(TEXT("StopJump")));
	return true;
}

/** Best-effort call extraction: the four unambiguous shapes resolve against indexed functions
 *  (incl. through the ancestor chain), and comments/strings/unknown receivers never produce
 *  callees. Also covers param capture in ParseCppBodies and param-type resolution. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameIQCppCallExtractTest, "GameIQ.Cpp.CallExtraction",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGameIQCppCallExtractTest::RunTest(const FString& Parameters)
{
	using namespace GameIQCpp;

	// ParseCppBodies captures the raw parameter list.
	{
		TSet<FString> Known;
		Known.Add(TEXT("AMyChar"));
		TArray<FCppBody> Bodies;
		TArray<FInputBindingSite> Bindings;
		ParseCppBodies(TEXT("void AMyChar::Foo(AWeapon* Weapon, int32 Count)\n{\n\tWeapon->Equip();\n}\n"),
			Known, Bodies, Bindings);
		if (!TestEqual(TEXT("one body"), Bodies.Num(), 1)) { return false; }
		TestEqual(TEXT("params captured"), Bodies[0].Params.TrimStartAndEnd(),
			FString(TEXT("AWeapon* Weapon, int32 Count")));
	}

	// ParseParamTypes: known types resolve (incl. template inner), defaults are stripped.
	{
		TSet<FString> KnownCanonical;
		KnownCanonical.Add(TEXT("Weapon"));
		TMap<FString, FString> Out;
		ParseParamTypes(TEXT("USceneComponent* InParent, AWeapon* Weapon, const FString& Name = TEXT(\"x\"), TArray<AWeapon*> Extra"),
			KnownCanonical, Out);
		TestEqual(TEXT("two resolvable params"), Out.Num(), 2);
		TestEqual(TEXT("pointer param"), Out.FindRef(TEXT("Weapon")), FString(TEXT("Weapon")));
		TestEqual(TEXT("template inner param"), Out.FindRef(TEXT("Extra")), FString(TEXT("Weapon")));
	}

	FCallResolver R;
	R.BaseOf.Add(TEXT("Character"), TEXT("BaseCharacter"));
	R.FunctionsOf.Add(TEXT("Character"), { TEXT("Equip"), TEXT("PickupWeapon"), TEXT("PlayEquipMontage") });
	R.FunctionsOf.Add(TEXT("BaseCharacter"), { TEXT("AttachWeaponToSocket") });
	R.FunctionsOf.Add(TEXT("Weapon"), { TEXT("Equip"), TEXT("Drop") });
	R.PropTypeOf.FindOrAdd(TEXT("Character")).Add(TEXT("EquippedWeapon"), TEXT("Weapon"));

	TMap<FString, FString> ParamTypes;
	ParamTypes.Add(TEXT("Weapon"), TEXT("Weapon"));

	const FString Body = TEXT(R"cpp({
	// PickupWeapon(in a comment) must not match
	const FString S = TEXT("Drop(in a string) must not match");
	PickupWeapon(OverlappingWeapon);                       // bare, own class
	AttachWeaponToSocket(EquippedWeapon, FName("Sock"));   // bare, inherited
	Super::AttachWeaponToSocket(nullptr, Name);            // Super::
	ACharacter::PlayEquipMontage(Name);                    // qualified
	this->Equip();                                         // this->
	EquippedWeapon->Drop();                                // UPROPERTY-typed receiver
	Weapon->Equip(GetMesh(), Name, this, this);            // parameter-typed receiver
	Other->Drop();                                         // unknown receiver: dropped
	GetOwner()->Equip();                                   // chained receiver: dropped
	UnknownFn(1);                                          // not indexed: dropped
})cpp");

	TSet<FString> Callees;
	ExtractCalls(Body, TEXT("Character"), ParamTypes, R, Callees);

	TestTrue(TEXT("bare own-class call"), Callees.Contains(TEXT("Character::PickupWeapon")));
	TestTrue(TEXT("bare inherited call resolves to declaring class"), Callees.Contains(TEXT("BaseCharacter::AttachWeaponToSocket")));
	TestTrue(TEXT("qualified call"), Callees.Contains(TEXT("Character::PlayEquipMontage")));
	TestTrue(TEXT("this-> call"), Callees.Contains(TEXT("Character::Equip")));
	TestTrue(TEXT("property-typed member call"), Callees.Contains(TEXT("Weapon::Drop")));
	TestTrue(TEXT("parameter-typed member call"), Callees.Contains(TEXT("Weapon::Equip")));
	TestEqual(TEXT("no comment/string/unknown-receiver callees"), Callees.Num(), 6);
	return true;
}

/**
 * Reference-weighted ranking: between two entities with identical text matches, the one the
 * project actually references (e.g. the IMC eleven hero blueprints bind) must outrank the one
 * nothing uses (the bird pawn's IMC).
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameIQQueryRefWeightTest, "GameIQ.Query.ReferenceWeightedRanking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGameIQQueryRefWeightTest::RunTest(const FString& Parameters)
{
	using namespace GameIQTestUtil;
	const FString DbPath = TempDbPath(TEXT("refweight.db"));

	FGameIQStore Store;
	if (!TestTrue(TEXT("open"), Store.OpenAtPath(DbPath))) { return false; }
	TArray<TSharedPtr<FJsonValue>> Edges;
	for (int32 i = 0; i < 8; ++i)
	{
		Edges.Add(Edge(*FString::Printf(TEXT("asset:/Game/Heroes/BP_Hero%d"), i),
			TEXT("asset:/Game/Input/IMC_Popular"), TEXT("binds-input")));
	}
	Store.IngestProducer(TEXT("test"),
		{
			// Insertion order favors the lonely one so the test fails without the boost.
			Entity(TEXT("asset:/Game/Input/IMC_Lonely"), TEXT("asset"), TEXT("IMC_Lonely")),
			Entity(TEXT("asset:/Game/Input/IMC_Popular"), TEXT("asset"), TEXT("IMC_Popular")),
		},
		Edges,
		{
			// Identical chunk text: bm25 and the name boost tie; only references differ.
			Chunk(TEXT("c1"), TEXT("asset:/Game/Input/IMC_Lonely"), TEXT("player input mapping context")),
			Chunk(TEXT("c2"), TEXT("asset:/Game/Input/IMC_Popular"), TEXT("player input mapping context")),
		});
	Store.Close();

	GameIQQuery::SetDbPathOverrideForTests(DbPath);
	ON_SCOPE_EXIT { GameIQQuery::SetDbPathOverrideForTests(FString()); };

	const TSharedPtr<FJsonValue> Result = ParseResult(GameIQQuery::Search(TEXT("player input"), FString(), 10, 0));
	const TArray<TSharedPtr<FJsonValue>>* Hits = nullptr;
	if (!TestTrue(TEXT("search returns array"), Result.IsValid() && Result->TryGetArray(Hits))) { return false; }
	if (!TestEqual(TEXT("both entities hit"), Hits->Num(), 2)) { return false; }
	TestEqual(TEXT("referenced asset outranks unreferenced twin"),
		HitEntityId((*Hits)[0]), FString(TEXT("asset:/Game/Input/IMC_Popular")));
	const TSharedPtr<FJsonObject>* Top = nullptr;
	if (TestTrue(TEXT("hit object"), (*Hits)[0]->TryGetObject(Top)))
	{
		TestEqual(TEXT("inboundRefs reported"), (int32)(*Top)->GetNumberField(TEXT("inboundRefs")), 8);
	}
	return true;
}

/**
 * Children paging + class rollup: a level's capped children must round-robin across classes
 * (never 50 rocks), GetEntity must report a full childrenByClass rollup, and Children(id, filter)
 * must page one class. Regression test for the "969-actor map hid its one sky actor" failure.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGameIQQueryChildrenTest, "GameIQ.Query.ChildrenRollupAndFilter",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGameIQQueryChildrenTest::RunTest(const FString& Parameters)
{
	using namespace GameIQTestUtil;
	const FString DbPath = TempDbPath(TEXT("children.db"));

	auto ActorEntity = [](const FString& Id, const FString& Name, const FString& Class, const TCHAR* Parent)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), Id);
		O->SetStringField(TEXT("kind"), TEXT("level-actor"));
		O->SetStringField(TEXT("name"), Name);
		O->SetStringField(TEXT("path"), TEXT("/Game/Test"));
		O->SetStringField(TEXT("source"), TEXT("asset"));
		O->SetStringField(TEXT("parent"), Parent);
		TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
		D->SetStringField(TEXT("class"), Class);
		O->SetObjectField(TEXT("detail"), D);
		return MakeShared<FJsonValueObject>(O);
	};

	const TCHAR* Map = TEXT("asset:/Game/Maps/L");
	FGameIQStore Store;
	if (!TestTrue(TEXT("open"), Store.OpenAtPath(DbPath))) { return false; }
	TArray<TSharedPtr<FJsonValue>> Entities;
	Entities.Add(Entity(Map, TEXT("asset"), TEXT("L")));
	for (int32 i = 0; i < 5; ++i)
	{
		Entities.Add(ActorEntity(FString::Printf(TEXT("%s::actor::Rock%d"), Map, i),
			FString::Printf(TEXT("Rock%d"), i), TEXT("StaticMeshActor"), Map));
	}
	Entities.Add(ActorEntity(FString::Printf(TEXT("%s::actor::Sky"), Map), TEXT("Sky"), TEXT("Ultra_Dynamic_Sky_C"), Map));
	Entities.Add(ActorEntity(FString::Printf(TEXT("%s::actor::Lamp"), Map), TEXT("Lamp"), TEXT("PointLight"), Map));
	Store.IngestProducer(TEXT("test"), Entities, {}, {});
	Store.Close();

	GameIQQuery::SetDbPathOverrideForTests(DbPath);
	ON_SCOPE_EXIT { GameIQQuery::SetDbPathOverrideForTests(FString()); };

	// GetEntity with a cap of 3: round-robin must show all three classes, not three rocks.
	{
		const TSharedPtr<FJsonValue> Result = ParseResult(GameIQQuery::GetEntity(Map, /*Cap=*/3));
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (!TestTrue(TEXT("entity payload"), Result.IsValid() && Result->TryGetObject(Payload))) { return false; }
		const TArray<TSharedPtr<FJsonValue>>* Kids = nullptr;
		if (!TestTrue(TEXT("children array"), (*Payload)->TryGetArrayField(TEXT("children"), Kids))) { return false; }
		if (!TestEqual(TEXT("children capped"), Kids->Num(), 3)) { return false; }
		TSet<FString> Classes;
		for (const TSharedPtr<FJsonValue>& K : *Kids)
		{
			Classes.Add(K->AsObject()->GetObjectField(TEXT("detail"))->GetStringField(TEXT("class")));
		}
		TestEqual(TEXT("one child per class under the cap"), Classes.Num(), 3);

		const TArray<TSharedPtr<FJsonValue>>* Rollup = nullptr;
		if (!TestTrue(TEXT("childrenByClass present"), (*Payload)->TryGetArrayField(TEXT("childrenByClass"), Rollup))) { return false; }
		TestEqual(TEXT("rollup covers all classes"), Rollup->Num(), 3);
		TestEqual(TEXT("rollup ordered by count"),
			(*Rollup)[0]->AsObject()->GetStringField(TEXT("class")), FString(TEXT("StaticMeshActor")));
		TestEqual(TEXT("rollup counts full set"),
			(int32)(*Rollup)[0]->AsObject()->GetNumberField(TEXT("count")), 5);
		TestTrue(TEXT("truncation note present"), (*Payload)->HasField(TEXT("note")));
	}

	// Children with a class filter pages exactly that class.
	{
		const TSharedPtr<FJsonValue> Result = ParseResult(GameIQQuery::Children(Map, TEXT("Light"), 50, 0));
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (!TestTrue(TEXT("children payload"), Result.IsValid() && Result->TryGetObject(Payload))) { return false; }
		TestEqual(TEXT("filter total"), (int32)(*Payload)->GetNumberField(TEXT("total")), 1);
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!TestTrue(TEXT("rows array"), (*Payload)->TryGetArrayField(TEXT("children"), Rows))) { return false; }
		if (!TestEqual(TEXT("one light"), Rows->Num(), 1)) { return false; }
		TestEqual(TEXT("light name"), (*Rows)[0]->AsObject()->GetStringField(TEXT("name")), FString(TEXT("Lamp")));
	}

	// Unfiltered Children reports the full child count even when the page is small.
	{
		const TSharedPtr<FJsonValue> Result = ParseResult(GameIQQuery::Children(Map, FString(), 2, 0));
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (!TestTrue(TEXT("children payload"), Result.IsValid() && Result->TryGetObject(Payload))) { return false; }
		TestEqual(TEXT("total is uncapped"), (int32)(*Payload)->GetNumberField(TEXT("total")), 7);
		const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
		if (!TestTrue(TEXT("rows array"), (*Payload)->TryGetArrayField(TEXT("children"), Rows))) { return false; }
		TestEqual(TEXT("page limited"), Rows->Num(), 2);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
