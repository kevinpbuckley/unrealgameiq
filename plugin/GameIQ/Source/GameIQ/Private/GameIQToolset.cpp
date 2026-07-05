// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQToolset.h"

#include "GameIQQuery.h"

FString UGameIQService::Search(const FString& Query, const FString& Kind, int32 Limit, int32 Offset)
{
	return GameIQQuery::Search(Query, Kind, Limit > 0 ? Limit : 20, Offset);
}

FString UGameIQService::GetEntity(const FString& Id)
{
	return GameIQQuery::GetEntity(Id, /*Cap=*/50);
}

FString UGameIQService::References(const FString& Id, const FString& Direction, int32 Depth,
	const FString& EdgeType, const FString& Kind, int32 Offset)
{
	return GameIQQuery::References(Id, Direction, Depth, EdgeType, Kind, /*Limit=*/200, Offset);
}

FString UGameIQService::Impact(const FString& Id, int32 Limit)
{
	return GameIQQuery::Impact(Id, /*MaxDepth=*/4, Limit);
}

FString UGameIQService::Explain(const FString& Topic)
{
	return GameIQQuery::Explain(Topic);
}

FString UGameIQService::ProjectStats(const FString& Facet)
{
	return GameIQQuery::ProjectStats(Facet);
}

FString UGameIQService::Coverage(const FString& DocType)
{
	return GameIQQuery::Coverage(DocType);
}

FString UGameIQService::Drift()
{
	return GameIQQuery::Drift();
}

FString UGameIQService::Doctor()
{
	return GameIQQuery::Doctor();
}
