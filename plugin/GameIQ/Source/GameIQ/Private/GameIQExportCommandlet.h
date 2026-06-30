// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQExportCommandlet.generated.h"

/**
 * Headless Tier 0 extractor (design §5.1). Reads the Asset Registry — identity,
 * class, and the dependency/referencer graph — for every project asset and writes
 * an ExtractorOutput JSON the Game IQ core ingests. No asset loading, no editor UI.
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQExport [-out=<dir>]
 *
 * Default output: <ProjectDir>/.gameiq/extract/registry.json
 */
UCLASS()
class UGameIQExportCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQExportCommandlet();

	//~ Begin UCommandlet interface
	virtual int32 Main(const FString& Params) override;
	//~ End UCommandlet interface
};
