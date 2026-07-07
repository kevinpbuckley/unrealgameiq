// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Regex.h"

/**
 * Regex-level C++ parsing shared by the code commandlet and its tests (inline for the same
 * unity-build reason as GameIQFileWalk.h). Three passes:
 *  - headers: reflected class/struct declarations, UFUNCTION/UPROPERTY members;
 *  - .cpp files: out-of-line function definitions (bodies) + Enhanced Input BindAction wiring;
 *  - call graph: best-effort `calls` edges from bodies, once every function is known.
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

	/** Out-of-line definition `AClass::Func(...) { ... }` found in a .cpp. `Params` is the raw
	 *  parameter-list text — call extraction resolves `Param->Fn(...)` receivers through it. */
	struct FCppBody { FString ClassName /*source name, with prefix*/, Func, Body, Params; };

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

			OutBodies.Add(FCppBody{ Cls, Fn, Body,
				Text.Mid(Def.GetMatchEnding(), CloseParen - Def.GetMatchEnding()) });
		}
	}

	// ---- best-effort call graph (`calls` edges) ------------------------------------------------
	// Only unambiguous shapes are extracted, and only calls that resolve to an indexed function
	// become edges. Dynamic dispatch across hierarchies, delegates/timers, chained receivers and
	// calls through local variables are intentionally not modeled — the query layer flags the
	// graph as partial so an empty result reads as "not modeled", not "no callers".

	/** Blank out comments and string/char literals (lengths preserved) so the call scan can't
	 *  match identifiers inside them. */
	inline FString StripCommentsAndStrings(const FString& In)
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
				Out[i++] = ' '; Out[i++] = ' ';
				while (i < N && !(Out[i] == '*' && i + 1 < N && Out[i + 1] == '/')) { Out[i++] = ' '; }
				if (i < N) { Out[i++] = ' '; Out[i++] = ' '; }
			}
			else if (C == '"' || C == '\'')
			{
				const TCHAR Quote = C;
				Out[i++] = ' ';
				while (i < N && Out[i] != Quote)
				{
					if (Out[i] == '\\' && i + 1 < N) { Out[i++] = ' '; }
					Out[i++] = ' ';
				}
				if (i < N) { Out[i++] = ' '; }
			}
			else { ++i; }
		}
		return Out;
	}

	/** Resolution tables for call extraction, built AFTER all headers and .cpp files are parsed —
	 *  non-UFUNCTION functions only become known once a body is seen. All keys and values are
	 *  canonical (prefix-less) names. */
	struct FCallResolver
	{
		TMap<FString, FString> BaseOf;                    // class -> base, only when the base is parsed too
		TMap<FString, TSet<FString>> FunctionsOf;         // class -> indexed function names
		TMap<FString, TMap<FString, FString>> PropTypeOf; // class -> UPROPERTY name -> canonical type

		/** First class in the ancestor chain (starting at `Class`) that declares `Fn`; empty if none. */
		FString OwnerOf(const FString& Class, const FString& Fn) const
		{
			FString C = Class;
			for (int32 Guard = 0; Guard < 64 && !C.IsEmpty(); ++Guard)
			{
				const TSet<FString>* Fns = FunctionsOf.Find(C);
				if (Fns && Fns->Contains(Fn)) { return C; }
				const FString* B = BaseOf.Find(C);
				C = B ? *B : FString();
			}
			return FString();
		}

		/** Canonical type of property `Prop` on `Class` or an ancestor; empty if unknown. */
		FString PropType(const FString& Class, const FString& Prop) const
		{
			FString C = Class;
			for (int32 Guard = 0; Guard < 64 && !C.IsEmpty(); ++Guard)
			{
				if (const TMap<FString, FString>* Props = PropTypeOf.Find(C))
				{
					if (const FString* T = Props->Find(Prop)) { return *T; }
				}
				const FString* B = BaseOf.Find(C);
				C = B ? *B : FString();
			}
			return FString();
		}
	};

	/** Param name -> canonical type, for parameters whose type is a parsed class. */
	inline void ParseParamTypes(const FString& Params, const TSet<FString>& KnownCanonical,
		TMap<FString, FString>& Out)
	{
		TArray<FString> Pieces;
		int32 Depth = 0, Start = 0;
		for (int32 i = 0; i <= Params.Len(); ++i)
		{
			const TCHAR C = i < Params.Len() ? Params[i] : TEXT(',');
			if (C == '<' || C == '(') { ++Depth; }
			else if (C == '>' || C == ')') { --Depth; }
			else if (C == ',' && Depth <= 0)
			{
				Pieces.Add(Params.Mid(Start, i - Start));
				Start = i + 1;
			}
		}
		for (FString P : Pieces)
		{
			int32 Eq = INDEX_NONE;
			if (P.FindChar(TEXT('='), Eq)) { P.LeftInline(Eq); } // strip default value
			P.TrimStartAndEndInline();
			int32 End = P.Len();
			while (End > 0 && (FChar::IsAlnum(P[End - 1]) || P[End - 1] == '_')) { --End; }
			const FString Name = P.Mid(End);
			const FString Type = P.Left(End);
			if (Name.IsEmpty() || Type.IsEmpty()) { continue; }
			const FString Canonical = ReflectionName(InnerTypeName(Type));
			if (!Canonical.IsEmpty() && KnownCanonical.Contains(Canonical)) { Out.Add(Name, Canonical); }
		}
	}

	/**
	 * Call sites in one body, resolved against indexed functions only. Shapes:
	 *   - bare `Fn(`            -> own class + ancestor chain
	 *   - `Super::Fn(`          -> ancestor chain from the base
	 *   - `AClass::Fn(`         -> that class + its ancestors (qualified/static calls)
	 *   - `X->Fn(` / `X.Fn(`    -> X is `this`, a UPROPERTY (own or inherited), or a parameter
	 * Callees are emitted as canonical `Class::Fn` keys.
	 */
	inline void ExtractCalls(const FString& RawBody, const FString& CanonicalClass,
		const TMap<FString, FString>& ParamTypes, const FCallResolver& R, TSet<FString>& OutCallees)
	{
		const FString Body = StripCommentsAndStrings(RawBody);
		const auto IsIdentChar = [](TCHAR C) { return FChar::IsAlnum(C) || C == '_'; };

		// `Cls::Fn(` and `Super::Fn(`
		FRegexMatcher Qual(FRegexPattern(TEXT("([A-Za-z_]\\w*)\\s*::\\s*([A-Za-z_]\\w*)\\s*\\(")), Body);
		while (Qual.FindNext())
		{
			const int32 Begin = Qual.GetMatchBeginning();
			if (Begin > 0 && (IsIdentChar(Body[Begin - 1]) || Body[Begin - 1] == ':')) { continue; }
			const FString Fn = Qual.GetCaptureGroup(2);
			FString Cls = Qual.GetCaptureGroup(1);
			if (Cls == TEXT("Super"))
			{
				const FString* B = R.BaseOf.Find(CanonicalClass);
				if (!B) { continue; } // base is an engine class — not indexed
				Cls = *B;
			}
			else { Cls = ReflectionName(Cls); }
			const FString Owner = R.OwnerOf(Cls, Fn);
			if (!Owner.IsEmpty()) { OutCallees.Add(Owner + TEXT("::") + Fn); }
		}

		// `X->Fn(` / `X.Fn(` — receiver must be `this`, a typed UPROPERTY, or a typed parameter;
		// a chained receiver (`A->B->Fn(`) is skipped: B's owner class is unknowable here.
		FRegexMatcher Member(FRegexPattern(
			TEXT("([A-Za-z_]\\w*)\\s*(->|\\.)\\s*([A-Za-z_]\\w*)\\s*\\(")), Body);
		while (Member.FindNext())
		{
			const int32 Begin = Member.GetMatchBeginning();
			const TCHAR Prev = Begin > 0 ? Body[Begin - 1] : TEXT(' ');
			if (IsIdentChar(Prev) || Prev == '>' || Prev == '.' || Prev == ':') { continue; }
			const FString Obj = Member.GetCaptureGroup(1);
			const FString Fn = Member.GetCaptureGroup(3);
			FString Cls;
			if (Obj == TEXT("this")) { Cls = CanonicalClass; }
			else if (const FString* PT = ParamTypes.Find(Obj)) { Cls = *PT; }
			else { Cls = R.PropType(CanonicalClass, Obj); }
			if (Cls.IsEmpty()) { continue; }
			const FString Owner = R.OwnerOf(Cls, Fn);
			if (!Owner.IsEmpty()) { OutCallees.Add(Owner + TEXT("::") + Fn); }
		}

		// bare `Fn(` — own class + ancestors. The preceding char must not put the identifier in a
		// member/qualified/declaration context (those are the shapes above, or noise).
		FRegexMatcher Bare(FRegexPattern(TEXT("([A-Za-z_]\\w*)\\s*\\(")), Body);
		while (Bare.FindNext())
		{
			const int32 Begin = Bare.GetMatchBeginning();
			const TCHAR Prev = Begin > 0 ? Body[Begin - 1] : TEXT(' ');
			if (IsIdentChar(Prev) || Prev == '>' || Prev == '.' || Prev == ':'
				|| Prev == '&' || Prev == '~') { continue; }
			const FString Fn = Bare.GetCaptureGroup(1);
			const FString Owner = R.OwnerOf(CanonicalClass, Fn);
			if (!Owner.IsEmpty()) { OutCallees.Add(Owner + TEXT("::") + Fn); }
		}
	}
}
