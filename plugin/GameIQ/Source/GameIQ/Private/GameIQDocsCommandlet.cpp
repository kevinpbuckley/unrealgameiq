// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQDocsCommandlet.h"

#include "Dom/JsonObject.h"
#include "GameIQDocsExtract.h"
#include "GameIQJson.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

// Log category shared with GameIQDocsExtract.h (declared there, defined once here).
DEFINE_LOG_CATEGORY(LogGameIQDocs);

namespace
{
	const TCHAR* DocsProducer = TEXT("gameiq-docs@0.1.0");
}

UGameIQDocsCommandlet::UGameIQDocsCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQDocsCommandlet::Main(const FString& /*Params*/)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	TArray<TSharedPtr<FJsonValue>> Entities, Edges, Chunks;

	int32 Walked = 0, Skipped = 0;
	const int32 Docs = GameIQDocsExtract::ExtractTree(
		Root, TEXT("repo"), FString(), Entities, Edges, Chunks, Walked, Skipped, /*bSkipPlugins=*/true);

	const FString OutDir = FPaths::Combine(Root, TEXT(".gameiq"), TEXT("extract"));
	IFileManager::Get().MakeDirectory(*OutDir, true);
	GameIQ::WriteOutput(OutDir, TEXT("docs.json"), DocsProducer, Entities, Edges, Chunks);

	UE_LOG(LogGameIQDocs, Display,
		TEXT("Game IQ docs: %d documents (%d files walked, %d skipped) → %d entities, %d chunks → docs.json"),
		Docs, Walked, Skipped, Entities.Num(), Chunks.Num());
	return 0;
}
