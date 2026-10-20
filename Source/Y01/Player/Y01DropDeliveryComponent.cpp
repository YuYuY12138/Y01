#include "Player/Y01DropDeliveryComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01EconomyComponent.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Match/Y01KillRewards.h"
#include "Match/Y01GameFlow.h"
#include "Combat/Y01Core.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

namespace { using E = EY01ItemError; using K = EY01TableKind; }

UY01DropDeliveryComponent::UY01DropDeliveryComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    bAutoActivate = false;
}
FY01ItemResult UY01DropDeliveryComponent::Initialize(double FlightSeconds)
{
    auto* P = Cast<AY01PlayerState>(GetOwner());
    if (!IsInGameThread() || !IsValid(P) || !P->HasAuthority() || P->GetDropDelivery() != this)
    { return FY01ItemResult::Fail(E::Authority, 0, TEXT("交付只属于唯一权威PlayerState")); }
    auto* L = P->GetLoadout(); auto* Economy = P->GetEconomy();
    if (!FMath::IsFinite(FlightSeconds) || FlightSeconds < 0)
    { return L->Fail(E::InvalidDefinition, TEXT("飞行秒数必须为有限非负double，零表示下次可执行调度")); }
    if (Ledger.bEnded || (L->GameState.IsValid() && L->GameState->GetWaveSnapshot().Phase == EY01MatchPhase::Ended))
    { return L->Fail(E::Stopped, TEXT("交付结束后不能恢复")); }
    auto R = L->CheckOwner(); if (!R.IsOk()) { return R; }
    if (!Economy->bInitialized || Economy->MatchId != L->MatchId || Economy->JoinSequence != L->JoinSequence)
    { return L->Fail(E::Session, TEXT("交付依赖同会话服务器经济快照")); }
    if (Ledger.bEnabled)
    { return Player == P && MatchId == L->MatchId && JoinSequence == L->JoinSequence && Duration == FlightSeconds
        ? R : L->Fail(E::Session, TEXT("不能替换待交付会话或ETA配置")); }
    Player = P; MatchId = L->MatchId; JoinSequence = L->JoinSequence; Duration = FlightSeconds;
    Ledger.bEnabled = true; SetActiveFlag(true);
    KillHook = GetWorld()->GetSubsystem<UY01KillRewards>()->OnEffectiveKill.AddUObject(this, &UY01DropDeliveryComponent::Consume);
    L->PublishCommittedView(); return R;
}
void UY01DropDeliveryComponent::Consume(const FY01KillEvent& Event)
{
    if (!IsInGameThread() || !IsEnabled() || !Player.IsValid() || Event.Killer != Player
        || Event.MatchId != MatchId || Event.JoinSequence != JoinSequence || !Event.KillEventId.IsValid()
        || !Event.SpawnId.IsValid() || !Event.Sequence || Event.Sequence <= LastSequence) { return; }
    for (const auto& Id : RecentEvents) { if (Id == Event.KillEventId) { return; } }
    // 发布者严格按致死序列排空；高水位拒绝任意久以前的同消息，不累积无限事件集合。
    LastSequence = Event.Sequence; RecentEvents[Event.Sequence % UE_ARRAY_COUNT(RecentEvents)] = Event.KillEventId;
    auto* L = Player->GetLoadout(); auto* Economy = Player->GetEconomy();
    auto Fault = [&](E Code)
    { Ledger.FaultKillEventId = Event.KillEventId; Ledger.Fault = Code; ++Mutation; L->PublishCommittedView(); };
    if (L->GameState->GetWaveSnapshot().Phase == EY01MatchPhase::Ended)
    { EndSession(EY01DeliveryCancel::Ended); return; }
    const auto Catalog = Economy->SnapshotCatalog(); FY01DataValidation Errors;
    const auto* Monster = Catalog.FindRow<FY01MonsterRow>(Event.Definition, Errors);
    if (!Monster || Event.Definition.Type != K::Monster) { Fault(E::InvalidDefinition); return; }
    if (Monster->ItemDropChance == 0) { return; }
    auto Random = Economy->LootRandom;
    if (Monster->ItemDropChance < 1 && double(Random.GetFraction()) >= Monster->ItemDropChance)
    { Economy->LootRandom = Random; return; }
    const double ETA = Event.DeathTime + Duration;
    if (!FMath::IsFinite(Event.DeathTime) || Event.DeathTime < 0 || !FMath::IsFinite(ETA) || ETA < Event.DeathTime
        || (Duration > 0 && ETA == Event.DeathTime) || Event.Origin.ContainsNaN()) { Fault(E::Precision); return; }
    const auto* Reward = Catalog.FindRow<FY01RewardPackageRow>(Monster->DropReward, Errors);
    if (!Reward || Reward->Domain != EY01RewardDomain::MatchPersonal || Reward->Gold || Reward->Experience || Reward->Items.IsEmpty())
    { Fault(E::InvalidDefinition); return; }
    TArray<FY01DeliveryRecord> Records;
    for (const auto& Entry : Reward->Items)
    {
        if (Entry.Quantity <= 0 || Entry.Quantity > MAX_int32 - Records.Num() - Ledger.Records.Num()) { Fault(E::Range); return; }
        for (int32 I = 0; I < Entry.Quantity; ++I)
        {
            FY01DataRef Definition = Entry.Item;
            if (Definition.Type == K::LocalItemPool && !Catalog.SamplePool(Entry.Item, Random, Definition, Errors))
            { Fault(E::InvalidDefinition); return; }
            FY01ItemInstance Item; const auto R = L->Resolve(Catalog, Definition, Item);
            if (!R.IsOk()) { Fault(R.Code); return; }
            auto& Record = Records.AddDefaulted_GetRef(); Record.DeliveryId = FGuid::NewGuid(); Record.ItemId = FGuid::NewGuid();
            Record.KillEventId = Event.KillEventId; Record.MatchId = MatchId; Record.JoinSequence = JoinSequence;
            Record.Definition = Definition; Record.Quality = Item.Quality; Record.Origin = Event.Origin;
            Record.StartTime = Event.DeathTime; Record.ETA = ETA;
        }
    }
    // 此处仍处于击杀Drain，不进入库存；整包结果成功归队后才推进独立LootRandom。
    Ledger.Records.Append(MoveTemp(Records)); Economy->LootRandom = Random; ++Mutation;
    L->PublishCommittedView();
}
FY01ItemResult UY01DropDeliveryComponent::Deliver(const FGuid& DeliveryId, double Now)
{
    auto* L = Player->GetLoadout(); auto* S = Player->GetSourceEffects();
    if (L->IsBusy() || S->bBusy || S->bInCall || !L->Transactions.Enter())
    { return L->Fail(E::Busy, TEXT("交付等待原库存或来源事务，不打断当前事务")); }
    ON_SCOPE_EXIT { L->Transactions.Leave(); };
    auto R = L->CheckOwner(); if (!R.IsOk()) { return R; }
    const auto* Found = Ledger.Records.FindByPredicate([&](const auto& V) { return V.DeliveryId == DeliveryId; });
    if (!Found || Found->State != EY01DeliveryState::Pending) { return L->Fail(E::Interrupted, TEXT("交付记录已改变")); }
    const auto Record = *Found; const uint64 Before = Mutation;
    UY01LoadoutComponent::FPreparedBatch Batch;
    R = L->PrepareBatch(Player->GetEconomy()->SnapshotCatalog(), {{Record.ItemId, Record.Definition}}, {}, Batch);
    if (!R.IsOk()) { return R; }
    // 到货身份仅由权威交付写入暂存物品，与库存及Delivered共同提交；不开放给普通入包请求。
    // 叠加到既有道具格时不改写该格到货身份，回执ItemId保留为已并入的预分配身份。
    if (!Batch.Merged.Contains(Record.ItemId))
    {
        Batch.Items.FindChecked(Record.ItemId).ArrivalDeliveryId = Record.DeliveryId;
        Batch.Result.Changes[0].Item.ArrivalDeliveryId = Record.DeliveryId;
    }
    auto Next = Ledger;
    auto* Delivered = Next.Records.FindByPredicate([&](const auto& V) { return V.DeliveryId == DeliveryId; });
    Delivered->State = EY01DeliveryState::Delivered; Delivered->DeliveredTime = Now; Delivered->DelaySeconds = Now - Record.ETA;
    // 来源事务仍使用其独立单调时钟；绝不与世界时间ETA相减。
    double SourceTime = 0; R = L->FromSource(S->SettleMutationTime(SourceTime)); if (!R.IsOk()) { return R; }
    return L->CommitBatch(nullptr, nullptr, MoveTemp(Batch), {}, {}, SourceTime, [&]
    {
        if (!IsEnabled() || L->GameState->GetWaveSnapshot().Phase == EY01MatchPhase::Ended)
        { return L->Fail(E::Stopped, TEXT("交付期间结束")); }
        if (Mutation != Before) { return L->Fail(E::Busy, TEXT("交付期间队列更新，保留原结果重试")); }
        if (Record.MatchId != L->MatchId || Record.JoinSequence != L->JoinSequence)
        { return L->Fail(E::Session, TEXT("交付收件会话不匹配")); }
        return L->Fail(E::None, TEXT(""));
    }, [&] { Ledger = MoveTemp(Next); ++Mutation; }, true);
}
bool UY01DropDeliveryComponent::SettleOwnedOnFallen()
{
    const auto* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AY01GameMode>() : nullptr;
    const auto* Core = Mode ? Mode->GetMatchCore() : nullptr;
    if (!IsInGameThread() || !GetOwner()->HasAuthority() || !Mode || !Mode->IsGameplayFrozen()
        || !IsValid(Core) || !Core->GetSnapshot().bFallen) { return false; }
    if (!Ledger.bEnabled) { return true; }
    if (!IsEnabled() || !Player.IsValid() || bScheduling) { return false; }
    bFallenSettlement = true;
    Schedule();
    return Ledger.Fault == E::None && !Ledger.Records.ContainsByPredicate([](const auto& R)
        { return R.State != EY01DeliveryState::Delivered; });
}
void UY01DropDeliveryComponent::Schedule()
{
    if (!IsInGameThread() || !IsEnabled() || !Player.IsValid() || bScheduling) { return; }
    auto* L = Player->GetLoadout();
    if (!L->GameState.IsValid() || L->GameState->GetWaveSnapshot().Phase == EY01MatchPhase::Ended)
    { EndSession(EY01DeliveryCancel::Ended); return; }
    TGuardValue<bool> Guard(bScheduling, true);
    const double Now = L->GameState->GetServerWorldTimeSeconds();
    if (!FMath::IsFinite(Now) || Now < 0) { return; }
    TArray<FGuid> Due;
    for (const auto& Record : Ledger.Records)
    { if (Record.State == EY01DeliveryState::Pending && (bFallenSettlement || Now >= Record.ETA)) { Due.Add(Record.DeliveryId); } }
    for (const auto& Id : Due)
    {
        if (!IsEnabled()) { break; }
        const auto Result = Deliver(Id, Now);
        auto* Record = Ledger.Records.FindByPredicate([&](const auto& V) { return V.DeliveryId == Id; });
        if (!Record || Record->State != EY01DeliveryState::Pending || Result.IsOk()) { continue; }
        if (Result.Code == E::Busy || Result.Code == E::Interrupted)
        { if (Record->RetryCount != MAX_uint32) { ++Record->RetryCount; } Record->DelaySeconds = Now - Record->ETA; }
        else { Record->State = EY01DeliveryState::Fault; Record->Fault = Result.Code; }
        ++Mutation;
    }
    Prune(); L->PublishCommittedView();
}
void UY01DropDeliveryComponent::Prune()
{
    // 只裁剪已终结视觉回执，绝不裁剪待交付物品或设置背包容量。
    int32 Terminal = 0;
    for (int32 I = Ledger.Records.Num() - 1; I >= 0; --I)
    { if ((Ledger.Records[I].State == EY01DeliveryState::Delivered || Ledger.Records[I].State == EY01DeliveryState::Cancelled)
        && ++Terminal > 64) { Ledger.Records.RemoveAt(I); } }
}
void UY01DropDeliveryComponent::EndSession(EY01DeliveryCancel Reason)
{
    if (!IsInGameThread() || !GetOwner() || !GetOwner()->HasAuthority() || !Ledger.bEnabled || Ledger.bEnded) { return; }
    Ledger.bEnded = true; ++Mutation;
    if (auto* Kills = GetWorld()->GetSubsystem<UY01KillRewards>()) { Kills->OnEffectiveKill.Remove(KillHook); }
    for (auto& Record : Ledger.Records)
    { if (Record.State == EY01DeliveryState::Pending) { Record.State = EY01DeliveryState::Cancelled;
        Record.CancelReason = Reason == EY01DeliveryCancel::None ? EY01DeliveryCancel::EndPlay : Reason; } }
    SetActiveFlag(false); Prune();
    if (Player.IsValid()) { Player->GetLoadout()->PublishCommittedView(); }
}
void UY01DropDeliveryComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    EndSession(EY01DeliveryCancel::EndPlay);
    if (HasBegunPlay()) { Super::EndPlay(Reason); }
}
