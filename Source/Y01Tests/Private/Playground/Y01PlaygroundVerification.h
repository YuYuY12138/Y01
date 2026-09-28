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
