#include "Player/Y01GrowthComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01EconomyComponent.h"
#include "Match/Y01GameFlow.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

namespace
{
bool Add(double& Total, double Delta)
{
    const double Next = Total + Delta;
    if (!FMath::IsFinite(Total) || Total < 0 || !FMath::IsFinite(Delta) || Delta < 0 || !FMath::IsFinite(Next)
        || (Delta > 0 && Next == Total) || (Total > 0 && Delta > 0 && Next == Delta)) { return false; }
    Total = Next; return true;
}
bool FloatValue(double Value)
{ return FMath::IsFinite(Value) && Value >= 0 && Value <= MAX_flt && (Value == 0 || static_cast<float>(Value) > 0); }
}
UY01GrowthComponent::UY01GrowthComponent() { PrimaryComponentTick.bCanEverTick = false; bAutoActivate = false; }
bool UY01GrowthComponent::Initialize()
{
    auto* P = Cast<AY01PlayerState>(GetOwner());
    if (!IsInGameThread() || !P || !P->HasAuthority() || P->GetGrowth() != this || bEnded) { return false; }
    auto* L = P->GetLoadout();
    if (L->IsBusy() || !L->CheckOwner().IsOk() || L->GameState->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { return false; }
    if (bInitialized) { return Player == P && MatchId == L->MatchId && JoinSequence == L->JoinSequence; }
    if (!GetWorld()->GetSubsystem<UY01KillRewards>()->Enable(L->MatchId)) { return false; }
    Player = P; MatchId = L->MatchId; JoinSequence = L->JoinSequence;
    bInitialized = true; Ledger.bEnabled = true; SetActiveFlag(true);
    const TWeakObjectPtr<UY01GrowthComponent> Weak(this);
    L->Transactions.BeforeEnter = [Weak] { return !Weak.IsValid() || !Weak->BlocksMutation(); };
    L->Transactions.AfterLeave = [Weak] { if (Weak.IsValid()) { Weak->Drain(); } };
    L->PublishCommittedView(); return true;
}
void UY01GrowthComponent::Drain() { if (GetWorld()) { GetWorld()->GetSubsystem<UY01KillRewards>()->Drain(); } }
bool UY01GrowthComponent::BlocksMutation() const
{ return bInitialized && !bConsuming && GetWorld()->GetSubsystem<UY01KillRewards>()->HasPending(Player.Get()); }
bool UY01GrowthComponent::CanConsume() const
{
    if (!Player.IsValid()) { return true; }
    const auto* S = Player->GetSourceEffects();
    return !Player->GetLoadout()->IsBusy() && !S->bBusy && !S->bInCall && !Player->GetCombatASC()->bAutoAttackStarting;
}
bool UY01GrowthComponent::FreezeSources(FY01KillEvent& Event)
{
    if (!IsEnabled() || !Player.IsValid() || Event.Killer != Player || Event.MatchId != MatchId) { return false; }
    auto* L = Player->GetLoadout(); auto* S = Player->GetSourceEffects();
    if (!L->CheckRequester(Cast<AController>(Player->GetOwner())).IsOk() || S->bEnded) { return false; }
    Event.JoinSequence = JoinSequence;
    for (const auto& Id : L->EquipmentSlots)
    {
        const auto* Item = L->Items.Find(Id); if (!Item) { continue; }
        const auto Source = L->SourceId(Id);
        if (!S->Records.ContainsByPredicate([&](const auto& R) { return R.Source.Id == Source.Id; })) { continue; }
        Event.Sources.Add({Source, Id, Item->Growth});
    }
    for (const auto& Skill : L->Skills)
    {
        if (!Skill.LearnedId.IsValid()) { continue; }
        const auto Source = L->SourceId(Skill.LearnedId, EY01RuntimeSourceKind::Skill);
        if (!S->Records.ContainsByPredicate([&](const auto& R) { return R.Source.Id == Source.Id; })) { continue; }
        Event.Sources.Add({Source, {}, Skill.Growth});
    }
    const auto* Economy = Player->GetEconomy();
    for (const auto& Id : Economy->OfferOrder)
    {
        const auto& Offer = Economy->OfferStates.FindChecked(Id);
        if (!Offer.bPurchased) { continue; }
        const auto* Record = S->Records.FindByPredicate([&](const auto& R) { return R.Source.Id == Offer.SourceId; });
        if (Record) { Event.Sources.Add({Record->Source, {}, Offer.Growth}); }
    }
    const auto& Choice = Economy->TalentState;
    if (!Choice.SelectedTalent.IsNone())
    {
        const auto* Selected = Choice.Candidates.FindByPredicate([&](const auto& C) { return C.TalentId == Choice.SelectedTalent; });
        const auto* Record = S->Records.FindByPredicate([&](const auto& R) { return R.Source.Id == Choice.SourceId; });
        if (Selected && Record) { Event.Sources.Add({Record->Source, {}, Selected->Growth}); }
    }
    // 尚未线性化的库存/来源操作必须先精确回滚；已提交来源始终从Records读取。
    if (!bConsuming)
    {
        if (L->Transactions.bBusy) { L->Transactions.bInterrupted = true; }
        if (S->bBusy) { S->bInterrupted = true; }
    }
    return true;
}
TArray<FY01ItemEffect> UY01GrowthComponent::EquipmentEffects(const FY01ItemInstance& Item)
{
    auto Effects = Item.Effects;
    int32 Serial = 0;
    for (const auto& Rule : Item.Growth)
    {
        if (Rule.Target != EY01GrowthTarget::EquipmentInstance || Rule.Accumulated == 0) { continue; }
        FY01ItemEffect Effect;
        // 静态EffectId允许任意合法行名；从本实例已占用集合中确定性避让，不覆盖静态贡献。
        do { Effect.EffectId = FName(*FString::Printf(TEXT("Growth_%d"), Serial++)); }
        while (Effects.ContainsByPredicate([&](const auto& E) { return E.EffectId == Effect.EffectId; }));
        Effect.Kind = EY01SourceEffectKind::AttackFlat; Effect.Magnitude = Rule.Accumulated; Effects.Add(Effect);
    }
    Effects.Sort([](const auto& A, const auto& B) { return A.EffectId.LexicalLess(B.EffectId); }); return Effects;
}
void UY01GrowthComponent::Consume(const FY01KillEvent& Event)
{
    if (!IsInGameThread() || !Player.IsValid() || Event.Killer != Player || Event.MatchId != MatchId
        || Event.JoinSequence != JoinSequence || !Event.KillEventId.IsValid() || Consumed.Contains(Event.KillEventId)) { return; }
    TGuardValue<bool> Consuming(bConsuming, true);
    auto* L = Player->GetLoadout();
    if (!L->Transactions.Enter()) { return; }
    ON_SCOPE_EXIT { L->Transactions.Leave(); L->PublishCommittedView(); };
    Consumed.Add(Event.KillEventId);
    const auto Result = Apply(Event);
    if (!Result.IsOk())
    {
        Ledger.FaultKillEventId = Event.KillEventId; Ledger.GrowthFault = Result.Code; Ledger.FaultMessage = Result.Message;
        UE_LOG(LogTemp, Display, TEXT("Y01R36 GrowthFault Event=%s Code=%d Reason=%s"),
            *Event.KillEventId.ToString(), int32(Result.Code), *Result.Message);
    }
}
FY01ItemResult UY01GrowthComponent::Apply(const FY01KillEvent& Event)
{
    using E = EY01ItemError; auto* L = Player->GetLoadout(); auto* S = Player->GetSourceEffects(); auto* ASC = Player->GetCombatASC();
    auto R = L->CheckOwner(); if (!R.IsOk()) { return R; }
    if (!IsEnabled() || L->bEnded) { return L->Fail(E::Stopped, TEXT("成长会话已结束")); }
    if (Ledger.AppliedKills == MAX_uint64 || L->Revision == MAX_uint64 || L->CommittedView.ViewRevision == MAX_uint64)
    { return L->Fail(E::Range, TEXT("成长或库存视图序列耗尽")); }
    auto NextLedger = Ledger; auto NextItems = L->Items;
    double StrengthDelta = 0; TSet<FGuid> Changed;
    for (const auto& Source : Event.Sources)
    {
        for (const auto& Rule : Source.Rules)
        {
            if (Rule.RuleId.IsNone() || Rule.Trigger != EY01GrowthTrigger::EffectiveKill || !FloatValue(Rule.Delta))
            { return L->Fail(E::Precision, TEXT("成长增量不能表示为有限非负float")); }
            if (Rule.Target == EY01GrowthTarget::EquipmentInstance && Rule.Attribute == EY01GrowthAttribute::AttackDamage
                && Source.Source.Kind == EY01RuntimeSourceKind::Equipment)
            {
                auto* Item = NextItems.Find(Source.ItemId);
                auto* G = Item ? Item->Growth.FindByPredicate([&](const auto& V) { return V.RuleId == Rule.RuleId; }) : nullptr;
                if (!G || Item->Location != EY01ItemLocation::EquipmentSlot || G->AppliedCount == MAX_uint64
                    || Item->EntityRevision == MAX_uint64 || !Add(G->Accumulated, Rule.Delta) || !FloatValue(G->Accumulated))
                { return L->Fail(E::Precision, TEXT("实例成长身份、计次或累计范围无效")); }
                ++G->AppliedCount; Changed.Add(Item->ItemId);
            }
            else if (Rule.Target == EY01GrowthTarget::Player && Rule.Attribute == EY01GrowthAttribute::Strength)
            {
                auto* Entry = NextLedger.PlayerRules.FindByPredicate([&](const auto& V)
                { return V.SourceId == Source.Source.Id && V.Rule.RuleId == Rule.RuleId; });
                if (!Entry)
                {
                    FY01PlayerGrowthEntry Value; Value.SourceId = Source.Source.Id; Value.Rule = Rule;
                    Value.Rule.AppliedCount = 0; Value.Rule.Accumulated = 0;
                    Entry = &NextLedger.PlayerRules.Add_GetRef(MoveTemp(Value));
                }
                if (Entry->Rule.AppliedCount == MAX_uint64 || !Add(Entry->Rule.Accumulated, Rule.Delta)
                    || !FloatValue(Entry->Rule.Accumulated) || !Add(StrengthDelta, Rule.Delta))
                { return L->Fail(E::Precision, TEXT("玩家成长批次累计溢出或吞值")); }
                ++Entry->Rule.AppliedCount;
            }
            else { return L->Fail(E::InvalidDefinition, TEXT("不支持的成长目标或属性")); }
        }
    }
    const float BeforeStrength = ASC->UnitStats.Strength;
    double Strength = BeforeStrength;
    if (!FloatValue(BeforeStrength) || !FloatValue(StrengthDelta) || !Add(Strength, StrengthDelta) || !FloatValue(Strength)
        || (StrengthDelta > 0 && (static_cast<float>(Strength) <= BeforeStrength
            || (BeforeStrength > 0 && static_cast<float>(Strength) == static_cast<float>(StrengthDelta))))
        || !Add(NextLedger.Strength, StrengthDelta) || !FloatValue(NextLedger.Strength))
    { return L->Fail(E::Precision, TEXT("玩家力量整批不能表示为有限非负累计值")); }
    TArray<FY01ResolvedSource> Additions; TArray<FY01RuntimeSourceId> Removals;
    for (const auto& Id : Changed)
    {
        auto& Item = NextItems.FindChecked(Id); ++Item.EntityRevision; Item.LastChangedRevision = L->Revision + 1;
        const auto SourceId = L->SourceId(Id); Removals.Add(SourceId);
        FY01ResolvedSource Source; Source.Kind = SourceId.Kind; Source.LocalKey = SourceId.LocalKey;
        for (const auto& Effect : EquipmentEffects(Item)) { Source.Effects.Add({Effect.EffectId, Effect.Kind, Effect.Magnitude, Effect.SourceAttribute, Effect.TargetAttribute, Effect.Amount}); }
        Additions.Add(MoveTemp(Source));
    }
    ++NextLedger.AppliedKills; NextLedger.LastKillEventId = Event.KillEventId;
    double Now = 0; R = L->FromSource(S->SettleMutationTime(Now)); if (!R.IsOk()) { return R; }
    FY01PreparedMutation Plan; R = L->FromSource(S->PrepareMutation(Additions, Removals, Plan)); if (!R.IsOk()) { return R; }
    Plan.PlayerStrength = NextLedger.Strength;
    FY01SourceTransaction Tx; R = L->FromSource(S->StageMutationAt(Plan, Tx, Now)); if (!R.IsOk()) { return R; }
    FY01ScopedSourceMutation Guard(MoveTemp(Tx));
    if (bEnded || L->bEnded || L->Transactions.bInterrupted || ASC->UnitStats.Strength != BeforeStrength)
    { return L->Fail(E::Interrupted, TEXT("成长暂存期间会话或事务被中断")); }
    return L->FromSource(Guard.CommitAt([&]
    {
        L->Items = MoveTemp(NextItems); ++L->Revision; Ledger = MoveTemp(NextLedger);
    }));
}
void UY01GrowthComponent::EndSession()
{
    if (!IsInGameThread() || !GetOwner() || !GetOwner()->HasAuthority() || bEnded) { return; }
    bEnded = true; Ledger.bEnded = bInitialized; SetActiveFlag(false);
    if (Player.IsValid()) { Player->GetLoadout()->Transactions.bInterrupted = true; }
    Drain();
}
void UY01GrowthComponent::EndPlay(const EEndPlayReason::Type Reason)
{ EndSession(); if (HasBegunPlay()) { Super::EndPlay(Reason); } }
