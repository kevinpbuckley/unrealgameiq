// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQConfigCommandlet.generated.h"

/**
 * Editor-less config/project extractor (design §5.1), C++ port of extract/config.ts so the
 * pipeline needs no Node: `.ini` sections and `.uproject`/`.uplugin` enabled plugins. Writes
 * `.gameiq/extract/config.json` (producer gameiq-config), which GameIQIndex ingests.
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQConfig
 */
UCLASS()
class UGameIQConfigCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQConfigCommandlet();
	virtual int32 Main(const FString& Params) override;
};
