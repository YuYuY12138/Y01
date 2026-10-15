#pragma once
#include "Match/Y01GameFlow.h"
#include "Player/Y01Character.h"
#include "GameFramework/Actor.h"
#include "Playground/Y01PlaygroundProfile.h"
#include "Playground/Y01TrainingRooms.h"
#include "Playground/Y01PlaygroundGrowth.h"
#include "Y01PlaygroundRuntime.generated.h"

class UStaticMeshComponent;
class USkeletalMeshComponent;
class UY01CombatAbilitySystem;
class AY01PlaygroundController;
struct FY01PendingDamage;

UCLASS()
class AY01PlaygroundConfig : public AActor
{
    GENERATED_BODY()
public:
    AY01PlaygroundConfig();
    UPROPERTY(EditAnywhere, Category="Playground") TObjectPtr<UY01PlaygroundProfile> Profile;
};

// 测试外观独立复制，不改变正式怪物类及生成工厂。
UCLASS()
class AY01PlaygroundVisual : public AActor
{
    GENERATED_BODY()
public:
    AY01PlaygroundVisual();
    virtual void Tick(float DeltaSeconds) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    UPROPERTY(ReplicatedUsing=OnRep_Target) TObjectPtr<AY01Monster> Target;
    UPROPERTY(Replicated) FY01DataRef Definition;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Shape;
private:
    void RefreshMeshFollow();
    UFUNCTION() void OnRep_Target();
    TWeakObjectPtr<USkeletalMeshComponent> FollowedMesh;
    FVector MeshLocationOffset = FVector::ZeroVector;
    FQuat MeshRotationOffset = FQuat::Identity;
    bool bStyled = false;
    bool bWasDead = false;
};

UCLASS()
class AY01PlaygroundCharacter : public AY01Character
{
    GENERATED_BODY()
public:
    AY01PlaygroundCharacter();
    virtual void BeginPlay() override;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Shape;
};

UCLASS()
class AY01PlaygroundGameMode : public AY01GameMode
{
    GENERATED_BODY()
public:
    AY01PlaygroundGameMode();
    virtual void Tick(float DeltaSeconds) override;
    virtual void BeginPlay() override;
    void RefreshTarget(AY01PlayerState* Player);
    bool IsCombatRunning() const;
    virtual bool IsCoreDefenseEnabled() const override { return bCoreDefenseEnabled; }
    virtual void OnPlayerDeath(AY01PlayerState* Player) override;
    virtual bool IsGameplayFrozen() const override;
    bool IsPlayerPresent(const AY01PlayerState* Player) const;
    UY01PlaygroundProfile* GetProfile() const { return LoadedProfile; }
    bool RequestAdvance(AY01PlaygroundController* Controller);
    bool RequestRestart(AY01PlaygroundController* Controller);
    virtual bool TryEndMatch(bool bSuccess) override;
    virtual void PostLogin(APlayerController* NewPlayer) override;
    void RefreshHealthBarAuthorization(AY01PlaygroundController* Controller) const;
    virtual void Logout(AController* Exiting) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    int32 AliveCount() const;
    UY01TrainingRooms* GetTraining() const { return Training; }
    UY01PlaygroundGrowth* GetGrowth() const { return Growth; }
protected:
    virtual bool LoadMatchConfiguration(FString& Error) override;
    virtual UY01PlaygroundProfile* SelectProfile(FString& Error);
    virtual bool CanCompleteWave() const override;
    virtual bool CanStartWave() const override;
    virtual FTransform GetMonsterSpawnTransform(int32 Ordinal) const override;
    virtual void OnWaveMonsterSpawned(AY01Monster* Monster) override;
    virtual void OnWaveRewardCommitted() override;
    virtual bool ShouldEndOnPlayerDeath() const { return !IsCoreDefenseEnabled(); }
    // 已同步冻结；第2段在此接失守协调器，先结清确认归属掉落，再进入Ended。
    virtual void OnCoreFallen();
    virtual bool ShouldEndOnFinalWave() const { return true; }
private:
    void ClearTargets();
    bool InitializeMatchCore(FString& Error);
    void FreezeCoreDefense();
    void HandleCoreFallen();
    void SettleCoreFallen();
    void CoreActorSpawned(AActor* Actor);
    UFUNCTION() void CoreEnded(AActor* Actor, EEndPlayReason::Type Reason);
    FDelegateHandle CoreSpawnHandle;
    bool bCoreDefenseEnabled = false;
    bool bCoreDefenseFrozen = false;
    bool bCoreFallen = false;
    bool bSettlingCore = false;
    bool bCoreSettlementComplete = false;
    bool CaptureDamageAudience(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target, FY01PendingDamage& Pending);
    UPROPERTY() TObjectPtr<UY01TrainingRooms> Training;
    UPROPERTY() TObjectPtr<UY01PlaygroundGrowth> Growth;
    UPROPERTY() TObjectPtr<UY01PlaygroundProfile> LoadedProfile;
    TSet<TWeakObjectPtr<const AY01PlayerState>> DepartedPlayers;
    bool bGameplay = false;
    bool bHealthBarShutdown = false;
    bool bFinishNextTick = false;
    bool bRestartRequested = false;
};

namespace Y01Playground
{
    float Health(const UY01CombatAbilitySystem* ASC);
    void ColorShape(UStaticMeshComponent* Shape, const FLinearColor& Color);
}
