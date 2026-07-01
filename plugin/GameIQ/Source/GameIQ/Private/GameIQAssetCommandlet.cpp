// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQAssetCommandlet.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "Engine/Level.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "GameIQJson.h"
#include "HAL/FileManager.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQAssets, Log, All);

namespace
{
	const TCHAR* AssetsProducer = TEXT("gameiq-ue-assets@0.1.0");
	constexpr int32 MaxActorEntitiesPerLevel = 500; // list this many actors; always aggregate counts

	/** Stable Game IQ id for a loaded asset, from its package name: asset:/Game/Path/Name. */
	FString AssetIdOf(const UObject* Obj)
	{
		return Obj ? FString::Printf(TEXT("asset:%s"), *Obj->GetOutermost()->GetName()) : FString();
	}

	bool IsProjectObject(const UObject* Obj)
	{
		return Obj && Obj->GetOutermost()->GetName().StartsWith(TEXT("/Game"));
	}

	// Accumulators handed to each recipe.
	struct FOut
	{
		TArray<TSharedPtr<FJsonValue>>& Entities;
		TArray<TSharedPtr<FJsonValue>>& Edges;
		TArray<TSharedPtr<FJsonValue>>& Chunks;
	};

	void AddSummary(FOut& O, const FString& AssetId, const FString& Text)
	{
		O.Chunks.Add(MakeShared<FJsonValueObject>(
			GameIQ::MakeChunk(FString::Printf(TEXT("%s#summary"), *AssetId), AssetId, TEXT("recipe-summary"), Text)));
	}

	void RecipeStaticMesh(FOut& O, const UStaticMesh* Mesh, const FString& Id, const FString& Name)
	{
		const int32 LODs = Mesh->GetNumLODs();
		const int32 Tris = LODs > 0 ? Mesh->GetNumTriangles(0) : 0;
		const TArray<FStaticMaterial>& Mats = Mesh->GetStaticMaterials();
		TArray<FString> MatNames;
		for (const FStaticMaterial& M : Mats)
		{
			if (M.MaterialInterface)
			{
				MatNames.Add(M.MaterialInterface->GetName());
				O.Edges.Add(MakeShared<FJsonValueObject>(
					GameIQ::MakeEdge(Id, AssetIdOf(M.MaterialInterface), TEXT("uses-material"))));
			}
		}
		AddSummary(O, Id, FString::Printf(TEXT("%s (StaticMesh)\nLODs: %d, Triangles(LOD0): %d, Material slots: %d\nMaterials: %s"),
			*Name, LODs, Tris, Mats.Num(), *FString::Join(MatNames, TEXT(", "))));
	}

	void RecipeSkeletalMesh(FOut& O, const USkeletalMesh* Mesh, const FString& Id, const FString& Name)
	{
		const USkeleton* Skel = Mesh->GetSkeleton();
		const TArray<FSkeletalMaterial>& Mats = Mesh->GetMaterials();
		TArray<FString> MatNames;
		for (const FSkeletalMaterial& M : Mats)
		{
			if (M.MaterialInterface)
			{
				MatNames.Add(M.MaterialInterface->GetName());
				O.Edges.Add(MakeShared<FJsonValueObject>(
					GameIQ::MakeEdge(Id, AssetIdOf(M.MaterialInterface), TEXT("uses-material"))));
			}
		}
		if (Skel)
		{
			O.Edges.Add(MakeShared<FJsonValueObject>(
				GameIQ::MakeEdge(Id, AssetIdOf(Skel), TEXT("uses-skeleton"))));
		}
		AddSummary(O, Id, FString::Printf(TEXT("%s (SkeletalMesh)\nSkeleton: %s, LODs: %d, Material slots: %d\nMaterials: %s"),
			*Name, Skel ? *Skel->GetName() : TEXT("none"), Mesh->GetLODNum(), Mats.Num(), *FString::Join(MatNames, TEXT(", "))));
	}

	void RecipeTexture(FOut& O, const UTexture2D* Tex, const FString& Id, const FString& Name)
	{
		AddSummary(O, Id, FString::Printf(TEXT("%s (Texture2D)\n%dx%d"), *Name, Tex->GetSizeX(), Tex->GetSizeY()));
	}

	void RecipeSkeleton(FOut& O, const USkeleton* Skel, const FString& Id, const FString& Name)
	{
		AddSummary(O, Id, FString::Printf(TEXT("%s (Skeleton)\nBones: %d"), *Name, Skel->GetReferenceSkeleton().GetNum()));
	}

	void RecipeDataTable(FOut& O, const UDataTable* Table, const FString& Id, const FString& Name)
	{
		const UScriptStruct* RowStruct = Table->GetRowStruct();
		const TArray<FName> RowNames = Table->GetRowNames();
		AddSummary(O, Id, FString::Printf(TEXT("%s (DataTable)\nRow struct: %s, Rows: %d"),
			*Name, RowStruct ? *RowStruct->GetName() : TEXT("none"), RowNames.Num()));
	}

	void RecipeMaterialInstance(FOut& O, const UMaterialInstance* MI, const FString& Id, const FString& Name)
	{
		if (MI->Parent)
		{
			O.Edges.Add(MakeShared<FJsonValueObject>(
				GameIQ::MakeEdge(Id, AssetIdOf(MI->Parent), TEXT("references"), TEXT("parent"))));
		}
		AddSummary(O, Id, FString::Printf(TEXT("%s (MaterialInstance)\nParent: %s"),
			*Name, MI->Parent ? *MI->Parent->GetName() : TEXT("none")));
	}

	void RecipeLevel(FOut& O, UWorld* World, const FString& Id, const FString& Name)
	{
		ULevel* Level = World->PersistentLevel;
		if (!Level) { return; }

		TMap<FString, int32> ClassCounts;
		int32 Listed = 0;
		for (const AActor* Actor : Level->Actors)
		{
			if (!Actor) { continue; }
			const FString ClassName = Actor->GetClass()->GetName();
			ClassCounts.FindOrAdd(ClassName)++;

			// Link the level to the class it places: BP asset, or native C++ class.
			if (const UClass* Cls = Actor->GetClass())
			{
				if (UObject* GenBy = Cls->ClassGeneratedBy)
				{
					O.Edges.Add(MakeShared<FJsonValueObject>(
						GameIQ::MakeEdge(Id, AssetIdOf(GenBy), TEXT("references"), TEXT("places"))));
				}
				else
				{
					O.Edges.Add(MakeShared<FJsonValueObject>(
						GameIQ::MakeEdge(Id, FString::Printf(TEXT("cpp:%s"), *ClassName), TEXT("references"), TEXT("places"))));
				}
			}

			if (Listed < MaxActorEntitiesPerLevel)
			{
				const FString ActorName = Actor->GetName();
				const FString ActorId = FString::Printf(TEXT("%s::actor::%s"), *Id, *ActorName);
				TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
				D->SetStringField(TEXT("class"), ClassName);
				D->SetStringField(TEXT("location"), Actor->GetActorLocation().ToString());
				O.Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
					ActorId, TEXT("level-actor"), ActorName, Name, TEXT("asset"), Id,
					FString::Printf(TEXT("%s (%s)"), *ActorName, *ClassName), D)));
				O.Edges.Add(MakeShared<FJsonValueObject>(
					GameIQ::MakeEdge(ActorId, Id, TEXT("placed-in-level"))));
				++Listed;
			}
		}

		ClassCounts.ValueSort([](int32 A, int32 B) { return A > B; });
		TArray<FString> CountLines;
		for (const TPair<FString, int32>& P : ClassCounts)
		{
			CountLines.Add(FString::Printf(TEXT("%s x%d"), *P.Key, P.Value));
		}
		AddSummary(O, Id, FString::Printf(TEXT("%s (Level)\nActors: %d (%d classes)\n%s"),
			*Name, Level->Actors.Num(), ClassCounts.Num(), *FString::Join(CountLines, TEXT(", "))));
	}
}

UGameIQAssetsCommandlet::UGameIQAssetsCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = true;
}

int32 UGameIQAssetsCommandlet::Main(const FString& Params)
{
	FString OutDir;
	if (!FParse::Value(*Params, TEXT("out="), OutDir))
	{
		OutDir = FPaths::Combine(FPaths::ProjectDir(), TEXT(".gameiq"), TEXT("extract"));
	}
	IFileManager::Get().MakeDirectory(*OutDir, /*Tree=*/true);

	FAssetRegistryModule& ARM =
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AR = ARM.Get();
	AR.SearchAllAssets(/*bSynchronous=*/true);

	FARFilter Filter;
	Filter.PackagePaths.Add(FName(TEXT("/Game")));
	Filter.bRecursivePaths = true;
	TArray<FAssetData> Assets;
	AR.GetAssets(Filter, Assets);

	TArray<TSharedPtr<FJsonValue>> Entities;
	TArray<TSharedPtr<FJsonValue>> Edges;
	TArray<TSharedPtr<FJsonValue>> Chunks;
	FOut O{ Entities, Edges, Chunks };

	int32 Considered = 0;
	for (const FAssetData& Data : Assets)
	{
		// Blueprints are handled by the GameIQBlueprints (Tier 2) commandlet.
		if (Data.AssetClassPath.GetAssetName().ToString().EndsWith(TEXT("Blueprint"))) { continue; }

		UObject* Asset = Data.GetAsset();
		if (!Asset || !IsProjectObject(Asset)) { continue; }
		++Considered;

		const FString Id = AssetIdOf(Asset);
		const FString Name = Data.AssetName.ToString();

		if (const UStaticMesh* SM = Cast<UStaticMesh>(Asset)) { RecipeStaticMesh(O, SM, Id, Name); }
		else if (const USkeletalMesh* SK = Cast<USkeletalMesh>(Asset)) { RecipeSkeletalMesh(O, SK, Id, Name); }
		else if (const UTexture2D* Tex = Cast<UTexture2D>(Asset)) { RecipeTexture(O, Tex, Id, Name); }
		else if (const USkeleton* Skel = Cast<USkeleton>(Asset)) { RecipeSkeleton(O, Skel, Id, Name); }
		else if (const UDataTable* DT = Cast<UDataTable>(Asset)) { RecipeDataTable(O, DT, Id, Name); }
		else if (const UMaterialInstance* MI = Cast<UMaterialInstance>(Asset)) { RecipeMaterialInstance(O, MI, Id, Name); }
		else if (UWorld* World = Cast<UWorld>(Asset)) { RecipeLevel(O, World, Id, Name); }
		else
		{
			// Generic fallback — no asset is invisible (design §5.1).
			AddSummary(O, Id, FString::Printf(TEXT("%s (%s)"), *Name, *Data.AssetClassPath.GetAssetName().ToString()));
		}
	}

	if (!GameIQ::WriteOutput(OutDir, TEXT("assets.json"), AssetsProducer, Entities, Edges, Chunks))
	{
		UE_LOG(LogGameIQAssets, Error, TEXT("Failed to write assets.json to %s"), *OutDir);
		return 1;
	}

	UE_LOG(LogGameIQAssets, Display,
		TEXT("Game IQ: summarized %d assets — %d entities, %d edges, %d chunks to %s"),
		Considered, Entities.Num(), Edges.Num(), Chunks.Num(), *FPaths::Combine(OutDir, TEXT("assets.json")));
	return 0;
}
