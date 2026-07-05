// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Text helpers shared by the store (aux token column), the query engine (FTS query building),
 * and the extractors (camelCase splitting for searchability). Inline so multiple TUs (merged in
 * unity builds) can share it without ODR conflicts, matching GameIQJson.h's convention.
 */
namespace GameIQText
{
	/** "DirectionalLight" → "Directional Light" so FTS (whole-word tokens) matches a search for "light". */
	inline FString SplitCamel(const FString& In)
	{
		FString Out;
		for (int32 i = 0; i < In.Len(); ++i)
		{
			const TCHAR C = In[i];
			if (i > 0 && FChar::IsUpper(C) && !FChar::IsUpper(In[i - 1]) && In[i - 1] != TEXT(' ')) { Out.AppendChar(TEXT(' ')); }
			Out.AppendChar(C);
		}
		return Out;
	}

	/** Word tokens of a query/text: runs of [alnum_], everything else is a separator. */
	inline TArray<FString> WordTokens(const FString& Input)
	{
		TArray<FString> Tokens;
		FString Cur;
		for (const TCHAR C : Input)
		{
			if (FChar::IsAlnum(C) || C == TEXT('_')) { Cur.AppendChar(C); }
			else if (Cur.Len() > 0) { Tokens.Add(Cur); Cur.Reset(); }
		}
		if (Cur.Len() > 0) { Tokens.Add(Cur); }
		return Tokens;
	}

	/**
	 * FTS5 MATCH expression from free text. Each token is double-quoted (so FTS metacharacters
	 * in user input can't break the query) and joined with the given operator ("AND"/"OR").
	 * bPrefix appends `*` to each quoted token for prefix matching (needs the fts prefix index).
	 * Empty when the input has no word tokens.
	 */
	inline FString BuildFtsQuery(const FString& Input, const TCHAR* Op, bool bPrefix = false)
	{
		FString Out;
		for (const FString& Token : WordTokens(Input))
		{
			if (Out.Len() > 0) { Out += FString::Printf(TEXT(" %s "), Op); }
			Out += TEXT("\"") + Token + TEXT("\"");
			if (bPrefix) { Out += TEXT("*"); }
		}
		return Out;
	}

	/**
	 * Auxiliary search tokens for a chunk: identifier-style tokens (BP_PlayerCharacter,
	 * DirectionalLight) from the entity id's tail and the chunk's first line, split on
	 * underscores and camelCase humps — so "player" matches a chunk about BP_PlayerCharacter
	 * even though FTS tokenizes whole words. Deduped, bounded, single line.
	 */
	inline FString BuildAuxTokens(const FString& EntityId, const FString& Text)
	{
		FString Source = EntityId.Replace(TEXT("/"), TEXT(" ")).Replace(TEXT(":"), TEXT(" "));
		int32 Newline;
		const FString FirstLine = Text.FindChar(TEXT('\n'), Newline) ? Text.Left(Newline) : Text;
		Source += TEXT(" ") + FirstLine.Left(200);

		TSet<FString> Seen;
		TArray<FString> Parts;
		for (const FString& Token : WordTokens(Source))
		{
			// Only identifiers that actually split into multiple words earn an aux entry.
			const FString Split = SplitCamel(Token.Replace(TEXT("_"), TEXT(" "))).TrimStartAndEnd();
			if (Split == Token || Split.IsEmpty()) { continue; }
			if (Seen.Contains(Split)) { continue; }
			Seen.Add(Split);
			Parts.Add(Split);
			if (Parts.Num() >= 24) { break; }
		}
		return FString::Join(Parts, TEXT(" "));
	}
}
