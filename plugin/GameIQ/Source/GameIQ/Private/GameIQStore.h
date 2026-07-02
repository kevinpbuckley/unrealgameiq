// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "SQLiteDatabase.h"

/**
 * In-editor write layer (design §5.2 / §8) — the C++ port of the TypeScript store+ingest,
 * writing the same `<ProjectDir>/.gameiq/index.db` (schema in lockstep with packages/core).
 * With this, the editor owns the whole pipeline: the save hook patches the index directly
 * and the GameIQIndex commandlet builds it — no Node ingest step. Uses UE's SQLiteCore and a
 * rollback journal (matching the TS store) so the standalone stdio server and the toolset can
 * still read the file concurrently.
 */
class FGameIQStore
{
public:
	/** Open (creating if needed) the project index read-write and ensure the schema. */
	bool Open();
	void Close();
	bool IsValid() const { return Db.IsValid(); }

	/**
	 * Incremental patch (save hook, §8): for each id in `Replaces` (or every top-level entity if
	 * empty), delete its subtree + owned edges/chunks, then upsert the delta. Inbound edges kept.
	 */
	void Patch(
		const TArray<FString>& Replaces,
		const TArray<TSharedPtr<FJsonValue>>& Entities,
		const TArray<TSharedPtr<FJsonValue>>& Edges,
		const TArray<TSharedPtr<FJsonValue>>& Chunks,
		const FString& Producer);

	/** Full-build per-producer merge (commandlet): drop this producer's rows, then insert. */
	void IngestProducer(
		const FString& Producer,
		const TArray<TSharedPtr<FJsonValue>>& Entities,
		const TArray<TSharedPtr<FJsonValue>>& Edges,
		const TArray<TSharedPtr<FJsonValue>>& Chunks);

	/** Stamp project identity + freshness meta (mirrors ingest.ts). */
	void SetProjectMeta(const FString& Name, const FString& Root, const FString& GeneratedAtIso);

private:
	FSQLiteDatabase Db;

	void EnsureSchema();
	void DeleteSubtree(const FString& RootId);
	void DeleteByProducer(const FString& Producer);
	void UpsertEntity(const FJsonObject& E, const FString& Producer);
	void InsertEdge(const FJsonObject& E, const FString& Producer);
	void InsertChunk(const FJsonObject& C, const FString& Producer);
	void SetMeta(const FString& Key, const FString& Value);
};
