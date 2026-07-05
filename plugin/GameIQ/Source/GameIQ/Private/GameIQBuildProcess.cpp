// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQBuildProcess.h"

#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQBuildProcess, Log, All);

namespace GameIQBuildProcess
{
	namespace
	{
		struct FInFlightStage
		{
			FString Name;
			FProcHandle Handle;
			double StartTime = 0.0;
		};

		/** Same resolution GameIQBuildRunner.cpp uses for the top-level build's own subprocess. */
		bool ResolveExeAndProject(FString& OutExe, FString& OutProject)
		{
			OutExe = FPlatformProcess::GenerateApplicationPath(TEXT("UnrealEditor-Cmd"), FApp::GetBuildConfiguration());
			OutProject = FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath());
			return FPaths::FileExists(OutExe) && !OutProject.IsEmpty();
		}
	}

	bool RunStagesConcurrently(
		const TArray<FStageSpec>& Specs,
		const FString& ExtraArgs,
		int32 MaxConcurrent,
		TArray<GameIQ::FStageTiming>& OutTimings)
	{
		FString Exe, Project;
		if (!ResolveExeAndProject(Exe, Project))
		{
			UE_LOG(LogGameIQBuildProcess, Error, TEXT("Game IQ: couldn't resolve UnrealEditor-Cmd/project to launch extraction stages."));
			return false;
		}
		MaxConcurrent = FMath::Max(1, MaxConcurrent);

		bool bAllOk = true;
		int32 NextToLaunch = 0;
		TArray<FInFlightStage> InFlight;

		auto LaunchNext = [&]()
		{
			const FStageSpec& Spec = Specs[NextToLaunch++];
			const FString Args = ExtraArgs.IsEmpty()
				? FString::Printf(TEXT("\"%s\" -run=%s -unattended -nopause -nosplash"), *Project, *Spec.RunArg)
				: FString::Printf(TEXT("\"%s\" -run=%s -unattended -nopause -nosplash %s"), *Project, *Spec.RunArg, *ExtraArgs);

			FInFlightStage Stage;
			Stage.Name = Spec.Name;
			Stage.StartTime = FPlatformTime::Seconds();
			Stage.Handle = FPlatformProcess::CreateProc(*Exe, *Args, /*bLaunchDetached=*/false,
				/*bLaunchHidden=*/true, /*bLaunchReallyHidden=*/true, nullptr, 0, nullptr, nullptr, nullptr);
			if (!Stage.Handle.IsValid())
			{
				UE_LOG(LogGameIQBuildProcess, Warning, TEXT("Game IQ: failed to start stage '%s' (-run=%s)."), *Spec.Name, *Spec.RunArg);
				bAllOk = false;
				OutTimings.Add(GameIQ::FStageTiming{Spec.Name, 0.0});
				return;
			}
			UE_LOG(LogGameIQBuildProcess, Display, TEXT("Game IQ: launched stage '%s' (-run=%s)..."), *Spec.Name, *Spec.RunArg);
			InFlight.Add(MoveTemp(Stage));
		};

		while (NextToLaunch < Specs.Num() && InFlight.Num() < MaxConcurrent)
		{
			LaunchNext();
		}

		while (InFlight.Num() > 0)
		{
			for (int32 i = InFlight.Num() - 1; i >= 0; --i)
			{
				if (FPlatformProcess::IsProcRunning(InFlight[i].Handle)) { continue; }

				int32 ReturnCode = -1;
				FPlatformProcess::GetProcReturnCode(InFlight[i].Handle, &ReturnCode);
				FPlatformProcess::CloseProc(InFlight[i].Handle);
				const double Elapsed = FPlatformTime::Seconds() - InFlight[i].StartTime;
				OutTimings.Add(GameIQ::FStageTiming{InFlight[i].Name, Elapsed});

				if (ReturnCode == 0)
				{
					UE_LOG(LogGameIQBuildProcess, Display, TEXT("Game IQ: stage '%s' done in %.1fs."), *InFlight[i].Name, Elapsed);
				}
				else
				{
					bAllOk = false;
					UE_LOG(LogGameIQBuildProcess, Error, TEXT("Game IQ: stage '%s' exited with code %d after %.1fs."),
						*InFlight[i].Name, ReturnCode, Elapsed);
				}
				InFlight.RemoveAt(i);

				if (NextToLaunch < Specs.Num()) { LaunchNext(); }
			}

			if (InFlight.Num() > 0) { FPlatformProcess::Sleep(0.25f); }
		}

		return bAllOk;
	}
}
