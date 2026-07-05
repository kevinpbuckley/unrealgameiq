// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQBuildCommandlet.h"

#include "GameIQBuildProcess.h"
#include "GameIQBuildTiming.h"
#include "GameIQExportCommandlet.h"
#include "GameIQIndexCommandlet.h"
#include "GameIQLinkCommandlet.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
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

	// Params is threaded through (previously every stage got FString(), silently dropping -out=/-full)
	// so -full (force a from-scratch rebuild, bypassing the Assets/Blueprints incremental-hash cache)
	// and -out= reach every sub-commandlet that recognizes them.

	// Tier 0 first, in-process: metadata-only (no UObject load), and nothing downstream depends on it.
	RunStage(TEXT("Registry export"), [&Params] { return NewObject<UGameIQExportCommandlet>()->Main(Params); }); // registry.json

	// The remaining 7 extraction stages are independent and producer-scoped (each writes its own
	// .gameiq/extract/*.json; order doesn't matter) — run them concurrently, each as its own
	// UnrealEditor-Cmd subprocess (see GameIQBuildProcess.h for why this is never threaded in-process).
	// Concurrency is capped, not unbounded: each stage is a full engine boot, so running all 7 at once
	// risks DDC/AssetRegistry-cache contention swamping the wall-clock win on modest hardware.
	UE_LOG(LogGameIQBuild, Display, TEXT("Game IQ: [2-8/%d] running 7 extraction stages concurrently..."), TotalSteps);
	const int32 MaxConcurrentStages = FMath::Clamp(FPlatformMisc::NumberOfCoresIncludingHyperthreads() / 2, 2, 4);
	const TArray<GameIQBuildProcess::FStageSpec> ParallelSpecs = {
		{ TEXT("Assets"), TEXT("GameIQAssets") },                   // assets.json  (Tier 1)
		{ TEXT("Blueprints"), TEXT("GameIQBlueprints") },           // blueprints.json (Tier 2)
		{ TEXT("Config"), TEXT("GameIQConfig") },                   // config.json
		{ TEXT("C++ code"), TEXT("GameIQCode") },                   // cpp.json
		{ TEXT("Docs"), TEXT("GameIQDocs") },                       // docs.json (design/brand/etc. — stated-intent)
		{ TEXT("Images"), TEXT("GameIQImages") },                   // images.json (concept art/level maps/brand)
		{ TEXT("External connectors"), TEXT("GameIQConnectors") },  // external-docs.json (Confluence/Notion/Drive exports)
	};
	const double ParallelStart = FPlatformTime::Seconds();
	const bool bParallelOk = GameIQBuildProcess::RunStagesConcurrently(ParallelSpecs, Params, MaxConcurrentStages, StageTimings);
	Step += ParallelSpecs.Num();
	UE_LOG(LogGameIQBuild, Display, TEXT("Game IQ: extraction stages done in %.1fs (concurrent-elapsed — per-stage numbers above are each stage's own wall-clock, not serial-additive)."),
		FPlatformTime::Seconds() - ParallelStart);
	if (!bParallelOk)
	{
		// A nonzero commandlet process exit code here does NOT reliably mean the stage's own
		// extraction/write failed — UnrealEditor-Cmd's exit code reflects the engine's own logged
		// Error-severity count (benign shutdown/content warnings included), not Main()'s return
		// value, and every stage (including ones untouched by this change) routinely exits nonzero
		// even on a clean run. The pre-existing serial code never checked these codes either — it
		// only ever gated on the Index stage's own result. Match that: log for visibility, but
		// still proceed to Index/Link so producers that DID succeed still make it into the index.
		UE_LOG(LogGameIQBuild, Warning, TEXT("Game IQ: one or more extraction stages reported a nonzero exit — see warnings above. Continuing to Index/Link regardless; any producer that failed to write its .gameiq/extract/*.json simply won't appear in this run's index."));
	}

	// Ingest everything into the SQLite index.
	const int32 IndexResult = RunStage(TEXT("Index"), [&Params] { return NewObject<UGameIQIndexCommandlet>()->Main(Params); });

	// Phase 2: link doc sections (stated intent) to the implementation they describe. Runs last —
	// it needs every entity already in the index to resolve references. (issue #6)
	RunStage(TEXT("Link"), [&Params] { return NewObject<UGameIQLinkCommandlet>()->Main(Params); });

	const double TotalSeconds = FPlatformTime::Seconds() - BuildStart;
	UE_LOG(LogGameIQBuild, Display, TEXT("Game IQ: full build complete in %.1fs."), TotalSeconds);
	GameIQ::AppendBuildTimingRecord(TEXT("GameIQBuild"), TotalSeconds, StageTimings);
	return IndexResult;
}
