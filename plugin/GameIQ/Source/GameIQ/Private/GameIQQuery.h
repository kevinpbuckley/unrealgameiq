// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * In-editor query layer (design §6.1) — the C++ port of the TypeScript query engine,
 * reading the same `<ProjectDir>/.gameiq/index.db` SQLite index (FTS5) via UE's SQLiteCore.
 * Runs in-process so the toolset answers a query without spawning Node or round-tripping
 * editor APIs. Each function returns a JSON string shaped like the stdio MCP server's output,
 * so an agent gets the same response whether it hits GameIQ headless or in-editor.
 */
namespace GameIQQuery
{
	/** Hybrid FTS search across all chunks; optional `Kind` filters to one entity kind. */
	FString Search(const FString& Query, const FString& Kind, int32 Limit = 20);

	/** Full detail for one entity id (entity + edges + children + chunks, arrays capped). */
	FString GetEntity(const FString& Id, int32 Cap = 50);

	/** BFS over the edge graph from `Id` up to `Depth` hops; optional edge-type / kind filters.
	 *  Direction is "in" | "out" | "both". */
	FString References(const FString& Id, const FString& Direction, int32 Depth = 1,
		const FString& EdgeType = TEXT(""), const FString& Kind = TEXT(""), int32 Limit = 200);

	/** "What could break if this changes" — transitive inbound refs ranked by edge severity. */
	FString Impact(const FString& Id, int32 MaxDepth = 4);

	/** Assembled bundle for a topic: top search hits + their immediate neighborhood. */
	FString Explain(const FString& Topic, int32 Limit = 8);

	/** Project-wide stats. Facet: "overview" | "kinds" | "edges" | "unused" | "largest-deps". */
	FString ProjectStats(const FString& Facet);

	/** Design/implementation coverage (issue #6): which documentation (stated intent) links to a real
	 *  implementation and which doesn't. Optional `DocType` restricts to one kind of doc. */
	FString Coverage(const FString& DocType = TEXT(""), int32 Limit = 500);

	/** Design/build drift (issue #6): where a doc section states a `key = value` that contradicts the
	 *  extracted value on the implementation entity it describes. */
	FString Drift(int32 Limit = 200);
}
