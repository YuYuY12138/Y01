#pragma once
#include "GameFramework/Actor.h"
#include "Y01PlaygroundVerification.generated.h"

// 仅显式验证命令行创建；普通地图和PIE不自动操作或退出。
UCLASS()
class AY01PlaygroundVerification : public AActor
{
    GENERATED_BODY()
public:
    AY01PlaygroundVerification();
    virtual void Tick(float DeltaSeconds) override;
private:
    bool bFinished = false;
    double NextAdvance = 0;
};

class UY01PlaygroundProfile;
// 独立伤害联网验收；只修改瞬态副本，不复用legacy技能验收结论。
UCLASS()
class AY01DamageVerification : public AActor
{
    GENERATED_BODY()
public:
    AY01DamageVerification();
    static bool Enabled();
    static UY01PlaygroundProfile* PrepareProfile(UY01PlaygroundProfile* Source);
    virtual void Tick(float DeltaSeconds) override;
private:
    int32 Phase = 0;
    double NextStep = 0;
    bool bFinished = false;
};
