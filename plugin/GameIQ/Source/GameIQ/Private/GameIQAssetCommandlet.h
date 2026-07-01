// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQAssetCommandlet.generated.h"

/**
 * Tier 1 extractor (design §5.1): typed per-asset summaries for the non-Blueprint
 * assets — static/skeletal meshes, textures, skeletons, data tables, and levels
 * (actor inventory) — plus semantic edges (uses-material, uses-skeleton,
 * placed-in-level). Anything without a bespoke recipe gets a generic summary so
 * no asset is invisible. Loads assets, so it needs the editor; run headless:
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQAssets [-out=<dir>]
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
