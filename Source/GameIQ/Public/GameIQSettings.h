// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "GameIQSettings.generated.h"

/**
 * Editor-facing settings for Game IQ, shown under Project Settings > Plugins > Game IQ.
 * Editing these writes <Project>/gameiq.config.json, which the (editor-less) Game IQ core
 * reads when it builds the index — so a team can keep third-party plugins out of "your game".
 */
UCLASS(config = Editor, defaultconfig, meta = (DisplayName = "Game IQ"))
class UGameIQSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	/**
	 * Directories excluded from the index. Project-relative (e.g. "Plugins/VibeUE") or a bare
	 * directory name (e.g. "VibeUE"). Use this to keep third-party plugin source/content out of
	 * the index so it reflects your game, not vendored plugins. (Game IQ's own plugin is always
	 * excluded.) Written to <Project>/gameiq.config.json.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Indexing")
	TArray<FString> ExcludeDirectories;

	/**
	 * Folder the in-repo docs stage scans for design docs (.md, .txt, .html/.htm, .pdf, …), project-relative
	 * (e.g. "Docs") or absolute. Empty = the whole project tree (legacy behavior). Point it at a
	 * dedicated docs folder to stop READMEs, tool notes and vendored text from entering the index.
	 * Written to <Project>/gameiq.config.json as `docsPath`.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Indexing")
	FString DocsDirectory;

	/**
	 * Keep the C++ side of the index fresh automatically: on editor startup (and after each Live
	 * Coding patch) Game IQ compares the source tree against the fingerprint of the last code
	 * extraction and runs a background C++-only reindex when they differ. Disable to refresh code
	 * only via GameIQ.Rebuild / GameIQ.ReindexCode.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Indexing")
	bool bAutoReindexCpp = true;

	/**
	 * Keep design docs fresh automatically: the docs folder (Docs Directory, or the project tree) is
	 * watched, and a change to a doc file (or a mismatch found at editor startup) runs a background
	 * docs-only reindex. Disable to refresh docs only via GameIQ.ReindexDocs.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Indexing")
	bool bAutoReindexDocs = true;

	/**
	 * Shortest time between two automatic docs reindexes started by the docs watcher. Each reindex
	 * runs a headless UnrealEditor-Cmd process next to the editor, so a project whose docs change
	 * every few seconds (agents writing notes) must not start one per change: changes that arrive
	 * inside the interval are picked up by one reindex when it ends.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Indexing", meta = (ClampMin = "10", ClampMax = "86400", Units = "s"))
	int32 AutoReindexDocsMinIntervalSeconds = 300;

	/**
	 * On editor startup (once the Asset Registry has scanned), add every /Game asset that is on disk
	 * but missing from the index (marketplace packs, a VCS sync — anything that arrived without an
	 * editor save) as registry-level entries with size facts and dependencies, and drop entries whose
	 * package is gone. Deep recipes and Blueprint pseudocode still need GameIQ.Rebuild.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Indexing")
	bool bCatchUpOnStartup = true;

	/**
	 * Take an automatic snapshot of the index each time git HEAD moves (checked every 30 s while
	 * the editor is open), so Changes can answer "what changed since <commit>". Only the newest
	 * Max Auto Snapshots are kept.
	 */
	UPROPERTY(config, EditAnywhere, Category = "Snapshots")
	bool bSnapshotPerCommit = true;

	UPROPERTY(config, EditAnywhere, Category = "Snapshots", meta = (ClampMin = "1", ClampMax = "500"))
	int32 MaxAutoSnapshots = 20;

	//~ UDeveloperSettings — surface under the "Plugins" category
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

#if WITH_EDITOR
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Serialize the current exclude list to <Project>/gameiq.config.json (UTF-8, no BOM). */
	void WriteConfigJson() const;
};
