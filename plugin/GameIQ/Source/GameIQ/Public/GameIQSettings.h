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

	//~ UDeveloperSettings — surface under the "Plugins" category
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

#if WITH_EDITOR
	virtual void PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	/** Serialize the current exclude list to <Project>/gameiq.config.json (UTF-8, no BOM). */
	void WriteConfigJson() const;
};
