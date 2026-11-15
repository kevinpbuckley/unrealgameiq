// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQSaveHook.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "DirectoryWatcherModule.h"
#include "GameIQAssetCommandlet.h"
#include "GameIQAssetFacts.h"
#include "GameIQBlueprintCommandlet.h"
#include "GameIQBuildRunner.h"
#include "GameIQCppFingerprint.h"
#include "GameIQGit.h"
#include "GameIQJson.h"
#include "GameIQProjectChecks.h"
#include "GameIQSettings.h"
#include "GameIQSnapshots.h"
#include "GameIQStore.h"
#include "IDirectoryWatcher.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SQLiteDatabase.h"
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
				const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
				if (!IsRunningCommandlet() && !GameIQAgentFiles::Check(ProjectRoot)->GetBoolField(TEXT("present")))
				{
					UE_LOG(LogGameIQHook, Warning, TEXT("Game IQ: no agent file (CLAUDE.md, AGENTS.md, GEMINI.md, .github/copilot-instructions.md) carries the GameIQ guide, so coding agents are not told the index exists. Run the console command GameIQ.GenerateAgentConfig (or Tools > Game IQ > Write Agent Guide)."));
				}
				This->CheckCppFreshness();
				This->CheckDocsFreshness();
				This->StartDocsWatcher();
				// Catch-up waits for the registry's initial scan; if it already finished, go now.
				IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
				if (!AR.IsLoadingAssets()) { This->StartCatchUpScan(); }
			}
			return false; // one-shot
		}), 5.0f);

	if (AutomationAllowed())
	{
		// Content that arrived without an editor save (issue #35): diff once the registry has scanned.
		FilesLoadedHandle = ARM.Get().OnFilesLoaded().AddUObject(this, &UGameIQSaveHookSubsystem::StartCatchUpScan);
		// Per-commit snapshots (issue #36): HEAD is a file read, cheap enough to poll.
		GitHeadHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UGameIQSaveHookSubsystem::CheckGitHead), 30.0f);
	}

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
	for (FTSTicker::FDelegateHandle* Handle : { &DocsDebounceHandle, &QueueHandle, &CatchUpHandle, &GitHeadHandle })
	{
		if (Handle->IsValid())
		{
			FTSTicker::GetCoreTicker().RemoveTicker(*Handle);
			Handle->Reset();
		}
	}
	StopDocsWatcher();
	if (FilesLoadedHandle.IsValid())
	{
		if (FAssetRegistryModule* ARMod = FModuleManager::GetModulePtr<FAssetRegistryModule>(TEXT("AssetRegistry")))
		{
			ARMod->Get().OnFilesLoaded().Remove(FilesLoadedHandle);
		}
		FilesLoadedHandle.Reset();
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

	// Registry view of the just-saved package: size and class facts (issue #33), same as the build.
	FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FAssetData> PackageAssets;
	ARM.Get().GetAssetsByPackageName(Package->GetFName(), PackageAssets);
	auto AddFacts = [&PackageAssets](const TSharedRef<FJsonObject>& Detail)
	{
		if (PackageAssets.Num() > 0) { GameIQAssetFacts::FillFacts(PackageAssets[0], Detail); }
	};

	if (BP)
	{
		// The blueprint entity itself (registry producer normally emits this; the delta replaces
		// the whole subtree rooted at Id, so it must re-include the root).
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("assetClass"), TEXT("Blueprint"));
		if (BP->ParentClass) { Detail->SetStringField(TEXT("parentClass"), BP->ParentClass->GetName()); }
		AddFacts(Detail);
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
		AddFacts(Detail);
		Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
			Id, TEXT("asset"), Asset->GetName(), PackageName, TEXT("asset"), FString(),
			FString::Printf(TEXT("%s (%s)"), *Asset->GetName(), *ClassName), Detail)));

		GameIQAsset::ExtractAsset(Asset, Entities, Edges, Chunks, PackageAssets.Num() > 0 ? &PackageAssets[0] : nullptr,
			/*bIncludeExternalActors=*/false);
	}

	// Dependency edges (keep "what this asset uses" fresh).
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
		// World saves patch UPSERT-ONLY: the hook re-extracts without external actors (loading
		// hundreds of OFPA packages mid-save would stall the editor), so the subtree delete was
		// wiping every build-produced external-actor entity of a World Partition level on each
		// save (issue #17). Upserting keeps them; actors deleted from the level linger until the
		// next build — which was already the documented refresh path for external actors.
		const bool bIsWorld = !BP && Asset->IsA<UWorld>();
		const bool bOk = bIsWorld
			? Store.Patch({}, Entities, Edges, Chunks, HookAssetsProducer)
			: Store.Patch({ Id }, Entities, Edges, Chunks, BP ? HookBlueprintProducer : HookAssetsProducer);
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
	if (bCppCheckInFlight || !AutomationAllowed()) { return; }
	if (!GetDefault<UGameIQSettings>()->bAutoReindexCpp) { return; }

	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	// No index, or the code stage has never run → nothing to keep fresh. A first full build is a
	// deliberate user action (GameIQ.Rebuild), not something to launch behind their back.
	if (!FPaths::FileExists(FPaths::Combine(Root, TEXT(".gameiq"), TEXT("index.db")))) { return; }
	if (!FPaths::FileExists(GameIQCppFingerprint::CppJsonPath(Root))) { return; }

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
			UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: C++ sources changed since the last code extraction — reindexing code in the background."));
			// Queued, not dropped, when another build holds the runner (a docs reindex used to make
			// the C++ check give up until the next editor start).
			This->QueueReindex(TEXT("GameIQCppBuild"));
		});
	});
}

bool UGameIQSaveHookSubsystem::AutomationAllowed()
{
	return !IsRunningCommandlet() && !FParse::Param(FCommandLine::Get(), TEXT("NoGameIQAuto"));
}

// ---- Background reindex queue --------------------------------------------------------------

void UGameIQSaveHookSubsystem::QueueReindex(const FString& RunArg)
{
	QueuedRuns.AddUnique(RunArg);
	if (!QueueHandle.IsValid())
	{
		QueueHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UGameIQSaveHookSubsystem::PumpReindexQueue), 1.0f);
	}
}

bool UGameIQSaveHookSubsystem::PumpReindexQueue(float /*DeltaTime*/)
{
	if (QueuedRuns.Num() == 0)
	{
		QueueHandle.Reset();
		return false;
	}
	if (FGameIQBuildRunner::Get().IsBuilding()) { return true; } // try again next second
	const FString Run = QueuedRuns[0];
	QueuedRuns.RemoveAt(0);
	const FText Label = Run == TEXT("GameIQDocsBuild")
		? NSLOCTEXT("GameIQ", "AutoReindexDocsStarted", "Game IQ: design docs changed — reindexing documents in the background…")
		: NSLOCTEXT("GameIQ", "AutoReindexCppStarted", "Game IQ: C++ changed — reindexing code in the background…");
	FGameIQBuildRunner::Get().StartBuild(Run, Label);
	return true;
}

// ---- Docs auto-reindex (issue #29) ---------------------------------------------------------

void UGameIQSaveHookSubsystem::CheckDocsFreshness()
{
	if (bDocsCheckInFlight || !AutomationAllowed()) { return; }
	if (!GetDefault<UGameIQSettings>()->bAutoReindexDocs) { return; }

	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	// Same rule as C++: no index, or the docs stage never ran → a first build is the user's call.
	if (!FPaths::FileExists(FPaths::Combine(Root, TEXT(".gameiq"), TEXT("index.db")))) { return; }
	if (!FPaths::FileExists(FPaths::Combine(Root, TEXT(".gameiq"), TEXT("extract"), TEXT("docs.json")))) { return; }

	bDocsCheckInFlight = true;
	TWeakObjectPtr<UGameIQSaveHookSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [WeakThis, Root]()
	{
		const FString Current = GameIQDocsFingerprint::Compute(Root);
		const FString Stored = GameIQDocsFingerprint::ReadStored(Root);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Current, Stored]()
		{
			UGameIQSaveHookSubsystem* This = WeakThis.Get();
			if (!This) { return; }
			This->bDocsCheckInFlight = false;
			// Empty Stored = an index from before docs fingerprints: reindex once, which stamps one.
			if (Current == Stored) { return; }
			UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: design docs changed since the last docs extraction — reindexing documents in the background."));
			This->LastDocsReindexQueuedAt = FPlatformTime::Seconds();
			This->QueueReindex(TEXT("GameIQDocsBuild"));
		});
	});
}

void UGameIQSaveHookSubsystem::StartDocsWatcher()
{
	if (!AutomationAllowed() || !GetDefault<UGameIQSettings>()->bAutoReindexDocs || DocsWatcherHandle.IsValid()) { return; }
	FDirectoryWatcherModule& DWM = FModuleManager::LoadModuleChecked<FDirectoryWatcherModule>(TEXT("DirectoryWatcher"));
	IDirectoryWatcher* Watcher = DWM.Get();
	if (!Watcher) { return; }

	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	WatchedDocsDir = GameIQDocsFingerprint::DocsRoot(Root);
	if (!IFileManager::Get().DirectoryExists(*WatchedDocsDir)) { return; }
	Watcher->RegisterDirectoryChangedCallback_Handle(WatchedDocsDir,
		IDirectoryWatcher::FDirectoryChanged::CreateUObject(this, &UGameIQSaveHookSubsystem::OnDocsDirectoryChanged),
		DocsWatcherHandle, 0);
	UE_LOG(LogGameIQHook, Log, TEXT("Game IQ: watching %s for design-doc changes (auto docs reindex)."), *WatchedDocsDir);
}

void UGameIQSaveHookSubsystem::StopDocsWatcher()
{
	if (!DocsWatcherHandle.IsValid()) { return; }
	if (FDirectoryWatcherModule* DWM = FModuleManager::GetModulePtr<FDirectoryWatcherModule>(TEXT("DirectoryWatcher")))
	{
		if (IDirectoryWatcher* Watcher = DWM->Get())
		{
			Watcher->UnregisterDirectoryChangedCallback_Handle(WatchedDocsDir, DocsWatcherHandle);
		}
	}
	DocsWatcherHandle.Reset();
}

void UGameIQSaveHookSubsystem::OnDocsDirectoryChanged(const TArray<FFileChangeData>& Changes)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	bool bDocTouched = false;
	for (const FFileChangeData& Change : Changes)
	{
		if (GameIQDocsFingerprint::IsDocFile(Root, FPaths::ConvertRelativePathToFull(Change.Filename)))
		{
			bDocTouched = true;
			break;
		}
	}
	if (!bDocTouched) { return; }

	// Debounce: an editor saving a doc fires several events, and an agent edits docs in bursts.
	// Re-arm a 10 s timer on every doc event; the fingerprint check runs once things go quiet.
	// A reindex is a whole UnrealEditor-Cmd process, so the timer also never fires sooner than
	// the minimum interval after the previous automatic docs reindex.
	float Delay = 10.0f;
	if (LastDocsReindexQueuedAt > 0.0)
	{
		const double MinInterval = static_cast<double>(GetDefault<UGameIQSettings>()->AutoReindexDocsMinIntervalSeconds);
		Delay = FMath::Max(Delay, static_cast<float>(MinInterval - (FPlatformTime::Seconds() - LastDocsReindexQueuedAt)));
	}
	if (DocsDebounceHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(DocsDebounceHandle);
	}
	DocsDebounceHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[WeakThis = TWeakObjectPtr<UGameIQSaveHookSubsystem>(this)](float) -> bool
		{
			if (UGameIQSaveHookSubsystem* This = WeakThis.Get())
			{
				This->DocsDebounceHandle.Reset();
				This->CheckDocsFreshness();
			}
			return false;
		}), Delay);
}

// ---- Startup catch-up scan (issue #35) -----------------------------------------------------

void UGameIQSaveHookSubsystem::StartCatchUpScan()
{
	if (bCatchUpDone || !AutomationAllowed() || !GetDefault<UGameIQSettings>()->bCatchUpOnStartup) { return; }
	bCatchUpDone = true;

	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const FString DbPath = FPaths::Combine(Root, TEXT(".gameiq"), TEXT("index.db"));
	if (!FPaths::FileExists(DbPath)) { return; } // no index yet: the first build is the user's call

	// Root asset ids currently in the index.
	TSet<FString> Indexed;
	{
		FSQLiteDatabase Db;
		if (!Db.Open(*DbPath, ESQLiteDatabaseOpenMode::ReadOnly)) { return; }
		{
			FSQLitePreparedStatement Stmt(Db,
				TEXT("SELECT substr(id, 7) AS pkg FROM entities WHERE id LIKE 'asset:/Game/%' AND parent IS NULL"));
			while (Stmt.IsValid() && Stmt.Step() == ESQLitePreparedStatementStepResult::Row)
			{
				FString Pkg;
				Stmt.GetColumnValueByName(TEXT("pkg"), Pkg);
				Indexed.Add(Pkg);
			}
		}
		Db.Close();
	}
	if (Indexed.Num() == 0) { return; } // empty index: a full build, not a catch-up, is what's needed

	IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	TArray<FAssetData> Assets;
	AR.GetAssetsByPath(FName(TEXT("/Game")), Assets, /*bRecursive=*/true, /*bIncludeOnlyOnDiskAssets=*/true);

	TSet<FString> InRegistry;
	for (const FAssetData& Data : Assets)
	{
		const FString Pkg = Data.PackageName.ToString();
		if (Pkg.Contains(TEXT("/__ExternalActors__/")) || Pkg.Contains(TEXT("/__ExternalObjects__/"))) { continue; }
		if (InRegistry.Contains(Pkg)) { continue; } // one row per package (its first asset)
		InRegistry.Add(Pkg);
		if (!Indexed.Contains(Pkg)) { CatchUpMissing.Add(Data); }
	}
	for (const FString& Pkg : Indexed)
	{
		if (Pkg.Contains(TEXT("/__ExternalActors__/")) || Pkg.Contains(TEXT("/__ExternalObjects__/"))) { continue; }
		if (!InRegistry.Contains(Pkg) && !FPackageName::DoesPackageExist(Pkg)) { CatchUpGone.Add(Pkg); }
	}

	UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: catch-up scan — %d asset(s) on disk missing from the index, %d indexed asset(s) gone from disk."),
		CatchUpMissing.Num(), CatchUpGone.Num());
	if (CatchUpMissing.Num() > 0 || CatchUpGone.Num() > 0)
	{
		CatchUpHandle = FTSTicker::GetCoreTicker().AddTicker(
			FTickerDelegate::CreateUObject(this, &UGameIQSaveHookSubsystem::CatchUpTick), 0.1f);
	}
}

bool UGameIQSaveHookSubsystem::CatchUpTick(float /*DeltaTime*/)
{
	// A running build re-exports the whole registry anyway; let it finish first.
	if (FGameIQBuildRunner::Get().IsBuilding()) { return true; }

	static const TCHAR* CatchUpRegistryProducer = TEXT("gameiq-ue-registry@0.1.0");
	FGameIQStore Store;
	if (!Store.Open()) { CatchUpHandle.Reset(); return false; }

	if (CatchUpGone.Num() > 0)
	{
		// Full subtree drop (every producer): the package no longer exists.
		Store.Patch(CatchUpGone, {}, {}, {}, HookAssetsProducer);
		UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: catch-up removed %d asset(s) no longer on disk."), CatchUpGone.Num());
		CatchUpGone.Reset();
	}

	// Registry-only work (no loads), 200 packages per tick keeps each hitch small.
	const int32 Batch = FMath::Min(200, CatchUpMissing.Num());
	if (Batch > 0)
	{
		IAssetRegistry& AR = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
		TArray<FString> Ids;
		TArray<TSharedPtr<FJsonValue>> RegEntities, RegEdges, TagEntities, TagEdges, TagChunks;
		for (int32 i = 0; i < Batch; ++i)
		{
			const FAssetData& Data = CatchUpMissing[i];
			Ids.Add(AssetIdFromPackage(Data.PackageName.ToString()));
			GameIQAssetFacts::AppendTier0(AR, Data, RegEntities, RegEdges);
			GameIQAsset::ExtractFromRegistryTags(Data, TagEntities, TagEdges, TagChunks);
		}
		const bool bOk = Store.PatchProducerScoped(Ids, RegEntities, RegEdges, {}, CatchUpRegistryProducer)
			&& Store.PatchProducerScoped(Ids, TagEntities, TagEdges, TagChunks, HookAssetsProducer);
		CatchUpMissing.RemoveAt(0, Batch);
		CatchUpAdded += bOk ? Batch : 0;
	}
	Store.Close();

	if (CatchUpMissing.Num() == 0)
	{
		if (CatchUpAdded > 0)
		{
			UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: catch-up added %d asset(s) that arrived without an editor save (registry-level rows; GameIQ.Rebuild adds deep recipes)."),
				CatchUpAdded);
		}
		CatchUpHandle.Reset();
		return false;
	}
	return true;
}

// ---- Per-commit snapshots (issue #36) ------------------------------------------------------

bool UGameIQSaveHookSubsystem::CheckGitHead(float /*DeltaTime*/)
{
	const UGameIQSettings* Settings = GetDefault<UGameIQSettings>();
	if (!Settings->bSnapshotPerCommit || bSnapshotInFlight) { return true; }

	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	if (!FPaths::FileExists(FPaths::Combine(Root, TEXT(".gameiq"), TEXT("index.db")))) { return true; }
	FString Sha, Branch;
	if (!GameIQGit::ReadHead(Root, Sha, Branch) || Sha == LastSeenGitHead) { return true; }
	// Snapshot a quiescent index: a running build is mid-ingest.
	if (FGameIQBuildRunner::Get().IsBuilding()) { return true; }

	bSnapshotInFlight = true;
	const int32 MaxAuto = Settings->MaxAutoSnapshots;
	TWeakObjectPtr<UGameIQSaveHookSubsystem> WeakThis(this);
	Async(EAsyncExecution::Thread, [WeakThis, Sha, Branch, MaxAuto]()
	{
		const FString Id = GameIQSnapshots::CreateForCommit(Sha, Branch, MaxAuto);
		AsyncTask(ENamedThreads::GameThread, [WeakThis, Sha, Id]()
		{
			UGameIQSaveHookSubsystem* This = WeakThis.Get();
			if (!This) { return; }
			This->bSnapshotInFlight = false;
			This->LastSeenGitHead = Sha;
			if (!Id.IsEmpty())
			{
				UE_LOG(LogGameIQHook, Display, TEXT("Game IQ: git HEAD is now %s — took index snapshot %s (Changes accepts the commit as 'since')."),
					*Sha.Left(10), *Id);
			}
		});
	});
	return true;
}
