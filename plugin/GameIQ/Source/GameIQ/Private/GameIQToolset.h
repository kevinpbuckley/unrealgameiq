// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "GameIQToolset.generated.h"

/**
 * Game IQ as a native UE 5.8 AI toolset (design §6.1). Registering a UToolsetDefinition
 * with `AICallable` static functions surfaces GameIQ's queries on Epic's MCP endpoint —
 * so any agent already on the native UE MCP or VibeUE can ask GameIQ questions with zero
 * extra setup, and each answer is served in-process from the SQLite index (no Node, no
 * per-call editor API round-trips). The standalone stdio MCP server stays for headless use.
 *
 * Python access mirrors VibeUE's services, e.g.:
 *   unreal.GameIQService.search("reload ammo", "blueprint")
 *   unreal.GameIQService.get_entity("asset:/Game/Blueprints/BP_Rifle")
 */
UCLASS(BlueprintType)
class UGameIQService : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/** Hybrid full-text search over the whole index; `Kind` optionally filters to one entity kind
	 *  (e.g. "blueprint", "asset", "level-actor"). Returns a JSON string. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString Search(const FString& Query, const FString& Kind = TEXT(""), int32 Limit = 20);

	/** Full detail for one entity id — entity fields, edges (in/out), child entities, and chunks,
	 *  with arrays capped and full counts reported. Returns a JSON string. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString GetEntity(const FString& Id);
};
