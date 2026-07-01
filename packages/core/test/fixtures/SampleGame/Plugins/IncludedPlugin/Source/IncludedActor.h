#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "IncludedActor.generated.h"

// A first-party plugin dir that is NOT excluded — it SHOULD appear in the index.
UCLASS()
class AIncludedActor : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category="Included")
	int32 IncludedValue = 0;
};
