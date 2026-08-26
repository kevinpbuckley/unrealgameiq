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
	/** Full-text search over the whole index (stemmed, camelCase-aware — "player" matches
	 *  BP_PlayerCharacter; name-match and reference-count weighted); `Kind` optionally filters to
	 *  one entity kind (e.g. "blueprint", "asset", "level-actor"); `Offset` pages through results.
	 *  `PathPrefix` scopes hits to one content path (e.g. "/Game/Coursera" — matches asset ids and
	 *  file paths). Returns a JSON string. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString Search(const FString& Query, const FString& Kind = TEXT(""), int32 Limit = 20, int32 Offset = 0,
		const FString& PathPrefix = TEXT(""));

	/** Full detail for one entity id — entity fields, edges (in/out), child entities, and chunks,
	 *  with arrays capped and full counts reported. Children are round-robin by class (every class
	 *  represented) with a `childrenByClass` rollup. Returns a JSON string. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString GetEntity(const FString& Id);

	/** Page through an entity's children (e.g. a level's placed actors), optionally filtered by
	 *  class — `ClassFilter` substring-matches the child's class (level-actor `detail.class`) or
	 *  entity kind, so Children(mapId, "Light") lists every light. Returns a JSON string with
	 *  `total` for the filter. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString Children(const FString& Id, const FString& ClassFilter = TEXT(""), int32 Limit = 50, int32 Offset = 0);

	/** Graph walk from an entity: who uses it / what it uses. `Direction` is "in", "out", or "both".
	 *  `EdgeType` optionally restricts to one edge (e.g. "uses-skeleton", "calls"); `Kind` filters
	 *  results to one entity kind; `Offset` pages through results. Returns a JSON string. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString References(const FString& Id, const FString& Direction = TEXT("both"), int32 Depth = 1,
		const FString& EdgeType = TEXT(""), const FString& Kind = TEXT(""), int32 Offset = 0);

	/** "What could break if this changes" — transitive inbound dependents ranked by severity.
	 *  Top `Limit` rows with full per-kind counts and a `truncated` flag. Returns a JSON string. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString Impact(const FString& Id, int32 Limit = 200);

	/** Assemble a bounded context bundle for a topic/system. `DetailLevel` is summary, standard,
	 *  or full; `Kinds` is a comma-separated allow-list; `Format` is json or markdown. The minimum
	 *  accepted `MaxChars` is 512 so the provenance/error envelope remains honest. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString Explain(const FString& Topic, const FString& DetailLevel = TEXT("standard"),
		int32 MaxChars = 40000, const FString& Kinds = TEXT(""), int32 Depth = 1,
		const FString& Format = TEXT("json"));

	/** Export one Blueprint's compact context by GameIQ id or /Game path. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString GetBlueprintContext(const FString& IdOrPath, const FString& DetailLevel = TEXT("logic"),
		int32 MaxChars = 40000, const FString& Format = TEXT("json"));

	/** Export a bounded project graph as Mermaid, Graphviz DOT, or structured JSON. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString ExportGraph(const FString& TopicOrId, const FString& Format = TEXT("mermaid"),
		const FString& Direction = TEXT("both"), int32 Depth = 1,
		const FString& EdgeTypes = TEXT(""), const FString& Kinds = TEXT(""), int32 MaxNodes = 50);

	/** Create a consistent named local snapshot of the index. `MetadataJson` must be an object. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString CreateSnapshot(const FString& Name, const FString& MetadataJson = TEXT("{}"));

	/** List local snapshots, newest first. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString ListSnapshots(int32 Limit = 50);

	/** Remove one local snapshot by id. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString DeleteSnapshot(const FString& Id);

	/** Semantic entity/edge/chunk changes between snapshots, or a snapshot and "current". */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString Changes(const FString& SinceSnapshot, const FString& UntilSnapshot = TEXT("current"),
		const FString& Kind = TEXT(""), const FString& PathPrefix = TEXT(""),
		int32 Limit = 200, int32 Offset = 0);

	/** Project-wide stats. `Facet` is "overview", "kinds", "edges", "unused", "largest-deps",
	 *  "authority" (extracted-fact vs stated-intent), or "doc-types". Returns a JSON string. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString ProjectStats(const FString& Facet = TEXT("overview"));

	/** Design → implementation coverage: which documentation (stated design intent) is backed by a real
	 *  implementation in the index and which isn't. Optional `DocType` (e.g. "game-design",
	 *  "level-design") restricts the report. Design docs are stated intent, not ground truth. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString Coverage(const FString& DocType = TEXT(""));

	/** Design ↔ build drift: doc sections stating a `property = value` that contradicts the value
	 *  extracted from the implementation they describe (e.g. GDD says speed 2000, Blueprint sets 1500).
	 *  Returns a JSON string. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString Drift();

	/** Index health check: schema/tables, row counts, per-producer breakdown, FTS consistency,
	 *  freshness stamps. Use to tell "empty project" apart from "broken index" before trusting
	 *  empty query results. Returns a JSON string. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "GameIQ")
	static FString Doctor();
};
