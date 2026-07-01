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

	inline TSharedRef<FJsonObject> MakeEntity(
		const FString& Id, const FString& Kind, const FString& Name, const FString& Path,
		const FString& Source, const FString& Parent, const FString& Summary,
		const TSharedPtr<FJsonObject>& Detail)
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

	/** Serialize a full ExtractorOutput and save it to OutDir/FileName. Returns success. */
	inline bool WriteOutput(
		const FString& OutDir, const FString& FileName, const FString& Producer,
		const TArray<TSharedPtr<FJsonValue>>& Entities,
		const TArray<TSharedPtr<FJsonValue>>& Edges,
		const TArray<TSharedPtr<FJsonValue>>& Chunks)
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

		FString Out;
		TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		return FFileHelper::SaveStringToFile(Out, *FPaths::Combine(OutDir, FileName));
	}
}
