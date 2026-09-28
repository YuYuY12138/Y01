#pragma once

#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffectExecutionCalculation.h"
#include "NativeGameplayTags.h"
#include "Data/Y01DataCatalog.h"
#include "Y01Combat.generated.h"

namespace Y01Tags
{
    Y01_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Damage);
}

UCLASS()
class Y01_API UY01AttributeSet : public UAttributeSet
{
    GENERATED_BODY()
public:
    UPROPERTY(ReplicatedUsing=OnRep_Health) FGameplayAttributeData Health;
    UPROPERTY(ReplicatedUsing=OnRep_MaxHealth) FGameplayAttributeData MaxHealth;
    UPROPERTY(ReplicatedUsing=OnRep_AttackDamage) FGameplayAttributeData AttackDamage;
    GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UY01AttributeSet, Health)
    GAMEPLAYATTRIBUTE_VALUE_GETTER(Health)
    GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UY01AttributeSet, MaxHealth)
    GAMEPLAYATTRIBUTE_VALUE_GETTER(MaxHealth)
    GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UY01AttributeSet, AttackDamage)
    GAMEPLAYATTRIBUTE_VALUE_GETTER(AttackDamage)
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;
    virtual void PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const override;
    virtual bool PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data) override;
    virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;
    virtual void PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue) override;
private:
    void ClampAttribute(const FGameplayAttribute& Attribute, float& Value) const;
    UFUNCTION() void OnRep_Health(const FGameplayAttributeData& OldValue);
    UFUNCTION() void OnRep_MaxHealth(const FGameplayAttributeData& OldValue);
    UFUNCTION() void OnRep_AttackDamage(const FGameplayAttributeData& OldValue);
};

UCLASS()
class Y01_API UY01DamageExecution : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()
public:
    virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters, FGameplayEffectCustomExecutionOutput& Output) const override;
};

UCLASS()
class Y01_API UY01DamageEffect : public UGameplayEffect
{
    GENERATED_BODY()
public:
    UY01DamageEffect();
};

UCLASS()
class Y01_API UY01BasicAttackAbility : public UGameplayAbility
{
    GENERATED_BODY()
public:
    UY01BasicAttackAbility();
    virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
        FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
    virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};

USTRUCT()
struct Y01_API FY01GrantedSource
{
    GENERATED_BODY()
    UPROPERTY() FY01DataRef Source;
    // 同一内容可能处于不同槽；此处不决定内容重复或堆叠规则。
    UPROPERTY() int32 Slot = INDEX_NONE;
    UPROPERTY() FGameplayAbilitySpecHandle Ability;
    UPROPERTY() FActiveGameplayEffectHandle Effect;
    UPROPERTY() TSubclassOf<UGameplayEffect> EffectClass;
    UPROPERTY() bool bEffectApplied = false;
    UPROPERTY() EY01SkillType SkillType = EY01SkillType::Passive;
    UPROPERTY() float CooldownSeconds = 0;
    double NextActivationTime = 0;
};

UCLASS()
class Y01_API UY01CombatAbilitySystem : public UAbilitySystemComponent
{
    GENERATED_BODY()
public:
    UY01CombatAbilitySystem();
    virtual void NotifyAbilityCommit(UGameplayAbility* Ability) override;
    virtual void InitAbilityActorInfo(AActor* InOwnerActor, AActor* InAvatarActor) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    // 只接受权威目录身份；先验证全部引用再提交。不同身份不能覆盖已初始化实体。
    bool TryInitializeFromData(const FY01DataCatalog& Catalog, const FY01DataRef& Definition, FString& OutError);
    bool IsCombatInitialized() const { return bInitialized; }
    const FY01DataRef& GetDefinition() const { return DefinitionRef; }
    const TArray<FY01GrantedSource>& GetGrantedSources() const { return GrantedSources; }
    const TArray<FY01DataRef>& GetInitialEquipment() const { return InitialEquipment; }
    const TArray<FY01DataRef>& GetInitialSkills() const { return InitialSkills; }
    float GetInitialMoveSpeed() const { return InitialMoveSpeed; }
    // 外部服务器逻辑显式选目标；不负责自动选敌、追击或攻击频率。
    bool SetCombatTarget(AActor* Target);
    AActor* GetCombatTarget() const { return CombatTarget.Get(); }
    bool IsValidCombatTarget(const AActor* Target) const;
    bool TryActivateBasicAttack();
    bool TryActivateSkillSlot(int32 Slot);
    bool IsAbilityReady(FGameplayAbilitySpecHandle Handle) const;
    float GetSkillCooldownRemaining(int32 Slot) const;
    FGameplayAbilitySpecHandle GetBasicAttackHandle() const { return BasicAttackHandle; }
private:
    UPROPERTY() TArray<FY01GrantedSource> GrantedSources;
    UPROPERTY() TArray<FY01DataRef> InitialEquipment;
    UPROPERTY() TArray<FY01DataRef> InitialSkills;
    UPROPERTY() FY01DataRef DefinitionRef;
    UPROPERTY() TWeakObjectPtr<AActor> CombatTarget;
    FGameplayAbilitySpecHandle BasicAttackHandle;
    float AttackRange = 0;
    // -1表示尚未收到配置，零是合法的表速度。
    UPROPERTY(ReplicatedUsing=OnRep_InitialMoveSpeed) float InitialMoveSpeed = -1;
    UFUNCTION() void OnRep_InitialMoveSpeed();
    bool bInitialized = false;
    bool bInitializing = false;
    bool bRequestInProgress = false;
};
