// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "Widgets/SCompoundWidget.h"

/**
 * Tools > Game IQ panel: shows the current index stats (project, entity/edge counts by kind,
 * last-built time, DB size) and a button to rebuild the index in-process (GameIQBuild).
 */
class SGameIQPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SGameIQPanel) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SGameIQPanel() override;

private:
	FReply OnRebuildClicked();
	FReply OnReindexDocsClicked();
	FReply OnRefreshClicked();
	void Refresh();
	/** Spawn a headless commandlet (`-run=<RunArg>`) as a background process; poll it to completion. */
	FReply StartBuild(const FString& RunArg, const FText& StartedMsg);
	/** Poll the background rebuild process; refresh stats and stop when it exits. */
	bool PollRebuild(float DeltaTime);

	FText StatsText;
	bool bBuilding = false;
	FProcHandle RebuildProc;
	FTSTicker::FDelegateHandle PollHandle;
};
