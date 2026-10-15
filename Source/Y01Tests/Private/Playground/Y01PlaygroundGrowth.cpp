#include "Playground/Y01PlaygroundGrowth.h"
#include "Playground/Y01PlaygroundRuntime.h"
#include "Playground/Y01TestSkill.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "GameplayEffectExtension.h"
#include "Engine/World.h"
#include <cmath>

UY01PlaygroundGrowth::UY01PlaygroundGrowth() { PrimaryComponentTick.bCanEverTick = false; }
FY01GrowthView UY01PlaygroundGrowth::Observe(const AY01PlayerState* Player) const
{ const auto* L = Ledgers.Find(Player); return L ? L->View : FY01GrowthView(); }
void UY01PlaygroundGrowth::GrantAttack(AY01PlayerState* Player, FY01GrowthView& V, double Delta, double& Total, int64 Count)
{
    if (Delta == 0) { return; }
    auto* ASC = Player->GetCombatASC(); const auto Attribute = UY01AttributeSet::GetAttackDamageAttribute();
    // 同一float指数区间内，稳定后的逐秒舍入步长相同；批量跳到区间末尾。
    // 不把rate乘秒数后舍入：小到无法表示的单秒增量不能被合并成有效授予。
    for (int32 Segment = 0; Count > 0 && Segment < 1024; ++Segment)
    {
        const float Before = ASC->GetNumericAttributeBase(Attribute), Current = ASC->GetNumericAttribute(Attribute);
        const double Desired = static_cast<double>(Before) + Delta;
        float After = static_cast<float>(Desired);
        if (!FMath::IsFinite(Delta) || Delta < 0 || !FMath::IsFinite(Before) || !FMath::IsFinite(Current)
            || !FMath::IsFinite(Desired) || Desired > MAX_flt || !FMath::IsFinite(After) || After <= Before
            || static_cast<double>(Current) + (static_cast<double>(After) - Before) > MAX_flt)
        { V.Error = TEXT("测试攻击增量溢出或无法表示，已拒绝；金币独立结算"); return; }
        const double Step = static_cast<double>(After) - Before;
        int64 Batch = 1;
        if (Before > 0 && Count > 1)
        {
            int Exponent = 0; std::frexp(static_cast<double>(Before), &Exponent);
            const double Limit = std::ldexp(1., Exponent);
            const float Next = static_cast<float>(static_cast<double>(After) + Delta);
            if (After < Limit && static_cast<double>(Next) - After == Step)
            {
                const double Last = std::nextafter(static_cast<float>(Limit), 0.f);
                const double Room = FMath::Min((Last - Before) / Step, (static_cast<double>(MAX_flt) - Current) / Step);
                if (FMath::IsFinite(Room) && Room >= 1)
                { Batch = static_cast<int64>(FMath::Min(static_cast<double>(Count), std::floor(Room))); }
            }
        }
        After = static_cast<float>(static_cast<double>(Before) + Step * Batch);
        const double Expected = static_cast<double>(After) - Before;
        if (!FMath::IsFinite(Total + Expected) || Total + Expected <= Total)
        { V.Error = TEXT("测试攻击实际累计无法表示，已拒绝"); return; }
        // 只写base；不移除GE，不将含被动的current回写，也不修改任何在途快照。
        ASC->SetNumericAttributeBase(Attribute, After);
        const double Actual = static_cast<double>(ASC->GetNumericAttributeBase(Attribute)) - Before;
        Total += Actual; Count -= Batch;
        if (Actual != Expected) { V.Error = TEXT("测试攻击base授予被外部修改，已停止本批"); return; }
    }
    if (Count > 0) { V.Error = TEXT("测试攻击批次无法安全表示，已拒绝余下部分"); }
}
void UY01PlaygroundGrowth::GrantGold(AY01PlayerState* Player, FY01GrowthView& V, double Seconds)
{
    const double Rate = V.Config.GoldPerEffectiveSecond;
    if (Rate == 0) { return; }
    const double Low = std::floor(Rate), FractionRate = std::fmod(Rate, 1.);
    const double Start = static_cast<double>(V.EffectiveSeconds) - Seconds;
    const double FractionalAmount = FractionRate * V.EffectiveSeconds;
    if (!FMath::IsFinite(FractionalAmount)) { V.Error = TEXT("测试金币小数无法表示，已拒绝"); return; }
    // 以累计有效秒为统一基准；不反复累加小数，避免0.1的舍入随tick分组改变进位。
    // 整额是否成功不影响此基准，失败后腾出容量也不能补发此前整额。
    const double FirstCarry = std::floor(FractionRate * Start);
    auto CarryAt = [&](double Count) { return std::floor(FractionRate * (Start + Count)); };
    auto AmountAt = [&](double Count) { return Low * Count + (CarryAt(Count) - FirstCarry); };
    V.GoldRemainder = std::fmod(FractionalAmount, 1.);
    const double Amount = AmountAt(Seconds), Capacity = MAX_int32 - Player->GetGold();
    double Grant = 0;
    if (FMath::IsFinite(Amount) && Amount <= Capacity) { Grant = Amount; }
    else
    {
        // 等价逐秒TryAdd：一次性跳过可成功的前缀；失败整额丢弃、合法小数保留。
        // 首次失败后余额小于ceil(rate)，只可能再成功一个floor(rate)，无需逐秒循环。
        V.Error = TEXT("测试金币整额溢出，已丢弃且不积欠；攻击独立结算");
        if (Rate <= static_cast<double>(MAX_int32) + 1)
        {
            // 同一累计基准二分前缀，避开除法在整数边界上的舍入；有效秒上限保证至多53轮。
            int64 Prefix = 0, End = static_cast<int64>(Seconds);
            while (Prefix < End)
            {
                const int64 Middle = Prefix + (End - Prefix + 1) / 2;
                if (AmountAt(static_cast<double>(Middle)) <= Capacity) { Prefix = Middle; }
                else { End = Middle - 1; }
            }
            Grant = AmountAt(static_cast<double>(Prefix));
            const double Remaining = Seconds - Prefix;
            const double LowCount = Remaining - (CarryAt(Seconds) - CarryAt(static_cast<double>(Prefix)));
            if (Remaining > 0 && LowCount > 0 && Capacity - Grant >= Low) { Grant += Low; }
        }
    }
    if (!FMath::IsFinite(Grant) || Grant < 0 || Grant > MAX_int32 || V.TimeGoldGranted > MAX_int64 - static_cast<int64>(Grant))
    { V.Error = TEXT("测试金币实际累计无法表示，已拒绝"); return; }
    if (Grant > 0 && Player->TryAddMatchReward(static_cast<int32>(Grant), 0)) { V.TimeGoldGranted += static_cast<int64>(Grant); }
}
void UY01PlaygroundGrowth::Advance(AY01PlayerState* Player, FY01GrowthLedger& L, double Now)
{
    const double Elapsed = Now - L.LastSample; L.LastSample = Now;
    if (!L.bRunning || L.bStopped || !L.View.Config.bEnabled) { return; }
    auto& V = L.View; const double Time = V.SecondRemainder + Elapsed;
    // 双精度能精确表示的整数上界；拒绝而非回绕/钳零/无限补算。
    constexpr double MaxExact = 9007199254740991.;
    if (!FMath::IsFinite(Elapsed) || Elapsed < 0 || !FMath::IsFinite(Time) || Time > MaxExact - V.EffectiveSeconds)
    { V.Error = TEXT("有效时间溢出或逆行，已拒绝"); return; }
    const double Seconds = std::floor(Time); V.SecondRemainder = Time - Seconds;
    if (Seconds == 0) { return; }
    V.EffectiveSeconds += static_cast<int64>(Seconds);
    GrantAttack(Player, V, V.Config.AttackPerEffectiveSecond, V.TimeAttackGranted, static_cast<int64>(Seconds));
    GrantGold(Player, V, Seconds);
}
void UY01PlaygroundGrowth::Update()
{
    auto* Mode = Cast<AY01PlaygroundGameMode>(GetOwner());
    if (bStopped || !Mode || !Mode->HasAuthority() || !Mode->IsMatchConfigured() || !Mode->GetProfile() || !Mode->GetFlowState()) { return; }
    FY01DataValidation Q;
    const auto* Row = Mode->GetProfile()->BuildCatalog().FindRow<FY01CharacterRow>(Mode->GetProfile()->Character, Q);
    if (!Row) { return; }
    const double Now = GetWorld()->GetTimeSeconds();
    for (APlayerState* State : Mode->GetFlowState()->PlayerArray)
    {
        auto* Player = Cast<AY01PlayerState>(State);
        if (!IsValid(Player) || !Player->HasAuthority()) { continue; }
        auto* Existing = Ledgers.Find(Player);
        if (!Existing)
        {
            auto& L = Ledgers.Add(Player); L.View.Config = Row->R33TestGrowth; L.LastSample = Now;
            Player->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddWeakLambda(this,
                [this, Weak = TWeakObjectPtr<AY01PlayerState>(Player)](const FOnAttributeChangeData& D)
                { if (Weak.IsValid() && D.OldValue > 0 && D.NewValue <= 0) { StopPlayer(Weak.Get(), TEXT("死亡")); } });
            Existing = &L;
        }
        auto& L = *Existing;
        if (L.bStopped) { continue; }
        auto* Pawn = Cast<APawn>(Player->GetCombatASC()->GetAvatarActor());
        FString Reason;
        if (!Player->IsCharacterInitialized()) { Reason = TEXT("等待初始化"); }
        else if (!IsValid(Pawn) || Pawn->IsActorBeingDestroyed() || !Pawn->GetController()) { Reason = TEXT("无有效Pawn"); }
        else if (Y01Playground::Health(Player->GetCombatASC()) <= 0) { Reason = TEXT("死亡"); }
        else if (Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { Reason = TEXT("Ended"); }
        else if (!L.View.Config.bEnabled) { Reason = TEXT("配置关闭"); }
        if (!Reason.IsEmpty()) { L.bRunning = false; L.LastSample = Now; L.View.StopReason = Reason; continue; }
        if (!L.bRunning) { L.LastSample = Now; L.bRunning = true; }
        L.View.StopReason = TEXT("运行中（R33测试，非正式平衡）"); Advance(Player, L, Now);
    }
    for (auto It = Ledgers.CreateIterator(); It; ++It) { if (!It.Key().IsValid()) { It.RemoveCurrent(); } }
    for (auto It = Monsters.CreateIterator(); It; ++It) { if (!It.Key().IsValid()) { It.RemoveCurrent(); } }
}
void UY01PlaygroundGrowth::StopPlayer(AY01PlayerState* Player, const FString& Reason)
{
    if (auto* L = Ledgers.Find(Player))
    { Advance(Player, *L, GetWorld()->GetTimeSeconds()); L->bRunning = false; L->bStopped = true; L->View.StopReason = Reason; }
}
void UY01PlaygroundGrowth::StopAll(const FString& Reason)
{
    for (auto& Pair : Ledgers) { if (Pair.Key.IsValid()) { StopPlayer(Pair.Key.Get(), Reason); } }
    bStopped = true;
}
void UY01PlaygroundGrowth::RegisterMonster(AY01Monster* Monster, AY01PlayerState* TrainingOwner)
{
    if (bStopped || !GetOwner()->HasAuthority() || !IsValid(Monster) || Monsters.Contains(Monster)) { return; }
    Monsters.Add(Monster, {TrainingOwner, TrainingOwner != nullptr, false});
    Monster->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddWeakLambda(this,
        [this, Weak = TWeakObjectPtr<AY01Monster>(Monster)](const FOnAttributeChangeData& Data) { OnMonsterHealth(Weak, Data); });
}
void UY01PlaygroundGrowth::OnMonsterHealth(TWeakObjectPtr<AY01Monster> Monster, const FOnAttributeChangeData& Data)
{
    auto* Record = Monsters.Find(Monster);
    if (bStopped || !Record || Record->bConsumed || !Monster.IsValid() || Monster->IsActorBeingDestroyed()
        || Data.OldValue <= 0 || Data.NewValue > 0) { return; }
    Record->bConsumed = true;
    const auto* Mod = Data.GEModData;
    if (!Mod || Mod->EvaluatedData.Attribute != UY01AttributeSet::GetHealthAttribute()
        || Mod->EvaluatedData.ModifierOp != EGameplayModOp::Additive || !FMath::IsFinite(Mod->EvaluatedData.Magnitude) || Mod->EvaluatedData.Magnitude >= 0
        || !Mod->EffectSpec.Def || !(Mod->EffectSpec.Def->IsA<UY01DamageEffect>() || Mod->EffectSpec.Def->IsA<UY01TestSkillDamageEffect>())) { return; }
    auto* Killer = Cast<UY01CombatAbilitySystem>(Mod->EffectSpec.GetContext().GetOriginalInstigatorAbilitySystemComponent());
    auto* Player = Killer ? Cast<AY01PlayerState>(Killer->GetOwner()) : nullptr;
    auto* Mode = Cast<AY01PlaygroundGameMode>(GetOwner());
    if (!Player || !Player->HasAuthority() || !Player->IsCharacterInitialized() || !IsValid(Killer->GetAvatarActor())
        || Y01Playground::Health(Killer) <= 0 || !Mode->GetFlowState()->PlayerArray.Contains(Player)
        || Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { return; }
    // 死亡已先使目标CombatSession失效；只核查生成登记及分区，不重跑要求活目标的攻击授权。
    if (Record->bTraining ? Record->Owner.Get() != Player || !Mode->GetTraining()->Monsters(Player).Contains(Monster.Get())
        : Mode->GetTraining()->Observe(Player).bInside || !Mode->GetWaveMonsters().Contains(Monster.Get())) { return; }
    auto* L = Ledgers.Find(Player);
    if (!L || L->bStopped || !L->View.Config.bEnabled) { return; }
    if (L->View.GrowthKills == MAX_int64) { L->View.Error = TEXT("有效击杀计数溢出，已拒绝"); return; }
    ++L->View.GrowthKills;
    GrantAttack(Player, L->View, L->View.Config.AttackPerEffectiveKill, L->View.KillAttackGranted);
}
void UY01PlaygroundGrowth::EndPlay(const EEndPlayReason::Type Reason)
{ StopAll(TEXT("EndPlay")); Monsters.Reset(); Ledgers.Reset(); Super::EndPlay(Reason); }
