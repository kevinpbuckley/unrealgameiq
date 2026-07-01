// Copyright Buckley Builds LLC. All Rights Reserved.

#include "GameIQSettings.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogGameIQSettings, Log, All);

#if WITH_EDITOR
void UGameIQSettings::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	WriteConfigJson();
}
#endif

void UGameIQSettings::WriteConfigJson() const
{
	TArray<TSharedPtr<FJsonValue>> Excludes;
	for (const FString& Dir : ExcludeDirectories)
	{
		const FString Normalized = Dir.Replace(TEXT("\\"), TEXT("/")).TrimStartAndEnd();
		if (!Normalized.IsEmpty())
		{
			Excludes.Add(MakeShared<FJsonValueString>(Normalized));
		}
	}

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("exclude"), Excludes);

	FString Out;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, Writer);

	const FString Path = FPaths::Combine(FPaths::ProjectDir(), TEXT("gameiq.config.json"));
	if (FFileHelper::SaveStringToFile(Out, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogGameIQSettings, Display, TEXT("Game IQ: wrote %s (%d excludes)"), *Path, Excludes.Num());
	}
	else
	{
		UE_LOG(LogGameIQSettings, Warning, TEXT("Game IQ: failed to write %s"), *Path);
	}
}
