// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "UObject/ObjectSaveContext.h"
#include "GameIQSaveHook.generated.h"

class UPackage;
struct FAssetData;

/**
 * Incremental index updates (design §8). While the editor is open this subsystem keeps the
 * Game IQ index in step with edits: on save it extracts the changed asset in-memory (same
 * recipe as the commandlets) and writes a delta to `<project>/.gameiq/extract/incremental/`;
 * on delete/rename it writes a delta that removes the asset. The MCP server drains pending
 * deltas before its next query, so an agent always sees the current project — no full rebuild,
 * no watcher, no editor stall.
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

	/** Serialize a delta to a unique file in the incremental dir. */
	void WriteDelta(
		const TArray<FString>& Replaces,
		const TArray<TSharedPtr<FJsonValue>>& Entities,
		const TArray<TSharedPtr<FJsonValue>>& Edges,
		const TArray<TSharedPtr<FJsonValue>>& Chunks);

	FDelegateHandle SaveHandle;
	FDelegateHandle RemovedHandle;
	FDelegateHandle RenamedHandle;
};
