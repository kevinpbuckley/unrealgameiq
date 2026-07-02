// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
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

private:
	FReply OnRebuildClicked();
	FReply OnRefreshClicked();
	void Refresh();

	FText StatsText;
	bool bBuilding = false;
};
