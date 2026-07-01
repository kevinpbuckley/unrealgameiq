// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQExportCommandlet.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
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

	/** Pull the prefix-less native parent class name from a Blueprint's registry tags, if present. */
	bool TryGetNativeParentClassName(const FAssetData& Data, FString& OutClassName)
	{
		FString Raw;
		if (!Data.GetTagValue(FName(TEXT("NativeParentClass")), Raw) &&
			!Data.GetTagValue(FName(TEXT("ParentClass")), Raw))
		{
			return false;
		}
		// Raw looks like "/Script/Engine.Character" or "Class'/Script/Engine.Character'"
		Raw.RemoveFromStart(TEXT("Class'"));
		Raw.RemoveFromEnd(TEXT("'"));
		int32 Dot = INDEX_NONE;
		if (Raw.FindLastChar(TEXT('.'), Dot))
		{
			OutClassName = Raw.RightChop(Dot + 1);
			return !OutClassName.IsEmpty();
		}
		return false;
	}
}

UGameIQExportCommandlet::UGameIQExportCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = true;
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
		++Considered;

		const FString Id = FString::Printf(TEXT("asset:%s"), *Package);
		const FString ClassName = Data.AssetClassPath.GetAssetName().ToString();
		const bool bIsBlueprint = ClassName.EndsWith(TEXT("Blueprint"));
		const FString Kind = bIsBlueprint ? TEXT("blueprint") : TEXT("asset");

		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("assetClass"), ClassName);
		Detail->SetStringField(TEXT("objectPath"), Data.GetObjectPathString());

		Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
			Id, Kind, Data.AssetName.ToString(), Package, TEXT("asset"), FString(), FString(), Detail)));

		// Tier 0 dependency graph (generic, from the registry — answers "what uses X?")
		TArray<FName> Deps;
		AR.GetDependencies(Data.PackageName, Deps, UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName& Dep : Deps)
		{
			const FString DepName = Dep.ToString();
			if (IsProjectPackage(DepName))
			{
				Edges.Add(MakeShared<FJsonValueObject>(
					GameIQ::MakeEdge(Id, FString::Printf(TEXT("asset:%s"), *DepName), TEXT("depends-on"))));
			}
		}

		// Cross-link Blueprints to their native C++ parent (matches the C++ extractor's canonical ids)
		FString ParentClassName;
		if (bIsBlueprint && TryGetNativeParentClassName(Data, ParentClassName))
		{
			Edges.Add(MakeShared<FJsonValueObject>(
				GameIQ::MakeEdge(Id, FString::Printf(TEXT("cpp:%s"), *ParentClassName), TEXT("inherits"))));
		}
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
