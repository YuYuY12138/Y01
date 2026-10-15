#pragma once

#include "Combat/Y01Combat.h"
#include "Player/Y01SourceRuntimeTypes.h"

// 无世界、无GAS写入、无上次衍生输入；旧AttackFlat仍由原来源GE负责。
struct Y01_API FY01ResolvedAttributes
{
    FY01UnitCombatStats Stats;
    double AttackBonus = 0;
    double GoldPerSecond = 0;
};
class Y01_API FY01AttributeResolver
{
public:
    static FY01RuntimeResult Resolve(const FY01UnitCombatStats& Base, double PlayerStrength,
        const TArray<FY01SourceEffectSnapshot>& Effects, FY01ResolvedAttributes& Out);
    static FString Describe(const FY01SourceEffectRow& Effect);
};
