// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FOnGameIQBuildFinished, bool /*bSuccess*/);

/**
 * Runs a Game IQ rebuild (GameIQBuild / GameIQDocsBuild) as a background headless UnrealEditor-Cmd
 * process, captures its stdout over a pipe, and surfaces live per-stage progress via a persistent
 * Slate notification and the Output Log (category LogGameIQRunner) — independent of whether the
 * Tools > Game IQ panel is open. Shared by the panel's buttons and the GameIQ.Rebuild /
 * GameIQ.ReindexDocs console commands so both paths show the same live feedback.
 */
class FGameIQBuildRunner
{
public:
	static FGameIQBuildRunner& Get();
	~FGameIQBuildRunner();

	/** Start a build if one isn't already running (RunArg is the commandlet's -run= value).
	 *  ExtraArgs is appended verbatim to the commandlet command line (e.g. "-full" to bypass the
	 *  Assets/Blueprints incremental-hash cache and force a from-scratch rebuild). */
	void StartBuild(const FString& RunArg, const FText& Label, const FString& ExtraArgs = FString());

	bool IsBuilding() const { return bBuilding; }
	/** Most recent GameIQ log line seen from the build process — the "what's happening now" text. */
	FString GetCurrentStage() const { return CurrentStage; }
	/** Last several GameIQ log lines from the build process, oldest first. */
	FString GetLogTail() const;

	/** Fired once, on the game thread, when a build finishes (success or failure). */
	FOnGameIQBuildFinished OnFinished;

private:
	FGameIQBuildRunner() = default;

	bool PollTick(float DeltaTime);
	void AppendOutput(const FString& NewText);
	void FinishBuild();

	FProcHandle Proc;
	void* ReadPipe = nullptr;
	void* WritePipe = nullptr;
	FString PendingLineFragment;
	TArray<FString> LogLines;
	FString CurrentStage;
	bool bBuilding = false;
	FDateTime BuildStartUtc;
	FTSTicker::FDelegateHandle PollHandle;
	TWeakPtr<class SNotificationItem> NotificationItem;
};
