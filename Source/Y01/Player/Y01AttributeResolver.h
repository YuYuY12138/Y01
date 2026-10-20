#pragma once

#include "Combat/Y01Combat.h"
#include "Player/Y01SourceRuntimeTypes.h"
#include "Y01AttributeResolver.generated.h"

UENUM()
enum class EY01ContributionKind : uint8 { Base, Flat, Percent, Mapping, Growth };

USTRUCT()
struct Y01_API FY01AttributeContribution
{
    GENERATED_BODY()
    UPROPERTY() EY01ContributionKind Kind = EY01ContributionKind::Base;
    UPROPERTY() FString SourceKey;
    UPROPERTY() FName EffectId;
    UPROPERTY() EY01Attribute SourceAttribute = EY01Attribute::None;
    UPROPERTY() double Amount = 0;
    // 固定值/成长/映射使用百分比前单位；百分比使用百分点，不分摊乘后值。
    UPROPERTY() double Contribution = 0;
    UPROPERTY() EY01EffectCondition Condition = EY01EffectCondition::None;
    UPROPERTY() double ConditionThreshold = 0;
    UPROPERTY() bool bActive = true;
};

USTRUCT()
struct Y01_API FY01AttributeBreakdown
{
    GENERATED_BODY()
    UPROPERTY() EY01Attribute Attribute = EY01Attribute::None;
    UPROPERTY() double Final = 0;
    UPROPERTY() double Subtotal = 0;
    UPROPERTY() double PercentTotal = 0;
    UPROPERTY() TArray<FY01AttributeContribution> Contributions;
};

// 无世界、无GAS写入、无上次衍生输入；所有来源攻击统一纳入纯解析。
struct Y01_API FY01ResolvedAttributes
{
    FY01UnitCombatStats Stats;
    double AttackBonus = 0;
    double GoldPerSecond = 0;
    // 仅归一化观测值，不在此处改变钱包或金币收入。
    double GoldGainPercent = 0;
    double CritChance = 0;          // 0-1，合计/100封顶1
    double CritMultiplierBonus = 0; // 倍率值直接相加
    double Lifesteal = 0;           // 非负不封顶
    TArray<FY01AttributeBreakdown> Breakdowns;
};
class Y01_API FY01AttributeResolver
{
public:
    static FY01RuntimeResult Resolve(const FY01UnitCombatStats& Base, const FY01PlayerGrowthTotals& PlayerGrowth,
        const TArray<FY01SourceEffectSnapshot>& Effects, FY01ResolvedAttributes& Out, double HealthRatio = 1);
    static bool ConditionActive(EY01EffectCondition Condition, double Threshold, double HealthRatio);
    static FString Describe(const FY01SourceEffectRow& Effect);
    static FString DescribeBreakdown(const FY01AttributeBreakdown& Breakdown);
    // 伤害/回复类显示：向零截断去小数（12.9→12），非零但不足1显示"<1"；不改任何实际数值。
    static FString WholeText(double Value);
};
