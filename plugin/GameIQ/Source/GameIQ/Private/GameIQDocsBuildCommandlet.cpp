// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQDocsBuildCommandlet.h"

#include "GameIQConnectorsCommandlet.h"
#include "GameIQDocsCommandlet.h"
#include "GameIQImageCommandlet.h"
#include "GameIQIndexCommandlet.h"
#include "GameIQLinkCommandlet.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQDocsBuild, Log, All);

UGameIQDocsBuildCommandlet::UGameIQDocsBuildCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQDocsBuildCommandlet::Main(const FString& /*Params*/)
{
	UE_LOG(LogGameIQDocsBuild, Display, TEXT("Game IQ: documents-only reindex (no asset/Blueprint/C++ re-scan)."));

	// Re-run only the documentation extractors.
	NewObject<UGameIQDocsCommandlet>()->Main(FString());       // docs.json
	NewObject<UGameIQImageCommandlet>()->Main(FString());      // images.json
	NewObject<UGameIQConnectorsCommandlet>()->Main(FString()); // external-docs.json

	// Ingest ONLY those producers — producer-scoped replace leaves everything else in the index intact.
	// '+' separates the files (FParse::Value truncates a value at a comma).
	const int32 Result = NewObject<UGameIQIndexCommandlet>()->Main(
		TEXT("only=docs.json+images.json+external-docs.json"));

	// Refresh intent→implementation links (describes/illustrates) against the existing code/asset entities.
	NewObject<UGameIQLinkCommandlet>()->Main(FString());
	return Result;
}
