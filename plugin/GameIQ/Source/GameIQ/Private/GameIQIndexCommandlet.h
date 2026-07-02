// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQIndexCommandlet.generated.h"

/**
 * Full index build in C++ (design §5.2, Phase 3b): ingest the extractor outputs under
 * `<ProjectDir>/.gameiq/extract/*.json` straight into the SQLite index via FGameIQStore —
 * the same job the Node `gameiq index` did, now editor-native so the pipeline needs no Node.
 * Producer-scoped merge, so re-running replaces each producer's rows. Run headless after the
 * extract commandlets:
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQIndex
 *
 * (Config extraction — the `gameiq-config` producer — is still TypeScript-only; see design §6.5.)
 */
UCLASS()
class UGameIQIndexCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQIndexCommandlet();

	//~ Begin UCommandlet interface
	virtual int32 Main(const FString& Params) override;
	//~ End UCommandlet interface
};
