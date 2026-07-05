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
 * Registry export runs first, in-process. The other 7 extraction stages (Assets, Blueprints,
 * Config, C++, Docs, Images, Connectors) are independent and run concurrently as their own
 * UnrealEditor-Cmd subprocesses (see GameIQBuildProcess.h), capped to a few at a time. Index and
 * Link then run serially, in-process, once every extraction stage has finished.
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQBuild [-out=<dir>] [-full]
 *
 * -full forces the Assets/Blueprints stages to bypass their incremental-hash cache and reprocess
 * every asset from scratch (see GameIQAssetCommandlet.h).
 */
UCLASS()
class UGameIQBuildCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQBuildCommandlet();
	virtual int32 Main(const FString& Params) override;
};
