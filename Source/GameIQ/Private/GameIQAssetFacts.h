// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "GameIQJson.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"

/**
 * Tier 0 rows and per-asset size/cook facts (issues #33, #35), shared by the registry export
 * commandlet, the editor save hook and the editor's startup catch-up scan so all three write the
 * same entity shape. Everything comes from the Asset Registry and the file system: no UObject load.
 */
namespace GameIQAssetFacts
{
	/** Uncooked on-disk size of a package: .uasset/.umap plus its .uexp/.ubulk/.uptnl sidecars.
	 *  -1 if the package has no file (in-memory only, or not a /Game package). */
	inline int64 PackageDiskBytes(const FString& PackageName)
	{
		FString Filename;
		if (!FPackageName::DoesPackageExist(PackageName, &Filename)) { return -1; }
		IFileManager& FM = IFileManager::Get();
		int64 Total = FM.FileSize(*Filename);
		if (Total < 0) { return -1; }
		const FString Base = FPaths::ChangeExtension(Filename, TEXT(""));
		for (const TCHAR* Ext : { TEXT(".uexp"), TEXT(".ubulk"), TEXT(".uptnl") })
		{
			const int64 Size = FM.FileSize(*(Base + Ext));
			if (Size > 0) { Total += Size; }
		}
		return Total;
	}

	/** Registry tags worth keeping as typed facts, and the fact name each becomes. Only tags the
	 *  asset class actually publishes appear, so one table serves every class. */
	inline const TArray<TPair<FName, FString>>& FactTags()
	{
		static const TArray<TPair<FName, FString>> Tags = {
			// Textures
			{ TEXT("Format"), TEXT("pixelFormat") }, { TEXT("CompressionSettings"), TEXT("compression") },
			{ TEXT("LODGroup"), TEXT("lodGroup") }, { TEXT("SRGB"), TEXT("srgb") },
			{ TEXT("VirtualTextureStreaming"), TEXT("virtualTexture") }, { TEXT("NeverStream"), TEXT("neverStream") },
			{ TEXT("HasAlphaChannel"), TEXT("hasAlpha") }, { TEXT("MaxTextureSize"), TEXT("maxTextureSize") },
			// Meshes
			{ TEXT("Triangles"), TEXT("triangles") }, { TEXT("Vertices"), TEXT("vertices") }, { TEXT("LODs"), TEXT("lods") },
			{ TEXT("NaniteEnabled"), TEXT("nanite") }, { TEXT("Materials"), TEXT("materialSlots") },
			{ TEXT("Bones"), TEXT("bones") }, { TEXT("MorphTargets"), TEXT("morphTargets") },
			{ TEXT("EstTotalCompressedSize"), TEXT("estCompressedBytes") },
			// Audio
			{ TEXT("Duration"), TEXT("durationSeconds") }, { TEXT("SampleRate"), TEXT("sampleRate") },
			{ TEXT("NumChannels"), TEXT("channels") }, { TEXT("CompressionQuality"), TEXT("compressionQuality") },
			{ TEXT("SoundAssetCompressionType"), TEXT("audioCompression") },
			// Animation
			{ TEXT("SequenceLength"), TEXT("lengthSeconds") }, { TEXT("NumberOfFrames"), TEXT("frames") },
			{ TEXT("SourceFrameRate"), TEXT("frameRate") },
		};
		return Tags;
	}

	/** Write `diskBytes` (top level, for SQL ordering) and a `facts` object (texture size/format/
	 *  compression/VT, mesh triangles/LODs, audio duration/sample rate, animation length) into an
	 *  asset entity's detail. */
	inline void FillFacts(const FAssetData& Data, const TSharedRef<FJsonObject>& Detail)
	{
		const int64 Bytes = PackageDiskBytes(Data.PackageName.ToString());
		if (Bytes >= 0) { Detail->SetNumberField(TEXT("diskBytes"), static_cast<double>(Bytes)); }

		TSharedRef<FJsonObject> Facts = MakeShared<FJsonObject>();
		FString Dims;
		if (Data.GetTagValue(TEXT("Dimensions"), Dims))
		{
			TArray<FString> Parts;
			Dims.ParseIntoArray(Parts, TEXT("x"));
			if (Parts.Num() >= 2)
			{
				Facts->SetNumberField(TEXT("width"), FCString::Atoi(*Parts[0]));
				Facts->SetNumberField(TEXT("height"), FCString::Atoi(*Parts[1]));
			}
		}
		for (const TPair<FName, FString>& Tag : FactTags())
		{
			FString Value;
			if (!Data.GetTagValue(Tag.Key, Value) || Value.IsEmpty()) { continue; }
			if (Value.IsNumeric()) { Facts->SetNumberField(Tag.Value, FCString::Atod(*Value)); }
			else if (Value.Equals(TEXT("True"), ESearchCase::IgnoreCase)) { Facts->SetBoolField(Tag.Value, true); }
			else if (Value.Equals(TEXT("False"), ESearchCase::IgnoreCase)) { Facts->SetBoolField(Tag.Value, false); }
			else { Facts->SetStringField(Tag.Value, Value); }
		}
		if (Facts->Values.Num() > 0) { Detail->SetObjectField(TEXT("facts"), Facts); }
	}

	/** Prefix-less native parent class name from a Blueprint's registry tags, if present. */
	inline bool TryGetNativeParentClassName(const FAssetData& Data, FString& OutClassName)
	{
		FString Raw;
		if (!Data.GetTagValue(FName(TEXT("NativeParentClass")), Raw) &&
			!Data.GetTagValue(FName(TEXT("ParentClass")), Raw))
		{
			return false;
		}
		// "/Script/Engine.Character" or "Class'/Script/Engine.Character'"
		Raw.RemoveFromStart(TEXT("Class'"));
		Raw.RemoveFromEnd(TEXT("'"));
		int32 Dot = INDEX_NONE;
		if (Raw.FindLastChar(TEXT('.'), Dot))
		{
			OutClassName = Raw.RightChop(Dot + 1);
			return !OutClassName.IsEmpty();
		}
		return false;
	}

	/** Tier 0 rows for one /Game asset: the identity entity (with size facts), `depends-on` edges to
	 *  the project packages it depends on, and a Blueprint's `inherits` edge to its native parent. */
	inline void AppendTier0(IAssetRegistry& AR, const FAssetData& Data,
		TArray<TSharedPtr<FJsonValue>>& Entities, TArray<TSharedPtr<FJsonValue>>& Edges)
	{
		const FString Package = Data.PackageName.ToString();
		const FString Id = FString::Printf(TEXT("asset:%s"), *Package);
		const FString ClassName = Data.AssetClassPath.GetAssetName().ToString();
		const bool bIsBlueprint = ClassName.EndsWith(TEXT("Blueprint"));

		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("assetClass"), ClassName);
		Detail->SetStringField(TEXT("objectPath"), Data.GetObjectPathString());
		FillFacts(Data, Detail);

		Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
			Id, bIsBlueprint ? TEXT("blueprint") : TEXT("asset"), Data.AssetName.ToString(), Package,
			TEXT("asset"), FString(), FString(), Detail)));

		TArray<FName> Deps;
		AR.GetDependencies(Data.PackageName, Deps, UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName& Dep : Deps)
		{
			const FString DepName = Dep.ToString();
			if (DepName.StartsWith(TEXT("/Game")) && DepName != Package)
			{
				Edges.Add(MakeShared<FJsonValueObject>(
					GameIQ::MakeEdge(Id, FString::Printf(TEXT("asset:%s"), *DepName), TEXT("depends-on"))));
			}
		}

		FString ParentClassName;
		if (bIsBlueprint && TryGetNativeParentClassName(Data, ParentClassName))
		{
			Edges.Add(MakeShared<FJsonValueObject>(
				GameIQ::MakeEdge(Id, FString::Printf(TEXT("cpp:%s"), *ParentClassName), TEXT("inherits"))));
		}
	}
}
