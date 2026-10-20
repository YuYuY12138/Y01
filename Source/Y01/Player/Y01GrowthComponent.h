#pragma once

#include "Components/ActorComponent.h"
#include "Match/Y01KillRewards.h"
#include "Y01GrowthComponent.generated.h"

UCLASS()
class Y01_API UY01GrowthComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01GrowthComponent();
    bool Initialize();
    bool IsEnabled() const { return bInitialized && !bEnded; }
    void EndSession();
    // 普攻真正发动（近战判定/飞行物发射）时计数；守卫窗口内调用安全，Tick 统一结算。
    void NotifyBasicAttack();
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    const FY01EffectiveKillGrowthLedger& GetLedger() const { return Ledger; }
    // 装配、成长和完整视图共用同一贡献构造，不修改静态效果。
    static TArray<FY01ItemEffect> EquipmentEffects(const FY01ItemInstance& Item);
private:
    friend class UY01KillRewards;
    friend class UY01SourceEffectsComponent;
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FY01R36GrowthTestAccess;
#endif
    bool FreezeSources(FY01KillEvent& Event);
    bool CanConsume() const;
    void Consume(const FY01KillEvent& Event);
    struct FGrowthBatch
    {
        FY01RuntimeSourceId Source;
        FGuid ItemId;                      // 装备实例批非空
        TArray<FY01ItemGrowth> Rules;      // Rule.Delta 本次增量
    };
    FY01ItemResult ApplyBatches(const TArray<FGrowthBatch>& Batches, const FGuid& KillEventId = {});
    bool SettleTick();
    FY01ItemResult Apply(const FY01KillEvent& Event);
    void Drain();
    bool BlocksMutation() const;
    TWeakObjectPtr<AY01PlayerState> Player;
    FGuid MatchId;
    int32 JoinSequence = 0;
    TSet<FGuid> Consumed;
    UPROPERTY() FY01EffectiveKillGrowthLedger Ledger;
    bool bInitialized = false;
    bool bEnded = false;
    bool bConsuming = false;
    double LastTickAt = 0;
    uint64 NextTickBatchId = 1;
    int32 PendingBasicAttacks = 0;
};
