#pragma once
#include "Match/Y01GameFlow.h"
#include "Player/Y01Character.h"
#include "GameFramework/Actor.h"
#include "Playground/Y01PlaygroundProfile.h"
#include "Y01PlaygroundRuntime.generated.h"

class UStaticMeshComponent;
class UY01CombatAbilitySystem;
class AY01PlaygroundController;

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
    UPROPERTY(Replicated) TObjectPtr<AY01Monster> Target;
    UPROPERTY(Replicated) FY01DataRef Definition;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UStaticMeshComponent> Shape;
private:
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
    UY01PlaygroundProfile* GetProfile() const { return LoadedProfile; }
    bool RequestAdvance(AY01PlaygroundController* Controller);
    bool RequestRestart(AY01PlaygroundController* Controller);
    int32 AliveCount() const;
protected:
    virtual bool LoadMatchConfiguration(FString& Error) override;
    virtual bool CanCompleteWave() const override;
    virtual bool CanStartWave() const override;
    virtual FTransform GetMonsterSpawnTransform(int32 Ordinal) const override;
    virtual void OnWaveMonsterSpawned(AY01Monster* Monster) override;
    virtual void OnWaveRewardCommitted() override;
private:
    void ClearTargets();
    UPROPERTY() TObjectPtr<UY01PlaygroundProfile> LoadedProfile;
    bool bGameplay = false;
    bool bFinishNextTick = false;
    bool bRestartRequested = false;
};

namespace Y01Playground
{
    float Health(const UY01CombatAbilitySystem* ASC);
    void ColorShape(UStaticMeshComponent* Shape, const FLinearColor& Color);
}
