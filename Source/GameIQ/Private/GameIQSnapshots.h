// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Local, review-oriented snapshots of the GameIQ index. Snapshots are consistent SQLite copies
 * stored beside the source index and never leave the machine. The query layer exposes the
 * functions through UGameIQService; keeping the implementation separate prevents the core
 * retrieval file from becoming a second store implementation.
 */
namespace GameIQSnapshots
{
	FString Create(const FString& Name, const FString& MetadataJson = TEXT("{}"));
	FString List(int32 Limit = 50);
	FString Remove(const FString& Id);
	FString Changes(const FString& SinceSnapshot, const FString& UntilSnapshot = TEXT("current"),
		const FString& Kind = TEXT(""), const FString& PathPrefix = TEXT(""),
		int32 Limit = 200, int32 Offset = 0);
}
