// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * Minimal git access for per-commit snapshots (issue #36). Reading HEAD is file-only (no process
 * spawn), so the editor can poll it cheaply; resolving an arbitrary ref ("HEAD~3", a tag, a short
 * sha) shells out to `git` and is only done when a query asks for it.
 */
namespace GameIQGit
{
	/** The git directory for a work tree: <Root>/.git, or the target of a `gitdir:` file (linked worktree). */
	inline FString GitDir(const FString& Root)
	{
		const FString Dot = FPaths::Combine(Root, TEXT(".git"));
		if (IFileManager::Get().DirectoryExists(*Dot)) { return Dot; }
		FString Text;
		if (FFileHelper::LoadFileToString(Text, *Dot))
		{
			Text.TrimStartAndEndInline();
			if (Text.RemoveFromStart(TEXT("gitdir:")))
			{
				Text.TrimStartAndEndInline();
				return FPaths::IsRelative(Text) ? FPaths::ConvertRelativePathToFull(Root, Text) : Text;
			}
		}
		return FString();
	}

	/** Shared git dir (refs, packed-refs) — a linked worktree's `commondir`, else the git dir itself. */
	inline FString CommonDir(const FString& GitDirPath)
	{
		FString Common;
		if (FFileHelper::LoadFileToString(Common, *FPaths::Combine(GitDirPath, TEXT("commondir"))))
		{
			Common.TrimStartAndEndInline();
			return FPaths::IsRelative(Common) ? FPaths::ConvertRelativePathToFull(GitDirPath, Common) : Common;
		}
		return GitDirPath;
	}

	inline FString ReadRef(const FString& GitDirPath, const FString& RefName)
	{
		for (const FString& Dir : { GitDirPath, CommonDir(GitDirPath) })
		{
			FString Sha;
			if (FFileHelper::LoadFileToString(Sha, *FPaths::Combine(Dir, RefName)))
			{
				return Sha.TrimStartAndEnd();
			}
		}
		FString Packed;
		if (FFileHelper::LoadFileToString(Packed, *FPaths::Combine(CommonDir(GitDirPath), TEXT("packed-refs"))))
		{
			TArray<FString> Lines;
			Packed.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				FString Sha, Name;
				if (Line.Split(TEXT(" "), &Sha, &Name) && Name.TrimStartAndEnd() == RefName) { return Sha; }
			}
		}
		return FString();
	}

	/** Current HEAD commit and branch ("" when detached), read from files. False if not a git work tree. */
	inline bool ReadHead(const FString& Root, FString& OutSha, FString& OutBranch)
	{
		OutSha.Reset();
		OutBranch.Reset();
		const FString Dir = GitDir(Root);
		if (Dir.IsEmpty()) { return false; }
		FString Head;
		if (!FFileHelper::LoadFileToString(Head, *FPaths::Combine(Dir, TEXT("HEAD")))) { return false; }
		Head.TrimStartAndEndInline();
		if (Head.RemoveFromStart(TEXT("ref:")))
		{
			Head.TrimStartAndEndInline();
			OutBranch = Head;
			OutBranch.RemoveFromStart(TEXT("refs/heads/"));
			OutSha = ReadRef(Dir, Head);
		}
		else
		{
			OutSha = Head;
		}
		// A sha is passed to `git show` later: accept nothing but hex (a hand-edited or damaged
		// HEAD/ref file must not become a git argument).
		if (OutSha.Len() < 40) { return false; }
		for (const TCHAR C : OutSha)
		{
			if (!FChar::IsHexDigit(C)) { OutSha.Reset(); return false; }
		}
		return true;
	}

	/** Run `git <Args>` in Root; true on exit code 0. Output trimmed. */
	inline bool Run(const FString& Root, const FString& Args, FString& OutStdOut)
	{
		int32 ReturnCode = -1;
		FString StdErr;
		const FString FullArgs = FString::Printf(TEXT("-C \"%s\" %s"), *Root, *Args);
		if (!FPlatformProcess::ExecProcess(TEXT("git"), *FullArgs, &ReturnCode, &OutStdOut, &StdErr)) { return false; }
		OutStdOut.TrimStartAndEndInline();
		return ReturnCode == 0;
	}

	/** Full sha of a commit-ish ("HEAD~3", "v1.2", "abc123", "main"), or empty. Refuses option-like input. */
	inline FString ResolveCommit(const FString& Root, const FString& Ref)
	{
		if (Ref.IsEmpty() || Ref.StartsWith(TEXT("-")) || Ref.Contains(TEXT("\"")) || Ref.Contains(TEXT(" "))) { return FString(); }
		FString Out;
		return Run(Root, FString::Printf(TEXT("rev-parse --verify --quiet \"%s^{commit}\""), *Ref), Out) && Out.Len() >= 40
			? Out.Left(40) : FString();
	}

	/** Committer time of a commit, or MinValue. */
	inline FDateTime CommitTime(const FString& Root, const FString& Sha)
	{
		FString Out;
		FDateTime Time = FDateTime::MinValue();
		if (Run(Root, FString::Printf(TEXT("show -s --format=%%ct %s"), *Sha), Out) && Out.IsNumeric())
		{
			Time = FDateTime::FromUnixTimestamp(FCString::Atoi64(*Out));
		}
		return Time;
	}

	/** One-line subject of a commit (for snapshot names), or empty. */
	inline FString Subject(const FString& Root, const FString& Sha)
	{
		FString Out;
		return Run(Root, FString::Printf(TEXT("show -s --format=%%s %s"), *Sha), Out) ? Out.Left(120) : FString();
	}
}
