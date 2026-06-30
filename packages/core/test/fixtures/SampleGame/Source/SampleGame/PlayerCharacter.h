#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "PlayerCharacter.generated.h"

class UHealthComponent;

UCLASS()
class SAMPLEGAME_API APlayerCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	APlayerCharacter();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components")
	TObjectPtr<UHealthComponent> Health;

	/** Called when the character lands; applies fall damage above a threshold. */
	UFUNCTION(BlueprintCallable, Category="Movement")
	void OnLanded(float FallDistance);
};
