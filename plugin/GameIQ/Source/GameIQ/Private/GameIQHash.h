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
 * Incremental-rebuild support (design: skip unchanged assets on a full rebuild without losing
 * completeness). `inline` so multiple commandlet translation units can include this without
 * ODR/redefinition conflicts, matching GameIQJson.h's convention.
 */
namespace GameIQHash
{
	inline constexpr int32 CacheSchemaVersion = 1;

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

	/** One asset's cached signature + the exact entities/edges/chunks its extraction produced
	 *  last run, carried forward verbatim on a cache hit so output completeness never regresses. */
	struct FCachedAssetRow
	{
		FString Signature;
		TArray<TSharedPtr<FJsonValue>> Entities;
		TArray<TSharedPtr<FJsonValue>> Edges;
		TArray<TSharedPtr<FJsonValue>> Chunks;
	};

	/** Load a producer's asset-hash side-car (e.g. `.gameiq/extract/asset-hashes.json`). Returns
	 *  an empty map on a missing file, a parse failure, or a schema-version mismatch — any of
	 *  which means "no usable cache", so callers naturally fall back to a full extraction. */
	inline TMap<FString, FCachedAssetRow> LoadAssetHashCache(const FString& FilePath)
	{
		TMap<FString, FCachedAssetRow> Result;

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

		const TSharedPtr<FJsonObject>* AssetsObj = nullptr;
		if (!Root->TryGetObjectField(TEXT("assets"), AssetsObj) || !AssetsObj || !AssetsObj->IsValid())
		{
			return Result;
		}

		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*AssetsObj)->Values)
		{
			const TSharedPtr<FJsonObject> EntryObj = Pair.Value.IsValid() ? Pair.Value->AsObject() : nullptr;
			if (!EntryObj.IsValid()) { continue; }

			FCachedAssetRow Row;
			EntryObj->TryGetStringField(TEXT("sig"), Row.Signature);
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (EntryObj->TryGetArrayField(TEXT("entities"), Arr)) { Row.Entities = *Arr; }
			if (EntryObj->TryGetArrayField(TEXT("edges"), Arr)) { Row.Edges = *Arr; }
			if (EntryObj->TryGetArrayField(TEXT("chunks"), Arr)) { Row.Chunks = *Arr; }
			Result.Add(Pair.Key, MoveTemp(Row));
		}
		return Result;
	}

	/** Save a producer's asset-hash side-car. Writes to a temp file then renames over the
	 *  destination so a process killed mid-write can never leave a half-written cache that would
	 *  cause false skips on the next run. */
	inline bool SaveAssetHashCache(const FString& FilePath, const TMap<FString, FCachedAssetRow>& Cache)
	{
		const TSharedRef<FJsonObject> AssetsObj = MakeShared<FJsonObject>();
		for (const TPair<FString, FCachedAssetRow>& Pair : Cache)
		{
			const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
			Entry->SetStringField(TEXT("sig"), Pair.Value.Signature);
			Entry->SetArrayField(TEXT("entities"), Pair.Value.Entities);
			Entry->SetArrayField(TEXT("edges"), Pair.Value.Edges);
			Entry->SetArrayField(TEXT("chunks"), Pair.Value.Chunks);
			AssetsObj->SetObjectField(Pair.Key, Entry);
		}

		const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetNumberField(TEXT("schemaVersion"), CacheSchemaVersion);
		Root->SetObjectField(TEXT("assets"), AssetsObj);

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
