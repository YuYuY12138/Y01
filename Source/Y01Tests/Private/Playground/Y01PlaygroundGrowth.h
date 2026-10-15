#pragma once
#include "Components/ActorComponent.h"
#include "Data/Y01DataTypes.h"
#include "Y01PlaygroundGrowth.generated.h"

class AY01PlayerState;
class AY01Monster;
struct FOnAttributeChangeData;

USTRUCT()
struct FY01GrowthView
{
    GENERATED_BODY()
    UPROPERTY() FY01R33TestGrowth Config;
    UPROPERTY() FString StopReason = TEXT("等待有效玩家");
    UPROPERTY() FString Error;
    UPROPERTY() int64 EffectiveSeconds = 0;
    UPROPERTY() double SecondRemainder = 0;
    UPROPERTY() int64 GrowthKills = 0;
    UPROPERTY() double TimeAttackGranted = 0;
    UPROPERTY() double KillAttackGranted = 0;
    UPROPERTY() int64 TimeGoldGranted = 0;
    UPROPERTY() double GoldRemainder = 0;
};

struct FY01GrowthLedger
{
    FY01GrowthView View;
    double LastSample = 0;
    bool bRunning = false;
    bool bStopped = false;
};
struct FY01GrowthMonster
{
    TWeakObjectPtr<AY01PlayerState> Owner;
    bool bTraining = false;
    bool bConsumed = false;
};

// 仅测试GameMode创建；没有RPC、正式科技/装备或训练Session依赖。
UCLASS()
class UY01PlaygroundGrowth : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01PlaygroundGrowth();
    void Update();
    void RegisterMonster(AY01Monster* Monster, AY01PlayerState* TrainingOwner = nullptr);
    void StopPlayer(AY01PlayerState* Player, const FString& Reason);
    void StopAll(const FString& Reason);
    FY01GrowthView Observe(const AY01PlayerState* Player) const;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    TMap<TWeakObjectPtr<AY01PlayerState>, FY01GrowthLedger> Ledgers;
    TMap<TWeakObjectPtr<AY01Monster>, FY01GrowthMonster> Monsters;
    bool bStopped = false;
    void Advance(AY01PlayerState* Player, FY01GrowthLedger& Ledger, double Now);
    void OnMonsterHealth(TWeakObjectPtr<AY01Monster> Monster, const FOnAttributeChangeData& Data);
    void GrantAttack(AY01PlayerState* Player, FY01GrowthView& View, double Delta, double& Total, int64 Count = 1);
    void GrantGold(AY01PlayerState* Player, FY01GrowthView& View, double Seconds);
};
