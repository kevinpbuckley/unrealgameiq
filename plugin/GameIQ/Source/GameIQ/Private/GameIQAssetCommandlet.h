// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Dom/JsonValue.h"
#include "GameIQAssetCommandlet.generated.h"

struct FAssetData;

/** Shared Tier 1 extraction so both the commandlet and the in-editor save hook use one recipe. */
namespace GameIQAsset
{
	/**
	 * Append one non-Blueprint asset's recipe (chunks/edges/child entities) for the given loaded
	 * object. `RegistryData` (optional) lets recipes prefer AssetRegistry tags over derived-data
	 * accessors (e.g. triangle counts without touching render data). `bIncludeExternalActors`
	 * controls whether a World Partition level pulls its One-File-Per-Actor packages — expensive,
	 * so the save hook (game-thread, mid-save) passes false and leaves that to the build.
	 */
	void ExtractAsset(
		UObject* Asset,
		TArray<TSharedPtr<FJsonValue>>& Entities,
		TArray<TSharedPtr<FJsonValue>>& Edges,
		TArray<TSharedPtr<FJsonValue>>& Chunks,
		const FAssetData* RegistryData = nullptr,
		bool bIncludeExternalActors = true);
}

/**
 * Tier 1 extractor (design §5.1): typed per-asset summaries for the non-Blueprint
 * assets — static/skeletal meshes, textures, skeletons, data tables, and levels
 * (actor inventory) — plus semantic edges (uses-material, uses-skeleton,
 * placed-in-level). Anything without a bespoke recipe gets a tag-based summary from
 * the AssetRegistry with NO UObject load (the majority of assets on marketplace-heavy
 * projects), so no asset is invisible and the stage stays fast. Run headless:
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQAssets [-out=<dir>] [-full]
 *
 * Incremental by default: an asset whose AssetRegistry content hash matches the last run's
 * signature cache (`asset-hashes.json`) is skipped entirely, and the output is a producer-scoped
 * DELTA (changed + removed ids) applied on top of the rows already in the index. Pass -full
 * (or delete the cache) to force a from-scratch snapshot.
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
