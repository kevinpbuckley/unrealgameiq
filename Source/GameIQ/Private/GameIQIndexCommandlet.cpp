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

	// Optional `-skipfiles=assets.json+…`: extract outputs the build orchestrator judged stale
	// (their stage failed to refresh them this run) — ingesting an old delta would re-apply
	// out-of-date rows on top of newer save-hook patches.
	TSet<FString> SkipFiles;
	FString Skip;
	if (FParse::Value(*Params, TEXT("skipfiles="), Skip) && !Skip.IsEmpty())
	{
		TArray<FString> Parts;
		Skip.ParseIntoArray(Parts, TEXT("+"));
		for (const FString& P : Parts) { const FString T = P.TrimStartAndEnd(); if (!T.IsEmpty()) { SkipFiles.Add(T); } }
	}

	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(ExtractDir / TEXT("*.json")), /*Files=*/true, /*Directories=*/false);
	// The signature caches are extractor-private state, not ExtractorOutput — filter them out
	// up front instead of fully parsing 60+ MB of JSON just to discover there's no `producer`.
	Files.RemoveAll([](const FString& F) { return F.EndsWith(TEXT("-hashes.json")) || F.EndsWith(TEXT(".tmp")); });
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
	int32 FailedIngests = 0;
	bool bMetaSet = false;

	for (const FString& File : Files)
	{
		if (OnlyFiles.Num() > 0 && !OnlyFiles.Contains(File)) { continue; }
		if (SkipFiles.Contains(File))
		{
			UE_LOG(LogGameIQIndex, Warning, TEXT("Game IQ: skipping stale extract output %s (its stage did not refresh it this run)."), *File);
			continue;
		}
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

		// Delta mode (incremental extract): apply on top of the rows already in the index,
		// replacing only this producer's rows for the listed subtrees. Full mode: wipe + reinsert.
		FString Mode;
		Root->TryGetStringField(TEXT("mode"), Mode);
		bool bOk;
		if (Mode == TEXT("delta"))
		{
			TArray<FString> Replaces;
			const TArray<TSharedPtr<FJsonValue>>* ReplacesJson = nullptr;
			if (Root->TryGetArrayField(TEXT("replaces"), ReplacesJson))
			{
				for (const TSharedPtr<FJsonValue>& V : *ReplacesJson)
				{
					FString Id;
					if (V.IsValid() && V->TryGetString(Id) && !Id.IsEmpty()) { Replaces.Add(Id); }
				}
			}
			if (Replaces.Num() == 0 && Entities->Num() == 0 && Edges->Num() == 0 && Chunks->Num() == 0)
			{
				UE_LOG(LogGameIQIndex, Display, TEXT("  %s: no changes (delta empty)"), *Producer);
				bOk = true;
			}
			else
			{
				bOk = Store.PatchProducerScoped(Replaces, *Entities, *Edges, *Chunks, Producer);
				UE_LOG(LogGameIQIndex, Display, TEXT("  %s: delta — %d replaced roots, %d entities, %d edges, %d chunks%s"),
					*Producer, Replaces.Num(), Entities->Num(), Edges->Num(), Chunks->Num(), bOk ? TEXT("") : TEXT(" — FAILED"));
			}
		}
		else
		{
			bOk = Store.IngestProducer(Producer, *Entities, *Edges, *Chunks);
			UE_LOG(LogGameIQIndex, Display, TEXT("  %s: %d entities, %d edges, %d chunks%s"),
				*Producer, Entities->Num(), Edges->Num(), Chunks->Num(), bOk ? TEXT("") : TEXT(" — FAILED"));
		}
		if (!bOk) { ++FailedIngests; continue; }
		TotalEntities += Entities->Num();
		TotalEdges += Edges->Num();
		TotalChunks += Chunks->Num();

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

	// Verify what actually landed in the DB — extract-file counts are not proof of ingestion
	// (the July 2026 empty-index bug shipped "success" logs against a schema-less DB).
	int64 DbEntities = 0, DbEdges = 0, DbChunks = 0;
	Store.GetCounts(DbEntities, DbEdges, DbChunks);
	Store.Close();

	UE_LOG(LogGameIQIndex, Display, TEXT("Game IQ: ingested %d files (%d entities, %d edges, %d chunks in this pass)."),
		Files.Num(), TotalEntities, TotalEdges, TotalChunks);
	UE_LOG(LogGameIQIndex, Display, TEXT("Game IQ: INDEX VERIFIED — DB now holds %lld entities, %lld edges, %lld chunks."),
		DbEntities, DbEdges, DbChunks);

	if (FailedIngests > 0)
	{
		UE_LOG(LogGameIQIndex, Error, TEXT("Game IQ: %d producer ingest(s) FAILED — the index is incomplete for those producers."), FailedIngests);
		return 1;
	}
	if (DbEntities == 0)
	{
		UE_LOG(LogGameIQIndex, Error, TEXT("Game IQ: INDEX EMPTY after ingest — something is wrong (schema, permissions, or empty extracts). Queries will return nothing."));
		return 1;
	}
	return 0;
}
