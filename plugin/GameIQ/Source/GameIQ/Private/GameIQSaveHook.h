// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "EditorSubsystem.h"
#include "UObject/ObjectSaveContext.h"
#include "GameIQSaveHook.generated.h"

class UPackage;
struct FAssetData;

/**
 * Incremental index updates (design §8). While the editor is open this subsystem keeps the
 * Game IQ index in step with edits: a save enqueues the package and a low-frequency ticker
 * extracts + patches SQLite on the next editor tick(s) — extraction and the DB write happen
 * OFF the save critical path, so saving never stalls on Game IQ. Deletes/renames patch
 * immediately (no extraction, just a subtree drop). No full rebuild, no watcher, no Node.
 *
 * C++ has no save event to hook (source edits happen outside the editor), so code freshness is
 * checked at the two moments new code can appear: editor startup and Live Coding patch complete.
 * A stat-only fingerprint of the source tree (GameIQCppFingerprint) is compared against the one
 * stamped by the last code extraction; on mismatch a background C++-only reindex (GameIQCppBuild)
 * launches via the shared build runner. Governed by UGameIQSettings::bAutoReindexCpp.
 */
UCLASS()
class UGameIQSaveHookSubsystem : public UEditorSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	void OnPackageSaved(const FString& PackageFilename, UPackage* Package, FObjectPostSaveContext Context);
	void OnAssetRemoved(const FAssetData& AssetData);
	void OnAssetRenamed(const FAssetData& AssetData, const FString& OldObjectPath);

	/** Ticker callback: extract + patch one queued package per tick. */
	bool ProcessQueue(float DeltaTime);
	void EnsureTicker();

	/** Extract the (still-loaded) package and patch the index. */
	void PatchSavedPackage(const FString& PackageName);

	/** Apply a subtree-drop patch (delete/rename) directly — cheap, no extraction. */
	void DropSubtree(const FString& EntityId);

	/** Compare the source-tree fingerprint with the last code extraction's; reindex C++ on mismatch. */
	void CheckCppFreshness();

	/** Saved packages waiting for extraction (deduped; insertion-ordered). */
	TArray<FString> PendingPackages;
	TSet<FString> PendingSet;
	FTSTicker::FDelegateHandle TickerHandle;

	FDelegateHandle SaveHandle;
	FDelegateHandle RemovedHandle;
	FDelegateHandle RenamedHandle;

	/** One-shot startup delay before the first C++ freshness check. */
	FTSTicker::FDelegateHandle StartupCppCheckHandle;
	/** Live Coding patch-complete subscription (bound only when Live Coding is compiled in). */
	FDelegateHandle LiveCodingHandle;
	/** Guards overlapping fingerprint walks (startup and a Live Coding patch can coincide). */
	bool bCppCheckInFlight = false;
};
