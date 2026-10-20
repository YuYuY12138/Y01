#pragma once
#include "Components/ActorComponent.h"
#include "GameFramework/Actor.h"
#include "Data/Y01DataTypes.h"
#include "Y01TrainingRooms.generated.h"

class AY01PlayerState;
class AY01PlaygroundController;
class AY01Monster;
class UY01CombatAbilitySystem;
class ACharacter;

USTRUCT()
struct FY01TrainingView
{
    GENERATED_BODY()
    UPROPERTY() bool bInside = false;
    UPROPERTY() int32 Room = 0;
    UPROPERTY() int32 Session = 0;
    UPROPERTY() int32 Stage = 0;
    UPROPERTY() int32 Alive = 0;
    UPROPERTY() int32 MaxAlive = 0;
    UPROPERTY() int32 Kills = 0;
    UPROPERTY() double EnteredAt = 0;
    UPROPERTY() double NextBatchAt = 0;
    UPROPERTY() FString Error;
};

UCLASS()
class AY01TrainingRoomGeometry : public AActor
{
    GENERATED_BODY()
public:
    AY01TrainingRoomGeometry();
};

struct FY01TrainingMonsterRecord
{
    TWeakObjectPtr<AY01Monster> Monster;
    TWeakObjectPtr<AActor> Visual;
    FY01RewardPackageRow Reward;
    bool bSettled = false;
};
struct FY01TrainingSession
{
    FY01TrainingView View;
    FTransform ReturnTransform;
    TWeakObjectPtr<AY01TrainingRoomGeometry> Geometry;
    TArray<FY01TrainingMonsterRecord> Records;
    FTimerHandle BatchTimer;
    TArray<FTimerHandle> PendingTimers;
    int32 Pending = 0;
};

// 每名玩家独立的权威会话；不占用主线波怪容器或主线计时器。
UCLASS()
class UY01TrainingRooms : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01TrainingRooms();
    bool Request(AY01PlaygroundController* Controller, bool bEnter);
    FY01TrainingView Observe(const AY01PlayerState* Player) const;
    TArray<AY01Monster*> Monsters(const AY01PlayerState* Player) const;
    void Remove(AY01PlayerState* Player);
    void StopAll();
    void ReturnPlayer(AY01PlayerState* Player);
    void Update();
    void BindCombat(UY01CombatAbilitySystem* ASC);
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    // 真实对局入房先等一个生成间隔；既有非计时自动化夹具保持入房即刷首批，练功计时用例显式关闭。
    bool bImmediateFirstBatchForTest = GIsAutomationTesting;
    FVector BaseReturnLocation(const ACharacter* Pawn, const FVector& Fallback) const;
private:
    bool PairAllowed(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target) const;
    const FY01TrainingStageRow* StageFor(const FY01TrainingSession& Session) const;
    FVector Center(const FY01TrainingSession& Session) const;
    void Leave(AY01PlayerState* Player, FY01TrainingSession& Session, bool bReturn);
    void Batch(TWeakObjectPtr<AY01PlayerState> Player, int32 SessionId);
    void Spawn(TWeakObjectPtr<AY01PlayerState> Player, int32 SessionId, FY01SpawnEntry Entry, FY01RewardPackageRow Reward);
    void OnHealth(TWeakObjectPtr<AY01PlayerState> Player, int32 SessionId, TWeakObjectPtr<AY01Monster> Monster, const struct FOnAttributeChangeData& Data);
    TMap<TWeakObjectPtr<AY01PlayerState>, FY01TrainingSession> Sessions;
    int32 NextRoom = 0;
    bool bStopped = false;
};
