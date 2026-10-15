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
};
