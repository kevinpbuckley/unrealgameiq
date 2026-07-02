// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQDocsCommandlet.generated.h"

/**
 * In-repo documentation extractor (issue #2). Walks the project's docs (markdown/text/HTML/RTF),
 * classifies each by type (issue #4), splits it into heading-delimited sections, and emits a
 * `document` entity + one `doc-section` child entity/chunk per section — all tagged `stated-intent`
 * (issue #3) so design intent is never confused with extracted ground truth. Writes
 * `.gameiq/extract/docs.json` (producer gameiq-docs), which GameIQIndex ingests.
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQDocs
 */
UCLASS()
class UGameIQDocsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQDocsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
