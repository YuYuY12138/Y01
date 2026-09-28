#pragma once
#include "Match/Y01GameFlow.h"
#include "Player/Y01PlayerController.h"
#include "Y01MatchFixture.h"
#include "Engine/StaticMeshActor.h"
#include "Y01Smoke.generated.h"

class AY01SmokeController;
UCLASS()
class AY01SmokeFloor : public AStaticMeshActor
{
    GENERATED_BODY()
public:
    AY01SmokeFloor();
};

UCLASS()
class AY01SmokeProbe : public AActor
{
    GENERATED_BODY()
public:
    AY01SmokeProbe();
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    UPROPERTY(Replicated) int32 Sequence = 0;
    UPROPERTY(Replicated) TObjectPtr<AY01Monster> Core;
    UPROPERTY(Replicated) TObjectPtr<AY01SmokeFloor> Floor;
    UPROPERTY(Replicated) TArray<TObjectPtr<AY01Monster>> Monsters;
    static FString Observe(UWorld* World, const AY01SmokeProbe* Probe);
};

UCLASS()
class AY01SmokeController : public AY01PlayerController
{
    GENERATED_BODY()
public:
    virtual void Tick(float DeltaSeconds) override;
    UFUNCTION(Server, Reliable) void ServerObservation(int32 Sequence, int32 ObserverId, const FString& Observation);
    UFUNCTION(Client, Reliable) void ClientSmokeFinished();
private:
    FString LastObservation;
};

UCLASS()
class AY01SmokeGameMode : public AY01GameMode
{
    GENERATED_BODY()
public:
    AY01SmokeGameMode();
    static bool IsOptedIn(const TCHAR* CommandLine);
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void RestartPlayer(AController* Player) override;
    void AcceptObservation(AY01SmokeController* Controller, int32 Sequence, int32 ObserverId, const FString& Observation);
    bool HasSmokeFixture() const { return Fixture.IsValid(); }
protected:
    virtual bool LoadMatchConfiguration(FString& OutError) override;
    virtual bool CanStartWave() const override;
    virtual bool CanCompleteWave() const override;
    virtual bool CanEnterBattle() const override;
    virtual FTransform GetMonsterSpawnTransform(int32 SpawnOrdinal) const override;
    virtual void OnWaveMonsterSpawned(AY01Monster* Monster) override;
private:
    void Checkpoint(const TCHAR* Name);
    bool Require(bool Value, const TCHAR* Message);
    TArray<AY01SmokeController*> Peers() const;
    UPROPERTY() TObjectPtr<AY01SmokeProbe> Probe;
    TUniquePtr<FY01MatchFixture> Fixture;
    TSet<int32> Acknowledged;
    FString Expected;
    FString CheckpointName;
    FString Scenario;
    bool bEnabled = false;
    bool bFinished = false;
    int32 Step = 0;
    double StartedAt = 0;
    double FinishAt = 0;
};
