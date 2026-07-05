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

	/** Saved packages waiting for extraction (deduped; insertion-ordered). */
	TArray<FString> PendingPackages;
	TSet<FString> PendingSet;
	FTSTicker::FDelegateHandle TickerHandle;

	FDelegateHandle SaveHandle;
	FDelegateHandle RemovedHandle;
	FDelegateHandle RenamedHandle;
};
