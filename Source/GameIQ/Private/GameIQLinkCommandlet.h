// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQLinkCommandlet.generated.h"

/**
 * Intent→implementation linker (issue #6, Phase 2). Runs AFTER ingest, when every entity is in the
 * index. It reads each `doc-section` (stated intent) and matches it to the implementation entities it
 * mentions — by verbatim entity id (explicit) or by distinctive name/asset token (inferred) — and
 * writes `describes` edges (producer gameiq-doc-links). Those edges power the `coverage` and `drift`
 * queries: "is this design implemented?" and "does the doc's stated value match the build?".
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQLink
 */
UCLASS()
class UGameIQLinkCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQLinkCommandlet();
	virtual int32 Main(const FString& Params) override;
};
