// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "GameIQConnectorsCommandlet.generated.h"

/**
 * External documentation connectors (issue #8, Phase 4). Reaches design docs that live *outside* the
 * repo — the canonical home for most studios (Confluence, Notion, Google Drive/Docs).
 *
 * v0 (this commandlet) ingests a **local export** of those systems: point it at a folder of exported
 * markdown/HTML (every one of Confluence/Notion/Drive can export to these) via gameiq.config.json:
 *
 *   { "externalDocs": [ { "source": "confluence", "path": "C:/exports/wiki" },
 *                        { "source": "notion",     "path": "D:/notion-export" } ] }
 *
 * Each doc is indexed with full provenance (source + stated-intent), namespaced ids, and participates
 * in search/coverage/drift just like in-repo docs. Live API sync (auth + incremental pull) is future
 * work tracked in issue #8; this gives real cross-source reach today without credentials in the index.
 *
 *   UnrealEditor-Cmd <Project>.uproject -run=GameIQConnectors
 */
UCLASS()
class UGameIQConnectorsCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UGameIQConnectorsCommandlet();
	virtual int32 Main(const FString& Params) override;
};
