#include "Player/Y01AttributeResolver.h"

namespace
{
using A = EY01Attribute;
using K = EY01SourceEffectKind;
using C = EY01ContributionKind;
bool Add(double& Sum, double Value)
{
    const double Next = Sum + Value;
    if (!FMath::IsFinite(Value) || Value < 0 || !FMath::IsFinite(Next)
        || (Value > 0 && (Next == Sum || (Sum > 0 && Next == Value)))) { return false; }
    Sum = Next; return true;
}
bool FloatValue(double V)
{ return FMath::IsFinite(V) && V >= 0 && V <= MAX_flt && (V == 0 || static_cast<float>(V) > 0); }
bool Product(double Left, double Right, double& Out)
{
    Out = Left * Right;
    return FMath::IsFinite(Out) && Out >= 0 && !(Left > 0 && Right > 0 && Out == 0);
}
bool Before(const FY01SourceEffectSnapshot& Left, const FY01SourceEffectSnapshot& Right)
{
    if (Left.Source.Kind != Right.Source.Kind) { return Left.Source.Kind < Right.Source.Kind; }
    const int32 Order = Left.Source.LocalKey.Compare(Right.Source.LocalKey, ESearchCase::CaseSensitive);
    return Order != 0 ? Order < 0 : Left.EffectId.LexicalLess(Right.EffectId);
}
bool PercentValue(double Subtotal, double Percent, double& Final)
{
    const double Normalized = Percent / 100.;
    if (!FMath::IsFinite(Normalized) || (Percent > 0 && Normalized == 0)) { return false; }
    double Factor = 1;
    // 零小计没有待保护的正增量；非零小计同时保护因子、double结果及float结果。
    if (Subtotal > 0 && !Add(Factor, Normalized)) { return false; }
    if (Subtotal == 0) { Factor += Normalized; }
    if (!Product(Subtotal, Factor, Final) || !FloatValue(Final)) { return false; }
    return Percent == 0 || Subtotal == 0 || (Final > Subtotal
        && static_cast<float>(Final) > static_cast<float>(Subtotal));
}
const TCHAR* Name(A Attribute)
{
    switch (Attribute)
    {
    case A::Strength: return TEXT("力量"); case A::Agility: return TEXT("敏捷"); case A::Wisdom: return TEXT("智慧");
    case A::AttackDamage: return TEXT("攻击"); case A::MaxHealth: return TEXT("生命上限"); case A::Armor: return TEXT("护甲");
    case A::HealthRegen: return TEXT("生命恢复/秒"); case A::GoldPerSecond: return TEXT("金币/秒"); case A::AttackRate: return TEXT("攻击次数/秒");
    case A::GoldGain: return TEXT("金币获取");
    default: return TEXT("未支持属性");
    }
}
}
bool FY01AttributeResolver::ConditionActive(EY01EffectCondition Condition, double Threshold, double HealthRatio)
{
    if (Condition == EY01EffectCondition::None) { return true; }
    if (!FMath::IsFinite(Threshold) || Threshold < 0 || Threshold > 100
        || !FMath::IsFinite(HealthRatio) || HealthRatio < 0 || HealthRatio > 1) { return false; }
    switch (Condition)
    {
    case EY01EffectCondition::HealthBelow: return HealthRatio < Threshold / 100.;
    case EY01EffectCondition::HealthAbove: return HealthRatio > Threshold / 100.;
    default: return false;
    }
}
FY01RuntimeResult FY01AttributeResolver::Resolve(const FY01UnitCombatStats& Base, const FY01PlayerGrowthTotals& PlayerGrowth,
    const TArray<FY01SourceEffectSnapshot>& Effects, FY01ResolvedAttributes& Out, double HealthRatio)
{
    Out = {};
    auto Fail = [] { return FY01RuntimeResult::Fail(EY01RuntimeError::Precision, TEXT("属性/系数非法、溢出或正增量被吞掉")); };
    if (!FMath::IsFinite(HealthRatio) || HealthRatio < 0 || HealthRatio > 1
        || !FMath::IsFinite(Base.AttackInterval) || Base.AttackInterval <= 0) { return Fail(); }
    constexpr int32 Count = static_cast<int32>(A::GoldGain) + 1;
    double Values[Count] = {}, Percents[Count] = {}, Coefficients[4][Count] = {};
    auto V = [&](A Attribute) -> double& { return Values[static_cast<int32>(Attribute)]; };
    auto AddValue = [&](int32 Index, double Value)
    {
        const double Previous = Values[Index];
        if (!Add(Values[Index], Value)) { return false; }
        return Index == static_cast<int32>(A::GoldPerSecond) || (FloatValue(Values[Index])
            && (Value == 0 || (static_cast<float>(Values[Index]) > static_cast<float>(Previous)
                && (Previous == 0 || static_cast<float>(Values[Index]) != static_cast<float>(Value)))));
    };
    V(A::Strength) = Base.Strength; V(A::Agility) = Base.Agility; V(A::Wisdom) = Base.Wisdom;
    V(A::AttackDamage) = Base.AttackDamage; V(A::MaxHealth) = Base.Health;
    V(A::Armor) = Base.Armor; V(A::HealthRegen) = Base.HealthRegen; V(A::AttackRate) = 1. / Base.AttackInterval;
    for (double Value : Values) { if (!FloatValue(Value)) { return Fail(); } }

    // 只在全部检查成功后发布，失败不泄漏Stats、加值或部分明细。
    FY01ResolvedAttributes Result;
    Result.Breakdowns.SetNum(Count - 1);
    for (int32 I = 1; I < Count; ++I)
    {
        auto& Row = Result.Breakdowns[I - 1]; Row.Attribute = static_cast<A>(I);
        if (Row.Attribute != A::GoldGain)
        {
            FY01AttributeContribution BaseContribution; BaseContribution.SourceKey = TEXT("基础");
            BaseContribution.Amount = Values[I]; BaseContribution.Contribution = Values[I];
            Row.Contributions.Add(MoveTemp(BaseContribution));
        }
    }
    const EY01Attribute DimAttrs[] = { A::Strength, A::Agility, A::Wisdom };
    const double DimAmounts[] = { PlayerGrowth.Strength, PlayerGrowth.Agility, PlayerGrowth.Wisdom };
    for (int32 I = 0; I < 3; ++I)
    {
        if (!AddValue(static_cast<int32>(DimAttrs[I]), DimAmounts[I])) { return Fail(); }
        FY01AttributeContribution Growth; Growth.Kind = C::Growth; Growth.SourceKey = TEXT("玩家成长");
        Growth.Amount = DimAmounts[I]; Growth.Contribution = DimAmounts[I];
        Result.Breakdowns[static_cast<int32>(DimAttrs[I]) - 1].Contributions.Add(MoveTemp(Growth));
    }
    // 生命上限成长与AttributeFlat→MaxHealth同通道：进小计、先于衍生百分比。
    if (!AddValue(static_cast<int32>(A::MaxHealth), PlayerGrowth.MaxHealth)) { return Fail(); }
    FY01AttributeContribution HealthGrowth; HealthGrowth.Kind = C::Growth; HealthGrowth.SourceKey = TEXT("玩家成长");
    HealthGrowth.Amount = PlayerGrowth.MaxHealth; HealthGrowth.Contribution = PlayerGrowth.MaxHealth;
    Result.Breakdowns[static_cast<int32>(A::MaxHealth) - 1].Contributions.Add(MoveTemp(HealthGrowth));

    // 暴击/吸血按百分点独立合计，倍率为倍率值直接相加；护盾种类不进属性结算。
    // 三者不占用按属性索引的结算数组，全部在主循环内按种类分发前旁路。
    double CritPoints = 0, StealPoints = 0, CritBonus = 0;
    auto Sorted = Effects; Sorted.Sort(Before);
    for (const auto& E : Sorted)
    {
        if (!Y01Data::ValidSourceEffect(E)) { return Fail(); }
        // 护盾种类不进属性结算，由后续护盾组件消费；在构造Entry（含SourceKey分配）前旁路，避免空转分配。
        if (E.Kind == K::ShieldPeriodic || E.Kind == K::ShieldOnCast) { continue; }
        A Target = E.TargetAttribute;
        if (E.Kind == K::AttackFlat) { Target = A::AttackDamage; }
        else if (E.Kind == K::GoldPerSecond) { Target = A::GoldPerSecond; }
        const int32 Index = static_cast<int32>(Target);
        FY01AttributeContribution Entry;
        Entry.SourceKey = FString::Printf(TEXT("%d/%s"), static_cast<int32>(E.Source.Kind), *E.Source.LocalKey);
        Entry.EffectId = E.EffectId; Entry.SourceAttribute = E.SourceAttribute;
        Entry.Condition = E.Condition; Entry.ConditionThreshold = E.ConditionThreshold;
        Entry.bActive = ConditionActive(E.Condition, E.ConditionThreshold, HealthRatio);
        Entry.Amount = (E.Kind == K::AttackFlat || E.Kind == K::GoldPerSecond) ? E.Magnitude : E.Amount;
        Entry.Contribution = Entry.bActive ? Entry.Amount : 0;
        // 暴击/倍率/吸血在switch与按属性索引之前旁路：三者独立合计，
        // 避免TargetAttribute=None走进属性数组或明细行（索引-1越界）。
        if (E.Kind == K::CritChance || E.Kind == K::CritMultiplierAdd || E.Kind == K::Lifesteal)
        {
            if (E.Kind == K::CritMultiplierAdd) { if (!Add(CritBonus, Entry.Contribution)) { return Fail(); } }
            else if (E.Kind == K::CritChance) { if (!Add(CritPoints, Entry.Contribution)) { return Fail(); } }
            else { if (!Add(StealPoints, Entry.Contribution)) { return Fail(); } }
            continue;
        }
        switch (E.Kind)
        {
        case K::AttackFlat:
        case K::GoldPerSecond:
        case K::AttributeFlat:
            Entry.Kind = E.bGrowthContribution ? C::Growth : C::Flat;
            if (!AddValue(Index, Entry.Contribution)) { return Fail(); }
            break;
        case K::AttributePercent:
            Entry.Kind = C::Percent;
            if (!Add(Percents[Index], Entry.Contribution)) { return Fail(); }
            break;
        case K::MappingCoefficientAdd:
            Entry.Kind = C::Mapping; Entry.Contribution = 0;
            if (!Add(Coefficients[static_cast<int32>(E.SourceAttribute)][Index], E.Amount)) { return Fail(); }
            break;
        default: return Fail();
        }
        Result.Breakdowns[Index - 1].Contributions.Add(MoveTemp(Entry));
    }
    for (int32 I = static_cast<int32>(A::Strength); I <= static_cast<int32>(A::Wisdom); ++I)
    {
        auto& Row = Result.Breakdowns[I - 1]; Row.Subtotal = Values[I]; Row.PercentTotal = Percents[I];
        if (!PercentValue(Row.Subtotal, Row.PercentTotal, Values[I])) { return Fail(); }
        Row.Final = Values[I];
    }
    for (int32 Target = static_cast<int32>(A::AttackDamage); Target <= static_cast<int32>(A::AttackRate); ++Target)
    {
        for (int32 Source = static_cast<int32>(A::Strength); Source <= static_cast<int32>(A::Wisdom); ++Source)
        {
            double Mapped = 0;
            if (!Product(Values[Source], Coefficients[Source][Target], Mapped) || !AddValue(Target, Mapped)) { return Fail(); }
        }
        auto& Row = Result.Breakdowns[Target - 1];
        for (auto& Entry : Row.Contributions)
        {
            if (Entry.Kind != C::Mapping) { continue; }
            if (!Product(V(Entry.SourceAttribute), Entry.Amount, Entry.Contribution)
                || (Target != static_cast<int32>(A::GoldPerSecond) && !FloatValue(Entry.Contribution))) { return Fail(); }
        }
        Row.Subtotal = Values[Target]; Row.PercentTotal = Percents[Target];
        if (Target != static_cast<int32>(A::GoldPerSecond)
            && !PercentValue(Row.Subtotal, Row.PercentTotal, Values[Target])) { return Fail(); }
        Row.Final = Values[Target];
    }
    auto& GoldGain = Result.Breakdowns[static_cast<int32>(A::GoldGain) - 1];
    GoldGain.PercentTotal = Percents[static_cast<int32>(A::GoldGain)]; GoldGain.Final = GoldGain.PercentTotal;
    Result.GoldGainPercent = GoldGain.PercentTotal / 100.;
    if (!FMath::IsFinite(Result.GoldGainPercent) || (GoldGain.PercentTotal > 0 && Result.GoldGainPercent == 0)) { return Fail(); }

    const double Interval = 1. / V(A::AttackRate);
    if (!FloatValue(Interval) || Interval <= 0
        || (V(A::AttackRate) > 1. / Base.AttackInterval && static_cast<float>(Interval) >= Base.AttackInterval)) { return Fail(); }
    Result.AttackBonus = V(A::AttackDamage) - Base.AttackDamage;
    if (!FloatValue(Result.AttackBonus) || double(Base.AttackDamage) + Result.AttackBonus > MAX_flt) { return Fail(); }
    const float Bonus = static_cast<float>(Result.AttackBonus), GasAttack = Base.AttackDamage + Bonus;
    if (!FMath::IsFinite(GasAttack) || (Result.AttackBonus > 0 && (Bonus == 0 || GasAttack == Base.AttackDamage
        || (Base.AttackDamage > 0 && GasAttack == Bonus)))) { return Fail(); }
    Result.Stats = Base; Result.Stats.Strength = static_cast<float>(V(A::Strength)); Result.Stats.Agility = static_cast<float>(V(A::Agility));
    Result.Stats.Wisdom = static_cast<float>(V(A::Wisdom)); Result.Stats.AttackDamage = static_cast<float>(V(A::AttackDamage));
    Result.Stats.Health = static_cast<float>(V(A::MaxHealth)); Result.Stats.Armor = static_cast<float>(V(A::Armor));
    Result.Stats.HealthRegen = static_cast<float>(V(A::HealthRegen)); Result.Stats.AttackInterval = static_cast<float>(Interval);
    Result.GoldPerSecond = V(A::GoldPerSecond);
    // 百分点合计后/100（暴击封顶1），倍率为倍率值直接相加；对冲除法下溢与金币获取%同款。
    Result.CritChance = FMath::Clamp(CritPoints / 100., 0., 1.);
    Result.CritMultiplierBonus = CritBonus;
    Result.Lifesteal = StealPoints / 100.;
    // 与AttackBonus同款：FloatValue保double可表示为非零有限float（含double→float溢出与下溢），
    // 合计>0而商下溢为0视为精度失败；吸血/100的float溢出理论不可达，并入同型检查。
    if (!FloatValue(Result.CritChance) || (CritPoints > 0 && Result.CritChance == 0)
        || !FloatValue(Result.CritMultiplierBonus)
        || !FloatValue(Result.Lifesteal) || (StealPoints > 0 && Result.Lifesteal == 0)) { return Fail(); }
    Out = MoveTemp(Result); return {};
}
namespace
{
FString ConditionSuffix(EY01EffectCondition Condition, double Threshold)
{
    switch (Condition)
    {
    case EY01EffectCondition::None: return FString();
    case EY01EffectCondition::HealthBelow: return FString::Printf(TEXT("（生命低于%g%%时）"), Threshold);
    case EY01EffectCondition::HealthAbove: return FString::Printf(TEXT("（生命高于%g%%时）"), Threshold);
    default: return TEXT("（未支持条件）");
    }
}
FString DescribeSourceKey(const FString& Key)
{
    int32 Slash = INDEX_NONE;
    if (!Key.FindChar(TEXT('/'), Slash)) { return Key; }
    const FString Prefix = Key.Left(Slash);
    const EY01RuntimeSourceKind Kinds[] = {EY01RuntimeSourceKind::Base, EY01RuntimeSourceKind::Equipment,
        EY01RuntimeSourceKind::Skill, EY01RuntimeSourceKind::Talent, EY01RuntimeSourceKind::Shop, EY01RuntimeSourceKind::Devoured};
    const TCHAR* Labels[] = {TEXT("基础"), TEXT("装备"), TEXT("技能"), TEXT("天赋"), TEXT("商店"), TEXT("已吞噬")};
    for (int32 I = 0; I < UE_ARRAY_COUNT(Kinds); ++I)
    {
        if (Prefix == FString::FromInt(static_cast<int32>(Kinds[I]))) { return FString(Labels[I]) + Key.Mid(Slash); }
    }
    return Key;
}
}
FString FY01AttributeResolver::WholeText(double Value)
{
    if (!FMath::IsFinite(Value)) { return TEXT("?"); }
    if (Value != 0 && FMath::Abs(Value) < 1) { return Value > 0 ? TEXT("<1") : TEXT(">-1"); }
    return FString::Printf(TEXT("%lld"), static_cast<long long>(FMath::TruncToDouble(Value)));
}
FString FY01AttributeResolver::Describe(const FY01SourceEffectRow& E)
{
    FString Text;
    const bool Whole = E.TargetAttribute == A::AttackDamage || E.TargetAttribute == A::HealthRegen;
    switch (E.Kind)
    {
    case EY01SourceEffectKind::AttackFlat: Text = TEXT("攻击 +") + WholeText(E.Magnitude); break;
    case EY01SourceEffectKind::GoldPerSecond: Text = FString::Printf(TEXT("金币/秒 +%g"), E.Magnitude); break;
    case EY01SourceEffectKind::AttributeFlat:
        Text = FString::Printf(TEXT("%s +%s"), Name(E.TargetAttribute), Whole ? *WholeText(E.Amount) : *FString::Printf(TEXT("%g"), E.Amount)); break;
    case EY01SourceEffectKind::AttributePercent: Text = FString::Printf(TEXT("%s +%g%%"), Name(E.TargetAttribute), E.Amount); break;
    case EY01SourceEffectKind::MappingCoefficientAdd:
        Text = FString::Printf(TEXT("每点%s → %s 系数 +%g"), Name(E.SourceAttribute), Name(E.TargetAttribute), E.Amount); break;
    case EY01SourceEffectKind::CritChance: Text = FString::Printf(TEXT("暴击率 +%g%%"), E.Amount); break;
    case EY01SourceEffectKind::CritMultiplierAdd: Text = FString::Printf(TEXT("暴击倍率 +%g"), E.Amount); break;
    case EY01SourceEffectKind::Lifesteal: Text = FString::Printf(TEXT("吸血 +%g%%"), E.Amount); break;
    case EY01SourceEffectKind::ShieldPeriodic:
        Text = FString::Printf(TEXT("每%g秒获得%s护盾，持续%g秒，本层上限%s"), E.IntervalSeconds, *WholeText(E.Amount), E.DurationSeconds, *WholeText(E.LayerCap)); break;
    case EY01SourceEffectKind::ShieldOnCast:
        Text = FString::Printf(TEXT("施放时获得%s护盾，持续%g秒，本层上限%s"), *WholeText(E.Amount), E.DurationSeconds, *WholeText(E.LayerCap)); break;
    default: Text = TEXT("未支持效果"); break;
    }
    return Text + ConditionSuffix(E.Condition, E.ConditionThreshold);
}
FString FY01AttributeResolver::DescribeBreakdown(const FY01AttributeBreakdown& B)
{
    const bool Whole = B.Attribute == A::AttackDamage || B.Attribute == A::HealthRegen;
    const auto Num = [Whole](double V) { return Whole ? WholeText(V) : FString::Printf(TEXT("%g"), V); };
    FString Text = FString::Printf(TEXT("%s\n本来源结算值：%s%s\n百分比前小计：%s\n百分比合计：%g%%"),
        Name(B.Attribute), *Num(B.Final), B.Attribute == A::GoldGain ? TEXT("%") : TEXT(""), *Num(B.Subtotal), B.PercentTotal);
    for (const auto& E : B.Contributions)
    {
        FString Detail;
        switch (E.Kind)
        {
        case C::Base: Detail = FString::Printf(TEXT("基础固定 +%g → +%g"), E.Amount, E.Contribution); break;
        case C::Flat: Detail = FString::Printf(TEXT("固定 +%g → +%g"), E.Amount, E.Contribution); break;
        case C::Percent: Detail = FString::Printf(TEXT("百分比 +%g%% → +%g 百分点"), E.Amount, E.Contribution); break;
        case C::Mapping: Detail = FString::Printf(TEXT("系数 %s × %g → +%g"), Name(E.SourceAttribute), E.Amount, E.Contribution); break;
        case C::Growth: Detail = FString::Printf(TEXT("成长 +%g → +%g"), E.Amount, E.Contribution); break;
        default: Detail = TEXT("未支持贡献"); break;
        }
        Text += FString::Printf(TEXT("\n%s · EffectId=%s · %s%s · %s"), *DescribeSourceKey(E.SourceKey),
            *E.EffectId.ToString(), *Detail, *ConditionSuffix(E.Condition, E.ConditionThreshold), E.bActive ? TEXT("生效") : TEXT("未生效"));
    }
    return Text;
}
