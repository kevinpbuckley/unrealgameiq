// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQBuildCommandlet.h"

#include "GameIQAssetCommandlet.h"
#include "GameIQBlueprintCommandlet.h"
#include "GameIQCodeCommandlet.h"
#include "GameIQConfigCommandlet.h"
#include "GameIQExportCommandlet.h"
#include "GameIQIndexCommandlet.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQBuild, Log, All);

UGameIQBuildCommandlet::UGameIQBuildCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQBuildCommandlet::Main(const FString& /*Params*/)
{
	UE_LOG(LogGameIQBuild, Display, TEXT("Game IQ: full build — running all extractors, then index."));

	// Extraction: each writes its .gameiq/extract/*.json (order doesn't matter — producer-scoped).
	NewObject<UGameIQExportCommandlet>()->Main(FString());     // registry.json (Tier 0)
	NewObject<UGameIQAssetsCommandlet>()->Main(FString());     // assets.json  (Tier 1)
	NewObject<UGameIQBlueprintsCommandlet>()->Main(FString()); // blueprints.json (Tier 2)
	NewObject<UGameIQConfigCommandlet>()->Main(FString());     // config.json
	NewObject<UGameIQCodeCommandlet>()->Main(FString());       // cpp.json

	// Ingest everything into the SQLite index.
	return NewObject<UGameIQIndexCommandlet>()->Main(FString());
}
