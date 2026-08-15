// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQBuildCommandlet.h"

#include "GameIQBuildProcess.h"
#include "GameIQBuildTiming.h"
#include "GameIQCodeCommandlet.h"
#include "GameIQConfigCommandlet.h"
#include "GameIQConnectorsCommandlet.h"
#include "GameIQDocsCommandlet.h"
#include "GameIQExportCommandlet.h"
#include "GameIQImageCommandlet.h"
#include "GameIQIndexCommandlet.h"
#include "GameIQLinkCommandlet.h"
#include "GameIQStore.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Templates/Function.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQBuild, Log, All);

UGameIQBuildCommandlet::UGameIQBuildCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQBuildCommandlet::Main(const FString& Params)
{
	UE_LOG(LogGameIQBuild, Display, TEXT("Game IQ: full build — running all extractors, then index."));

	const double BuildStart = FPlatformTime::Seconds();
	TArray<GameIQ::FStageTiming> StageTimings;

	// Ensure the index DB + its generation stamp exist BEFORE the extract stages read them:
	// the incremental signature caches are keyed to the DB generation, so pre-creating here is
	// what lets the very next build after a fresh clone/self-heal run incrementally. This also
	// surfaces a broken schema immediately (Open self-heals) instead of after 10 minutes of extract.
	{
		FGameIQStore Store;
		if (!Store.Open())
		{
			UE_LOG(LogGameIQBuild, Error, TEXT("Game IQ: cannot open/create the index DB — aborting the build."));
			return 1;
		}
		Store.Close();
	}

	int32 Step = 0;
	const int32 TotalSteps = 10;
	auto RunStage = [&Step, TotalSteps, &StageTimings](const TCHAR* Name, TFunctionRef<int32()> Run) -> int32
	{
		UE_LOG(LogGameIQBuild, Display, TEXT("Game IQ: [%d/%d] %s..."), ++Step, TotalSteps, Name);
		const double Start = FPlatformTime::Seconds();
		const int32 Result = Run();
		const double Elapsed = FPlatformTime::Seconds() - Start;
		UE_LOG(LogGameIQBuild, Display, TEXT("Game IQ: %s done in %.1fs"), Name, Elapsed);
		StageTimings.Add(GameIQ::FStageTiming{Name, Elapsed});
		return Result;
	};

	// Params is threaded through so -full (force a from-scratch rebuild, bypassing the
	// Assets/Blueprints incremental caches) and -out= reach every sub-commandlet.

	// The two asset-LOADING stages are the only ones heavy enough to deserve their own engine
	// boot — launch them as concurrent subprocesses first, then do everything cheap in THIS
	// process while they grind. (The previous shape gave all 7 extract stages a subprocess each:
	// ~25-30s of engine boot per stage to do seconds of file parsing.)
	const TArray<GameIQBuildProcess::FStageSpec> HeavySpecs = {
		{ TEXT("Assets"), TEXT("GameIQAssets"), TEXT("assets.json") },          // Tier 1
		{ TEXT("Blueprints"), TEXT("GameIQBlueprints"), TEXT("blueprints.json") }, // Tier 2
	};
	UE_LOG(LogGameIQBuild, Display, TEXT("Game IQ: launching Assets + Blueprints as concurrent subprocesses..."));
	TArray<GameIQBuildProcess::FLaunchedStage> Heavy = GameIQBuildProcess::LaunchStages(HeavySpecs, Params);
	Step += HeavySpecs.Num();

	// Cheap stages in-process, overlapping the subprocesses. Tier 0 first: metadata-only
	// (no UObject load), and nothing downstream depends on it.
	RunStage(TEXT("Registry export"), [&Params] { return NewObject<UGameIQExportCommandlet>()->Main(Params); });        // registry.json
	RunStage(TEXT("C++ code"), [&Params] { return NewObject<UGameIQCodeCommandlet>()->Main(Params); });                  // cpp.json
	RunStage(TEXT("Config"), [&Params] { return NewObject<UGameIQConfigCommandlet>()->Main(Params); });                  // config.json
	RunStage(TEXT("Docs"), [&Params] { return NewObject<UGameIQDocsCommandlet>()->Main(Params); });                      // docs.json
	RunStage(TEXT("Images"), [&Params] { return NewObject<UGameIQImageCommandlet>()->Main(Params); });                   // images.json
	RunStage(TEXT("External connectors"), [&Params] { return NewObject<UGameIQConnectorsCommandlet>()->Main(Params); }); // external-docs.json

	// Now block on the heavy stages. Success is judged by output freshness, not exit codes
	// (UnrealEditor-Cmd's exit code reflects logged engine errors, not the commandlet result).
	const FString ExtractDir = FPaths::Combine(FPaths::ProjectDir(), TEXT(".gameiq"), TEXT("extract"));
	TArray<FString> StaleOutputs;
	GameIQBuildProcess::WaitForStages(Heavy, ExtractDir, StageTimings, StaleOutputs);

	// Ingest everything into the SQLite index — skipping any output a failed stage left stale,
	// so an old delta can't roll the index backwards.
	FString IndexParams = Params;
	if (StaleOutputs.Num() > 0)
	{
		IndexParams += FString::Printf(TEXT(" -skipfiles=%s"), *FString::Join(StaleOutputs, TEXT("+")));
	}
	const int32 IndexResult = RunStage(TEXT("Index"), [&IndexParams] { return NewObject<UGameIQIndexCommandlet>()->Main(IndexParams); });

	// Phase 2: link doc sections (stated intent) to the implementation they describe. Runs last —
	// it needs every entity already in the index to resolve references. (issue #6)
	RunStage(TEXT("Link"), [&Params] { return NewObject<UGameIQLinkCommandlet>()->Main(Params); });

	const double TotalSeconds = FPlatformTime::Seconds() - BuildStart;
	if (IndexResult != 0)
	{
		UE_LOG(LogGameIQBuild, Error, TEXT("Game IQ: BUILD FAILED VERIFICATION in %.1fs — see the Index stage errors above."), TotalSeconds);
	}
	else if (StaleOutputs.Num() > 0)
	{
		UE_LOG(LogGameIQBuild, Warning, TEXT("Game IQ: build complete in %.1fs, but %d extract stage(s) failed and were skipped — their producers kept their previous index state."), TotalSeconds, StaleOutputs.Num());
	}
	else
	{
		UE_LOG(LogGameIQBuild, Display, TEXT("Game IQ: full build complete in %.1fs."), TotalSeconds);
	}
	GameIQ::AppendBuildTimingRecord(TEXT("GameIQBuild"), TotalSeconds, StageTimings);

	// Result marker — what FGameIQBuildRunner (and anything else launching this as a subprocess)
	// judges the build by, since UnrealEditor-Cmd's exit code only counts logged engine errors.
	{
		GameIQ::FBuildResult Result;
		Result.RunType = TEXT("GameIQBuild");
		Result.bSuccess = IndexResult == 0;
		Result.TotalSeconds = TotalSeconds;
		FGameIQStore Store;
		if (Store.Open())
		{
			Store.GetCounts(Result.Entities, Result.Edges, Result.Chunks);
			Store.Close();
		}
		GameIQ::WriteBuildResult(Result);
	}
	return IndexResult;
}
