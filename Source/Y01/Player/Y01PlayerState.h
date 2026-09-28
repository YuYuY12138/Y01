#pragma once

#include "GameFramework/PlayerState.h"
#include "AbilitySystemInterface.h"
#include "Combat/Y01Combat.h"
#include "Y01PlayerState.generated.h"

UCLASS()
class Y01_API AY01PlayerState : public APlayerState, public IAbilitySystemInterface
{
    GENERATED_BODY()
public:
    AY01PlayerState();
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystem; }
    UY01CombatAbilitySystem* GetCombatASC() const { return AbilitySystem; }
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    bool TryInitializeCharacter(const FY01DataCatalog& Catalog, const FY01DataRef& Character, FString& OutError);
    bool IsCharacterInitialized() const { return AbilitySystem->IsCombatInitialized(); }
    bool TryActivateSkillSlot(int32 Slot);
    // GameMode在外部负责波次幂等和奖励归属；此处仅进行权威、数值及溢出校验。
    bool TryAddMatchReward(int32 GoldDelta, int32 ExperienceDelta);
    UFUNCTION() bool CanAddMatchReward(int32 GoldDelta, int32 ExperienceDelta) const;
    int32 GetGold() const { return Gold; }
    int32 GetExperience() const { return Experience; }
    const TArray<FY01DataRef>& GetEquipmentSlots() const { return EquipmentSlots; }
    const TArray<FY01DataRef>& GetSkillSlots() const { return SkillSlots; }
private:
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01CombatAbilitySystem> AbilitySystem;
    UPROPERTY() TObjectPtr<UY01AttributeSet> Attributes;
    UPROPERTY(Replicated) int32 Gold = 0;
    UPROPERTY(Replicated) int32 Experience = 0;
    UPROPERTY(Replicated) TArray<FY01DataRef> EquipmentSlots;
    UPROPERTY(Replicated) TArray<FY01DataRef> SkillSlots;
};
