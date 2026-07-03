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
		const FRegexPattern StylePat(TEXT("<w:pStyle w:val=\"(Heading[1-6]|Title)\""));
		// `<w:t>` or `<w:t xml:space=...>` ONLY — the space/`>` after `w:t` stops it matching
		// `<w:tbl>`, `<w:top>`, `<w:tab/>`, `<w:tblStyle>`, etc. (which corrupted headings on real docs).
		const FRegexPattern TextPat(TEXT("<w:t(?: [^>]*)?>([\\s\\S]*?)</w:t>"));

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

	inline int32 FindAscii(const uint8* Data, int32 N, const FString& S, int32 From)
	{
		const FTCHARToUTF8 Conv(*S);
		return FindBytes(Data, N, Conv.Get(), Conv.Length(), From);
	}

	/** Locate `<ObjNum> 0 obj` with a left digit-boundary, so "4 0 obj" doesn't match inside "234 0 obj". */
	inline int32 FindObjDef(const uint8* Data, int32 N, int32 ObjNum)
	{
		const FString Needle = FString::Printf(TEXT("%d 0 obj"), ObjNum);
		int32 From = 0, Pos;
		while ((Pos = FindAscii(Data, N, Needle, From)) != INDEX_NONE)
		{
			if (Pos == 0 || !(Data[Pos - 1] >= '0' && Data[Pos - 1] <= '9')) { return Pos; }
			From = Pos + 1;
		}
		return INDEX_NONE;
	}

	inline uint32 HexToU32(const FString& H)
	{
		uint32 V = 0;
		for (const TCHAR C : H) { if (FChar::IsHexDigit(C)) { V = V * 16 + FParse::HexDigit(C); } }
		return V;
	}

	/** UTF-16BE hex (e.g. "0052") → string. TCHAR is 16-bit on Windows, so surrogate pairs append in order. */
	inline FString Utf16HexToString(const FString& H)
	{
		FString S;
		for (int32 i = 0; i + 4 <= H.Len(); i += 4)
		{
			uint32 Cu = 0;
			for (int32 K = 0; K < 4; ++K) { Cu = Cu * 16 + FParse::HexDigit(H[i + K]); }
			if (Cu != 0) { S.AppendChar(static_cast<TCHAR>(Cu)); }
		}
		return S;
	}

	/** Parse a /ToUnicode CMap: fills code→unicode Map and the code byte-width (from codespacerange). */
	inline void ParseCMap(const FString& C, TMap<uint32, FString>& Map, int32& ByteWidth)
	{
		{
			FRegexMatcher M(FRegexPattern(TEXT("begincodespacerange([\\s\\S]*?)endcodespacerange")), C);
			if (M.FindNext())
			{
				FRegexMatcher H(FRegexPattern(TEXT("<([0-9A-Fa-f]+)>")), M.GetCaptureGroup(1));
				if (H.FindNext()) { ByteWidth = FMath::Max(ByteWidth, H.GetCaptureGroup(1).Len() / 2); }
			}
		}
		{
			FRegexMatcher Blk(FRegexPattern(TEXT("beginbfchar([\\s\\S]*?)endbfchar")), C);
			while (Blk.FindNext())
			{
				FRegexMatcher E(FRegexPattern(TEXT("<([0-9A-Fa-f]+)>\\s*<([0-9A-Fa-f]+)>")), Blk.GetCaptureGroup(1));
				while (E.FindNext()) { Map.Add(HexToU32(E.GetCaptureGroup(1)), Utf16HexToString(E.GetCaptureGroup(2))); }
			}
		}
		{
			FRegexMatcher Blk(FRegexPattern(TEXT("beginbfrange([\\s\\S]*?)endbfrange")), C);
			while (Blk.FindNext())
			{
				// <lo> <hi> <dst> form (arrayed dst form is uncommon in exporter output; skipped).
				FRegexMatcher E(FRegexPattern(TEXT("<([0-9A-Fa-f]+)>\\s*<([0-9A-Fa-f]+)>\\s*<([0-9A-Fa-f]+)>")), Blk.GetCaptureGroup(1));
				while (E.FindNext())
				{
					const uint32 Lo = HexToU32(E.GetCaptureGroup(1));
					const uint32 Hi = HexToU32(E.GetCaptureGroup(2));
					const uint32 Base = HexToU32(E.GetCaptureGroup(3));
					for (uint32 Code = Lo; Code <= Hi && (Code - Lo) < 65536; ++Code)
					{
						Map.Add(Code, FString().AppendChar(static_cast<TCHAR>(Base + (Code - Lo))));
					}
				}
			}
		}
	}

	/** Find the stream after `From`, decode it (inflate if FlateDecode), advance `From` past endstream. */
	inline bool DecodeStreamAt(const uint8* D, int32 N, int32& From, TArray<uint8>& Out)
	{
		while (From < N)
		{
			const int32 S = FindBytes(D, N, "stream", 6, From);
			if (S == INDEX_NONE) { From = N; return false; }
			if (S >= 3 && FMemory::Memcmp(D + S - 3, "end", 3) == 0) { From = S + 6; continue; }

			int32 DataStart = S + 6;
			if (DataStart < N && D[DataStart] == '\r') { ++DataStart; }
			if (DataStart < N && D[DataStart] == '\n') { ++DataStart; }
			const int32 End = FindBytes(D, N, "endstream", 9, DataStart);
			if (End == INDEX_NONE) { From = N; return false; }

			const int32 DictFrom = FMath::Max(0, S - 512);
			const bool bFlate = FindBytes(D, S, "/FlateDecode", 12, DictFrom) != INDEX_NONE;
			const int32 RawLen = End - DataStart;

			bool bHave = false;
			Out.Reset();
			if (RawLen > 0)
			{
				if (bFlate) { bHave = ZInflate(D + DataStart, RawLen, Out); }
				else { Out.Append(D + DataStart, RawLen); bHave = true; }
			}
			From = End + 9;
			return bHave;
		}
		return false;
	}

	/**
	 * Decoded content bytes → text. A small content-stream tokenizer: it shows the strings of Tj/TJ
	 * through the ACTIVE font's ToUnicode CMap (switched by `/Fn … Tf`, so each Type0 subset uses its
	 * own map — no cross-font collision), and derives line breaks from the text matrix (`Tm`/`Td`/`T*`)
	 * absolute Y. Word spacing comes from the document's own space glyphs (mapped via the CMap) and
	 * in-string spaces — NOT from an artificial space after every show op, which exporters that emit
	 * one glyph per Tj (e.g. Google Docs) would turn into "R i n Q".
	 */
	inline void HarvestContentText(const TArray<uint8>& Content, FString& Out,
		const TMap<FString, TMap<uint32, FString>>& FontCMaps, const TMap<FString, int32>& FontWidths)
	{
		const int32 N = Content.Num();
		int32 i = 0;
		bool bInText = false;

		const TMap<uint32, FString>* ActiveCMap = nullptr;
		int32 ActiveWidth = 1;
		uint32 Pending = 0; int32 PendCount = 0;
		auto ResetPending = [&]() { Pending = 0; PendCount = 0; };
		bool bWroteAny = false;
		auto EmitCode = [&](uint8 B)
		{
			if (!ActiveCMap) { Out.AppendChar(static_cast<TCHAR>(B)); bWroteAny = true; return; }
			Pending = (Pending << 8) | B;
			if (++PendCount >= ActiveWidth)
			{
				if (const FString* M = ActiveCMap->Find(Pending)) { Out.Append(*M); }
				else { Out.AppendChar(static_cast<TCHAR>(Pending & 0xFF)); }
				bWroteAny = true; Pending = 0; PendCount = 0;
			}
		};

		// Line-break detection from the text matrix: track the current line's Y translation; a string
		// shown at a Y different from the previously shown line starts a new line.
		double Ty = 0.0, LineY = 0.0; bool bHaveLineY = false;
		auto NewlineIfMoved = [&]()
		{
			if (bHaveLineY && FMath::Abs(Ty - LineY) > 2.0 && bWroteAny) { Out.AppendChar(TEXT('\n')); }
			LineY = Ty; bHaveLineY = true;
		};

		TArray<double> Nums;
		FString LastName;

		auto ParseLiteralString = [&]()
		{
			NewlineIfMoved();
			++i; int32 Depth = 1;
			while (i < N && Depth > 0)
			{
				const uint8 Ch = Content[i];
				if (Ch == '\\' && i + 1 < N)
				{
					const uint8 Nx = Content[i + 1];
					switch (Nx)
					{
					case 'n': EmitCode('\n'); i += 2; break;
					case 'r': EmitCode('\r'); i += 2; break;
					case 't': EmitCode('\t'); i += 2; break;
					case 'b': case 'f': EmitCode(' '); i += 2; break;
					case '(': EmitCode('('); i += 2; break;
					case ')': EmitCode(')'); i += 2; break;
					case '\\': EmitCode('\\'); i += 2; break;
					case '\r': i += (i + 2 < N && Content[i + 2] == '\n') ? 3 : 2; break;
					case '\n': i += 2; break;
					default:
						if (Nx >= '0' && Nx <= '7')
						{
							int32 Val = 0, K = 0; int32 J = i + 1;
							while (J < N && K < 3 && Content[J] >= '0' && Content[J] <= '7') { Val = Val * 8 + (Content[J] - '0'); ++J; ++K; }
							EmitCode(static_cast<uint8>(Val)); i = J;
						}
						else { EmitCode(Nx); i += 2; }
						break;
					}
				}
				else if (Ch == '(') { ++Depth; EmitCode('('); ++i; }
				else if (Ch == ')') { --Depth; if (Depth > 0) { EmitCode(')'); } ++i; }
				else { EmitCode(Ch); ++i; }
			}
			ResetPending();
		};

		auto ParseHexString = [&]()
		{
			NewlineIfMoved();
			++i; FString Hex;
			while (i < N && Content[i] != '>')
			{
				if (FChar::IsHexDigit(static_cast<TCHAR>(Content[i]))) { Hex.AppendChar(static_cast<TCHAR>(Content[i])); }
				++i;
			}
			if (i < N) { ++i; }
			if (Hex.Len() % 2 == 1) { Hex.AppendChar(TEXT('0')); }
			for (int32 H = 0; H + 1 < Hex.Len(); H += 2)
			{
				EmitCode(static_cast<uint8>(FParse::HexDigit(Hex[H]) * 16 + FParse::HexDigit(Hex[H + 1])));
			}
			ResetPending();
		};

		auto IsDelim = [](uint8 C) { return C == '(' || C == ')' || C == '<' || C == '>' || C == '[' || C == ']' || C == '{' || C == '}' || C == '/' || C == '%'; };

		while (i < N)
		{
			const uint8 C = Content[i];
			if (C == ' ' || C == '\r' || C == '\n' || C == '\t' || C == '\f' || C == 0) { ++i; continue; }
			if (C == '%') { while (i < N && Content[i] != '\n') { ++i; } continue; } // comment
			if (C == '/') { ++i; LastName.Reset(); while (i < N && !IsDelim(Content[i]) && Content[i] > ' ') { LastName.AppendChar(static_cast<TCHAR>(Content[i])); ++i; } continue; }
			if (C == '[' || C == ']') { ++i; continue; } // TJ array brackets: elements are contiguous
			if (C == '<' && i + 1 < N && Content[i + 1] == '<') { i += 2; continue; } // dict open
			if (C == '>') { ++i; continue; }
			if (C == '(') { if (bInText) { ParseLiteralString(); } else { int32 D2 = 1; ++i; while (i < N && D2 > 0) { if (Content[i] == '\\') { i += 2; } else { if (Content[i] == '(') ++D2; else if (Content[i] == ')') --D2; ++i; } } } continue; }
			if (C == '<') { if (bInText) { ParseHexString(); } else { while (i < N && Content[i] != '>') { ++i; } if (i < N) ++i; } continue; }
			if ((C >= '0' && C <= '9') || C == '-' || C == '+' || C == '.')
			{
				FString Num;
				while (i < N && ((Content[i] >= '0' && Content[i] <= '9') || Content[i] == '-' || Content[i] == '+' || Content[i] == '.' || Content[i] == 'e' || Content[i] == 'E'))
				{ Num.AppendChar(static_cast<TCHAR>(Content[i])); ++i; }
				Nums.Add(FCString::Atod(*Num));
				continue;
			}
			if (FChar::IsAlpha(static_cast<TCHAR>(C)) || C == '\'' || C == '"' || C == '*')
			{
				FString Op;
				while (i < N && (FChar::IsAlnum(static_cast<TCHAR>(Content[i])) || Content[i] == '*' || Content[i] == '\'' || Content[i] == '"')) { Op.AppendChar(static_cast<TCHAR>(Content[i])); ++i; }

				if (Op == TEXT("BT")) { bInText = true; ResetPending(); }
				else if (Op == TEXT("ET")) { bInText = false; ResetPending(); }
				else if (Op == TEXT("Tf"))
				{
					if (const TMap<uint32, FString>* M = FontCMaps.Find(LastName)) { ActiveCMap = M->Num() > 0 ? M : nullptr; ActiveWidth = FMath::Max(1, FontWidths.FindRef(LastName)); }
					else { ActiveCMap = nullptr; ActiveWidth = 1; }
					ResetPending();
				}
				else if (Op == TEXT("Tm") && Nums.Num() >= 6) { Ty = Nums[5]; }                 // absolute line Y
				else if ((Op == TEXT("Td") || Op == TEXT("TD")) && Nums.Num() >= 2) { Ty += Nums[1]; } // relative line move
				else if (Op == TEXT("T*")) { Ty -= 12.0; }                                       // next line (leading unknown)

				Nums.Reset();
				continue;
			}
			++i;
		}
		if (bWroteAny) { Out.AppendChar(TEXT('\n')); }
	}

	/**
	 * PDF → text. Two passes: (1) build a code→unicode map from every /ToUnicode CMap so subsetted
	 * embedded fonts (Type0/CID — what Word/Google/InDesign emit) decode to real text, not shifted
	 * glyph codes; (2) scan content streams, inflate the FlateDecode ones, harvest strings shown
	 * between BT…ET through that map. Scanned/image-only PDFs (no text layer) return empty so the
	 * caller degrades honestly. bfrange-array CMaps and astral glyphs are the remaining gaps (issue #5).
	 */
	inline bool ExtractPdf(const FString& Path, FString& Out)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || Bytes.Num() == 0) { return false; }
		const int32 N = Bytes.Num();
		const uint8* D = Bytes.GetData();

		// --- Pass 1: resolve each font resource (/Fn) → its own ToUnicode CMap ---
		// Font resource dicts (/Font << /F4 4 0 R … >>) map names to font objects; each font object's
		// /ToUnicode points at a CMap stream. Per-font maps avoid the glyph-code collisions a global
		// merge produces across Type0 subsets.
		TMap<FString, TMap<uint32, FString>> FontCMaps;
		TMap<FString, int32> FontWidths;
		{
			// Byte-scan for font resource refs `/Fn N 0 R` (NUL-safe — a whole-file FString would be
			// truncated at the first NUL in a binary stream, hiding most font dicts).
			TMap<FString, int32> NameToObj;
			int32 p = 0;
			while ((p = FindBytes(D, N, "/F", 2, p)) != INDEX_NONE)
			{
				int32 q = p + 1; FString Name;
				while (q < N && (FChar::IsAlnum(static_cast<TCHAR>(D[q])) || D[q] == '+')) { Name.AppendChar(static_cast<TCHAR>(D[q])); ++q; }
				while (q < N && D[q] == ' ') { ++q; }
				int32 Num = 0; bool bGot = false;
				while (q < N && D[q] >= '0' && D[q] <= '9') { Num = Num * 10 + (D[q] - '0'); ++q; bGot = true; }
				while (q < N && D[q] == ' ') { ++q; }
				if (bGot && q + 2 < N && D[q] == '0' && D[q + 1] == ' ' && D[q + 2] == 'R') { NameToObj.Add(Name, Num); }
				p += 2;
			}

			for (const TPair<FString, int32>& P : NameToObj)
			{
				const int32 ObjPos = FindObjDef(D, N, P.Value);
				if (ObjPos == INDEX_NONE) { continue; }
				int32 EndObj = FindAscii(D, N, TEXT("endobj"), ObjPos);
				if (EndObj == INDEX_NONE) { EndObj = FMath::Min(N, ObjPos + 4000); }
				const int32 TuPos = FindAscii(D, N, TEXT("/ToUnicode"), ObjPos);
				if (TuPos == INDEX_NONE || TuPos > EndObj) { continue; }

				int32 q = TuPos + 10;
				while (q < N && !(D[q] >= '0' && D[q] <= '9') && q < TuPos + 24) { ++q; }
				int32 CmapNum = 0; bool bGot = false;
				while (q < N && D[q] >= '0' && D[q] <= '9') { CmapNum = CmapNum * 10 + (D[q] - '0'); ++q; bGot = true; }
				if (!bGot) { continue; }

				const int32 CmapPos = FindObjDef(D, N, CmapNum);
				if (CmapPos == INDEX_NONE) { continue; }
				int32 From = CmapPos;
				TArray<uint8> CmapBytes;
				if (DecodeStreamAt(D, N, From, CmapBytes) && CmapBytes.Num() > 0)
				{
					const FString CmapText = FString(FUTF8ToTCHAR(reinterpret_cast<const ANSICHAR*>(CmapBytes.GetData()), CmapBytes.Num()));
					TMap<uint32, FString> Map; int32 Width = 1;
					ParseCMap(CmapText, Map, Width);
					if (Map.Num() > 0) { FontWidths.Add(P.Key, Width); FontCMaps.Add(P.Key, MoveTemp(Map)); }
				}
			}
		}

		// --- Pass 2: harvest text from every content stream, per-font mapped ---
		FString Text;
		int32 From = 0; int32 Streams = 0;
		TArray<uint8> Decoded;
		while (Streams < 8000 && DecodeStreamAt(D, N, From, Decoded))
		{
			if (Decoded.Num() > 0) { HarvestContentText(Decoded, Text, FontCMaps, FontWidths); }
			++Streams;
		}

		// Decompose common typographic ligatures so search matches the plain word (e.g. "strafing" when
		// the PDF used the fi-ligature glyph). Built from code points to avoid source-charset ambiguity.
		{
			auto Lig = [](uint32 Cp) { return FString().AppendChar(static_cast<TCHAR>(Cp)); };
			Text = Text.Replace(*Lig(0xFB00), TEXT("ff")).Replace(*Lig(0xFB01), TEXT("fi"))
				.Replace(*Lig(0xFB02), TEXT("fl")).Replace(*Lig(0xFB03), TEXT("ffi")).Replace(*Lig(0xFB04), TEXT("ffl"));
		}
		Text = Text.Replace(TEXT("\r"), TEXT(" "));
		while (Text.Contains(TEXT("  "))) { Text = Text.Replace(TEXT("  "), TEXT(" ")); }
		while (Text.Contains(TEXT("\n\n\n"))) { Text = Text.Replace(TEXT("\n\n\n"), TEXT("\n\n")); }
		Out = Text.TrimStartAndEnd();
		return !Out.IsEmpty();
	}
}
