#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ThirdPartyActor.generated.h"

// This lives under an excluded plugin dir — it should NOT appear in the index.
UCLASS()
class AThirdPartyActor : public AActor
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, Category="ThirdParty")
	int32 ThirdPartyValue = 0;
};
