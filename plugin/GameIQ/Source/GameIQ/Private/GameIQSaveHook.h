// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "EditorSubsystem.h"
#include "UObject/ObjectSaveContext.h"
#include "GameIQSaveHook.generated.h"

class UPackage;

/**
 * Incremental index updates (design §8). While the editor is open, this editor subsystem
 * hooks package saves: when a Blueprint is saved, it extracts that Blueprint in-memory
 * (instant — it's already loaded) with the same recipe as the commandlet and writes a delta
 * to `<project>/.gameiq/extract/incremental/`. The running MCP server drains that delta
 * before its next query, so an agent always sees the just-saved state — no full rebuild,
 * no watcher process, no editor stall.
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
	FDelegateHandle SaveHandle;
};
