// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQAssetCommandlet.h"

#include "Animation/Skeleton.h"
#include "AssetRegistry/ARFilter.h"
#include "Components/ActorComponent.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SpotLightComponent.h"
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
#include "GameIQHash.h"
#include "GameIQJson.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Materials/MaterialInstance.h"
#include "Materials/MaterialInterface.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/Field.h"
#include "UObject/Package.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQAssets, Log, All);

namespace
{
	const TCHAR* AssetsProducer = TEXT("gameiq-ue-assets@0.1.0");
	constexpr int32 MaxActorEntitiesPerLevel = 500; // list this many actors; always aggregate counts

	// A full GC pass costs O(entire live UObject graph), not O(one asset) — flushing after every
	// asset would spend more time in GC than extraction and would thrash shared dependencies
	// (a material used by 50 meshes gets reloaded 50 times). Flushing every N loaded assets instead
	// amortizes that cost while still bounding peak memory across a full /Game scan.
	// (Named per-commandlet: unity builds merge these anonymous namespaces into one TU.)
	constexpr int32 AssetGCFlushInterval = 300;

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

	/**
	 * Texture2D's summary only needs its dimensions, which the AssetRegistry already publishes
	 * as the `Dimensions` tag ("WxH", e.g. "2048x2048" — confirmed live against a running 5.8
	 * editor) — so it never needs a full UObject load. Mirrors RecipeTexture's summary text
	 * exactly, from FAssetData alone. Returns false (caller falls back to a full load) if the
	 * tag is missing or unparseable.
	 */
	bool ExtractTextureNoLoad(FOut& O, const FAssetData& Data, const FString& Id, const FString& Name)
	{
		FString Dims, WStr, HStr;
		if (!Data.GetTagValue(TEXT("Dimensions"), Dims) || !Dims.Split(TEXT("x"), &WStr, &HStr, ESearchCase::IgnoreCase))
		{
			return false;
		}
		AddSummary(O, Id, FString::Printf(TEXT("%s (Texture2D)\n%dx%d"), *Name, FCString::Atoi(*WStr), FCString::Atoi(*HStr)));
		return true;
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

	/** "DirectionalLight" → "Directional Light" so FTS (whole-word tokens) matches a search for "light". */
	FString SplitCamel(const FString& In)
	{
		FString Out;
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR C = In[i];
			if (i > 0 && FChar::IsUpper(C) && !FChar::IsUpper(In[i - 1])) { Out.AppendChar(TEXT(' ')); }
			Out.AppendChar(C);
		}
		return Out;
	}

	/**
	 * Reflect the edit-exposed UPROPERTYs of `Struct` over `Container`, emitting only the ones that
	 * DIFFER from `Default` (i.e. actually configured, not engine defaults) as `name=value` lines +
	 * detail JSON fields, and asset object-references as `references` edges. Recurses one level into
	 * structs so nested overrides (e.g. FPostProcessSettings) are captured. `Budget` bounds the total.
	 */
	void ReflectConfiguredProps(
		FOut& O, const FString& OwnerId, UObject* Parent, const UStruct* Struct,
		const void* Container, const void* Default, const FString& Prefix,
		TArray<FString>& Lines, TSharedRef<FJsonObject>& OutJson, int32& Budget, int32 Depth)
	{
		// Noise that differs from defaults on every placed actor but says nothing about configuration:
		// transforms (already captured as `location`), attachment plumbing, tag arrays, heavy sub-structs.
		static const TSet<FString> Skip = {
			TEXT("RelativeLocation"), TEXT("RelativeRotation"), TEXT("RelativeScale3D"),
			TEXT("RootComponent"), TEXT("AttachParent"), TEXT("AttachSocketName"),
			TEXT("BodyInstance"), TEXT("ComponentTags"), TEXT("Tags"), TEXT("AssetImportData"),
			TEXT("BlueprintCreatedComponents"), TEXT("InstanceComponents") };

		for (TFieldIterator<FProperty> It(Struct); It && Budget > 0; ++It)
		{
			const FProperty* Prop = *It;
			if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_Deprecated)) { continue; }
			if (!Prop->HasAnyPropertyFlags(CPF_Edit)) { continue; } // only user-facing settings
			if (Skip.Contains(Prop->GetName())) { continue; }

			const void* Val = Prop->ContainerPtrToValuePtr<void>(Container);
			const void* Def = Default ? Prop->ContainerPtrToValuePtr<void>(Default) : nullptr;
			if (Def && Prop->Identical(Val, Def, PPF_None)) { continue; } // unchanged from the default

			const FString Field = Prefix.IsEmpty() ? Prop->GetName() : Prefix + TEXT(".") + Prop->GetName();

			// Object references → edge + name, but only to real assets (skip components/actors/levels).
			if (const FObjectPropertyBase* ObjProp = CastField<FObjectPropertyBase>(Prop))
			{
				UObject* Ref = ObjProp->GetObjectPropertyValue(Val);
				if (Ref && Ref->IsAsset() && IsProjectObject(Ref))
				{
					Lines.Add(FString::Printf(TEXT("%s=%s"), *Field, *Ref->GetName()));
					OutJson->SetStringField(Field, Ref->GetName());
					O.Edges.Add(MakeShared<FJsonValueObject>(
						GameIQ::MakeEdge(OwnerId, AssetIdOf(Ref), TEXT("references"), Prop->GetName())));
					--Budget;
				}
				continue;
			}

			if (Prop->IsA<FNumericProperty>() || Prop->IsA<FBoolProperty>() || Prop->IsA<FNameProperty>() ||
				Prop->IsA<FStrProperty>() || Prop->IsA<FEnumProperty>() || Prop->IsA<FByteProperty>())
			{
				FString Value;
				Prop->ExportTextItem_Direct(Value, Val, /*Default=*/nullptr, Parent, PPF_None);
				Value = GameIQ::OneLine(Value);
				if (Value.Len() > 0 && Value.Len() < 80)
				{
					Lines.Add(FString::Printf(TEXT("%s=%s"), *Field, *Value));
					OutJson->SetStringField(Field, Value);
					--Budget;
				}
				continue;
			}

			// One-level struct dive so nested overrides (FPostProcessSettings, FLinearColor, …) surface.
			if (Depth < 1)
			{
				if (const FStructProperty* SP = CastField<FStructProperty>(Prop))
				{
					ReflectConfiguredProps(O, OwnerId, Parent, SP->Struct, Val, Def, Field, Lines, OutJson, Budget, Depth + 1);
				}
			}
		}
	}

	/**
	 * Reflection-based detail for any actor class without a bespoke recipe (SkyLight, SkyAtmosphere,
	 * ExponentialHeightFog, VolumetricCloud, PostProcessVolume, Landscape, …). Captures the actor's own
	 * configured (non-default) properties plus those of each distinct component class. Returns a compact
	 * `name=value` string, or an explicit "engine defaults" marker when nothing was configured — so
	 * callers can tell "uses engine defaults" apart from "unindexed". (issue #1)
	 */
	FString GenericActorDetail(FOut& O, const AActor* Actor, const FString& ActorId, TSharedRef<FJsonObject>& OutJson)
	{
		TArray<FString> Lines;
		int32 Budget = 28;
		UObject* Parent = const_cast<AActor*>(Actor);

		// The actor's own overrides (e.g. APostProcessVolume::Settings/Priority/bUnbound).
		ReflectConfiguredProps(O, ActorId, Parent, Actor->GetClass(), Actor, Actor->GetArchetype(),
			FString(), Lines, OutJson, Budget, 0);

		// Each distinct component class' overrides (SkyLight/Fog/Atmosphere/Cloud settings live here).
		// Dedupe by class so a Landscape's many identical components don't flood the budget.
		TSet<FName> SeenClasses;
		TArray<UActorComponent*> Comps;
		Actor->GetComponents(Comps);
		for (UActorComponent* Comp : Comps)
		{
			if (!Comp || Budget <= 0) { continue; }
			if (SeenClasses.Contains(Comp->GetClass()->GetFName())) { continue; }
			SeenClasses.Add(Comp->GetClass()->GetFName());
			const UObject* Arch = Comp->GetArchetype();
			const FString CompPrefix = Comp->GetClass()->GetName().Replace(TEXT("Component"), TEXT(""));
			ReflectConfiguredProps(O, ActorId, Comp, Comp->GetClass(), Comp,
				Arch ? Arch : Comp->GetClass()->GetDefaultObject(), CompPrefix, Lines, OutJson, Budget, 0);
		}

		if (Lines.Num() == 0)
		{
			// Explicit, machine-readable: this actor was indexed and genuinely uses engine defaults.
			OutJson->SetBoolField(TEXT("usesEngineDefaults"), true);
			return TEXT("engine defaults");
		}
		return FString::Join(Lines, TEXT(" "));
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
				OutJson->SetBoolField(TEXT("castShadows"), LC->CastShadows != 0);
				OutJson->SetBoolField(TEXT("visible"), LC->GetVisibleFlag());

				FString Extra;
				if (const UPointLightComponent* PLC = Cast<UPointLightComponent>(LC))
				{
					OutJson->SetNumberField(TEXT("attenuationRadius"), PLC->AttenuationRadius);
					Extra += FString::Printf(TEXT(" radius=%.0f"), PLC->AttenuationRadius);
					if (const USpotLightComponent* SLC = Cast<USpotLightComponent>(LC))
					{
						OutJson->SetNumberField(TEXT("outerConeAngle"), SLC->OuterConeAngle);
						Extra += FString::Printf(TEXT(" cone=%.0f"), SLC->OuterConeAngle);
					}
				}
				return FString::Printf(TEXT("intensity=%.0f color=%d,%d,%d mobility=%s shadows=%s visible=%s%s"),
					LC->Intensity, Color.R, Color.G, Color.B, *Mob,
					LC->CastShadows ? TEXT("yes") : TEXT("no"),
					LC->GetVisibleFlag() ? TEXT("yes") : TEXT("no"), *Extra);
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
		// Everything else (SkyLight, SkyAtmosphere, fog, clouds, post-process, landscape, …) gets its
		// actual configured properties via reflection, so the index reflects the real level setup. (issue #1)
		return GenericActorDetail(O, Actor, ActorId, OutJson);
	}

	void RecipeLevel(FOut& O, UWorld* World, const FString& Id, const FString& Name)
	{
		ULevel* Level = World->PersistentLevel;
		if (!Level) { return; }

		TMap<FString, int32> ClassCounts;
		int32 Listed = 0;
		int32 Total = 0;

		// Emit one placed actor: a `level-actor` entity (with typed light/mesh detail), a
		// `placed-in-level` edge, a searchable chunk, and a level→class "places" edge.
		auto ExtractActor = [&](const AActor* Actor)
		{
			if (!Actor) { return; }
			++Total;
			const FString ClassName = Actor->GetClass()->GetName();
			ClassCounts.FindOrAdd(ClassName)++;

			// World Partition / editor-generated infrastructure is counted (in the summary) but not worth a
			// per-actor entity — it's derived, not authored, and would bury real actors under the cap.
			static const TSet<FString> DerivedInfra = {
				TEXT("WorldPartitionHLOD"), TEXT("WorldPartitionMiniMap"), TEXT("HLODActor") };
			if (DerivedInfra.Contains(ClassName)) { return; }

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
				// Per-actor chunk so placed actors are searchable by class/name. Include the camelCase-split
				// class ("Directional Light") so a plain search for "light" / "mesh" / "camera" matches the
				// compound class token (FTS indexes whole words).
				O.Chunks.Add(MakeShared<FJsonValueObject>(GameIQ::MakeChunk(
					FString::Printf(TEXT("%s#actor"), *ActorId), ActorId, TEXT("recipe-summary"),
					FString::Printf(TEXT("%s (%s / %s) in %s%s"), *ActorLabel, *ClassName, *SplitCamel(ClassName), *Name,
						Detail.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" — %s"), *Detail)))));
				++Listed;
			}
		};

		for (const AActor* Actor : Level->Actors) { ExtractActor(Actor); }

		// World Partition levels keep their real actors (lights, meshes, …) as One-File-Per-Actor
		// packages under __ExternalActors__, which are NOT in Level->Actors — so a naive walk sees
		// only WorldSettings/Brush/WorldDataLayers. Pull those packages from the Asset Registry and
		// extract each the same way, so a WP level's lights/actors become real, linked level-actor
		// entities with full detail instead of orphaned generic assets. (§12.12)
		int32 External = 0;
		{
			const FString LevelPackage = World->GetPackage()->GetName();
			const FString ExtPath = ULevel::GetExternalActorsPath(LevelPackage);
			if (!ExtPath.IsEmpty())
			{
				FAssetRegistryModule& ARM = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
				FARFilter Filter;
				Filter.PackagePaths.Add(FName(*ExtPath));
				Filter.bRecursivePaths = true;
				TArray<FAssetData> ExtAssets;
				ARM.Get().GetAssets(Filter, ExtAssets);
				for (const FAssetData& AD : ExtAssets)
				{
					if (AActor* Actor = Cast<AActor>(AD.GetAsset())) { ExtractActor(Actor); ++External; }
				}
			}
		}

		ClassCounts.ValueSort([](int32 A, int32 B) { return A > B; });
		TArray<FString> CountLines;
		for (const TPair<FString, int32>& P : ClassCounts)
		{
			CountLines.Add(FString::Printf(TEXT("%s x%d"), *P.Key, P.Value));
		}
		// Full per-class counts are always reported; per-actor entities are capped (design §12 Q7).
		const FString WpNote = External > 0
			? FString::Printf(TEXT(" (World Partition: %d external actors resolved)"), External) : FString();
		const FString CapNote = Total > MaxActorEntitiesPerLevel
			? FString::Printf(TEXT(" (%d listed as entities, all counted below)"), MaxActorEntitiesPerLevel)
			: FString();
		AddSummary(O, Id, FString::Printf(TEXT("%s (Level)\nActors: %d in %d classes%s%s\n%s"),
			*Name, Total, ClassCounts.Num(), *WpNote, *CapNote, *FString::Join(CountLines, TEXT(", "))));
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

	// Incremental rebuild (-full forces a from-scratch pass, ignoring/overwriting the cache).
	const bool bForceFull = FParse::Param(*Params, TEXT("full"));
	const FString HashCachePath = FPaths::Combine(OutDir, TEXT("asset-hashes.json"));
	TMap<FString, GameIQHash::FCachedAssetRow> PrevCache =
		bForceFull ? TMap<FString, GameIQHash::FCachedAssetRow>() : GameIQHash::LoadAssetHashCache(HashCachePath);
	TMap<FString, GameIQHash::FCachedAssetRow> NewCache;
	NewCache.Reserve(PrevCache.Num());

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

	const int32 TotalAssets = Assets.Num();
	int32 Considered = 0;
	int32 Skipped = 0;
	int32 Index = 0;
	int32 LoadedSinceLastGC = 0;
	double LastProgressLogTime = FPlatformTime::Seconds();
	for (const FAssetData& Data : Assets)
	{
		++Index;
		const double Now = FPlatformTime::Seconds();
		if (Now - LastProgressLogTime >= 2.0)
		{
			UE_LOG(LogGameIQAssets, Display, TEXT("Game IQ assets: %d/%d scanned (%d loaded, %d skipped-unchanged so far)..."),
				Index, TotalAssets, Considered - Skipped, Skipped);
			LastProgressLogTime = Now;
		}

		// Blueprints are handled by the GameIQBlueprints (Tier 2) commandlet.
		if (Data.AssetClassPath.GetAssetName().ToString().EndsWith(TEXT("Blueprint"))) { continue; }

		// World Partition external actors (One-File-Per-Actor) are resolved by RecipeLevel into typed,
		// level-linked actor entities — skip them here so they aren't also emitted as orphaned generic
		// assets (which surfaced only misleading top-level property diffs, no light detail).
		if (Data.PackageName.ToString().Contains(TEXT("/__ExternalActors__/"))) { continue; }

		const FString Id = GameIQHash::AssetIdOfPackage(Data.PackageName);
		const FString Sig = GameIQHash::ComputeChangeSignature(AR, Data);

		// Incremental rebuild: an unchanged asset's previously-extracted rows are carried forward
		// verbatim (from the last run's cache), so completeness never regresses — only the
		// (expensive) load + extraction is skipped. An empty Sig means "unknown" and is never cached
		// or matched, so an asset Game IQ can't confidently fingerprint is always reprocessed.
		if (!Sig.IsEmpty())
		{
			if (const GameIQHash::FCachedAssetRow* Cached = PrevCache.Find(Id))
			{
				if (Cached->Signature == Sig)
				{
					Entities.Append(Cached->Entities);
					Edges.Append(Cached->Edges);
					Chunks.Append(Cached->Chunks);
					NewCache.Add(Id, *Cached);
					++Considered;
					++Skipped;
					continue;
				}
			}
		}

		const int32 EntitiesBefore = Entities.Num();
		const int32 EdgesBefore = Edges.Num();
		const int32 ChunksBefore = Chunks.Num();

		// Texture2D's summary is fully derivable from AssetRegistry tags (Dimensions) — skip the
		// full load entirely for it. Any UTexture2D subclass falls through to the normal load path
		// below unchanged, exactly as it did before this fast path existed.
		bool bHandled = false;
		if (Data.AssetClassPath.GetAssetName() == FName(TEXT("Texture2D")))
		{
			FOut O{ Entities, Edges, Chunks };
			bHandled = ExtractTextureNoLoad(O, Data, Id, Data.AssetName.ToString());
		}

		if (!bHandled)
		{
			UObject* Asset = Data.GetAsset();
			if (!Asset || !IsProjectObject(Asset)) { continue; }
			GameIQAsset::ExtractAsset(Asset, Entities, Edges, Chunks);

			// Bound peak memory across a full /Game scan: nothing here holds a reference to Asset
			// (or the dependencies it pulled in) past this point, so a periodic flush lets the GC
			// actually reclaim them — a commandlet's Main() never yields back to the engine tick
			// loop that would otherwise trigger this automatically.
			if (++LoadedSinceLastGC >= AssetGCFlushInterval)
			{
				CollectGarbage(RF_NoFlags, /*bPerformFullPurge=*/true);
				LoadedSinceLastGC = 0;
			}
		}
		++Considered;

		if (!Sig.IsEmpty())
		{
			GameIQHash::FCachedAssetRow Row;
			Row.Signature = Sig;
			if (const int32 N = Entities.Num() - EntitiesBefore) { Row.Entities.Append(Entities.GetData() + EntitiesBefore, N); }
			if (const int32 N = Edges.Num() - EdgesBefore) { Row.Edges.Append(Edges.GetData() + EdgesBefore, N); }
			if (const int32 N = Chunks.Num() - ChunksBefore) { Row.Chunks.Append(Chunks.GetData() + ChunksBefore, N); }
			NewCache.Add(Id, MoveTemp(Row));
		}
	}

	GameIQHash::SaveAssetHashCache(HashCachePath, NewCache);
	UE_LOG(LogGameIQAssets, Display, TEXT("Game IQ assets: %d/%d loaded, %d skipped (unchanged)."),
		Considered - Skipped, TotalAssets, Skipped);

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
