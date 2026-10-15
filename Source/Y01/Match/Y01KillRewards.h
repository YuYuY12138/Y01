#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "Player/Y01ItemRuntimeTypes.h"
#include "Player/Y01SourceRuntimeTypes.h"
#include "Y01KillRewards.generated.h"

class AY01Monster;
class AY01PlayerState;
class UY01CombatAbilitySystem;

struct Y01_API FY01KillSource
{
    FY01RuntimeSourceId Source;
    FGuid ItemId;
    TArray<FY01ItemGrowth> Rules;
};
// 只由真实Health执行生成；不是RPC或客户端奖励载荷。
struct Y01_API FY01KillEvent
{
    FGuid KillEventId;
    FGuid MatchId;
    FGuid SpawnId;
    FY01DataRef Definition;
    TWeakObjectPtr<AY01PlayerState> Killer;
    int32 JoinSequence = 0;
    uint64 Sequence = 0;
    uint64 SourceSession = 0;
    uint64 TargetSession = 0;
    float ActualDamage = 0;
    FVector Origin = FVector::ZeroVector;
    double DeathTime = 0;
    TArray<FY01KillSource> Sources;
};
struct FY01KillAuthorization
{
    TWeakObjectPtr<UY01CombatAbilitySystem> Source;
    TWeakObjectPtr<UY01CombatAbilitySystem> Target;
    TWeakObjectPtr<AY01PlayerState> Player;
    FGuid SpawnId;
    uint64 SourceSession = 0;
    uint64 TargetSession = 0;
};

UCLASS()
class Y01_API UY01KillRewards : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    bool Enable(const FGuid& Match);
    FGuid RegisterMonster(AY01Monster* Monster);
    // 消费者只订阅合法事件，不与成长结果耦合；重入事件按致死顺序排队。
    DECLARE_MULTICAST_DELEGATE_OneParam(FOnEffectiveKill, const FY01KillEvent&);
    FOnEffectiveKill OnEffectiveKill;
    void Drain();
    bool HasPending(const AY01PlayerState* Player) const;
    void EndSession();
    virtual void Deinitialize() override;
private:
    friend class UY01AttributeSet;
    FY01KillAuthorization Capture(UY01CombatAbilitySystem* Source, UY01CombatAbilitySystem* Target) const;
    FGuid FreezeDeath(const FY01KillAuthorization& Authorization, float OldHealth, float NewHealth);
    void Complete(const FGuid& EventId);
    struct FSpawn
    {
        FGuid Id;
        FY01DataRef Definition;
        bool bKilled = false;
    };
    struct FPending { FY01KillEvent Event; bool bReady = false; };
    TMap<TWeakObjectPtr<AY01Monster>, FSpawn> Spawns;
    TArray<FPending> Pending;
    FGuid MatchId;
    uint64 Sequence = 0;
    bool bEnabled = false;
    bool bEnded = false;
    bool bDraining = false;
};
