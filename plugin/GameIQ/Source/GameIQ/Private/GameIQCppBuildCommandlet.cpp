// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQCppBuildCommandlet.h"

#include "GameIQBuildTiming.h"
#include "GameIQCodeCommandlet.h"
#include "GameIQIndexCommandlet.h"
#include "GameIQLinkCommandlet.h"
#include "GameIQStore.h"
#include "HAL/PlatformTime.h"
#include "Templates/Function.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQCppBuild, Log, All);

UGameIQCppBuildCommandlet::UGameIQCppBuildCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQCppBuildCommandlet::Main(const FString& /*Params*/)
{
	UE_LOG(LogGameIQCppBuild, Display, TEXT("Game IQ: C++-only reindex (no asset/Blueprint/docs re-scan)."));

	const double BuildStart = FPlatformTime::Seconds();
	TArray<GameIQ::FStageTiming> StageTimings;

	int32 Step = 0;
	const int32 TotalSteps = 3;
	auto RunStage = [&Step, TotalSteps, &StageTimings](const TCHAR* Name, TFunctionRef<int32()> Run) -> int32
	{
		UE_LOG(LogGameIQCppBuild, Display, TEXT("Game IQ: [%d/%d] %s..."), ++Step, TotalSteps, Name);
		const double Start = FPlatformTime::Seconds();
		const int32 Result = Run();
		const double Elapsed = FPlatformTime::Seconds() - Start;
		UE_LOG(LogGameIQCppBuild, Display, TEXT("Game IQ: %s done in %.1fs"), Name, Elapsed);
		StageTimings.Add(GameIQ::FStageTiming{Name, Elapsed});
		return Result;
	};

	// Re-run only the code extractor (writes cpp.json + cpp.fingerprint).
	RunStage(TEXT("C++"), [] { return NewObject<UGameIQCodeCommandlet>()->Main(FString()); });

	// Ingest ONLY the cpp producer — producer-scoped replace leaves everything else in the index intact.
	const int32 Result = RunStage(TEXT("Index (C++ only)"), []
	{
		return NewObject<UGameIQIndexCommandlet>()->Main(TEXT("only=cpp.json"));
	});

	// Refresh intent→implementation links (describes/illustrates) against the refreshed code entities.
	RunStage(TEXT("Link"), [] { return NewObject<UGameIQLinkCommandlet>()->Main(FString()); });

	const double TotalSeconds = FPlatformTime::Seconds() - BuildStart;
	UE_LOG(LogGameIQCppBuild, Display, TEXT("Game IQ: C++ reindex complete in %.1fs."), TotalSeconds);
	GameIQ::AppendBuildTimingRecord(TEXT("GameIQCppBuild"), TotalSeconds, StageTimings);

	// Result marker — subprocess launchers judge the run by this, not the exit code (which only
	// counts logged engine errors). See GameIQBuildCommandlet.cpp.
	{
		GameIQ::FBuildResult BuildResult;
		BuildResult.RunType = TEXT("GameIQCppBuild");
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
