// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQExportCommandlet.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "GameIQAssetFacts.h"
#include "GameIQJson.h"
#include "HAL/FileManager.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQExport, Log, All);

namespace
{
	const TCHAR* RegistryProducer = TEXT("gameiq-ue-registry@0.1.0");

	/** Only project content (skip /Engine, /Script, etc.) — Game IQ indexes *your* game. */
	bool IsProjectPackage(const FString& PackageName)
	{
		return PackageName.StartsWith(TEXT("/Game"));
	}

}

UGameIQExportCommandlet::UGameIQExportCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = false; // benign engine load warnings shouldn't inflate the error count
}

int32 UGameIQExportCommandlet::Main(const FString& Params)
{
	FString OutDir;
	if (!FParse::Value(*Params, TEXT("out="), OutDir))
	{
		OutDir = FPaths::Combine(FPaths::ProjectDir(), TEXT(".gameiq"), TEXT("extract"));
	}
	IFileManager::Get().MakeDirectory(*OutDir, /*Tree=*/true);

	FAssetRegistryModule& ARM =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();

	UE_LOG(LogGameIQExport, Log, TEXT("Scanning all assets..."));
	AR.SearchAllAssets(/*bSynchronous=*/true);

	TArray<FAssetData> Assets;
	AR.GetAllAssets(Assets, /*bIncludeOnlyOnDiskAssets=*/true);

	TArray<TSharedPtr<FJsonValue>> Entities;
	TArray<TSharedPtr<FJsonValue>> Edges;
	const TArray<TSharedPtr<FJsonValue>> NoChunks;

	int32 Considered = 0;
	for (const FAssetData& Data : Assets)
	{
		const FString Package = Data.PackageName.ToString();
		if (!IsProjectPackage(Package))
		{
			continue;
		}
		// One-File-Per-Actor packages are level actors: the level recipe indexes them as typed
		// level-actor entities (the assets stage and the save hook skip them the same way).
		if (Package.Contains(TEXT("/__ExternalActors__/")) || Package.Contains(TEXT("/__ExternalObjects__/")))
		{
			continue;
		}
		++Considered;

		// Tier 0 identity (+ size facts, issue #33), dependency graph and native-parent link —
		// shared with the save hook and the editor catch-up scan (GameIQAssetFacts.h).
		GameIQAssetFacts::AppendTier0(AR, Data, Entities, Edges);
	}

	if (!GameIQ::WriteOutput(OutDir, TEXT("registry.json"), RegistryProducer, Entities, Edges, NoChunks))
	{
		UE_LOG(LogGameIQExport, Error, TEXT("Failed to write registry.json to %s"), *OutDir);
		return 1;
	}

	UE_LOG(LogGameIQExport, Display,
		TEXT("Game IQ: wrote %d entities and %d edges (%d project assets) to %s"),
		Entities.Num(), Edges.Num(), Considered, *FPaths::Combine(OutDir, TEXT("registry.json")));
	return 0;
}
