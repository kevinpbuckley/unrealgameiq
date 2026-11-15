// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

/**
 * Editor-less file walking for the C++ ports of the config/code extractors (design §5.1),
 * mirroring packages/core/src/util/walk.ts + config.ts so the ids/coverage match. Inline so
 * multiple commandlet TUs (merged in unity builds) can share it without ODR conflicts.
 */
namespace GameIQWalk
{
	/** Project-relative path with forward slashes, e.g. "Source/Foo/Bar.h". */
	inline FString RelPath(const FString& Root, const FString& Full)
	{
		FString Rel = Full;
		FPaths::MakePathRelativeTo(Rel, *(Root / TEXT("")));
		return Rel.Replace(TEXT("\\"), TEXT("/"));
	}

	/** gameiq.config.json `exclude` list + the always-excluded GameIQ plugin (config.ts parity). */
	inline TArray<FString> LoadExcludes(const FString& Root)
	{
		TArray<FString> Excludes;
		Excludes.Add(TEXT("Plugins/GameIQ")); // ALWAYS_EXCLUDE

		FString Json;
		if (FFileHelper::LoadFileToString(Json, *FPaths::Combine(Root, TEXT("gameiq.config.json"))))
		{
			TSharedPtr<FJsonObject> Obj;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid() && Obj->TryGetArrayField(TEXT("exclude"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					FString S;
					if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
					{
						Excludes.Add(S.Replace(TEXT("\\"), TEXT("/")).TrimEnd());
					}
				}
			}
		}
		return Excludes;
	}

	/**
	 * gameiq.config.json `shallowPaths`: package-path prefixes ("/Game/UltraDynamicSky") for
	 * third-party/marketplace content living inside /Game. Assets under a shallow path keep full
	 * Tier 0 registry coverage (identity + dependency graph) but skip deep extraction — no UObject
	 * load in the Assets stage, no Tier 2 pseudocode in the Blueprints stage. Both faster and
	 * better retrieval (vendor internals stop drowning search results).
	 */
	inline TArray<FString> LoadShallowPackagePaths(const FString& Root)
	{
		TArray<FString> Paths;
		FString Json;
		if (FFileHelper::LoadFileToString(Json, *FPaths::Combine(Root, TEXT("gameiq.config.json"))))
		{
			TSharedPtr<FJsonObject> Obj;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
			if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid() && Obj->TryGetArrayField(TEXT("shallowPaths"), Arr))
			{
				for (const TSharedPtr<FJsonValue>& V : *Arr)
				{
					FString S;
					if (V.IsValid() && V->TryGetString(S) && !S.IsEmpty())
					{
						S = S.Replace(TEXT("\\"), TEXT("/")).TrimStartAndEnd();
						while (S.EndsWith(TEXT("/"))) { S.LeftChopInline(1); }
						if (S.StartsWith(TEXT("/"))) { Paths.Add(S); }
					}
				}
			}
		}
		return Paths;
	}

	/**
	 * gameiq.config.json `docsPath`: folder the in-repo docs stage scans (project-relative or
	 * absolute). Empty = unset → scan the whole project tree (legacy behavior).
	 */
	inline FString LoadDocsPath(const FString& Root)
	{
		FString Json;
		if (FFileHelper::LoadFileToString(Json, *FPaths::Combine(Root, TEXT("gameiq.config.json"))))
		{
			TSharedPtr<FJsonObject> Obj;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
			FString S;
			if (FJsonSerializer::Deserialize(Reader, Obj) && Obj.IsValid() && Obj->TryGetStringField(TEXT("docsPath"), S))
			{
				return S.Replace(TEXT("\\"), TEXT("/")).TrimStartAndEnd();
			}
		}
		return FString();
	}

	/** True when a package name ("/Game/Foo/Bar") falls under one of the shallow path prefixes. */
	inline bool IsShallowPackage(const FString& PackageName, const TArray<FString>& ShallowPaths)
	{
		for (const FString& P : ShallowPaths)
		{
			if (PackageName == P || PackageName.StartsWith(P + TEXT("/"))) { return true; }
		}
		return false;
	}

	/** True when `S` matches a glob: `*` and `?` as in FString::MatchesWildcard (`*` also crosses
	 *  `/`, so `**` works as "any depth"). Case-insensitive, like the rest of the walk on Windows. */
	inline bool MatchesGlob(const FString& S, const FString& Pattern)
	{
		return S.MatchesWildcard(Pattern, ESearchCase::IgnoreCase);
	}

	/**
	 * `exclude` entries are a directory name ("VibeUE"), a project-relative path ("Plugins/VibeUE"),
	 * or a glob over the project-relative path or name ("Docs/Archive*", "Test?Data", "*.bak") — issue
	 * #30. A glob ending in a slash plus one or two stars also excludes the directory it names.
	 */
	inline bool IsExcluded(const FString& Name, const FString& Rel, const TArray<FString>& Excludes)
	{
		for (const FString& E : Excludes)
		{
			if (E.Contains(TEXT("*")) || E.Contains(TEXT("?")))
			{
				if (MatchesGlob(Rel, E) || MatchesGlob(Name, E)) { return true; }
				FString Base = E;
				if (Base.RemoveFromEnd(TEXT("/**")) || Base.RemoveFromEnd(TEXT("/*")))
				{
					if (MatchesGlob(Rel, Base) || MatchesGlob(Name, Base)) { return true; }
				}
				continue;
			}
			if (Name == E || Rel == E || Rel.StartsWith(E + TEXT("/"))) { return true; }
		}
		return false;
	}

	/**
	 * Directories the walk never enters (issue #30): build/cache output, every dot-directory
	 * (.git, .gameiq, .claude, .vs, .idea, .vscode — agent worktrees under .claude/worktrees were
	 * 46 of 105 indexed documents in one project), and any linked git worktree: a second copy of
	 * the project that would duplicate every entity. A linked worktree is a directory holding a
	 * `.git` FILE whose `gitdir:` points into a `worktrees/` folder. A git SUBMODULE also has a
	 * `.git` file (pointing into `modules/`), but it is real project content (plugins are often
	 * submodules), so it is kept, like a nested clone with a `.git` directory.
	 */
	inline bool IsLinkedWorktree(const FString& FullDir)
	{
		const FString GitMarker = FullDir / TEXT(".git");
		IFileManager& FM = IFileManager::Get();
		if (!FM.FileExists(*GitMarker) || FM.DirectoryExists(*GitMarker)) { return false; }
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *GitMarker)) { return false; }
		Text.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Text.TrimStart().StartsWith(TEXT("gitdir:")) && Text.Contains(TEXT("/worktrees/"));
	}

	inline bool ShouldSkipDirectory(const FString& FullDir, const FString& Name)
	{
		static const TArray<FString> Skip = {
			TEXT("Intermediate"), TEXT("Binaries"), TEXT("Saved"), TEXT("DerivedDataCache"),
			TEXT("node_modules") };
		if (Name.StartsWith(TEXT("."))) { return true; }
		if (Skip.Contains(Name)) { return true; }
		return IsLinkedWorktree(FullDir);
	}

	inline void Collect(const FString& Root, const FString& Dir, const TArray<FString>& LowerExts,
		const TArray<FString>& Excludes, TArray<FString>& Out)
	{
		IFileManager& FM = IFileManager::Get();

		TArray<FString> Files;
		FM.FindFiles(Files, *(Dir / TEXT("*.*")), /*Files=*/true, /*Directories=*/false);
		for (const FString& F : Files)
		{
			const FString Ext = FString(TEXT(".")) + FPaths::GetExtension(F).ToLower();
			if (!LowerExts.Contains(Ext)) { continue; }
			// File-level globs ("*.generated.h", "Docs/Archive/*") — directory entries never match a file name.
			if (IsExcluded(F, RelPath(Root, Dir / F), Excludes)) { continue; }
			Out.Add(Dir / F);
		}

		TArray<FString> Dirs;
		FM.FindFiles(Dirs, *(Dir / TEXT("*")), /*Files=*/false, /*Directories=*/true);
		for (const FString& D : Dirs)
		{
			const FString Full = Dir / D;
			if (ShouldSkipDirectory(Full, D)) { continue; }
			if (IsExcluded(D, RelPath(Root, Full), Excludes)) { continue; }
			Collect(Root, Full, LowerExts, Excludes, Out);
		}
	}

	/** Recursively list files under `Root` whose extension is in `LowerExts` (".h", ".ini", …). */
	inline TArray<FString> WalkFiles(const FString& Root, const TArray<FString>& LowerExts)
	{
		TArray<FString> Out;
		Collect(Root, Root, LowerExts, LoadExcludes(Root), Out);
		return Out;
	}
}
