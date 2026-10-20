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
UY01GrowthComponent::UY01GrowthComponent()
{ PrimaryComponentTick.bCanEverTick = true; PrimaryComponentTick.TickInterval = 1.f; bAutoActivate = false; }
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
    for (const auto& D : L->Devoured)
    {
        const auto Source = L->SourceId(D.ItemId, EY01RuntimeSourceKind::Devoured);
        if (!S->Records.ContainsByPredicate([&](const auto& R) { return R.Source.Id == Source.Id; })) { continue; }
        Event.Sources.Add({Source, D.ItemId, D.Growth});
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
        Effect.Kind = EY01SourceEffectKind::AttackFlat; Effect.Magnitude = Rule.Accumulated;
        Effect.bGrowthContribution = true; Effects.Add(Effect);
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
    TArray<FGrowthBatch> Batches;
    for (const auto& Source : Event.Sources)
    {
        FGrowthBatch Batch; Batch.Source = Source.Source; Batch.ItemId = Source.ItemId;
        // FreezeSources 冻结来源的全部规则；击杀批只结算 EffectiveKill，时间/普攻成长由 SettleTick 按批结算。
        for (const auto& Rule : Source.Rules)
        {
            if (Rule.Trigger == EY01GrowthTrigger::EffectiveKill) { Batch.Rules.Add(Rule); }
        }
        if (!Batch.Rules.IsEmpty()) { Batches.Add(MoveTemp(Batch)); }
    }
    return ApplyBatches(Batches, Event.KillEventId);
}
FY01ItemResult UY01GrowthComponent::ApplyBatches(const TArray<FGrowthBatch>& Batches, const FGuid& KillEventId)
{
    using E = EY01ItemError; auto* L = Player->GetLoadout(); auto* S = Player->GetSourceEffects(); auto* ASC = Player->GetCombatASC();
    auto R = L->CheckOwner(); if (!R.IsOk()) { return R; }
    if (!IsEnabled() || L->bEnded) { return L->Fail(E::Stopped, TEXT("成长会话已结束")); }
    if (Ledger.AppliedKills == MAX_uint64 || L->Revision == MAX_uint64 || L->CommittedView.ViewRevision == MAX_uint64)
    { return L->Fail(E::Range, TEXT("成长或库存视图序列耗尽")); }
    auto NextLedger = Ledger; auto NextItems = L->Items; auto NextDevoured = L->Devoured;
    double DimDeltas[4] = {}; TSet<FGuid> Changed, ChangedDevoured;
    for (const auto& Source : Batches)
    {
        for (const auto& Rule : Source.Rules)
        {
            const bool bKnownTrigger = Rule.Trigger == EY01GrowthTrigger::EffectiveKill
                || Rule.Trigger == EY01GrowthTrigger::CombatTime || Rule.Trigger == EY01GrowthTrigger::BasicAttack;
            if (Rule.RuleId.IsNone() || !bKnownTrigger || !FloatValue(Rule.Delta))
            { return L->Fail(E::Precision, TEXT("成长增量不能表示为有限非负float")); }
            const bool bInstance = Rule.Target == EY01GrowthTarget::EquipmentInstance && Rule.Attribute == EY01GrowthAttribute::AttackDamage;
            if (bInstance && Source.Source.Kind == EY01RuntimeSourceKind::Equipment)
            {
                auto* Item = NextItems.Find(Source.ItemId);
                auto* G = Item ? Item->Growth.FindByPredicate([&](const auto& V) { return V.RuleId == Rule.RuleId; }) : nullptr;
                if (!G || Item->Location != EY01ItemLocation::EquipmentSlot || G->AppliedCount == MAX_uint64
                    || Item->EntityRevision == MAX_uint64 || !Add(G->Accumulated, Rule.Delta) || !FloatValue(G->Accumulated))
                { return L->Fail(E::Precision, TEXT("实例成长身份、计次或累计范围无效")); }
                ++G->AppliedCount; Changed.Add(Item->ItemId);
            }
            else if (bInstance && Source.Source.Kind == EY01RuntimeSourceKind::Devoured)
            {
                auto* Item = NextDevoured.FindByPredicate([&](const auto& V) { return V.ItemId == Source.ItemId; });
                auto* G = Item ? Item->Growth.FindByPredicate([&](const auto& V) { return V.RuleId == Rule.RuleId; }) : nullptr;
                if (!G || G->AppliedCount == MAX_uint64 || Item->EntityRevision == MAX_uint64
                    || !Add(G->Accumulated, Rule.Delta) || !FloatValue(G->Accumulated))
                { return L->Fail(E::Precision, TEXT("已吞噬实例成长身份、计次或累计范围无效")); }
                ++G->AppliedCount; ChangedDevoured.Add(Item->ItemId);
            }
            else if (Rule.Target == EY01GrowthTarget::Player && Rule.Attribute != EY01GrowthAttribute::AttackDamage)
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
                    || !FloatValue(Entry->Rule.Accumulated))
                { return L->Fail(E::Precision, TEXT("玩家成长批次累计溢出或吞值")); }
                const int32 Dim = Rule.Attribute == EY01GrowthAttribute::Strength ? 0
                    : Rule.Attribute == EY01GrowthAttribute::Agility ? 1
                    : Rule.Attribute == EY01GrowthAttribute::Wisdom ? 2 : 3;
                if (!Add(DimDeltas[Dim], Rule.Delta)) { return L->Fail(E::Precision, TEXT("玩家成长批次累计溢出或吞值")); }
                ++Entry->Rule.AppliedCount;
            }
            else { return L->Fail(E::InvalidDefinition, TEXT("不支持的成长目标或属性")); }
        }
    }
    const float BeforeStrength = ASC->UnitStats.Strength, BeforeAgility = ASC->UnitStats.Agility, BeforeWisdom = ASC->UnitStats.Wisdom;
    if (!FloatValue(BeforeStrength) || !FloatValue(DimDeltas[0]) || !Add(NextLedger.Strength, DimDeltas[0]) || !FloatValue(NextLedger.Strength)
        || !Add(NextLedger.Agility, DimDeltas[1]) || !FloatValue(NextLedger.Agility)
        || !Add(NextLedger.Wisdom, DimDeltas[2]) || !FloatValue(NextLedger.Wisdom)
        || !Add(NextLedger.MaxHealth, DimDeltas[3]) || !FloatValue(NextLedger.MaxHealth))
    { return L->Fail(E::Precision, TEXT("玩家成长整批不能表示为有限非负累计值")); }
    TArray<FY01ResolvedSource> Additions; TArray<FY01RuntimeSourceId> Removals;
    for (const auto& Id : Changed)
    {
        auto& Item = NextItems.FindChecked(Id); ++Item.EntityRevision; Item.LastChangedRevision = L->Revision + 1;
        const auto SourceId = L->SourceId(Id); Removals.Add(SourceId);
        FY01ResolvedSource Source; Source.Kind = SourceId.Kind; Source.LocalKey = SourceId.LocalKey;
        for (const auto& Effect : EquipmentEffects(Item)) { Source.Effects.Add({Effect.EffectId, Effect.Kind, Effect.Magnitude, Effect.SourceAttribute, Effect.TargetAttribute, Effect.Amount,
            Effect.Condition, Effect.ConditionThreshold, Effect.bGrowthContribution, Effect.DurationSeconds, Effect.LayerCap, Effect.IntervalSeconds}); }
        Additions.Add(MoveTemp(Source));
    }
    for (const auto& Id : ChangedDevoured)
    {
        auto& Item = *NextDevoured.FindByPredicate([&](const auto& V) { return V.ItemId == Id; });
        ++Item.EntityRevision; Item.LastChangedRevision = L->Revision + 1;
        const auto SourceId = L->SourceId(Id, EY01RuntimeSourceKind::Devoured); Removals.Add(SourceId);
        FY01ResolvedSource Source; Source.Kind = SourceId.Kind; Source.LocalKey = SourceId.LocalKey;
        for (const auto& Effect : EquipmentEffects(Item)) { Source.Effects.Add({Effect.EffectId, Effect.Kind, Effect.Magnitude, Effect.SourceAttribute, Effect.TargetAttribute, Effect.Amount,
            Effect.Condition, Effect.ConditionThreshold, Effect.bGrowthContribution, Effect.DurationSeconds, Effect.LayerCap, Effect.IntervalSeconds}); }
        Additions.Add(MoveTemp(Source));
    }
    // AppliedKills/LastKillEventId 仅由击杀路径维护，与视图双向配对校验及既有测试语义一致；Tick 批不改写。
    if (KillEventId.IsValid()) { ++NextLedger.AppliedKills; NextLedger.LastKillEventId = KillEventId; }
    double Now = 0; R = L->FromSource(S->SettleMutationTime(Now)); if (!R.IsOk()) { return R; }
    FY01PreparedMutation Plan; R = L->FromSource(S->PrepareMutation(Additions, Removals, Plan)); if (!R.IsOk()) { return R; }
    Plan.PlayerGrowth = FY01PlayerGrowthTotals{NextLedger.Strength, NextLedger.Agility, NextLedger.Wisdom, NextLedger.MaxHealth};
    FY01SourceTransaction Tx; R = L->FromSource(S->StageMutationAt(Plan, Tx, Now)); if (!R.IsOk()) { return R; }
    FY01ScopedSourceMutation Guard(MoveTemp(Tx));
    if (bEnded || L->bEnded || L->Transactions.bInterrupted || ASC->UnitStats.Strength != BeforeStrength
        || ASC->UnitStats.Agility != BeforeAgility || ASC->UnitStats.Wisdom != BeforeWisdom)
    { return L->Fail(E::Interrupted, TEXT("成长暂存期间会话或事务被中断")); }
    return L->FromSource(Guard.CommitAt([&]
    {
        L->Items = MoveTemp(NextItems); L->Devoured = MoveTemp(NextDevoured); ++L->Revision; Ledger = MoveTemp(NextLedger);
    }));
}
void UY01GrowthComponent::NotifyBasicAttack()
{
    if (IsInGameThread() && Player.IsValid() && bInitialized && !bEnded && PendingBasicAttacks < MAX_int32) { ++PendingBasicAttacks; }
}
bool UY01GrowthComponent::SettleTick()
{
    if (!IsInGameThread() || !Player.IsValid()) { return false; }
    auto* P = Player.Get(); auto* L = P->GetLoadout(); auto* S = P->GetSourceEffects();
    if (!P->HasAuthority() || bEnded || !bInitialized || L->bEnded || S->bEnded) { return false; }
    const double Now = GetWorld()->GetTimeSeconds();
    const double RawElapsed = LastTickAt > 0 ? FMath::Min(Now - LastTickAt, 10.) : 0.;
    LastTickAt = Now;
    auto* GS = GetWorld()->GetGameState<AY01GameState>();
    if (!GS || GS->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { return false; }
    // 时间成长只在主线Battle计时；普攻成长开局即结算（含开波前与练功房）。
    const double Elapsed = GS->GetWaveSnapshot().Phase == EY01MatchPhase::Battle ? RawElapsed : 0.;
    if (Elapsed <= 0 && PendingBasicAttacks == 0) { return false; }
    TArray<FGrowthBatch> Batches;
    // 只换算本次增量入批（Rule.Delta），累计规则与击杀路径共用 ApplyBatches。
    auto Collect = [&](FGrowthBatch& Batch, const TArray<FY01ItemGrowth>& Rules)
    {
        for (const auto& Rule : Rules)
        {
            if (Rule.Trigger == EY01GrowthTrigger::CombatTime && Elapsed > 0)
            { FY01ItemGrowth R2 = Rule; R2.Delta = Elapsed * Rule.Delta; Batch.Rules.Add(MoveTemp(R2)); }
            else if (Rule.Trigger == EY01GrowthTrigger::BasicAttack && PendingBasicAttacks > 0)
            { FY01ItemGrowth R2 = Rule; R2.Delta = PendingBasicAttacks * Rule.Delta; Batch.Rules.Add(MoveTemp(R2)); }
        }
    };
    auto Add = [&](FGrowthBatch&& Batch)
    { if (!Batch.Rules.IsEmpty()) { Batches.Add(MoveTemp(Batch)); } };
    // 装备实例：只在槽内计时（卸下暂停并保留已得）。
    for (const auto& Id : L->EquipmentSlots)
    {
        const auto* Item = L->Items.Find(Id); if (!Item) { continue; }
        const auto Source = L->SourceId(Id);
        if (!S->Records.ContainsByPredicate([&](const auto& R) { return R.Source.Id == Source.Id; })) { continue; }
        FGrowthBatch Batch; Batch.Source = Source; Batch.ItemId = Id;
        Collect(Batch, Item->Growth);
        Add(MoveTemp(Batch));
    }
    // 已吞噬装备按原触发方式继续成长，本局常驻。
    for (const auto& D : L->Devoured)
    {
        const auto Source = L->SourceId(D.ItemId, EY01RuntimeSourceKind::Devoured);
        if (!S->Records.ContainsByPredicate([&](const auto& R) { return R.Source.Id == Source.Id; })) { continue; }
        FGrowthBatch Batch; Batch.Source = Source; Batch.ItemId = D.ItemId;
        Collect(Batch, D.Growth);
        Add(MoveTemp(Batch));
    }
    // 玩家侧规则（技能/天赋/商品）：来源仍在 Records 即计时；Kind/LocalKey 与击杀路径（FreezeSources）完全一致。
    for (const auto& Skill : L->Skills)
    {
        if (!Skill.LearnedId.IsValid()) { continue; }
        const auto Source = L->SourceId(Skill.LearnedId, EY01RuntimeSourceKind::Skill);
        if (!S->Records.ContainsByPredicate([&](const auto& R) { return R.Source.Id == Source.Id; })) { continue; }
        FGrowthBatch Batch; Batch.Source = Source;
        Collect(Batch, Skill.Growth);
        Add(MoveTemp(Batch));
    }
    auto* Economy = P->GetEconomy();
    for (const auto& Id : Economy->OfferOrder)
    {
        const auto& Offer = Economy->OfferStates.FindChecked(Id);
        if (!Offer.bPurchased) { continue; }
        const auto* Record = S->Records.FindByPredicate([&](const auto& R) { return R.Source.Id == Offer.SourceId; });
        if (!Record) { continue; }
        FGrowthBatch Batch; Batch.Source = Record->Source;
        Collect(Batch, Offer.Growth);
        Add(MoveTemp(Batch));
    }
    const auto& Choice = Economy->TalentState;
    if (!Choice.SelectedTalent.IsNone())
    {
        const auto* Selected = Choice.Candidates.FindByPredicate([&](const auto& C) { return C.TalentId == Choice.SelectedTalent; });
        const auto* Record = S->Records.FindByPredicate([&](const auto& R) { return R.Source.Id == Choice.SourceId; });
        if (Selected && Record)
        {
            FGrowthBatch Batch; Batch.Source = Record->Source;
            Collect(Batch, Selected->Growth);
            Add(MoveTemp(Batch));
        }
    }
    if (Batches.IsEmpty()) { return false; }
    // 先清零再应用；应用失败按既有 Fault 路径记录，计数不回滚。
    PendingBasicAttacks = 0;
    TGuardValue<bool> Consuming(bConsuming, true);
    if (!L->Transactions.Enter()) { return false; }
    ON_SCOPE_EXIT { L->Transactions.Leave(); L->PublishCommittedView(); };
    const auto Result = ApplyBatches(Batches);
    if (!Result.IsOk())
    {
        Ledger.FaultKillEventId.Invalidate(); Ledger.GrowthFault = Result.Code; Ledger.FaultMessage = Result.Message;
        UE_LOG(LogTemp, Display, TEXT("Y01R37 GrowthTickFault Batch=%llu Code=%d Reason=%s"),
            NextTickBatchId++, int32(Result.Code), *Result.Message);
    }
    return true;
}
void UY01GrowthComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!GetOwner() || !GetOwner()->HasAuthority() || !Player.IsValid() || bEnded) { return; }
    // 击杀优先：HasPending 含 Drain 全局真，与 BlocksMutation 同语义，避免与击杀 Drain 竞争事务。
    if (GetWorld()->GetSubsystem<UY01KillRewards>()->HasPending(Player.Get())) { return; }
    SettleTick();
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
