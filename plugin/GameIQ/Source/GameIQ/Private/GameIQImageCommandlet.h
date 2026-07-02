// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQImageCommandlet.generated.h"

/**
 * Image ingestion (issue #7, Phase 3). Indexes reference imagery — concept art, level layout maps,
 * brand assets, wireframes — as `image` entities carrying path + dimensions + tags (+ an optional
 * sidecar caption), never the pixels. The MCP response hands back the file path so a vision-capable
 * agent can open the real image on demand; keyword search finds it via its tags/caption. Writes
 * `.gameiq/extract/images.json` (producer gameiq-images, stated-intent).
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQImages
 */
UCLASS()
class UGameIQImageCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQImageCommandlet();
	virtual int32 Main(const FString& Params) override;
};
