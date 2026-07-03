// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQDocsBuildCommandlet.generated.h"

/**
 * Documents-only reindex. Re-runs just the documentation extractors (docs, images, external
 * connectors), ingests only their producers into the existing index (producer-scoped, so code /
 * asset / Blueprint entities are left untouched), and refreshes the intent→implementation links.
 * Much faster than a full `GameIQBuild` when only docs changed, and safe to run against a live index.
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQDocsBuild
 */
UCLASS()
class UGameIQDocsBuildCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQDocsBuildCommandlet();
	virtual int32 Main(const FString& Params) override;
};
