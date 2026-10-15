#include "Combat/Y01Monster.h"
#include "Combat/Y01Targeting.h"
#include "Combat/Y01Core.h"
#include "Data/Y01DataSubsystem.h"
#include "Engine/GameInstance.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

namespace
{
uint64 GY01MovementRegistrationOrder = 0;
constexpr int32 Y01MaxSeparationNeighbors = 8;
constexpr double Y01SeparationInterval = .1;
constexpr float Y01SeparationSpeedFraction = .2f;
}

bool FY01MonsterPresentationState::IsRegistered() const
{
    return bRegistered && WorldEpoch.IsValid() && Revision > 0
        && (Kind == EY01MonsterPresentationKind::Normal || Kind == EY01MonsterPresentationKind::Boss)
        && ((RegionId == 0 && RegionSession == 0) || (RegionId > 0 && RegionSession > 0));
}
bool FY01MonsterPresentationState::NetSerialize(FArchive& Ar, UPackageMap*, bool& bOutSuccess)
{
    auto Value = *this;
    uint8 Registered = Value.bRegistered, Type = static_cast<uint8>(Value.Kind), Damaged = Value.bEverActuallyDamaged, Dead = Value.bDead;
    Ar << Registered << Type << Value.WorldEpoch << Value.RegionId << Value.RegionSession << Damaged << Dead << Value.Revision;
    Value.bRegistered = Registered != 0; Value.Kind = static_cast<EY01MonsterPresentationKind>(Type);
    Value.bEverActuallyDamaged = Damaged != 0; Value.bDead = Dead != 0;
    bOutSuccess = !Ar.IsError() && Registered <= 1 && Damaged <= 1 && Dead <= 1 && Type <= static_cast<uint8>(EY01MonsterPresentationKind::Boss)
        && (Value.bRegistered ? Value.IsRegistered() : Value.Kind == EY01MonsterPresentationKind::Unknown && Value.RegionId == INDEX_NONE && Value.RegionSession == 0);
    if (Ar.IsLoading() && bOutSuccess) { *this = Value; }
    return true;
}
void AY01Monster::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION_NOTIFY(AY01Monster, PresentationState, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME(AY01Monster, DetectionRadius);
}
void AY01Monster::EnsurePresentationWorld()
{
    if (PresentationWorld.Get() == GetWorld()) { return; }
    if (AcceptedPresentationState.WorldEpoch.IsValid()) { RetiredPresentationEpochs.Add(AcceptedPresentationState.WorldEpoch); }
    HighestPresentationRevision = FMath::Max(HighestPresentationRevision, PresentationState.Revision);
    PresentationWorld = GetWorld(); AcceptedPresentationState = {}; PresentationState = {};
    PresentationState.Revision = HighestPresentationRevision;
    OnHealthBarPresentationChanged.Broadcast();
}
FY01MonsterPresentationState AY01Monster::GetHealthBarPresentationState() const
{
    if (bPresentationEnded || IsActorBeingDestroyed() || !GetWorld() || GetWorld()->bIsTearingDown || PresentationWorld.Get() != GetWorld()) { return {}; }
    return HasAuthority() ? PresentationState : AcceptedPresentationState;
}
void AY01Monster::PublishPresentation()
{
    ++PresentationState.Revision; HighestPresentationRevision = PresentationState.Revision;
    AcceptedPresentationState = PresentationState; ForceNetUpdate(); OnHealthBarPresentationChanged.Broadcast();
    RefreshMovementPeersFromWorld();
}
void AY01Monster::OnRep_PresentationState()
{
    const auto Incoming = PresentationState;
    // 新World第一次接收也不能把本包误作旧世界水位。
    PresentationState = AcceptedPresentationState; EnsurePresentationWorld();
    if (HasAuthority() || bPresentationEnded || !GetWorld() || GetWorld()->bIsTearingDown
        || !Incoming.WorldEpoch.IsValid() || RetiredPresentationEpochs.Contains(Incoming.WorldEpoch)
        || Incoming.Revision <= HighestPresentationRevision
        || (AcceptedPresentationState.WorldEpoch.IsValid() && Incoming.WorldEpoch != AcceptedPresentationState.WorldEpoch)) { return; }
    if (Incoming.bRegistered ? !Incoming.IsRegistered() : Incoming.Kind != EY01MonsterPresentationKind::Unknown || Incoming.RegionId != INDEX_NONE || Incoming.RegionSession != 0) { return; }
    PresentationState = Incoming; AcceptedPresentationState = Incoming; HighestPresentationRevision = Incoming.Revision;
    OnHealthBarPresentationChanged.Broadcast(); RefreshMovementPeersFromWorld();
}
bool AY01Monster::RegisterHealthBarPresentation(int32 RegionId, int32 RegionSession)
{
    if (!HasAuthority() || bPresentationEnded || IsActorBeingDestroyed() || !GetWorld() || GetWorld()->bIsTearingDown
        || !AbilitySystem->IsCombatInitialized() || AbilitySystem->GetDefinition().Type != EY01TableKind::Monster
        || AbilitySystem->GetDefinition().RowName.IsNone()
        || !((RegionId == 0 && RegionSession == 0) || (RegionId > 0 && RegionSession > 0))) { return false; }
    EnsurePresentationWorld();
    const auto* Service = GetWorld()->GetSubsystem<UY01DamageEvents>();
    if (!Service || !Service->GetWorldEpoch().IsValid() || RetiredPresentationEpochs.Contains(Service->GetWorldEpoch())
        || PresentationState.bDead || AbilitySystem->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) <= 0) { return false; }
    const auto Kind = AbilitySystem->GetDefinition().RowName == TEXT("Boss") ? EY01MonsterPresentationKind::Boss : EY01MonsterPresentationKind::Normal;
    if (PresentationState.IsRegistered() && PresentationState.WorldEpoch == Service->GetWorldEpoch()
        && PresentationState.RegionId == RegionId && PresentationState.RegionSession == RegionSession && PresentationState.Kind == Kind) { return true; }
    PresentationState.bRegistered = true; PresentationState.Kind = Kind; PresentationState.WorldEpoch = Service->GetWorldEpoch();
    PresentationState.RegionId = RegionId; PresentationState.RegionSession = RegionSession;
    if (Kind == EY01MonsterPresentationKind::Normal && MovementRegistrationOrder == 0) { MovementRegistrationOrder = ++GY01MovementRegistrationOrder; }
    NextSeparationRefreshTime = 0; PublishPresentation(); return true;
}
void AY01Monster::UnregisterHealthBarPresentation()
{
    if (!HasAuthority()) { return; }
    EnsurePresentationWorld();
    if (!PresentationState.bRegistered) { return; }
    PresentationState.bRegistered = false; PresentationState.Kind = EY01MonsterPresentationKind::Unknown;
    PresentationState.RegionId = INDEX_NONE; PresentationState.RegionSession = 0;
    SeparationCandidates.Reset(); NextSeparationRefreshTime = 0; PublishPresentation();
}
void AY01Monster::RecordHealthBarHealthChange(bool bActualDamage, bool bDied)
{
    if (!HasAuthority() || bPresentationEnded || IsActorBeingDestroyed() || !GetWorld() || GetWorld()->bIsTearingDown) { return; }
    EnsurePresentationWorld();
    const auto* Service = GetWorld()->GetSubsystem<UY01DamageEvents>();
    if (!Service || !Service->GetWorldEpoch().IsValid() || PresentationState.bDead) { return; }
    if ((!bActualDamage || PresentationState.bEverActuallyDamaged) && !bDied) { return; }
    PresentationState.WorldEpoch = Service->GetWorldEpoch();
    PresentationState.bEverActuallyDamaged |= bActualDamage; PresentationState.bDead |= bDied;
    PublishPresentation();
}
void AY01Monster::EndPlay(EEndPlayReason::Type Reason)
{
    ClearMovementPeers(); bPresentationEnded = true; UnregisterHealthBarPresentation();
    OnHealthBarPresentationChanged.Broadcast(); OnHealthBarPresentationChanged.Clear(); Super::EndPlay(Reason);
}

AY01Monster::AY01Monster()
{
    AbilitySystem = CreateDefaultSubobject<UY01CombatAbilitySystem>(TEXT("AbilitySystem"));
    Attributes = CreateDefaultSubobject<UY01AttributeSet>(TEXT("Attributes"));
    AbilitySystem->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);
    bReplicates = true;
    AutoPossessAI = EAutoPossessAI::Disabled;
    GetCapsuleComponent()->InitCapsuleSize(34.f, 88.f);
}
bool AY01Monster::TryInitializeMonster(const FY01DataCatalog& Catalog, const FY01DataRef& Monster, FString& OutError)
{
    OutError.Reset();
    if (!HasAuthority() || Monster.Type != EY01TableKind::Monster)
    { OutError = TEXT("怪物初始化需要服务器及Monster引用"); return false; }
    FY01DataValidation Validation;
    const auto* Row = Catalog.FindRow<FY01MonsterRow>(Monster, Validation);
    if (!Row || !FMath::IsFinite(Row->DetectionRadius) || Row->DetectionRadius < 0)
    { OutError = FString::Printf(TEXT("Monster=%s DetectionRadius必须有限非负"), *Monster.RowName.ToString()); return false; }
    const bool bWasInitialized = AbilitySystem->IsCombatInitialized();
    AbilitySystem->InitAbilityActorInfo(this, this);
    if (!AbilitySystem->TryInitializeFromData(Catalog, Monster, OutError)) { return false; }
    if (!bWasInitialized) { DetectionRadius = Row->DetectionRadius; NextDetectionTime = 0; }
    GetCharacterMovement()->MaxWalkSpeed = AbilitySystem->GetInitialMoveSpeed();
    return true;
}
bool AY01Monster::IsMovementPeerEligible() const
{
    if (!IsValid(this) || bPresentationEnded || IsActorBeingDestroyed() || !GetWorld() || GetWorld()->bIsTearingDown) { return false; }
    const auto& State = HasAuthority() ? PresentationState : AcceptedPresentationState;
    return State.IsRegistered() && State.Kind == EY01MonsterPresentationKind::Normal && !State.bDead;
}
bool AY01Monster::SharesMovementDomain(const AY01Monster* Other) const
{
    if (!Other || Other == this || Other->GetWorld() != GetWorld() || !IsMovementPeerEligible() || !Other->IsMovementPeerEligible()) { return false; }
    const auto& State = HasAuthority() ? PresentationState : AcceptedPresentationState;
    const auto& OtherState = Other->HasAuthority() ? Other->PresentationState : Other->AcceptedPresentationState;
    return State.WorldEpoch == OtherState.WorldEpoch && State.RegionId == OtherState.RegionId
        && State.RegionSession == OtherState.RegionSession;
}
void AY01Monster::SetMovementPeer(AY01Monster* Other, bool bIgnore)
{
    if (!Other || Other == this) { return; }
    if (bIgnore && !SharesMovementDomain(Other)) { bIgnore = false; }
    GetCapsuleComponent()->IgnoreActorWhenMoving(Other, bIgnore);
    Other->GetCapsuleComponent()->IgnoreActorWhenMoving(this, bIgnore);
    if (bIgnore)
    {
        MovementPeers.Add(Other); Other->MovementPeers.Add(this);
    }
    else
    {
        MovementPeers.Remove(Other); Other->MovementPeers.Remove(this);
    }
}
void AY01Monster::ClearMovementPeers()
{
    const auto Peers = MovementPeers.Array();
    for (const auto& WeakPeer : Peers)
    { if (AY01Monster* Peer = WeakPeer.Get()) { SetMovementPeer(Peer, false); } }
    MovementPeers.Empty();
}
void AY01Monster::RefreshMovementPeers(TConstArrayView<AY01Monster*> Candidates)
{
    TSet<TWeakObjectPtr<AY01Monster>> Desired;
    if (IsMovementPeerEligible())
    {
        for (AY01Monster* Candidate : Candidates)
        { if (SharesMovementDomain(Candidate)) { Desired.Add(Candidate); SetMovementPeer(Candidate, true); } }
    }
    const auto Existing = MovementPeers.Array();
    for (const auto& WeakPeer : Existing)
    {
        AY01Monster* Peer = WeakPeer.Get();
        if (!Peer || !Desired.Contains(Peer)) { if (Peer) { SetMovementPeer(Peer, false); } else { MovementPeers.Remove(WeakPeer); } }
    }
}
void AY01Monster::RefreshMovementPeersFromWorld()
{
    if (!GetWorld()) { ClearMovementPeers(); return; }
    TArray<AY01Monster*> Candidates;
    for (TActorIterator<AY01Monster> It(GetWorld()); It; ++It) { Candidates.Add(*It); }
    RefreshMovementPeers(Candidates);
}
FVector AY01Monster::ResolveMovementVelocity(TConstArrayView<AY01Monster*> Candidates, const FVector& DesiredVelocity)
{
    if (!HasAuthority() || !IsMovementPeerEligible()) { return DesiredVelocity; }
    struct FNeighbor { AY01Monster* Monster; double DistanceSquared; };
    TArray<FNeighbor> Neighbors;
    const FVector Location = GetActorLocation();
    const float OwnRadius = GetCapsuleComponent()->GetScaledCapsuleRadius();
    for (AY01Monster* Candidate : Candidates)
    {
        if (!SharesMovementDomain(Candidate)) { continue; }
        const double DistanceSquared = FVector::DistSquared2D(Location, Candidate->GetActorLocation());
        const float SeparationRadius = OwnRadius + Candidate->GetCapsuleComponent()->GetScaledCapsuleRadius();
        if (DistanceSquared <= FMath::Square(SeparationRadius)) { Neighbors.Add({Candidate, DistanceSquared}); }
    }
    Neighbors.Sort([](const FNeighbor& A, const FNeighbor& B)
    {
        if (!FMath::IsNearlyEqual(A.DistanceSquared, B.DistanceSquared)) { return A.DistanceSquared < B.DistanceSquared; }
        return A.Monster->MovementRegistrationOrder < B.Monster->MovementRegistrationOrder;
    });
    if (Neighbors.Num() > Y01MaxSeparationNeighbors) { Neighbors.SetNum(Y01MaxSeparationNeighbors); }
    FVector Separation = FVector::ZeroVector;
    for (const FNeighbor& Neighbor : Neighbors)
    {
        const float SeparationRadius = OwnRadius + Neighbor.Monster->GetCapsuleComponent()->GetScaledCapsuleRadius();
        FVector Away = Location - Neighbor.Monster->GetActorLocation(); Away.Z = 0;
        const float Distance = Away.Size2D();
        if (Distance <= KINDA_SMALL_NUMBER)
        {
            const bool bBefore = MovementRegistrationOrder != 0 && Neighbor.Monster->MovementRegistrationOrder != 0
                ? MovementRegistrationOrder < Neighbor.Monster->MovementRegistrationOrder
                : GetFName().LexicalLess(Neighbor.Monster->GetFName());
            Away = FVector(bBefore ? 1.f : -1.f, 0, 0);
        }
        else { Away /= Distance; }
        Separation += Away * (1.f - FMath::Clamp(Distance / SeparationRadius, 0.f, 1.f));
    }
    if (Separation.IsNearlyZero()) { return DesiredVelocity; }
    const float MaxSpeed = GetCharacterMovement()->MaxWalkSpeed;
    const float MaxBias = MaxSpeed * Y01SeparationSpeedFraction;
    const FVector Bias = (Separation * MaxBias).GetClampedToMaxSize2D(MaxBias);
    return (DesiredVelocity + Bias).GetClampedToMaxSize(MaxSpeed);
}
void AY01Monster::RefreshLocalSeparationCandidates()
{
    if (!HasAuthority() || !GetWorld() || !IsMovementPeerEligible())
    { SeparationCandidates.Reset(); return; }
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now < NextSeparationRefreshTime) { return; }
    NextSeparationRefreshTime = Now + Y01SeparationInterval;
    TArray<FOverlapResult> Overlaps;
    FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(Y01MonsterSeparation), false, this);
    const float SearchRadius = GetCapsuleComponent()->GetScaledCapsuleRadius() * 2.f;
    GetWorld()->OverlapMultiByObjectType(Overlaps, GetActorLocation(), FQuat::Identity,
        FCollisionObjectQueryParams(ECC_Pawn), FCollisionShape::MakeSphere(SearchRadius), QueryParams);
    struct FCandidate { TWeakObjectPtr<AY01Monster> Monster; double DistanceSquared; };
    TArray<FCandidate> Nearby;
    for (const FOverlapResult& Overlap : Overlaps)
    {
        AY01Monster* Candidate = Cast<AY01Monster>(Overlap.GetActor());
        if (SharesMovementDomain(Candidate))
        { Nearby.Add({Candidate, FVector::DistSquared2D(GetActorLocation(), Candidate->GetActorLocation())}); }
    }
    Nearby.Sort([](const FCandidate& A, const FCandidate& B)
    {
        if (!FMath::IsNearlyEqual(A.DistanceSquared, B.DistanceSquared)) { return A.DistanceSquared < B.DistanceSquared; }
        return A.Monster->MovementRegistrationOrder < B.Monster->MovementRegistrationOrder;
    });
    if (Nearby.Num() > Y01MaxSeparationNeighbors) { Nearby.SetNum(Y01MaxSeparationNeighbors); }
    SeparationCandidates.Reset(Nearby.Num());
    for (const FCandidate& Candidate : Nearby) { SeparationCandidates.Add(Candidate.Monster); }
}
FVector AY01Monster::ResolveMovementVelocity(const FVector& DesiredVelocity)
{
    RefreshLocalSeparationCandidates();
    TArray<AY01Monster*> Candidates;
    for (const auto& WeakCandidate : SeparationCandidates)
    { if (AY01Monster* Candidate = WeakCandidate.Get()) { Candidates.Add(Candidate); } }
    return ResolveMovementVelocity(Candidates, DesiredVelocity);
}
AActor* AY01Monster::RefreshDetectionTarget(TConstArrayView<AActor*> Candidates, AY01Core* CoreFallback)
{
    if (!HasAuthority() || !GetWorld()) { return nullptr; }
    if (CoreFallback)
    {
        // 只绕过核心的发现半径；玩家仍必须在发现范围内，双方权限和存活检查仍由ASC裁定。
        AActor* Target = Y01Targeting::NearestXY(AbilitySystem, Candidates, DetectionRadius);
        if (!Target && AbilitySystem->IsLegalCombatTarget(CoreFallback)) { Target = CoreFallback; }
        AbilitySystem->SetDiscoveredTarget(Target);
        if (!Target) { GetCharacterMovement()->StopMovementImmediately(); }
        return AbilitySystem->GetCombatTarget();
    }
    AActor* Current = AbilitySystem->GetCombatTarget();
    const bool bLost = Current && (!Candidates.Contains(Current) || !Y01Targeting::InRadiusXY(AbilitySystem, Current, DetectionRadius));
    if (bLost || DetectionRadius <= 0 || IsActorBeingDestroyed() || !AbilitySystem->IsCombatInitialized()
        || AbilitySystem->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) <= 0)
    {
        AbilitySystem->SetDiscoveredTarget(nullptr); GetCharacterMovement()->StopMovementImmediately();
        if (bLost) { NextDetectionTime = 0; }
        if (DetectionRadius <= 0 || IsActorBeingDestroyed() || !AbilitySystem->IsCombatInitialized()
            || AbilitySystem->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) <= 0) { return nullptr; }
    }
    const double Now = GetWorld()->GetTimeSeconds();
    if (Now >= NextDetectionTime)
    {
        NextDetectionTime = Now + .1;
        AbilitySystem->SetDiscoveredTarget(Y01Targeting::NearestXY(AbilitySystem, Candidates, DetectionRadius));
    }
    Current = AbilitySystem->GetCombatTarget();
    if (!Current) { GetCharacterMovement()->StopMovementImmediately(); }
    return Current;
}
void AY01Monster::BeginPlay()
{
    Super::BeginPlay();
    EnsurePresentationWorld();
    AbilitySystem->InitAbilityActorInfo(this, this);
    if (HasAuthority() && !MonsterDefinition.RowName.IsNone())
    {
        const auto* Data = GetGameInstance() ? GetGameInstance()->GetSubsystem<UY01DataSubsystem>() : nullptr;
        if (Data && Data->GetCatalog())
        {
            FString Error;
            if (!TryInitializeMonster(*Data->GetCatalog(), MonsterDefinition, Error))
            { UE_LOG(LogTemp, Warning, TEXT("怪物初始化失败：%s"), *Error); }
        }
    }
}
