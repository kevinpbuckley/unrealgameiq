// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQBlueprintCommandlet.generated.h"

/**
 * Tier 2 extractor (design §5.1, the killer feature): loads every project
 * Blueprint and renders its event/function graphs to greppable pseudocode text,
 * plus `calls` edges into C++. This is the "my agent just read my Blueprint" pass.
 * Needs the editor (loads assets); run headless as a commandlet:
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQBlueprints [-out=<dir>]
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
