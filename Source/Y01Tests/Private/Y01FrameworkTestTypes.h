#pragma once

#include "GameplayEffect.h"
#include "Player/Y01PlayerController.h"
#include "Match/Y01GameFlow.h"
#include "Y01FrameworkTestTypes.generated.h"

UCLASS()
class UY01TestInitialEffect : public UGameplayEffect
{
    GENERATED_BODY()
public:
    UY01TestInitialEffect();
};

UCLASS()
class UY01TestInstantEffect : public UY01TestInitialEffect
{
    GENERATED_BODY()
public:
    UY01TestInstantEffect();
};

UCLASS()
class AY01TestController : public AY01PlayerController
{
    GENERATED_BODY()
public:
    void PrepareInput();
    void Inject(const UInputAction* Action, const FInputActionValue& Value);
};

UCLASS()
class AY01WaveReentryTestMode : public AY01GameMode
{
    GENERATED_BODY()
public:
    int32 RewardCallbacks = 0;
    bool bNestedComplete = false;
    bool bNestedStart = false;
    bool bNestedEnd = false;
protected:
    virtual void OnWaveRewardCommitted() override
    {
        ++RewardCallbacks;
        bNestedComplete = TryCompleteWave();
        bNestedStart = TryStartNextWave();
        bNestedEnd = TryEndMatch(false);
    }
};
