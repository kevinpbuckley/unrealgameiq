// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQCodeCommandlet.h"

#include "Dom/JsonObject.h"
#include "GameIQCppFingerprint.h"
#include "GameIQCppParse.h"
#include "GameIQFileWalk.h"
#include "GameIQJson.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQCode, Log, All);

namespace
{
	// Function bodies are indexed for retrieval ("where is the dodge logic"), not as a source
	// mirror — cap keeps one pathological function from dominating the FTS index.
	constexpr int32 MaxBodyChunkChars = 2400;
}

UGameIQCodeCommandlet::UGameIQCodeCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQCodeCommandlet::Main(const FString& /*Params*/)
{
	using namespace GameIQCpp;

	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());

	TArray<FParsedClass> All;
	for (const FString& File : GameIQWalk::WalkFiles(Root, { TEXT(".h") }))
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File)) { continue; }
		if (!Text.Contains(TEXT("UCLASS")) && !Text.Contains(TEXT("USTRUCT")) && !Text.Contains(TEXT("UINTERFACE"))) { continue; }
		ParseHeader(Text, File, All);
	}

	// Canonical (prefix-less) reflection names of every parsed type, for resolving property refs;
	// source names (with prefix) gate the .cpp definition pass below.
	TSet<FString> Known, KnownSource;
	for (const FParsedClass& C : All) { Known.Add(ReflectionName(C.Name)); KnownSource.Add(C.Name); }

	TArray<TSharedPtr<FJsonValue>> Entities, Edges, Chunks;
	TSet<FString> FunctionIds; // header-pass functions, so the .cpp pass doesn't re-emit entities
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
			FunctionIds.Add(Fid);
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

	// ---- .cpp pass: function bodies + Enhanced Input wiring. Header UFUNCTION scraping never
	// sees plain-method input handlers (Move/Look/Attack…), so this pass also CREATES their
	// cpp-function entities; bodies become `cpp-body` chunks and BindAction call sites become
	// property → handler `binds-input` edges. ----
	int32 NumBodies = 0, NumBindings = 0;
	TSet<FString> BodyChunkIds, BindingKeys;
	for (const FString& File : GameIQWalk::WalkFiles(Root, { TEXT(".cpp") }))
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *File) || !Text.Contains(TEXT("::"))) { continue; }

		TArray<FCppBody> Bodies;
		TArray<FInputBindingSite> Bindings;
		ParseCppBodies(Text, KnownSource, Bodies, Bindings);
		const FString Rel = GameIQWalk::RelPath(Root, File);

		for (const FCppBody& B : Bodies)
		{
			const FString Canonical = ReflectionName(B.ClassName);
			const FString Fid = FString::Printf(TEXT("cpp:%s::%s"), *Canonical, *B.Func);
			const FString ChunkId = Fid + TEXT("#body");
			if (BodyChunkIds.Contains(ChunkId)) { continue; } // overloads share an id: first wins
			BodyChunkIds.Add(ChunkId);

			if (!FunctionIds.Contains(Fid))
			{
				FunctionIds.Add(Fid);
				Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
					Fid, TEXT("cpp-function"), B.Func, Rel, TEXT("code"),
					FString::Printf(TEXT("cpp:%s"), *Canonical),
					FString::Printf(TEXT("%s::%s()"), *B.ClassName, *B.Func), nullptr)));
			}

			FString BodyText = B.Body;
			if (BodyText.Len() > MaxBodyChunkChars)
			{
				BodyText = BodyText.Left(MaxBodyChunkChars) + TEXT("\n… (truncated)");
			}
			Chunks.Add(MakeShared<FJsonValueObject>(GameIQ::MakeChunk(
				ChunkId, Fid, TEXT("cpp-body"),
				FString::Printf(TEXT("%s::%s\n%s"), *B.ClassName, *B.Func, *BodyText))));
			++NumBodies;
		}

		for (const FInputBindingSite& Bind : Bindings)
		{
			const FString Canonical = ReflectionName(Bind.ClassName);
			const FString Key = FString::Printf(TEXT("%s|%s|%s"), *Canonical, *Bind.Prop, *Bind.Handler);
			if (BindingKeys.Contains(Key)) { continue; } // Triggered/Completed pairs → one edge
			BindingKeys.Add(Key);
			Edges.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEdge(
				FString::Printf(TEXT("cpp:%s::%s"), *Canonical, *Bind.Prop),
				FString::Printf(TEXT("cpp:%s::%s"), *Canonical, *Bind.Handler),
				TEXT("binds-input"), Bind.Trigger)));
			++NumBindings;
		}
	}

	const FString OutDir = FPaths::Combine(Root, TEXT(".gameiq"), TEXT("extract"));
	IFileManager::Get().MakeDirectory(*OutDir, true);
	GameIQ::WriteOutput(OutDir, TEXT("cpp.json"), TEXT("gameiq-cpp@0.1.0"), Entities, Edges, Chunks);
	// Stamp the source fingerprint this extraction saw — the editor's staleness check compares
	// against it to decide whether an automatic C++-only reindex is needed (GameIQSaveHook).
	GameIQCppFingerprint::Write(Root, GameIQCppFingerprint::Compute(Root));
	UE_LOG(LogGameIQCode, Display,
		TEXT("Game IQ code: %d classes, %d bodies, %d input bindings → %d entities, %d edges, %d chunks → cpp.json"),
		All.Num(), NumBodies, NumBindings, Entities.Num(), Edges.Num(), Chunks.Num());
	return 0;
}
