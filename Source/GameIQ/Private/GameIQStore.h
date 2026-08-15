// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonValue.h"
#include "SQLiteDatabase.h"

/**
 * In-editor write layer (design §5.2 / §8) — owns `<ProjectDir>/.gameiq/index.db`.
 * The save hook patches the index directly and the GameIQIndex commandlet builds it.
 * Uses UE's SQLiteCore with a rollback journal — WAL is impossible here: UE's custom SQLite VFS
 * is iVersion 1 with no shared-memory (xShm*) methods, so `PRAGMA journal_mode=WAL` silently
 * no-ops. Contention is handled by busy_timeout plus a bounded BEGIN IMMEDIATE retry loop.
 *
 * Every SQL statement is executed individually with its result checked: UE's
 * FSQLiteDatabase::Execute prepares a SINGLE statement (sqlite3 prepare semantics — anything
 * after the first ';' is silently discarded), which is why the original multi-statement
 * migration strings created only the first table while stamping the schema as complete.
 */
class FGameIQStore
{
public:
	/** Open (creating if needed) the project index read-write and ensure the schema.
	 *  Self-heals: an inconsistent schema (missing tables despite a stamped user_version)
	 *  is rebuilt from scratch, and the extract hash caches are invalidated via a fresh
	 *  dbGeneration so the next build doesn't skip "unchanged" assets against an empty DB. */
	bool Open();

	/** Open a specific DB file (tests). Same schema/self-heal behavior. */
	bool OpenAtPath(const FString& DbPath);

	void Close();
	bool IsValid() const { return Db.IsValid(); }

	/** Absolute path of the project index DB. */
	static FString DefaultDbPath();

	/** Read one meta value without keeping a handle open (e.g. dbGeneration for cache checks).
	 *  Empty if the DB or key doesn't exist. */
	static FString ReadMetaValue(const FString& Key);

	/**
	 * Incremental patch (save hook, §8): for each id in `Replaces` (or every top-level entity if
	 * empty), delete its subtree + owned edges/chunks, then upsert the delta. Inbound edges kept.
	 * Returns false if the transaction could not be applied (e.g. persistent lock contention).
	 */
	bool Patch(
		const TArray<FString>& Replaces,
		const TArray<TSharedPtr<FJsonValue>>& Entities,
		const TArray<TSharedPtr<FJsonValue>>& Edges,
		const TArray<TSharedPtr<FJsonValue>>& Chunks,
		const FString& Producer);

	/**
	 * Producer-scoped delta (incremental extract ingest): for each id in `Replaces`, delete only
	 * the rows OWNED BY `Producer` in that id's subtree, then insert the delta rows. Unlike
	 * Patch(), rows other producers own on the same entities (e.g. the registry's root asset
	 * entity and its depends-on edges) are left untouched, so extract deltas compose with the
	 * registry's full refresh in any order.
	 */
	bool PatchProducerScoped(
		const TArray<FString>& Replaces,
		const TArray<TSharedPtr<FJsonValue>>& Entities,
		const TArray<TSharedPtr<FJsonValue>>& Edges,
		const TArray<TSharedPtr<FJsonValue>>& Chunks,
		const FString& Producer);

	/** Full per-producer merge (commandlet): drop this producer's rows, then insert.
	 *  Returns false if the transaction could not be applied. */
	bool IngestProducer(
		const FString& Producer,
		const TArray<TSharedPtr<FJsonValue>>& Entities,
		const TArray<TSharedPtr<FJsonValue>>& Edges,
		const TArray<TSharedPtr<FJsonValue>>& Chunks);

	/** Stamp project identity + freshness meta (mirrors ingest.ts). */
	void SetProjectMeta(const FString& Name, const FString& Root, const FString& GeneratedAtIso);

	void SetMeta(const FString& Key, const FString& Value);
	FString GetMeta(const FString& Key);

	/** Row counts for verification/health: entities, edges, chunks. */
	void GetCounts(int64& OutEntities, int64& OutEdges, int64& OutChunks);

	/** Rows owned by one producer (chunks table). */
	int64 CountChunksForProducer(const FString& Producer);

private:
	FSQLiteDatabase Db;
	FString OpenedPath;

	/** Execute one statement, logging on failure. */
	bool Exec(const TCHAR* Sql);
	/** BEGIN IMMEDIATE with bounded retries on lock contention. */
	bool BeginTransaction();
	bool EnsureSchema();
	/** True if all core tables exist (schema consistent with user_version). */
	bool SchemaLooksValid();
	/** Delete extract hash caches + stamp a fresh dbGeneration (after a DB rebuild). */
	void InvalidateExtractCaches();
	bool OpenInternal(const FString& DbPath, bool bAllowRecreate);

	void DeleteSubtree(const FString& RootId);
	void DeleteSubtreeForProducer(const FString& RootId, const FString& Producer);
	TArray<FString> CollectSubtreeIds(const FString& RootId);
	void DeleteByProducer(const FString& Producer);
	void UpsertEntity(const FJsonObject& E, const FString& Producer);
	void InsertEdge(const FJsonObject& E, const FString& Producer);
	void InsertChunk(const FJsonObject& C, const FString& Producer);
	void InsertRows(
		const TArray<TSharedPtr<FJsonValue>>& Entities,
		const TArray<TSharedPtr<FJsonValue>>& Edges,
		const TArray<TSharedPtr<FJsonValue>>& Chunks,
		const FString& Producer);

	/** Lazily prepared, reused across calls — keyed by SQL text so each distinct
	 *  statement is compiled once per Db lifetime instead of once per row. Cleared in Close(). */
	TMap<FString, FSQLitePreparedStatement> StmtCache;
	FSQLitePreparedStatement& GetCachedStmt(const TCHAR* Sql);
};
