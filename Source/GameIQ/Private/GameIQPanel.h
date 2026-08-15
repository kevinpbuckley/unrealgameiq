// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

/**
 * Tools > Game IQ panel: shows the current index stats (project, entity/edge counts by kind,
 * last-built time, DB size), a live stage/log readout while a rebuild is running, and buttons to
 * kick off a rebuild via FGameIQBuildRunner (GameIQBuild / GameIQDocsBuild).
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
	void OnBuildFinished(bool bSuccess);

	FText StatsText;
	FDelegateHandle BuildFinishedHandle;
};
