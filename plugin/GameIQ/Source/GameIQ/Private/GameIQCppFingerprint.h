// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameIQFileWalk.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

/**
 * C++ source staleness fingerprint (auto code reindex). The fingerprint is the MD5 of every
 * indexed source file's project-relative path, size, and mtime — over the same GameIQWalk file
 * set the code extractor scans, so "fingerprint changed" == "the code stage would see different
 * input". GameIQCode stamps it next to cpp.json after each extraction; the editor save-hook
 * subsystem recomputes it on startup (and after a Live Coding patch) and kicks a C++-only
 * reindex on mismatch. Inline so commandlet + subsystem TUs share it without ODR conflicts
 * (GameIQFileWalk.h pattern).
 */
namespace GameIQCppFingerprint
{
	inline FString FingerprintPath(const FString& Root)
	{
		return FPaths::Combine(Root, TEXT(".gameiq"), TEXT("extract"), TEXT("cpp.fingerprint"));
	}

	inline FString CppJsonPath(const FString& Root)
	{
		return FPaths::Combine(Root, TEXT(".gameiq"), TEXT("extract"), TEXT("cpp.json"));
	}

	/** Hash of (relpath|size|mtime) over every .h/.cpp the code extractor would walk. Stat-only —
	 *  no file contents are read — so it stays cheap enough to run at editor startup. */
	inline FString Compute(const FString& Root)
	{
		TArray<FString> Files = GameIQWalk::WalkFiles(Root, { TEXT(".h"), TEXT(".cpp") });
		Files.Sort();

		IFileManager& FM = IFileManager::Get();
		FString Manifest;
		for (const FString& File : Files)
		{
			const FFileStatData Stat = FM.GetStatData(*File);
			Manifest += FString::Printf(TEXT("%s|%lld|%lld\n"),
				*GameIQWalk::RelPath(Root, File),
				Stat.FileSize,
				Stat.ModificationTime.GetTicks());
		}
		return FMD5::HashAnsiString(*Manifest);
	}

	inline FString ReadStored(const FString& Root)
	{
		FString Stored;
		FFileHelper::LoadFileToString(Stored, *FingerprintPath(Root));
		return Stored.TrimStartAndEnd();
	}

	inline void Write(const FString& Root, const FString& Fingerprint)
	{
		FFileHelper::SaveStringToFile(Fingerprint, *FingerprintPath(Root),
			FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}
