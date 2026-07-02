// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQBuildCommandlet.generated.h"

/**
 * One-shot full index build (design §5.2): runs every extractor (registry, assets, blueprints,
 * config, code) then GameIQIndex, all in a single editor boot — the Node-free replacement for the
 * old multi-step `gameiq index`. Nothing outside Unreal is involved.
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQBuild
 */
UCLASS()
class UGameIQBuildCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQBuildCommandlet();
	virtual int32 Main(const FString& Params) override;
};
