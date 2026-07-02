// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQBuildCommandlet.h"

#include "GameIQAssetCommandlet.h"
#include "GameIQBlueprintCommandlet.h"
#include "GameIQCodeCommandlet.h"
#include "GameIQConfigCommandlet.h"
#include "GameIQConnectorsCommandlet.h"
#include "GameIQDocsCommandlet.h"
#include "GameIQExportCommandlet.h"
#include "GameIQImageCommandlet.h"
#include "GameIQIndexCommandlet.h"
#include "GameIQLinkCommandlet.h"

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
	NewObject<UGameIQDocsCommandlet>()->Main(FString());       // docs.json (design/brand/etc. — stated-intent)
	NewObject<UGameIQImageCommandlet>()->Main(FString());      // images.json (concept art/level maps/brand)
	NewObject<UGameIQConnectorsCommandlet>()->Main(FString()); // external-docs.json (Confluence/Notion/Drive exports)

	// Ingest everything into the SQLite index.
	const int32 IndexResult = NewObject<UGameIQIndexCommandlet>()->Main(FString());

	// Phase 2: link doc sections (stated intent) to the implementation they describe. Runs last —
	// it needs every entity already in the index to resolve references. (issue #6)
	NewObject<UGameIQLinkCommandlet>()->Main(FString());
	return IndexResult;
}
