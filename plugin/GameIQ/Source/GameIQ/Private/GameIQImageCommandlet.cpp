// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQImageCommandlet.h"

#include "Dom/JsonObject.h"
#include "GameIQFileWalk.h"
#include "GameIQJson.h"
#include "HAL/FileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQImages, Log, All);

namespace
{
	const TCHAR* ImagesProducer = TEXT("gameiq-images@0.1.0");

	const TArray<FString> ImageExts = {
		TEXT(".png"), TEXT(".jpg"), TEXT(".jpeg"), TEXT(".bmp"), TEXT(".tga"),
		TEXT(".webp"), TEXT(".gif"), TEXT(".svg") };

	// Only index imagery that lives where design/reference art is kept — not cooked Content textures or
	// vendored plugins. First path segment (case-insensitive) must be one of these (or config opts in).
	const TArray<FString> DefaultImageRoots = {
		TEXT("documentation"), TEXT("docs"), TEXT("art"), TEXT("reference"), TEXT("references"),
		TEXT("concept"), TEXT("conceptart"), TEXT("design"), TEXT("brand"), TEXT("branding"),
		TEXT("marketing"), TEXT("ux"), TEXT("ui") };

	bool UnderImageRoot(const FString& Rel)
	{
		FString First = Rel;
		int32 Slash;
		if (Rel.FindChar(TEXT('/'), Slash)) { First = Rel.Left(Slash); }
		First = First.ToLower();
		return DefaultImageRoots.Contains(First);
	}

	/** "Arena_Layout_01" / "T_Rock-Diffuse" → distinct lower-case tag tokens (split on _-, camelCase). */
	void TokenizeName(const FString& In, TSet<FString>& Out)
	{
		FString Cur;
		auto Flush = [&]()
		{
			if (Cur.Len() >= 2) { Out.Add(Cur.ToLower()); }
			Cur.Reset();
		};
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR C = In[i];
			if (C == TEXT('_') || C == TEXT('-') || C == TEXT(' ') || C == TEXT('.')) { Flush(); continue; }
			if (i > 0 && FChar::IsUpper(C) && !FChar::IsUpper(In[i - 1])) { Flush(); }
			if (FChar::IsAlnum(C)) { Cur.AppendChar(C); }
		}
		Flush();
	}

	/** Read width/height from the image header without decoding pixels. 0×0 if unsupported (svg/tga/gif). */
	void ReadDimensions(IImageWrapperModule& Module, const FString& File, int32& W, int32& H)
	{
		W = 0; H = 0;
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File) || Bytes.Num() == 0) { return; }
		const EImageFormat Fmt = Module.DetectImageFormat(Bytes.GetData(), Bytes.Num());
		if (Fmt == EImageFormat::Invalid) { return; }
		const TSharedPtr<IImageWrapper> Wrapper = Module.CreateImageWrapper(Fmt);
		if (Wrapper.IsValid() && Wrapper->SetCompressed(Bytes.GetData(), Bytes.Num()))
		{
			W = Wrapper->GetWidth();
			H = Wrapper->GetHeight();
		}
	}
}

UGameIQImageCommandlet::UGameIQImageCommandlet()
{
	IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true;
}

int32 UGameIQImageCommandlet::Main(const FString& /*Params*/)
{
	const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	IImageWrapperModule& ImageModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));

	TArray<TSharedPtr<FJsonValue>> Entities, Edges, Chunks;
	int32 Walked = 0, Indexed = 0;

	for (const FString& File : GameIQWalk::WalkFiles(Root, ImageExts))
	{
		++Walked;
		const FString Rel = GameIQWalk::RelPath(Root, File);
		if (Rel.StartsWith(TEXT("Plugins/")) || !UnderImageRoot(Rel)) { continue; }

		const FString Stem = FPaths::GetBaseFilename(File);
		const FString Ext = FPaths::GetExtension(File).ToLower();
		int32 W = 0, H = 0;
		ReadDimensions(ImageModule, File, W, H);

		// tags from the path + filename so search finds "layout"/"logo"/"arena" etc.
		TSet<FString> TagSet;
		{
			TArray<FString> Segs; Rel.ParseIntoArray(Segs, TEXT("/"), true);
			for (const FString& Seg : Segs) { TokenizeName(Seg, TagSet); }
			TagSet.Add(TEXT("image"));
		}
		TArray<FString> Tags = TagSet.Array();

		// Optional sidecar caption: "<image>.caption.txt". The vision-caption pass is external (an agent
		// with vision writes the sidecar); absent it we degrade to path/filename tags. (issue #7)
		FString Caption;
		FFileHelper::LoadFileToString(Caption, *(File + TEXT(".caption.txt")));
		Caption = GameIQ::OneLine(Caption);

		const FString Id = FString::Printf(TEXT("image:%s"), *Rel);
		const FString Stamp = IFileManager::Get().GetTimeStamp(*File).ToIso8601();

		TSharedRef<FJsonObject> Detail = MakeShared<FJsonObject>();
		Detail->SetStringField(TEXT("relPath"), Rel);
		Detail->SetStringField(TEXT("absPath"), File); // hand-off so a vision agent can open the real file
		Detail->SetStringField(TEXT("ext"), Ext);
		Detail->SetNumberField(TEXT("width"), W);
		Detail->SetNumberField(TEXT("height"), H);
		Detail->SetStringField(TEXT("source"), TEXT("repo"));
		if (!Stamp.IsEmpty()) { Detail->SetStringField(TEXT("lastModified"), Stamp); }
		if (!Caption.IsEmpty()) { Detail->SetStringField(TEXT("caption"), Caption); }
		{
			TArray<TSharedPtr<FJsonValue>> TagArr;
			for (const FString& T : Tags) { TagArr.Add(MakeShared<FJsonValueString>(T)); }
			Detail->SetArrayField(TEXT("tags"), TagArr);
		}

		const FString Dims = (W > 0 && H > 0) ? FString::Printf(TEXT("%dx%d"), W, H) : TEXT("?");
		Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
			Id, TEXT("image"), Stem, Rel, TEXT("images"), FString(),
			FString::Printf(TEXT("%s (%s image, %s) — %s"), *Stem, *Ext, *Dims, *Rel),
			Detail, GameIQ::Authority::StatedIntent)));

		// Searchable chunk: filename tokens + tags + caption. No pixels in the DB.
		Chunks.Add(MakeShared<FJsonValueObject>(GameIQ::MakeChunk(
			FString::Printf(TEXT("%s#img"), *Id), Id, TEXT("image"),
			FString::Printf(TEXT("%s [%s] tags: %s%s"), *Stem, *Rel, *FString::Join(Tags, TEXT(", ")),
				Caption.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("\n%s"), *Caption)))));
		++Indexed;
	}

	const FString OutDir = FPaths::Combine(Root, TEXT(".gameiq"), TEXT("extract"));
	IFileManager::Get().MakeDirectory(*OutDir, true);
	GameIQ::WriteOutput(OutDir, TEXT("images.json"), ImagesProducer, Entities, Edges, Chunks);

	UE_LOG(LogGameIQImages, Display,
		TEXT("Game IQ images: %d indexed (%d files walked) → %d entities, %d chunks → images.json"),
		Indexed, Walked, Entities.Num(), Chunks.Num());
	return 0;
}
