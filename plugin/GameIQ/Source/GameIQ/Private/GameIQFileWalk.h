// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * Editor-less file walking for the C++ ports of the config/code extractors (design §5.1),
 * mirroring packages/core/src/util/walk.ts + config.ts so the ids/coverage match. Inline so
 * multiple commandlet TUs (merged in unity builds) can share it without ODR conflicts.
 */
namespace GameIQWalk
{
	/** Project-relative path with forward slashes, e.g. "Source/Foo/Bar.h". */
	inline FString RelPath(const FString& Root, const FString& Full)
	{
		FString Rel = Full;
		FPaths::MakePathRelativeTo(Rel, *(Root / TEXT("")));
		return Rel.Replace(TEXT("\\"), TEXT("/"));
	}

	/** gameiq.config.json `exclude` list + the always-excluded GameIQ plugin (config.ts parity). */
	inline TArray<FString> LoadExcludes(const FString& Root)
	{
		TArray<FString> Excludes;
		Excludes.Add(TEXT("Plugins/GameIQ")); // ALWAYS_EXCLUDE

		FString Json;
		if (FFileHelper::LoadFileToString(Json, *FPaths::Combine(Root, TEXT("gameiq.config.json"))))
		{
			TSharedPtr<FJsonObject> Obj;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid() && Obj->TryGetArrayField(TEXT("exclude"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					FString S;
					if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
					{
						Excludes.Add(S.Replace(TEXT("\\"), TEXT("/")).TrimEnd());
					}
				}
			}
		}
		return Excludes;
	}

	inline bool IsExcluded(const FString& Name, const FString& Rel, const TArray<FString>& Excludes)
	{
		for (const FString& E : Excludes)
		{
			if (Name == E || Rel == E || Rel.StartsWith(E + TEXT("/"))) { return true; }
		}
		return false;
	}

	inline void Collect(const FString& Root, const FString& Dir, const TArray<FString>& LowerExts,
		const TArray<FString>& Excludes, TArray<FString>& Out)
	{
		static const TArray<FString> Skip = {
			TEXT("Intermediate"), TEXT("Binaries"), TEXT("Saved"), TEXT("DerivedDataCache"),
			TEXT(".git"), TEXT("node_modules"), TEXT(".gameiq") };

		IFileManager& FM = IFileManager::Get();

		TArray<FString> Files;
		FM.FindFiles(Files, *(Dir / TEXT("*.*")), /*Files=*/true, /*Directories=*/false);
		for (const FString& F : Files)
		{
			const FString Ext = FString(TEXT(".")) + FPaths::GetExtension(F).ToLower();
			if (LowerExts.Contains(Ext)) { Out.Add(Dir / F); }
		}

		TArray<FString> Dirs;
		FM.FindFiles(Dirs, *(Dir / TEXT("*")), /*Files=*/false, /*Directories=*/true);
		for (const FString& D : Dirs)
		{
			if (Skip.Contains(D)) { continue; }
			const FString Full = Dir / D;
			if (IsExcluded(D, RelPath(Root, Full), Excludes)) { continue; }
			Collect(Root, Full, LowerExts, Excludes, Out);
		}
	}

	/** Recursively list files under `Root` whose extension is in `LowerExts` (".h", ".ini", …). */
	inline TArray<FString> WalkFiles(const FString& Root, const TArray<FString>& LowerExts)
	{
		TArray<FString> Out;
		Collect(Root, Root, LowerExts, LoadExcludes(Root), Out);
		return Out;
	}
}
