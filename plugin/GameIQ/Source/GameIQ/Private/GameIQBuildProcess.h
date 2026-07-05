// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameIQBuildTiming.h"
#include "HAL/PlatformProcess.h"

/**
 * UI-agnostic helper for running independent Game IQ extraction stages as concurrent
 * UnrealEditor-Cmd subprocesses (GameIQBuildCommandlet.cpp: each stage writes its own
 * .gameiq/extract/*.json, order doesn't matter). Deliberately never threads stages in-process:
 * Assets/Blueprints call GetAsset() (UE asset loading is expected to run on the game thread of a
 * single process), and every stage constructs a fresh UCommandlet UObject via NewObject(), which
 * isn't safe off the game thread either — the same reasoning GameIQBuildRunner.cpp already
 * applies to the whole build when launched from the live editor. Separate OS processes sidestep
 * both risks the way that existing design already trusts.
 */
namespace GameIQBuildProcess
{
	struct FStageSpec
	{
		FString Name;
		FString RunArg; // the commandlet's -run= value, e.g. "GameIQAssets"
	};

	/**
	 * Launch each spec as its own headless UnrealEditor-Cmd process, capping concurrency at
	 * MaxConcurrent in-flight processes at once (queues the rest) — a full engine boot per stage
	 * is heavy, so unbounded concurrency risks DDC/AssetRegistry-cache contention swamping the
	 * wall-clock win. Blocks until every stage finishes. Returns true only if every stage started
	 * successfully and exited with code 0. Appends each stage's wall-clock to OutTimings.
	 */
	bool RunStagesConcurrently(
		const TArray<FStageSpec>& Specs,
		const FString& ExtraArgs,
		int32 MaxConcurrent,
		TArray<GameIQ::FStageTiming>& OutTimings);
}
