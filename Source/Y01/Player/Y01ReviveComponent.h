#pragma once

#include "Components/ActorComponent.h"
#include "Y01ReviveComponent.generated.h"

class AY01PlayerState;
class ACharacter;

// 基础波次规则与可替换方案分离；策略只在死亡边界执行一次，不接收客户端参数。
struct Y01_API FY01ReviveContext
{
    int32 WaveIndex = INDEX_NONE;
    double BaseWaitSeconds = 0;
    double DeathTime = 0;
    FTransform DeathTransform;
};
struct Y01_API FY01RevivePlan
{
    double WaitSeconds = 0;
    bool bAtDeathLocation = false;
    float HealthRatio = 1;
    float HealthValue = 0;
};

USTRUCT()
struct Y01_API FY01ReviveSnapshot
{
    GENERATED_BODY()
    UPROPERTY() uint64 DeathSequence = 0;
    UPROPERTY() bool bWaiting = false;
    UPROPERTY() bool bCancelled = false;
    UPROPERTY() int32 WaveIndex = INDEX_NONE;
    UPROPERTY() double DeathTime = 0;
    UPROPERTY() double Deadline = 0;
    UPROPERTY() double ServerSampleTime = 0;
    bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
};
template<> struct TStructOpsTypeTraits<FY01ReviveSnapshot> : TStructOpsTypeTraitsBase2<FY01ReviveSnapshot>
{ enum { WithNetSerializer = true }; };

UCLASS()
class Y01_API UY01ReviveComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01ReviveComponent();
    void RecordDeath();
    void Update();
    void Cancel();
    bool BlocksActions() const { return Snapshot.bWaiting || Snapshot.bCancelled; }
    bool LocksDeadHealth() const { return DeathPawn.IsValid() && BlocksActions() && !bRestoringHealth; }
    const FY01ReviveSnapshot& GetSnapshot() const { return Snapshot; }
    TFunction<void(const FY01ReviveContext&, FY01RevivePlan&)> PlanPolicy;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
private:
    bool FindSafeLocation(ACharacter* Pawn, FVector& OutLocation) const;
    bool TryExecute();
    AY01PlayerState* Player() const;
    UPROPERTY(Replicated) FY01ReviveSnapshot Snapshot;
    FY01ReviveContext Context;
    FY01RevivePlan Plan;
    TWeakObjectPtr<ACharacter> DeathPawn;
    bool bRecording = false;
    bool bExecuting = false;
    bool bRestoringHealth = false;
};
