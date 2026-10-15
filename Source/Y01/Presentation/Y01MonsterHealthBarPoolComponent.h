#pragma once

#include "Components/ActorComponent.h"
#include "Player/Y01PlayerController.h"
#include "Presentation/Y01MonsterHealthBarSettings.h"
#include "Y01MonsterHealthBarPoolComponent.generated.h"

class AY01Monster;
class UY01MonsterHealthBarWorldWidgetComponent;
class UY01MonsterHealthBarWidget;
class ULevel;
struct FOnAttributeChangeData;

USTRUCT()
struct FY01MonsterHealthBarLease
{
    GENERATED_BODY()
    UPROPERTY(Transient) TObjectPtr<AActor> Actor;
    UPROPERTY(Transient) TObjectPtr<UY01MonsterHealthBarWorldWidgetComponent> Display;
    UPROPERTY(Transient) TObjectPtr<UY01MonsterHealthBarWidget> Widget;
    TWeakObjectPtr<AY01Monster> Target;
    FDelegateHandle HealthHandle, MaxHealthHandle;
    uint64 Version = 0;
    uint64 AuthorizationGeneration = 0;
    double DeathDeadline = 0;
    bool bDeathHolding = false;
};

struct FY01HealthBarCandidate
{
    FDelegateHandle ChangedHandle;
    uint64 Sequence = 0;
};

// 每个本地Controller独占；只消费持久事实和ASC，绝不写回战斗状态。
UCLASS()
class Y01_API UY01MonsterHealthBarPoolComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01MonsterHealthBarPoolComponent();
    bool InitializePool();
    void Shutdown();
    bool IsReadyFor(const UWorld* World) const { return !bShutdown && PoolWorld.IsValid() && PoolWorld.Get() == World; }
    virtual void TickComponent(float DeltaSeconds, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
protected:
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
    virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
private:
    UPROPERTY(Transient) TArray<FY01MonsterHealthBarLease> Entries;
    TMap<TWeakObjectPtr<AY01Monster>, FY01HealthBarCandidate> Candidates;
    TWeakObjectPtr<AY01PlayerController> Controller;
    TWeakObjectPtr<UWorld> PoolWorld;
    FY01HealthBarPoolSettings Settings;
    FY01HealthBarAuthorization Authorization;
    FDelegateHandle SpawnHandle, DestroyHandle, CleanupHandle, AuthorizationHandle, LevelHandle;
    uint64 NextCandidateSequence = 0;
    uint64 NextLeaseVersion = 0;
    double Clock = 0;
    double NextSelection = 0;
    bool bShutdown = false;
    bool Grow();
    void RegisterCandidate(AActor* Actor);
    void RemoveCandidate(AActor* Actor);
    void CandidateChanged(TWeakObjectPtr<AY01Monster> Target);
    void AuthorizationChanged();
    void LevelAdded(ULevel* Level, UWorld* World);
    void WorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
    void SelectCandidates();
    void Borrow(int32 Index, AY01Monster* Target);
    void Return(FY01MonsterHealthBarLease& Lease);
    bool ValidateLease(FY01MonsterHealthBarLease& Lease);
    bool BuildView(FMatrix& OutViewProjection, FVector& OutEye) const;
    bool InView(const AY01Monster* Target, const FMatrix& ViewProjection) const;
    FVector Anchor(const AY01Monster* Target) const;
    void Update(FY01MonsterHealthBarLease& Lease, float DeltaSeconds);
    void AttributeChanged(const FOnAttributeChangeData& Data, int32 Index, TWeakObjectPtr<AY01Monster> Target, uint64 Version, uint64 Generation);
};
