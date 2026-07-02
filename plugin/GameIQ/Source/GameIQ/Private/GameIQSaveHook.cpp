// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQSaveHook.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "GameIQBlueprintCommandlet.h"
#include "GameIQJson.h"
#include "HAL/FileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQHook, Log, All);

void UGameIQSaveHookSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	SaveHandle = UPackage::PackageSavedWithContextEvent.AddUObject(this, &UGameIQSaveHookSubsystem::OnPackageSaved);
	UE_LOG(LogGameIQHook, Log, TEXT("Game IQ save hook active — Blueprint saves patch the index incrementally."));
}

void UGameIQSaveHookSubsystem::Deinitialize()
{
	UPackage::PackageSavedWithContextEvent.Remove(SaveHandle);
	Super::Deinitialize();
}

void UGameIQSaveHookSubsystem::OnPackageSaved(const FString& /*PackageFilename*/, UPackage* Package, FObjectPostSaveContext /*Context*/)
{
	if (!Package) { return; }
	const FString PackageName = Package->GetName(); // "/Game/..."
	if (!PackageName.StartsWith(TEXT("/Game"))) { return; }

	// Find the Blueprint in the saved package (v1 handles Blueprints — the most-edited asset;
	// other asset types stay fresh via `gameiq index`).
	TArray<UObject*> Objects;
	GetObjectsWithPackage(Package, Objects, EGetObjectsFlags::None);
	UBlueprint* BP = nullptr;
	for (UObject* Obj : Objects)
	{
		BP = Cast<UBlueprint>(Obj);
		if (BP) { break; }
	}
	if (!BP) { return; }

	TArray<TSharedPtr<FJsonValue>> Entities, Edges, Chunks;
	const FString Id = FString::Printf(TEXT("asset:%s"), *PackageName);

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

	// Dependency edges (keep "what this Blueprint uses" fresh).
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FName> Deps;
	ARM.Get().GetDependencies(Package->GetFName(), Deps, UE::AssetRegistry::EDependencyCategory::Package);
	for (const FName& Dep : Deps)
	{
		const FString DepName = Dep.ToString();
		if (DepName.StartsWith(TEXT("/Game")))
		{
			Edges.Add(MakeShared<FJsonValueObject>(
				GameIQ::MakeEdge(Id, FString::Printf(TEXT("asset:%s"), *DepName), TEXT("depends-on"))));
		}
	}

	const FString IncDir = FPaths::Combine(FPaths::ProjectDir(), TEXT(".gameiq"), TEXT("extract"), TEXT("incremental"));
	IFileManager::Get().MakeDirectory(*IncDir, /*Tree=*/true);
	const FString File = FPaths::Combine(IncDir, FGuid::NewGuid().ToString() + TEXT(".json"));
	if (GameIQ::WriteDelta(File, TEXT("gameiq-bridge@0.1.0"), { Id }, Entities, Edges, Chunks))
	{
		UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: wrote incremental delta for %s"), *PackageName);
	}
}
