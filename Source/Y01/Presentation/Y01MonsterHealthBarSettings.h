#pragma once

#include "Engine/DeveloperSettings.h"
#include "Y01MonsterHealthBarSettings.generated.h"

struct FY01HealthBarPoolSettings
{
    bool bEnabled = true;
    int32 PrewarmCount = 32;
    int32 Capacity = 128;
    float DeathHoldSeconds = .3f;
};

// 池初始化时读取一次；修改配置后重开客户端生效。
UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Y01普通怪血条"))
class Y01_API UY01MonsterHealthBarSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    UPROPERTY(Config, EditAnywhere, Category="HealthBar") bool bEnabled = true;
    UPROPERTY(Config, EditAnywhere, Category="HealthBar", meta=(ClampMin="0", ClampMax="128")) int32 PrewarmCount = 32;
    UPROPERTY(Config, EditAnywhere, Category="HealthBar", meta=(ClampMin="1", ClampMax="128")) int32 Capacity = 128;
    UPROPERTY(Config, EditAnywhere, Category="HealthBar", meta=(ClampMin="0")) float DeathHoldSeconds = .3f;
    FY01HealthBarPoolSettings Snapshot() const
    {
        FY01HealthBarPoolSettings Result;
        Result.bEnabled = bEnabled; Result.Capacity = FMath::Clamp(Capacity, 1, 128);
        Result.PrewarmCount = FMath::Clamp(PrewarmCount, 0, Result.Capacity);
        if (FMath::IsFinite(DeathHoldSeconds)) { Result.DeathHoldSeconds = FMath::Max(0.f, DeathHoldSeconds); }
        else
        {
            static bool bDiagnosed = false;
            if (!bDiagnosed) { bDiagnosed = true; UE_LOG(LogTemp, Display, TEXT("Y01HealthBar：非有限DeathHoldSeconds，回退0.3秒（本进程仅诊断一次）")); }
        }
        return Result;
    }
};
