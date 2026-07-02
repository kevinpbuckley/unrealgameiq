// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQModule.h"

#include "GameIQToolset.h"
#include "Misc/CoreDelegates.h"
#include "ToolsetRegistry/UToolsetRegistry.h"

#define LOCTEXT_NAMESPACE "FGameIQModule"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQ, Log, All);

void FGameIQModule::StartupModule()
{
	UE_LOG(LogGameIQ, Log, TEXT("Game IQ module started. Run `-run=GameIQExport` to export the index."));

	// The ToolsetRegistry needs the editor up. Register now if it already is (hot-reload / late load),
	// otherwise wait for PostEngineInit.
	if (UToolsetRegistry::IsAvailable())
	{
		RegisterToolsets();
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
}

void FGameIQModule::ShutdownModule()
{
	if (PostEngineInitHandle.IsValid())
	{
		FCoreDelegates::GetOnPostEngineInit().Remove(PostEngineInitHandle);
		PostEngineInitHandle.Reset();
	}
	if (UToolsetRegistry::IsAvailable())
	{
		UToolsetRegistry::UnregisterToolsetClass(UGameIQService::StaticClass());
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGameIQModule, GameIQ)
