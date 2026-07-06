// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Regex.h"

/**
 * Regex-level C++ parsing shared by the code commandlet and its tests (inline for the same
 * unity-build reason as GameIQFileWalk.h). Two passes:
 *  - headers: reflected class/struct declarations, UFUNCTION/UPROPERTY members;
 *  - .cpp files: out-of-line function definitions (bodies) + Enhanced Input BindAction wiring.
 * The .cpp pass matters because input handlers (Move/Look/Attack…) are usually NOT UFUNCTIONs,
 * so the header pass alone never sees them — "what does dodge do" was unanswerable.
 */
namespace GameIQCpp
{
	struct FProp { FString Name; FString Type; };
	struct FParsedClass
	{
		FString Macro, Name, Base, File;
		TArray<FString> Functions;
		TArray<FProp> Properties;
	};

	/** Out-of-line definition `AClass::Func(...) { ... }` found in a .cpp. */
	struct FCppBody { FString ClassName /*source name, with prefix*/, Func, Body; };

	/** `BindAction(Prop, ETriggerEvent::X, this, &AClass::Handler)` call site.
	 *  ("Site" suffix: the engine already owns `FInputBinding`, and unity builds merge TUs.) */
	struct FInputBindingSite { FString ClassName, Prop, Handler, Trigger; };

	/** Strip the UE source prefix (A/U/F/I/S/T/E + uppercase) so ids match the reflection name. */
	inline FString ReflectionName(const FString& Ident)
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
	inline FString InnerTypeName(const FString& Type)
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
	inline int32 FindBlock(const FString& Text, int32 From, int32& OutClose)
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

	/** Index of the ')' matching the '(' at `Open`, or -1. */
	inline int32 MatchParen(const FString& Text, int32 Open)
	{
		int32 Depth = 0;
		for (int32 i = Open; i < Text.Len(); ++i)
		{
			if (Text[i] == '(') { ++Depth; }
			else if (Text[i] == ')') { if (--Depth == 0) { return i; } }
		}
		return -1;
	}

	/** Reflected class/struct declarations + UFUNCTION/UPROPERTY members from one header. */
	inline void ParseHeader(const FString& Text, const FString& File, TArray<FParsedClass>& Out)
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

	/**
	 * Out-of-line definitions of `KnownSourceNames` classes in one .cpp, plus BindAction wiring.
	 * A `Cls::Fn(` match only counts as a definition when the balanced parameter list is followed
	 * by `{` (after const/noexcept) or by a constructor init list (`:` with Fn == Cls) — call
	 * sites end in `;`/`)`/`,` and are rejected, so `Super::BeginPlay();` never produces a body.
	 */
	inline void ParseCppBodies(const FString& Text, const TSet<FString>& KnownSourceNames,
		TArray<FCppBody>& OutBodies, TArray<FInputBindingSite>& OutBindings)
	{
		FRegexMatcher Def(FRegexPattern(TEXT("\\b([A-Za-z_]\\w*)::([A-Za-z_]\\w*)\\s*\\(")), Text);
		while (Def.FindNext())
		{
			const FString Cls = Def.GetCaptureGroup(1);
			if (!KnownSourceNames.Contains(Cls)) { continue; }
			const FString Fn = Def.GetCaptureGroup(2);

			const int32 CloseParen = MatchParen(Text, Def.GetMatchEnding() - 1);
			if (CloseParen < 0) { continue; }

			// Skip whitespace and trailing qualifiers; anything else means "not a definition".
			int32 i = CloseParen + 1;
			while (i < Text.Len())
			{
				while (i < Text.Len() && FChar::IsWhitespace(Text[i])) { ++i; }
				if (i < Text.Len() && (FChar::IsAlpha(Text[i]) || Text[i] == '_'))
				{
					int32 j = i;
					while (j < Text.Len() && (FChar::IsAlnum(Text[j]) || Text[j] == '_')) { ++j; }
					const FString Word = Text.Mid(i, j - i);
					if (Word == TEXT("const") || Word == TEXT("noexcept")
						|| Word == TEXT("override") || Word == TEXT("final")) { i = j; continue; }
				}
				break;
			}
			if (i >= Text.Len()) { continue; }

			const bool bBody = Text[i] == '{';
			// `:` after the params is a ctor init list, but only for an actual constructor —
			// otherwise it's a ternary branch like `x ? AFoo::Bar(y) : z`.
			const bool bCtorInit = Text[i] == ':' && (i + 1 >= Text.Len() || Text[i + 1] != ':') && Fn == Cls;
			if (!bBody && !bCtorInit) { continue; }

			int32 CloseBrace = 0;
			const int32 OpenBrace = FindBlock(Text, i, CloseBrace);
			if (OpenBrace < 0) { continue; }
			const FString Body = Text.Mid(OpenBrace, CloseBrace - OpenBrace + 1);

			// Enhanced Input wiring inside this body. The instance argument (`this`, a
			// component, …) is skipped; an optional `Obj->`/`Obj.` prefix on the action is
			// allowed so `Input->JumpAction` still yields "JumpAction".
			FRegexMatcher Bind(FRegexPattern(TEXT(
				"BindAction\\s*\\(\\s*(?:[A-Za-z_]\\w*\\s*(?:->|\\.)\\s*)?([A-Za-z_]\\w*)\\s*,\\s*ETriggerEvent::([A-Za-z_]\\w*)\\s*,\\s*[^,]+,\\s*&\\s*([A-Za-z_]\\w*)::([A-Za-z_]\\w*)")), Body);
			while (Bind.FindNext())
			{
				OutBindings.Add(FInputBindingSite{
					Bind.GetCaptureGroup(3), Bind.GetCaptureGroup(1),
					Bind.GetCaptureGroup(4), Bind.GetCaptureGroup(2) });
			}

			OutBodies.Add(FCppBody{ Cls, Fn, Body });
		}
	}
}
