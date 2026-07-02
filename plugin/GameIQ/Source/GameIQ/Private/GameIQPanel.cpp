// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQPanel.h"

#include "Dom/JsonObject.h"
#include "GameIQBuildCommandlet.h"
#include "GameIQQuery.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
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
		Out += FString::Printf(TEXT("Last update: %s\n\n"), *PrettyTime(R->GetStringField(TEXT("lastIngestAtIso"))));
		Out += FString::Printf(TEXT("Entities:    %d\n"), Total);
		Out += FString::Join(KindLines, TEXT("\n"));
		Out += FString::Printf(TEXT("\n\nEdges:       %d\n"), EdgeTotal);
		return FText::FromString(Out);
	}
}

void SGameIQPanel::Construct(const FArguments& InArgs)
{
	Refresh();

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
				.ToolTipText(LOCTEXT("RebuildTip", "Re-run all extractors and rebuild the SQLite index in-process."))
				.IsEnabled_Lambda([this]() { return !bBuilding; })
				.OnClicked(this, &SGameIQPanel::OnRebuildClicked)
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.Text(LOCTEXT("Refresh", "Refresh"))
				.OnClicked(this, &SGameIQPanel::OnRefreshClicked)
			]
		]

		// Stats
		+ SVerticalBox::Slot().FillHeight(1.0f).Padding(12, 4, 12, 12)
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
}

FReply SGameIQPanel::OnRefreshClicked()
{
	Refresh();
	return FReply::Handled();
}

FReply SGameIQPanel::OnRebuildClicked()
{
	bBuilding = true;
	{
		FScopedSlowTask Task(1.0f, LOCTEXT("Rebuilding", "Rebuilding the Game IQ index — extracting and ingesting…"));
		Task.MakeDialog();
		Task.EnterProgressFrame(1.0f);
		if (UGameIQBuildCommandlet* Build = NewObject<UGameIQBuildCommandlet>())
		{
			Build->Main(FString());
		}
	}
	bBuilding = false;
	Refresh();
	return FReply::Handled();
}

#undef LOCTEXT_NAMESPACE
