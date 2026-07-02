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
}
