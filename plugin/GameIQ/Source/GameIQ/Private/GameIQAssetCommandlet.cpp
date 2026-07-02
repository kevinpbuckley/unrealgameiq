// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQAssetCommandlet.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/ARFilter.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "EnhancedActionKeyMapping.h"
#include "Engine/Light.h"
#include "Engine/StaticMeshActor.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Dom/JsonObject.h"
#include "Engine/DataTable.h"
#include "Engine/EngineTypes.h"
#include "Engine/Level.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture.h"
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
#include "UObject/Field.h"
#include "UObject/Package.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/UnrealType.h"

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

	/**
	 * Generic fallback for any asset without a bespoke recipe (design §5.1 "reflection export
	 * is the foundation"). Reflects the asset's top-level UPROPERTYs into a bounded property
	 * bag via ExportTextItem_Direct, and turns object-reference properties into `references`
	 * edges — so Niagara systems, sounds, physics assets, data assets, etc. are never invisible.
	 */
	FString GenericReflectionSummary(FOut& O, const UObject* Asset, const FString& Id)
	{
		TArray<FString> Lines;
		int32 Count = 0;
		for (TFieldIterator<FProperty> It(Asset->GetClass()); It && Count < 40; ++It)
		{
			const FProperty* Prop = *It;
			if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) { continue; }

			if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
			{
				UObject* Ref = ObjProp->GetObjectPropertyValue(ObjProp->ContainerPtrToValuePtr<void>(Asset));
				if (Ref && IsProjectObject(Ref))
				{
					Lines.Add(FString::Printf(TEXT("%s=%s"), *Prop->GetName(), *Ref->GetName()));
					O.Edges.Add(MakeShared<FJsonValueObject>(
						GameIQ::MakeEdge(Id, AssetIdOf(Ref), TEXT("references"), Prop->GetName())));
					++Count;
				}
				continue;
			}

			if (Prop->IsA<FNumericProperty>() || Prop->IsA<FBoolProperty>() || Prop->IsA<FNameProperty>() ||
				Prop->IsA<FStrProperty>() || Prop->IsA<FEnumProperty>() || Prop->IsA<FByteProperty>())
			{
				FString Value;
				Prop->ExportTextItem_Direct(Value, Prop->ContainerPtrToValuePtr<void>(Asset), nullptr,
					const_cast<UObject*>(Asset), PPF_None);
				Value = GameIQ::OneLine(Value);
				if (Value.Len() > 0 && Value.Len() < 60)
				{
					Lines.Add(FString::Printf(TEXT("%s=%s"), *Prop->GetName(), *Value));
					++Count;
				}
			}
		}
		return FString::Join(Lines, TEXT(", "));
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

	void RecipeMaterial(FOut& O, const UMaterialInterface* Mat, const FString& Id, const FString& Name)
	{
		FString Blend = FString::FromInt(static_cast<int32>(Mat->GetBlendMode()));
		if (const UEnum* BlendEnum = StaticEnum<EBlendMode>())
		{
			Blend = BlendEnum->GetNameStringByValue(static_cast<int64>(Mat->GetBlendMode()));
		}

		TArray<UTexture*> Textures;
		Mat->GetUsedTextures(Textures);
		TArray<FString> TexNames;
		for (const UTexture* Tex : Textures)
		{
			if (Tex && IsProjectObject(Tex))
			{
				TexNames.Add(Tex->GetName());
				O.Edges.Add(MakeShared<FJsonValueObject>(
					GameIQ::MakeEdge(Id, AssetIdOf(Tex), TEXT("uses-texture"))));
			}
		}

		FString ParentLine;
		const bool bIsInstance = Mat->IsA<UMaterialInstance>();
		if (const UMaterialInstance* MI = Cast<UMaterialInstance>(Mat))
		{
			if (MI->Parent)
			{
				O.Edges.Add(MakeShared<FJsonValueObject>(
					GameIQ::MakeEdge(Id, AssetIdOf(MI->Parent), TEXT("references"), TEXT("parent"))));
				ParentLine = FString::Printf(TEXT("Parent: %s\n"), *MI->Parent->GetName());
			}
			// Overridden texture parameters are serialized (readable headless, unlike GetUsedTextures).
			for (const FTextureParameterValue& TP : MI->TextureParameterValues)
			{
				if (TP.ParameterValue && IsProjectObject(TP.ParameterValue))
				{
					TexNames.Add(FString::Printf(TEXT("%s=%s"), *TP.ParameterInfo.Name.ToString(), *TP.ParameterValue->GetName()));
					O.Edges.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEdge(
						Id, AssetIdOf(TP.ParameterValue), TEXT("uses-texture"), TP.ParameterInfo.Name.ToString())));
				}
			}
		}

		AddSummary(O, Id, FString::Printf(TEXT("%s (%s)\n%sBlend: %s, TwoSided: %s, Textures: %d\n%s"),
			*Name, bIsInstance ? TEXT("MaterialInstance") : TEXT("Material"), *ParentLine, *Blend,
			Mat->IsTwoSided() ? TEXT("yes") : TEXT("no"), TexNames.Num(), *FString::Join(TexNames, TEXT(", "))));
	}

	/** InputMappingContext — the key→action bindings (Enhanced Input). */
	void RecipeInputMappingContext(FOut& O, const UInputMappingContext* IMC, const FString& Id, const FString& Name)
	{
		TArray<FString> Lines;
		for (const FEnhancedActionKeyMapping& M : IMC->GetMappings())
		{
			const FString ActionName = M.Action ? M.Action->GetName() : TEXT("<none>");
			Lines.Add(FString::Printf(TEXT("%s <- %s"), *ActionName, *M.Key.ToString()));
			if (M.Action)
			{
				O.Edges.Add(MakeShared<FJsonValueObject>(
					GameIQ::MakeEdge(Id, AssetIdOf(M.Action), TEXT("references"), TEXT("maps"))));
			}
		}
		AddSummary(O, Id, FString::Printf(TEXT("%s (InputMappingContext)\nMappings (%d):\n%s"),
			*Name, IMC->GetMappings().Num(), *FString::Join(Lines, TEXT("\n"))));
	}

	/** InputAction — the value type of a mappable action. */
	void RecipeInputAction(FOut& O, const UInputAction* IA, const FString& Id, const FString& Name)
	{
		const UEnum* ValueEnum = StaticEnum<EInputActionValueType>();
		const FString ValueType = ValueEnum
			? ValueEnum->GetNameStringByValue(static_cast<int64>(IA->ValueType))
			: FString::FromInt(static_cast<int32>(IA->ValueType));
		AddSummary(O, Id, FString::Printf(TEXT("%s (InputAction)\nValueType: %s"), *Name, *ValueType));
	}

	/** Type-specific one-line detail for a placed actor (lights, static meshes), for the searchable chunk. */
	FString ActorDetail(FOut& O, const AActor* Actor, const FString& ActorId, TSharedRef<FJsonObject>& OutJson)
	{
		if (const ALight* Light = Cast<ALight>(Actor))
		{
			if (const ULightComponent* LC = Light->GetLightComponent())
			{
				const FColor Color = LC->LightColor;
				const FString Mob = StaticEnum<EComponentMobility::Type>()
					? StaticEnum<EComponentMobility::Type>()->GetNameStringByValue(static_cast<int64>(LC->Mobility))
					: FString();
				OutJson->SetNumberField(TEXT("intensity"), LC->Intensity);
				OutJson->SetStringField(TEXT("color"), FString::Printf(TEXT("%d,%d,%d"), Color.R, Color.G, Color.B));
				OutJson->SetStringField(TEXT("mobility"), Mob);
				return FString::Printf(TEXT("intensity=%.0f color=%d,%d,%d mobility=%s"),
					LC->Intensity, Color.R, Color.G, Color.B, *Mob);
			}
		}
		if (const AStaticMeshActor* SMA = Cast<AStaticMeshActor>(Actor))
		{
			if (const UStaticMeshComponent* SMC = SMA->GetStaticMeshComponent())
			{
				if (const UStaticMesh* Mesh = SMC->GetStaticMesh())
				{
					OutJson->SetStringField(TEXT("mesh"), Mesh->GetName());
					O.Edges.Add(MakeShared<FJsonValueObject>(
						GameIQ::MakeEdge(ActorId, AssetIdOf(Mesh), TEXT("references"), TEXT("mesh"))));
					return FString::Printf(TEXT("mesh=%s"), *Mesh->GetName());
				}
			}
		}
		return FString();
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
				const FString ActorLabel = Actor->GetActorNameOrLabel();
				const FString ActorId = FString::Printf(TEXT("%s::actor::%s"), *Id, *Actor->GetName());
				TSharedRef<FJsonObject> D = MakeShared<FJsonObject>();
				D->SetStringField(TEXT("class"), ClassName);
				D->SetStringField(TEXT("location"), Actor->GetActorLocation().ToString());
				const FString Detail = ActorDetail(O, Actor, ActorId, D);

				O.Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
					ActorId, TEXT("level-actor"), ActorLabel, Name, TEXT("asset"), Id,
					FString::Printf(TEXT("%s (%s)"), *ActorLabel, *ClassName), D)));
				O.Edges.Add(MakeShared<FJsonValueObject>(
					GameIQ::MakeEdge(ActorId, Id, TEXT("placed-in-level"))));
				// Per-actor chunk so placed actors are searchable by class/name ("find the DirectionalLights").
				O.Chunks.Add(MakeShared<FJsonValueObject>(GameIQ::MakeChunk(
					FString::Printf(TEXT("%s#actor"), *ActorId), ActorId, TEXT("recipe-summary"),
					FString::Printf(TEXT("%s (%s) in %s%s"), *ActorLabel, *ClassName, *Name,
						Detail.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" — %s"), *Detail)))));
				++Listed;
			}
		}

		ClassCounts.ValueSort([](int32 A, int32 B) { return A > B; });
		TArray<FString> CountLines;
		for (const TPair<FString, int32>& P : ClassCounts)
		{
			CountLines.Add(FString::Printf(TEXT("%s x%d"), *P.Key, P.Value));
		}
		// Full per-class counts are always reported; per-actor entities are capped (design §12 Q7).
		const FString CapNote = Level->Actors.Num() > MaxActorEntitiesPerLevel
			? FString::Printf(TEXT(" (%d listed as entities, all counted below)"), MaxActorEntitiesPerLevel)
			: FString();
		AddSummary(O, Id, FString::Printf(TEXT("%s (Level)\nActors: %d in %d classes%s\n%s"),
			*Name, Level->Actors.Num(), ClassCounts.Num(), *CapNote, *FString::Join(CountLines, TEXT(", "))));
	}
}

void GameIQAsset::ExtractAsset(
	UObject* Asset,
	TArray<TSharedPtr<FJsonValue>>& Entities,
	TArray<TSharedPtr<FJsonValue>>& Edges,
	TArray<TSharedPtr<FJsonValue>>& Chunks)
{
	if (!Asset || !IsProjectObject(Asset)) { return; }
	FOut O{ Entities, Edges, Chunks };
	const FString Id = AssetIdOf(Asset);
	const FString Name = Asset->GetName();

	if (const UStaticMesh* SM = Cast<UStaticMesh>(Asset)) { RecipeStaticMesh(O, SM, Id, Name); }
	else if (const USkeletalMesh* SK = Cast<USkeletalMesh>(Asset)) { RecipeSkeletalMesh(O, SK, Id, Name); }
	else if (const UTexture2D* Tex = Cast<UTexture2D>(Asset)) { RecipeTexture(O, Tex, Id, Name); }
	else if (const USkeleton* Skel = Cast<USkeleton>(Asset)) { RecipeSkeleton(O, Skel, Id, Name); }
	else if (const UDataTable* DT = Cast<UDataTable>(Asset)) { RecipeDataTable(O, DT, Id, Name); }
	else if (const UMaterialInterface* Mat = Cast<UMaterialInterface>(Asset)) { RecipeMaterial(O, Mat, Id, Name); }
	else if (const UInputMappingContext* IMC = Cast<UInputMappingContext>(Asset)) { RecipeInputMappingContext(O, IMC, Id, Name); }
	else if (const UInputAction* IA = Cast<UInputAction>(Asset)) { RecipeInputAction(O, IA, Id, Name); }
	else if (UWorld* World = Cast<UWorld>(Asset)) { RecipeLevel(O, World, Id, Name); }
	else
	{
		const FString Props = GenericReflectionSummary(O, Asset, Id);
		const FString ClassName = Asset->GetClass()->GetName();
		AddSummary(O, Id, Props.IsEmpty()
			? FString::Printf(TEXT("%s (%s)"), *Name, *ClassName)
			: FString::Printf(TEXT("%s (%s)\n%s"), *Name, *ClassName, *Props));
	}
}

UGameIQAssetsCommandlet::UGameIQAssetsCommandlet()
{
	IsClient = false;
	IsServer = false;
	IsEditor = true;
	LogToConsole = true;
	ShowErrorCount = false; // benign engine load warnings shouldn't inflate the error count
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

	int32 Considered = 0;
	for (const FAssetData& Data : Assets)
	{
		// Blueprints are handled by the GameIQBlueprints (Tier 2) commandlet.
		if (Data.AssetClassPath.GetAssetName().ToString().EndsWith(TEXT("Blueprint"))) { continue; }

		UObject* Asset = Data.GetAsset();
		if (!Asset || !IsProjectObject(Asset)) { continue; }
		++Considered;
		GameIQAsset::ExtractAsset(Asset, Entities, Edges, Chunks);
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
