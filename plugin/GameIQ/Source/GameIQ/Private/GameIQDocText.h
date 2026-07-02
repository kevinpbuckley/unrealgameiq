// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Regex.h"

/**
 * Document text extraction + section parsing (issues #2/#5). Turns a source doc into a title plus a
 * list of heading-delimited sections so the docs extractor can emit one searchable chunk per section
 * rather than one blob per file. Markdown/plain-text/reStructuredText are parsed natively; HTML and
 * RTF are reduced to markdown-ish text and reuse the same section parser; DOCX/PDF are reported
 * unsupported (they need a converter — see issue #5) and skipped, never silently emptied. Inline so
 * the docs/link commandlet TUs share it without ODR conflicts under unity builds.
 */
namespace GameIQDocText
{
	/** Replace every match of an ICU regex `Pattern` in `Input` with `Replacement` (UE has no built-in). */
	inline FString RegexReplaceAll(const FString& Input, const FString& Pattern, const FString& Replacement)
	{
		const FRegexPattern Pat(Pattern);
		FRegexMatcher M(Pat, Input);
		FString Out;
		int32 Last = 0;
		while (M.FindNext())
		{
			const int32 B = M.GetMatchBeginning();
			const int32 E = M.GetMatchEnding();
			if (B < Last) { continue; } // guard against zero-width overlaps
			Out += Input.Mid(Last, B - Last);
			Out += Replacement;
			Last = E;
		}
		Out += Input.Mid(Last);
		return Out;
	}

	struct FSection
	{
		FString Heading; // "" for the pre-heading preamble
		FString Slug;    // stable, unique anchor within the doc
		FString Body;    // section text (excluding the heading line)
		int32   Level = 1;
	};

	struct FParsed
	{
		FString Title;            // first H1, else empty (caller falls back to filename)
		TArray<FSection> Sections;
		bool bSupported = true;   // false => format needs a converter; caller skips + logs
		FString Format;           // "markdown" | "text" | "html" | "rtf" | "docx" | "pdf" | "unknown"
	};

	/** kebab-case anchor from a heading; deduped against `Used`. */
	inline FString Slugify(const FString& In, TSet<FString>& Used)
	{
		FString S;
		for (const TCHAR C : In)
		{
			if (FChar::IsAlnum(C)) { S.AppendChar(FChar::ToLower(C)); }
			else if (C == TEXT(' ') || C == TEXT('-') || C == TEXT('_')) { S.AppendChar(TEXT('-')); }
		}
		while (S.Contains(TEXT("--"))) { S = S.Replace(TEXT("--"), TEXT("-")); }
		S.TrimStartAndEndInline();
		while (S.StartsWith(TEXT("-"))) { S.RightChopInline(1); }
		while (S.EndsWith(TEXT("-"))) { S.LeftChopInline(1); }
		if (S.IsEmpty()) { S = TEXT("section"); }
		FString Base = S;
		int32 N = 2;
		while (Used.Contains(S)) { S = FString::Printf(TEXT("%s-%d"), *Base, N++); }
		Used.Add(S);
		return S;
	}

	/** Markdown ATX heading? Returns level (1..6) and the heading text, or 0. */
	inline int32 HeadingLevel(const FString& Line, FString& OutText)
	{
		int32 i = 0;
		while (i < Line.Len() && Line[i] == TEXT('#')) { ++i; }
		if (i >= 1 && i <= 6 && i < Line.Len() && Line[i] == TEXT(' '))
		{
			OutText = Line.Mid(i + 1).TrimStartAndEnd();
			// drop a trailing "####" closing run if present
			OutText = OutText.TrimChar(TEXT('#')).TrimStartAndEnd();
			return i;
		}
		return 0;
	}

	/** Parse markdown-ish text (already de-formatted from HTML/RTF if needed) into sections. */
	inline void ParseMarkdown(const FString& Text, FParsed& Out)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, /*CullEmpty=*/false);

		TSet<FString> UsedSlugs;
		FSection Current;
		Current.Heading = FString();
		Current.Level = 0;
		bool bHaveCurrent = false;
		TArray<FString> Body;

		auto Flush = [&]()
		{
			if (!bHaveCurrent && Body.Num() == 0) { return; }
			Current.Body = FString::Join(Body, TEXT("\n")).TrimStartAndEnd();
			// keep a preamble only if it has real content
			if (bHaveCurrent || !Current.Body.IsEmpty())
			{
				if (Current.Slug.IsEmpty()) { Current.Slug = Slugify(Current.Heading.IsEmpty() ? TEXT("overview") : Current.Heading, UsedSlugs); }
				Out.Sections.Add(Current);
			}
			Body.Reset();
		};

		for (const FString& Raw : Lines)
		{
			FString HeadingText;
			const int32 Level = HeadingLevel(Raw.TrimStart(), HeadingText);
			if (Level > 0)
			{
				Flush();
				Current = FSection();
				Current.Heading = HeadingText;
				Current.Level = Level;
				Current.Slug = Slugify(HeadingText, UsedSlugs);
				bHaveCurrent = true;
				if (Out.Title.IsEmpty() && Level == 1) { Out.Title = HeadingText; }
			}
			else
			{
				Body.Add(Raw);
			}
		}
		Flush();

		// A doc with no headings at all → one "body" section.
		if (Out.Sections.Num() == 0)
		{
			FSection S;
			S.Slug = TEXT("body");
			S.Body = Text.TrimStartAndEnd();
			Out.Sections.Add(S);
		}
	}

	/** Strip a small set of HTML entities we actually see in exported docs. */
	inline FString DecodeEntities(FString S)
	{
		S = S.Replace(TEXT("&nbsp;"), TEXT(" ")).Replace(TEXT("&amp;"), TEXT("&"))
			 .Replace(TEXT("&lt;"), TEXT("<")).Replace(TEXT("&gt;"), TEXT(">"))
			 .Replace(TEXT("&quot;"), TEXT("\"")).Replace(TEXT("&#39;"), TEXT("'"))
			 .Replace(TEXT("&mdash;"), TEXT("—")).Replace(TEXT("&ndash;"), TEXT("–"));
		return S;
	}

	/** Reduce HTML to markdown-ish text: headings → "# …", block tags → newlines, strip the rest. */
	inline FString HtmlToText(const FString& Html)
	{
		FString S = Html;
		// remove script/style/comment blocks (dotall via [\s\S], case-insensitive)
		S = RegexReplaceAll(S, TEXT("(?i)<script[\\s\\S]*?</script>"), TEXT(" "));
		S = RegexReplaceAll(S, TEXT("(?i)<style[\\s\\S]*?</style>"), TEXT(" "));
		S = RegexReplaceAll(S, TEXT("<!--[\\s\\S]*?-->"), TEXT(" "));
		// headings → ATX so the markdown parser picks them up
		for (int32 L = 1; L <= 6; ++L)
		{
			const FString Hashes = FString::ChrN(L, TEXT('#'));
			S = RegexReplaceAll(S, FString::Printf(TEXT("(?i)<h%d[^>]*>"), L), FString::Printf(TEXT("\n%s "), *Hashes));
			S = RegexReplaceAll(S, FString::Printf(TEXT("(?i)</h%d>"), L), TEXT("\n"));
		}
		// common block elements → line breaks
		S = RegexReplaceAll(S, TEXT("(?i)<(br|/p|/div|/li|/tr)[^>]*>"), TEXT("\n"));
		S = RegexReplaceAll(S, TEXT("(?i)<li[^>]*>"), TEXT("\n- "));
		// strip all remaining tags
		S = RegexReplaceAll(S, TEXT("<[^>]+>"), TEXT(""));
		S = DecodeEntities(S);
		// collapse >2 blank lines
		while (S.Contains(TEXT("\n\n\n"))) { S = S.Replace(TEXT("\n\n\n"), TEXT("\n\n")); }
		return S;
	}

	/** Very small RTF → text: drop groups/control words, turn \par into newlines. */
	inline FString RtfToText(const FString& Rtf)
	{
		FString Out;
		Out.Reserve(Rtf.Len());
		int32 i = 0;
		const int32 N = Rtf.Len();
		while (i < N)
		{
			const TCHAR C = Rtf[i];
			if (C == TEXT('\\'))
			{
				// control word: \word or \word123, optional trailing space
				int32 j = i + 1;
				if (j < N && (Rtf[j] == TEXT('\\') || Rtf[j] == TEXT('{') || Rtf[j] == TEXT('}')))
				{
					Out.AppendChar(Rtf[j]); i = j + 1; continue; // escaped literal
				}
				FString Word;
				while (j < N && FChar::IsAlpha(Rtf[j])) { Word.AppendChar(Rtf[j]); ++j; }
				while (j < N && (FChar::IsDigit(Rtf[j]) || Rtf[j] == TEXT('-'))) { ++j; }
				if (j < N && Rtf[j] == TEXT(' ')) { ++j; }
				if (Word == TEXT("par") || Word == TEXT("line") || Word == TEXT("pard")) { Out.AppendChar(TEXT('\n')); }
				i = j;
			}
			else if (C == TEXT('{') || C == TEXT('}')) { ++i; }
			else { Out.AppendChar(C); ++i; }
		}
		return Out;
	}

	/** Dispatch on extension (lowercase, no dot). Loads/parses; sets bSupported=false for DOCX/PDF. */
	inline FParsed Parse(const FString& RawContent, const FString& Ext)
	{
		FParsed Out;
		if (Ext == TEXT("md") || Ext == TEXT("markdown") || Ext == TEXT("mdx"))
		{
			Out.Format = TEXT("markdown");
			ParseMarkdown(RawContent, Out);
		}
		else if (Ext == TEXT("html") || Ext == TEXT("htm"))
		{
			Out.Format = TEXT("html");
			ParseMarkdown(HtmlToText(RawContent), Out);
		}
		else if (Ext == TEXT("rtf"))
		{
			Out.Format = TEXT("rtf");
			ParseMarkdown(RtfToText(RawContent), Out);
		}
		else if (Ext == TEXT("txt") || Ext == TEXT("rst") || Ext == TEXT("text") || Ext == TEXT("log"))
		{
			Out.Format = TEXT("text");
			ParseMarkdown(RawContent, Out); // headings optional; falls back to a single body section
		}
		else if (Ext == TEXT("docx") || Ext == TEXT("doc") || Ext == TEXT("pdf"))
		{
			// Needs a converter (zip/XML for DOCX, a PDF text layer). Left pluggable; skipped for now
			// so we never emit a truncated/empty doc that looks indexed. See issue #5.
			Out.Format = Ext;
			Out.bSupported = false;
		}
		else
		{
			Out.Format = TEXT("unknown");
			Out.bSupported = false;
		}
		return Out;
	}
}
