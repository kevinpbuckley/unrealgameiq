// Copyright Buckley Builds LLC. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

/**
 * Game IQ editor module. Hosts the GameIQExport commandlet (headless Tier 0
 * extraction) and, later, the in-editor bridge that pushes live saves to the
 * index. Editor-only — never cooked into a packaged game (design §10).
 */
class FGameIQModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;
};
