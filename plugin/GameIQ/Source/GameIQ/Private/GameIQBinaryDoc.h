// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Regex.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"

THIRD_PARTY_INCLUDES_START
#include "zlib.h"
THIRD_PARTY_INCLUDES_END

#if WITH_EDITOR
#include "FileUtilities/ZipArchiveReader.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#endif

/**
 * Binary document text extraction (issue #5), dependency-free — using only what UE 5.8 already ships:
 *   - DOCX: `FZipArchiveReader` (FileUtilities, over the bundled libzip) reads `word/document.xml`.
 *   - PDF:  the text layer is pulled from FlateDecode content streams via zlib (`FCompression`).
 * Both reduce to markdown-ish text (headings as ATX '#') so the shared section parser handles them.
 * Inline so the docs/connector commandlet TUs share it without ODR conflicts under unity builds.
 */
namespace GameIQBinaryDoc
{
	inline FString DecodeXmlEntities(FString S)
	{
		// &amp; must be last so it doesn't double-decode.
		return S.Replace(TEXT("&lt;"), TEXT("<")).Replace(TEXT("&gt;"), TEXT(">"))
			 .Replace(TEXT("&quot;"), TEXT("\"")).Replace(TEXT("&apos;"), TEXT("'"))
			 .Replace(TEXT("&#39;"), TEXT("'")).Replace(TEXT("&#160;"), TEXT(" "))
			 .Replace(TEXT("&amp;"), TEXT("&"));
	}

	/**
	 * DOCX → markdown-ish text. A .docx is a ZIP; `word/document.xml` holds the body as `<w:p>`
	 * paragraphs of `<w:t>` text runs, with heading level in `<w:pStyle w:val="Heading1"|"Title">`.
	 * Returns false if the file isn't a readable docx (e.g. legacy binary .doc), so the caller degrades.
	 */
	inline bool ExtractDocx(const FString& Path, FString& Out)
	{
#if WITH_EDITOR
		IFileHandle* Handle = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path);
		if (!Handle) { return false; }
		FZipArchiveReader Zip(Handle); // takes ownership of Handle (closes it in dtor)
		if (!Zip.IsValid()) { return false; }

		TArray<uint8> Xml;
		if (!Zip.TryReadFile(TEXT("word/document.xml"), Xml) || Xml.Num() == 0) { return false; }
		const FString XmlStr = FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(Xml.GetData()), Xml.Num()));

		const FRegexPattern ParaPat(TEXT("<w:p[ >][\\s\\S]*?</w:p>"));
		const FRegexPattern StylePat(TEXT("w:val=\"(Heading[1-6]|Title)\""));
		const FRegexPattern TextPat(TEXT("<w:t[^>]*>([\\s\\S]*?)</w:t>"));

		TArray<FString> Lines;
		FRegexMatcher PM(ParaPat, XmlStr);
		while (PM.FindNext())
		{
			const FString Para = XmlStr.Mid(PM.GetMatchBeginning(), PM.GetMatchEnding() - PM.GetMatchBeginning());

			int32 Level = 0;
			{
				FRegexMatcher SM(StylePat, Para);
				if (SM.FindNext())
				{
					const FString V = SM.GetCaptureGroup(1);
					Level = (V == TEXT("Title")) ? 1 : (V[7] - TEXT('0')); // "Heading" is 7 chars; [7] is the digit
					Level = FMath::Clamp(Level, 1, 6);
				}
			}

			FString Text;
			{
				FRegexMatcher TM(TextPat, Para);
				while (TM.FindNext()) { Text += TM.GetCaptureGroup(1); }
			}
			Text = DecodeXmlEntities(Text);

			if (Level > 0) { Lines.Add(FString::ChrN(Level, TEXT('#')) + TEXT(" ") + Text.TrimStartAndEnd()); }
			else { Lines.Add(Text); }
		}
		Out = FString::Join(Lines, TEXT("\n"));
		return true;
#else
		return false;
#endif
	}

	// ---- PDF text-layer extraction (bounded, dependency-free via zlib) --------------------------

	/** Streaming zlib inflate (auto-detects zlib/gzip header). Bounded to 64 MB out. */
	inline bool ZInflate(const uint8* Src, int32 SrcLen, TArray<uint8>& Out)
	{
		if (SrcLen <= 0) { return false; }
		z_stream Strm;
		FMemory::Memzero(&Strm, sizeof(Strm));
		if (inflateInit2(&Strm, 47) != Z_OK) { return false; } // 47 = auto-detect zlib/gzip, max window
		Strm.next_in = const_cast<Bytef*>(Src);
		Strm.avail_in = static_cast<uInt>(SrcLen);

		uint8 Buf[16384];
		int Ret = Z_OK;
		do
		{
			Strm.next_out = Buf;
			Strm.avail_out = sizeof(Buf);
			Ret = inflate(&Strm, Z_NO_FLUSH);
			if (Ret != Z_OK && Ret != Z_STREAM_END && Ret != Z_BUF_ERROR) { break; }
			const int32 Produced = static_cast<int32>(sizeof(Buf) - Strm.avail_out);
			if (Produced > 0) { Out.Append(Buf, Produced); }
			if (Ret == Z_BUF_ERROR && Produced == 0) { break; } // no forward progress
			if (Out.Num() > 64 * 1024 * 1024) { break; }
		} while (Ret != Z_STREAM_END && Strm.avail_in > 0);

		inflateEnd(&Strm);
		return Out.Num() > 0;
	}

	/** Find `Needle` in `[Data+From, Data+N)`; returns index or INDEX_NONE. */
	inline int32 FindBytes(const uint8* Data, int32 N, const char* Needle, int32 NeedleLen, int32 From)
	{
		for (int32 i = FMath::Max(0, From); i + NeedleLen <= N; ++i)
		{
			if (FMemory::Memcmp(Data + i, Needle, NeedleLen) == 0) { return i; }
		}
		return INDEX_NONE;
	}

	/** Decoded content-stream bytes (Latin-1) → readable text: strings shown between BT…ET. */
	inline void HarvestContentText(const TArray<uint8>& Content, FString& Out)
	{
		const int32 N = Content.Num();
		int32 i = 0;
		bool bInText = false;
		auto AppendByte = [&Out](uint8 B) { Out.AppendChar(static_cast<TCHAR>(B)); };

		while (i < N)
		{
			const uint8 C = Content[i];

			// BT / ET toggle text mode (token with a boundary after it).
			if ((C == 'B' || C == 'E') && i + 1 < N && Content[i + 1] == 'T' &&
				(i + 2 >= N || Content[i + 2] == ' ' || Content[i + 2] == '\r' || Content[i + 2] == '\n' || Content[i + 2] == '\t'))
			{
				bInText = (C == 'B');
				if (!bInText) { Out.Append(TEXT("\n")); }
				i += 2;
				continue;
			}

			if (bInText && C == '(')
			{
				// literal string with escapes + nesting
				++i; int32 Depth = 1;
				while (i < N && Depth > 0)
				{
					const uint8 Ch = Content[i];
					if (Ch == '\\' && i + 1 < N)
					{
						const uint8 Nx = Content[i + 1];
						switch (Nx)
						{
						case 'n': AppendByte('\n'); i += 2; break;
						case 'r': AppendByte('\r'); i += 2; break;
						case 't': AppendByte('\t'); i += 2; break;
						case 'b': case 'f': AppendByte(' '); i += 2; break;
						case '(': AppendByte('('); i += 2; break;
						case ')': AppendByte(')'); i += 2; break;
						case '\\': AppendByte('\\'); i += 2; break;
						case '\r': i += (i + 2 < N && Content[i + 2] == '\n') ? 3 : 2; break; // line continuation
						case '\n': i += 2; break;
						default:
							if (Nx >= '0' && Nx <= '7') // octal \ddd
							{
								int32 Val = 0, K = 0; int32 J = i + 1;
								while (J < N && K < 3 && Content[J] >= '0' && Content[J] <= '7') { Val = Val * 8 + (Content[J] - '0'); ++J; ++K; }
								AppendByte(static_cast<uint8>(Val)); i = J;
							}
							else { AppendByte(Nx); i += 2; }
							break;
						}
					}
					else if (Ch == '(') { ++Depth; AppendByte('('); ++i; }
					else if (Ch == ')') { --Depth; if (Depth > 0) { AppendByte(')'); } ++i; }
					else { AppendByte(Ch); ++i; }
				}
				Out.AppendChar(TEXT(' '));
				continue;
			}

			if (bInText && C == '<' && i + 1 < N && Content[i + 1] != '<')
			{
				// hex string <48 65 ...>
				++i; FString Hex;
				while (i < N && Content[i] != '>')
				{
					const uint8 H = Content[i];
					if (FChar::IsHexDigit(static_cast<TCHAR>(H))) { Hex.AppendChar(static_cast<TCHAR>(H)); }
					++i;
				}
				if (i < N) { ++i; } // skip '>'
				if (Hex.Len() % 2 == 1) { Hex.AppendChar(TEXT('0')); }
				for (int32 H = 0; H + 1 < Hex.Len(); H += 2)
				{
					const int32 Byte = FParse::HexDigit(Hex[H]) * 16 + FParse::HexDigit(Hex[H + 1]);
					AppendByte(static_cast<uint8>(Byte));
				}
				Out.AppendChar(TEXT(' '));
				continue;
			}

			++i;
		}
	}

	/**
	 * PDF → text. Brute-force but robust for text-layer PDFs: scan for content streams, inflate the
	 * FlateDecode ones (zlib), and harvest strings shown between BT…ET. Returns false / trivially-empty
	 * for scanned/image-only PDFs (no text layer) so the caller degrades honestly. No font ToUnicode
	 * remapping, so custom-encoded embedded fonts may yield gibberish — acceptable best-effort (issue #5).
	 */
	inline bool ExtractPdf(const FString& Path, FString& Out)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() == 0) { return false; }
		const int32 N = Bytes.Num();
		const uint8* D = Bytes.GetData();

		FString Text;
		int32 i = 0;
		int32 Streams = 0;
		while (i < N && Streams < 4000)
		{
			const int32 S = FindBytes(D, N, "stream", 6, i);
			if (S == INDEX_NONE) { break; }
			// Skip the 'stream' inside 'endstream'.
			if (S >= 3 && FMemory::Memcmp(D + S - 3, "end", 3) == 0) { i = S + 6; continue; }

			int32 DataStart = S + 6;
			if (DataStart < N && D[DataStart] == '\r') { ++DataStart; }
			if (DataStart < N && D[DataStart] == '\n') { ++DataStart; }

			const int32 End = FindBytes(D, N, "endstream", 9, DataStart);
			if (End == INDEX_NONE) { break; }

			// Is this stream FlateDecode? Look back at the preceding dictionary.
			const int32 DictFrom = FMath::Max(0, S - 512);
			const bool bFlate = FindBytes(D, S, "/FlateDecode", 12, DictFrom) != INDEX_NONE;

			const int32 RawLen = End - DataStart;
			if (RawLen > 0)
			{
				TArray<uint8> Decoded;
				bool bHave = false;
				if (bFlate) { bHave = ZInflate(D + DataStart, RawLen, Decoded); }
				else { Decoded.Append(D + DataStart, RawLen); bHave = true; }
				if (bHave) { HarvestContentText(Decoded, Text); ++Streams; }
			}
			i = End + 9;
		}

		// Collapse runaway whitespace produced by per-string spacing.
		Text = Text.Replace(TEXT("\r"), TEXT(" "));
		while (Text.Contains(TEXT("  "))) { Text = Text.Replace(TEXT("  "), TEXT(" ")); }
		while (Text.Contains(TEXT("\n\n\n"))) { Text = Text.Replace(TEXT("\n\n\n"), TEXT("\n\n")); }
		Out = Text.TrimStartAndEnd();
		return !Out.IsEmpty();
	}
}
