#pragma once

#include "Combat/Y01Combat.h"
#include "GameFramework/Actor.h"
#include "Y01DamageEffects.generated.h"

struct Y01_API FY01DamageSelection
{
    FVector Center = FVector::ZeroVector;
    TArray<TWeakObjectPtr<AActor>> Targets;
};
namespace Y01Damage
{
    Y01_API float Evaluate(const FY01DamageEffectRow& Row, const FY01UnitCombatStats& Stats);
    Y01_API bool Select(const UY01CombatAbilitySystem* Source, const FY01DamageEffectRow& Row,
        TConstArrayView<AActor*> Candidates, FRandomStream& Random, FY01DamageSelection& Out);
    Y01_API bool ValidateSource(const TArray<FY01ItemDamageEffect>& Effects, EY01TableKind Kind, EY01SkillType SkillType);
    Y01_API FString Describe(const TArray<FY01ItemDamageEffect>& Effects);
}

UCLASS()
class Y01_API UY01DamageAbility : public UY01LoadoutAbility
{
    GENERATED_BODY()
public:
    UY01DamageAbility();
    virtual bool SupportsLoadoutSkill(const FY01TestSkillConfig& Config) const override;
    virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
        FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
protected:
    virtual void ExecuteSkill(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};

UCLASS()
class Y01_API UY01TemplateDamageExecution : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()
public:
    virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters,
        FGameplayEffectCustomExecutionOutput& Output) const override;
};
UCLASS()
class Y01_API UY01TemplateDamageEffect : public UGameplayEffect
{
    GENERATED_BODY()
public:
    UY01TemplateDamageEffect();
};

// 仅复制占位表现；没有碰撞伤害、RPC授权或奖励回调。Product为Instant时是主动区域技能的短暂范围指示圈。
UCLASS()
class Y01_API AY01DamageVisual : public AActor
{
    GENERATED_BODY()
public:
    AY01DamageVisual();
    void Configure(EY01DamageProduct InProduct, double InRadius);
    EY01DamageProduct GetProduct() const { return Product; }
    float GetRadius() const { return Radius; }
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
private:
    UPROPERTY() TObjectPtr<class UStaticMeshComponent> Shape;
    UPROPERTY() TObjectPtr<class UInstancedStaticMeshComponent> Rings;
    UPROPERTY(ReplicatedUsing=OnRep_Visual) EY01DamageProduct Product = EY01DamageProduct::Instant;
    UPROPERTY(ReplicatedUsing=OnRep_Visual) float Radius = 20;
    UFUNCTION() void OnRep_Visual();
};
