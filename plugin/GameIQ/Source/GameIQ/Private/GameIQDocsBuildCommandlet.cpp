// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQDocsBuildCommandlet.h"

#include "GameIQBuildTiming.h"
#include "GameIQConnectorsCommandlet.h"
#include "GameIQDocsCommandlet.h"
#include "GameIQImageCommandlet.h"
#include "GameIQIndexCommandlet.h"
#include "GameIQLinkCommandlet.h"
#include "GameIQStore.h"
#include "HAL/PlatformTime.h"
#include "Templates/Function.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQDocsBuild, Log, All);

UGameIQDocsBuildCommandlet::UGameIQDocsBuildCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQDocsBuildCommandlet::Main(const FString& /*Params*/)
{
	UE_LOG(LogGameIQDocsBuild, Display, TEXT("Game IQ: documents-only reindex (no asset/Blueprint/C++ re-scan)."));

	const double BuildStart = FPlatformTime::Seconds();
	TArray<GameIQ::FStageTiming> StageTimings;

	int32 Step = 0;
	const int32 TotalSteps = 5;
	auto RunStage = [&Step, TotalSteps, &StageTimings](const TCHAR* Name, TFunctionRef<int32()> Run) -> int32
	{
		UE_LOG(LogGameIQDocsBuild, Display, TEXT("Game IQ: [%d/%d] %s..."), ++Step, TotalSteps, Name);
		const double Start = FPlatformTime::Seconds();
		const int32 Result = Run();
		const double Elapsed = FPlatformTime::Seconds() - Start;
		UE_LOG(LogGameIQDocsBuild, Display, TEXT("Game IQ: %s done in %.1fs"), Name, Elapsed);
		StageTimings.Add(GameIQ::FStageTiming{Name, Elapsed});
		return Result;
	};

	// Re-run only the documentation extractors.
	RunStage(TEXT("Docs"), [] { return NewObject<UGameIQDocsCommandlet>()->Main(FString()); });                      // docs.json
	RunStage(TEXT("Images"), [] { return NewObject<UGameIQImageCommandlet>()->Main(FString()); });                   // images.json
	RunStage(TEXT("External connectors"), [] { return NewObject<UGameIQConnectorsCommandlet>()->Main(FString()); }); // external-docs.json

	// Ingest ONLY those producers — producer-scoped replace leaves everything else in the index intact.
	// '+' separates the files (FParse::Value truncates a value at a comma).
	const int32 Result = RunStage(TEXT("Index (docs only)"), []
	{
		return NewObject<UGameIQIndexCommandlet>()->Main(TEXT("only=docs.json+images.json+external-docs.json"));
	});

	// Refresh intent→implementation links (describes/illustrates) against the existing code/asset entities.
	RunStage(TEXT("Link"), [] { return NewObject<UGameIQLinkCommandlet>()->Main(FString()); });

	const double TotalSeconds = FPlatformTime::Seconds() - BuildStart;
	UE_LOG(LogGameIQDocsBuild, Display, TEXT("Game IQ: documents reindex complete in %.1fs."), TotalSeconds);
	GameIQ::AppendBuildTimingRecord(TEXT("GameIQDocsBuild"), TotalSeconds, StageTimings);

	// Result marker — subprocess launchers judge the run by this, not the exit code (which only
	// counts logged engine errors). See GameIQBuildCommandlet.cpp.
	{
		GameIQ::FBuildResult BuildResult;
		BuildResult.RunType = TEXT("GameIQDocsBuild");
		BuildResult.bSuccess = Result == 0;
		BuildResult.TotalSeconds = TotalSeconds;
		FGameIQStore Store;
		if (Store.Open())
		{
			Store.GetCounts(BuildResult.Entities, BuildResult.Edges, BuildResult.Chunks);
			Store.Close();
		}
		GameIQ::WriteBuildResult(BuildResult);
	}
	return Result;
}
