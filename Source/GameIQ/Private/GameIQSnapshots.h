// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Local, review-oriented snapshots of the GameIQ index. Snapshots are consistent SQLite copies
 * stored beside the source index and never leave the machine. The query layer exposes the
 * functions through UGameIQService; keeping the implementation separate prevents the core
 * retrieval file from becoming a second store implementation.
 *
 * Snapshots are tied to git (issue #36): every manifest records `gitHead`/`gitBranch`, the editor
 * takes an automatic snapshot when HEAD moves (CreateForCommit), and Changes accepts a git ref or
 * an ISO time wherever it accepts a snapshot id.
 */
namespace GameIQSnapshots
{
	FString Create(const FString& Name, const FString& MetadataJson = TEXT("{}"));
	FString List(int32 Limit = 50);
	FString Remove(const FString& Id);

	/** `SinceSnapshot` / `UntilSnapshot`: a snapshot id, "current", a git ref (sha, branch, tag,
	 *  "HEAD~3") or an ISO-8601 time. A ref resolves to the snapshot taken at that commit, else the
	 *  newest snapshot taken at or before the commit time; a time to the newest snapshot at or before it. */
	FString Changes(const FString& SinceSnapshot, const FString& UntilSnapshot = TEXT("current"),
		const FString& Kind = TEXT(""), const FString& PathPrefix = TEXT(""),
		int32 Limit = 200, int32 Offset = 0);

	/** Automatic per-commit snapshot (metadata.auto = true). No-op (returns empty) when a snapshot
	 *  for `GitHead` already exists. Prunes automatic snapshots beyond `MaxAuto`. Returns the id. */
	FString CreateForCommit(const FString& GitHead, const FString& GitBranch, int32 MaxAuto);

	/** Newest snapshot's recorded gitHead (empty if none) — the editor compares HEAD against it. */
	FString LatestSnapshotGitHead();
}
