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
}
