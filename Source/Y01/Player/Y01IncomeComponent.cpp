#include "Player/Y01IncomeComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01LoadoutComponent.h"
#include "Match/Y01GameFlow.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include <cmath>

namespace
{
using EError = EY01RuntimeError;
using EPhase = EY01IncomePhase;
FY01RuntimeResult Fail(EError Code, const TCHAR* Message) { return FY01RuntimeResult::Fail(Code, Message); }
}
UY01IncomeComponent::UY01IncomeComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    bAutoActivate = false;
}
double UY01IncomeComponent::ReadNow() const
{
#if WITH_DEV_AUTOMATION_TESTS
    if (TestClock) { return TestClock(); }
#endif
    return FPlatformTime::Seconds();
}
FY01RuntimeResult UY01IncomeComponent::Initialize(const FGuid& Match, int32 JoinSeq, double Remainder)
{
    auto* P = Cast<AY01PlayerState>(GetOwner());
    if (!IsInGameThread() || !IsValid(P) || !P->HasAuthority() || P->IsActorBeingDestroyed())
    { return Fail(EError::Authority, TEXT("收入初始化需要有效服务器PlayerState")); }
    if (!Match.IsValid() || JoinSeq <= 0) { return Fail(EError::InvalidIdentity, TEXT("会话身份无效")); }
    if (!FMath::IsFinite(Remainder) || Remainder < 0 || Remainder >= 1)
    { return Fail(EError::InvalidNumber, TEXT("初始余量必须有限且位于[0,1)")); }
    if (bEnded || State.Phase == EPhase::Stopped || State.Phase == EPhase::Fault)
    { return Fail(EError::Stopped, TEXT("收入会话已永久停止")); }
    if (State.Phase != EPhase::Disabled)
    {
        return Player == P && MatchId == Match && JoinSequence == JoinSeq ? FY01RuntimeResult()
            : Fail(EError::Session, TEXT("不得覆盖收入会话身份"));
    }
    Player = P; MatchId = Match; JoinSequence = JoinSeq;
    State.Phase = EPhase::Waiting; State.StartRemainder = Remainder; State.Remainder = Remainder;
    SetActiveFlag(true);
    return {};
}
FY01RuntimeResult UY01IncomeComponent::CheckOwner(bool bEnding) const
{
    const auto* P = Player.Get();
    if (!IsInGameThread() || !P || P != GetOwner() || !P->HasAuthority() || (!bEnding && P->IsActorBeingDestroyed()))
    { return Fail(EError::Authority, TEXT("收入操作需要有效权威拥有者")); }
    if (State.Phase == EPhase::Disabled) { return Fail(EError::Disabled, TEXT("收入尚未显式初始化")); }
    return {};
}
FY01RuntimeResult UY01IncomeComponent::Start()
{
    auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    if (auto* S = Player->GetSourceEffects(); S->bBusy) { return S->Interrupt(); }
    if (bEnded || State.Phase == EPhase::Stopped || State.Phase == EPhase::Fault)
    { return Fail(EError::Stopped, TEXT("停止或故障收入不能重新Start")); }
    if (State.Phase == EPhase::Running) { return {}; }
    if (bPaused) { State.Phase = EPhase::Running; return {}; }
    const double Now = ReadNow();
    if (!FMath::IsFinite(Now) || Now < 0) { return Fail(EError::Time, TEXT("Start时间必须有限非负")); }
    State.StartTime = State.LastTime = Now; State.StartRemainder = State.Remainder;
    State.PostedWhole = 0; State.Phase = EPhase::Running;
    return {};
}
FY01RuntimeResult UY01IncomeComponent::Pause()
{
    if (State.Phase == EPhase::Disabled) { return {}; }
    auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    if (bPaused || bPausing) { return {}; }
    TGuardValue<bool> Guard(bPausing, true);
    // 结算仅写收入账本和余额，不触碰来源暂存、不中断外层事务。
    R = SettleAt(ReadNow()); bPaused = true;
    return R;
}
FY01RuntimeResult UY01IncomeComponent::Resume()
{
    if (State.Phase == EPhase::Disabled) { return {}; }
    auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    if (bEnded || State.Phase == EPhase::Fault || State.Phase == EPhase::Stopped)
    { return Fail(EError::Stopped, TEXT("结束或故障收入不能恢复")); }
    if (!bPaused) { return {}; }
    if (bPausing || Player->GetSourceEffects()->bBusy || Player->GetSourceEffects()->bInCall)
    { return Fail(EError::Busy, TEXT("收入恢复等待来源事务")); }
    TGuardValue<bool> Guard(bPausing, true);
    const double Now = ReadNow();
    R = CheckOwner(); if (!R.IsOk()) { return R; }
    const auto* Mode = GetWorld()->GetAuthGameMode<AY01GameMode>();
    if (bEnded || State.Phase == EPhase::Fault || State.Phase == EPhase::Stopped || (Mode && Mode->IsGameplayFrozen()))
    { return Fail(EError::Stopped, TEXT("恢复期间收入结束或玩法冻结")); }
    if (Player->GetSourceEffects()->IsBusy()) { return Fail(EError::Busy, TEXT("恢复期间来源事务占用")); }
    if (!FMath::IsFinite(Now) || Now < State.LastTime) { return Fail(EError::Time, TEXT("恢复时间无效")); }
    State.StartTime = State.LastTime = Now; State.StartRemainder = State.Remainder; State.PostedWhole = 0;
    bPaused = false; return {};
}
FY01RuntimeResult UY01IncomeComponent::Settle()
{
    auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    if (bPaused) { return {}; }
    if (auto* S = Player->GetSourceEffects(); S->bBusy) { return S->Interrupt(); }
    return SettleAt(ReadNow());
}
FY01RuntimeResult UY01IncomeComponent::SettleAt(double Now, bool bEnding)
{
    auto R = CheckOwner(bEnding); if (!R.IsOk()) { return R; }
    if (State.Phase == EPhase::Fault) { return State.Fault; }
    if (bPaused || State.Phase == EPhase::Waiting || State.Phase == EPhase::Stopped) { return {}; }
    auto Fault = [&](EError Code, const TCHAR* Message)
    {
        State.Fault = Fail(Code, Message); State.Phase = EPhase::Fault;
        UE_LOG(LogTemp, Display, TEXT("Y01R36 INCOME_FAULT Owner=%s Code=%d Reason=%s"), *GetNameSafe(Player.Get()), static_cast<int32>(Code), Message);
        return State.Fault;
    };
    const double Elapsed = Now - State.StartTime;
    if (!FMath::IsFinite(Now) || !FMath::IsFinite(Elapsed) || Elapsed < 0 || Now < State.LastTime)
    { return Fault(EError::Time, TEXT("结算时间非有限或倒退，保留最后有效账本")); }
    if (Now == State.LastTime) { return {}; }
    const double Earned = Elapsed * State.Rate;
    const double Gross = State.StartRemainder + Earned;
    if (!FMath::IsFinite(Earned) || !FMath::IsFinite(Gross) || Gross < 0)
    { return Fault(EError::InvalidNumber, TEXT("收入gross溢出或无效")); }
    if ((Elapsed > 0 && State.Rate > 0 && Earned == 0)
        || (Earned > 0 && Gross == State.StartRemainder)
        || (State.StartRemainder > 0 && Gross == Earned)
        || (State.Rate > 0 && Gross <= static_cast<double>(State.PostedWhole) + State.Remainder))
    { return Fault(EError::Precision, TEXT("正收入或余量被完全吞掉，停止收入")); }
    const double WholeDouble = std::floor(Gross);
    // MAX_int64转double会向上舍入成2^63；必须在任何整数cast之前拒绝等号。
    constexpr double Int64Exclusive = 9223372036854775808.0;
    if (!FMath::IsFinite(WholeDouble) || WholeDouble < 0 || WholeDouble >= Int64Exclusive)
    { return Fault(EError::IntegerRange, TEXT("整额收入超出int64安全转换范围")); }
    const int64 Whole = static_cast<int64>(WholeDouble);
    if (State.PostedWhole < 0 || Whole < State.PostedWhole)
    { return Fault(EError::IntegerRange, TEXT("分段已入账整额无效")); }
    const int64 Delta = Whole - State.PostedWhole;
    const double Remainder = Gross - WholeDouble;
    const int32 Balance = Player->GetGold();
    if (Balance < 0 || Delta > static_cast<int64>(MAX_int32) - Balance)
    { return Fault(EError::BalanceRange, TEXT("金币容量不足，本次自然结算整次拒绝")); }
    if (State.TotalIncome < 0 || Delta > MAX_int64 - State.TotalIncome
        || State.TotalSpending < 0 || State.Sequence < 0 || State.Sequence == MAX_int64)
    { return Fault(EError::AuditRange, TEXT("收入累计或流水序列溢出")); }
    if (!FMath::IsFinite(Remainder) || Remainder < 0 || Remainder >= 1)
    { return Fault(EError::Precision, TEXT("分段余量无效")); }
    // 上面已验证所有算术和审计；唯一余额接口不广播玩法委托，没有半份账本窗口。
    if (Delta > 0 && !Player->TryAddMatchReward(static_cast<int32>(Delta), 0))
    { return Fault(EError::BalanceRange, TEXT("唯一余额入账被拒绝")); }
    State.PostedWhole = Whole; State.Remainder = Remainder; State.LastTime = Now;
    State.TotalIncome += Delta; ++State.Sequence;
    FY01IncomeEntry Entry;
    Entry.Sequence = State.Sequence; Entry.SegmentStart = State.StartTime; Entry.Time = Now;
    Entry.Rate = State.Rate; Entry.Delta = Delta; Entry.Balance = Player->GetGold(); Entry.Remainder = Remainder;
    if (State.Recent.Num() == 32) { State.Recent.RemoveAt(0, 1, EAllowShrinking::No); }
    State.Recent.Add(Entry);
    UE_LOG(LogTemp, Display, TEXT("Y01R36 INCOME Owner=%s Match=%s Join=%d Seq=%lld Kind=Natural Start=%.17g Time=%.17g Rate=%.17g Delta=%lld Balance=%d Remainder=%.17g"),
        *GetNameSafe(Player.Get()), *MatchId.ToString(), JoinSequence, Entry.Sequence, Entry.SegmentStart,
        Now, Entry.Rate, Delta, Entry.Balance, Remainder);
    return {};
}
FY01ItemResult UY01IncomeComponent::PrepareSpending(int32 Price, double Now, FPreparedSpending& Out) const
{
    Out = {}; FPreparedSpending Prepared;
    Prepared.Price = Price; Prepared.Time = Now; Prepared.Sequence = State.Sequence; Prepared.TotalSpending = State.TotalSpending;
    auto R = ValidateSpending(Prepared); if (!R.IsOk()) { return R; }
    Prepared.Recent = State.Recent;
    if (Prepared.Recent.Num() == 32) { Prepared.Recent.RemoveAt(0, 1, EAllowShrinking::No); }
    Prepared.Recent.AddDefaulted(); Out = MoveTemp(Prepared); return R;
}
FY01ItemResult UY01IncomeComponent::ValidateSpending(const FPreparedSpending& Prepared) const
{
    using E = EY01ItemError;
    const auto Reject = [](E Code, const TCHAR* Message) { return FY01ItemResult::Fail(Code, 0, Message); };
    if (!CheckOwner().IsOk() || !Player->GetLoadout()->IsBusy())
    { return Reject(E::Authority, TEXT("钱包提交只允许有效拥有者持有原库存事务")); }
    if (bEnded || State.Phase == EPhase::Stopped || State.Phase == EPhase::Fault)
    { return Reject(E::Stopped, TEXT("停止或故障收入不能支出")); }
    if (Prepared.Price < 0 || !FMath::IsFinite(Prepared.Time) || Prepared.Time < 0 || Player->GetGold() < 0
        || State.TotalSpending < 0 || Prepared.Price > MAX_int64 - State.TotalSpending
        || State.Sequence < 0 || State.Sequence == MAX_int64 || State.Recent.Num() > 32)
    { return Reject(E::Range, TEXT("价格、余额、累计支出或审计序列范围无效")); }
    if (Prepared.Sequence != State.Sequence || Prepared.TotalSpending != State.TotalSpending)
    { return Reject(E::Interrupted, TEXT("暂存期间钱包审计被改变")); }
    if (Prepared.Price > Player->GetGold()) { return Reject(E::InsufficientFunds, TEXT("金币不足")); }
    return {};
}
void UY01IncomeComponent::CommitSpending(FPreparedSpending&& Prepared)
{
    // 验证已完成：只修改实时余额及支出字段，不恢复旧Gold或整份收入分段。
    Player->Gold -= Prepared.Price; State.TotalSpending += Prepared.Price; ++State.Sequence;
    auto& Entry = Prepared.Recent.Last(); Entry.Sequence = State.Sequence; Entry.Kind = EY01IncomeEntryKind::Spending;
    Entry.SegmentStart = State.StartTime; Entry.Time = Prepared.Time; Entry.Rate = State.Rate;
    Entry.Delta = -static_cast<int64>(Prepared.Price); Entry.Balance = Player->GetGold(); Entry.Remainder = State.Remainder;
    State.Recent = MoveTemp(Prepared.Recent);
}
void UY01IncomeComponent::ChangeRate(double Rate, double Now)
{
    // 仅来源组件在完整预检和旧段结算之后调用，无委托、无失败步骤。
    if (State.Rate == Rate) { return; }
    State.Rate = Rate;
    if (bPaused) { return; }
    State.StartTime = State.LastTime = Now;
    State.StartRemainder = State.Remainder; State.PostedWhole = 0;
}
FY01RuntimeResult UY01IncomeComponent::Stop()
{
    auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    if (auto* S = Player->GetSourceEffects(); S->bBusy)
    { return S->RequestStop(); }
    return StopAt(ReadNow());
}
FY01RuntimeResult UY01IncomeComponent::StopAt(double Now, bool bEnding)
{
    auto R = CheckOwner(bEnding); if (!R.IsOk()) { return R; }
    if (State.Phase == EPhase::Stopped) { return {}; }
    R = SettleAt(Now, bEnding);
    if (State.Phase != EPhase::Fault) { State.Phase = EPhase::Stopped; }
    bEnded = true; SetActiveFlag(false);
    return R;
}
FY01IncomeSnapshot UY01IncomeComponent::Observe() const
{
    FY01IncomeSnapshot Snapshot;
    if (const auto* P = Player.Get(); P && P->GetLoadout()->IsInitialized()
        && (P->GetLoadout()->IsBusy() || P->GetSourceEffects()->bBusy))
    { Snapshot.State.Fault = Fail(EError::Busy, TEXT("共同事务期间不发布半份收入快照")); return Snapshot; }
    Snapshot.State = State;
    if (const auto* P = Player.Get()) { Snapshot.Balance = P->GetGold(); }
    return Snapshot;
}
void UY01IncomeComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    if (!bEnded && State.Phase != EPhase::Disabled)
    {
        if (Player.IsValid() && Player->GetSourceEffects()->bBusy)
        { Player->GetSourceEffects()->RequestStop(); }
        else { StopAt(ReadNow(), true); }
    }
    bEnded = true;
    if (HasBegunPlay()) { Super::EndPlay(Reason); }
}
