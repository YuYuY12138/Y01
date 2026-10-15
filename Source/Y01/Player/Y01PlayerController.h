#pragma once

#include "GameFramework/PlayerController.h"
#include "InputActionValue.h"
#include "Combat/Y01DamageEvents.h"
#include "Y01PlayerController.generated.h"

class UInputAction;
class UInputMappingContext;
class UY01DamageNumberPoolComponent;
class UY01MonsterHealthBarPoolComponent;
DECLARE_MULTICAST_DELEGATE(FY01DamageDisplayReset);
struct FY01MonsterPresentationState;

USTRUCT()
struct Y01_API FY01HealthBarAuthorization
{
    GENERATED_BODY()
    UPROPERTY() bool bEnabled = false;
    UPROPERTY() FGuid WorldEpoch;
    UPROPERTY() int32 RegionId = INDEX_NONE;
    UPROPERTY() int32 RegionSession = 0;
    UPROPERTY() uint64 Generation = 0;
    bool IsValidScope() const;
    bool Matches(const FY01MonsterPresentationState& State) const;
    bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
};
template<> struct TStructOpsTypeTraits<FY01HealthBarAuthorization> : TStructOpsTypeTraitsBase2<FY01HealthBarAuthorization>
{ enum { WithNetSerializer = true }; };

UCLASS()
class Y01_API AY01PlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    void RequestSkillSlot(int32 Slot);
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    // 玩法显式接入；与伤害数字RPC、受众、final事件完全独立。
    void SetHealthBarRegion(int32 RegionId, int32 RegionSession);
    void RevokeHealthBarAuthorization();
    FY01HealthBarAuthorization GetHealthBarAuthorization() const;
    FSimpleMulticastDelegate OnHealthBarAuthorizationChanged;
    virtual void Tick(float DeltaSeconds) override;
    virtual void PreClientTravel(const FString& PendingURL, ETravelType TravelType, bool bIsSeamlessTravel) override;
    virtual void PostSeamlessTravel() override;
    void SetDamageRegion(int32 RegionId, bool bPreserveFinalDamage = false);
    void QueueDamageEvent(const FY01DamageAudience& Audience, const FY01DamageEvent& Event);
    const FY01DamageAudience& GetDamageAudience() const { return ServerDamageAudience; }
    const FY01DamageAudience& GetReceivedDamageAudience() const { return ClientDamageAudience; }
    FY01DamageEventDelegate OnDamageReceived;
    FY01DamageDisplayReset OnDamageDisplayReset;
    const UInputMappingContext* GetKeyboardMapping() const { return KeyboardMapping; }
    const UInputAction* GetMoveAction() const { return MoveAction; }
    const UInputAction* GetSkillAction(int32 Slot) const { return SkillActions.IsValidIndex(Slot) ? SkillActions[Slot] : nullptr; }
protected:
    bool bDamageEventsEnabled = false;
    bool bHealthBarsEnabled = false;
    virtual void SetupInputComponent() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
    UPROPERTY(ReplicatedUsing=OnRep_HealthBarAuthorization) FY01HealthBarAuthorization HealthBarAuthorization;
    UFUNCTION() void OnRep_HealthBarAuthorization();
    FY01HealthBarAuthorization AcceptedHealthBarAuthorization;
    TWeakObjectPtr<UWorld> HealthBarWorld;
    TSet<FGuid> RetiredHealthBarEpochs;
    uint64 HighestHealthBarGeneration = 0;
    bool bHealthBarTravelling = false;
    bool bHealthBarEnded = false;
    void EnsureHealthBarWorld();
    void ClearHealthBarsForTravel();
    void PublishHealthBarAuthorization();
    void EnsureHealthBarPool();
    void ReleaseHealthBarPool();
    UPROPERTY(Transient) TObjectPtr<UY01MonsterHealthBarPoolComponent> HealthBarPool;
    UFUNCTION(Client, Reliable) void ClientSetDamageAudience(const FY01DamageAudience& Audience);
    UFUNCTION(Client, Unreliable) void ClientDamageBatch(const FY01DamageAudience& Audience, const TArray<FY01DamageEvent>& Events);
    friend class UY01DamageEvents;
    void ResetDamageDisplay(uint64 KeepReceivedId = 0);
    void StageFinalDamage(const FY01DamageAudience& Audience, const FY01DamageEvent& Event);
    FY01DamageAudience FinalDamageAudience;
    FY01DamageEvent FinalDamageEvent;
    bool bFinalDamageCommitted = false;
    void EnsureDamageNumberPool();
    void ReleaseDamageNumberPool();
    UPROPERTY(Transient) TObjectPtr<UY01DamageNumberPoolComponent> DamageNumberPool;
    void ClearDamageForTravel();
    bool IsDamageTravelBlocked();
    TWeakObjectPtr<UWorld> DamageTravelWorld;
    FGuid RetiredDamageEpoch;
    bool bDamageTravelling = false;
    FY01DamageAudience ServerDamageAudience, ClientDamageAudience;
    TArray<FY01DamageEvent> PendingDamageEvents;
    TSet<uint64> ReceivedDamageIds;
    uint64 HighestDamageId = 0;
    double NextDamageBatchAt = 0;
    bool bSendingDamageBatch = false;
    bool bDamageEnded = false;
    void BuildKeyboardMapping();
    void InstallMapping();
    void Move(const FInputActionValue& Value);
    void Skill(const FInputActionValue& Value, int32 Slot);
    UFUNCTION(Server, Reliable) void ServerRequestSkillSlot(int32 Slot);
    UPROPERTY(Transient) TObjectPtr<UInputMappingContext> KeyboardMapping;
    UPROPERTY(Transient) TObjectPtr<UInputAction> MoveAction;
    UPROPERTY(Transient) TArray<TObjectPtr<UInputAction>> SkillActions;
};
