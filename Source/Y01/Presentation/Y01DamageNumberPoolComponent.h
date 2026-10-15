#pragma once

#include "Components/ActorComponent.h"
#include "Combat/Y01DamageEvents.h"
#include "Y01DamageNumberPoolComponent.generated.h"

class AY01PlayerController;
class UY01DamageNumberWidget;
class UY01DamageWorldWidgetComponent;

USTRUCT()
struct FY01DamageNumberEntry
{
    GENERATED_BODY()
    UPROPERTY(Transient) TObjectPtr<AActor> Actor;
    UPROPERTY(Transient) TObjectPtr<UY01DamageWorldWidgetComponent> Display;
    UPROPERTY(Transient) TObjectPtr<UY01DamageNumberWidget> Widget;
    FY01DamageEvent Event;
    double Age = 0;
    uint64 BorrowSerial = 0;
    bool bActive = false;
};

// 每本地Controller一份，只有客户端展示数据；不反向依赖测试模块。
UCLASS()
class Y01_API UY01DamageNumberPoolComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01DamageNumberPoolComponent();
    bool InitializePool();
    void Shutdown();
    bool IsReadyFor(const UWorld* World) const { return !bShutdown && PoolWorld.Get() == World; }
    virtual void TickComponent(float DeltaSeconds, ELevelTick TickType, FActorComponentTickFunction* TickFunction) override;
protected:
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
    virtual void OnComponentDestroyed(bool bDestroyingHierarchy) override;
private:
    static constexpr int32 PrewarmCount = 32;
    static constexpr int32 Capacity = 128;
    static constexpr double Lifetime = .8;
    UPROPERTY(Transient) TArray<FY01DamageNumberEntry> Entries;
    TWeakObjectPtr<AY01PlayerController> Controller;
    TWeakObjectPtr<UWorld> PoolWorld;
    FDelegateHandle ReceivedHandle, ResetHandle, CleanupHandle;
    uint64 NextBorrowSerial = 0;
    bool bShutdown = false;
    bool Grow();
    void Receive(const FY01DamageEvent& Event);
    void ResetDisplay();
    void Return(FY01DamageNumberEntry& Entry);
    void Update(FY01DamageNumberEntry& Entry, float DeltaSeconds);
    void WorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources);
};
