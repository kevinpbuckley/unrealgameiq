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
	/** FTS search across all chunks (porter-stemmed, AND-first with OR and prefix fallbacks,
	 *  camelCase-aware via the aux column); optional `Kind` filters to one entity kind.
	 *  `Offset` pages through results. `PathPrefix` scopes hits to one content path (e.g.
	 *  "/Game/Coursera") — matches asset ids and entity paths. Hits are re-ranked by how much
	 *  of the query appears in the entity NAME and by inbound reference count (log-scale, so
	 *  the asset the project leans on outranks an equal text match nothing uses), and bulk
	 *  asset classes (textures, material instances) are demoted so they can't drown
	 *  intent-shaped queries. */
	FString Search(const FString& Query, const FString& Kind, int32 Limit = 20, int32 Offset = 0,
		const FString& PathPrefix = TEXT(""));

	/** Full detail for one entity id (entity + edges + children + chunks, arrays capped).
	 *  Children are returned round-robin by class (one of each class first) with a
	 *  `childrenByClass` rollup, so a capped list still shows every class. */
	FString GetEntity(const FString& Id, int32 Cap = 50);

	/** Page through an entity's children, optionally filtered by class (level-actor `detail.class`
	 *  or entity kind, substring match) — the drill-down companion to GetEntity's rollup. */
	FString Children(const FString& Id, const FString& ClassFilter = TEXT(""), int32 Limit = 50, int32 Offset = 0);

	/** Graph walk (recursive CTE) from `Id` up to `Depth` hops; optional edge-type / kind filters.
	 *  Direction is "in" | "out" | "both". `Offset` pages through results. */
	FString References(const FString& Id, const FString& Direction, int32 Depth = 1,
		const FString& EdgeType = TEXT(""), const FString& Kind = TEXT(""), int32 Limit = 200, int32 Offset = 0);

	/** "What could break if this changes" — transitive inbound refs ranked by edge severity.
	 *  Capped at `Limit` rows; full byKind counts always reported. */
	FString Impact(const FString& Id, int32 MaxDepth = 4, int32 Limit = 200);

	/** Assembled bundle for a topic: top search hits + their immediate neighborhood. */
	FString Explain(const FString& Topic, int32 Limit = 8);

	/** Project-wide stats. Facet: "overview" | "kinds" | "edges" | "unused" | "largest-deps". */
	FString ProjectStats(const FString& Facet);

	/** Index health: schema/table check, row counts, per-producer breakdown, freshness, journal
	 *  mode, FTS/content consistency — distinguishes "empty project" from "broken index". */
	FString Doctor();

	/** Design/implementation coverage (issue #6): which documentation (stated intent) links to a real
	 *  implementation and which doesn't. Optional `DocType` restricts to one kind of doc. */
	FString Coverage(const FString& DocType = TEXT(""), int32 Limit = 500);

	/** Design/build drift (issue #6): where a doc section states a `key = value` that contradicts the
	 *  extracted value on the implementation entity it describes. */
	FString Drift(int32 Limit = 200);

	/** Tests only: point the query layer at a specific DB file instead of the project index.
	 *  Pass empty to restore the default. */
	void SetDbPathOverrideForTests(const FString& Path);
}
