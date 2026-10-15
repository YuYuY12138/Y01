#pragma once
#include "GameFramework/Actor.h"
#include "Y01MonsterHealthBarVerification.generated.h"

class UY01PlaygroundProfile;
class AY01PlayerController;
class AY01Monster;
class UY01CombatAbilitySystem;
class ACameraActor;

// 仅测试模块；自然场只读，隔离场必须显式传入验证开关。
UCLASS()
class AY01MonsterHealthBarVerification : public AActor
{
    GENERATED_BODY()
public:
    AY01MonsterHealthBarVerification();
    static bool Controlled(const TCHAR* CommandLine);
    static UY01PlaygroundProfile* PrepareProfile(UY01PlaygroundProfile* Source, bool bControlled);
    static FString Observe(AY01PlayerController* PC);
    static bool Damage(UY01CombatAbilitySystem* Source, AY01Monster* Target, float Amount);
    static FVector GridPosition(int32 Index);
    static void Install(UWorld* World);
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
    void Execute(const FString& Action);
    void SetCamera(const FString& Action);
    TArray<TWeakObjectPtr<AY01Monster>> Targets;
    TWeakObjectPtr<ACameraActor> Camera;
    TWeakObjectPtr<AActor> Occluder;
    double NextObserve = 0;
    int32 LastSequence = 0;
    FString LastAction;
    bool bGridCamera = false;
};
