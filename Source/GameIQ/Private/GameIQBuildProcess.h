// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameIQBuildTiming.h"
#include "HAL/PlatformProcess.h"
#include "Misc/DateTime.h"

/**
 * UI-agnostic helper for running heavyweight Game IQ extraction stages as concurrent
 * UnrealEditor-Cmd subprocesses (each stage writes its own .gameiq/extract/*.json, order
 * doesn't matter). Deliberately never threads stages in-process: Assets/Blueprints call
 * GetAsset() (UE asset loading is expected to run on the game thread of a single process),
 * and every stage constructs a fresh UCommandlet UObject via NewObject(), which isn't safe
 * off the game thread either. Separate OS processes sidestep both risks.
 *
 * Split into Launch + Wait so the orchestrator can run its cheap in-process stages (config,
 * C++ parse, docs — pure file work) WHILE the asset-loading subprocesses grind, instead of
 * paying a full engine boot per trivial stage.
 */
namespace GameIQBuildProcess
{
	struct FStageSpec
	{
		FString Name;
		FString RunArg;     // the commandlet's -run= value, e.g. "GameIQAssets"
		FString OutputFile; // extract file this stage must refresh, e.g. "assets.json" (empty = don't check)
	};

	struct FLaunchedStage
	{
		FStageSpec Spec;
		FProcHandle Handle;
		double StartTime = 0.0;
		FDateTime LaunchUtc;
		bool bLaunched = false;
	};

	/** Launch each spec as its own headless UnrealEditor-Cmd process (all at once — callers pass
	 *  only the stages heavy enough to deserve their own engine boot). */
	TArray<FLaunchedStage> LaunchStages(const TArray<FStageSpec>& Specs, const FString& ExtraArgs);

	/**
	 * Block until every launched stage exits. Appends per-stage wall-clock to OutTimings.
	 * Success per stage is judged by whether its declared output file was rewritten after launch —
	 * NOT by the process exit code, which reflects the engine's logged-error count (benign content
	 * warnings included) and is routinely nonzero on clean runs. Stages whose output file is
	 * missing/stale are appended to OutStaleOutputs so the ingest can skip them.
	 */
	bool WaitForStages(
		TArray<FLaunchedStage>& Stages,
		const FString& ExtractDir,
		TArray<GameIQ::FStageTiming>& OutTimings,
		TArray<FString>& OutStaleOutputs);
}
