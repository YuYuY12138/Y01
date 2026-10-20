#pragma once
#include "Combat/Y01Combat.h"
#include "GameFramework/Actor.h"
#include "Y01TestSkill.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class AY01Monster;

UENUM()
enum class EY01TestAttackPhase : uint8 { Active, HitFeedback };

// 只复制成功Commit的实例；数值全由授予时的技能表配置和服务器属性产生。
USTRUCT()
struct FY01TestAttackState
{
    GENERATED_BODY()
    UPROPERTY() FY01TestSkillConfig Config;
    UPROPERTY() FY01DataRef Skill;
    UPROPERTY() TObjectPtr<AActor> Caster;
    UPROPERTY() FVector Origin = FVector::ZeroVector;
    UPROPERTY() FVector Direction = FVector::ZeroVector;
    UPROPERTY() FVector Position = FVector::ZeroVector;
    UPROPERTY() EY01TestAttackPhase Phase = EY01TestAttackPhase::Active;
    UPROPERTY() double StartedAt = 0;
    UPROPERTY() double EndsAt = 0;
    UPROPERTY() float DamageSnapshot = 0;
    UPROPERTY() bool bCritical = false; // 发射/放置时判定一次，命中沿用（bLifesteal 从Config读，不入状态）。
    UPROPERTY() bool bAutoAttack = false;
    UPROPERTY() float MeleeAngle = 0;
    UPROPERTY() TObjectPtr<AActor> SelectedTarget;
};

UCLASS()
class UY01TestSkillAbility : public UY01LoadoutAbility
{
    GENERATED_BODY()
public:
    UY01TestSkillAbility();
    virtual bool SupportsLoadoutSkill(const FY01TestSkillConfig& Config) const override
    { return Config.IsValid() && (Config.Mode == EY01TestSkillMode::Area || Config.Mode == EY01TestSkillMode::Projectile); }
    virtual bool CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
        const FGameplayTagContainer* SourceTags = nullptr, const FGameplayTagContainer* TargetTags = nullptr,
        FGameplayTagContainer* OptionalRelevantTags = nullptr) const override;
    virtual void ExecuteSkill(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
        FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData) override;
};

// 仅测试模块的真实GA生命周期探针，不向生产暴露故障或时钟入口。
UCLASS()
class UY01LearnProbeAbility : public UY01BasicAttackAbility
{
    GENERATED_BODY()
public:
    static TFunction<void(UAbilitySystemComponent*, FGameplayAbilitySpecHandle)> Given;
    static TFunction<void(UAbilitySystemComponent*, FGameplayAbilitySpecHandle)> Removed;
    virtual void OnGiveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec) override;
    virtual void OnRemoveAbility(const FGameplayAbilityActorInfo* ActorInfo, const FGameplayAbilitySpec& Spec) override;
};

UCLASS()
class UY01TestSkillDamageExecution : public UGameplayEffectExecutionCalculation
{
    GENERATED_BODY()
public:
    virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters,
        FGameplayEffectCustomExecutionOutput& Output) const override;
};

UCLASS()
class UY01TestSkillDamageEffect : public UGameplayEffect
{
    GENERATED_BODY()
public:
    UY01TestSkillDamageEffect();
};

UCLASS()
class AY01TestAttack : public AActor
{
    GENERATED_BODY()
public:
    AY01TestAttack();
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    const FY01TestAttackState& GetState() const { return State; }
    bool AuthorizesDamage(const UY01CombatAbilitySystem* InstigatorASC, const UY01CombatAbilitySystem* Target) const;
    static bool TryAutoAttack(UY01CombatAbilitySystem* ASC);
    static AActor* FindAutoTarget(const UY01CombatAbilitySystem* ASC);
private:
    friend class UY01TestSkillAbility;
    void Initialize(UY01CombatAbilitySystem* ASC, const FY01GrantedSource& Grant, FVector Direction, float Damage);
    void ActivateState(UY01CombatAbilitySystem* ASC);
    void Cancel();
    void CancelWithReason(const TCHAR* Reason);
    bool SourceValid() const;
    bool TargetValid(AActor* Target) const;
    void RememberTargets();
    void CheckArea();
    void SweepBall(FVector End);
    void ApplyHit(AActor* Target);
    void DrawState();
    UFUNCTION() void OnRep_State();
    UFUNCTION() void SourceEnded(AActor* Actor, EEndPlayReason::Type Reason);
    UFUNCTION() void TargetEnded(AActor* Actor, EEndPlayReason::Type Reason);
    void TargetInvalidated();
    UPROPERTY(ReplicatedUsing=OnRep_State) FY01TestAttackState State;
    UPROPERTY() TObjectPtr<USphereComponent> Collision;
    UPROPERTY() TObjectPtr<UStaticMeshComponent> Shape;
    TWeakObjectPtr<UY01CombatAbilitySystem> Source;
    TWeakObjectPtr<UY01CombatAbilitySystem> ApplyingTarget;
    TMap<TWeakObjectPtr<UY01CombatAbilitySystem>, uint64> TargetSessions;
    TSet<TWeakObjectPtr<AActor>> HitTargets;
    TWeakObjectPtr<UY01CombatAbilitySystem> SelectedASC;
    FDelegateHandle SessionHandle;
    FDelegateHandle TargetSessionHandle;
    uint64 SourceSession = 0;
    double LastStepAt = 0;
    bool bInitialized = false;
};
