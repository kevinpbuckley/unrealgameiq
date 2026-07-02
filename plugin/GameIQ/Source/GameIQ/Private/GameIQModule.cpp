// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQModule.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GameIQPanel.h"
#include "GameIQToolset.h"
#include "Misc/CoreDelegates.h"
#include "Styling/AppStyle.h"
#include "Textures/SlateIcon.h"
#include "ToolMenus.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "Widgets/Docking/SDockTab.h"
#include "WorkspaceMenuStructure.h"
#include "WorkspaceMenuStructureModule.h"

#define LOCTEXT_NAMESPACE "FGameIQModule"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQ, Log, All);

static const FName GameIQTabName("GameIQPanel");

void FGameIQModule::StartupModule()
{
	UE_LOG(LogGameIQ, Log, TEXT("Game IQ module started. Run `-run=GameIQBuild` to build the index."));

	// The ToolsetRegistry + editor UI need the editor up. Register now if it already is (hot-reload /
	// late load), otherwise wait for PostEngineInit.
	if (UToolsetRegistry::IsAvailable())
	{
		RegisterToolsets();
		RegisterUI();
	}
	else
	{
		PostEngineInitHandle = FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FGameIQModule::RegisterToolsets);
	}
}

void FGameIQModule::RegisterToolsets()
{
	if (UToolsetRegistry::IsAvailable())
	{
		UToolsetRegistry::RegisterToolsetClass(UGameIQService::StaticClass());
		UE_LOG(LogGameIQ, Display, TEXT("Game IQ: registered GameIQService on the ToolsetRegistry / MCP endpoint."));
	}
	else
	{
		UE_LOG(LogGameIQ, Warning, TEXT("Game IQ: ToolsetRegistry unavailable; GameIQService not exposed on MCP."));
	}

	// Deferred-load path: PostEngineInit fired, so the editor UI is safe to register now too.
	RegisterUI();
}

TSharedRef<SDockTab> FGameIQModule::OnSpawnGameIQTab(const FSpawnTabArgs& /*Args*/)
{
	return SNew(SDockTab)
		.TabRole(ETabRole::NomadTab)
		[
			SNew(SGameIQPanel)
		];
}

void FGameIQModule::RegisterUI()
{
	if (!FSlateApplication::IsInitialized() || !UToolMenus::IsToolMenuUIEnabled()) { return; }
	if (FGlobalTabmanager::Get()->HasTabSpawner(GameIQTabName)) { return; } // already registered

	FGlobalTabmanager::Get()->RegisterNomadTabSpawner(GameIQTabName,
		FOnSpawnTab::CreateRaw(this, &FGameIQModule::OnSpawnGameIQTab))
		.SetDisplayName(LOCTEXT("TabTitle", "Game IQ"))
		.SetTooltipText(LOCTEXT("TabTooltip", "Game IQ project index: stats and rebuild."))
		.SetGroup(WorkspaceMenu::GetMenuStructure().GetToolsCategory())
		.SetIcon(FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"));

	// Add a Tools > Game IQ menu entry that opens the tab.
	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools");
	if (ToolsMenu)
	{
		FToolMenuSection& Section = ToolsMenu->FindOrAddSection("Programming");
		Section.AddMenuEntry(
			"OpenGameIQ",
			LOCTEXT("MenuEntry", "Game IQ"),
			LOCTEXT("MenuEntryTip", "Open the Game IQ index panel (stats + rebuild)."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), "Icons.Search"),
			FUIAction(FExecuteAction::CreateLambda([]()
			{
				FGlobalTabmanager::Get()->TryInvokeTab(GameIQTabName);
			})));
	}

	UE_LOG(LogGameIQ, Display, TEXT("Game IQ: registered Tools > Game IQ panel."));
}

void FGameIQModule::UnregisterUI()
{
	if (FSlateApplication::IsInitialized() && FGlobalTabmanager::Get()->HasTabSpawner(GameIQTabName))
	{
		FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(GameIQTabName);
	}
	if (UObjectInitialized() && UToolMenus::Get())
	{
		UToolMenus::Get()->RemoveEntry("LevelEditor.MainMenu.Tools", "Programming", "OpenGameIQ");
	}
}

void FGameIQModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	UnregisterUI();
	if (UToolsetRegistry::IsAvailable())
	{
		UToolsetRegistry::UnregisterToolsetClass(UGameIQService::StaticClass());
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGameIQModule, GameIQ)
