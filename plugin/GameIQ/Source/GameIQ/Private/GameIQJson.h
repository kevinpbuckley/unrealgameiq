// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/**
 * Shared builders for the ExtractorOutput JSON contract (packages/shared).
 * `inline` so multiple commandlet translation units (which UE merges in unity
 * builds) can include this without ODR/redefinition conflicts.
 */
namespace GameIQ
{
	inline constexpr int32 SchemaVersion = 1; // lockstep with packages/shared SCHEMA_VERSION

	/** Collapse a string to a single trimmed line (shared by the commandlets). */
	inline FString OneLine(const FString& In)
	{
		return In.Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" ")).TrimStartAndEnd();
	}

	/**
	 * Provenance/authority of an indexed fact (issue #3). Code/BP/asset/config are extracted from
	 * the source of truth and self-freshen — `extracted-fact`. Design/documentation is what a human
	 * *said*, and drifts from the build — `stated-intent`. The two must never be confused: a stale
	 * design doc must not read as ground truth. Ingest defaults a missing authority to extracted-fact,
	 * so only the docs producers opt into stated-intent.
	 */
	namespace Authority
	{
		inline const TCHAR* ExtractedFact = TEXT("extracted-fact");
		inline const TCHAR* StatedIntent = TEXT("stated-intent");
	}

	inline TSharedRef<FJsonObject> MakeEntity(
		const FString& Id, const FString& Kind, const FString& Name, const FString& Path,
		const FString& Source, const FString& Parent, const FString& Summary,
		const TSharedPtr<FJsonObject>& Detail, const FString& AuthorityTag = FString())
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), Id);
		O->SetStringField(TEXT("kind"), Kind);
		O->SetStringField(TEXT("name"), Name);
		O->SetStringField(TEXT("path"), Path);
		O->SetStringField(TEXT("source"), Source);
		if (!Parent.IsEmpty()) { O->SetStringField(TEXT("parent"), Parent); }
		if (!Summary.IsEmpty()) { O->SetStringField(TEXT("summary"), Summary); }
		if (Detail.IsValid()) { O->SetObjectField(TEXT("detail"), Detail); }
		if (!AuthorityTag.IsEmpty()) { O->SetStringField(TEXT("authority"), AuthorityTag); }
		return O;
	}

	inline TSharedRef<FJsonObject> MakeEdge(
		const FString& Src, const FString& Dst, const FString& Type, const FString& Via = FString())
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("src"), Src);
		O->SetStringField(TEXT("dst"), Dst);
		O->SetStringField(TEXT("type"), Type);
		if (!Via.IsEmpty())
		{
			TSharedRef<FJsonObject> Attrs = MakeShared<FJsonObject>();
			Attrs->SetStringField(TEXT("via"), Via);
			O->SetObjectField(TEXT("attrs"), Attrs);
		}
		return O;
	}

	inline TSharedRef<FJsonObject> MakeChunk(
		const FString& Id, const FString& EntityId, const FString& Kind, const FString& Text)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), Id);
		O->SetStringField(TEXT("entityId"), EntityId);
		O->SetStringField(TEXT("kind"), Kind);
		O->SetStringField(TEXT("text"), Text);
		return O;
	}

	/**
	 * Serialize a full ExtractorOutput and save it to OutDir/FileName. Returns success.
	 *
	 * `Replaces` (optional) switches the file to DELTA mode: it lists the entity ids whose
	 * producer-owned subtrees this output replaces (changed + removed assets). Ingest applies a
	 * delta with FGameIQStore::PatchProducerScoped on top of the rows already in the index,
	 * instead of a full DeleteByProducer + reinsert — so an incremental extract only serializes
	 * what actually changed. Pass nullptr (default) for a full snapshot.
	 */
	inline bool WriteOutput(
		const FString& OutDir, const FString& FileName, const FString& Producer,
		const TArray<TSharedPtr<FJsonValue>>& Entities,
		const TArray<TSharedPtr<FJsonValue>>& Edges,
		const TArray<TSharedPtr<FJsonValue>>& Chunks,
		const TArray<FString>* Replaces = nullptr)
	{
		TSharedRef<FJsonObject> Project = MakeShared<FJsonObject>();
		Project->SetStringField(TEXT("name"), FApp::GetProjectName());
		Project->SetStringField(TEXT("root"), FPaths::ConvertRelativePathToFull(FPaths::ProjectDir()));

		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schemaVersion"), SchemaVersion);
		Root->SetStringField(TEXT("generatedAtIso"), FDateTime::UtcNow().ToIso8601());
		Root->SetStringField(TEXT("producer"), Producer);
		Root->SetObjectField(TEXT("project"), Project);
		Root->SetArrayField(TEXT("entities"), Entities);
		Root->SetArrayField(TEXT("edges"), Edges);
		Root->SetArrayField(TEXT("chunks"), Chunks);
		if (Replaces)
		{
			Root->SetStringField(TEXT("mode"), TEXT("delta"));
			TArray<TSharedPtr<FJsonValue>> ReplacesJson;
			for (const FString& R : *Replaces) { ReplacesJson.Add(MakeShared<FJsonValueString>(R)); }
			Root->SetArrayField(TEXT("replaces"), ReplacesJson);
		}

		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		// Force UTF-8 without BOM: pseudocode contains non-ASCII (→, …) which would
		// otherwise make SaveStringToFile emit UTF-16+BOM and break JSON parsers.
		return FFileHelper::SaveStringToFile(
			Out, *FPaths::Combine(OutDir, FileName), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

}
