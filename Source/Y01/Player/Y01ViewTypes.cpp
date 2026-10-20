#include "Player/Y01ViewTypes.h"
#include "Player/Y01LoadoutComponent.h"
#include "Combat/Y01DamageEffects.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Misc/Crc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformProcess.h"
#include "Misc/SecureHash.h"
#include "UObject/UnrealType.h"
#include <limits>

namespace Y01ViewTiming
{
double Now()
{
    return IsInGameThread() && FParse::Param(FCommandLine::Get(), TEXT("R36Trace"))
        && FParse::Param(FCommandLine::Get(), TEXT("R36Timing")) ? FPlatformTime::Seconds() : 0.;
}
FY01ViewAck Scope(const FY01CommittedView& View)
{
    FY01ViewAck Result; Result.MatchId = View.MatchId; Result.JoinSequence = View.JoinSequence;
    Result.ConnectionId = View.ConnectionId; Result.ViewRevision = View.ViewRevision; return Result;
}
void Mark(const TCHAR* Stage, const FY01ViewAck& Scope, double Start, int32 Bytes, uint64 Seq, int32 Items, const FY01CommittedView* View)
{
    const double Time = Now(); if (!Time) { return; }
    const uint64 Cycles = FPlatformTime::Cycles64(); const double End = FPlatformTime::Seconds();
    static uint32 Count = 0;
    if (Count >= 16384) { return; }
    ++Count;
    FString ViewDigest; double DigestSeconds = 0;
    if (View)
    {
        const double DigestStart = FPlatformTime::Seconds(); FString Export;
        FY01CommittedView::StaticStruct()->ExportText(Export, View, nullptr, nullptr, PPF_None, nullptr);
        FTCHARToUTF8 Encoded(*Export); FSHAHash Hash;
        FSHA1::HashBuffer(Encoded.Get(), Encoded.Length(), Hash.Hash); ViewDigest = Hash.ToString();
        DigestSeconds = FPlatformTime::Seconds() - DigestStart;
    }
    // 同一进程的UE时间夹取原生计数器；分析端必须验证偏移后才能做跨进程差值。
    UE_LOG(LogTemp, Display, TEXT("Y01R36_TRACE {\"kind\":\"TIMING\",\"stage\":\"%s\",\"pid\":%u,\"event\":%u,\"time\":%.9f,\"clockEnd\":%.9f,\"qpc\":\"%llu\",\"secondsPerCycle\":%.15f,\"start\":%.9f,\"match\":\"%s\",\"join\":%d,\"connection\":\"%s\",\"revision\":%llu,\"transfer\":\"%s\",\"window\":%d,\"bytes\":%d,\"seq\":%llu,\"items\":%d,\"digest\":\"%s\",\"digestSeconds\":%.9f}"),
        Stage, FPlatformProcess::GetCurrentProcessId(), Count, Time, End, Cycles, FPlatformTime::GetSecondsPerCycle64(), Start,
        *Scope.MatchId.ToString(), Scope.JoinSequence, *Scope.ConnectionId.ToString(), Scope.ViewRevision,
        *Scope.TransferId.ToString(), Scope.WindowSequence, Bytes, Seq, Items, *ViewDigest, DigestSeconds);
}
}

namespace
{
constexpr int32 WindowBytes = FY01ViewWire::ChunkBytes * FY01ViewWire::WindowChunks;
constexpr uint32 AttributeWireVersion = 0x59303107;
constexpr uint32 DeltaWireVersion = 0x59303108;
enum : uint32
{
    DGold = 1u << 0, DExperience = 1u << 1, DHealth = 1u << 2, DStats = 1u << 3,
    DGrowth = 1u << 4, DDelivery = 1u << 5, DIncome = 1u << 6, DEquipment = 1u << 7,
    DSkills = 1u << 8, DOffers = 1u << 9, DRecipes = 1u << 10, DTalent = 1u << 11,
    DReceipts = 1u << 12, DResponse = 1u << 13, DTickets = 1u << 14, DAttributes = 1u << 15, DDevoured = 1u << 16
};
enum : uint32
{
    SLearned = 1u << 0, SSkill = 1u << 1, SSlot = 1u << 2, SEntity = 1u << 3,
    SDeadline = 1u << 4, SDisplay = 1u << 5, SEffects = 1u << 6, SDamage = 1u << 7, SGrowth = 1u << 8
};
enum : uint32
{
    ODisplay = 1u << 0, OKind = 1u << 1, OPrice = 1u << 2, OQuantity = 1u << 3,
    OPurchased = 1u << 4, OSourceKey = 1u << 5, OSourceId = 1u << 6, OEffects = 1u << 7, OGrowth = 1u << 8,
    OItemType = 1u << 9, OOdds = 1u << 10
};
bool Reject(FString& Error, const TCHAR* Message) { Error = Message; return false; }
template<class T> bool Same(const T& A, const T& B) { return T::StaticStruct()->CompareScriptStruct(&A, &B, 0); }
template<class A, class B> bool SameSession(const A& Left, const B& Right)
{ return Left.MatchId == Right.MatchId && Left.JoinSequence == Right.JoinSequence && Left.ConnectionId == Right.ConnectionId; }
uint32 Hash(const TArray<uint8>& Bytes) { return FCrc::MemCrc32(Bytes.GetData(), Bytes.Num()); }
bool ValidBreakdowns(const TArray<FY01AttributeBreakdown>& Rows)
{
    using A = EY01Attribute; using C = EY01ContributionKind;
    if (Rows.IsEmpty()) { return true; } // 未初始化来源的旧夹具仍允许空明细。
    if (Rows.Num() != static_cast<int32>(A::GoldGain)) { return false; }
    const auto NonNegative = [](double Value) { return FMath::IsFinite(Value) && Value >= 0; };
    const auto Close = [](double Actual, double Expected, double Rounding = 8.)
    {
        return FMath::IsFinite(Expected) && FMath::Abs(Actual - Expected) <= FMath::Max(1.e-9,
            FMath::Abs(Expected) * std::numeric_limits<double>::epsilon() * Rounding);
    };
    const auto SourceKey = [](const FY01AttributeContribution& Entry)
    {
        if (Entry.Kind == C::Base) { return Entry.SourceKey == TEXT("基础"); }
        if (Entry.Kind == C::Growth && Entry.SourceKey == TEXT("玩家成长")) { return true; }
        FString Kind, Key;
        if (!Entry.SourceKey.Split(TEXT("/"), &Kind, &Key) || Key.IsEmpty()
            || (Kind != TEXT("0") && Kind != TEXT("1") && Kind != TEXT("2") && Kind != TEXT("3") && Kind != TEXT("4") && Kind != TEXT("5"))) { return false; }
        for (TCHAR Character : Key)
        { if (!FChar::IsAlnum(Character) && Character != TEXT('_') && Character != TEXT('-')) { return false; } }
        return true;
    };
    for (int32 Index = 0; Index < Rows.Num(); ++Index)
    {
        const auto& Row = Rows[Index];
        // 精确枚举顺序同时排除None、未知、重复或缺失行。
        if (Row.Attribute != static_cast<A>(Index + 1) || !NonNegative(Row.Final)
            || !NonNegative(Row.Subtotal) || !NonNegative(Row.PercentTotal)) { return false; }
        double Subtotal = 0, Percent = 0;
        for (const auto& Entry : Row.Contributions)
        {
            if (static_cast<uint8>(Entry.Kind) > static_cast<uint8>(C::Growth)
                || !SourceKey(Entry) || !NonNegative(Entry.Amount) || !NonNegative(Entry.Contribution)
                || !NonNegative(Entry.ConditionThreshold) || Entry.ConditionThreshold > 100
                || (!Entry.bActive && Entry.Contribution != 0)) { return false; }
            switch (Entry.Condition)
            {
            case EY01EffectCondition::None:
                if (Entry.ConditionThreshold != 0 || !Entry.bActive) { return false; }
                break;
            case EY01EffectCondition::HealthBelow:
            case EY01EffectCondition::HealthAbove:
                if (!Y01Data::IsConditionalTarget(Row.Attribute)
                    || (Entry.Kind != C::Flat && Entry.Kind != C::Percent)) { return false; }
                break;
            default: return false;
            }
            if (Entry.Kind == C::Mapping)
            {
                if (!Y01Data::IsPrimaryAttribute(Entry.SourceAttribute) || !Y01Data::IsDerivedAttribute(Row.Attribute)
                    || Entry.Condition != EY01EffectCondition::None) { return false; }
            }
            else if (Entry.SourceAttribute != A::None || Entry.Contribution != (Entry.bActive ? Entry.Amount : 0)) { return false; }
            if (Entry.Kind == C::Percent)
            {
                if (Row.Attribute == A::GoldPerSecond) { return false; }
                Percent += Entry.Contribution;
            }
            else
            {
                if (Row.Attribute == A::GoldGain) { return false; }
                Subtotal += Entry.Contribution;
            }
            if (!NonNegative(Subtotal) || !NonNegative(Percent)) { return false; }
        }
        // 映射在解析器中先合并系数；贡献显示逐项相乘，求和检查容纳该舍入顺序差异。
        const double SumRounding = FMath::Max(8., double(Row.Contributions.Num()) * 2.);
        if (!Close(Row.Subtotal, Subtotal, SumRounding) || !Close(Row.PercentTotal, Percent, SumRounding)) { return false; }
        if (Row.Attribute == A::GoldGain)
        {
            if (Row.Subtotal != 0 || Row.Final != Row.PercentTotal) { return false; }
        }
        else if (!Close(Row.Final, Row.Subtotal * (1. + Row.PercentTotal / 100.))) { return false; }
    }
    return true;
}
bool ValidView(const FY01CommittedView& V)
{
    if (!V.MatchId.IsValid() || V.JoinSequence <= 0 || !V.ConnectionId.IsValid() || !V.ViewRevision || !V.Revision
        || !V.ConfigRevision || !V.ExpectedSeq || V.EquipmentSlots.Num() != Y01Data::EquipmentSlotCount
        || V.Skills.Num() != Y01Data::SkillSlotCount || V.Receipts.Num() > 64 || V.Tickets.Num() != Y01Data::SkillSlotCount
        || !ValidBreakdowns(V.AttributeBreakdowns)) { return false; }
    const auto FloatValue = [](double Value)
    { return FMath::IsFinite(Value) && Value >= 0 && Value <= MAX_flt && (Value == 0 || static_cast<float>(Value) > 0); };
    const auto& Ledger = V.Growth;
    if (!FloatValue(Ledger.Strength) || !FloatValue(Ledger.Agility) || !FloatValue(Ledger.Wisdom) || !FloatValue(Ledger.MaxHealth)
        || (Ledger.AppliedKills != 0) != Ledger.LastKillEventId.IsValid()
        || (Ledger.FaultKillEventId.IsValid() && Ledger.GrowthFault == EY01ItemError::None)
        || static_cast<uint8>(Ledger.GrowthFault) > static_cast<uint8>(EY01ItemError::PurchaseLimit)
        || (Ledger.GrowthFault == EY01ItemError::None && !Ledger.FaultMessage.IsEmpty())) { return false; }
    if (!Ledger.bEnabled && (Ledger.bEnded || Ledger.AppliedKills || Ledger.Strength != 0 || Ledger.Agility != 0
        || Ledger.Wisdom != 0 || Ledger.MaxHealth != 0 || !Ledger.PlayerRules.IsEmpty()
        || Ledger.FaultKillEventId.IsValid())) { return false; }
    const FString Prefix = FString::Printf(TEXT("R36/%s/P%d/"), *V.MatchId.ToString(EGuidFormats::DigitsWithHyphens), V.JoinSequence);
    TMap<FString, TSet<FName>> PlayerRules;
    for (const auto& Entry : Ledger.PlayerRules)
    {
        const auto& G = Entry.Rule; FString Kind, Key;
        if (!Entry.SourceId.StartsWith(Prefix, ESearchCase::CaseSensitive)
            || !Entry.SourceId.Mid(Prefix.Len()).Split(TEXT("/"), &Kind, &Key)
            || (Kind != TEXT("Equipment") && Kind != TEXT("Skill") && Kind != TEXT("Shop") && Kind != TEXT("Talent")
                && Kind != TEXT("Devoured"))) { return false; }
        if (Key.IsEmpty() || Key.Len() > 128) { return false; }
        for (TCHAR C : Key) { if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-')) { return false; } }
        auto& Seen = PlayerRules.FindOrAdd(Entry.SourceId);
        if (G.RuleId.IsNone() || Seen.Contains(G.RuleId)
            || !(G.Trigger == EY01GrowthTrigger::EffectiveKill || G.Trigger == EY01GrowthTrigger::CombatTime
                || G.Trigger == EY01GrowthTrigger::BasicAttack)
            || G.Target != EY01GrowthTarget::Player || G.Attribute == EY01GrowthAttribute::AttackDamage
            || !FloatValue(G.Delta) || !FloatValue(G.Accumulated) || !G.AppliedCount
            || (G.Trigger == EY01GrowthTrigger::EffectiveKill && G.AppliedCount > Ledger.AppliedKills)) { return false; }
        Seen.Add(G.RuleId);
    }
    const auto ValidRules = [&](const TArray<FY01ItemGrowth>& Rules, bool Equipment)
    {
        // 未启用成长的旧视图保留原编码契约，不追加有效击杀业务语义。
        if (!Ledger.bEnabled) { return true; }
        TSet<FName> Seen;
        for (const auto& G : Rules)
        {
            const bool Instance = G.Target == EY01GrowthTarget::EquipmentInstance && G.Attribute == EY01GrowthAttribute::AttackDamage;
            const bool Player = G.Target == EY01GrowthTarget::Player && G.Attribute != EY01GrowthAttribute::AttackDamage;
            if (G.RuleId.IsNone() || Seen.Contains(G.RuleId)
                || !(G.Trigger == EY01GrowthTrigger::EffectiveKill || G.Trigger == EY01GrowthTrigger::CombatTime
                    || G.Trigger == EY01GrowthTrigger::BasicAttack)
                || (!Player && !(Equipment && Instance)) || !FMath::IsFinite(G.Delta) || G.Delta < 0
                || !FloatValue(G.Accumulated) || (!Instance && (G.AppliedCount || G.Accumulated != 0))
                || (!G.AppliedCount && G.Accumulated != 0)) { return false; }
            Seen.Add(G.RuleId);
        }
        return true;
    };
    const auto ValidEffects = [](const TArray<FY01ItemEffect>& Effects, bool Equipment)
    {
        TSet<FName> Seen;
        for (const auto& E : Effects)
        {
            if (E.EffectId.IsNone() || Seen.Contains(E.EffectId) || !Y01Data::ValidSourceEffect(E)
                || (!Equipment && E.Kind != EY01SourceEffectKind::AttackFlat && E.Kind != EY01SourceEffectKind::GoldPerSecond)) { return false; }
            Seen.Add(E.EffectId);
        }
        return true;
    };
    TSet<FName> Offers;
    for (const auto& S : V.ShopOffers)
    {
        if (S.OfferId.IsNone() || Offers.Contains(S.OfferId) || S.Price < 0 || S.Quantity <= 0
            || (S.ShopKind != EY01ShopKind::RandomPool && S.ShopKind != EY01ShopKind::RandomTier && S.ShopKind != EY01ShopKind::DirectEffect
                && S.ShopKind != EY01ShopKind::DirectItem)) { return false; }
        Offers.Add(S.OfferId);
        for (const auto& O : S.Odds)
        { if (static_cast<uint8>(O.Quality) > static_cast<uint8>(EY01Quality::Red) || !FMath::IsFinite(O.Weight) || O.Weight <= 0) { return false; } }
        if (S.ShopKind != EY01ShopKind::DirectEffect)
        {
            if (S.bPurchased || !S.SourceKey.IsEmpty() || !S.SourceId.IsEmpty() || !S.Effects.IsEmpty() || !S.Growth.IsEmpty()) { return false; }
            continue;
        }
        if (S.ItemType != EY01TableKind::None || !S.Odds.IsEmpty()) { return false; }
        const FString Key = S.OfferId.ToString().ToLower();
        if (S.Quantity != 1 || Key.IsEmpty() || Key.Len() > 128 || S.SourceKey != Key
            || S.SourceId != FString::Printf(TEXT("R36/%s/P%d/Shop/%s"), *V.MatchId.ToString(EGuidFormats::DigitsWithHyphens), V.JoinSequence, *Key)
            || (S.Effects.IsEmpty() && S.Growth.IsEmpty())) { return false; }
        for (TCHAR C : Key) { if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-')) { return false; } }
        TSet<FName> Effects, Growth;
        for (const auto& E : S.Effects)
        {
            if (E.EffectId.IsNone() || Effects.Contains(E.EffectId)
                || (E.Kind != EY01SourceEffectKind::AttackFlat && E.Kind != EY01SourceEffectKind::GoldPerSecond)
                || !Y01Data::ValidSourceEffect(E)
                || (E.Kind == EY01SourceEffectKind::AttackFlat && (E.Magnitude > MAX_flt || (E.Magnitude > 0 && static_cast<float>(E.Magnitude) == 0)))) { return false; }
            Effects.Add(E.EffectId);
        }
        for (const auto& G : S.Growth)
        {
            if (G.RuleId.IsNone() || Growth.Contains(G.RuleId)
                || !(G.Trigger == EY01GrowthTrigger::EffectiveKill || G.Trigger == EY01GrowthTrigger::CombatTime
                    || G.Trigger == EY01GrowthTrigger::BasicAttack)
                || G.Target != EY01GrowthTarget::Player || G.Attribute == EY01GrowthAttribute::AttackDamage
                || !FMath::IsFinite(G.Delta) || G.Delta < 0 || G.AppliedCount != 0 || G.Accumulated != 0) { return false; }
            Growth.Add(G.RuleId);
        }
    }
    TSet<FName> Recipes; TSet<uint16> RecipeKinds;
    for (const auto& S : V.SynthesisRecipes)
    {
        const auto& R = S.Recipe; const uint16 Kind = uint16(static_cast<uint8>(R.InputType)) * 256 + static_cast<uint8>(R.InputQuality);
        if (S.RecipeId.IsNone() || Recipes.Contains(S.RecipeId) || RecipeKinds.Contains(Kind)
            || (R.InputType != EY01TableKind::Equipment && R.InputType != EY01TableKind::Skill)
            || static_cast<uint8>(R.InputQuality) >= static_cast<uint8>(EY01Quality::Red)
            || R.InputCount != Y01Data::SynthesisInputCount || R.OutputPool.Type != EY01TableKind::LocalItemPool || R.OutputPool.RowName.IsNone()) { return false; }
        Recipes.Add(S.RecipeId); RecipeKinds.Add(Kind);
    }
    const auto& Choice = V.TalentChoice;
    if (!Choice.ChoiceId.IsValid())
    {
        if (Choice.Definition.Type != EY01TableKind::None || !Choice.Definition.RowName.IsNone() || !Choice.Candidates.IsEmpty()
            || !Choice.SelectedTalent.IsNone() || !Choice.SourceKey.IsEmpty() || !Choice.SourceId.IsEmpty()) { return false; }
    }
    else
    {
        const FString Key = Choice.Definition.RowName.ToString().ToLower();
        if (Choice.Definition.Type != EY01TableKind::TalentChoice || Choice.Definition.RowName.IsNone()
            || Choice.Candidates.Num() != Y01Data::TalentCandidateCount || Key.IsEmpty() || Key.Len() > 128 || Choice.SourceKey != Key
            || Choice.SourceId != FString::Printf(TEXT("R36/%s/P%d/Talent/%s"), *V.MatchId.ToString(EGuidFormats::DigitsWithHyphens), V.JoinSequence, *Key)) { return false; }
        for (TCHAR C : Key) { if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-')) { return false; } }
        TSet<FName> Candidates;
        for (const auto& C : Choice.Candidates)
        {
            if (C.TalentId.IsNone() || Candidates.Contains(C.TalentId) || C.DisplayName.IsEmpty()
                || static_cast<uint8>(C.Quality) > static_cast<uint8>(EY01Quality::Red) || (C.Effects.IsEmpty() && C.Growth.IsEmpty())) { return false; }
            Candidates.Add(C.TalentId); TSet<FName> Effects, Growth;
            for (const auto& E : C.Effects)
            {
                if (E.EffectId.IsNone() || Effects.Contains(E.EffectId)
                    || (E.Kind != EY01SourceEffectKind::AttackFlat && E.Kind != EY01SourceEffectKind::GoldPerSecond)
                    || !Y01Data::ValidSourceEffect(E)
                    || (E.Kind == EY01SourceEffectKind::AttackFlat && (E.Magnitude > MAX_flt || (E.Magnitude > 0 && static_cast<float>(E.Magnitude) == 0)))) { return false; }
                Effects.Add(E.EffectId);
            }
            for (const auto& G : C.Growth)
            {
                if (G.RuleId.IsNone() || Growth.Contains(G.RuleId)
                    || !(G.Trigger == EY01GrowthTrigger::EffectiveKill || G.Trigger == EY01GrowthTrigger::CombatTime
                        || G.Trigger == EY01GrowthTrigger::BasicAttack)
                    || G.Target != EY01GrowthTarget::Player || G.Attribute == EY01GrowthAttribute::AttackDamage
                    || !FMath::IsFinite(G.Delta) || G.Delta < 0 || G.AppliedCount || G.Accumulated != 0) { return false; }
                Growth.Add(G.RuleId);
            }
        }
        if (!Choice.SelectedTalent.IsNone() && !Candidates.Contains(Choice.SelectedTalent)) { return false; }
    }
    const auto& Delivery = V.Delivery;
    const auto NonNegative = [](double Value) { return FMath::IsFinite(Value) && Value >= 0; };
    if (static_cast<uint8>(Delivery.Fault) > static_cast<uint8>(EY01ItemError::PurchaseLimit)
        || (Delivery.Fault != EY01ItemError::None) != Delivery.FaultKillEventId.IsValid()
        || (!Delivery.bEnabled && (Delivery.bEnded || !Delivery.Records.IsEmpty() || Delivery.FaultKillEventId.IsValid()))) { return false; }
    TMap<FGuid, const FY01DeliveryRecord*> DeliveryIds, DeliveryItems;
    int32 Terminal = 0;
    for (const auto& R : Delivery.Records)
    {
        if (!R.DeliveryId.IsValid() || !R.KillEventId.IsValid() || !R.ItemId.IsValid()
            || R.MatchId != V.MatchId || R.JoinSequence != V.JoinSequence
            || DeliveryIds.Contains(R.DeliveryId) || DeliveryItems.Contains(R.ItemId)
            || (R.Definition.Type != EY01TableKind::Equipment && R.Definition.Type != EY01TableKind::Skill
                && R.Definition.Type != EY01TableKind::Consumable)
            || R.Definition.RowName.IsNone() || static_cast<uint8>(R.Quality) > static_cast<uint8>(EY01Quality::Red)
            || R.Origin.ContainsNaN() || !NonNegative(R.StartTime) || !NonNegative(R.ETA) || R.ETA < R.StartTime
            || !NonNegative(R.DelaySeconds) || !NonNegative(R.DeliveredTime)
            || static_cast<uint8>(R.State) > static_cast<uint8>(EY01DeliveryState::Cancelled)
            || static_cast<uint8>(R.Fault) > static_cast<uint8>(EY01ItemError::PurchaseLimit)
            || static_cast<uint8>(R.CancelReason) > static_cast<uint8>(EY01DeliveryCancel::Session)) { return false; }
        if ((R.State == EY01DeliveryState::Fault) != (R.Fault != EY01ItemError::None)
            || (R.State == EY01DeliveryState::Cancelled) != (R.CancelReason != EY01DeliveryCancel::None)) { return false; }
        if (R.State == EY01DeliveryState::Delivered)
        { if (R.DeliveredTime < R.ETA || R.DelaySeconds != R.DeliveredTime - R.ETA) { return false; } }
        else if (R.DeliveredTime != 0) { return false; }
        if (R.State == EY01DeliveryState::Pending)
        { if (Delivery.bEnded) { return false; } }
        else if (++Terminal > 64) { return false; }
        DeliveryIds.Add(R.DeliveryId, &R); DeliveryItems.Add(R.ItemId, &R);
    }
    TSet<FGuid> ArrivalIds;
    TSet<FGuid> Ids; uint64 Order = 0; TArray<FGuid> Slots; Slots.SetNum(Y01Data::EquipmentSlotCount);
    for (const auto& I : V.Items)
    {
        if (!I.ItemId.IsValid() || Ids.Contains(I.ItemId) || !I.EntityRevision || !I.LastChangedRevision
            || I.LastChangedRevision > V.Revision || I.AcquisitionOrder <= Order
            || I.Count < 1 || (I.Count != 1 && I.Definition.Type != EY01TableKind::Consumable)
            || !ValidRules(I.Growth, I.Definition.Type == EY01TableKind::Equipment)
            || !ValidEffects(I.Effects, I.Definition.Type == EY01TableKind::Equipment)
            || !Y01Damage::ValidateSource(I.DamageEffects, I.Definition.Type, I.Display.SkillType)) { return false; }
        Order = I.AcquisitionOrder; Ids.Add(I.ItemId);
        if (I.ArrivalDeliveryId.IsValid())
        {
            if (!Delivery.bEnabled || ArrivalIds.Contains(I.ArrivalDeliveryId)) { return false; }
            ArrivalIds.Add(I.ArrivalDeliveryId);
            if (const auto* R = DeliveryIds.Find(I.ArrivalDeliveryId); R
                && ((*R)->ItemId != I.ItemId || (*R)->State != EY01DeliveryState::Delivered)) { return false; }
        }
        // 已消耗物品可以不再在包中；仍在包中的到货物必须与保留回执对应。裁剪回执不清除物品标记。
        if (const auto* R = DeliveryItems.Find(I.ItemId); R && (*R)->State == EY01DeliveryState::Delivered)
        {
            if (I.ArrivalDeliveryId != (*R)->DeliveryId || I.Definition.Type != (*R)->Definition.Type
                || I.Definition.RowName != (*R)->Definition.RowName || I.Display.Quality != (*R)->Quality) { return false; }
        }
        if (I.Location == EY01ItemLocation::EquipmentSlot)
        {
            if (!Slots.IsValidIndex(I.Slot) || Slots[I.Slot].IsValid() || I.Definition.Type != EY01TableKind::Equipment) { return false; }
            Slots[I.Slot] = I.ItemId;
        }
        else if (I.Location != EY01ItemLocation::Backpack || I.Slot != INDEX_NONE) { return false; }
    }
    if (Slots != V.EquipmentSlots) { return false; }
    TSet<FName> DevouredRows;
    for (const auto& I : V.Devoured)
    {
        if (!I.ItemId.IsValid() || Ids.Contains(I.ItemId) || I.Definition.Type != EY01TableKind::Equipment
            || I.Definition.RowName.IsNone() || DevouredRows.Contains(I.Definition.RowName) || I.Display.Quality != EY01Quality::Red
            || I.Count != 1 || I.Location != EY01ItemLocation::Backpack || I.Slot != INDEX_NONE
            || !ValidRules(I.Growth, true) || !ValidEffects(I.Effects, true)
            || !Y01Damage::ValidateSource(I.DamageEffects, EY01TableKind::Equipment, I.Display.SkillType)) { return false; }
        Ids.Add(I.ItemId); DevouredRows.Add(I.Definition.RowName);
    }
    TSet<FName> Skills;
    for (int32 Index = 0; Index < V.Skills.Num(); ++Index)
    {
        const auto& S = V.Skills[Index];
        if (!ValidRules(S.Growth, false) || !ValidEffects(S.Effects, false)
            || !Y01Damage::ValidateSource(S.DamageEffects, EY01TableKind::Skill, S.Display.SkillType)
            || (!S.LearnedId.IsValid() && !S.DamageEffects.IsEmpty())) { return false; }
        if (S.LearnedId.IsValid())
        {
            if (S.Slot != Index || S.SkillId.IsNone() || !S.EntityRevision || Ids.Contains(S.LearnedId)
                || Skills.Contains(S.SkillId) || !FMath::IsFinite(S.Deadline) || S.Deadline < 0) { return false; }
            Ids.Add(S.LearnedId); Skills.Add(S.SkillId);
        }
    }
    return true;
}
template<class T> bool SameArray(const TArray<T>& A, const TArray<T>& B)
{
    if (A.Num() != B.Num()) { return false; }
    for (int32 Index = 0; Index < A.Num(); ++Index) { if (!Same(A[Index], B[Index])) { return false; } }
    return true;
}
template<class T> bool Optional(uint32 Fields, uint32 Bit, const TArray<T>& Values)
{ return Values.Num() == ((Fields & Bit) ? 1 : 0); }
template<class T> void SetOptional(TArray<T>& Values, const T& Value) { Values.Add(Value); }

void BuildItemDelta(const FY01CommittedView& Base, const FY01CommittedView& Latest,
    TArray<FY01ViewChange>& Manifest, TArray<FY01ViewItem>& ChangedItems)
{
    TMap<FGuid, const FY01ViewItem*> Old;
    for (const auto& I : Base.Items) { Old.Add(I.ItemId, &I); }
    for (const auto& I : Latest.Items)
    {
        const auto* Previous = Old.Find(I.ItemId);
        if (!Previous || !Same(I, **Previous))
        {
            auto& M = Manifest.AddDefaulted_GetRef(); M.ItemId = I.ItemId;
            M.Kind = Previous ? EY01ItemChangeKind::Changed : EY01ItemChangeKind::Added;
            M.ExpectedEntityRevision = Previous ? (*Previous)->EntityRevision : 0;
            M.EntityRevision = I.EntityRevision; M.LastChangedRevision = I.LastChangedRevision; ChangedItems.Add(I);
        }
        Old.Remove(I.ItemId);
    }
    for (const auto& Pair : Old)
    {
        auto& M = Manifest.AddDefaulted_GetRef(); M.ItemId = Pair.Key; M.Kind = EY01ItemChangeKind::Removed;
        M.ExpectedEntityRevision = Pair.Value->EntityRevision; M.LastChangedRevision = Latest.Revision;
    }
}
void BuildDelta(const FY01CommittedView& Base, const FY01CommittedView& Latest, FY01ViewDeltaCommit& D)
{
    D.BaseViewRevision = Base.ViewRevision; D.MatchId = Latest.MatchId; D.JoinSequence = Latest.JoinSequence;
    D.ConnectionId = Latest.ConnectionId; D.ViewRevision = Latest.ViewRevision; D.Revision = Latest.Revision;
    D.SourceRevision = Latest.SourceRevision; D.ConfigRevision = Latest.ConfigRevision; D.ExpectedSeq = Latest.ExpectedSeq;
    D.SampleTime = Latest.SampleTime;
    if (Latest.Gold != Base.Gold) { D.Fields |= DGold; D.Gold = Latest.Gold; }
    if (Latest.Experience != Base.Experience) { D.Fields |= DExperience; D.Experience = Latest.Experience; }
    if (Latest.Health != Base.Health) { D.Fields |= DHealth; D.Health = Latest.Health; }
    if (!Same(Latest.Stats, Base.Stats)) { D.Fields |= DStats; SetOptional(D.Stats, Latest.Stats); }
    if (!Same(Latest.Growth, Base.Growth)) { D.Fields |= DGrowth; SetOptional(D.Growth, Latest.Growth); }
    if (!Same(Latest.Delivery, Base.Delivery)) { D.Fields |= DDelivery; SetOptional(D.Delivery, Latest.Delivery); }
    if (Latest.IncomeRate != Base.IncomeRate) { D.Fields |= DIncome; D.IncomeRate = Latest.IncomeRate; }
    if (!SameArray(Latest.AttributeBreakdowns, Base.AttributeBreakdowns))
    { D.Fields |= DAttributes; D.AttributeBreakdowns = Latest.AttributeBreakdowns; }
    if (!SameArray(Latest.Devoured, Base.Devoured)) { D.Fields |= DDevoured; D.Devoured = Latest.Devoured; }
    for (int32 Index = 0; Index < Latest.EquipmentSlots.Num(); ++Index)
    {
        if (Latest.EquipmentSlots[Index] != Base.EquipmentSlots[Index])
        { D.Fields |= DEquipment; auto& V = D.EquipmentSlots.AddDefaulted_GetRef(); V.Index = Index; V.Value = Latest.EquipmentSlots[Index]; }
    }
    for (int32 Index = 0; Index < Latest.Skills.Num(); ++Index)
    {
        const auto& N = Latest.Skills[Index]; const auto& O = Base.Skills[Index];
        if (Same(N, O)) { continue; }
        D.Fields |= DSkills; auto& V = D.Skills.AddDefaulted_GetRef(); V.Index = Index;
        if (N.LearnedId != O.LearnedId) { V.Fields |= SLearned; V.LearnedId = N.LearnedId; }
        if (N.SkillId != O.SkillId) { V.Fields |= SSkill; V.SkillId = N.SkillId; }
        if (N.Slot != O.Slot) { V.Fields |= SSlot; V.Slot = N.Slot; }
        if (N.EntityRevision != O.EntityRevision) { V.Fields |= SEntity; V.EntityRevision = N.EntityRevision; }
        if (N.Deadline != O.Deadline) { V.Fields |= SDeadline; V.Deadline = N.Deadline; }
        if (!Same(N.Display, O.Display)) { V.Fields |= SDisplay; SetOptional(V.Display, N.Display); }
        if (!SameArray(N.Effects, O.Effects)) { V.Fields |= SEffects; V.Effects = N.Effects; }
        if (!SameArray(N.DamageEffects, O.DamageEffects)) { V.Fields |= SDamage; V.DamageEffects = N.DamageEffects; }
        if (!SameArray(N.Growth, O.Growth)) { V.Fields |= SGrowth; V.Growth = N.Growth; }
    }
    if (!SameArray(Latest.ShopOffers, Base.ShopOffers))
    {
        D.Fields |= DOffers; TMap<FName, const FY01ShopOfferState*> Old;
        for (const auto& V : Base.ShopOffers) { Old.Add(V.OfferId, &V); }
        for (const auto& N : Latest.ShopOffers)
        {
            D.ShopOfferOrder.Add(N.OfferId); const auto* Found = Old.Find(N.OfferId);
            if (Found && Same(N, **Found)) { continue; }
            const FY01ShopOfferState O = Found ? **Found : FY01ShopOfferState();
            auto& V = D.ChangedShopOffers.AddDefaulted_GetRef(); V.OfferId = N.OfferId;
            if (!N.DisplayName.EqualTo(O.DisplayName)) { V.Fields |= ODisplay; V.DisplayName = N.DisplayName; }
            if (N.ShopKind != O.ShopKind) { V.Fields |= OKind; V.ShopKind = N.ShopKind; }
            if (N.Price != O.Price) { V.Fields |= OPrice; V.Price = N.Price; }
            if (N.Quantity != O.Quantity) { V.Fields |= OQuantity; V.Quantity = N.Quantity; }
            if (N.bPurchased != O.bPurchased) { V.Fields |= OPurchased; V.bPurchased = N.bPurchased; }
            if (N.SourceKey != O.SourceKey) { V.Fields |= OSourceKey; V.SourceKey = N.SourceKey; }
            if (N.SourceId != O.SourceId) { V.Fields |= OSourceId; V.SourceId = N.SourceId; }
            if (!SameArray(N.Effects, O.Effects)) { V.Fields |= OEffects; V.Effects = N.Effects; }
            if (!SameArray(N.Growth, O.Growth)) { V.Fields |= OGrowth; V.Growth = N.Growth; }
            if (N.ItemType != O.ItemType) { V.Fields |= OItemType; V.ItemType = N.ItemType; }
            if (!SameArray(N.Odds, O.Odds)) { V.Fields |= OOdds; V.Odds = N.Odds; }
        }
    }
    if (!SameArray(Latest.SynthesisRecipes, Base.SynthesisRecipes))
    {
        D.Fields |= DRecipes; TMap<FName, const FY01SynthesisState*> Old;
        for (const auto& V : Base.SynthesisRecipes) { Old.Add(V.RecipeId, &V); }
        for (const auto& N : Latest.SynthesisRecipes)
        { D.SynthesisRecipeOrder.Add(N.RecipeId); const auto* O = Old.Find(N.RecipeId); if (!O || !Same(N, **O)) { D.ChangedSynthesisRecipes.Add(N); } }
    }
    if (!Same(Latest.TalentChoice, Base.TalentChoice)) { D.Fields |= DTalent; SetOptional(D.TalentChoice, Latest.TalentChoice); }
    if (!SameArray(Latest.Receipts, Base.Receipts))
    {
        D.Fields |= DReceipts; TMap<uint64, const FY01ViewReceipt*> Old;
        for (const auto& V : Base.Receipts) { Old.Add(V.Request.Seq, &V); }
        for (const auto& N : Latest.Receipts)
        { D.ReceiptOrder.Add(N.Request.Seq); const auto* O = Old.Find(N.Request.Seq); if (!O || !Same(N, **O)) { D.ChangedReceipts.Add(N); } }
    }
    if (Latest.ResponseSerial != Base.ResponseSerial || !Same(Latest.Response, Base.Response))
    { D.Fields |= DResponse; D.ResponseSerial = Latest.ResponseSerial; SetOptional(D.Response, Latest.Response); }
    for (int32 Index = 0; Index < Latest.Tickets.Num(); ++Index)
    {
        if (!Same(Latest.Tickets[Index], Base.Tickets[Index]))
        { D.Fields |= DTickets; auto& V = D.Tickets.AddDefaulted_GetRef(); V.Index = Index; V.Value = Latest.Tickets[Index]; }
    }
    BuildItemDelta(Base, Latest, D.Manifest, D.ChangedItems);
}

bool ApplyItems(const TArray<FY01ViewChange>& Manifest, const TArray<FY01ViewItem>& ChangedItems,
    const TArray<FY01ViewItem>& BaseItems, uint64 Revision, TArray<FY01ViewItem>& Out)
{
    TMap<FGuid, FY01ViewItem> Items; for (const auto& I : BaseItems) { Items.Add(I.ItemId, I); }
    TMap<FGuid, const FY01ViewItem*> Changed;
    for (const auto& I : ChangedItems) { if (Changed.Contains(I.ItemId)) { return false; } Changed.Add(I.ItemId, &I); }
    TSet<FGuid> Seen;
    for (const auto& M : Manifest)
    {
        if (!M.ItemId.IsValid() || Seen.Contains(M.ItemId)) { return false; } Seen.Add(M.ItemId);
        auto* Old = Items.Find(M.ItemId); if ((Old ? Old->EntityRevision : 0) != M.ExpectedEntityRevision) { return false; }
        if (M.Kind == EY01ItemChangeKind::Removed)
        {
            if (!Old || M.EntityRevision || Changed.Contains(M.ItemId) || M.LastChangedRevision != Revision) { return false; }
            Items.Remove(M.ItemId); continue;
        }
        const auto* Value = Changed.Find(M.ItemId);
        if (!Value || (M.Kind != EY01ItemChangeKind::Added && M.Kind != EY01ItemChangeKind::Changed)
            || (M.Kind == EY01ItemChangeKind::Added) != (Old == nullptr)
            || (*Value)->EntityRevision != M.EntityRevision || (*Value)->LastChangedRevision != M.LastChangedRevision
            || (Old && M.EntityRevision <= Old->EntityRevision)) { return false; }
        Items.Add(M.ItemId, **Value); Changed.Remove(M.ItemId);
    }
    if (!Changed.IsEmpty()) { return false; }
    Items.GenerateValueArray(Out); Out.Sort([](const auto& A, const auto& B) { return A.AcquisitionOrder < B.AcquisitionOrder; }); return true;
}
bool Apply(const FY01ViewCommit& Commit, const FY01CommittedView& Base, FY01CommittedView& Out)
{
    if (Commit.BaseViewRevision != Base.ViewRevision || !Commit.Header.Items.IsEmpty()
        || (Base.ViewRevision && (!SameSession(Base, Commit.Header) || Commit.Header.ViewRevision <= Base.ViewRevision))) { return false; }
    Out = Commit.Header;
    return ApplyItems(Commit.Manifest, Commit.ChangedItems, Base.Items, Out.Revision, Out.Items) && ValidView(Out);
}
bool Apply(const FY01ViewDeltaCommit& D, const FY01CommittedView& Base, FY01CommittedView& Out)
{
    constexpr uint32 All = (DDevoured << 1) - 1;
    if (!Base.ViewRevision || D.BaseViewRevision != Base.ViewRevision || !SameSession(Base, D)
        || D.ViewRevision <= Base.ViewRevision || (D.Fields & ~All)
        || !Optional(D.Fields, DStats, D.Stats) || !Optional(D.Fields, DGrowth, D.Growth)
        || !Optional(D.Fields, DDelivery, D.Delivery) || !Optional(D.Fields, DTalent, D.TalentChoice)
        || !Optional(D.Fields, DResponse, D.Response)
        || (!(D.Fields & DAttributes) && !D.AttributeBreakdowns.IsEmpty())
        || (!(D.Fields & DDevoured) && !D.Devoured.IsEmpty())) { return false; }
    Out = Base; Out.MatchId = D.MatchId; Out.JoinSequence = D.JoinSequence; Out.ConnectionId = D.ConnectionId;
    Out.ViewRevision = D.ViewRevision; Out.Revision = D.Revision; Out.SourceRevision = D.SourceRevision;
    Out.ConfigRevision = D.ConfigRevision; Out.ExpectedSeq = D.ExpectedSeq; Out.SampleTime = D.SampleTime;
    if (D.Fields & DGold) { Out.Gold = D.Gold; } if (D.Fields & DExperience) { Out.Experience = D.Experience; }
    if (D.Fields & DHealth) { Out.Health = D.Health; } if (D.Fields & DStats) { Out.Stats = D.Stats[0]; }
    if (D.Fields & DGrowth) { Out.Growth = D.Growth[0]; } if (D.Fields & DDelivery) { Out.Delivery = D.Delivery[0]; }
    if (D.Fields & DIncome) { Out.IncomeRate = D.IncomeRate; }
    if (D.Fields & DAttributes) { Out.AttributeBreakdowns = D.AttributeBreakdowns; }
    if (D.Fields & DDevoured) { Out.Devoured = D.Devoured; }
    TSet<int32> Slots;
    for (const auto& V : D.EquipmentSlots)
    { if (!(D.Fields & DEquipment) || !Out.EquipmentSlots.IsValidIndex(V.Index) || Slots.Contains(V.Index)) { return false; } Slots.Add(V.Index); Out.EquipmentSlots[V.Index] = V.Value; }
    if ((D.Fields & DEquipment) && D.EquipmentSlots.IsEmpty()) { return false; }
    Slots.Reset();
    for (const auto& V : D.Skills)
    {
        constexpr uint32 AllSkill = (SGrowth << 1) - 1;
        if (!(D.Fields & DSkills) || !Out.Skills.IsValidIndex(V.Index) || Slots.Contains(V.Index) || !V.Fields || (V.Fields & ~AllSkill)
            || !Optional(V.Fields, SDisplay, V.Display)) { return false; }
        Slots.Add(V.Index); auto& N = Out.Skills[V.Index];
        if (V.Fields & SLearned) { N.LearnedId = V.LearnedId; } if (V.Fields & SSkill) { N.SkillId = V.SkillId; }
        if (V.Fields & SSlot) { N.Slot = V.Slot; } if (V.Fields & SEntity) { N.EntityRevision = V.EntityRevision; }
        if (V.Fields & SDeadline) { N.Deadline = V.Deadline; } if (V.Fields & SDisplay) { N.Display = V.Display[0]; }
        if (V.Fields & SEffects) { N.Effects = V.Effects; } if (V.Fields & SDamage) { N.DamageEffects = V.DamageEffects; }
        if (V.Fields & SGrowth) { N.Growth = V.Growth; }
    }
    if (((D.Fields & DSkills) != 0) != !D.Skills.IsEmpty()) { return false; }
    if (D.Fields & DOffers)
    {
        TMap<FName, FY01ShopOfferState> Values; for (const auto& V : Out.ShopOffers) { if (Values.Contains(V.OfferId)) { return false; } Values.Add(V.OfferId, V); }
        TSet<FName> Changed;
        for (const auto& V : D.ChangedShopOffers)
        {
            constexpr uint32 AllOffer = (OOdds << 1) - 1;
            if (V.OfferId.IsNone() || Changed.Contains(V.OfferId) || !V.Fields || (V.Fields & ~AllOffer)) { return false; }
            Changed.Add(V.OfferId); auto& N = Values.FindOrAdd(V.OfferId); N.OfferId = V.OfferId;
            if (V.Fields & ODisplay) { N.DisplayName = V.DisplayName; } if (V.Fields & OKind) { N.ShopKind = V.ShopKind; }
            if (V.Fields & OPrice) { N.Price = V.Price; } if (V.Fields & OQuantity) { N.Quantity = V.Quantity; }
            if (V.Fields & OPurchased) { N.bPurchased = V.bPurchased; } if (V.Fields & OSourceKey) { N.SourceKey = V.SourceKey; }
            if (V.Fields & OSourceId) { N.SourceId = V.SourceId; } if (V.Fields & OEffects) { N.Effects = V.Effects; }
            if (V.Fields & OGrowth) { N.Growth = V.Growth; }
            if (V.Fields & OItemType) { N.ItemType = V.ItemType; } if (V.Fields & OOdds) { N.Odds = V.Odds; }
        }
        Out.ShopOffers.Reset(); TSet<FName> Seen;
        for (FName Id : D.ShopOfferOrder)
        { const auto* V = Values.Find(Id); if (Id.IsNone() || Seen.Contains(Id) || !V) { return false; } Seen.Add(Id); Out.ShopOffers.Add(*V); }
        for (FName Id : Changed) { if (!Seen.Contains(Id)) { return false; } }
    }
    else if (!D.ShopOfferOrder.IsEmpty() || !D.ChangedShopOffers.IsEmpty()) { return false; }
    if (D.Fields & DRecipes)
    {
        TMap<FName, FY01SynthesisState> Values; for (const auto& V : Out.SynthesisRecipes) { Values.Add(V.RecipeId, V); }
        TSet<FName> Changed;
        for (const auto& V : D.ChangedSynthesisRecipes)
        { if (V.RecipeId.IsNone() || Changed.Contains(V.RecipeId)) { return false; } Changed.Add(V.RecipeId); Values.Add(V.RecipeId, V); }
        Out.SynthesisRecipes.Reset(); TSet<FName> Seen;
        for (FName Id : D.SynthesisRecipeOrder)
        { const auto* V = Values.Find(Id); if (Id.IsNone() || Seen.Contains(Id) || !V) { return false; } Seen.Add(Id); Out.SynthesisRecipes.Add(*V); }
        for (FName Id : Changed) { if (!Seen.Contains(Id)) { return false; } }
    }
    else if (!D.SynthesisRecipeOrder.IsEmpty() || !D.ChangedSynthesisRecipes.IsEmpty()) { return false; }
    if (D.Fields & DTalent) { Out.TalentChoice = D.TalentChoice[0]; }
    if (D.Fields & DReceipts)
    {
        TMap<uint64, FY01ViewReceipt> Values; for (const auto& V : Out.Receipts) { Values.Add(V.Request.Seq, V); }
        TSet<uint64> Changed;
        for (const auto& V : D.ChangedReceipts)
        { if (!V.Request.Seq || Changed.Contains(V.Request.Seq)) { return false; } Changed.Add(V.Request.Seq); Values.Add(V.Request.Seq, V); }
        Out.Receipts.Reset(); TSet<uint64> Seen;
        for (uint64 Seq : D.ReceiptOrder)
        { const auto* V = Values.Find(Seq); if (!Seq || Seen.Contains(Seq) || !V) { return false; } Seen.Add(Seq); Out.Receipts.Add(*V); }
        for (uint64 Seq : Changed) { if (!Seen.Contains(Seq)) { return false; } }
    }
    else if (!D.ReceiptOrder.IsEmpty() || !D.ChangedReceipts.IsEmpty()) { return false; }
    if (D.Fields & DResponse) { Out.ResponseSerial = D.ResponseSerial; Out.Response = D.Response[0]; }
    Slots.Reset();
    for (const auto& V : D.Tickets)
    { if (!(D.Fields & DTickets) || !Out.Tickets.IsValidIndex(V.Index) || Slots.Contains(V.Index)) { return false; } Slots.Add(V.Index); Out.Tickets[V.Index] = V.Value; }
    if (((D.Fields & DTickets) != 0) != !D.Tickets.IsEmpty()) { return false; }
    return ApplyItems(D.Manifest, D.ChangedItems, Base.Items, Out.Revision, Out.Items) && ValidView(Out);
}
}

bool FY01ViewWire::NetDeltaSerialize(FNetDeltaSerializeInfo& Params)
{
    if (Params.Reader) { Receiver = Params.Object; }
    return FastArrayDeltaSerialize<FY01ViewChunk, FY01ViewWire>(Items, Params, *this);
}
void FY01ViewWire::PostReplicatedReceive(const FPostReplicatedReceiveParameters& Params)
{
    if (auto* Component = Cast<UY01LoadoutComponent>(Receiver.Get())) { Component->ReceiveViewWindow(); }
}

bool FY01ViewSender::Restart(const FY01ViewAck& Scope, FY01ViewWire& Wire, FString& Error)
{
    Error.Reset();
    if (!InFlight.ViewRevision || !Same(Scope, Pending)) { return Reject(Error, TEXT("重同步身份或序号不匹配")); }
    Acknowledged = {}; InFlight = {}; Pending = {}; Payload.Reset(); Offset = 0;
    return Begin(Wire, Error);
}
void FY01ViewSender::Reset()
{ Acknowledged = {}; InFlight = {}; Latest = {}; Pending = {}; Payload.Reset(); Offset = 0; Digest = 0; }
bool FY01ViewSender::IsCurrent(const FY01CommittedView& View) const
{
    return Latest.ViewRevision && View.ViewRevision == Latest.ViewRevision && SameSession(View, Latest)
        && (InFlight.ViewRevision || Acknowledged.ViewRevision == Latest.ViewRevision);
}
bool FY01ViewSender::Offer(const FY01CommittedView& View, FY01ViewWire& Wire, FString& Error)
{
    Error.Reset();
    if (!ValidView(View)) { return Reject(Error, TEXT("无效完整视图，未截断或发送")); }
    if (Latest.ViewRevision && (!SameSession(View, Latest) || View.ViewRevision < Latest.ViewRevision
        || (View.ViewRevision == Latest.ViewRevision && !Same(View, Latest))))
    { return Reject(Error, TEXT("拒绝跨会话、回退或同版本异载荷")); }
    Latest = View;
    if (InFlight.ViewRevision || Acknowledged.ViewRevision == Latest.ViewRevision) { return true; }
    return Begin(Wire, Error);
}
bool FY01ViewSender::Begin(FY01ViewWire& Wire, FString& Error)
{
    const double TimingStart = Y01ViewTiming::Now();
    Payload.Reset(); FMemoryWriter Writer(Payload, true); FObjectAndNameAsStringProxyArchive Ar(Writer, false);
    Ar.SetWantBinaryPropertySerialization(true);
    if (Acknowledged.ViewRevision)
    {
        uint32 Version = DeltaWireVersion; Ar << Version;
        FY01ViewDeltaCommit Delta; BuildDelta(Acknowledged, Latest, Delta);
        FY01ViewDeltaCommit::StaticStruct()->SerializeBin(Ar, &Delta);
    }
    else
    {
        uint32 Version = AttributeWireVersion; Ar << Version;
        FY01ViewCommit Commit; Commit.Header = Latest; Commit.Header.Items.Reset();
        BuildItemDelta({}, Latest, Commit.Manifest, Commit.ChangedItems);
        FY01ViewCommit::StaticStruct()->SerializeBin(Ar, &Commit);
    }
    if (Ar.IsError() || Payload.IsEmpty()) { Payload.Reset(); return Reject(Error, TEXT("提交编码失败；没有满包截断")); }
    InFlight = Latest; Pending.MatchId = Latest.MatchId; Pending.JoinSequence = Latest.JoinSequence;
    Pending.ConnectionId = Latest.ConnectionId; Pending.TransferId = FGuid::NewGuid(); Pending.ViewRevision = Latest.ViewRevision;
    Pending.WindowSequence = 1; Offset = 0; Digest = Hash(Payload);
    Y01ViewTiming::Mark(TEXT("ENCODE"), Pending, TimingStart, Payload.Num(), Latest.Response.Request.Seq, Latest.Items.Num());
    FillWindow(Wire); return true;
}
void FY01ViewSender::FillWindow(FY01ViewWire& Wire)
{
    const int32 Count = FMath::DivideAndRoundUp(FMath::Min(WindowBytes, Payload.Num() - Offset), FY01ViewWire::ChunkBytes);
    if (Wire.Items.Num() > Count) { Wire.Items.SetNum(Count); Wire.MarkArrayDirty(); }
    while (Wire.Items.Num() < Count) { Wire.Items.AddDefaulted(); }
    for (int32 Index = 0; Index < Count; ++Index)
    {
        auto& C = Wire.Items[Index]; C.Scope = Pending; C.Offset = Offset + Index * FY01ViewWire::ChunkBytes;
        C.TotalBytes = Payload.Num(); C.Digest = Digest; C.Bytes.Reset();
        C.Bytes.Append(Payload.GetData() + C.Offset, FMath::Min(FY01ViewWire::ChunkBytes, Payload.Num() - C.Offset));
        C.ChunkDigest = Hash(C.Bytes); Wire.MarkItemDirty(C);
    }
    Y01ViewTiming::Mark(TEXT("SEND_WINDOW"), Pending, 0, Payload.Num(), InFlight.Response.Request.Seq, InFlight.Items.Num());
}
bool FY01ViewSender::Acknowledge(const FY01ViewAck& Ack, FY01ViewWire& Wire, FString& Error)
{
    Error.Reset();
    if (!InFlight.ViewRevision || !Same(Ack, Pending)) { return Reject(Error, TEXT("ACK的会话、连接、提交或窗口序号不匹配")); }
    Y01ViewTiming::Mark(TEXT("ACK_WINDOW"), Ack, 0, Payload.Num(), InFlight.Response.Request.Seq, InFlight.Items.Num());
    Offset += FMath::Min(WindowBytes, Payload.Num() - Offset);
    if (Offset < Payload.Num()) { ++Pending.WindowSequence; FillWindow(Wire); return true; }
    Acknowledged = MoveTemp(InFlight); InFlight = {}; Payload.Reset(); Pending = {}; Offset = 0;
    if (Latest.ViewRevision > Acknowledged.ViewRevision) { return Begin(Wire, Error); }
    Wire.Items.Reset(); Wire.MarkArrayDirty(); return true;
}

void FY01ViewReceiver::Reset()
{ Published = {}; Pending = {}; Payload.Reset(); TotalBytes = 0; Digest = 0; }
bool FY01ViewReceiver::Receive(const FY01ViewWire& Wire, FY01ViewAck& Ack, FString& Error)
{
    Ack = {}; Error.Reset();
    if (Wire.Items.IsEmpty() || Wire.Items.Num() > FY01ViewWire::WindowChunks) { return Reject(Error, TEXT("空或越界传输窗口")); }
    const auto& First = Wire.Items[0]; const auto& Scope = First.Scope;
    const double TimingStart = Y01ViewTiming::Now();
    if (!Scope.MatchId.IsValid() || Scope.JoinSequence <= 0 || !Scope.ConnectionId.IsValid() || !Scope.TransferId.IsValid()
        || !Scope.ViewRevision || Scope.WindowSequence <= 0 || First.TotalBytes <= 0)
    { return Reject(Error, TEXT("分片身份或长度无效")); }
    const int64 Start64 = int64(Scope.WindowSequence - 1) * WindowBytes;
    if (Start64 >= First.TotalBytes) { return Reject(Error, TEXT("窗口起点越界")); }
    const int32 Start = int32(Start64), Length = FMath::Min(WindowBytes, First.TotalBytes - Start);
    const int32 Count = FMath::DivideAndRoundUp(Length, FY01ViewWire::ChunkBytes);
    if (Wire.Items.Num() != Count) { return Reject(Error, TEXT("窗口未完整到齐；保留上份业务视图")); }
    TArray<const FY01ViewChunk*> Ordered; Ordered.SetNumZeroed(Count);
    for (const auto& C : Wire.Items)
    {
        if (!Same(C.Scope, Scope) || C.TotalBytes != First.TotalBytes || C.Digest != First.Digest || C.Offset < Start
            || C.Offset >= Start + int64(Length) || (C.Offset - Start) % FY01ViewWire::ChunkBytes != 0
            || C.Bytes.Num() != FMath::Min(FY01ViewWire::ChunkBytes, C.TotalBytes - C.Offset) || Hash(C.Bytes) != C.ChunkDigest)
        { return Reject(Error, TEXT("窗口混合身份、缺字节或分片校验失败")); }
        const int32 Index = (C.Offset - Start) / FY01ViewWire::ChunkBytes;
        if (Ordered[Index]) { return Reject(Error, TEXT("重复分片占用")); } Ordered[Index] = &C;
    }
    if (Same(Scope, Pending)) { Ack = Pending; return true; }
    if (Scope.TransferId != Pending.TransferId)
    {
        const auto& Known = Pending.ViewRevision ? Pending : Scope;
        const bool NewConnection = Scope.ConnectionId != Known.ConnectionId && Scope.ViewRevision > Known.ViewRevision;
        if (Scope.WindowSequence != 1 || Scope.MatchId != Known.MatchId || Scope.JoinSequence != Known.JoinSequence
            || (!SameSession(Scope, Known) && !NewConnection) || Scope.ViewRevision < Known.ViewRevision
            || (Published.ViewRevision && (Scope.MatchId != Published.MatchId || Scope.JoinSequence != Published.JoinSequence
                || Scope.ViewRevision < Published.ViewRevision)))
        { return Reject(Error, TEXT("新传输缺少首窗口或会话过期，需要重同步")); }
        Payload.Reset(); TotalBytes = First.TotalBytes; Digest = First.Digest;
    }
    else if (Scope.WindowSequence != Pending.WindowSequence + 1 || Scope.ViewRevision != Pending.ViewRevision || !SameSession(Scope, Pending))
    { return Reject(Error, TEXT("窗口乱序；拒绝跳过未确认数据")); }
    if (Start != Payload.Num() || First.TotalBytes != TotalBytes || First.Digest != Digest) { return Reject(Error, TEXT("组装偏移或清单校验不一致")); }
    for (const auto* C : Ordered) { Payload.Append(C->Bytes); }
    Y01ViewTiming::Mark(TEXT("RECEIVE_WINDOW"), Scope, TimingStart, TotalBytes);
    if (Payload.Num() == TotalBytes)
    {
        Y01ViewTiming::Mark(TEXT("ASSEMBLED"), Scope, 0, TotalBytes);
        const double DecodeStart = Y01ViewTiming::Now();
        if (Hash(Payload) != Digest) { Payload.SetNum(Start); return Reject(Error, TEXT("完整提交校验失败")); }
        FMemoryReader Reader(Payload, true); FObjectAndNameAsStringProxyArchive Ar(Reader, false);
        Ar.SetWantBinaryPropertySerialization(true);
        uint32 Version = 0; Ar << Version; FY01CommittedView Next; bool Applied = false;
        if (Version == AttributeWireVersion)
        {
            FY01ViewCommit Commit; FY01ViewCommit::StaticStruct()->SerializeBin(Ar, &Commit);
            Applied = !Ar.IsError() && SameSession(Commit.Header, Scope)
                && Commit.Header.ViewRevision == Scope.ViewRevision && Apply(Commit, {}, Next);
        }
        else if (Version == DeltaWireVersion)
        {
            FY01ViewDeltaCommit Delta; FY01ViewDeltaCommit::StaticStruct()->SerializeBin(Ar, &Delta);
            Applied = !Ar.IsError() && SameSession(Delta, Scope) && Delta.ViewRevision == Scope.ViewRevision
                && Apply(Delta, Published, Next);
        }
        else { Payload.SetNum(Start); return Reject(Error, TEXT("完整视图属性编码版本不兼容")); }
        if (!Applied || Ar.IsError() || Reader.Tell() != Payload.Num())
        { Payload.SetNum(Start); return Reject(Error, TEXT("清单、期望版本或槽位不完整，需要重同步")); }
        Published = MoveTemp(Next); Payload.Reset();
        Y01ViewTiming::Mark(TEXT("PUBLISHED"), Scope, DecodeStart, TotalBytes, Published.Response.Request.Seq, Published.Items.Num());
    }
    Pending = Scope; Ack = Pending; return true;
}
