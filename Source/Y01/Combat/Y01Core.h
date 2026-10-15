#pragma once
#include "GameFramework/Pawn.h"
#include "AbilitySystemInterface.h"
#include "Combat/Y01Combat.h"
#include "Blueprint/UserWidget.h"
#include "Y01Core.generated.h"

class UCapsuleComponent;
class UStaticMeshComponent;
class UWidgetComponent;
class UProgressBar;
class UTextBlock;

// 同包服务器快照；生命唯一写源为ASC，表现不使用客户端血差或本地时钟。
USTRUCT()
struct Y01_API FY01CoreSnapshot
{
    GENERATED_BODY()
    UPROPERTY() bool bInitialized = false;
    UPROPERTY() bool bFallen = false;
    UPROPERTY() bool bFrozen = false;
    UPROPERTY() float Health = 0;
    UPROPERTY() float MaxHealth = 0;
    UPROPERTY() FY01CoreSettings Settings;
    UPROPERTY() uint64 HitSequence = 0;
    UPROPERTY() double LastHitTime = -1;
    UPROPERTY() uint64 AlertSequence = 0;
    UPROPERTY() double LastAlertTime = -1;
    UPROPERTY() double ServerSampleTime = 0;
    bool IsCritical() const { return bInitialized && !bFallen && MaxHealth > 0 && Health / MaxHealth < Settings.CriticalHealthRatio; }
    bool IsHitFlash() const { return HitSequence > 0 && ServerSampleTime >= LastHitTime && ServerSampleTime - LastHitTime < .3; }
    FString AlertText() const;
    bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
};
template<> struct TStructOpsTypeTraits<FY01CoreSnapshot> : TStructOpsTypeTraitsBase2<FY01CoreSnapshot>
{ enum { WithNetSerializer = true }; };

UCLASS()
class Y01_API UY01CoreHealthWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void Present(const FY01CoreSnapshot& State);
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
private:
    UPROPERTY() TObjectPtr<UProgressBar> Bar;
    UPROPERTY() TObjectPtr<UTextBlock> Label;
};

UCLASS()
class Y01_API AY01Core : public APawn, public IAbilitySystemInterface
{
    GENERATED_BODY()
public:
    AY01Core();
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystem; }
    UY01CombatAbilitySystem* GetCombatASC() const { return AbilitySystem; }
    // 每局唯一地图Pawn，由主线注册者调用一次；拒绝重置、换表及跨波重新初始化。
    bool TryInitializeCore(const FY01DataCatalog& Catalog, const FY01DataRef& Rules, FString& Error);
    const FY01CoreSnapshot& GetSnapshot() const { return Snapshot; }
    void Freeze();
    FSimpleMulticastDelegate OnFallen;
    // 仅出生搜索锚点，后段必须在周边做碰撞/地面安全查询，不能直接当安全复活点。
    FTransform GetReviveAnchorTransform() const { return GetActorTransform(); }
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
private:
    friend class UY01AttributeSet;
    void RecordHealthChange(bool bActualDamage, bool bDied);
    void Sample();
    UFUNCTION() void OnRep_Snapshot();
    UPROPERTY(VisibleAnywhere) TObjectPtr<UCapsuleComponent> Capsule;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Shape;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UWidgetComponent> HealthWidget;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01CombatAbilitySystem> AbilitySystem;
    UPROPERTY() TObjectPtr<UY01AttributeSet> Attributes;
    UPROPERTY(ReplicatedUsing=OnRep_Snapshot) FY01CoreSnapshot Snapshot;
};
