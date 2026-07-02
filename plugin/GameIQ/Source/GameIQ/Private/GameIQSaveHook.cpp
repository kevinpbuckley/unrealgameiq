// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQSaveHook.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "GameIQAssetCommandlet.h"
#include "GameIQBlueprintCommandlet.h"
#include "GameIQJson.h"
#include "GameIQStore.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQHook, Log, All);

namespace
{
	/** Map a package name ("/Game/Foo/Bar") to its Game IQ entity id. */
	FString AssetIdFromPackage(const FString& PackageName)
	{
		return FString::Printf(TEXT("asset:%s"), *PackageName);
	}

	/** Strip an object path ("/Game/Foo/Bar.Bar") down to its package ("/Game/Foo/Bar"). */
	FString PackageFromObjectPath(const FString& ObjectPath)
	{
		FString Package = ObjectPath;
		int32 Dot;
		if (Package.FindChar(TEXT('.'), Dot))
		{
			Package.LeftInline(Dot);
		}
		return Package;
	}
}

void UGameIQSaveHookSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	SaveHandle = UPackage::PackageSavedWithContextEvent.AddUObject(this, &UGameIQSaveHookSubsystem::OnPackageSaved);

	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	RemovedHandle = ARM.Get().OnAssetRemoved().AddUObject(this, &UGameIQSaveHookSubsystem::OnAssetRemoved);
	RenamedHandle = ARM.Get().OnAssetRenamed().AddUObject(this, &UGameIQSaveHookSubsystem::OnAssetRenamed);

	UE_LOG(LogGameIQHook, Log, TEXT("Game IQ save hook active — saves, deletes, and renames patch the index incrementally."));
}

void UGameIQSaveHookSubsystem::Deinitialize()
{
	UPackage::PackageSavedWithContextEvent.Remove(SaveHandle);
	if (FAssetRegistryModule* ARM = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
	{
		ARM->Get().OnAssetRemoved().Remove(RemovedHandle);
		ARM->Get().OnAssetRenamed().Remove(RenamedHandle);
	}
	Super::Deinitialize();
}

void UGameIQSaveHookSubsystem::OnPackageSaved(const FString& /*PackageFilename*/, UPackage* Package, FObjectPostSaveContext /*Context*/)
{
	if (!Package) { return; }
	const FString PackageName = Package->GetName(); // "/Game/..."
	if (!PackageName.StartsWith(TEXT("/Game"))) { return; }

	// Find the primary asset in the saved package — a Blueprint if present, else the first asset.
	TArray<UObject*> Objects;
	GetObjectsWithPackage(Package, Objects, EGetObjectsFlags::None);
	UBlueprint* BP = nullptr;
	UObject* Asset = nullptr;
	for (UObject* Obj : Objects)
	{
		if (UBlueprint* AsBP = Cast<UBlueprint>(Obj)) { BP = AsBP; Asset = Obj; break; }
	}
	if (!BP)
	{
		for (UObject* Obj : Objects)
		{
			if (Obj && Obj->IsAsset()) { Asset = Obj; break; }
		}
	}
	if (!Asset) { return; }

	TArray<TSharedPtr<FJsonValue>> Entities, Edges, Chunks;
	const FString Id = AssetIdFromPackage(PackageName);

	if (BP)
	{
		// The blueprint entity itself (registry producer normally emits this; the delta replaces
		// the whole subtree rooted at Id, so it must re-include the root).
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("assetClass"), TEXT("Blueprint"));
		if (BP->ParentClass) { Detail->SetStringField(TEXT("parentClass"), BP->ParentClass->GetName()); }
		Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
			Id, TEXT("blueprint"), BP->GetName(), PackageName, TEXT("asset"), FString(),
			FString::Printf(TEXT("Blueprint %s"), *BP->GetName()), Detail)));
		if (BP->ParentClass)
		{
			Edges.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEdge(
				Id, FString::Printf(TEXT("cpp:%s"), *BP->ParentClass->GetName()), TEXT("inherits"))));
		}

		// Tier 2 detail — same recipe as the commandlet.
		GameIQBlueprint::ExtractBlueprint(BP, Entities, Edges, Chunks);
	}
	else
	{
		// Non-Blueprint asset — re-emit its identity entity + Tier 1 recipe (same as the commandlet).
		const FString ClassName = Asset->GetClass()->GetName();
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("assetClass"), ClassName);
		Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
			Id, TEXT("asset"), Asset->GetName(), PackageName, TEXT("asset"), FString(),
			FString::Printf(TEXT("%s (%s)"), *Asset->GetName(), *ClassName), Detail)));

		GameIQAsset::ExtractAsset(Asset, Entities, Edges, Chunks);
	}

	// Dependency edges (keep "what this asset uses" fresh).
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FName> Deps;
	ARM.Get().GetDependencies(Package->GetFName(), Deps, UE::AssetRegistry::EDependencyCategory::Package);
	for (const FName& Dep : Deps)
	{
		const FString DepName = Dep.ToString();
		if (DepName.StartsWith(TEXT("/Game")))
		{
			Edges.Add(MakeShared<FJsonValueObject>(
				GameIQ::MakeEdge(Id, AssetIdFromPackage(DepName), TEXT("depends-on"))));
		}
	}

	WriteDelta({ Id }, Entities, Edges, Chunks);
	UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: wrote incremental delta for %s"), *PackageName);
}

void UGameIQSaveHookSubsystem::OnAssetRemoved(const FAssetData& AssetData)
{
	const FString PackageName = AssetData.PackageName.ToString();
	if (!PackageName.StartsWith(TEXT("/Game"))) { return; }

	// Empty delta whose `replaces` root drops the whole subtree — the asset is gone.
	const FString Id = AssetIdFromPackage(PackageName);
	WriteDelta({ Id }, {}, {}, {});
	UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: removed %s from the index"), *PackageName);
}

void UGameIQSaveHookSubsystem::OnAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath)
{
	const FString OldPackage = PackageFromObjectPath(OldObjectPath);
	if (!OldPackage.StartsWith(TEXT("/Game"))) { return; }

	// Drop the old id; the rename saves the new package, so OnPackageSaved re-extracts the
	// asset under its new id. Deleting the old subtree here keeps the stale entity from lingering.
	const FString OldId = AssetIdFromPackage(OldPackage);
	WriteDelta({ OldId }, {}, {}, {});
	UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: dropped renamed %s (new id follows on save)"), *OldPackage);
}

void UGameIQSaveHookSubsystem::WriteDelta(
	const TArray<FString>& Replaces,
	const TArray<TSharedPtr<FJsonValue>>& Entities,
	const TArray<TSharedPtr<FJsonValue>>& Edges,
	const TArray<TSharedPtr<FJsonValue>>& Chunks)
{
	// Write the patch straight into the SQLite index (design §8, Phase 3) — no JSON delta, no Node
	// drain step. The toolset and the standalone stdio server both read the file we just updated.
	FGameIQStore Store;
	if (Store.Open())
	{
		Store.Patch(Replaces, Entities, Edges, Chunks, TEXT("gameiq-bridge@0.1.0"));
		Store.Close();
	}
}
