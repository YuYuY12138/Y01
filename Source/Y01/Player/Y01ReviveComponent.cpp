#include "Player/Y01ReviveComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01DamageEffectsComponent.h"
#include "Match/Y01GameFlow.h"
#include "Combat/Y01Core.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "Misc/ScopeExit.h"

bool FY01ReviveSnapshot::NetSerialize(FArchive& Ar, UPackageMap*, bool& bOutSuccess)
{
    Ar << DeathSequence << bWaiting << bCancelled << WaveIndex << DeathTime << Deadline << ServerSampleTime;
    bOutSuccess = !Ar.IsError(); return true;
}
UY01ReviveComponent::UY01ReviveComponent()
{ PrimaryComponentTick.bCanEverTick = false; SetIsReplicatedByDefault(true); }
void UY01ReviveComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{ Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UY01ReviveComponent, Snapshot); }
AY01PlayerState* UY01ReviveComponent::Player() const { return Cast<AY01PlayerState>(GetOwner()); }
void UY01ReviveComponent::RecordDeath()
{
    auto* P = Player(); auto* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AY01GameMode>() : nullptr;
    if (!IsInGameThread() || !P || !P->HasAuthority() || !Mode || !Mode->IsCoreDefenseEnabled()
        || !P->IsCharacterInitialized() || BlocksActions() || bRecording
        || P->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) > 0) { return; }
    TGuardValue<bool> Guard(bRecording, true);
    DeathPawn = Cast<ACharacter>(P->GetCombatASC()->GetAvatarActor());
    if (!DeathPawn.IsValid()) { return; }
    // 在收入、清房、策略等任何可重入回调之前发布死亡身份。
    Snapshot.bWaiting = true; Snapshot.bCancelled = false;
    if (Snapshot.DeathSequence != MAX_uint64) { ++Snapshot.DeathSequence; }
    Snapshot.DeathTime = Snapshot.ServerSampleTime = GetWorld()->GetTimeSeconds();
    Context = {}; Context.DeathTime = Snapshot.DeathTime; Context.DeathTransform = DeathPawn->GetActorTransform();
    const bool HasRule = Mode->ResolveReviveRule(Context.WaveIndex, Context.BaseWaitSeconds);
    Snapshot.WaveIndex = Context.WaveIndex;
    Plan = {}; Plan.WaitSeconds = Context.BaseWaitSeconds;
    DeathPawn->GetCharacterMovement()->StopMovementImmediately(); DeathPawn->GetCharacterMovement()->DisableMovement();
    DeathPawn->ConsumeMovementInputVector();
    P->GetCombatASC()->UpdateHealthRegen(false);
    P->GetIncome()->Pause();
    Mode->OnPlayerDeath(P);
    // 练功房返回传送可能重新设置移动模式，死亡锁必须在返回后仍成立。
    if (DeathPawn.IsValid()) { DeathPawn->GetCharacterMovement()->DisableMovement(); }
    if (PlanPolicy) { PlanPolicy(Context, Plan); }
    Snapshot.Deadline = Snapshot.DeathTime + Plan.WaitSeconds;
    if (!HasRule || !FMath::IsFinite(Plan.WaitSeconds) || Plan.WaitSeconds < 0
        || !FMath::IsFinite(Snapshot.Deadline) || (Plan.WaitSeconds > 0 && Snapshot.Deadline <= Snapshot.DeathTime)
        || !FMath::IsFinite(Plan.HealthRatio) || Plan.HealthRatio < 0 || Plan.HealthRatio > 1
        || !FMath::IsFinite(Plan.HealthValue) || Plan.HealthValue < 0 || (Plan.HealthRatio == 0 && Plan.HealthValue == 0)
        || Mode->IsGameplayFrozen() || Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended)
    { Cancel(); }
    P->ForceNetUpdate();
}
void UY01ReviveComponent::Cancel()
{
    auto* P = Player(); if (!P || !P->HasAuthority()) { return; }
    Snapshot.bWaiting = false; Snapshot.bCancelled = true; Snapshot.ServerSampleTime = GetWorld()->GetTimeSeconds();
    P->ForceNetUpdate();
}
void UY01ReviveComponent::Update()
{
    auto* P = Player(); if (!P || !P->HasAuthority() || !Snapshot.bWaiting || bRecording || bExecuting) { return; }
    const auto* Mode = GetWorld()->GetAuthGameMode<AY01GameMode>();
    if (!Mode || Mode->IsGameplayFrozen() || Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended)
    { Cancel(); return; }
    Snapshot.ServerSampleTime = GetWorld()->GetTimeSeconds();
    if (Snapshot.ServerSampleTime >= Snapshot.Deadline) { TryExecute(); }
}
bool UY01ReviveComponent::FindSafeLocation(ACharacter* Pawn, FVector& OutLocation) const
{
    const auto* Mode = GetWorld()->GetAuthGameMode<AY01GameMode>();
    const auto* Core = Mode ? Mode->GetMatchCore() : nullptr;
    if (!IsValid(Core)) { return false; }
    const FVector CoreAnchor = Core->GetReviveAnchorTransform().GetLocation();
    const FVector Anchor = Plan.bAtDeathLocation ? Context.DeathTransform.GetLocation() : CoreAnchor;
    const auto* Capsule = Pawn->GetCapsuleComponent();
    const float Radius = Capsule->GetScaledCapsuleRadius(), Half = Capsule->GetScaledCapsuleHalfHeight();
    // 核心胶囊仅Overlap、外观无碰撞；占地必须显式包含二者，不包含悬空血条。
    FBox CoreBounds(ForceInit);
    TInlineComponentArray<UPrimitiveComponent*> Components;
    Core->GetComponents(Components);
    for (const auto* Component : Components)
    {
        if (Component->IsA<UCapsuleComponent>() || Component->IsA<UStaticMeshComponent>())
        { CoreBounds += Component->Bounds.GetBox(); }
    }
    if (!CoreBounds.IsValid) { return false; }
    constexpr double SafetyGap = 8.;
    const double CoreRadius = FVector::Dist2D(CoreAnchor, CoreBounds.GetCenter()) + CoreBounds.GetExtent().Size2D();
    const double FirstRing = Plan.bAtDeathLocation ? 150. : CoreRadius + Radius + SafetyGap;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(Y01ReviveGround), false, Pawn);
    FCollisionObjectQueryParams Ground(FCollisionObjectQueryParams::InitType::AllStaticObjects);
    for (int32 I = 0; I < (Plan.bAtDeathLocation ? 33 : 32); ++I)
    {
        const int32 RingIndex = Plan.bAtDeathLocation ? I - 1 : I;
        const double Angle = FMath::Max(0, RingIndex) % 8 * UE_PI / 4;
        const double Distance = RingIndex < 0 ? 0 : FirstRing + (RingIndex / 8) * 100;
        const FVector XY = Anchor + FVector(Distance * FMath::Cos(Angle), Distance * FMath::Sin(Angle), 0);
        FHitResult Hit;
        if (!GetWorld()->LineTraceSingleByObjectType(Hit, XY + FVector(0,0,500), XY - FVector(0,0,1500), Ground, Query)
            || !Hit.bBlockingHit || Hit.ImpactNormal.Z < Pawn->GetCharacterMovement()->GetWalkableFloorZ()) { continue; }
        const FVector Location = Hit.ImpactPoint + FVector(0,0,Half + 2);
        if (Location.Z + Half + SafetyGap >= CoreBounds.Min.Z && Location.Z - Half - SafetyGap <= CoreBounds.Max.Z
            && FVector::DistSquared2D(Location, CoreBounds.GetClosestPointTo(Location)) <= FMath::Square(Radius + SafetyGap)) { continue; }
        if (GetWorld()->OverlapBlockingTestByProfile(Location, FQuat::Identity, Capsule->GetCollisionProfileName(),
            FCollisionShape::MakeCapsule(Radius, Half), Query)) { continue; }
        OutLocation = Location; return true;
    }
    return false;
}
bool UY01ReviveComponent::TryExecute()
{
    TGuardValue<bool> Guard(bExecuting, true);
    auto* P = Player(); auto* Pawn = DeathPawn.Get(); auto* Mode = GetWorld()->GetAuthGameMode<AY01GameMode>();
    const auto Valid = [&]
    {
        return IsValid(P) && IsValid(Pawn) && IsValid(Mode) && Mode->GetFlowState()
            && !P->IsActorBeingDestroyed() && !Pawn->IsActorBeingDestroyed()
            && Snapshot.bWaiting && !Mode->IsGameplayFrozen() && Mode->GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Ended
            && Mode->GetFlowState()->PlayerArray.Contains(P) && P->GetCombatASC()->GetAvatarActor() == Pawn
            && Pawn->GetPlayerState() == P && Pawn->GetController() && Pawn->GetController()->GetPlayerState<AY01PlayerState>() == P
            && !P->GetLoadout()->IsBusy() && !P->GetSourceEffects()->IsBusy();
    };
    if (!Valid()) { return false; }
    bool bCommitted = false;
    ON_SCOPE_EXIT
    {
        if (!bCommitted && IsValid(P) && IsValid(Pawn))
        {
            P->GetIncome()->Pause();
            Pawn->GetCharacterMovement()->StopMovementImmediately(); Pawn->GetCharacterMovement()->DisableMovement();
            P->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0);
        }
    };
    FVector Location;
    if (!FindSafeLocation(Pawn, Location) || !Pawn->TeleportTo(Location, Context.DeathTransform.Rotator(), false, true) || !Valid()) { return false; }
    Pawn->GetCharacterMovement()->DisableMovement();
    if (!P->GetIncome()->Resume().IsOk() || !Valid()) { return false; }
    auto* ASC = P->GetCombatASC();
    const float MaxHealth = ASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute());
    const double Requested = double(MaxHealth) * Plan.HealthRatio + Plan.HealthValue;
    if (!FMath::IsFinite(MaxHealth) || MaxHealth <= 0 || !FMath::IsFinite(Requested) || Requested <= 0) { return false; }
    {
        TGuardValue<bool> Restoring(bRestoringHealth, true);
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), FMath::Min(double(MaxHealth), Requested));
    }
    if (!Valid() || ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) <= 0)
    { return false; }
    bCommitted = true; Snapshot.bWaiting = false; DeathPawn.Reset();
    ASC->UpdateHealthRegen(false);
    Pawn->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
    P->GetDamageEffects()->SynchronizeCommitted();
    P->GetLoadout()->PublishCommittedView(); P->ForceNetUpdate(); Pawn->ForceNetUpdate();
    return true;
}
