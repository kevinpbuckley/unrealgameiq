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

private:
	/** Expose UGameIQService on UE 5.8's native ToolsetRegistry / MCP endpoint (needs GEditor). */
	void RegisterToolsets();

	/** Register the Tools > Game IQ panel (dockable tab + menu entry) and unregister on shutdown. */
	void RegisterUI();
	void UnregisterUI();
	TSharedRef<class SDockTab> OnSpawnGameIQTab(const class FSpawnTabArgs& Args);

	FDelegateHandle PostEngineInitHandle;
};
