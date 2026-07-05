// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQBuildRunner.h"

#include "Framework/Notifications/NotificationManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/Paths.h"
#include "Widgets/Notifications/SNotificationList.h"

// Live-streamed lines from a background build (mirrors the child commandlet's own log categories,
// which all start with "LogGameIQ" — see the substring match in AppendOutput below).
DEFINE_LOG_CATEGORY_STATIC(LogGameIQRunner, Log, All);

namespace
{
	constexpr int32 MaxLogLines = 40;

	void Notify(const FText& Text, SNotificationItem::ECompletionState State)
	{
		FNotificationInfo Info(Text);
		Info.ExpireDuration = 4.0f;
		if (TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info))
		{
			Item->SetCompletionState(State);
		}
	}
}

FGameIQBuildRunner& FGameIQBuildRunner::Get()
{
	static FGameIQBuildRunner Instance;
	return Instance;
}

FGameIQBuildRunner::~FGameIQBuildRunner()
{
	if (PollHandle.IsValid())
	{
		FTSTicker::GetCoreTicker().RemoveTicker(PollHandle);
	}
	if (Proc.IsValid())
	{
		FPlatformProcess::CloseProc(Proc); // don't kill the build; just release our handle
	}
	if (ReadPipe || WritePipe)
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
	}
}

void FGameIQBuildRunner::StartBuild(const FString& RunArg, const FText& Label, const FString& ExtraArgs)
{
	if (bBuilding)
	{
		Notify(NSLOCTEXT("GameIQBuildRunner", "AlreadyRunning", "Game IQ: a rebuild is already running."), SNotificationItem::CS_Fail);
		return;
	}

	// Run as a *separate* headless commandlet process — never in-process. Loading every asset fresh
	// inside the live editor trips engine ensures (e.g. Blueprint SimpleConstructionScript parent
	// fix-up); a clean commandlet process doesn't, and the rollback-journal index tolerates the editor
	// reading it concurrently.
	const FString Exe = FPlatformProcess::GenerateApplicationPath(TEXT("UnrealEditor-Cmd"), FApp::GetBuildConfiguration());
	const FString Project = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
	if (!FPaths::FileExists(Exe) || Project.IsEmpty())
	{
		Notify(NSLOCTEXT("GameIQBuildRunner", "NoCmd", "Game IQ: couldn't find UnrealEditor-Cmd to run the rebuild."), SNotificationItem::CS_Fail);
		return;
	}

	if (!FPlatformProcess::CreatePipe(ReadPipe, WritePipe))
	{
		Notify(NSLOCTEXT("GameIQBuildRunner", "NoPipe", "Game IQ: couldn't create a pipe to capture build output."), SNotificationItem::CS_Fail);
		return;
	}

	const FString Args = ExtraArgs.IsEmpty()
		? FString::Printf(TEXT("\"%s\" -run=%s -unattended -nopause -nosplash"), *Project, *RunArg)
		: FString::Printf(TEXT("\"%s\" -run=%s -unattended -nopause -nosplash %s"), *Project, *RunArg, *ExtraArgs);
	Proc = FPlatformProcess::CreateProc(*Exe, *Args, /*bLaunchDetached=*/false,
		/*bLaunchHidden=*/true, /*bLaunchReallyHidden=*/true, nullptr, 0, nullptr, WritePipe, nullptr);
	if (!Proc.IsValid())
	{
		Notify(NSLOCTEXT("GameIQBuildRunner", "SpawnFail", "Game IQ: failed to start the rebuild process."), SNotificationItem::CS_Fail);
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		ReadPipe = WritePipe = nullptr;
		return;
	}

	LogLines.Reset();
	PendingLineFragment.Reset();
	CurrentStage = Label.ToString();
	bBuilding = true;

	FNotificationInfo Info(Label);
	Info.ExpireDuration = 4.0f;
	Info.bFireAndForget = false;
	NotificationItem = FSlateNotificationManager::Get().AddNotification(Info);
	if (TSharedPtr<SNotificationItem> Item = NotificationItem.Pin())
	{
		Item->SetCompletionState(SNotificationItem::CS_Pending);
	}

	PollHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FGameIQBuildRunner::PollTick), 0.5f);
}

void FGameIQBuildRunner::AppendOutput(const FString& NewText)
{
	FString Combined = PendingLineFragment + NewText;
	Combined.ReplaceInline(TEXT("\r"), TEXT(""));

	int32 NewlineIdx;
	while (Combined.FindChar(TEXT('\n'), NewlineIdx))
	{
		const FString Line = Combined.Left(NewlineIdx);
		Combined = Combined.RightChop(NewlineIdx + 1);

		// Every GameIQ log category is named "LogGameIQ*" — this substring filters the child
		// process's raw stdout (engine boot spam, shader stats, etc.) down to the lines worth
		// showing the user live.
		if (!Line.IsEmpty() && Line.Contains(TEXT("LogGameIQ")))
		{
			CurrentStage = Line;
			LogLines.Add(Line);
			if (LogLines.Num() > MaxLogLines) { LogLines.RemoveAt(0); }
			UE_LOG(LogGameIQRunner, Display, TEXT("%s"), *Line);
		}
	}
	PendingLineFragment = Combined;
}

bool FGameIQBuildRunner::PollTick(float /*DeltaTime*/)
{
	if (ReadPipe)
	{
		const FString NewOutput = FPlatformProcess::ReadPipe(ReadPipe);
		if (!NewOutput.IsEmpty())
		{
			AppendOutput(NewOutput);
		}
	}

	if (Proc.IsValid() && FPlatformProcess::IsProcRunning(Proc))
	{
		if (TSharedPtr<SNotificationItem> Item = NotificationItem.Pin())
		{
			Item->SetText(FText::FromString(CurrentStage));
		}
		return true; // keep polling
	}

	FinishBuild();
	return false; // stop ticker
}

void FGameIQBuildRunner::FinishBuild()
{
	// Final drain in case the process exited between the last read and now.
	if (ReadPipe)
	{
		const FString Remainder = FPlatformProcess::ReadPipe(ReadPipe);
		if (!Remainder.IsEmpty()) { AppendOutput(Remainder); }
	}

	int32 ReturnCode = -1;
	if (Proc.IsValid())
	{
		FPlatformProcess::GetProcReturnCode(Proc, &ReturnCode);
		FPlatformProcess::CloseProc(Proc);
		Proc.Reset();
	}
	if (ReadPipe || WritePipe)
	{
		FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		ReadPipe = WritePipe = nullptr;
	}

	bBuilding = false;
	PollHandle.Reset();

	const bool bSuccess = (ReturnCode == 0);
	if (TSharedPtr<SNotificationItem> Item = NotificationItem.Pin())
	{
		Item->SetText(bSuccess
			? NSLOCTEXT("GameIQBuildRunner", "Done", "Game IQ: index rebuild complete.")
			: FText::Format(NSLOCTEXT("GameIQBuildRunner", "Failed", "Game IQ: rebuild failed (exit code {0})."), FText::AsNumber(ReturnCode)));
		Item->SetCompletionState(bSuccess ? SNotificationItem::CS_Success : SNotificationItem::CS_Fail);
		Item->ExpireAndFadeout();
	}
	NotificationItem.Reset();

	OnFinished.Broadcast(bSuccess);
}

FString FGameIQBuildRunner::GetLogTail() const
{
	return FString::Join(LogLines, TEXT("\n"));
}
