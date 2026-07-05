// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Dom/JsonValue.h"
#include "GameIQAssetCommandlet.generated.h"

/** Shared Tier 1 extraction so both the commandlet and the in-editor save hook use one recipe. */
namespace GameIQAsset
{
	/** Append one non-Blueprint asset's recipe (chunks/edges/child entities) for the given loaded object. */
	void ExtractAsset(
		UObject* Asset,
		TArray<TSharedPtr<FJsonValue>>& Entities,
		TArray<TSharedPtr<FJsonValue>>& Edges,
		TArray<TSharedPtr<FJsonValue>>& Chunks);
}

/**
 * Tier 1 extractor (design §5.1): typed per-asset summaries for the non-Blueprint
 * assets — static/skeletal meshes, textures, skeletons, data tables, and levels
 * (actor inventory) — plus semantic edges (uses-material, uses-skeleton,
 * placed-in-level). Anything without a bespoke recipe gets a generic summary so
 * no asset is invisible. Loads assets, so it needs the editor; run headless:
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQAssets [-out=<dir>] [-full]
 *
 * Incremental by default: an asset whose AssetRegistry content hash (FAssetPackageData::
 * GetPackageSavedHash) matches the last run's `asset-hashes.json` side-car is never reloaded —
 * its previously-extracted rows are carried forward verbatim. Pass -full (or delete
 * `.gameiq/extract/asset-hashes.json`) to force a from-scratch rebuild.
 *
 * Default output: <ProjectDir>/.gameiq/extract/assets.json
 */
UCLASS()
class UGameIQAssetsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQAssetsCommandlet();

	//~ Begin UCommandlet interface
	virtual int32 Main(const FString& Params) override;
	//~ End UCommandlet interface
};
