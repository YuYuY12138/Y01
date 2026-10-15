#pragma once

#include "Components/ActorComponent.h"
#include "Player/Y01ItemRuntimeTypes.h"
#include "Y01DropDeliveryComponent.generated.h"

UENUM()
enum class EY01DeliveryState : uint8 { Pending, Delivered, Fault, Cancelled };
UENUM()
enum class EY01DeliveryCancel : uint8 { None, Ended, Logout, EndPlay, Session };

USTRUCT()
struct Y01_API FY01DeliveryRecord
{
    GENERATED_BODY()
    UPROPERTY() FGuid DeliveryId;
    UPROPERTY() FGuid KillEventId;
    UPROPERTY() FGuid MatchId;
    UPROPERTY() int32 JoinSequence = 0;
    UPROPERTY() FGuid ItemId;
    UPROPERTY() FY01DataRef Definition;
    UPROPERTY() FVector Origin = FVector::ZeroVector;
    UPROPERTY() double StartTime = 0;
    UPROPERTY() double ETA = 0;
    UPROPERTY() EY01DeliveryState State = EY01DeliveryState::Pending;
    UPROPERTY() EY01DeliveryCancel CancelReason = EY01DeliveryCancel::None;
    UPROPERTY() EY01ItemError Fault = EY01ItemError::None;
    UPROPERTY() uint32 RetryCount = 0;
    UPROPERTY() double DelaySeconds = 0;
    UPROPERTY() double DeliveredTime = 0;
    UPROPERTY() EY01Quality Quality = EY01Quality::Green;
};

USTRUCT()
struct Y01_API FY01DeliveryLedger
{
    GENERATED_BODY()
    UPROPERTY() bool bEnabled = false;
    UPROPERTY() bool bEnded = false;
    UPROPERTY() FGuid FaultKillEventId;
    UPROPERTY() EY01ItemError Fault = EY01ItemError::None;
    UPROPERTY() TArray<FY01DeliveryRecord> Records;
};

// 仅服务器有效击杀订阅和调度；客户端没有领奖或动画完成入口。
UCLASS()
class Y01_API UY01DropDeliveryComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01DropDeliveryComponent();
    FY01ItemResult Initialize(double FlightSeconds = 1);
    void Schedule();
    // 失守后忽略ETA；未交付、忙事务或故障均返回false，账本原身份保留。
    bool SettleOwnedOnFallen();
    bool IsEnabled() const { return Ledger.bEnabled && !Ledger.bEnded; }
    void EndSession(EY01DeliveryCancel Reason);
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    const FY01DeliveryLedger& GetLedger() const { return Ledger; }
private:
    void Consume(const struct FY01KillEvent& Event);
    FY01ItemResult Deliver(const FGuid& DeliveryId, double Now);
    void Prune();
    UPROPERTY() FY01DeliveryLedger Ledger;
    TWeakObjectPtr<class AY01PlayerState> Player;
    FGuid MatchId;
    int32 JoinSequence = 0;
    double Duration = 1;
    uint64 LastSequence = 0;
    uint64 Mutation = 0;
    FGuid RecentEvents[64];
    FDelegateHandle KillHook;
    bool bScheduling = false;
    bool bFallenSettlement = false;
};
