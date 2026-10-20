#include "Match/Y01KillRewards.h"
#include "Combat/Y01Monster.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01GrowthComponent.h"
#include "Match/Y01GameFlow.h"
#include "Engine/World.h"

bool UY01KillRewards::Enable(const FGuid& Match)
{
    if (!IsInGameThread() || !GetWorld()->GetAuthGameMode<AY01GameMode>() || !Match.IsValid() || bEnded
        || (bEnabled && MatchId != Match)) { return false; }
    MatchId = Match; bEnabled = true; return true;
}
FGuid UY01KillRewards::RegisterMonster(AY01Monster* Monster)
{
    if (!IsInGameThread() || !bEnabled || bEnded || !IsValid(Monster) || !Monster->HasAuthority()
        || Monster->GetWorld() != GetWorld() || Monster->IsActorBeingDestroyed()) { return {}; }
    auto* ASC = Monster->GetCombatASC();
    if (!ASC || !ASC->IsCombatInitialized() || ASC->GetDefinition().Type != EY01TableKind::Monster
        || ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) <= 0) { return {}; }
    if (const auto* Existing = Spawns.Find(Monster)) { return Existing->Id; }
    // 弱引用过期项不再参与登记；存活或已死亡同对象不允许重置身份。
    for (auto It = Spawns.CreateIterator(); It; ++It) { if (!It.Key().IsValid()) { It.RemoveCurrent(); } }
    FSpawn Spawn; Spawn.Id = FGuid::NewGuid(); Spawn.Definition = ASC->GetDefinition();
    const FGuid Id = Spawn.Id; Spawns.Add(Monster, MoveTemp(Spawn)); return Id;
}
FY01KillAuthorization UY01KillRewards::Capture(UY01CombatAbilitySystem* Source, UY01CombatAbilitySystem* Target) const
{
    FY01KillAuthorization A;
    if (!IsInGameThread() || !bEnabled || bEnded || !IsValid(Source) || !IsValid(Target)
        || Source->GetWorld() != GetWorld() || Target->GetWorld() != GetWorld()) { return A; }
    auto* Player = Cast<AY01PlayerState>(Source->GetOwner());
    auto* Monster = Cast<AY01Monster>(Target->GetAvatarActor());
    const auto* Spawn = Spawns.Find(Monster);
    const auto* State = GetWorld()->GetGameState<AY01GameState>();
    if (!Player || Source != Player->GetCombatASC() || !Player->HasAuthority() || !Player->GetGrowth()->IsEnabled()
        || !Monster || Monster->GetCombatASC() != Target || !Spawn || Spawn->bKilled || !State
        || State->GetWaveSnapshot().Phase == EY01MatchPhase::Ended || !Source->IsLegalCombatTarget(Monster)) { return A; }
    A.Source = Source; A.Target = Target; A.Player = Player; A.SpawnId = Spawn->Id;
    A.SourceSession = Source->GetCombatSession(); A.TargetSession = Target->GetCombatSession(); return A;
}
FGuid UY01KillRewards::FreezeDeath(const FY01KillAuthorization& A, float OldHealth, float NewHealth)
{
    if (!IsInGameThread() || !bEnabled || bEnded || !A.Player.IsValid() || !A.Source.IsValid() || !A.Target.IsValid()
        || !FMath::IsFinite(OldHealth) || !FMath::IsFinite(NewHealth) || OldHealth <= 0 || NewHealth != 0
        || A.SourceSession != A.Source->GetCombatSession() || A.TargetSession != A.Target->GetCombatSession()
        || Sequence == MAX_uint64) { return {}; }
    auto* Monster = Cast<AY01Monster>(A.Target->GetAvatarActor());
    auto* Spawn = Spawns.Find(Monster);
    if (!Spawn || Spawn->bKilled || Spawn->Id != A.SpawnId) { return {}; }
    FY01KillEvent Event; Event.KillEventId = FGuid::NewGuid(); Event.MatchId = MatchId;
    Event.SpawnId = Spawn->Id; Event.Definition = Spawn->Definition; Event.Killer = A.Player;
    Event.Monster = Monster;   // Cast失败时Spawn为空已提前return，此处必为有效怪物对象。
    Event.SourceSession = A.SourceSession; Event.TargetSession = A.TargetSession; Event.ActualDamage = OldHealth - NewHealth;
    Event.Origin = A.Target->GetAvatarActor()->GetActorLocation();
    Event.DeathTime = GetWorld()->GetGameState<AY01GameState>()->GetServerWorldTimeSeconds();
    if (!A.Player->GetGrowth()->FreezeSources(Event)) { return {}; }
    Event.Sequence = ++Sequence; Spawn->bKilled = true;
    const FGuid Id = Event.KillEventId; Pending.Add({MoveTemp(Event), false}); return Id;
}
void UY01KillRewards::Complete(const FGuid& Id)
{
    if (!IsInGameThread() || !Id.IsValid()) { return; }
    if (auto* Found = Pending.FindByPredicate([&](const auto& P) { return P.Event.KillEventId == Id; })) { Found->bReady = true; }
    Drain();
}
bool UY01KillRewards::HasPending(const AY01PlayerState* Player) const
{
    return bDraining || Pending.ContainsByPredicate([&](const auto& P) { return P.Event.Killer.Get() == Player; });
}
void UY01KillRewards::Drain()
{
    if (!IsInGameThread() || bDraining) { return; }
    TGuardValue<bool> Guard(bDraining, true);
    while (!Pending.IsEmpty() && Pending[0].bReady)
    {
        auto* Player = Pending[0].Event.Killer.Get();
        if (Player && !Player->GetGrowth()->CanConsume()) { break; }
        auto Event = MoveTemp(Pending[0].Event); Pending.RemoveAt(0, 1, EAllowShrinking::No);
        if (Player) { Player->GetGrowth()->Consume(Event); }
        // 成长故障不改变合法击杀事实；下一子步的掉落仅订阅这里。
        OnEffectiveKill.Broadcast(Event);
    }
}
void UY01KillRewards::EndSession() { if (IsInGameThread()) { bEnded = true; Spawns.Reset(); Drain(); } }
void UY01KillRewards::Deinitialize()
{
    bEnded = true; bEnabled = false; Spawns.Reset(); Pending.Reset(); OnEffectiveKill.Clear(); Super::Deinitialize();
}
