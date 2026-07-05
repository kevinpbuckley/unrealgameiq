// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQBuildProcess.h"

#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQBuildProcess, Log, All);

namespace GameIQBuildProcess
{
	namespace
	{
		/** Same resolution GameIQBuildRunner.cpp uses for the top-level build's own subprocess. */
		bool ResolveExeAndProject(FString& OutExe, FString& OutProject)
		{
			OutExe = FPlatformProcess::GenerateApplicationPath(TEXT("UnrealEditor-Cmd"), FApp::GetBuildConfiguration());
			OutProject = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
			return FPaths::FileExists(OutExe) && !OutProject.IsEmpty();
		}
	}

	TArray<FLaunchedStage> LaunchStages(const TArray<FStageSpec>& Specs, const FString& ExtraArgs)
	{
		TArray<FLaunchedStage> Stages;

		FString Exe, Project;
		if (!ResolveExeAndProject(Exe, Project))
		{
			UE_LOG(LogGameIQBuildProcess, Error, TEXT("Game IQ: couldn't resolve UnrealEditor-Cmd/project to launch extraction stages."));
			for (const FStageSpec& Spec : Specs)
			{
				FLaunchedStage Failed;
				Failed.Spec = Spec;
				Stages.Add(MoveTemp(Failed));
			}
			return Stages;
		}

		for (const FStageSpec& Spec : Specs)
		{
			const FString Args = ExtraArgs.IsEmpty()
				? FString::Printf(TEXT("\"%s\" -run=%s -unattended -nopause -nosplash"), *Project, *Spec.RunArg)
				: FString::Printf(TEXT("\"%s\" -run=%s -unattended -nopause -nosplash %s"), *Project, *Spec.RunArg, *ExtraArgs);

			FLaunchedStage Stage;
			Stage.Spec = Spec;
			Stage.StartTime = FPlatformTime::Seconds();
			Stage.LaunchUtc = FDateTime::UtcNow();
			Stage.Handle = FPlatformProcess::CreateProc(*Exe, *Args, /*bLaunchDetached=*/false,
				/*bLaunchHidden=*/true, /*bLaunchReallyHidden=*/true, nullptr, 0, nullptr, nullptr, nullptr);
			Stage.bLaunched = Stage.Handle.IsValid();
			if (Stage.bLaunched)
			{
				UE_LOG(LogGameIQBuildProcess, Display, TEXT("Game IQ: launched stage '%s' (-run=%s)..."), *Spec.Name, *Spec.RunArg);
			}
			else
			{
				UE_LOG(LogGameIQBuildProcess, Warning, TEXT("Game IQ: failed to start stage '%s' (-run=%s)."), *Spec.Name, *Spec.RunArg);
			}
			Stages.Add(MoveTemp(Stage));
		}
		return Stages;
	}

	bool WaitForStages(
		TArray<FLaunchedStage>& Stages,
		const FString& ExtractDir,
		TArray<GameIQ::FStageTiming>& OutTimings,
		TArray<FString>& OutStaleOutputs)
	{
		bool bAllOk = true;

		TArray<int32> Pending;
		for (int32 i = 0; i < Stages.Num(); ++i)
		{
			if (Stages[i].bLaunched) { Pending.Add(i); }
			else
			{
				bAllOk = false;
				OutTimings.Add(GameIQ::FStageTiming{Stages[i].Spec.Name, 0.0});
				if (!Stages[i].Spec.OutputFile.IsEmpty()) { OutStaleOutputs.Add(Stages[i].Spec.OutputFile); }
			}
		}

		while (Pending.Num() > 0)
		{
			for (int32 p = Pending.Num() - 1; p >= 0; --p)
			{
				FLaunchedStage& Stage = Stages[Pending[p]];
				if (FPlatformProcess::IsProcRunning(Stage.Handle)) { continue; }

				int32 ReturnCode = -1;
				FPlatformProcess::GetProcReturnCode(Stage.Handle, &ReturnCode);
				FPlatformProcess::CloseProc(Stage.Handle);
				const double Elapsed = FPlatformTime::Seconds() - Stage.StartTime;
				OutTimings.Add(GameIQ::FStageTiming{Stage.Spec.Name, Elapsed});

				// The exit code reflects the engine's logged-error count (benign shutdown/content
				// warnings included), not the commandlet's own result — every stage routinely exits
				// nonzero on clean runs. What actually matters is whether the stage refreshed its
				// extract output; judge success by the output file's timestamp.
				bool bFresh = true;
				if (!Stage.Spec.OutputFile.IsEmpty())
				{
					const FString OutPath = ExtractDir / Stage.Spec.OutputFile;
					const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*OutPath);
					bFresh = Stamp != FDateTime::MinValue() && Stamp >= Stage.LaunchUtc - FTimespan::FromMinutes(1);
				}

				if (bFresh)
				{
					UE_LOG(LogGameIQBuildProcess, Display, TEXT("Game IQ: stage '%s' done in %.1fs (exit %d, output fresh)."),
						*Stage.Spec.Name, Elapsed, ReturnCode);
				}
				else
				{
					bAllOk = false;
					OutStaleOutputs.Add(Stage.Spec.OutputFile);
					UE_LOG(LogGameIQBuildProcess, Error,
						TEXT("Game IQ: stage '%s' FAILED — it exited (code %d) after %.1fs without refreshing %s; its stale output will be skipped by the ingest."),
						*Stage.Spec.Name, ReturnCode, Elapsed, *Stage.Spec.OutputFile);
				}
				Pending.RemoveAt(p);
			}

			if (Pending.Num() > 0) { FPlatformProcess::Sleep(0.25f); }
		}

		return bAllOk;
	}
}
