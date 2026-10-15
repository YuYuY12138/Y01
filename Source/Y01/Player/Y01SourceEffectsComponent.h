#pragma once

#include "Components/ActorComponent.h"
#include "Data/Y01DataCatalog.h"
#include "Player/Y01SourceRuntimeTypes.h"
#include "Player/Y01AttributeResolver.h"
#include "Y01SourceEffectsComponent.generated.h"

class AY01PlayerState;
class UY01CombatAbilitySystem;
class UY01IncomeComponent;
struct FAggregator;

UCLASS()
class Y01_API UY01SourceEffectsComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01SourceEffectsComponent();
    FY01RuntimeResult Initialize(const FGuid& Match, int32 JoinSeq, double InitialRemainder = 0);
    FY01RuntimeResult InitializeProfile(const FY01DataCatalog& Catalog, const TArray<FY01DataRef>& BaseAttributeMappings,
        int32 InitialGold, double BaseGoldPerSecond);
    FY01RuntimeResult PrepareSource(const FY01DataCatalog& Catalog, EY01RuntimeSourceKind Kind,
        const FString& LocalKey, const TArray<FY01DataRef>& EffectIds, FY01PreparedSource& OutPlan);
    // 暂存成功后调用方必须提交或回滚；允许在二者之间暂存GA/其他受限服务器业务。
    FY01RuntimeResult StageSource(const FY01PreparedSource& Plan, FY01SourceTransaction& OutTransaction);
    FY01RuntimeResult CommitSource(const FY01SourceTransaction& Transaction);
    FY01RuntimeResult RollbackSource(const FY01SourceTransaction& Transaction);
    FY01RuntimeResult RemoveSource(EY01RuntimeSourceKind Kind, const FString& LocalKey);
    FY01RuntimeResult PrepareMutation(const TArray<FY01ResolvedSource>& Additions,
        const TArray<FY01RuntimeSourceId>& Removals, FY01PreparedMutation& OutPlan);
    FY01RuntimeResult StageMutation(const FY01PreparedMutation& Plan, FY01SourceTransaction& OutTransaction);
    FY01RuntimeResult CommitMutation(const FY01SourceTransaction& Transaction);
    FY01RuntimeResult RollbackMutation(const FY01SourceTransaction& Transaction);
    FY01RuntimeResult Observe(FY01SourceSnapshot& OutSnapshot) const;
    void EndSession();
    bool IsBusy() const { return bBusy || bInCall; }
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    friend class UY01IncomeComponent;
    friend class UY01CombatAbilitySystem;
    friend class UY01AttributeSet;
    friend class UY01LoadoutComponent;
    friend class UY01DamageEffectsComponent;
    friend class UY01EconomyComponent;
    friend class UY01GrowthComponent;
    friend class UY01DropDeliveryComponent;
    friend class FY01ScopedSourceMutation;
    // 只向受信任库存开放：同一时刻结算、共同无失败提交点，随后才广播旧GE删除。
    FY01RuntimeResult SettleMutationTime(double& OutNow);
    FY01RuntimeResult CommitMutationAt(const FY01SourceTransaction& Transaction, TFunctionRef<void()> CommitOwner);
    void StabilizeAttackAggregator(FAggregator& Aggregator) const;
    struct FRecord
    {
        FY01RuntimeSourceId Source;
        TArray<FY01SourceEffectSnapshot> Effects;
        TArray<FActiveGameplayEffectHandle> Handles;
    };
    FY01RuntimeResult Check(bool bAllowBusy = false) const;
    FY01RuntimeResult Interrupt();
    FY01RuntimeResult RequestStop();
    FY01RuntimeResult MakeId(EY01RuntimeSourceKind Kind, const FString& LocalKey, FY01RuntimeSourceId& Out) const;
    FY01RuntimeResult ValidateTotals(const TArray<FY01SourceEffectSnapshot>& Extra, const TSet<FString>& Exclude,
        double& OutRate, double& OutCurrent, FY01ResolvedAttributes* OutAttributes = nullptr,
        TOptional<double> CandidateStrength = {}) const;
    FY01RuntimeResult ValidateHandles(bool bStaged = false) const;
    FY01RuntimeResult ValidatePending() const;
    FY01RuntimeResult StageMutationAt(const FY01PreparedMutation& Plan, FY01SourceTransaction& OutTransaction, double Now);
    FY01RuntimeResult AbortPending();
    bool Owns(const FY01SourceTransaction& Transaction) const;
    void FinishTransaction();
    TWeakObjectPtr<AY01PlayerState> Player;
    TWeakObjectPtr<UY01CombatAbilitySystem> ASC;
    TWeakObjectPtr<UY01IncomeComponent> Income;
    FGuid MatchId;
    int32 JoinSequence = 0;
    uint64 Revision = 0;
    TArray<FRecord> Records;
    TArray<FRecord> Pending;
    TSet<FString> PendingRemoved;
    TArray<FActiveGameplayEffectHandle> Inhibited;
    FGuid PendingToken;
    double PendingRate = 0;
    double PendingTime = 0;
    double StopTime = 0;
    float BeforeBase = 0;
    float BeforeCurrent = 0;
    double PendingCurrent = 0;
    FY01UnitCombatStats BaseAttributes;
    FY01ResolvedAttributes PendingAttributes;
    double PlayerStrength = 0;
    double PendingStrength = 0;
    double DerivedAttackBonus = 0;
    FActiveGameplayEffectHandle DerivedAttackHandle;
    FActiveGameplayEffectHandle PendingDerivedAttackHandle;
    bool bDerivedAttackChanged = false;
    bool bProfileInitialized = false;
    int32 ProfileInitialGold = 0;
    int32 ApplyingRecordIndex = INDEX_NONE;
    int32 ApplyingEffectIndex = INDEX_NONE;
    bool bInitialized = false;
    bool bBusy = false;
    bool bInCall = false;
    bool bInterrupted = false;
    bool bStopRequested = false;
    bool bStopTimeCaptured = false;
    bool bEndCleanupComplete = false;
    bool bEnded = false;
};
