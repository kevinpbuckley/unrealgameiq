// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/FileManager.h"
#include "IO/IoHash.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

/**
 * Incremental-rebuild support: skip unchanged assets on a rebuild without losing completeness.
 *
 * The cache stores ONLY per-asset signatures (plus the dbGeneration it was built against). The
 * previously-extracted rows for unchanged assets are NOT duplicated here — they already live in
 * the SQLite index, and the extractor emits a producer-scoped *delta* (`replaces` = changed +
 * removed ids) that the ingest applies on top. (v1 of this cache carried every asset's full
 * entities/edges/chunks forward through a JSON side-car that grew larger than the extract itself
 * — 60 MB of pure duplication per run on a mid-size project.)
 *
 * `inline` so multiple commandlet translation units can include this without ODR/redefinition
 * conflicts, matching GameIQJson.h's convention.
 */
namespace GameIQHash
{
	inline constexpr int32 CacheSchemaVersion = 2; // v2: sig-only + dbGeneration (v1 carried full rows)

	/** Stable Game IQ id for an asset from its package name, with no load required — matches
	 *  the `asset:%s` format used by the loaded-object AssetIdOf() and GameIQSaveHook.cpp. */
	inline FString AssetIdOfPackage(FName PackageName)
	{
		return FString::Printf(TEXT("asset:%s"), *PackageName.ToString());
	}

	/**
	 * Cheap, reliable per-asset change signal — the AssetRegistry's own content-addressed
	 * package hash (FAssetPackageData::GetPackageSavedHash, UE 5.8+), fetched metadata-only via
	 * IAssetRegistry::GetAssetPackageDataCopy (no UObject load). Returns an empty string if
	 * unknown (package data missing, or a zero hash) — callers must treat an empty signature as
	 * "always changed" so an asset Game IQ can't confidently fingerprint is never wrongly skipped.
	 */
	inline FString ComputeChangeSignature(IAssetRegistry& AR, const FAssetData& Data)
	{
		const TOptional<FAssetPackageData> PackageData = AR.GetAssetPackageDataCopy(Data.PackageName);
		if (!PackageData.IsSet()) { return FString(); }
		const FIoHash Hash = PackageData->GetPackageSavedHash();
		return Hash.IsZero() ? FString() : LexToString(Hash);
	}

	/**
	 * Load a producer's signature cache (e.g. `.gameiq/extract/asset-hashes.json`): id → sig.
	 * Returns an empty map on a missing file, a parse failure, a schema-version mismatch, or a
	 * dbGeneration mismatch (the index DB was rebuilt since this cache was written, so "unchanged"
	 * assets would be carried forward into a DB that doesn't hold their rows) — any of which means
	 * "no usable cache", so callers naturally fall back to a full extraction.
	 */
	inline TMap<FString, FString> LoadSignatureCache(const FString& FilePath, const FString& CurrentDbGeneration)
	{
		TMap<FString, FString> Result;

		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FilePath)) { return Result; }

		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) { return Result; }

		int32 Version = 0;
		if (!Root->TryGetNumberField(TEXT("schemaVersion"), Version) || Version != CacheSchemaVersion)
		{
			return Result;
		}

		FString Generation;
		Root->TryGetStringField(TEXT("dbGeneration"), Generation);
		if (CurrentDbGeneration.IsEmpty() || Generation != CurrentDbGeneration)
		{
			return Result;
		}

		const TSharedPtr<FJsonObject>* SigsObj = nullptr;
		if (!Root->TryGetObjectField(TEXT("sigs"), SigsObj) || !SigsObj || !SigsObj->IsValid())
		{
			return Result;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*SigsObj)->Values)
		{
			FString Sig;
			if (Pair.Value.IsValid() && Pair.Value->TryGetString(Sig) && !Sig.IsEmpty())
			{
				Result.Add(Pair.Key, MoveTemp(Sig));
			}
		}
		return Result;
	}

	/** Save a producer's signature cache. Writes to a temp file then renames over the destination
	 *  so a process killed mid-write can never leave a half-written cache that would cause false
	 *  skips on the next run. */
	inline bool SaveSignatureCache(const FString& FilePath, const TMap<FString, FString>& Sigs, const FString& DbGeneration)
	{
		const TSharedRef<FJsonObject> SigsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, FString>& Pair : Sigs)
		{
			SigsObj->SetStringField(Pair.Key, Pair.Value);
		}

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schemaVersion"), CacheSchemaVersion);
		Root->SetStringField(TEXT("dbGeneration"), DbGeneration);
		Root->SetObjectField(TEXT("sigs"), SigsObj);

		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Root, Writer);

		const FString TempPath = FilePath + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Out, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return false;
		}
		return IFileManager::Get().Move(*FilePath, *TempPath, /*bReplace=*/true);
	}
}
