#include "Combat/Y01DamageEvents.h"
#include "Combat/Y01Combat.h"
#include "Player/Y01PlayerController.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

void UY01DamageEvents::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    WorldEpoch = FGuid::NewGuid();
}
void UY01DamageEvents::OnWorldEndPlay(UWorld& InWorld)
{
    WorldEpoch.Invalidate(); AudiencePolicy.Unbind(); OnDamageCommitted.Clear();
    for (auto It = InWorld.GetPlayerControllerIterator(); It; ++It)
    { if (auto* PC = Cast<AY01PlayerController>(It->Get())) { PC->ClearDamageForTravel(); } }
    Super::OnWorldEndPlay(InWorld);
}
void UY01DamageEvents::Deinitialize()
{
    WorldEpoch.Invalidate(); OnDamageCommitted.Clear(); AudiencePolicy.Unbind();
    Super::Deinitialize();
}
bool UY01DamageEvents::ResolveAudience(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target, FY01PendingDamage& Pending)
{
    const AActor* Avatar = Target ? Target->GetAvatarActor() : nullptr;
    if (!WorldEpoch.IsValid() || GetWorld()->GetNetMode() == NM_Client || !Source || !Target
        || Source->GetWorld() != GetWorld() || Target->GetWorld() != GetWorld()
        || !Source->IsCombatInitialized() || !Target->IsCombatInitialized()
        || !IsValid(Avatar) || !Avatar->HasAuthority() || Avatar->GetActorLocation().ContainsNaN()) { return false; }
    Pending.Event.WorldEpoch = WorldEpoch; Pending.Event.WorldPosition = Avatar->GetActorLocation();
    if (AudiencePolicy.IsBound() && !AudiencePolicy.Execute(Source, Target, Pending))
    { Pending.Event.RegionId = INDEX_NONE; Pending.Recipients.Reset(); }
    return true;
}
FY01PendingDamage UY01DamageEvents::Capture(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target)
{
    FY01PendingDamage Pending;
    if (ResolveAudience(Source, Target, Pending)) { Pending.Event.EventId = ++NextEventId; }
    return Pending;
}
void UY01DamageEvents::StageFinalDamage(const FY01PendingDamage& Pending, const UY01CombatAbilitySystem* Target)
{
    const auto* Pawn = Target ? Cast<APawn>(Target->GetAvatarActor()) : nullptr;
    auto* Owner = Pawn ? Cast<AY01PlayerController>(Pawn->GetController()) : nullptr;
    if (!Owner || !WorldEpoch.IsValid() || Pending.Event.WorldEpoch != WorldEpoch) { return; }
    for (const auto& Recipient : Pending.Recipients)
    { if (Recipient.Controller == Owner) { Owner->StageFinalDamage(Recipient.Audience, Pending.Event); } }
}
void UY01DamageEvents::Commit(const FY01PendingDamage& Pending)
{
    const auto& E = Pending.Event;
    if (!WorldEpoch.IsValid() || E.WorldEpoch != WorldEpoch || GetWorld()->GetNetMode() == NM_Client
        || E.EventId == 0 || !E.HasValidAmount() || E.WorldPosition.ContainsNaN()) { return; }
    if (FParse::Param(FCommandLine::Get(), TEXT("Y01DamageTrace")))
    {
        const TCHAR* Action = E.Type == EY01DamageEventType::Damage ? TEXT("COMMIT") : TEXT("HEAL");
        UE_LOG(LogTemp, Display, TEXT("Y01Damage %s Id=%llu Epoch=%s Region=%d Actual=%.9g Display=%.9g Type=%d X=%.17g Y=%.17g Z=%.17g"),
            Action, E.EventId, *E.WorldEpoch.ToString(EGuidFormats::Digits), E.RegionId, E.ActualDamage, E.DisplayAmount,
            static_cast<int32>(E.Type), E.WorldPosition.X, E.WorldPosition.Y, E.WorldPosition.Z);
    }
    for (const auto& Recipient : Pending.Recipients)
    {
        if (auto* PC = Recipient.Controller.Get(); PC && PC->GetWorld() == GetWorld())
        { PC->QueueDamageEvent(Recipient.Audience, E); }
    }
    OnDamageCommitted.Broadcast(E);
}
