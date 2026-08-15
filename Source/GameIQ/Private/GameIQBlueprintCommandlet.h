// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "Dom/JsonValue.h"
#include "GameIQBlueprintCommandlet.generated.h"

class UBlueprint;

/** Shared Tier 2 extraction so both the commandlet and the in-editor save hook use one recipe. */
namespace GameIQBlueprint
{
	/** Append this Blueprint's entities/edges/chunks (graphs → pseudocode, variables, components, interfaces). */
	void ExtractBlueprint(
		UBlueprint* Blueprint,
		TArray<TSharedPtr<FJsonValue>>& Entities,
		TArray<TSharedPtr<FJsonValue>>& Edges,
		TArray<TSharedPtr<FJsonValue>>& Chunks);
}

/**
 * Tier 2 extractor (design §5.1, the killer feature): loads every project
 * Blueprint and renders its event/function graphs to greppable pseudocode text,
 * plus `calls` edges into C++. This is the "my agent just read my Blueprint" pass.
 * Needs the editor (loads assets); run headless as a commandlet:
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQBlueprints [-out=<dir>] [-full]
 *
 * Incremental by default — see GameIQAssetCommandlet.h; this producer's side-car is
 * `.gameiq/extract/blueprint-hashes.json`.
 *
 * Default output: <ProjectDir>/.gameiq/extract/blueprints.json
 */
UCLASS()
class UGameIQBlueprintsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQBlueprintsCommandlet();

	//~ Begin UCommandlet interface
	virtual int32 Main(const FString& Params) override;
	//~ End UCommandlet interface
};
