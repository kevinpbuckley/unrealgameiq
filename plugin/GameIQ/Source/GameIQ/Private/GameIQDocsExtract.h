// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "GameIQBinaryDoc.h"
#include "GameIQDocText.h"
#include "GameIQDocType.h"
#include "GameIQFileWalk.h"
#include "GameIQJson.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * Shared document extraction (issues #2/#5/#8). Turns a tree of doc files into document + doc-section
 * entities/chunks. Used by the in-repo docs commandlet and the external-connector commandlet (which
 * points it at a local Confluence/Notion/Drive export), so both produce the same searchable, typed,
 * provenance-tagged shape. Log category is defined once in GameIQDocsCommandlet.cpp.
 */
DECLARE_LOG_CATEGORY_EXTERN(LogGameIQDocs, Log, All);

namespace GameIQDocsExtract
{
	constexpr int32 MaxSectionsPerDoc = 400;

	// Extensions we walk. DOCX/PDF are walked so we can *report* them (they need a converter, issue #5)
	// rather than silently ignore — but never emit a truncated doc.
	inline const TArray<FString>& DocExts()
	{
		static const TArray<FString> Exts = {
			TEXT(".md"), TEXT(".markdown"), TEXT(".mdx"), TEXT(".txt"), TEXT(".rst"), TEXT(".text"),
			TEXT(".html"), TEXT(".htm"), TEXT(".rtf"), TEXT(".docx"), TEXT(".doc"), TEXT(".pdf") };
		return Exts;
	}

	/**
	 * Load a doc as text, decoding UTF-8 even without a BOM. FFileHelper::LoadFileToString falls back to
	 * platform ANSI for BOM-less files, which mojibakes em-dashes/smart-quotes common in design docs.
	 */
	inline bool LoadDocText(const FString& File, FString& Out)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *File)) { return false; }
		const int32 N = Bytes.Num();
		if (N == 0) { Out.Reset(); return true; }
		const uint8* D = Bytes.GetData();
		const bool bUtf8Bom = N >= 3 && D[0] == 0xEF && D[1] == 0xBB && D[2] == 0xBF;
		const bool bUtf16   = N >= 2 && ((D[0] == 0xFF && D[1] == 0xFE) || (D[0] == 0xFE && D[1] == 0xFF));
		if (bUtf16 || bUtf8Bom) { return FFileHelper::LoadFileToString(Out, *File); }
		Out = FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(D), N));
		return true;
	}

	inline FString IsoStamp(const FString& File)
	{
		const FDateTime T = IFileManager::Get().GetTimeStamp(*File);
		return T == FDateTime::MinValue() ? FString() : T.ToIso8601();
	}

	/**
	 * Extract every doc under `Root` into the arrays. `Source` tags provenance origin ("repo" or a
	 * connector name), `IdPrefix` namespaces ids so external mirrors don't collide with in-repo docs
	 * (e.g. "confluence:"). Returns the number of documents; fills the walked/skipped counters.
	 */
	inline int32 ExtractTree(const FString& Root, const FString& Source, const FString& IdPrefix,
		TArray<TSharedPtr<FJsonValue>>& Entities, TArray<TSharedPtr<FJsonValue>>& Edges,
		TArray<TSharedPtr<FJsonValue>>& Chunks, int32& OutWalked, int32& OutSkipped, bool bSkipPlugins)
	{
		const TMap<FString, FString> Overrides = GameIQDocs::LoadDocTypeOverrides(Root);
		int32 Docs = 0;

		for (const FString& File : GameIQWalk::WalkFiles(Root, DocExts()))
		{
			++OutWalked;
			const FString Rel = GameIQWalk::RelPath(Root, File);
			// Vendored plugin docs are noise for *this* project's design; the GameIQ plugin is already
			// excluded by the walk. Project docs live outside Plugins/.
			if (bSkipPlugins && Rel.StartsWith(TEXT("Plugins/"))) { continue; }
			// Image caption sidecars (<image>.caption.txt) belong to the image entity, not a doc of their own.
			if (Rel.EndsWith(TEXT(".caption.txt"))) { continue; }

			const FString Ext = FPaths::GetExtension(File).ToLower();
			GameIQDocText::FParsed Parsed;
			if (Ext == TEXT("docx") || Ext == TEXT("doc"))
			{
				// DOCX (zip+XML) via FZipArchiveReader; legacy binary .doc isn't a zip → falls through unsupported.
				FString Text;
				if (GameIQBinaryDoc::ExtractDocx(File, Text) && !Text.TrimStartAndEnd().IsEmpty())
				{
					Parsed.Format = TEXT("docx"); Parsed.bSupported = true;
					GameIQDocText::ParseMarkdown(Text, Parsed);
				}
				else { Parsed.Format = Ext; Parsed.bSupported = false; }
			}
			else if (Ext == TEXT("pdf"))
			{
				// PDF text layer via FlateDecode-stream inflate; scanned/image PDFs yield nothing → skip honestly.
				FString Text;
				if (GameIQBinaryDoc::ExtractPdf(File, Text) && Text.TrimStartAndEnd().Len() >= 16)
				{
					Parsed.Format = TEXT("pdf"); Parsed.bSupported = true;
					GameIQDocText::ParseMarkdown(Text, Parsed);
				}
				else { Parsed.Format = TEXT("pdf (no extractable text layer)"); Parsed.bSupported = false; }
			}
			else
			{
				FString Content;
				if (!LoadDocText(File, Content)) { ++OutSkipped; continue; }
				Parsed = GameIQDocText::Parse(Content, Ext);
			}

			if (!Parsed.bSupported)
			{
				++OutSkipped;
				UE_LOG(LogGameIQDocs, Display, TEXT("  skip %s (%s — issue #5)"), *Rel, *Parsed.Format);
				continue;
			}

			const FString Stem = FPaths::GetBaseFilename(File);
			const FString Title = Parsed.Title.IsEmpty() ? Stem : Parsed.Title;
			const FString DocType = GameIQDocs::Classify(Rel, Overrides);
			const FString DocId = FString::Printf(TEXT("%sdoc:%s"), *IdPrefix, *Rel);
			const FString Stamp = IsoStamp(File);
			const int64 Bytes = IFileManager::Get().FileSize(*File);

			TArray<FString> SectionNames;
			const int32 SectionCount = FMath::Min(Parsed.Sections.Num(), MaxSectionsPerDoc);
			for (int32 i = 0; i < SectionCount; ++i)
			{
				SectionNames.Add(Parsed.Sections[i].Heading.IsEmpty() ? TEXT("(overview)") : Parsed.Sections[i].Heading);
			}

			TSharedRef<FJsonObject> DocDetail = MakeShared<FJsonObject>();
			DocDetail->SetStringField(TEXT("docType"), DocType);
			DocDetail->SetStringField(TEXT("format"), Parsed.Format);
			DocDetail->SetStringField(TEXT("relPath"), Rel);
			DocDetail->SetStringField(TEXT("title"), Title);
			DocDetail->SetStringField(TEXT("source"), Source);
			if (!Stamp.IsEmpty()) { DocDetail->SetStringField(TEXT("lastModified"), Stamp); }
			DocDetail->SetNumberField(TEXT("byteSize"), static_cast<double>(Bytes));
			DocDetail->SetNumberField(TEXT("sectionCount"), Parsed.Sections.Num());
			{
				TArray<TSharedPtr<FJsonValue>> Names;
				for (const FString& Nm : SectionNames) { Names.Add(MakeShared<FJsonValueString>(Nm)); }
				DocDetail->SetArrayField(TEXT("sections"), Names);
			}

			Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
				DocId, TEXT("document"), Title, Rel, TEXT("docs"), FString(),
				FString::Printf(TEXT("%s (%s doc, %d sections) — %s"), *Title, *DocType, Parsed.Sections.Num(), *Rel),
				DocDetail, GameIQ::Authority::StatedIntent)));

			Chunks.Add(MakeShared<FJsonValueObject>(GameIQ::MakeChunk(
				FString::Printf(TEXT("%s#doc"), *DocId), DocId, TEXT("doc-overview"),
				FString::Printf(TEXT("%s [%s] (%s)\nSections: %s"), *Title, *DocType, *Rel, *FString::Join(SectionNames, TEXT(", "))))));

			for (int32 i = 0; i < SectionCount; ++i)
			{
				const GameIQDocText::FSection& S = Parsed.Sections[i];
				const FString SecId = FString::Printf(TEXT("%s#%s"), *DocId, *S.Slug);
				const FString SecName = S.Heading.IsEmpty() ? TEXT("Overview") : S.Heading;

				TSharedRef<FJsonObject> SecDetail = MakeShared<FJsonObject>();
				SecDetail->SetStringField(TEXT("docType"), DocType);
				SecDetail->SetStringField(TEXT("heading"), SecName);
				SecDetail->SetNumberField(TEXT("level"), S.Level);
				SecDetail->SetStringField(TEXT("relPath"), Rel);
				SecDetail->SetStringField(TEXT("anchor"), S.Slug);
				SecDetail->SetStringField(TEXT("source"), Source);

				Entities.Add(MakeShared<FJsonValueObject>(GameIQ::MakeEntity(
					SecId, TEXT("doc-section"), SecName, Rel, TEXT("docs"), DocId,
					FString::Printf(TEXT("%s › %s"), *Title, *SecName), SecDetail, GameIQ::Authority::StatedIntent)));

				const FString Text = FString::Printf(TEXT("%s › %s [%s]\n%s"), *Title, *SecName, *DocType, *S.Body).Left(8000);
				Chunks.Add(MakeShared<FJsonValueObject>(GameIQ::MakeChunk(
					FString::Printf(TEXT("%s#chunk"), *SecId), SecId, TEXT("doc-section"), Text)));
			}

			if (Parsed.Sections.Num() > MaxSectionsPerDoc)
			{
				UE_LOG(LogGameIQDocs, Display, TEXT("  %s: capped at %d of %d sections"),
					*Rel, MaxSectionsPerDoc, Parsed.Sections.Num());
			}
			++Docs;
		}
		return Docs;
	}
}
