#pragma once
#include "Playground/Y01PlaygroundRuntime.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01R36Profile.h"
#include "Player/Y01ItemRuntimeTypes.h"
#include "Y01R36Runtime.generated.h"

// 独立入口，不覆盖旧Config或已有资产；三套内容尚不代表S4完成。
UCLASS()
class AY01R36Config : public AActor
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, Category="R36") TSoftObjectPtr<UY01R36Profile> Baseline;
    UPROPERTY(EditAnywhere, Category="R36") TSoftObjectPtr<UY01R36Profile> Content;
    UPROPERTY(EditAnywhere, Category="R36") TSoftObjectPtr<UY01R36Profile> Compare;
    UY01R36Profile* Resolve(const FString& Selection) const;
};

UCLASS()
class AY01R36GameMode : public AY01PlaygroundGameMode
{
    GENERATED_BODY()
public:
    AY01R36GameMode();
    virtual void Tick(float DeltaSeconds) override;
    virtual void Logout(AController* Exiting) override;
    const FString& GetLoadoutError() const { return LoadoutError; }
protected:
    virtual UY01PlaygroundProfile* SelectProfile(FString& Error) override;
    virtual void OnPlayerInitialized(AY01PlayerState* Player) override;
    virtual bool CanStartWave() const override;
    virtual bool UseAutomaticWaves() const override;
    virtual bool IsAutomaticPlayerReady(const AY01PlayerState* Player) const override;
    virtual bool ShouldEndOnPlayerDeath() const override;
    virtual bool ShouldEndOnFinalWave() const override;
    virtual void OnMatchPhaseChanged(EY01MatchPhase Previous, EY01MatchPhase Current) override;
    virtual void RequestControlledExit();
    UY01R36Profile* BuildFormalProfile(FString& Error);
private:
    TSet<TWeakObjectPtr<AY01PlayerState>> EconomyPlayers;
    FGuid MatchId;
    int32 NextJoin = 0;
    FString LoadoutError;
    float StopPollSeconds = 0;
    bool bControlledExitRequested = false;
    bool bLifecycleDeathInjected = false;
};

class UY01R36Panel;
UCLASS()
class AY01R36Controller : public AY01PlaygroundController
{
    GENERATED_BODY()
public:
    UFUNCTION(Server, Reliable) void ServerItemRequest(FY01ItemRequest Request);
    virtual void ServerPlaygroundAction_Implementation(int32 Action) override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    UY01R36Panel* GetPanel() const { return Panel; }
    const TArray<FString>& GetTrace() const { return Trace; }
private:
    TArray<FString> Trace;
    double NextTraceTime = 0;
    UPROPERTY(Transient) TObjectPtr<UY01R36Panel> Panel;
    bool bPanelEnded = false;
    bool bBurstSent = false;
};
