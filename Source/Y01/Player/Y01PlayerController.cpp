#include "Player/Y01PlayerController.h"
#include "Presentation/Y01DamageNumberPoolComponent.h"
#include "Presentation/Y01MonsterHealthBarPoolComponent.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Engine/NetConnection.h"
#include "Player/Y01Character.h"
#include "Player/Y01PlayerState.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Combat/Y01Monster.h"
#include "Net/UnrealNetwork.h"

bool FY01HealthBarAuthorization::IsValidScope() const
{
    return bEnabled && WorldEpoch.IsValid() && Generation > 0
        && ((RegionId == 0 && RegionSession == 0) || (RegionId > 0 && RegionSession > 0));
}
bool FY01HealthBarAuthorization::Matches(const FY01MonsterPresentationState& State) const
{
    return IsValidScope() && State.IsRegistered() && WorldEpoch == State.WorldEpoch
        && RegionId == State.RegionId && RegionSession == State.RegionSession;
}
bool FY01HealthBarAuthorization::NetSerialize(FArchive& Ar, UPackageMap*, bool& bOutSuccess)
{
    auto Value = *this; uint8 Enabled = Value.bEnabled;
    Ar << Enabled << Value.WorldEpoch << Value.RegionId << Value.RegionSession << Value.Generation;
    Value.bEnabled = Enabled != 0;
    bOutSuccess = !Ar.IsError() && Enabled <= 1
        && (Value.bEnabled ? Value.IsValidScope() : Value.RegionId == INDEX_NONE && Value.RegionSession == 0);
    if (Ar.IsLoading() && bOutSuccess) { *this = Value; }
    return true;
}
void AY01PlayerController::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION_NOTIFY(AY01PlayerController, HealthBarAuthorization, COND_OwnerOnly, REPNOTIFY_Always);
}
void AY01PlayerController::EnsureHealthBarWorld()
{
    if (HealthBarWorld.Get() == GetWorld()) { return; }
    ReleaseHealthBarPool();
    if (AcceptedHealthBarAuthorization.WorldEpoch.IsValid()) { RetiredHealthBarEpochs.Add(AcceptedHealthBarAuthorization.WorldEpoch); }
    HighestHealthBarGeneration = FMath::Max(HighestHealthBarGeneration, HealthBarAuthorization.Generation);
    HealthBarWorld = GetWorld(); bHealthBarTravelling = false;
    HealthBarAuthorization = {}; HealthBarAuthorization.Generation = HighestHealthBarGeneration;
    AcceptedHealthBarAuthorization = {}; OnHealthBarAuthorizationChanged.Broadcast();
}
FY01HealthBarAuthorization AY01PlayerController::GetHealthBarAuthorization() const
{
    if (!bHealthBarsEnabled || bHealthBarEnded || bHealthBarTravelling || IsActorBeingDestroyed()
        || !GetWorld() || GetWorld()->bIsTearingDown || HealthBarWorld.Get() != GetWorld()) { return {}; }
    return HasAuthority() ? HealthBarAuthorization : AcceptedHealthBarAuthorization;
}
void AY01PlayerController::PublishHealthBarAuthorization()
{
    ++HealthBarAuthorization.Generation; HighestHealthBarGeneration = HealthBarAuthorization.Generation;
    AcceptedHealthBarAuthorization = HealthBarAuthorization; ForceNetUpdate(); OnHealthBarAuthorizationChanged.Broadcast();
}
void AY01PlayerController::SetHealthBarRegion(int32 RegionId, int32 RegionSession)
{
    if (!HasAuthority() || !bHealthBarsEnabled || bHealthBarEnded || IsActorBeingDestroyed() || !GetWorld() || GetWorld()->bIsTearingDown) { return; }
    EnsureHealthBarWorld();
    if (bHealthBarTravelling || !((RegionId == 0 && RegionSession == 0) || (RegionId > 0 && RegionSession > 0))) { return; }
    const auto* Service = GetWorld()->GetSubsystem<UY01DamageEvents>();
    if (!Service || !Service->GetWorldEpoch().IsValid() || RetiredHealthBarEpochs.Contains(Service->GetWorldEpoch())) { return; }
    if (HealthBarAuthorization.bEnabled && HealthBarAuthorization.WorldEpoch == Service->GetWorldEpoch()
        && HealthBarAuthorization.RegionId == RegionId && HealthBarAuthorization.RegionSession == RegionSession) { return; }
    HealthBarAuthorization.bEnabled = true; HealthBarAuthorization.WorldEpoch = Service->GetWorldEpoch();
    HealthBarAuthorization.RegionId = RegionId; HealthBarAuthorization.RegionSession = RegionSession; PublishHealthBarAuthorization();
}
void AY01PlayerController::RevokeHealthBarAuthorization()
{
    if (!HasAuthority()) { return; }
    EnsureHealthBarWorld();
    if (!HealthBarAuthorization.bEnabled) { return; }
    HealthBarAuthorization.bEnabled = false; HealthBarAuthorization.RegionId = INDEX_NONE;
    HealthBarAuthorization.RegionSession = 0; PublishHealthBarAuthorization();
}
void AY01PlayerController::ClearHealthBarsForTravel()
{
    ReleaseHealthBarPool();
    if (bHealthBarTravelling) { return; }
    RevokeHealthBarAuthorization();
    if (AcceptedHealthBarAuthorization.WorldEpoch.IsValid()) { RetiredHealthBarEpochs.Add(AcceptedHealthBarAuthorization.WorldEpoch); }
    HealthBarWorld = GetWorld(); bHealthBarTravelling = true;
    HealthBarAuthorization.bEnabled = false; HealthBarAuthorization.RegionId = INDEX_NONE; HealthBarAuthorization.RegionSession = 0;
    AcceptedHealthBarAuthorization = {}; OnHealthBarAuthorizationChanged.Broadcast();
}
void AY01PlayerController::OnRep_HealthBarAuthorization()
{
    const auto Incoming = HealthBarAuthorization;
    HealthBarAuthorization = AcceptedHealthBarAuthorization; EnsureHealthBarWorld();
    if (HasAuthority() || !bHealthBarsEnabled || bHealthBarEnded || bHealthBarTravelling || !GetWorld() || GetWorld()->bIsTearingDown
        || !Incoming.WorldEpoch.IsValid() || RetiredHealthBarEpochs.Contains(Incoming.WorldEpoch)
        || Incoming.Generation <= HighestHealthBarGeneration
        || (AcceptedHealthBarAuthorization.WorldEpoch.IsValid() && Incoming.WorldEpoch != AcceptedHealthBarAuthorization.WorldEpoch)) { return; }
    if (Incoming.bEnabled ? !Incoming.IsValidScope() : Incoming.RegionId != INDEX_NONE || Incoming.RegionSession != 0) { return; }
    HealthBarAuthorization = Incoming; AcceptedHealthBarAuthorization = Incoming; HighestHealthBarGeneration = Incoming.Generation;
    OnHealthBarAuthorizationChanged.Broadcast();
}

namespace
{
constexpr int32 DamageCapacity = 128;
constexpr int32 DamageChunkCapacity = 8;
// UE5.7 Generic实测每事件61字节；按64预算，另留128覆盖Audience(40)、数组和RPC/内容头。
constexpr int32 DamageEventBytesBudget = 64;
constexpr int32 DamageRPCBytesBudget = 128;
static_assert(DamageRPCBytesBudget + DamageEventBytesBudget * DamageChunkCapacity < 768);
constexpr uint64 DamageHistory = 512;
bool IsDamageValid(const FY01DamageEvent& E, const FY01DamageAudience& Scope)
{
    return Scope.WorldEpoch.IsValid() && Scope.Generation > 0 && Scope.RegionId >= 0
        && E.WorldEpoch == Scope.WorldEpoch && (E.RegionId == Scope.RegionId || Scope.IsFinalEvent(E)) && E.EventId > 0
        && E.HasValidAmount() && !E.WorldPosition.ContainsNaN();
}
}
void AY01PlayerController::EnsureHealthBarPool()
{
#if !UE_SERVER
    if (!bHealthBarsEnabled || bHealthBarEnded || bHealthBarTravelling || !IsLocalController() || !GetLocalPlayer()
        || !GetWorld() || GetWorld()->bIsTearingDown || GetNetMode() == NM_DedicatedServer)
    { ReleaseHealthBarPool(); return; }
    if (IsValid(HealthBarPool) && !HealthBarPool->IsBeingDestroyed() && HealthBarPool->IsReadyFor(GetWorld())) { return; }
    ReleaseHealthBarPool();
    if (!GetHealthBarAuthorization().IsValidScope() || !GetDefault<UY01MonsterHealthBarSettings>()->bEnabled) { return; }
    HealthBarPool = NewObject<UY01MonsterHealthBarPoolComponent>(this, NAME_None, RF_Transient);
    AddInstanceComponent(HealthBarPool); HealthBarPool->RegisterComponent();
    if (!HealthBarPool->InitializePool()) { ReleaseHealthBarPool(); }
#endif
}
void AY01PlayerController::ReleaseHealthBarPool()
{
    if (IsValid(HealthBarPool)) { HealthBarPool->Shutdown(); HealthBarPool->DestroyComponent(); }
    HealthBarPool = nullptr;
}
void AY01PlayerController::EnsureDamageNumberPool()
{
#if !UE_SERVER
    if (!bDamageEventsEnabled || bDamageEnded || !IsLocalController() || !GetLocalPlayer()
        || !GetWorld() || GetWorld()->bIsTearingDown || GetNetMode() == NM_DedicatedServer || IsDamageTravelBlocked()) { return; }
    const auto* Service = GetWorld()->GetSubsystem<UY01DamageEvents>();
    if (!Service || !Service->GetWorldEpoch().IsValid() || Service->GetWorldEpoch() == RetiredDamageEpoch) { return; }
    if (IsValid(DamageNumberPool) && !DamageNumberPool->IsBeingDestroyed() && DamageNumberPool->IsReadyFor(GetWorld())) { return; }
    ReleaseDamageNumberPool();
    DamageNumberPool = NewObject<UY01DamageNumberPoolComponent>(this, NAME_None, RF_Transient);
    AddInstanceComponent(DamageNumberPool); DamageNumberPool->RegisterComponent();
    if (!DamageNumberPool->InitializePool()) { ReleaseDamageNumberPool(); }
#endif
}
void AY01PlayerController::ReleaseDamageNumberPool()
{
    if (IsValid(DamageNumberPool)) { DamageNumberPool->Shutdown(); DamageNumberPool->DestroyComponent(); }
    DamageNumberPool = nullptr;
}
void AY01PlayerController::ClearDamageForTravel()
{
    ClearHealthBarsForTravel();
    if (bDamageTravelling) { return; }
    bDamageTravelling = true; DamageTravelWorld = GetWorld();
    RetiredDamageEpoch = HasAuthority() ? ServerDamageAudience.WorldEpoch : ClientDamageAudience.WorldEpoch;
    PendingDamageEvents.Reset(); ServerDamageAudience.WorldEpoch.Invalidate(); ClientDamageAudience.WorldEpoch.Invalidate();
    FinalDamageEvent = {}; FinalDamageAudience = {}; bFinalDamageCommitted = false;
    ReleaseDamageNumberPool(); ResetDamageDisplay();
}
bool AY01PlayerController::IsDamageTravelBlocked()
{
    if (bDamageTravelling && DamageTravelWorld.Get() != GetWorld())
    { bDamageTravelling = false; NextDamageBatchAt = 0; }
    return bDamageTravelling;
}
void AY01PlayerController::PreClientTravel(const FString& PendingURL, ETravelType TravelType, bool bIsSeamlessTravel)
{
    ClearDamageForTravel(); Super::PreClientTravel(PendingURL, TravelType, bIsSeamlessTravel);
}
void AY01PlayerController::PostSeamlessTravel()
{
    // 服务器保留的Controller可能已不在旧World列表；在引擎收尾点清旧状态并允许新World配置。
    const bool bHealthBarWorldChanged = HealthBarWorld.Get() != GetWorld();
    ClearDamageForTravel();
    // 无PreClientTravel时，撤权会绑定到新World；收尾只解除新World的旅行拦截。
    if (bHealthBarWorldChanged) { EnsureHealthBarWorld(); bHealthBarTravelling = false; }
    bDamageTravelling = false; DamageTravelWorld.Reset(); NextDamageBatchAt = 0;
    Super::PostSeamlessTravel();
}
void AY01PlayerController::StageFinalDamage(const FY01DamageAudience& Audience, const FY01DamageEvent& Event)
{
    if (!HasAuthority() || !bDamageEventsEnabled || bDamageEnded || IsDamageTravelBlocked()
        || !(Audience == ServerDamageAudience) || Audience.RegionId <= 0 || !IsDamageValid(Event, Audience)) { return; }
    FinalDamageAudience = Audience; FinalDamageEvent = Event; bFinalDamageCommitted = false;
}
void AY01PlayerController::SetDamageRegion(int32 RegionId, bool bPreserveFinalDamage)
{
    if (!HasAuthority() || !bDamageEventsEnabled || bDamageEnded || IsDamageTravelBlocked()) { return; }
    const auto* Service = GetWorld()->GetSubsystem<UY01DamageEvents>();
    if (!Service || !Service->GetWorldEpoch().IsValid() || Service->GetWorldEpoch() == RetiredDamageEpoch) { return; }
    if (ServerDamageAudience.WorldEpoch == Service->GetWorldEpoch() && ServerDamageAudience.RegionId == RegionId) { return; }
    const bool KeepFinal = bPreserveFinalDamage && RegionId == 0 && FinalDamageEvent.EventId != 0
        && FinalDamageAudience == ServerDamageAudience && FinalDamageEvent.WorldEpoch == Service->GetWorldEpoch();
    PendingDamageEvents.Reset();
    ServerDamageAudience.WorldEpoch = Service->GetWorldEpoch(); ServerDamageAudience.RegionId = RegionId;
    ++ServerDamageAudience.Generation;
    ServerDamageAudience.FinalEventId = KeepFinal ? FinalDamageEvent.EventId : 0;
    ServerDamageAudience.FinalRegionId = KeepFinal ? FinalDamageEvent.RegionId : INDEX_NONE;
    // 已提交的致命事件以新控制代次重发；接收端保留它的去重身份，不重播已展示数字。
    if (KeepFinal && bFinalDamageCommitted) { PendingDamageEvents.Add(FinalDamageEvent); }
    if (!KeepFinal) { FinalDamageEvent = {}; FinalDamageAudience = {}; bFinalDamageCommitted = false; }
    ClientSetDamageAudience(ServerDamageAudience);
}
void AY01PlayerController::QueueDamageEvent(const FY01DamageAudience& Audience, const FY01DamageEvent& Event)
{
    const bool FinalHandoff = Audience == FinalDamageAudience && ServerDamageAudience.IsFinalEvent(Event);
    if (!HasAuthority() || !bDamageEventsEnabled || bDamageEnded || IsDamageTravelBlocked()
        || (!(Audience == ServerDamageAudience) && !FinalHandoff) || !IsDamageValid(Event, ServerDamageAudience)) { return; }
    if (Event.WorldEpoch == FinalDamageEvent.WorldEpoch && Event.EventId == FinalDamageEvent.EventId) { bFinalDamageCommitted = true; }
    if (PendingDamageEvents.Num() == DamageCapacity) { PendingDamageEvents.RemoveAt(0, 1, EAllowShrinking::No); }
    PendingDamageEvents.Add(Event);
}
void AY01PlayerController::Tick(float DeltaSeconds)
{
    if (bSendingDamageBatch) { return; }
    Super::Tick(DeltaSeconds);
    EnsureHealthBarWorld();
    EnsureHealthBarPool();
    EnsureDamageNumberPool();
    if (!HasAuthority() || !bDamageEventsEnabled || bDamageEnded || IsDamageTravelBlocked() || PendingDamageEvents.IsEmpty()) { return; }
    const double Now = GetWorld()->GetRealTimeSeconds();
    if (Now < NextDamageBatchAt) { return; }
    NextDamageBatchAt = Now + .05;
    TGuardValue<bool> Sending(bSendingDamageBatch, true);
    auto Events = MoveTemp(PendingDamageEvents); PendingDamageEvents.Reset();
    const auto Audience = ServerDamageAudience;
    const TWeakObjectPtr<UWorld> BatchWorld = GetWorld();
    const bool Trace = FParse::Param(FCommandLine::Get(), TEXT("Y01DamageTrace"));
    if (Trace) { UE_LOG(LogTemp, Display, TEXT("Y01Damage BATCH Controller=%s Time=%.9f Count=%d"), *GetName(), Now, Events.Num()); }
    for (int32 Offset = 0; Offset < Events.Num(); Offset += DamageChunkCapacity)
    {
        // 本地listen server的消费委托可能切区、切图或销毁Controller；旧快照立即停发，不碰重入的新队列。
        if (!IsValid(this) || IsActorBeingDestroyed() || bDamageEnded || !bDamageEventsEnabled || !HasAuthority()
            || IsDamageTravelBlocked() || !BatchWorld.IsValid() || GetWorld() != BatchWorld.Get() || BatchWorld->bIsTearingDown
            || !(Audience == ServerDamageAudience)) { break; }
        if (const auto* Connection = GetNetConnection(); Connection && !Connection->IsNetReady())
        {
            // 引擎会直接丢弃饱和时的Unreliable RPC；仅保留尚未发送的尾部，不重试已发送包。
            const int32 Unsent = Events.Num() - Offset;
            Events.RemoveAt(0, Offset, EAllowShrinking::No);
            Events.Append(PendingDamageEvents);
            if (Events.Num() > DamageCapacity) { Events.RemoveAt(0, Events.Num() - DamageCapacity, EAllowShrinking::No); }
            PendingDamageEvents = MoveTemp(Events);
            if (Trace) { UE_LOG(LogTemp, Display, TEXT("Y01Damage DEFER Controller=%s Time=%.9f Count=%d Pending=%d NetSpeed=%d QueuedBits=%d"),
                *GetName(), Now, Unsent, PendingDamageEvents.Num(), Connection->CurrentNetSpeed, Connection->QueuedBits); }
            break;
        }
        TArray<FY01DamageEvent> Chunk;
        Chunk.Append(Events.GetData() + Offset, FMath::Min(DamageChunkCapacity, Events.Num() - Offset));
        if (Trace)
        {
            UE_LOG(LogTemp, Display, TEXT("Y01Damage CHUNK Controller=%s Time=%.9f Offset=%d Count=%d BytesBudget=%d"),
                *GetName(), Now, Offset, Chunk.Num(), DamageRPCBytesBudget + DamageEventBytesBudget * Chunk.Num());
            for (const auto& E : Chunk)
            { UE_LOG(LogTemp, Display, TEXT("Y01Damage SEND Controller=%s Player=%d Id=%llu Epoch=%s Region=%d Generation=%llu Actual=%.9g Display=%.9g Type=%d"),
                *GetName(), PlayerState ? PlayerState->GetPlayerId() : -1, E.EventId, *E.WorldEpoch.ToString(EGuidFormats::Digits), E.RegionId,
                Audience.Generation, E.ActualDamage, E.DisplayAmount, static_cast<int32>(E.Type)); }
        }
        ClientDamageBatch(Audience, Chunk);
    }
}
void AY01PlayerController::ResetDamageDisplay(uint64 KeepReceivedId)
{
    ReceivedDamageIds.Reset(); HighestDamageId = KeepReceivedId;
    if (KeepReceivedId) { ReceivedDamageIds.Add(KeepReceivedId); }
    OnDamageDisplayReset.Broadcast();
}
void AY01PlayerController::ClientSetDamageAudience_Implementation(const FY01DamageAudience& Audience)
{
    if (!bDamageEventsEnabled || bDamageEnded || IsDamageTravelBlocked() || GetNetMode() == NM_DedicatedServer || !IsLocalController()
        || !Audience.WorldEpoch.IsValid() || Audience.WorldEpoch == RetiredDamageEpoch
        || Audience.RegionId < INDEX_NONE || Audience.Generation <= ClientDamageAudience.Generation) { return; }
    const uint64 KeepId = Audience.WorldEpoch == ClientDamageAudience.WorldEpoch && ReceivedDamageIds.Contains(Audience.FinalEventId) ? Audience.FinalEventId : 0;
    ClientDamageAudience = Audience; EnsureDamageNumberPool(); ResetDamageDisplay(KeepId);
}
void AY01PlayerController::ClientDamageBatch_Implementation(const FY01DamageAudience& Audience, const TArray<FY01DamageEvent>& Events)
{
    if (!bDamageEventsEnabled || bDamageEnded || IsDamageTravelBlocked() || GetNetMode() == NM_DedicatedServer || !IsLocalController()
        || !(Audience == ClientDamageAudience) || Events.Num() > DamageChunkCapacity) { return; }
    EnsureDamageNumberPool();
    for (const auto& E : Events)
    {
        // 消费委托可能切区或结束World，每一项都再次确认代次。
        if (!IsValid(this) || IsActorBeingDestroyed() || bDamageEnded || !bDamageEventsEnabled || IsDamageTravelBlocked()
            || !GetWorld() || GetWorld()->bIsTearingDown || !(Audience == ClientDamageAudience)) { break; }
        if (!IsDamageValid(E, Audience) || (HighestDamageId >= DamageHistory && E.EventId <= HighestDamageId - DamageHistory)
            || ReceivedDamageIds.Contains(E.EventId)) { continue; }
        HighestDamageId = FMath::Max(HighestDamageId, E.EventId);
        for (auto It = ReceivedDamageIds.CreateIterator(); It; ++It)
        { if (HighestDamageId >= DamageHistory && *It <= HighestDamageId - DamageHistory) { It.RemoveCurrent(); } }
        ReceivedDamageIds.Add(E.EventId);
        if (FParse::Param(FCommandLine::Get(), TEXT("Y01DamageTrace")))
        { UE_LOG(LogTemp, Display, TEXT("Y01Damage RECEIVE Controller=%s Player=%d Id=%llu Epoch=%s Region=%d Generation=%llu Actual=%.9g Display=%.9g Type=%d"),
            *GetName(), PlayerState ? PlayerState->GetPlayerId() : -1, E.EventId, *E.WorldEpoch.ToString(EGuidFormats::Digits), E.RegionId,
            Audience.Generation, E.ActualDamage, E.DisplayAmount, static_cast<int32>(E.Type)); }
        OnDamageReceived.Broadcast(E);
    }
}

void AY01PlayerController::RequestSkillSlot(int32 Slot)
{
    if (Slot < 0 || Slot >= Y01Data::SkillSlotCount) { return; }
    if (HasAuthority()) { ServerRequestSkillSlot_Implementation(Slot); }
    else if (IsLocalController()) { ServerRequestSkillSlot(Slot); }
}
void AY01PlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();
    BuildKeyboardMapping();
    if (auto* Input = Cast<UEnhancedInputComponent>(InputComponent))
    {
        Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ThisClass::Move);
        for (int32 Slot = 0; Slot < Y01Data::SkillSlotCount; ++Slot)
        { Input->BindAction(SkillActions[Slot], ETriggerEvent::Started, this, &ThisClass::Skill, Slot); }
    }
    InstallMapping();
}
void AY01PlayerController::BeginPlay()
{
    Super::BeginPlay();
    EnsureHealthBarWorld();
    if (IsLocalController()) { BuildKeyboardMapping(); InstallMapping(); }
    EnsureHealthBarPool();
    EnsureDamageNumberPool();
}
void AY01PlayerController::EndPlay(EEndPlayReason::Type Reason)
{
    bHealthBarEnded = true; ReleaseHealthBarPool(); RevokeHealthBarAuthorization();
    AcceptedHealthBarAuthorization = {}; OnHealthBarAuthorizationChanged.Broadcast(); OnHealthBarAuthorizationChanged.Clear();
    bDamageEnded = true; PendingDamageEvents.Reset();
    FinalDamageEvent = {}; FinalDamageAudience = {}; bFinalDamageCommitted = false;
    ServerDamageAudience.WorldEpoch.Invalidate(); ClientDamageAudience.WorldEpoch.Invalidate();
    ReleaseDamageNumberPool(); ResetDamageDisplay(); OnDamageReceived.Clear(); OnDamageDisplayReset.Clear();
    if (auto* Local = GetLocalPlayer())
    {
        if (auto* Subsystem = Local->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(); Subsystem && KeyboardMapping)
        { Subsystem->RemoveMappingContext(KeyboardMapping); }
    }
    Super::EndPlay(Reason);
}
void AY01PlayerController::BuildKeyboardMapping()
{
    if (KeyboardMapping) { return; }
    KeyboardMapping = NewObject<UInputMappingContext>(this, TEXT("KeyboardMapping"));
    MoveAction = NewObject<UInputAction>(this, TEXT("Move"));
    MoveAction->ValueType = EInputActionValueType::Axis2D;
    auto MapMove = [&](FKey Key, bool bNegate, bool bSwap)
    {
        auto& Mapping = KeyboardMapping->MapKey(MoveAction, Key);
        if (bNegate) { Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(KeyboardMapping)); }
        if (bSwap)
        {
            auto* Swizzle = NewObject<UInputModifierSwizzleAxis>(KeyboardMapping);
            Swizzle->Order = EInputAxisSwizzle::YXZ; Mapping.Modifiers.Add(Swizzle);
        }
    };
    MapMove(EKeys::W, false, true); MapMove(EKeys::S, true, true);
    MapMove(EKeys::A, true, false); MapMove(EKeys::D, false, false);
    const FKey Keys[] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four };
    for (int32 Slot = 0; Slot < Y01Data::SkillSlotCount; ++Slot)
    {
        auto* Action = NewObject<UInputAction>(this, FName(*FString::Printf(TEXT("SkillSlot%d"), Slot + 1)));
        Action->ValueType = EInputActionValueType::Boolean;
        SkillActions.Add(Action); KeyboardMapping->MapKey(Action, Keys[Slot]);
    }
}
void AY01PlayerController::InstallMapping()
{
    if (auto* Local = GetLocalPlayer())
    {
        if (auto* Subsystem = Local->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(); Subsystem && KeyboardMapping)
        { Subsystem->AddMappingContext(KeyboardMapping, 0); }
    }
}
void AY01PlayerController::Move(const FInputActionValue& Value)
{
    auto* Avatar = Cast<AY01Character>(GetPawn());
    if (!Avatar || IsMoveInputIgnored()) { return; }
    const FVector2D Axis = Value.Get<FVector2D>();
    if (!FMath::IsFinite(Axis.X) || !FMath::IsFinite(Axis.Y)) { return; }
    const FVector Direction = FVector(Axis.Y, Axis.X, 0).GetClampedToMaxSize(1.f);
    Avatar->AddMovementInput(FVector::ForwardVector, Direction.X);
    Avatar->AddMovementInput(FVector::RightVector, Direction.Y);
}
void AY01PlayerController::Skill(const FInputActionValue& Value, int32 Slot) { RequestSkillSlot(Slot); }
void AY01PlayerController::ServerRequestSkillSlot_Implementation(int32 Slot)
{
    if (!HasAuthority() || Slot < 0 || Slot >= Y01Data::SkillSlotCount) { return; }
    auto* State = GetPlayerState<AY01PlayerState>();
    auto* Avatar = Cast<AY01Character>(GetPawn());
    if (!State || !Avatar || Avatar->GetController() != this || Avatar->GetPlayerState() != State
        || State->GetCombatASC()->GetAvatarActor() != Avatar) { return; }
    State->TryActivateSkillSlot(Slot);
}
