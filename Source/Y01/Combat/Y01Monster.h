#pragma once

#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "Combat/Y01Combat.h"
#include "Y01Monster.generated.h"

UCLASS()
class Y01_API AY01Monster : public ACharacter, public IAbilitySystemInterface
{
    GENERATED_BODY()
public:
    AY01Monster();
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystem; }
    UY01CombatAbilitySystem* GetCombatASC() const { return AbilitySystem; }
    bool TryInitializeMonster(const FY01DataCatalog& Catalog, const FY01DataRef& Monster, FString& OutError);
    virtual void BeginPlay() override;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Y01") FY01DataRef MonsterDefinition;
private:
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01CombatAbilitySystem> AbilitySystem;
    UPROPERTY() TObjectPtr<UY01AttributeSet> Attributes;
};
