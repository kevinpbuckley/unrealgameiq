// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQToolset.h"

#include "GameIQQuery.h"

FString UGameIQService::Search(const FString& Query, const FString& Kind, int32 Limit)
{
	return GameIQQuery::Search(Query, Kind, Limit > 0 ? Limit : 20);
}

FString UGameIQService::GetEntity(const FString& Id)
{
	return GameIQQuery::GetEntity(Id, /*Cap=*/50);
}
