// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQConnectorsCommandlet.h"

#include "Dom/JsonObject.h"
#include "GameIQDocsExtract.h"
#include "GameIQJson.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQConnectors, Log, All);

namespace
{
	const TCHAR* ConnectorsProducer = TEXT("gameiq-docs-external@0.1.0");

	struct FExternalSource { FString Source; FString Path; };

	/** Read gameiq.config.json `externalDocs: [{source, path}]`. */
	TArray<FExternalSource> LoadExternalSources(const FString& Root)
	{
		TArray<FExternalSource> Out;
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *FPaths::Combine(Root, TEXT("gameiq.config.json")))) { return Out; }
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid() && Obj->TryGetArrayField(TEXT("externalDocs"), Arr))
		{
			for (const TSharedPtr<FJsonValue>& V : *Arr)
			{
				const TSharedPtr<FJsonObject> O = V.IsValid() ? V->AsObject() : nullptr;
				if (!O.IsValid()) { continue; }
				FExternalSource S;
				O->TryGetStringField(TEXT("source"), S.Source);
				O->TryGetStringField(TEXT("path"), S.Path);
				if (!S.Source.IsEmpty() && !S.Path.IsEmpty()) { Out.Add(S); }
			}
		}
		return Out;
	}
}

UGameIQConnectorsCommandlet::UGameIQConnectorsCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQConnectorsCommandlet::Main(const FString& /*Params*/)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	const TArray<FExternalSource> Sources = LoadExternalSources(Root);

	TArray<TSharedPtr<FJsonValue>> Entities, Edges, Chunks;
	int32 TotalDocs = 0;

	for (const FExternalSource& Src : Sources)
	{
		const FString Full = FPaths::ConvertRelativePathToFull(Src.Path);
		if (!IFileManager::Get().DirectoryExists(*Full))
		{
			UE_LOG(LogGameIQConnectors, Warning, TEXT("Game IQ connectors: '%s' path not found: %s"), *Src.Source, *Full);
			continue;
		}
		int32 Walked = 0, Skipped = 0;
		// Namespaced ids ("confluence:doc:...") so external mirrors never collide with in-repo docs.
		const FString Prefix = Src.Source + TEXT(":");
		const int32 N = GameIQDocsExtract::ExtractTree(
			Full, Src.Source, Prefix, Entities, Edges, Chunks, Walked, Skipped, /*bSkipPlugins=*/false);
		TotalDocs += N;
		UE_LOG(LogGameIQConnectors, Display, TEXT("Game IQ connectors: '%s' → %d docs (%d walked, %d skipped) from %s"),
			*Src.Source, N, Walked, Skipped, *Full);
	}

	// Always write (producer-scoped) so removing a source clears its docs on the next build.
	const FString OutDir = FPaths::Combine(Root, TEXT(".gameiq"), TEXT("extract"));
	IFileManager::Get().MakeDirectory(*OutDir, true);
	GameIQ::WriteOutput(OutDir, TEXT("external-docs.json"), ConnectorsProducer, Entities, Edges, Chunks);

	if (Sources.Num() == 0)
	{
		UE_LOG(LogGameIQConnectors, Display,
			TEXT("Game IQ connectors: no `externalDocs` configured in gameiq.config.json — nothing to sync. "
			     "Add [{source,path}] entries pointing at a Confluence/Notion/Drive export folder."));
	}
	else
	{
		UE_LOG(LogGameIQConnectors, Display,
			TEXT("Game IQ connectors: %d external documents from %d source(s) → %d entities → external-docs.json"),
			TotalDocs, Sources.Num(), Entities.Num());
	}
	return 0;
}
