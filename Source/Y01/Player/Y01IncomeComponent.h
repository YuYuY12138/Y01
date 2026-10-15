#pragma once

#include "Components/ActorComponent.h"
#include "Player/Y01SourceRuntimeTypes.h"
#include "Player/Y01ItemRuntimeTypes.h"
#include "Y01IncomeComponent.generated.h"

class AY01PlayerState;

UCLASS()
class Y01_API UY01IncomeComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01IncomeComponent();
    // 普通C++服务器入口，不是RPC；时间只在模块内部从DS单调时钟捕获。
    FY01RuntimeResult Start();
    FY01RuntimeResult Settle();
    FY01RuntimeResult Stop();
    FY01RuntimeResult Pause();
    FY01RuntimeResult Resume();
    bool IsPaused() const { return bPaused; }
    FY01IncomeSnapshot Observe() const;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    friend class UY01SourceEffectsComponent;
    friend class UY01EconomyComponent;
    struct FPreparedSpending
    {
        int32 Price = 0;
        double Time = 0;
        int64 Sequence = 0;
        int64 TotalSpending = 0;
        TArray<FY01IncomeEntry> Recent;
    };
    FY01ItemResult PrepareSpending(int32 Price, double Now, FPreparedSpending& Out) const;
    FY01ItemResult ValidateSpending(const FPreparedSpending& Prepared) const;
    void CommitSpending(FPreparedSpending&& Prepared);
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FY01R36EconomyTestAccess;
    friend struct FY01R36DeliveryTestAccess;
#endif
    friend struct FY01R36RuntimeTestAccess;
    FY01RuntimeResult Initialize(const FGuid& Match, int32 JoinSeq, double Remainder);
    FY01RuntimeResult CheckOwner(bool bEnding = false) const;
    FY01RuntimeResult SettleAt(double Now, bool bEnding = false);
    FY01RuntimeResult StopAt(double Now, bool bEnding = false);
    void ChangeRate(double Rate, double Now);
    double ReadNow() const;
    TWeakObjectPtr<AY01PlayerState> Player;
    FGuid MatchId;
    int32 JoinSequence = 0;
    FY01IncomeState State;
    bool bEnded = false;
    bool bPaused = false;
    bool bPausing = false;
#if WITH_DEV_AUTOMATION_TESTS
    TFunction<double()> TestClock;
#endif
};
