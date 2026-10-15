#pragma once

#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffectExecutionCalculation.h"
#include "NativeGameplayTags.h"
#include "Data/Y01DataCatalog.h"
#include "Combat/Y01DamageEvents.h"
#include "Match/Y01KillRewards.h"
#include "Y01Combat.generated.h"

class UY01LoadoutComponent;

namespace Y01Tags
{
    Y01_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Damage);
    Y01_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_DamageDisplay);
    Y01_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Damage);
    Y01_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Effect_Healing);
    Y01_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Healing);
    Y01_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_EquipmentAttack);
}

UCLASS()
class Y01_API UY01AttributeSet : public UAttributeSet
{
    GENERATED_BODY()
public:
    UPROPERTY(ReplicatedUsing=OnRep_Health) FGameplayAttributeData Health;
    UPROPERTY(ReplicatedUsing=OnRep_MaxHealth) FGameplayAttributeData MaxHealth;
    UPROPERTY(ReplicatedUsing=OnRep_AttackDamage) FGameplayAttributeData AttackDamage;
    UPROPERTY(ReplicatedUsing=OnRep_Armor) FGameplayAttributeData Armor;
    UPROPERTY(ReplicatedUsing=OnRep_HealthRegen) FGameplayAttributeData HealthRegen;
    GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UY01AttributeSet, Armor)
    GAMEPLAYATTRIBUTE_VALUE_GETTER(Armor)
    GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UY01AttributeSet, HealthRegen)
    GAMEPLAYATTRIBUTE_VALUE_GETTER(HealthRegen)
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
    virtual void OnAttributeAggregatorCreated(const FGameplayAttribute& Attribute, FAggregator* NewAggregator) const override;
private:
    struct FHealthExecution
    {
        const FGameplayEffectModCallbackData* Callback = nullptr;
        TWeakObjectPtr<UY01DamageEvents> Service;
        FY01PendingDamage Pending;
        FY01KillAuthorization KillAuthorization;
        FGuid KillEventId;
        bool bDamageTagged = false;
        bool bHealingTagged = false;
        bool bFrozen = false;
    };
    TArray<FHealthExecution> HealthExecutions;
    void ClampAttribute(const FGameplayAttribute& Attribute, float& Value) const;
    UFUNCTION() void OnRep_Health(const FGameplayAttributeData& OldValue);
    UFUNCTION() void OnRep_MaxHealth(const FGameplayAttributeData& OldValue);
    UFUNCTION() void OnRep_AttackDamage(const FGameplayAttributeData& OldValue);
    UFUNCTION() void OnRep_Armor(const FGameplayAttributeData& OldValue);
    UFUNCTION() void OnRep_HealthRegen(const FGameplayAttributeData& OldValue);
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
class Y01_API UY01HealingEffect : public UGameplayEffect
{
    GENERATED_BODY()
public:
    UY01HealingEffect();
};

UCLASS()
class Y01_API UY01EquipmentAttackEffect : public UGameplayEffect
{
    GENERATED_BODY()
public:
    UY01EquipmentAttackEffect();
};

// 服务器同步技能适配契约：固定外层保护激活到提交广播和执行结束。
// 原生实现必须显式声明支持的静态配置；不以任意可加载GA冒充可执行技能。
UCLASS(Abstract)
class Y01_API UY01LoadoutAbility : public UGameplayAbility
{
    GENERATED_BODY()
public:
    virtual bool SupportsLoadoutSkill(const FY01TestSkillConfig& Config) const { return false; }
    virtual void ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) final override;
protected:
    virtual void ExecuteSkill(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) {}
};

UCLASS()
class Y01_API UY01BasicAttackAbility : public UY01LoadoutAbility
{
    GENERATED_BODY()
public:
    UY01BasicAttackAbility();
    virtual bool SupportsLoadoutSkill(const FY01TestSkillConfig& Config) const override { return Config.IsValid() && Config.Mode == EY01TestSkillMode::None; }
    virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
        FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
    virtual void ExecuteSkill(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};

USTRUCT()
struct Y01_API FY01GrantedSource
{
    GENERATED_BODY()
    UPROPERTY() FY01DataRef Source;
    UPROPERTY() FGuid LearnedId;
    // 同一内容可能处于不同槽；此处不决定内容重复或堆叠规则。
    UPROPERTY() int32 Slot = INDEX_NONE;
    UPROPERTY() FGameplayAbilitySpecHandle Ability;
    UPROPERTY() FActiveGameplayEffectHandle Effect;
    UPROPERTY() TSubclassOf<UGameplayEffect> EffectClass;
    UPROPERTY() bool bEffectApplied = false;
    UPROPERTY() FActiveGameplayEffectHandle EquipmentAttackEffect;
    UPROPERTY() float AttackBonus = 0;
    UPROPERTY() EY01Quality EquipmentQuality = EY01Quality::Green;
    UPROPERTY() EY01SkillType SkillType = EY01SkillType::Passive;
    UPROPERTY() float CooldownSeconds = 0;
    UPROPERTY() FY01TestSkillConfig TestSkill;
    double NextActivationTime = 0;
    uint64 CommitSerial = 0;
    float CommittedAttackDamage = 0;
};

USTRUCT()
struct Y01_API FY01UnitCombatStats
{
    GENERATED_BODY()
    UPROPERTY() float Health = 0;
    UPROPERTY() float AttackDamage = 0;
    UPROPERTY() float AttackInterval = 1;
    UPROPERTY() EY01AttackType AttackType = EY01AttackType::Melee;
    UPROPERTY() float Range = 0;
    UPROPERTY() float MeleeAngle = 90;
    UPROPERTY() float ProjectileSpeed = 400;
    UPROPERTY() float Armor = 0;
    UPROPERTY() float MoveSpeed = 0;
    UPROPERTY() float HealthRegen = 0;
    UPROPERTY() float Strength = 0;
    UPROPERTY() float Agility = 0;
    UPROPERTY() float Wisdom = 0;
    template <typename T> void ReadRow(const T& R)
    {
        Health = R.Health; AttackDamage = R.AttackDamage; AttackInterval = R.AttackInterval; AttackType = R.AttackType;
        Range = R.Range; MeleeAngle = R.MeleeAngle; ProjectileSpeed = R.ProjectileSpeed;
        Armor = R.Armor; MoveSpeed = R.MoveSpeed; HealthRegen = R.HealthRegen;
    }
};

UCLASS()
class Y01_API UY01CombatAbilitySystem : public UAbilitySystemComponent
{
    GENERATED_BODY()
public:
    UY01CombatAbilitySystem();
    virtual void NotifyAbilityCommit(UGameplayAbility* Ability) override;
    virtual void NotifyAbilityActivated(FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability) override;
    virtual void InitAbilityActorInfo(AActor* InOwnerActor, AActor* InAvatarActor) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    // 只接受权威目录身份；先验证全部引用再提交。不同身份不能覆盖已初始化实体。
    bool TryInitializeFromData(const FY01DataCatalog& Catalog, const FY01DataRef& Definition, FString& OutError);
    bool IsCombatInitialized() const { return bInitialized; }
    const FY01DataRef& GetDefinition() const { return DefinitionRef; }
    const TArray<FY01GrantedSource>& GetGrantedSources() const;
    const TArray<FY01DataRef>& GetInitialEquipment() const { return InitialEquipment; }
    const TArray<FY01DataRef>& GetInitialSkills() const { return InitialSkills; }
    float GetInitialMoveSpeed() const { return InitialMoveSpeed; }
    // 外部服务器逻辑显式选目标；不负责自动选敌、追击或攻击频率。
    bool SetCombatTarget(AActor* Target);
    // 发现/追击只校验合法性；攻击仍必须通过自身范围、CD和来源事务门禁。
    bool SetDiscoveredTarget(AActor* Target);
    AActor* GetCombatTarget() const { return CombatTarget.Get(); }
    bool IsValidCombatTarget(const AActor* Target) const;
    // 通用合法性不含距离；专属技能必须另做服务器几何校验，不是客户端绕过开关。
    bool IsLegalCombatTarget(const AActor* Target) const;
    const FY01GrantedSource* FindGrantedAbility(FGameplayAbilitySpecHandle Handle) const;
    // 可选权威区域许可；未安装时保持旧战斗语义，双方都必须许可。
    TFunction<bool(const UY01CombatAbilitySystem*, const UY01CombatAbilitySystem*)> TargetPermission;
    bool IsCombatPairAllowed(const UY01CombatAbilitySystem* Other) const;
    // 切区使尚未完成的攻击及后续飞行物会话失效；不退还已提交CD或消耗。
    uint64 GetCombatSession() const { return CombatSession; }
    void InvalidateCombatSession();
    FSimpleMulticastDelegate CombatSessionInvalidated;
    bool TryActivateBasicAttack();
    bool TryActivateSkillSlot(int32 Slot);
    bool IsAbilityReady(FGameplayAbilitySpecHandle Handle) const;
    float GetSkillCooldownRemaining(int32 Slot) const;
    FGameplayAbilitySpecHandle GetBasicAttackHandle() const { return BasicAttackHandle; }
    FY01UnitCombatStats GetUnitStats() const;
    double GetNextAutoAttackTime() const { return NextAutoAttackTime; }
    bool IsAutoAttackInRange(const AActor* Target) const;
    bool TryAutoAttack(AActor* Target, TFunctionRef<bool(float)> StartAttack);
    bool ApplyHealingTo(UY01CombatAbilitySystem* Target, float Amount);
    void UpdateHealthRegen(bool bEffective);
private:
    friend class AY01Core;
    bool TryInitializeCore(const FY01CoreSettings& Settings, FString& Error);
    friend class AY01PlayerState;
    friend class UY01LoadoutComponent;
    friend class UY01LoadoutAbility;
    friend class UY01SourceEffectsComponent;
    friend class UY01GrowthComponent;
    UY01LoadoutComponent* ActiveLoadout() const;
    void UpdateRegenDisplay(float Gained, double Now, double Elapsed, bool bKeep);
    bool TrySetEquipmentSlot(const FY01DataCatalog& Catalog, int32 Slot, const FY01DataRef& Equipment, FString& OutError);
    bool ApplyEquipmentAttack(FY01GrantedSource& Grant);
    void LogEquipment(const TCHAR* Action, const FY01GrantedSource& Grant) const;
    UPROPERTY(Replicated) FY01UnitCombatStats UnitStats;
    UPROPERTY(Replicated) double NextAutoAttackTime = 0;
    double LastRegenTime = 0;
    double RegenRemainder = 0;
    double RegenDisplayAmount = 0;
    double RegenDisplayStartedAt = -1;
    uint64 RegenDisplaySession = 0;
    FGuid RegenDisplayEpoch;
    int32 RegenDisplayRegion = INDEX_NONE;
    bool bRegenEffective = true;
    bool bAutoAttackStarting = false;
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
    uint64 CombatSession = 0;
};
