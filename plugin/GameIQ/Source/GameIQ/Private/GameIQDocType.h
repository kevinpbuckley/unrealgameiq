// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "GameIQFileWalk.h" // LoadExcludes' JSON helpers / config location
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * Document-type taxonomy + classification (issue #4). "Documentation" is not one thing — a brand
 * guideline, a level layout, and a lore bible answer different questions and deserve type-specific
 * handling. Classification is cheap and deterministic: path/filename keyword heuristics first, with a
 * `gameiq.config.json` override (`docTypes: { "<path-prefix>": "<docType>" }`). Inline so the docs +
 * link commandlet TUs share it without ODR conflicts under unity builds.
 */
namespace GameIQDocs
{
	// The full range of documentation game studios actually produce. Kept as stable string tags so
	// the query layer + agents can filter (`Search(..., kind)` is entity-kind; docType lives in detail).
	namespace DocType
	{
		inline const TCHAR* GameDesign        = TEXT("game-design");
		inline const TCHAR* LevelDesign       = TEXT("level-design");
		inline const TCHAR* TechnicalDesign   = TEXT("technical-design");
		inline const TCHAR* UxUi              = TEXT("ux-ui");
		inline const TCHAR* BrandGuidelines   = TEXT("brand-guidelines");
		inline const TCHAR* ArtBible          = TEXT("art-bible");
		inline const TCHAR* AudioDesign       = TEXT("audio-design");
		inline const TCHAR* Narrative         = TEXT("narrative");
		inline const TCHAR* DialogueScript    = TEXT("dialogue-script");
		inline const TCHAR* Localization      = TEXT("localization");
		inline const TCHAR* Production        = TEXT("production");
		inline const TCHAR* QaTest            = TEXT("qa-test");
		inline const TCHAR* Postmortem        = TEXT("postmortem");
		inline const TCHAR* Onboarding        = TEXT("onboarding");
		inline const TCHAR* Accessibility     = TEXT("accessibility");
		inline const TCHAR* Legal             = TEXT("legal");
		inline const TCHAR* RatingsCompliance = TEXT("ratings-compliance");
		inline const TCHAR* Marketing         = TEXT("marketing");
		inline const TCHAR* MeetingNotes      = TEXT("meeting-notes");
		inline const TCHAR* Other             = TEXT("other");
	}

	/** True if `Hay` (already lowercased) contains any of the needles. */
	inline bool ContainsAny(const FString& Hay, std::initializer_list<const TCHAR*> Needles)
	{
		for (const TCHAR* N : Needles)
		{
			if (Hay.Contains(N)) { return true; }
		}
		return false;
	}

	/** Optional per-project overrides from gameiq.config.json: { "docTypes": { "Docs/Brand": "brand-guidelines" } }. */
	inline TMap<FString, FString> LoadDocTypeOverrides(const FString& Root)
	{
		TMap<FString, FString> Map;
		FString Json;
		if (FFileHelper::LoadFileToString(Json, *FPaths::Combine(Root, TEXT("gameiq.config.json"))))
		{
			TSharedPtr<FJsonObject> Obj;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			const TSharedPtr<FJsonObject>* Sub = nullptr;
			if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid() &&
				Obj->TryGetObjectField(TEXT("docTypes"), Sub) && Sub && Sub->IsValid())
			{
				for (const auto& Pair : (*Sub)->Values)
				{
					FString V;
					if (Pair.Value.IsValid() && Pair.Value->TryGetString(V) && !V.IsEmpty())
					{
						const FString Key = FString(Pair.Key).Replace(TEXT("\\"), TEXT("/")).TrimEnd();
						Map.Add(Key, V);
					}
				}
			}
		}
		return Map;
	}

	/**
	 * Classify a doc by its project-relative path. `Overrides` (from config) win; otherwise keyword
	 * heuristics run most-specific first so e.g. a brand style-guide isn't caught by the generic
	 * "design" bucket. Anything unmatched is `other` — still indexed, never dropped.
	 */
	inline FString Classify(const FString& RelPath, const TMap<FString, FString>& Overrides)
	{
		const FString Rel = RelPath.Replace(TEXT("\\"), TEXT("/"));
		for (const TPair<FString, FString>& O : Overrides)
		{
			if (Rel == O.Key || Rel.StartsWith(O.Key + TEXT("/")) || Rel.StartsWith(O.Key)) { return O.Value; }
		}

		const FString P = Rel.ToLower();
		const FString File = FPaths::GetCleanFilename(P);

		// Most-specific → least. Brand/art/audio before generic "design"; readme/onboarding early.
		if (ContainsAny(P, { TEXT("brand"), TEXT("logo"), TEXT("styleguide"), TEXT("style-guide"), TEXT("style_guide"), TEXT("visual-identity"), TEXT("visual identity") })) { return DocType::BrandGuidelines; }
		if (ContainsAny(P, { TEXT("artbible"), TEXT("art-bible"), TEXT("art_bible"), TEXT("art bible"), TEXT("art-style"), TEXT("art-direction"), TEXT("artdirection") })) { return DocType::ArtBible; }
		if (ContainsAny(P, { TEXT("audio"), TEXT("sound"), TEXT("music"), TEXT("sfx"), TEXT("wwise"), TEXT("fmod") })) { return DocType::AudioDesign; }
		if (ContainsAny(File, { TEXT("readme"), TEXT("contributing"), TEXT("onboarding"), TEXT("setup"), TEXT("getting-started"), TEXT("getting_started") }) ||
			ContainsAny(P, { TEXT("coding-standard"), TEXT("coding_standard"), TEXT("code-style"), TEXT("conventions") })) { return DocType::Onboarding; }
		if (ContainsAny(P, { TEXT("accessibility"), TEXT("a11y") })) { return DocType::Accessibility; }
		if (ContainsAny(P, { TEXT("esrb"), TEXT("pegi"), TEXT("rating"), TEXT("cert"), TEXT("compliance"), TEXT("age-rating") })) { return DocType::RatingsCompliance; }
		if (ContainsAny(P, { TEXT("legal"), TEXT("license"), TEXT("licence"), TEXT("eula"), TEXT("privacy"), TEXT("attribution"), TEXT("third-party"), TEXT("thirdparty") })) { return DocType::Legal; }
		if (ContainsAny(P, { TEXT("marketing"), TEXT("pitch"), TEXT("press"), TEXT("store-page"), TEXT("storepage"), TEXT("trailer"), TEXT("key-art"), TEXT("keyart"), TEXT("messaging") })) { return DocType::Marketing; }
		if (ContainsAny(P, { TEXT("localization"), TEXT("localisation"), TEXT("loc-"), TEXT("/loc/"), TEXT("glossary"), TEXT("i18n"), TEXT("l10n") })) { return DocType::Localization; }
		if (ContainsAny(P, { TEXT("dialogue"), TEXT("dialog"), TEXT("vo-script"), TEXT("vo_script"), TEXT("voiceover"), TEXT("barks") })) { return DocType::DialogueScript; }
		if (ContainsAny(P, { TEXT("narrative"), TEXT("lore"), TEXT("story"), TEXT("worldbuild"), TEXT("world-build"), TEXT("character-bio"), TEXT("characters"), TEXT("bible") })) { return DocType::Narrative; }
		if (ContainsAny(P, { TEXT("ux"), TEXT("/ui/"), TEXT("ui-"), TEXT("-ui"), TEXT("wireframe"), TEXT("hud"), TEXT("menu-flow"), TEXT("user-flow"), TEXT("userflow") })) { return DocType::UxUi; }
		if (ContainsAny(P, { TEXT("level-design"), TEXT("leveldesign"), TEXT("level_design"), TEXT("/levels/"), TEXT("/level/"), TEXT("layout"), TEXT("whitebox"), TEXT("greybox"), TEXT("blockout"), TEXT("beat-chart"), TEXT("pacing"), TEXT("encounter"), TEXT("lighting") })) { return DocType::LevelDesign; }
		if (ContainsAny(P, { TEXT("tdd"), TEXT("technical-design"), TEXT("technical_design"), TEXT("architecture"), TEXT("tech-design"), TEXT("systems-design") })) { return DocType::TechnicalDesign; }
		if (ContainsAny(P, { TEXT("postmortem"), TEXT("post-mortem"), TEXT("retro") })) { return DocType::Postmortem; }
		if (ContainsAny(P, { TEXT("qa"), TEXT("test-plan"), TEXT("testplan"), TEXT("triage"), TEXT("bug") })) { return DocType::QaTest; }
		if (ContainsAny(P, { TEXT("roadmap"), TEXT("milestone"), TEXT("sprint"), TEXT("production"), TEXT("schedule"), TEXT("backlog") })) { return DocType::Production; }
		if (ContainsAny(P, { TEXT("meeting"), TEXT("adr"), TEXT("decision-record"), TEXT("notes") })) { return DocType::MeetingNotes; }
		if (ContainsAny(P, { TEXT("gdd"), TEXT("game-design"), TEXT("game_design"), TEXT("design-doc"), TEXT("designdoc"), TEXT("feature-design"), TEXT("mechanic"), TEXT("one-pager"), TEXT("onepager") })) { return DocType::GameDesign; }
		if (P.Contains(TEXT("design"))) { return DocType::GameDesign; } // generic fallback within a design/ tree

		return DocType::Other;
	}
}
