// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQCppBuildCommandlet.generated.h"

/**
 * C++-only reindex. Re-runs just the code extractor, ingests only its producer into the existing
 * index (producer-scoped, so asset / Blueprint / docs entities are left untouched), and refreshes
 * the intent→implementation links. Much faster than a full GameIQBuild when only source changed,
 * and safe to run against a live index. Triggered automatically by the editor when the source
 * fingerprint no longer matches the last extraction (see GameIQSaveHook), or manually via the
 * GameIQ.ReindexCode console command.
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQCppBuild
 */
UCLASS()
class UGameIQCppBuildCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQCppBuildCommandlet();
	virtual int32 Main(const FString& Params) override;
};
