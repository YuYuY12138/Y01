#pragma once

#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "Data/Y01DataTypes.h"
#include "Y01Character.generated.h"

class USpringArmComponent;
class UCameraComponent;

UCLASS()
class Y01_API AY01Character : public ACharacter, public IAbilitySystemInterface
{
    GENERATED_BODY()
public:
    AY01Character();
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
    virtual void PossessedBy(AController* NewController) override;
    virtual void OnRep_PlayerState() override;
    void RefreshAbilityActorInfo();
    // 可由后续GameMode在SpawnActorDeferred期间配置；None时不猜测默认行。
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Y01") FY01DataRef CharacterDefinition;
private:
    UPROPERTY(VisibleAnywhere) TObjectPtr<USpringArmComponent> CameraBoom;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCameraComponent> FollowCamera;
};
