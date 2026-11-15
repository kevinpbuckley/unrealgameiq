// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQPanel.h"

#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "GameIQBuildRunner.h"
#include "GameIQProjectChecks.h"
#include "GameIQQuery.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "GameIQPanel"

namespace
{
	FString PanelIndexDbPath()
	{
		return FPaths::Combine(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()), TEXT(".gameiq"), TEXT("index.db"));
	}

	/** "2026-07-02T04:24:08.847Z" -> "2026-07-02 04:24". Empty -> "never". */
	FString PrettyTime(const FString& Iso)
	{
		if (Iso.IsEmpty()) { return TEXT("never"); }
		FString S = Iso.Replace(TEXT("T"), TEXT(" "));
		return S.Len() >= 16 ? S.Left(16) : S;
	}

	/** Build the human-readable stats block from GameIQQuery::ProjectStats("overview"). */
	FText BuildStatsText()
	{
		const FString DbPath = PanelIndexDbPath();
		if (!FPaths::FileExists(DbPath))
		{
			return FText::FromString(FString::Printf(
				TEXT("No index found at:\n%s\n\nClick \"Rebuild Index\" to build it."), *DbPath));
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(GameIQQuery::ProjectStats(TEXT("overview")));
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			return FText::FromString(TEXT("Could not read the index."));
		}
		const TSharedPtr<FJsonObject> R = Root->GetObjectField(TEXT("result"));
		if (!R.IsValid())
		{
			return FText::FromString(TEXT("Could not read the index."));
		}

		const FString Project = R->GetStringField(TEXT("projectName"));
		const int32 Total = static_cast<int32>(R->GetNumberField(TEXT("totalEntities")));
		const int64 SizeBytes = IFileManager::Get().FileSize(*DbPath);
		const double SizeMB = SizeBytes > 0 ? SizeBytes / (1024.0 * 1024.0) : 0.0;

		int32 EdgeTotal = 0;
		const TArray<TSharedPtr<FJsonValue>>* Edges = nullptr;
		if (R->TryGetArrayField(TEXT("byEdgeType"), Edges))
		{
			for (const TSharedPtr<FJsonValue>& V : *Edges)
			{
				if (const TSharedPtr<FJsonObject> O = V->AsObject()) { EdgeTotal += static_cast<int32>(O->GetNumberField(TEXT("count"))); }
			}
		}

		TArray<FString> KindLines;
		const TArray<TSharedPtr<FJsonValue>>* Kinds = nullptr;
		if (R->TryGetArrayField(TEXT("byKind"), Kinds))
		{
			for (const TSharedPtr<FJsonValue>& V : *Kinds)
			{
				if (const TSharedPtr<FJsonObject> O = V->AsObject())
				{
					KindLines.Add(FString::Printf(TEXT("    %s  %d"),
						*O->GetStringField(TEXT("kind")), static_cast<int32>(O->GetNumberField(TEXT("count")))));
				}
			}
		}

		FString Out;
		Out += FString::Printf(TEXT("Project:     %s\n"), *Project);
		Out += FString::Printf(TEXT("Index:       %s\n"), *DbPath);
		Out += FString::Printf(TEXT("Size:        %.1f MB\n"), SizeMB);
		Out += FString::Printf(TEXT("Last built:  %s\n"), *PrettyTime(R->GetStringField(TEXT("lastGeneratedAtIso"))));
		Out += FString::Printf(TEXT("Last update: %s\n"), *PrettyTime(R->GetStringField(TEXT("lastIngestAtIso"))));
		// Per-source freshness (issue #29): the global stamp moves on any write and hides an old docs extract.
		const TSharedPtr<FJsonObject>* BySource = nullptr;
		if (R->TryGetObjectField(TEXT("bySource"), BySource))
		{
			for (const auto& PRaw : (*BySource)->Values)
			{
				// UE 5.8: FJsonObject keys are TSharedString, not FString
				const TPair<FString, TSharedPtr<FJsonValue>> P(FString(*PRaw.Key), PRaw.Value);
				Out += FString::Printf(TEXT("    %-12s %s\n"), *P.Key, *PrettyTime(P.Value->AsString()));
			}
		}
		Out += TEXT("\n");
		Out += FString::Printf(TEXT("Entities:    %d\n"), Total);
		Out += FString::Join(KindLines, TEXT("\n"));
		Out += FString::Printf(TEXT("\n\nEdges:       %d\n"), EdgeTotal);
		return FText::FromString(Out);
	}
}

void SGameIQPanel::Construct(const FArguments& InArgs)
{
	Refresh();
	BuildFinishedHandle = FGameIQBuildRunner::Get().OnFinished.AddSP(this, &SGameIQPanel::OnBuildFinished);

	ChildSlot
	[
		SNew(SVerticalBox)

		// Header
		+ SVerticalBox::Slot().AutoHeight().Padding(12, 12, 12, 4)
		[
			SNew(STextBlock)
			.Text(LOCTEXT("Title", "Unreal Game IQ — Project Index"))
			.Font(FAppStyle::Get().GetFontStyle("HeadingSmall"))
		]

		// Buttons
		+ SVerticalBox::Slot().AutoHeight().Padding(12, 4, 12, 8)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("Rebuild", "Rebuild Index"))
				.ToolTipText(LOCTEXT("RebuildTip", "Re-run every extractor (assets, Blueprints, C++, config, docs) and rebuild the whole index. Same as console command GameIQ.Rebuild."))
				.IsEnabled_Lambda([]() { return !FGameIQBuildRunner::Get().IsBuilding(); })
				.OnClicked(this, &SGameIQPanel::OnRebuildClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0, 0, 8, 0)
			[
				SNew(SButton)
				.Text(LOCTEXT("ReindexDocs", "Reindex Documents"))
				.ToolTipText(LOCTEXT("ReindexDocsTip", "Reindex only documentation (docs, images, external sources) and refresh links — leaves code/asset entities untouched. Much faster than a full rebuild. Same as console command GameIQ.ReindexDocs."))
				.IsEnabled_Lambda([]() { return !FGameIQBuildRunner::Get().IsBuilding(); })
				.OnClicked(this, &SGameIQPanel::OnReindexDocsClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Refresh", "Refresh"))
				.OnClicked(this, &SGameIQPanel::OnRefreshClicked)
			]
		]

		// Agent-guide warning (issue #26): agents that are never told the index exists don't use it.
		+ SVerticalBox::Slot().AutoHeight().Padding(12, 0, 12, 8)
		[
			SNew(SBorder)
			.Visibility_Lambda([this]() { return bAgentGuideMissing ? EVisibility::Visible : EVisibility::Collapsed; })
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			.Padding(8)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Center)
				[
					SNew(STextBlock)
					.Text(LOCTEXT("GuideMissing", "No agent file (CLAUDE.md, AGENTS.md, GEMINI.md) mentions Game IQ, so coding agents won't know to query it."))
					.ColorAndOpacity(FLinearColor(1.0f, 0.75f, 0.2f))
					.AutoWrapText(true)
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
				[
					SNew(SButton)
					.Text(LOCTEXT("WriteGuide", "Write Agent Guide"))
					.ToolTipText(LOCTEXT("WriteGuideTip", "Write the Game IQ usage block into CLAUDE.md, GEMINI.md and AGENTS.md at the project root and add .gameiq/ to .gitignore. Same as console command GameIQ.GenerateAgentConfig All."))
					.OnClicked(this, &SGameIQPanel::OnWriteGuideClicked)
				]
			]
		]

		// Live stage/log readout — only visible while a build is running.
		+ SVerticalBox::Slot().AutoHeight().Padding(12, 0, 12, 8)
		[
			SNew(SBorder)
			.Visibility_Lambda([]() { return FGameIQBuildRunner::Get().IsBuilding() ? EVisibility::Visible : EVisibility::Collapsed; })
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			.Padding(8)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(STextBlock)
					.Text_Lambda([]() { return FText::FromString(FGameIQBuildRunner::Get().GetCurrentStage()); })
					.Font(FCoreStyle::GetDefaultFontStyle("Bold", 9))
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
				[
					SNew(STextBlock)
					.Text_Lambda([]() { return FText::FromString(FGameIQBuildRunner::Get().GetLogTail()); })
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 8))
					.AutoWrapText(true)
				]
			]
		]

		// Stats
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(12, 0, 12, 12)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::Get().GetBrush("ToolPanel.GroupBorder"))
			.Padding(10)
			[
				SNew(SScrollBox)
				+ SScrollBox::Slot()
				[
					SNew(STextBlock)
					.Text_Lambda([this]() { return StatsText; })
					.Font(FCoreStyle::GetDefaultFontStyle("Mono", 9))
				]
			]
		]
	];
}

void SGameIQPanel::Refresh()
{
	StatsText = BuildStatsText();
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	bAgentGuideMissing = !GameIQAgentFiles::Check(Root)->GetBoolField(TEXT("present"));
}

FReply SGameIQPanel::OnWriteGuideClicked()
{
	if (GEngine)
	{
		GEngine->Exec(nullptr, TEXT("GameIQ.GenerateAgentConfig All"));
	}
	Refresh();
	return FReply::Handled();
}

FReply SGameIQPanel::OnRefreshClicked()
{
	Refresh();
	return FReply::Handled();
}

FReply SGameIQPanel::OnRebuildClicked()
{
	FGameIQBuildRunner::Get().StartBuild(TEXT("GameIQBuild"),
		LOCTEXT("Started", "Game IQ: rebuilding the full index in the background…"));
	return FReply::Handled();
}

FReply SGameIQPanel::OnReindexDocsClicked()
{
	FGameIQBuildRunner::Get().StartBuild(TEXT("GameIQDocsBuild"),
		LOCTEXT("StartedDocs", "Game IQ: reindexing documents in the background…"));
	return FReply::Handled();
}

void SGameIQPanel::OnBuildFinished(bool /*bSuccess*/)
{
	Refresh();
}

SGameIQPanel::~SGameIQPanel()
{
	FGameIQBuildRunner::Get().OnFinished.Remove(BuildFinishedHandle);
}

#undef LOCTEXT_NAMESPACE
