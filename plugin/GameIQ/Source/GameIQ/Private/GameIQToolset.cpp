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

FString UGameIQService::References(const FString& Id, const FString& Direction, int32 Depth,
	const FString& EdgeType, const FString& Kind)
{
	return GameIQQuery::References(Id, Direction, Depth, EdgeType, Kind);
}

FString UGameIQService::Impact(const FString& Id)
{
	return GameIQQuery::Impact(Id);
}

FString UGameIQService::Explain(const FString& Topic)
{
	return GameIQQuery::Explain(Topic);
}

FString UGameIQService::ProjectStats(const FString& Facet)
{
	return GameIQQuery::ProjectStats(Facet);
}
