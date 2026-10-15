#pragma once

#include "Components/ActorComponent.h"
#include "Combat/Y01DamageEffects.h"
#include "Y01DamageEffectsComponent.generated.h"

class AY01PlayerState;

UCLASS()
class Y01_API UY01DamageEffectsComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01DamageEffectsComponent();
    void ResetCombat();
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    friend class UY01SourceEffectsComponent;
    friend class UY01ReviveComponent;
    // 只同步共同提交后的服务器来源，不从FastArray或显示视图推导资格。
    void SynchronizeCommitted();
    friend class UY01DamageAbility;
    friend class UY01TemplateDamageExecution;
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FY01DamageNextTestAccess;
#endif
    struct FSource
    {
        FString Id;
        FGuid LearnedId;
        FY01ItemDamageEffect Effect;
        uint64 Generation = 0;
        double Next = 0;
    };
    struct FBeat
    {
        double Next = 0;
        uint64 Serial = 0;
        bool bInside = true;
    };
    struct FProduct
    {
        uint64 Id = 0;
        FString SourceId;
        uint64 Generation = 0;
        FY01ItemDamageEffect Effect;
        TWeakObjectPtr<AActor> Target;
        uint64 TargetSession = 0;
        FVector Position = FVector::ZeroVector;
        double Expires = 0;
        double LastMove = 0;
        float Snapshot = 0;
        TMap<TWeakObjectPtr<AActor>, FBeat> Beats;
        TWeakObjectPtr<AY01DamageVisual> Visual;
    };
    struct FActiveCast
    {
        FSource Source;
        FY01DamageSelection Selection;
        float Snapshot = 0;
    };
    bool Eligible() const;
    bool CanRun(FGameplayAbilitySpecHandle Active = {}) const;
    TArray<AActor*> Candidates() const;
    double Now() const;
    void Update(double Time);
    bool CanActivate(FGameplayAbilitySpecHandle Handle) const;
    bool PrepareActive(FGameplayAbilitySpecHandle Handle, TArray<FActiveCast>& Out);
    void ExecuteActive(const TArray<FActiveCast>& Casts, uint64 Epoch, FGameplayAbilitySpecHandle Handle);
    void Launch(const FSource& Source, const FY01DamageSelection& Selection, float Snapshot, double Time);
    void Advance(uint64 Id, double Time);
    void Hit(uint64 Execution, uint64 Beat, AActor* Target, float Damage);
    bool ConsumeAuthorization(const FGameplayEffectSpec& Spec, const UY01CombatAbilitySystem* Source,
        const UY01CombatAbilitySystem* Target, float& Damage);
    void RemoveProduct(uint64 Id);
    bool MakeVisual(FProduct& Product);
    AY01PlayerState* Player() const;
    TMap<FString, FSource> Sources;
    TMap<uint64, FProduct> Products;
    FRandomStream Random;
    uint64 Serial = 0;
    uint64 Generation = 0;
    uint64 Epoch = 0;
    double LastTime = 0;
    bool bUpdating = false;
    bool bSynchronizing = false;
    TMap<uint64, TMap<TWeakObjectPtr<AActor>, uint64>> Executed;
    bool bBoundSession = false;
    bool bEnded = false;
    FGameplayAbilitySpecHandle ActiveHandle;
    const FGameplayEffectContext* AuthorizedContext = nullptr;
    TWeakObjectPtr<UY01CombatAbilitySystem> AuthorizedTarget;
    float AuthorizedDamage = 0;
    bool bAuthorizationAvailable = false;
};
