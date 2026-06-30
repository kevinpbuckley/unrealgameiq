// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQModule.h"

#define LOCTEXT_NAMESPACE "FGameIQModule"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQ, Log, All);

void FGameIQModule::StartupModule()
{
	UE_LOG(LogGameIQ, Log, TEXT("Game IQ module started. Run `-run=GameIQExport` to export the index."));
}

void FGameIQModule::ShutdownModule()
{
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FGameIQModule, GameIQ)
