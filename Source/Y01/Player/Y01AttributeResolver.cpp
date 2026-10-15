#include "Player/Y01AttributeResolver.h"

namespace
{
using A = EY01Attribute;
bool Add(double& Sum, double Value)
{
    const double Next = Sum + Value;
    if (!FMath::IsFinite(Value) || Value < 0 || !FMath::IsFinite(Next)
        || (Value > 0 && (Next == Sum || (Sum > 0 && Next == Value)))) { return false; }
    Sum = Next; return true;
}
bool FloatValue(double V)
{ return FMath::IsFinite(V) && V >= 0 && V <= MAX_flt && (V == 0 || static_cast<float>(V) > 0); }
const TCHAR* Name(A Attribute)
{
    switch (Attribute)
    {
    case A::Strength: return TEXT("力量"); case A::Agility: return TEXT("敏捷"); case A::Wisdom: return TEXT("智慧");
    case A::AttackDamage: return TEXT("攻击"); case A::MaxHealth: return TEXT("生命上限"); case A::Armor: return TEXT("护甲");
    case A::HealthRegen: return TEXT("生命恢复/秒"); case A::GoldPerSecond: return TEXT("金币/秒"); case A::AttackRate: return TEXT("攻击次数/秒");
    default: return TEXT("未支持属性");
    }
}
}
FY01RuntimeResult FY01AttributeResolver::Resolve(const FY01UnitCombatStats& Base, double PlayerStrength,
    const TArray<FY01SourceEffectSnapshot>& Effects, FY01ResolvedAttributes& Out)
{
    Out = {};
    auto Fail = [] { return FY01RuntimeResult::Fail(EY01RuntimeError::Precision, TEXT("属性/系数非法、溢出或正增量被吞掉")); };
    if (!FMath::IsFinite(Base.AttackInterval) || Base.AttackInterval <= 0) { return Fail(); }
    constexpr int32 Count = static_cast<int32>(A::AttackRate) + 1;
    double Values[Count] = {}, Direct[Count] = {}, Coefficients[4][Count] = {};
    auto V = [&](A Attribute) -> double& { return Values[static_cast<int32>(Attribute)]; };
    auto AddValue = [&](int32 Index, double Value)
    {
        const double Before = Values[Index];
        if (!Add(Values[Index], Value)) { return false; }
        return Index == static_cast<int32>(A::GoldPerSecond) || (FloatValue(Values[Index])
            && (Value == 0 || (static_cast<float>(Values[Index]) > static_cast<float>(Before)
                && (Before == 0 || static_cast<float>(Values[Index]) != static_cast<float>(Value)))));
    };
    V(A::Strength) = Base.Strength; V(A::Agility) = Base.Agility; V(A::Wisdom) = Base.Wisdom;
    V(A::MaxHealth) = Base.Health; V(A::Armor) = Base.Armor; V(A::HealthRegen) = Base.HealthRegen;
    V(A::AttackRate) = 1. / Base.AttackInterval;
    for (double Value : Values) { if (!FloatValue(Value)) { return Fail(); } }
    if (!FloatValue(Base.AttackDamage) || !AddValue(static_cast<int32>(A::Strength), PlayerStrength)) { return Fail(); }
    for (const auto& E : Effects)
    {
        if (!Y01Data::ValidSourceEffect(E)) { return Fail(); }
        switch (E.Kind)
        {
        case EY01SourceEffectKind::AttackFlat: break;
        case EY01SourceEffectKind::GoldPerSecond:
            if (!Add(V(A::GoldPerSecond), E.Magnitude)) { return Fail(); } break;
        case EY01SourceEffectKind::AttributeFlat:
            if (!Add(Direct[static_cast<int32>(E.TargetAttribute)], E.Amount)) { return Fail(); } break;
        case EY01SourceEffectKind::MappingCoefficientAdd:
            if (!Add(Coefficients[static_cast<int32>(E.SourceAttribute)][static_cast<int32>(E.TargetAttribute)], E.Amount)) { return Fail(); } break;
        default: return Fail();
        }
    }
    for (int32 I = 1; I < Count; ++I) { if (!AddValue(I, Direct[I])) { return Fail(); } }
    for (int32 Target = static_cast<int32>(A::AttackDamage); Target < Count; ++Target)
    {
        for (int32 Source = 1; Source <= 3; ++Source)
        {
            const double Product = Values[Source] * Coefficients[Source][Target];
            if ((Values[Source] > 0 && Coefficients[Source][Target] > 0 && Product == 0) || !AddValue(Target, Product)) { return Fail(); }
        }
    }
    Out.AttackBonus = V(A::AttackDamage);
    if (!AddValue(static_cast<int32>(A::AttackDamage), Base.AttackDamage)) { return Fail(); }
    for (int32 I = 1; I < Count; ++I)
    { if (I != static_cast<int32>(A::GoldPerSecond) && !FloatValue(Values[I])) { return Fail(); } }
    const double Interval = 1. / V(A::AttackRate);
    if (!FloatValue(Interval) || Interval <= 0
        || (V(A::AttackRate) > 1. / Base.AttackInterval && static_cast<float>(Interval) >= Base.AttackInterval)) { return Fail(); }
    Out.Stats = Base; Out.Stats.Strength = static_cast<float>(V(A::Strength)); Out.Stats.Agility = static_cast<float>(V(A::Agility));
    Out.Stats.Wisdom = static_cast<float>(V(A::Wisdom)); Out.Stats.AttackDamage = static_cast<float>(V(A::AttackDamage));
    Out.Stats.Health = static_cast<float>(V(A::MaxHealth)); Out.Stats.Armor = static_cast<float>(V(A::Armor));
    Out.Stats.HealthRegen = static_cast<float>(V(A::HealthRegen)); Out.Stats.AttackInterval = static_cast<float>(Interval);
    Out.GoldPerSecond = V(A::GoldPerSecond); return {};
}
FString FY01AttributeResolver::Describe(const FY01SourceEffectRow& E)
{
    switch (E.Kind)
    {
    case EY01SourceEffectKind::AttackFlat: return FString::Printf(TEXT("攻击 +%g"), E.Magnitude);
    case EY01SourceEffectKind::GoldPerSecond: return FString::Printf(TEXT("金币/秒 +%g"), E.Magnitude);
    case EY01SourceEffectKind::AttributeFlat: return FString::Printf(TEXT("%s +%g"), Name(E.TargetAttribute), E.Amount);
    case EY01SourceEffectKind::MappingCoefficientAdd:
        return FString::Printf(TEXT("每点%s → %s 系数 +%g"), Name(E.SourceAttribute), Name(E.TargetAttribute), E.Amount);
    default: return TEXT("未支持效果");
    }
}
