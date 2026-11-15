// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "GameIQDocsExtract.h"
#include "GameIQFileWalk.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

/**
 * Editor-free project checks shared by Doctor, the editor subsystem and the agent-config command
 * (inline, GameIQFileWalk.h pattern). Everything here reads files only: no asset loads, no
 * Asset Registry, so the same answers come out of the query CLI (scripts/gameiq_query.py), which
 * mirrors this logic in Python.
 */

/** Docs staleness fingerprint (issue #29) — same idea as GameIQCppFingerprint, over doc files. */
namespace GameIQDocsFingerprint
{
	inline FString FingerprintPath(const FString& Root)
	{
		return FPaths::Combine(Root, TEXT(".gameiq"), TEXT("extract"), TEXT("docs.fingerprint"));
	}

	/** The folder the docs stage scans: gameiq.config.json `docsPath`, else the project root. */
	inline FString DocsRoot(const FString& Root)
	{
		const FString DocsPath = GameIQWalk::LoadDocsPath(Root);
		if (DocsPath.IsEmpty()) { return Root; }
		return FPaths::ConvertRelativePathToFull(FPaths::IsRelative(DocsPath) ? FPaths::Combine(Root, DocsPath) : DocsPath);
	}

	/** The doc files the docs stage would read (same walk, same Plugins/ and caption skips). */
	inline TArray<FString> DocFiles(const FString& Root)
	{
		const FString DocsRootDir = DocsRoot(Root);
		TArray<FString> Out;
		if (!IFileManager::Get().DirectoryExists(*DocsRootDir)) { return Out; }
		for (const FString& File : GameIQWalk::WalkFiles(DocsRootDir, GameIQDocsExtract::DocExts()))
		{
			const FString Rel = GameIQWalk::RelPath(DocsRootDir, File);
			if (Rel.StartsWith(TEXT("Plugins/")) || Rel.EndsWith(TEXT(".caption.txt"))) { continue; }
			Out.Add(File);
		}
		Out.Sort();
		return Out;
	}

	/** MD5 of (relpath|size|mtime) over the doc files. Stat-only. */
	inline FString Compute(const FString& Root)
	{
		const FString DocsRootDir = DocsRoot(Root);
		IFileManager& FM = IFileManager::Get();
		FString Manifest;
		for (const FString& File : DocFiles(Root))
		{
			const FFileStatData Stat = FM.GetStatData(*File);
			Manifest += FString::Printf(TEXT("%s|%lld|%lld\n"), *GameIQWalk::RelPath(DocsRootDir, File),
				Stat.FileSize, Stat.ModificationTime.GetTicks());
		}
		return FMD5::HashAnsiString(*Manifest);
	}

	inline FString ReadStored(const FString& Root)
	{
		FString Stored;
		FFileHelper::LoadFileToString(Stored, *FingerprintPath(Root));
		return Stored.TrimStartAndEnd();
	}

	inline void Write(const FString& Root, const FString& Fingerprint)
	{
		FFileHelper::SaveStringToFile(Fingerprint, *FingerprintPath(Root), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	/** True when a changed file (absolute path) is one the docs stage reads — the watcher filter. */
	inline bool IsDocFile(const FString& Root, const FString& AbsFile)
	{
		const FString Ext = FString(TEXT(".")) + FPaths::GetExtension(AbsFile).ToLower();
		if (!GameIQDocsExtract::DocExts().Contains(Ext)) { return false; }
		const FString DocsRootDir = DocsRoot(Root);
		FString Rel = AbsFile.Replace(TEXT("\\"), TEXT("/"));
		const FString Prefix = DocsRootDir.Replace(TEXT("\\"), TEXT("/")) / TEXT("");
		if (!Rel.StartsWith(Prefix, ESearchCase::IgnoreCase)) { return false; }
		Rel = Rel.Mid(Prefix.Len());
		if (Rel.StartsWith(TEXT("Plugins/")) || Rel.EndsWith(TEXT(".caption.txt"))) { return false; }
		// Same directory rules as the walk: dot-dirs, build output and config excludes never count.
		TArray<FString> Parts;
		Rel.ParseIntoArray(Parts, TEXT("/"));
		const TArray<FString> Excludes = GameIQWalk::LoadExcludes(DocsRootDir);
		FString Walked = DocsRootDir;
		for (int32 i = 0; i + 1 < Parts.Num(); ++i)
		{
			Walked = Walked / Parts[i];
			if (GameIQWalk::ShouldSkipDirectory(Walked, Parts[i])) { return false; }
			if (GameIQWalk::IsExcluded(Parts[i], GameIQWalk::RelPath(DocsRootDir, Walked), Excludes)) { return false; }
		}
		return true;
	}
}

/** Disk-versus-index gap (issue #35): packages under Content/ against indexed asset ids. */
namespace GameIQDiskGap
{
	/** "/Game/..." package names of every .uasset/.umap under <Root>/Content, minus OFPA packages. */
	inline TSet<FString> DiskPackages(const FString& Root)
	{
		TSet<FString> Out;
		const FString ContentDir = FPaths::Combine(Root, TEXT("Content"));
		if (!IFileManager::Get().DirectoryExists(*ContentDir)) { return Out; }
		for (const TCHAR* Pattern : { TEXT("*.uasset"), TEXT("*.umap") })
		{
			TArray<FString> Files;
			IFileManager::Get().FindFilesRecursive(Files, *ContentDir, Pattern, /*Files=*/true, /*Directories=*/false);
			for (const FString& File : Files)
			{
				FString Rel = GameIQWalk::RelPath(ContentDir, File);
				if (Rel.Contains(TEXT("__ExternalActors__/")) || Rel.Contains(TEXT("__ExternalObjects__/"))) { continue; }
				const FString Dir = FPaths::GetPath(Rel);
				Out.Add(Dir.IsEmpty()
					? FString(TEXT("/Game/")) + FPaths::GetBaseFilename(Rel)
					: FString(TEXT("/Game/")) + Dir + TEXT("/") + FPaths::GetBaseFilename(Rel));
			}
		}
		return Out;
	}

	/** Folder bucket for the report: two levels under /Game ("/Game/Adventure_Pack/Meshes"). */
	inline FString FolderOf(const FString& Package)
	{
		TArray<FString> Parts;
		Package.ParseIntoArray(Parts, TEXT("/"));
		if (Parts.Num() >= 4) { return FString::Printf(TEXT("/%s/%s/%s"), *Parts[0], *Parts[1], *Parts[2]); }
		if (Parts.Num() >= 3) { return FString::Printf(TEXT("/%s/%s"), *Parts[0], *Parts[1]); }
		return TEXT("/Game");
	}

	inline TSharedRef<FJsonObject> Compare(const FString& Root, const TSet<FString>& IndexedPackages)
	{
		const TSet<FString> OnDisk = DiskPackages(Root);
		TMap<FString, int32> MissingByFolder;
		TArray<FString> MissingSample;
		int32 Missing = 0;
		for (const FString& P : OnDisk)
		{
			if (IndexedPackages.Contains(P)) { continue; }
			++Missing;
			MissingByFolder.FindOrAdd(FolderOf(P))++;
			if (MissingSample.Num() < 20) { MissingSample.Add(P); }
		}
		TArray<FString> GoneSample;
		int32 Gone = 0;
		// Only meaningful when the disk walk saw content at all (a DB moved away from its project
		// would otherwise report every asset as gone).
		if (OnDisk.Num() > 0)
		{
			for (const FString& P : IndexedPackages)
			{
				if (OnDisk.Contains(P)) { continue; }
				if (P.Contains(TEXT("/__ExternalActors__/")) || P.Contains(TEXT("/__ExternalObjects__/"))) { continue; }
				++Gone;
				if (GoneSample.Num() < 20) { GoneSample.Add(P); }
			}
		}
		MissingByFolder.ValueSort([](int32 A, int32 B) { return A > B; });

		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetNumberField(TEXT("diskAssets"), OnDisk.Num());
		Out->SetNumberField(TEXT("indexedAssets"), IndexedPackages.Num());
		Out->SetNumberField(TEXT("missingFromIndex"), Missing);
		Out->SetNumberField(TEXT("goneFromDisk"), Gone);
		TArray<TSharedPtr<FJsonValue>> Folders;
		for (const TPair<FString, int32>& F : MissingByFolder)
		{
			if (Folders.Num() >= 25) { break; }
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("folder"), F.Key);
			Row->SetNumberField(TEXT("missing"), F.Value);
			Folders.Add(MakeShared<FJsonValueObject>(Row));
		}
		Out->SetArrayField(TEXT("missingByFolder"), Folders);
		auto ToJson = [](const TArray<FString>& A)
		{
			TArray<TSharedPtr<FJsonValue>> V;
			for (const FString& S : A) { V.Add(MakeShared<FJsonValueString>(S)); }
			return V;
		};
		Out->SetArrayField(TEXT("missingSample"), ToJson(MissingSample));
		Out->SetArrayField(TEXT("goneSample"), ToJson(GoneSample));
		return Out;
	}
}

/** Agent files and .gitignore (issues #26, #37). */
namespace GameIQAgentFiles
{
	inline const TCHAR* BeginMarker() { return TEXT("<!-- BEGIN GameIQ agent guidance -->"); }
	inline const TCHAR* EndMarker() { return TEXT("<!-- END GameIQ agent guidance -->"); }

	inline const TArray<FString>& AgentFileNames()
	{
		static const TArray<FString> Names = {
			TEXT("CLAUDE.md"), TEXT("AGENTS.md"), TEXT("GEMINI.md"), TEXT(".github/copilot-instructions.md") };
		return Names;
	}

	inline bool IsGitRepo(const FString& Root)
	{
		const FString Git = FPaths::Combine(Root, TEXT(".git"));
		return IFileManager::Get().DirectoryExists(*Git) || IFileManager::Get().FileExists(*Git);
	}

	/** True when a .gitignore line ignores the .gameiq folder (".gameiq", "/.gameiq/", ".gameiq/(double star)"). */
	inline bool GitignoreCoversIndex(const FString& Root)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Root, TEXT(".gitignore")))) { return false; }
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		for (FString L : Lines)
		{
			L.TrimStartAndEndInline();
			L.RemoveFromStart(TEXT("/"));
			L.RemoveFromEnd(TEXT("/**"));
			L.RemoveFromEnd(TEXT("/*"));
			L.RemoveFromEnd(TEXT("/"));
			if (L == TEXT(".gameiq") || L == TEXT("**/.gameiq")) { return true; }
		}
		return false;
	}

	/** Append ".gameiq/" to <Root>/.gitignore when the project is a git repo and it's not covered.
	 *  Returns true if the file was changed. */
	inline bool EnsureGitignore(const FString& Root)
	{
		if (!IsGitRepo(Root) || GitignoreCoversIndex(Root)) { return false; }
		const FString Path = FPaths::Combine(Root, TEXT(".gitignore"));
		FString Text;
		FFileHelper::LoadFileToString(Text, *Path);
		if (!Text.IsEmpty() && !Text.EndsWith(TEXT("\n"))) { Text += TEXT("\n"); }
		Text += TEXT("\n# Unreal Game IQ index (rebuilt locally; never commit)\n.gameiq/\n");
		return FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	inline TSharedRef<FJsonObject> Check(const FString& Root)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Files;
		bool bPresent = false;
		for (const FString& Name : AgentFileNames())
		{
			const FString Path = FPaths::Combine(Root, Name);
			FString Text;
			if (!FFileHelper::LoadFileToString(Text, *Path)) { continue; }
			// An @import line pointing at the sample counts too (GenerateAgentConfig ... import).
			const bool bHas = Text.Contains(BeginMarker()) || Text.Contains(TEXT("AGENTS.md.sample"));
			bPresent |= bHas;
			TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
			Row->SetStringField(TEXT("file"), Name);
			Row->SetBoolField(TEXT("hasGameIQGuide"), bHas);
			Files.Add(MakeShared<FJsonValueObject>(Row));
		}
		Out->SetArrayField(TEXT("agentFiles"), Files);
		Out->SetBoolField(TEXT("present"), bPresent);
		Out->SetBoolField(TEXT("gitRepo"), IsGitRepo(Root));
		Out->SetBoolField(TEXT("gitignoresIndex"), GitignoreCoversIndex(Root));
		return Out;
	}
}
