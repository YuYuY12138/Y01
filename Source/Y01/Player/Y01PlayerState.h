#pragma once

#include "GameFramework/PlayerState.h"
#include "AbilitySystemInterface.h"
#include "Combat/Y01Combat.h"
#include "Y01PlayerState.generated.h"

class UY01SourceEffectsComponent;
class UY01IncomeComponent;
class UY01LoadoutComponent;
class UY01EconomyComponent;
class UY01GrowthComponent;
class UY01DropDeliveryComponent;
class UY01DamageEffectsComponent;
class UY01ReviveComponent;

UCLASS()
class Y01_API AY01PlayerState : public APlayerState, public IAbilitySystemInterface
{
    GENERATED_BODY()
public:
    AY01PlayerState();
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystem; }
    UY01CombatAbilitySystem* GetCombatASC() const { return AbilitySystem; }
    UY01SourceEffectsComponent* GetSourceEffects() const { return SourceEffects; }
    UY01IncomeComponent* GetIncome() const { return Income; }
    UY01LoadoutComponent* GetLoadout() const { return Loadout; }
    UY01EconomyComponent* GetEconomy() const { return Economy; }
    UY01GrowthComponent* GetGrowth() const { return Growth; }
    UY01DropDeliveryComponent* GetDropDelivery() const { return DropDelivery; }
    UY01DamageEffectsComponent* GetDamageEffects() const { return DamageEffects; }
    UY01ReviveComponent* GetRevive() const { return Revive; }
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    bool TryInitializeCharacter(const FY01DataCatalog& Catalog, const FY01DataRef& Character, FString& OutError);
    bool IsCharacterInitialized() const { return AbilitySystem->IsCombatInitialized(); }
    bool TryActivateSkillSlot(int32 Slot);
    // 仅服务器通用槽事务；空引用卸下，身份必须属于请求控制器，不是库存接口。
    bool TrySetEquipmentSlot(const FY01DataCatalog& Catalog, AController* Requester, int32 Slot, const FY01DataRef& Equipment, FString& OutError);
    // GameMode在外部负责波次幂等和奖励归属；此处仅进行权威、数值及溢出校验。
    bool TryAddMatchReward(int32 GoldDelta, int32 ExperienceDelta);
    UFUNCTION() bool CanAddMatchReward(int32 GoldDelta, int32 ExperienceDelta) const;
    int32 GetGold() const { return Gold; }
    int32 GetExperience() const { return Experience; }
    const TArray<FY01DataRef>& GetEquipmentSlots() const { return EquipmentSlots; }
    const TArray<FY01DataRef>& GetSkillSlots() const { return SkillSlots; }
private:
    friend class UY01IncomeComponent;
    bool bEquipmentChanging = false;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01CombatAbilitySystem> AbilitySystem;
    UPROPERTY() TObjectPtr<UY01AttributeSet> Attributes;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01SourceEffectsComponent> SourceEffects;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01IncomeComponent> Income;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01LoadoutComponent> Loadout;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01EconomyComponent> Economy;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01GrowthComponent> Growth;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01DropDeliveryComponent> DropDelivery;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01DamageEffectsComponent> DamageEffects;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01ReviveComponent> Revive;
    UPROPERTY(Replicated) int32 Gold = 0;
    UPROPERTY(Replicated) int32 Experience = 0;
    UPROPERTY(Replicated) TArray<FY01DataRef> EquipmentSlots;
    UPROPERTY(Replicated) TArray<FY01DataRef> SkillSlots;
};
