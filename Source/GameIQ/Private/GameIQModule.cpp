// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQModule.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "GameIQBuildRunner.h"
#include "GameIQPanel.h"
#include "GameIQToolset.h"
#include "HAL/FileManager.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
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

static FString FindGameIQPluginDir()
{
	IPluginManager& PluginManager = IPluginManager::Get();
	TSharedPtr<IPlugin> Plugin = PluginManager.FindPlugin(TEXT("GameIQ"));
	if (Plugin.IsValid())
	{
		return Plugin->GetBaseDir();
	}

	TArray<FString> SearchPaths;
	SearchPaths.Add(FPaths::ProjectPluginsDir() / TEXT("GameIQ"));
	SearchPaths.Add(FPaths::ProjectPluginsDir() / TEXT("unrealgameiq") / TEXT("plugin") / TEXT("GameIQ"));
	SearchPaths.Add(FPaths::EnginePluginsDir() / TEXT("Marketplace") / TEXT("GameIQ"));
	SearchPaths.Add(FPaths::EnginePluginsDir() / TEXT("GameIQ"));

	for (const FString& SearchPath : SearchPaths)
	{
		const FString AbsPath = FPaths::ConvertRelativePathToFull(SearchPath);
		if (FPaths::DirectoryExists(AbsPath) && FPaths::FileExists(AbsPath / TEXT("GameIQ.uplugin")))
		{
			return AbsPath;
		}
	}

	return FString();
}

static void GenerateGameIQAgentConfig(const TArray<FString>& Args, FOutputDevice& Ar)
{
	const FString Client = (Args.Num() > 0) ? Args[0].ToLower() : TEXT("all");

	bool bImportRequested = false;
	for (int32 i = 1; i < Args.Num(); ++i)
	{
		const FString A = Args[i].ToLower();
		if (A == TEXT("import") || A == TEXT("link") || A == TEXT("-import") || A == TEXT("--import"))
		{
			bImportRequested = true;
		}
	}

	const FString PluginDir = FindGameIQPluginDir();
	if (PluginDir.IsEmpty())
	{
		Ar.Log(TEXT("GameIQ.GenerateAgentConfig: ERROR — could not locate the GameIQ plugin."));
		return;
	}

	const FString SamplePath = FPaths::ConvertRelativePathToFull(
		FPaths::Combine(PluginDir, TEXT("Content"), TEXT("samples"), TEXT("AGENTS.md.sample")));
	FString SampleContent;
	if (!FFileHelper::LoadFileToString(SampleContent, *SamplePath))
	{
		Ar.Logf(TEXT("GameIQ.GenerateAgentConfig: ERROR — could not read sample at %s"), *SamplePath);
		return;
	}

	TArray<TPair<FString, bool>> Targets;
	if (Client == TEXT("claude") || Client == TEXT("claudecode"))
	{
		Targets.Add(TPair<FString, bool>(TEXT("CLAUDE.md"), true));
	}
	else if (Client == TEXT("gemini"))
	{
		Targets.Add(TPair<FString, bool>(TEXT("GEMINI.md"), true));
	}
	else if (Client == TEXT("copilot"))
	{
		Targets.Add(TPair<FString, bool>(TEXT(".github/copilot-instructions.md"), false));
	}
	else if (Client == TEXT("codex") || Client == TEXT("cursor") || Client == TEXT("agents") || Client == TEXT("agent"))
	{
		Targets.Add(TPair<FString, bool>(TEXT("AGENTS.md"), false));
	}
	else if (Client == TEXT("all"))
	{
		Targets.Add(TPair<FString, bool>(TEXT("CLAUDE.md"), true));
		Targets.Add(TPair<FString, bool>(TEXT("GEMINI.md"), true));
		Targets.Add(TPair<FString, bool>(TEXT("AGENTS.md"), false));
	}
	else
	{
		Ar.Logf(TEXT("GameIQ.GenerateAgentConfig: unknown client '%s'. Use: ClaudeCode | Gemini | Codex | Cursor | Copilot | All."), *Client);
		return;
	}

	const FString BeginMarker = TEXT("<!-- BEGIN GameIQ agent guidance -->");
	const FString EndMarker = TEXT("<!-- END GameIQ agent guidance -->");

	// Older bundled samples carried the guidance markers themselves; strip any embedded copies so
	// the block (wrapped in markers below) never nests them — nested markers break the refresh
	// logic, which replaces from the first BEGIN to the first END and strands the extras.
	SampleContent.ReplaceInline(*BeginMarker, TEXT(""));
	SampleContent.ReplaceInline(*EndMarker, TEXT(""));
	SampleContent.TrimStartAndEndInline();
	SampleContent += TEXT("\n");

	const FString ProjectRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	IFileManager& FileManager = IFileManager::Get();

	for (const TPair<FString, bool>& Target : Targets)
	{
		const FString& FileName = Target.Key;
		const bool bSupportsImport = Target.Value;
		const bool bUseImport = bImportRequested && bSupportsImport;
		if (bImportRequested && !bSupportsImport)
		{
			Ar.Logf(TEXT("GameIQ.GenerateAgentConfig: %s does not resolve @-imports — copying the guide in instead."), *FileName);
		}

		FString Body;
		if (bUseImport)
		{
			Body = FString::Printf(TEXT("@%s\n"), *SamplePath);
		}
		else
		{
			Body = SampleContent;
			if (!Body.EndsWith(TEXT("\n")))
			{
				Body += TEXT("\n");
			}
		}

		const FString Block = BeginMarker + TEXT("\n") + Body + EndMarker + TEXT("\n");
		const FString FullPath = FPaths::Combine(ProjectRoot, FileName);

		FString NewContent;
		FString Existing;
		if (FFileHelper::LoadFileToString(Existing, *FullPath))
		{
			const int32 BeginIdx = Existing.Find(TEXT("<!-- BEGIN GameIQ"), ESearchCase::IgnoreCase, ESearchDir::FromStart);
			if (BeginIdx != INDEX_NONE)
			{
				int32 EndIdx = Existing.Find(*EndMarker, ESearchCase::IgnoreCase, ESearchDir::FromStart, BeginIdx);
				if (EndIdx != INDEX_NONE)
				{
					EndIdx += EndMarker.Len();
					if (EndIdx < Existing.Len() && Existing[EndIdx] == TEXT('\n'))
					{
						++EndIdx;
					}
					NewContent = Existing.Left(BeginIdx) + Block + Existing.RightChop(EndIdx);
				}
				else
				{
					NewContent = Existing.Left(BeginIdx) + Block;
				}
				Ar.Logf(TEXT("GameIQ.GenerateAgentConfig: refreshed GameIQ block in %s"), *FullPath);
			}
			else
			{
				NewContent = Existing;
				if (!NewContent.EndsWith(TEXT("\n")))
				{
					NewContent += TEXT("\n");
				}
				NewContent += TEXT("\n") + Block;
				Ar.Logf(TEXT("GameIQ.GenerateAgentConfig: appended GameIQ block to existing %s"), *FullPath);
			}
		}
		else
		{
			NewContent = Block;
			Ar.Logf(TEXT("GameIQ.GenerateAgentConfig: created %s (%s)"), *FullPath, bUseImport ? TEXT("import") : TEXT("copy"));
		}

		FileManager.MakeDirectory(*FPaths::GetPath(FullPath), true);
		if (!FFileHelper::SaveStringToFile(NewContent, *FullPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			Ar.Logf(TEXT("GameIQ.GenerateAgentConfig: ERROR — failed to write %s"), *FullPath);
		}
	}

	Ar.Logf(TEXT("GameIQ.GenerateAgentConfig: done (source: %s)."), *SamplePath);
}

static FAutoConsoleCommandWithArgsAndOutputDevice GenerateAgentConfigCommand(
	TEXT("GameIQ.GenerateAgentConfig"),
	TEXT("Write the GameIQ agent guide to the project root from the bundled sample. ")
	TEXT("Usage: GameIQ.GenerateAgentConfig [ClaudeCode|Gemini|Codex|Cursor|Copilot|All] [import]. ")
	TEXT("Default All -> CLAUDE.md + GEMINI.md + AGENTS.md. 'import' writes a one-line @import for Claude/Gemini (others copy)."),
	FConsoleCommandWithArgsAndOutputDeviceDelegate::CreateStatic(GenerateGameIQAgentConfig)
);

static void GameIQRebuildConsoleCommand(const TArray<FString>& Args)
{
	// "full" bypasses the Assets/Blueprints incremental-hash cache and forces a from-scratch rebuild.
	const bool bFull = Args.ContainsByPredicate([](const FString& A) { return A.Equals(TEXT("full"), ESearchCase::IgnoreCase); });
	FGameIQBuildRunner::Get().StartBuild(TEXT("GameIQBuild"),
		NSLOCTEXT("GameIQModule", "ConsoleRebuildStarted", "Game IQ: rebuilding the full index in the background…"),
		bFull ? TEXT("-full") : FString());
}

static void GameIQReindexDocsConsoleCommand()
{
	FGameIQBuildRunner::Get().StartBuild(TEXT("GameIQDocsBuild"),
		NSLOCTEXT("GameIQModule", "ConsoleReindexDocsStarted", "Game IQ: reindexing documents in the background…"));
}

static FAutoConsoleCommand GameIQRebuildCommand(
	TEXT("GameIQ.Rebuild"),
	TEXT("Rebuild the full Game IQ project index in the background (same as the Tools > Game IQ panel's ")
	TEXT("Rebuild Index button). Progress shows as an editor notification, in the Tools > Game IQ panel ")
	TEXT("if open, and in the Output Log under LogGameIQRunner. Usage: GameIQ.Rebuild [full] — 'full' bypasses ")
	TEXT("the Assets/Blueprints incremental-hash cache and reprocesses every asset from scratch."),
	FConsoleCommandWithArgsDelegate::CreateStatic(GameIQRebuildConsoleCommand)
);

static FAutoConsoleCommand GameIQReindexDocsCommand(
	TEXT("GameIQ.ReindexDocs"),
	TEXT("Reindex only Game IQ documentation (docs, images, external connectors) and refresh links, ")
	TEXT("leaving code/asset entities untouched. Much faster than GameIQ.Rebuild."),
	FConsoleCommandDelegate::CreateStatic(GameIQReindexDocsConsoleCommand)
);

static void GameIQReindexCodeConsoleCommand()
{
	FGameIQBuildRunner::Get().StartBuild(TEXT("GameIQCppBuild"),
		NSLOCTEXT("GameIQModule", "ConsoleReindexCodeStarted", "Game IQ: reindexing C++ in the background…"));
}

static FAutoConsoleCommand GameIQReindexCodeCommand(
	TEXT("GameIQ.ReindexCode"),
	TEXT("Reindex only Game IQ C++ entities (code extractor, producer-scoped ingest, link refresh), ")
	TEXT("leaving asset/Blueprint/docs entities untouched. Also runs automatically on editor startup and ")
	TEXT("after Live Coding patches when sources changed (Project Settings > Plugins > Game IQ)."),
	FConsoleCommandDelegate::CreateStatic(GameIQReindexCodeConsoleCommand)
);

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
