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

	// gameiq.config.json `docsPath` narrows the scan to a dedicated docs folder; unset = whole tree.
	FString DocsRoot = Root;
	const FString DocsPath = GameIQWalk::LoadDocsPath(Root);
	if (!DocsPath.IsEmpty())
	{
		DocsRoot = FPaths::IsRelative(DocsPath) ? FPaths::Combine(Root, DocsPath) : DocsPath;
		DocsRoot = FPaths::ConvertRelativePathToFull(DocsRoot);
		if (!IFileManager::Get().DirectoryExists(*DocsRoot))
		{
			UE_LOG(LogGameIQDocs, Warning, TEXT("Game IQ docs: docsPath '%s' not found (%s) — no documents indexed."),
				*DocsPath, *DocsRoot);
		}
	}

	int32 Walked = 0, Skipped = 0;
	const int32 Docs = GameIQDocsExtract::ExtractTree(
		DocsRoot, TEXT("repo"), FString(), Entities, Edges, Chunks, Walked, Skipped, /*bSkipPlugins=*/true);

	const FString OutDir = FPaths::Combine(Root, TEXT(".gameiq"), TEXT("extract"));
	IFileManager::Get().MakeDirectory(*OutDir, true);
	GameIQ::WriteOutput(OutDir, TEXT("docs.json"), DocsProducer, Entities, Edges, Chunks);

	UE_LOG(LogGameIQDocs, Display,
		TEXT("Game IQ docs: %d documents (%d files walked, %d skipped) → %d entities, %d chunks → docs.json"),
		Docs, Walked, Skipped, Entities.Num(), Chunks.Num());
	return 0;
}
