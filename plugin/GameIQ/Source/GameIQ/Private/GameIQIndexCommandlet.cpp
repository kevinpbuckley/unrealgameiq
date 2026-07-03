// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQIndexCommandlet.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameIQStore.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQIndex, Log, All);

UGameIQIndexCommandlet::UGameIQIndexCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UGameIQIndexCommandlet::Main(const FString& Params)
{
	const FString ExtractDir = FPaths::Combine(FPaths::ProjectDir(), TEXT(".gameiq"), TEXT("extract"));

	// Optional `-only=docs.json+images.json+…` restricts ingest to those extract files. Because ingest
	// is producer-scoped (each file's producer rows are replaced), this reindexes just those producers
	// without touching the rest of the index — e.g. a fast docs-only refresh. Files are '+'-separated
	// ('+', not ',', because FParse::Value truncates a value at the first comma).
	TSet<FString> OnlyFiles;
	FString Only;
	if (FParse::Value(*Params, TEXT("only="), Only) && !Only.IsEmpty())
	{
		TArray<FString> Parts;
		Only.ParseIntoArray(Parts, TEXT("+"));
		for (const FString& P : Parts) { const FString T = P.TrimStartAndEnd(); if (!T.IsEmpty()) { OnlyFiles.Add(T); } }
	}

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(ExtractDir / TEXT("*.json")), /*Files=*/true, /*Directories=*/false);
	if (Files.Num() == 0)
	{
		UE_LOG(LogGameIQIndex, Warning, TEXT("Game IQ: no extractor outputs in %s — run the extract commandlets first."), *ExtractDir);
		return 1;
	}

	FGameIQStore Store;
	if (!Store.Open())
	{
		UE_LOG(LogGameIQIndex, Error, TEXT("Game IQ: could not open the index for writing."));
		return 1;
	}

	int32 TotalEntities = 0, TotalEdges = 0, TotalChunks = 0;
	bool bMetaSet = false;

	for (const FString& File : Files)
	{
		if (OnlyFiles.Num() > 0 && !OnlyFiles.Contains(File)) { continue; }
		const FString FullPath = ExtractDir / File;
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *FullPath))
		{
			UE_LOG(LogGameIQIndex, Warning, TEXT("Game IQ: could not read %s"), *FullPath);
			continue;
		}

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			UE_LOG(LogGameIQIndex, Warning, TEXT("Game IQ: could not parse %s"), *FullPath);
			continue;
		}

		FString Producer;
		if (!Root->TryGetStringField(TEXT("producer"), Producer) || Producer.IsEmpty())
		{
			UE_LOG(LogGameIQIndex, Warning, TEXT("Game IQ: %s has no producer; skipping"), *File);
			continue;
		}

		static const TArray<TSharedPtr<FJsonValue>> Empty;
		const TArray<TSharedPtr<FJsonValue>>* Entities = &Empty;
		const TArray<TSharedPtr<FJsonValue>>* Edges = &Empty;
		const TArray<TSharedPtr<FJsonValue>>* Chunks = &Empty;
		Root->TryGetArrayField(TEXT("entities"), Entities);
		Root->TryGetArrayField(TEXT("edges"), Edges);
		Root->TryGetArrayField(TEXT("chunks"), Chunks);

		Store.IngestProducer(Producer, *Entities, *Edges, *Chunks);
		TotalEntities += Entities->Num();
		TotalEdges += Edges->Num();
		TotalChunks += Chunks->Num();
		UE_LOG(LogGameIQIndex, Display, TEXT("  %s: %d entities, %d edges, %d chunks"),
			*Producer, Entities->Num(), Edges->Num(), Chunks->Num());

		if (!bMetaSet)
		{
			const TSharedPtr<FJsonObject>* Project = nullptr;
			FString Name, RootPath, GeneratedAt;
			if (Root->TryGetObjectField(TEXT("project"), Project) && Project && Project->IsValid())
			{
				(*Project)->TryGetStringField(TEXT("name"), Name);
				(*Project)->TryGetStringField(TEXT("root"), RootPath);
			}
			Root->TryGetStringField(TEXT("generatedAtIso"), GeneratedAt);
			Store.SetProjectMeta(Name, RootPath, GeneratedAt);
			bMetaSet = true;
		}
	}

	Store.Close();
	UE_LOG(LogGameIQIndex, Display, TEXT("Game IQ: indexed %d files → %d entities, %d edges, %d chunks."),
		Files.Num(), TotalEntities, TotalEdges, TotalChunks);
	return 0;
}
