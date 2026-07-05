// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/**
 * Per-run build timing history, appended to `.gameiq/build-timings.json` after every GameIQBuild /
 * GameIQDocsBuild — so slow stages (e.g. the Assets tier's full synchronous asset loads) can be
 * compared across rebuilds later instead of only read live off one run's log.
 * `inline` so both build commandlets (merged into one unity build) can include this without
 * ODR/redefinition conflicts, matching GameIQJson.h's convention.
 */
namespace GameIQ
{
	struct FStageTiming
	{
		FString Name;
		double Seconds = 0.0;
	};

	/** Keep only the most recent runs so the file doesn't grow unbounded. */
	inline constexpr int32 MaxTimingHistory = 20;

	inline FString BuildTimingsFilePath()
	{
		return FPaths::Combine(FPaths::ProjectDir(), TEXT(".gameiq"), TEXT("build-timings.json"));
	}

	/** Append one run's timing record, keeping only the most recent MaxTimingHistory entries. */
	inline void AppendBuildTimingRecord(const FString& RunType, double TotalSeconds, const TArray<FStageTiming>& Stages)
	{
		const FString FilePath = BuildTimingsFilePath();

		TArray<TSharedPtr<FJsonValue>> History;
		FString Existing;
		if (FFileHelper::LoadFileToString(Existing, *FilePath))
		{
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Existing);
			FJsonSerializer::Deserialize(Reader, History);
		}

		TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
		Record->SetStringField(TEXT("timestampIso"), FDateTime::UtcNow().ToIso8601());
		Record->SetStringField(TEXT("runType"), RunType);
		Record->SetNumberField(TEXT("totalSeconds"), TotalSeconds);

		TArray<TSharedPtr<FJsonValue>> StagesJson;
		for (const FStageTiming& Stage : Stages)
		{
			TSharedRef<FJsonObject> S = MakeShared<FJsonObject>();
			S->SetStringField(TEXT("name"), Stage.Name);
			S->SetNumberField(TEXT("seconds"), Stage.Seconds);
			StagesJson.Add(MakeShared<FJsonValueObject>(S));
		}
		Record->SetArrayField(TEXT("stages"), StagesJson);

		History.Add(MakeShared<FJsonValueObject>(Record));
		while (History.Num() > MaxTimingHistory)
		{
			History.RemoveAt(0);
		}

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(History, Writer);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		FFileHelper::SaveStringToFile(Out, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	// ---- build result marker -------------------------------------------------------------------
	// UnrealEditor-Cmd's process exit code is the engine's logged-error count (benign content
	// warnings and an intentional store self-heal both inflate it), NOT the commandlet's own
	// result — so anything launching a build as a subprocess must judge the outcome by an
	// artifact the commandlet wrote, never the exit code. The build commandlets write this
	// marker as their last act; FGameIQBuildRunner reads it back.

	struct FBuildResult
	{
		FString RunType;
		bool bSuccess = false;
		double TotalSeconds = 0.0;
		FDateTime TimestampUtc;
		int64 Entities = 0;
		int64 Edges = 0;
		int64 Chunks = 0;
	};

	inline FString BuildResultFilePath()
	{
		return FPaths::Combine(FPaths::ProjectDir(), TEXT(".gameiq"), TEXT("last-build.json"));
	}

	inline void WriteBuildResult(const FBuildResult& Result)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("runType"), Result.RunType);
		Root->SetBoolField(TEXT("success"), Result.bSuccess);
		Root->SetNumberField(TEXT("totalSeconds"), Result.TotalSeconds);
		Root->SetStringField(TEXT("timestampIso"), FDateTime::UtcNow().ToIso8601());
		Root->SetNumberField(TEXT("entities"), (double)Result.Entities);
		Root->SetNumberField(TEXT("edges"), (double)Result.Edges);
		Root->SetNumberField(TEXT("chunks"), (double)Result.Chunks);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);
		const FString FilePath = BuildResultFilePath();
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);
		FFileHelper::SaveStringToFile(Out, *FilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/** Read the last build's result marker. False on missing/unparseable file. */
	inline bool ReadBuildResult(FBuildResult& OutResult)
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *BuildResultFilePath())) { return false; }
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) { return false; }

		Root->TryGetStringField(TEXT("runType"), OutResult.RunType);
		Root->TryGetBoolField(TEXT("success"), OutResult.bSuccess);
		Root->TryGetNumberField(TEXT("totalSeconds"), OutResult.TotalSeconds);
		double N = 0.0;
		if (Root->TryGetNumberField(TEXT("entities"), N)) { OutResult.Entities = (int64)N; }
		if (Root->TryGetNumberField(TEXT("edges"), N)) { OutResult.Edges = (int64)N; }
		if (Root->TryGetNumberField(TEXT("chunks"), N)) { OutResult.Chunks = (int64)N; }
		FString Stamp;
		if (!Root->TryGetStringField(TEXT("timestampIso"), Stamp) || !FDateTime::ParseIso8601(*Stamp, OutResult.TimestampUtc))
		{
			return false;
		}
		return true;
	}
}
