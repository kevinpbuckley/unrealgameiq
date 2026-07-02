// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQCodeCommandlet.h"

#include "Dom/JsonObject.h"
#include "GameIQFileWalk.h"
#include "GameIQJson.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQCode, Log, All);

namespace
{
	struct FProp { FString Name; FString Type; };
	struct FParsedClass
	{
		FString Macro, Name, Base, File;
		TArray<FString> Functions;
		TArray<FProp> Properties;
	};

	/** Strip the UE source prefix (A/U/F/I/S/T/E + uppercase) so ids match the reflection name. */
	FString ReflectionName(const FString& Ident)
	{
		if (Ident.Len() >= 2 && FChar::IsUpper(Ident[1]))
		{
			const TCHAR C = Ident[0];
			if (C == 'A' || C == 'U' || C == 'F' || C == 'I' || C == 'S' || C == 'T' || C == 'E')
			{
				return Ident.Mid(1);
			}
		}
		return Ident;
	}

	/** TObjectPtr<UFoo> / UFoo* / TArray<UBar> → the inner UE type identifier, or empty. */
	FString InnerTypeName(const FString& Type)
	{
		FString Base;
		FRegexMatcher Tmpl(FRegexPattern(TEXT("<\\s*([A-Za-z_]\\w*)")), Type);
		if (Tmpl.FindNext())
		{
			Base = Tmpl.GetCaptureGroup(1);
		}
		else
		{
			Base = Type.Replace(TEXT("*"), TEXT("")).Replace(TEXT("&"), TEXT(""));
		}
		FRegexMatcher Id(FRegexPattern(TEXT("[A-Za-z_]\\w*")), Base);
		FString Last;
		while (Id.FindNext()) { Last = Id.GetCaptureGroup(0); } // trailing identifier
		return Last;
	}

	/** Matching `{ ... }` starting at/after `From`; returns open index (and sets OutClose), or -1. */
	int32 FindBlock(const FString& Text, int32 From, int32& OutClose)
	{
		const int32 Open = Text.Find(TEXT("{"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		if (Open < 0) { return -1; }
		int32 Depth = 0;
		for (int32 i = Open; i < Text.Len(); ++i)
		{
			if (Text[i] == '{') { ++Depth; }
			else if (Text[i] == '}') { if (--Depth == 0) { OutClose = i; return Open; } }
		}
		return -1;
	}

	void ParseFile(const FString& Text, const FString& File, TArray<FParsedClass>& Out)
	{
		FRegexMatcher Reflected(FRegexPattern(TEXT("(UCLASS|USTRUCT|UINTERFACE)\\s*\\(")), Text);
		while (Reflected.FindNext())
		{
			const int32 Idx = Reflected.GetMatchBeginning();
			const FString Macro = Reflected.GetCaptureGroup(1);
			const FString After = Text.Mid(Idx, 4000);

			FRegexMatcher Decl(FRegexPattern(
				TEXT("\\)\\s*(?:class|struct)\\s+(?:[A-Z0-9_]+_API\\s+)?([A-Za-z_]\\w*)\\b(?:\\s*:\\s*public\\s+([A-Za-z_]\\w*))?")), After);
			if (!Decl.FindNext()) { continue; }

			const int32 DeclAbs = Idx + Decl.GetMatchBeginning();
			int32 Close = 0;
			const int32 Open = FindBlock(Text, DeclAbs, Close);
			if (Open < 0) { continue; }
			const FString Body = Text.Mid(Open, Close - Open + 1);

			FParsedClass C;
			C.Macro = Macro;
			C.Name = Decl.GetCaptureGroup(1);
			C.Base = Decl.GetCaptureGroup(2); // empty if absent
			C.File = File;

			FRegexMatcher Fn(FRegexPattern(TEXT("UFUNCTION\\s*\\([\\s\\S]*?\\)\\s*[\\s\\S]*?\\b([A-Za-z_]\\w*)\\s*\\(")), Body);
			while (Fn.FindNext()) { C.Functions.Add(Fn.GetCaptureGroup(1)); }

			FRegexMatcher Prop(FRegexPattern(TEXT("UPROPERTY\\s*\\([\\s\\S]*?\\)\\s*([A-Za-z_][\\w:<>,\\s\\*&]*?)\\b([A-Za-z_]\\w*)\\s*(?:;|=|\\{)")), Body);
			while (Prop.FindNext())
			{
				const FString Type = Prop.GetCaptureGroup(1).TrimStartAndEnd();
				if (Type.IsEmpty()) { continue; }
				C.Properties.Add(FProp{ Prop.GetCaptureGroup(2), Type });
			}

			Out.Add(MoveTemp(C));
		}
	}
}

UGameIQCodeCommandlet::UGameIQCodeCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQCodeCommandlet::Main(const FString& /*Params*/)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	TArray<FParsedClass> All;
	for (const FString& File : GameIQWalk::WalkFiles(Root, { TEXT(".h") }))
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) { continue; }
		if (!Text.Contains(TEXT("UCLASS")) && !Text.Contains(TEXT("USTRUCT")) && !Text.Contains(TEXT("UINTERFACE"))) { continue; }
		ParseFile(Text, File, All);
	}

	// Canonical (prefix-less) reflection names of every parsed type, for resolving property refs.
	TSet<FString> Known;
	for (const FParsedClass& C : All) { Known.Add(ReflectionName(C.Name)); }

	TArray<TSharedPtr<FJsonValue>> Entities, Edges, Chunks;
	for (const FParsedClass& C : All)
	{
		const FString Rel = GameIQWalk::RelPath(Root, C.File);
		const FString Canonical = ReflectionName(C.Name);
		const FString Id = FString::Printf(TEXT("cpp:%s"), *Canonical);
		const FString Kind = C.Macro == TEXT("USTRUCT") ? TEXT("cpp-struct") : TEXT("cpp-class");
		const FString Header = FString::Printf(TEXT("%s %s%s"), *C.Macro, *C.Name,
			C.Base.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" : public %s"), *C.Base));

		// class entity
		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("sourceName"), C.Name);
		if (C.Base.IsEmpty()) { Detail->SetField(TEXT("base"), MakeShared<FJsonValueNull>()); }
		else { Detail->SetStringField(TEXT("base"), C.Base); }
		TArray<TSharedPtr<FJsonValue>> FnArr;
		for (const FString& F : C.Functions) { FnArr.Add(MakeShared<FJsonValueString>(F)); }
		Detail->SetArrayField(TEXT("functions"), FnArr);
		TArray<TSharedPtr<FJsonValue>> PropArr;
		for (const FProp& P : C.Properties)
		{
			TSharedRef<FJsonObject> PO = MakeShared<FJsonObject>();
			PO->SetStringField(TEXT("name"), P.Name);
			PO->SetStringField(TEXT("type"), P.Type);
			PropArr.Add(MakeShared<FJsonValueObject>(PO));
		}
		Detail->SetArrayField(TEXT("properties"), PropArr);
		Detail->SetStringField(TEXT("file"), Rel);

		Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
			Id, Kind, C.Name, Rel, TEXT("code"), FString(), Header, Detail)));

		if (!C.Base.IsEmpty())
		{
			Edges.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEdge(
				Id, FString::Printf(TEXT("cpp:%s"), *ReflectionName(C.Base)), TEXT("inherits"))));
		}

		TArray<FString> MemberLines;
		for (const FString& Fn : C.Functions)
		{
			const FString Fid = FString::Printf(TEXT("cpp:%s::%s"), *Canonical, *Fn);
			Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
				Fid, TEXT("cpp-function"), Fn, Rel, TEXT("code"), Id,
				FString::Printf(TEXT("%s::%s()"), *C.Name, *Fn), nullptr)));
			Chunks.Add(MakeShared<FJsonValueObject>(GameIQ::MakeChunk(
				FString::Printf(TEXT("%s#sig"), *Fid), Fid, TEXT("cpp-signature"),
				FString::Printf(TEXT("%s::%s()"), *C.Name, *Fn))));
			MemberLines.Add(FString::Printf(TEXT("  %s()"), *Fn));
		}
		for (const FProp& P : C.Properties)
		{
			const FString Pid = FString::Printf(TEXT("cpp:%s::%s"), *Canonical, *P.Name);
			TSharedRef<FJsonObject> PDetail = MakeShared<FJsonObject>();
			PDetail->SetStringField(TEXT("type"), P.Type);
			Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
				Pid, TEXT("cpp-property"), P.Name, Rel, TEXT("code"), Id,
				FString::Printf(TEXT("%s %s"), *P.Type, *P.Name), PDetail)));

			const FString Ref = ReflectionName(InnerTypeName(P.Type));
			if (!Ref.IsEmpty() && Ref != Canonical && Known.Contains(Ref))
			{
				Edges.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEdge(
					Id, FString::Printf(TEXT("cpp:%s"), *Ref), TEXT("references"), P.Name)));
			}
			MemberLines.Add(FString::Printf(TEXT("  %s %s"), *P.Type, *P.Name));
		}

		Chunks.Add(MakeShared<FJsonValueObject>(GameIQ::MakeChunk(
			FString::Printf(TEXT("%s#sig"), *Id), Id, TEXT("cpp-signature"),
			FString::Printf(TEXT("%s\n%s"), *Header, *FString::Join(MemberLines, TEXT("\n"))))));
	}

	const FString OutDir = FPaths::Combine(Root, TEXT(".gameiq"), TEXT("extract"));
	IFileManager::Get().MakeDirectory(*OutDir, true);
	GameIQ::WriteOutput(OutDir, TEXT("cpp.json"), TEXT("gameiq-cpp@0.1.0"), Entities, Edges, Chunks);
	UE_LOG(LogGameIQCode, Display, TEXT("Game IQ code: %d classes → %d entities, %d edges, %d chunks → cpp.json"),
		All.Num(), Entities.Num(), Edges.Num(), Chunks.Num());
	return 0;
}
