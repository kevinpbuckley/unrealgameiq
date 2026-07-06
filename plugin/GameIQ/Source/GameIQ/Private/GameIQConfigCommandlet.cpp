// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQConfigCommandlet.h"

#include "Dom/JsonObject.h"
#include "GameIQFileWalk.h"
#include "GameIQJson.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQConfig, Log, All);

namespace
{
	struct FIniKey { FString Key; FString Value; };
	struct FIniSection { FString Name; TArray<FIniKey> Keys; };

	// Mirror of config.ts parseIni.
	TArray<FIniSection> ParseIni(const FString& Text)
	{
		TArray<FIniSection> Sections;
		FIniSection* Current = nullptr;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*CullEmpty=*/false);
		for (const FString& Raw : Lines)
		{
			const FString Line = Raw.TrimStartAndEnd();
			if (Line.IsEmpty() || Line.StartsWith(TEXT(";")) || Line.StartsWith(TEXT("#"))) { continue; }
			if (Line.StartsWith(TEXT("[")) && Line.EndsWith(TEXT("]")))
			{
				FIniSection S;
				S.Name = Line.Mid(1, Line.Len() - 2);
				Sections.Add(S);
				Current = &Sections.Last();
				continue;
			}
			int32 Eq;
			if (Current && Line.FindChar(TEXT('='), Eq) && Eq > 0)
			{
				Current->Keys.Add(FIniKey{ Line.Left(Eq).TrimStartAndEnd(), Line.Mid(Eq + 1).TrimStartAndEnd() });
			}
		}
		return Sections;
	}
}

UGameIQConfigCommandlet::UGameIQConfigCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQConfigCommandlet::Main(const FString& /*Params*/)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	TArray<TSharedPtr<FJsonValue>> Entities, Edges, Chunks;

	// --- .ini sections ---
	for (const FString& File : GameIQWalk::WalkFiles(Root, { TEXT(".ini") }))
	{
		// Stale duplicates ("DefaultEngine - Copy.ini", "*backup*") carry outdated settings that
		// outrank the live file in search — never index them.
		const FString Base = FPaths::GetBaseFilename(File);
		if (Base.Contains(TEXT(" - Copy")) || Base.Contains(TEXT("backup"), ESearchCase::IgnoreCase)) { continue; }
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) { continue; }
		const FString Rel = GameIQWalk::RelPath(Root, File);
		for (const FIniSection& Section : ParseIni(Text))
		{
			const FString Id = FString::Printf(TEXT("config:%s#%s"), *Rel, *Section.Name);

			TArray<FString> BodyLines;
			TArray<TSharedPtr<FJsonValue>> KeyArr;
			for (const FIniKey& K : Section.Keys)
			{
				BodyLines.Add(FString::Printf(TEXT("%s=%s"), *K.Key, *K.Value));
				TSharedRef<FJsonObject> KO = MakeShared<FJsonObject>();
				KO->SetStringField(TEXT("key"), K.Key);
				KO->SetStringField(TEXT("value"), K.Value);
				KeyArr.Add(MakeShared<FJsonValueObject>(KO));
			}
			const FString Body = FString::Join(BodyLines, TEXT("\n"));

			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetStringField(TEXT("file"), Rel);
			Detail->SetArrayField(TEXT("keys"), KeyArr);

			Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
				Id, TEXT("config-section"), Section.Name, Rel, TEXT("config"), FString(),
				FString::Printf(TEXT("[%s] in %s (%d keys)"), *Section.Name, *Rel, Section.Keys.Num()), Detail)));
			Chunks.Add(MakeShared<FJsonValueObject>(GameIQ::MakeChunk(
				FString::Printf(TEXT("%s#block"), *Id), Id, TEXT("config-block"),
				FString::Printf(TEXT("[%s] (%s)\n%s"), *Section.Name, *Rel, *Body))));
		}
	}

	// --- .uproject / .uplugin enabled plugins ---
	for (const FString& File : GameIQWalk::WalkFiles(Root, { TEXT(".uproject"), TEXT(".uplugin") }))
	{
		FString Json;
		if (!FFileHelper::LoadFileToString(Json, *File)) { continue; }
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (!FJsonSerializer::Deserialize(Reader, Obj) || !Obj.IsValid()) { continue; }
		const FString Rel = GameIQWalk::RelPath(Root, File);

		const TArray<TSharedPtr<FJsonValue>>* Plugins = nullptr;
		if (!Obj->TryGetArrayField(TEXT("Plugins"), Plugins)) { continue; }
		for (const TSharedPtr<FJsonValue>& V : *Plugins)
		{
			const TSharedPtr<FJsonObject> P = V.IsValid() ? V->AsObject() : nullptr;
			if (!P.IsValid()) { continue; }
			FString Name;
			if (!P->TryGetStringField(TEXT("Name"), Name) || Name.IsEmpty()) { continue; }
			bool bEnabled = true;
			P->TryGetBoolField(TEXT("Enabled"), bEnabled);

			const FString Id = FString::Printf(TEXT("plugin:%s"), *Name);
			TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
			Detail->SetBoolField(TEXT("enabled"), bEnabled);
			Detail->SetStringField(TEXT("declaredIn"), Rel);

			Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
				Id, TEXT("plugin"), Name, Rel, TEXT("config"), FString(),
				FString::Printf(TEXT("Plugin %s (%s) — declared in %s"), *Name, bEnabled ? TEXT("enabled") : TEXT("disabled"), *Rel), Detail)));
			Chunks.Add(MakeShared<FJsonValueObject>(GameIQ::MakeChunk(
				FString::Printf(TEXT("%s#decl"), *Id), Id, TEXT("config-block"),
				FString::Printf(TEXT("Plugin %s enabled=%s in %s"), *Name, bEnabled ? TEXT("true") : TEXT("false"), *Rel))));
		}
	}

	const FString OutDir = FPaths::Combine(Root, TEXT(".gameiq"), TEXT("extract"));
	IFileManager::Get().MakeDirectory(*OutDir, true);
	GameIQ::WriteOutput(OutDir, TEXT("config.json"), TEXT("gameiq-config@0.1.0"), Entities, Edges, Chunks);
	UE_LOG(LogGameIQConfig, Display, TEXT("Game IQ config: %d entities, %d chunks → config.json"), Entities.Num(), Chunks.Num());
	return 0;
}
