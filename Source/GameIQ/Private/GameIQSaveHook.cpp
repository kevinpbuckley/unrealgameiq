// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQSaveHook.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "GameIQAssetCommandlet.h"
#include "GameIQBlueprintCommandlet.h"
#include "GameIQBuildRunner.h"
#include "GameIQCppFingerprint.h"
#include "GameIQJson.h"
#include "GameIQSettings.h"
#include "GameIQStore.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/UObjectHash.h"

#if WITH_LIVE_CODING
#include "ILiveCodingModule.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogGameIQHook, Log, All);

namespace
{
	// Rows written by the hook use the SAME producer ids as the corresponding commandlets, so a
	// full rebuild's producer-scoped replace supersedes hook patches cleanly — a distinct
	// "bridge" producer left orphaned rows behind (rebuilds only replaced the commandlet
	// producers' rows, never the bridge's).
	const TCHAR* HookAssetsProducer = TEXT("gameiq-ue-assets@0.1.0");
	const TCHAR* HookBlueprintProducer = TEXT("gameiq-ue-bp@0.1.0");

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

	// C++ freshness: no save event exists for source edits, so check at the two moments new code
	// can appear — shortly after startup (a recompiled editor means sources may have changed) and
	// after each Live Coding patch. Delayed a few seconds so the check never competes with boot.
	StartupCppCheckHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[WeakThis = TWeakObjectPtr<UGameIQSaveHookSubsystem>(this)](float) -> bool
		{
			if (UGameIQSaveHookSubsystem* This = WeakThis.Get())
			{
				This->StartupCppCheckHandle.Reset();
				This->CheckCppFreshness();
			}
			return false; // one-shot
		}), 5.0f);

#if WITH_LIVE_CODING
	if (ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME))
	{
		LiveCodingHandle = LiveCoding->GetOnPatchCompleteDelegate().AddUObject(
			this, &UGameIQSaveHookSubsystem::CheckCppFreshness);
	}
#endif

	UE_LOG(LogGameIQHook, Log, TEXT("Game IQ save hook active — saves, deletes, and renames patch the index incrementally (deferred off the save path); C++ freshness is checked on startup and Live Coding patches."));
}

void UGameIQSaveHookSubsystem::Deinitialize()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (StartupCppCheckHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(StartupCppCheckHandle);
		StartupCppCheckHandle.Reset();
	}
#if WITH_LIVE_CODING
	if (LiveCodingHandle.IsValid())
	{
		if (ILiveCodingModule* LiveCoding = FModuleManager::GetModulePtr<ILiveCodingModule>(LIVE_CODING_MODULE_NAME))
		{
			LiveCoding->GetOnPatchCompleteDelegate().Remove(LiveCodingHandle);
		}
		LiveCodingHandle.Reset();
	}
#endif
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

	// World Partition One-File-Per-Actor packages save on every actor tweak; their level-actor
	// entities are owned by the level's recipe (which the build refreshes) — extracting each OFPA
	// package here would produce orphaned generic entities under a different id scheme.
	if (PackageName.Contains(TEXT("/__ExternalActors__/")) || PackageName.Contains(TEXT("/__ExternalObjects__/"))) { return; }

	// Enqueue only — extraction and the SQLite write run from the ticker on a later editor tick,
	// keeping Game IQ entirely off the save critical path (saving a big level used to stall here).
	if (!PendingSet.Contains(PackageName))
	{
		PendingSet.Add(PackageName);
		PendingPackages.Add(PackageName);
	}
	EnsureTicker();
}

void UGameIQSaveHookSubsystem::EnsureTicker()
{
	if (!TickerHandle.IsValid())
	{
		TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UGameIQSaveHookSubsystem::ProcessQueue), 0.25f);
	}
}

bool UGameIQSaveHookSubsystem::ProcessQueue(float /*DeltaTime*/)
{
	if (PendingPackages.Num() == 0)
	{
		TickerHandle.Reset();
		return false; // stop ticking until the next save
	}

	// One package per tick keeps each editor hitch small even after a bulk save-all.
	const FString PackageName = PendingPackages[0];
	PendingPackages.RemoveAt(0);
	PendingSet.Remove(PackageName);
	PatchSavedPackage(PackageName);
	return true;
}

void UGameIQSaveHookSubsystem::PatchSavedPackage(const FString& PackageName)
{
	// The package was just saved, so it's still loaded in this editor. If it vanished between
	// save and tick (rare — e.g. save-then-delete), the delete handler already dropped its rows.
	UPackage* Package = FindPackage(nullptr, *PackageName);
	if (!Package)
	{
		UE_LOG(LogGameIQHook, Verbose, TEXT("Game IQ: %s no longer loaded — skipping incremental patch (next build covers it)."), *PackageName);
		return;
	}

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
		// External actors are skipped (bIncludeExternalActors=false): pulling every OFPA package of
		// a World Partition level would mean loading hundreds of packages on the game thread.
		const FString ClassName = Asset->GetClass()->GetName();
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("assetClass"), ClassName);
		Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
			Id, TEXT("asset"), Asset->GetName(), PackageName, TEXT("asset"), FString(),
			FString::Printf(TEXT("%s (%s)"), *Asset->GetName(), *ClassName), Detail)));

		GameIQAsset::ExtractAsset(Asset, Entities, Edges, Chunks, /*RegistryData=*/nullptr, /*bIncludeExternalActors=*/false);
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

	FGameIQStore Store;
	if (Store.Open())
	{
		const bool bOk = Store.Patch({ Id }, Entities, Edges, Chunks, BP ? HookBlueprintProducer : HookAssetsProducer);
		Store.Close();
		if (bOk)
		{
			UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: patched index for %s"), *PackageName);
		}
		else
		{
			UE_LOG(LogGameIQHook, Warning, TEXT("Game IQ: incremental patch for %s failed — the next build will refresh it."), *PackageName);
		}
	}
}

void UGameIQSaveHookSubsystem::OnAssetRemoved(const FAssetData& AssetData)
{
	const FString PackageName = AssetData.PackageName.ToString();
	if (!PackageName.StartsWith(TEXT("/Game"))) { return; }
	if (PackageName.Contains(TEXT("/__ExternalActors__/")) || PackageName.Contains(TEXT("/__ExternalObjects__/"))) { return; }

	// Immediate: dropping a subtree is cheap (no extraction).
	DropSubtree(AssetIdFromPackage(PackageName));
	UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: removed %s from the index"), *PackageName);
}

void UGameIQSaveHookSubsystem::OnAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath)
{
	const FString OldPackage = PackageFromObjectPath(OldObjectPath);
	if (!OldPackage.StartsWith(TEXT("/Game"))) { return; }

	// Drop the old id; the rename saves the new package, so the save queue re-extracts the
	// asset under its new id. Deleting the old subtree here keeps the stale entity from lingering.
	DropSubtree(AssetIdFromPackage(OldPackage));
	UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: dropped renamed %s (new id follows on save)"), *OldPackage);
}

void UGameIQSaveHookSubsystem::DropSubtree(const FString& EntityId)
{
	FGameIQStore Store;
	if (Store.Open())
	{
		Store.Patch({ EntityId }, {}, {}, {}, HookAssetsProducer);
		Store.Close();
	}
}

void UGameIQSaveHookSubsystem::CheckCppFreshness()
{
	if (bCppCheckInFlight) { return; }
	if (!GetDefault<UGameIQSettings>()->bAutoReindexCpp) { return; }

	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	// No index, or the code stage has never run → nothing to keep fresh. A first full build is a
	// deliberate user action (GameIQ.Rebuild), not something to launch behind their back.
	if (!FPaths::FileExists(FPaths::Combine(Root, TEXT(".gameiq"), TEXT("index.db")))) { return; }
	if (!FPaths::FileExists(GameIQCppFingerprint::CppJsonPath(Root))) { return; }
	if (FGameIQBuildRunner::Get().IsBuilding()) { return; }

	// The fingerprint is stat-only but still walks the whole source tree — keep it off the game
	// thread; only the compare + build kick hop back.
	bCppCheckInFlight = true;
	TWeakObjectPtr<UGameIQSaveHookSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [WeakThis, Root]()
	{
		const FString Current = GameIQCppFingerprint::Compute(Root);
		const FString Stored = GameIQCppFingerprint::ReadStored(Root);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Current, Stored]()
		{
			UGameIQSaveHookSubsystem* This = WeakThis.Get();
			if (!This) { return; }
			This->bCppCheckInFlight = false;
			// An empty Stored (pre-fingerprint index) counts as stale: the reindex it triggers
			// stamps the first fingerprint, so this self-heals one build later.
			if (Current == Stored) { return; }
			if (FGameIQBuildRunner::Get().IsBuilding()) { return; }
			UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: C++ sources changed since the last code extraction — reindexing code in the background."));
			FGameIQBuildRunner::Get().StartBuild(TEXT("GameIQCppBuild"),
				NSLOCTEXT("GameIQ", "AutoReindexCppStarted", "Game IQ: C++ changed — reindexing code in the background…"));
		});
	});
}
