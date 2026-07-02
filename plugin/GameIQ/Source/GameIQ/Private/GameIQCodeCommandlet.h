// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQCodeCommandlet.generated.h"

/**
 * Editor-less C++ reflection-surface extractor (design §5.1), C++ port of extract/cpp.ts so the
 * pipeline needs no Node. Lightweight header parse — UE's reflection macros make headers regular
 * enough that regex + brace-matching gets UCLASS/USTRUCT/UINTERFACE, the class hierarchy, and
 * UFUNCTION/UPROPERTY members with property→type references. Canonical ids use the prefix-less
 * reflection name so BP→C++ edges from the UE commandlet resolve here. Writes
 * `.gameiq/extract/cpp.json` (producer gameiq-cpp), which GameIQIndex ingests.
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQCode
 */
UCLASS()
class UGameIQCodeCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQCodeCommandlet();
	virtual int32 Main(const FString& Params) override;
};
