// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Regex.h"

/**
 * /Game asset paths found in text (issue #32): DataTable cells and C++ string literals. Shared by
 * the assets stage (DataTable rows) and the code stage (`loads-by-path` edges); inline for the
 * same unity-build reason as GameIQFileWalk.h.
 */
namespace GameIQPaths
{
	/** "/Game/A/B.B_C" or "/Game/A/B.B:Sub" -> "/Game/A/B" (package name). */
	inline FString PackageOf(FString Path)
	{
		int32 Cut = INDEX_NONE;
		if (Path.FindChar(TEXT(':'), Cut)) { Path.LeftInline(Cut); }
		if (Path.FindChar(TEXT('.'), Cut)) { Path.LeftInline(Cut); }
		return Path;
	}

	/** Unique /Game package names mentioned anywhere in `Text` (exported property text, JSON, …). */
	inline TArray<FString> FindPackages(const FString& Text)
	{
		TArray<FString> Out;
		if (!Text.Contains(TEXT("/Game/"))) { return Out; }
		FRegexMatcher M(FRegexPattern(TEXT("/Game/[A-Za-z0-9_\\-/\\.:]+")), Text);
		while (M.FindNext())
		{
			FString Package = PackageOf(M.GetCaptureGroup(0));
			while (Package.EndsWith(TEXT("/"))) { Package.LeftChopInline(1); }
			if (Package.Len() > 6) { Out.AddUnique(Package); }
		}
		return Out;
	}

	/** One /Game string literal found in code, classified for edge emission. */
	struct FLiteral
	{
		FString Literal;   // as written, e.g. "/Game/Anims/A_Idle_%s.A_Idle_%s"
		FString Target;    // "asset:/Game/..." for an exact path, "asset-pattern:/Game/...*" for a pattern
	};

	/**
	 * Classify a code literal. Exact object/package paths become `asset:` targets; printf-style
	 * format strings (%s, %d, …), folder literals ("/Game/Audio/SFX/") and name prefixes ending in
	 * '_' ("/Game/UI/T_wound_") become `asset-pattern:` wildcards over package names.
	 */
	inline FLiteral Classify(const FString& Literal)
	{
		FLiteral Out{ Literal };
		FString Path = Literal;
		if (Path.Contains(TEXT("%")))
		{
			Path = PackageOf(Path);
			FRegexMatcher Spec(FRegexPattern(TEXT("%[-+ #0-9.]*[A-Za-z]")), Path);
			FString Pattern;
			int32 Last = 0;
			while (Spec.FindNext())
			{
				Pattern += Path.Mid(Last, Spec.GetMatchBeginning() - Last) + TEXT("*");
				Last = Spec.GetMatchEnding();
			}
			Pattern += Path.Mid(Last);
			Out.Target = TEXT("asset-pattern:") + Pattern;
			return Out;
		}
		if (Path.EndsWith(TEXT("/")))
		{
			Out.Target = TEXT("asset-pattern:") + Path + TEXT("*");
			return Out;
		}
		const FString Package = PackageOf(Path);
		if (!Path.Contains(TEXT(".")) && Package.EndsWith(TEXT("_")))
		{
			Out.Target = TEXT("asset-pattern:") + Package + TEXT("*");
			return Out;
		}
		Out.Target = TEXT("asset:") + Package;
		return Out;
	}

	/** Blank out comments (keeping string literals) so a path in a comment is not an edge. */
	inline FString StripComments(const FString& In)
	{
		FString Out = In;
		const int32 N = Out.Len();
		int32 i = 0;
		while (i < N)
		{
			const TCHAR C = Out[i];
			if (C == '/' && i + 1 < N && Out[i + 1] == '/')
			{
				while (i < N && Out[i] != '\n') { Out[i++] = ' '; }
			}
			else if (C == '/' && i + 1 < N && Out[i + 1] == '*')
			{
				while (i < N && !(Out[i] == '*' && i + 1 < N && Out[i + 1] == '/')) { Out[i++] = ' '; }
				if (i < N) { Out[i++] = ' '; Out[i++] = ' '; }
			}
			else if (C == '"' || C == '\'')
			{
				const TCHAR Quote = C;
				++i;
				while (i < N && Out[i] != Quote && Out[i] != '\n')
				{
					if (Out[i] == '\\' && i + 1 < N) { ++i; }
					++i;
				}
				if (i < N) { ++i; }
			}
			else { ++i; }
		}
		return Out;
	}

	/** Every "/Game/..." string literal in C++ source (comments excluded), in order, unique. */
	inline TArray<FLiteral> FindCodeLiterals(const FString& Source)
	{
		TArray<FLiteral> Out;
		if (!Source.Contains(TEXT("/Game/"))) { return Out; }
		const FString Code = StripComments(Source);
		FRegexMatcher M(FRegexPattern(TEXT("\"(/Game/[^\"\\s]*)")), Code);
		TSet<FString> Seen;
		while (M.FindNext())
		{
			const FString Literal = M.GetCaptureGroup(1);
			if (Literal.Len() <= 6 || Seen.Contains(Literal)) { continue; }
			Seen.Add(Literal);
			Out.Add(Classify(Literal));
		}
		return Out;
	}
}
