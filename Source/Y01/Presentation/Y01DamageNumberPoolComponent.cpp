#include "Presentation/Y01DamageNumberPoolComponent.h"
#include "Presentation/Y01DamageNumberWidget.h"
#include "Player/Y01PlayerController.h"
#include "Match/Y01GameFlow.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
bool TraceNumbers()
{ return FParse::Param(FCommandLine::Get(), TEXT("Y01DamageTrace")) || FParse::Param(FCommandLine::Get(), TEXT("Y01DamageNumberTrace")); }
}
UY01DamageNumberPoolComponent::UY01DamageNumberPoolComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
    SetIsReplicatedByDefault(false);
}
bool UY01DamageNumberPoolComponent::InitializePool()
{
#if UE_SERVER
    return false;
#else
    auto* PC = Cast<AY01PlayerController>(GetOwner());
    if (bShutdown || !PC || !PC->IsLocalController() || !PC->GetLocalPlayer() || !GetWorld()
        || GetWorld()->bIsTearingDown || GetNetMode() == NM_DedicatedServer) { return false; }
    if (PoolWorld.IsValid()) { return PoolWorld.Get() == GetWorld(); }
    Controller = PC; PoolWorld = GetWorld(); Entries.Reserve(Capacity);
    ReceivedHandle = PC->OnDamageReceived.AddUObject(this, &ThisClass::Receive);
    ResetHandle = PC->OnDamageDisplayReset.AddUObject(this, &ThisClass::ResetDisplay);
    CleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this, &ThisClass::WorldCleanup);
    for (int32 I = 0; I < PrewarmCount; ++I) { if (!Grow()) { Shutdown(); return false; } }
    if (TraceNumbers()) { UE_LOG(LogTemp, Display, TEXT("Y01DamageNumber PREWARM Controller=%s World=%s Count=%d Cap=%d"), *PC->GetName(), *GetWorld()->GetName(), Entries.Num(), Capacity); }
    return true;
#endif
}
bool UY01DamageNumberPoolComponent::Grow()
{
    auto* PC = Controller.Get(); auto* World = PoolWorld.Get();
    if (bShutdown || !PC || !World || World != GetWorld() || World->bIsTearingDown || World->GetNetMode() == NM_DedicatedServer || Entries.Num() >= Capacity) { return false; }
    FActorSpawnParameters Params; Params.Owner = PC; Params.ObjectFlags |= RF_Transient;
    Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Actor = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, Params);
    if (!Actor) { return false; }
    Actor->SetReplicates(false); Actor->PrimaryActorTick.bCanEverTick = false;
    Actor->SetActorHiddenInGame(true); Actor->SetActorEnableCollision(false);
    auto* Display = NewObject<UY01DamageWorldWidgetComponent>(Actor, TEXT("DamageWorldWidget"), RF_Transient);
    Actor->AddInstanceComponent(Display); Actor->SetRootComponent(Display);
    Display->SetOwnerPlayer(PC->GetLocalPlayer()); Display->SetVisibility(false); Display->RegisterComponent();
    auto* Widget = CreateWidget<UY01DamageNumberWidget>(PC, UY01DamageNumberWidget::StaticClass());
    if (!Widget) { Display->DestroyComponent(); Actor->Destroy(); return false; }
    Widget->TakeWidget(); Display->SetWidget(Widget);
    auto& Entry = Entries.AddDefaulted_GetRef(); Entry.Actor = Actor; Entry.Display = Display; Entry.Widget = Widget;
    Return(Entry);
    return true;
}
void UY01DamageNumberPoolComponent::Receive(const FY01DamageEvent& Event)
{
    auto* PC = Controller.Get();
    if (bShutdown || !PC || !IsReadyFor(GetWorld()) || GetNetMode() == NM_DedicatedServer || !PC->IsLocalController()
        || !Event.HasValidAmount() || Event.WorldPosition.ContainsNaN()) { return; }
    const auto& Audience = PC->GetReceivedDamageAudience();
    if (!Audience.WorldEpoch.IsValid() || Event.WorldEpoch != Audience.WorldEpoch || Event.EventId == 0
        || (Event.RegionId != Audience.RegionId && !Audience.IsFinalEvent(Event))) { return; }
    int32 Index = Entries.IndexOfByPredicate([](const auto& Entry) { return !Entry.bActive; });
    if (Index == INDEX_NONE && Entries.Num() < Capacity && Grow()) { Index = Entries.Num() - 1; }
    bool ReusedOldest = false;
    if (Index == INDEX_NONE && !Entries.IsEmpty())
    {
        Index = 0; ReusedOldest = true;
        for (int32 I = 1; I < Entries.Num(); ++I) { if (Entries[I].BorrowSerial < Entries[Index].BorrowSerial) { Index = I; } }
    }
    if (Index == INDEX_NONE) { return; }
    auto& Entry = Entries[Index]; Return(Entry);
    Entry.Event = Event; Entry.Age = 0; Entry.BorrowSerial = ++NextBorrowSerial; Entry.bActive = true;
    FY01MatchPresentation Presentation;
    if (const auto* State = GetWorld()->GetGameState<AY01GameState>(); State && State->GetPresentation().Revision > 0)
    { Presentation = State->GetPresentation().Values; }
    Entry.Widget->SetNumber(Event.DisplayAmount, Event.Type == EY01DamageEventType::Healing ? Presentation.HealingColor : Presentation.DamageColor);
    Entry.Actor->SetActorHiddenInGame(false); Entry.Display->SetVisibility(true);
    Update(Entry, 0); SetComponentTickEnabled(true);
    if (TraceNumbers())
    {
        UE_LOG(LogTemp, Display, TEXT("Y01DamageNumber BORROW Controller=%s Id=%llu Epoch=%s Region=%d Slot=%d Serial=%llu Oldest=%d Actual=%.9g Display=%.9g Type=%d X=%.17g Y=%.17g Z=%.17g Lifetime=0.8 Rise=60"),
            *PC->GetName(), Event.EventId, *Event.WorldEpoch.ToString(EGuidFormats::Digits), Event.RegionId, Index, Entry.BorrowSerial, ReusedOldest,
            Event.ActualDamage, Event.DisplayAmount, static_cast<int32>(Event.Type), Entry.Actor->GetActorLocation().X,
            Entry.Actor->GetActorLocation().Y, Entry.Actor->GetActorLocation().Z);
    }
}
void UY01DamageNumberPoolComponent::Return(FY01DamageNumberEntry& Entry)
{
    if (TraceNumbers() && Entry.bActive)
    { UE_LOG(LogTemp, Display, TEXT("Y01DamageNumber RETURN Controller=%s Id=%llu Serial=%llu Age=%.6f"), *GetNameSafe(Controller.Get()), Entry.Event.EventId, Entry.BorrowSerial, Entry.Age); }
    if (IsValid(Entry.Widget)) { Entry.Widget->ResetNumber(); }
    if (IsValid(Entry.Display)) { Entry.Display->SetVisibility(false); Entry.Display->SetComponentTickEnabled(false); }
    if (IsValid(Entry.Actor))
    { Entry.Actor->SetActorHiddenInGame(true); Entry.Actor->SetActorTransform(FTransform::Identity); Entry.Actor->SetOwner(Controller.Get()); }
    Entry.Event = {}; Entry.Age = 0; Entry.BorrowSerial = 0; Entry.bActive = false;
}
void UY01DamageNumberPoolComponent::Update(FY01DamageNumberEntry& Entry, float DeltaSeconds)
{
    const float Alpha = static_cast<float>(FMath::Clamp(Entry.Age / Lifetime, 0., 1.));
    const FVector Position = Entry.Event.WorldPosition + FVector(0,0,100 + 60 * Alpha);
    FVector Eye; FRotator ViewRotation; Controller->GetPlayerViewPoint(Eye, ViewRotation);
    // 引擎OnlyOwnerSee比较ViewTarget而非Controller；只改显示归属，不附着或跟随受击Actor。
    Entry.Actor->SetOwner(Controller->GetViewTarget());
    Entry.Actor->SetActorLocationAndRotation(Position, (Eye - Position).Rotation());
    Entry.Widget->SetRenderOpacity(1.f - Alpha);
    Entry.Display->DrawFromPool(DeltaSeconds);
}
void UY01DamageNumberPoolComponent::TickComponent(float DeltaSeconds, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaSeconds, TickType, TickFunction);
    if (bShutdown) { return; }
    if (!Controller.IsValid() || !IsReadyFor(GetWorld()) || GetNetMode() == NM_DedicatedServer || !Controller->IsLocalController()) { Shutdown(); return; }
    if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0) { return; }
    bool Active = false;
    for (auto& Entry : Entries)
    {
        if (!Entry.bActive) { continue; }
        Entry.Age += DeltaSeconds;
        if (Entry.Age + 1.e-6 >= Lifetime) { Return(Entry); continue; }
        Update(Entry, DeltaSeconds); Active = true;
    }
    SetComponentTickEnabled(Active);
}
void UY01DamageNumberPoolComponent::ResetDisplay()
{
    if (bShutdown) { return; }
    const auto* PC = Controller.Get();
    const auto Audience = PC ? PC->GetReceivedDamageAudience() : FY01DamageAudience();
    bool Active = false;
    for (auto& Entry : Entries)
    {
        if (Entry.bActive && Audience.WorldEpoch.IsValid() && Audience.IsFinalEvent(Entry.Event)) { Active = true; continue; }
        Return(Entry);
    }
    SetComponentTickEnabled(Active);
}
void UY01DamageNumberPoolComponent::WorldCleanup(UWorld* World, bool bSessionEnded, bool bCleanupResources)
{
    // Controller可以已迁移到新World，按初始化时的World清理而不是GetWorld()。
    if (World == PoolWorld.Get()) { Shutdown(); DestroyComponent(); }
}
void UY01DamageNumberPoolComponent::Shutdown()
{
    if (bShutdown) { return; }
    bShutdown = true; SetComponentTickEnabled(false);
    if (auto* PC = Controller.Get()) { PC->OnDamageReceived.Remove(ReceivedHandle); PC->OnDamageDisplayReset.Remove(ResetHandle); }
    FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle);
    if (TraceNumbers()) { UE_LOG(LogTemp, Display, TEXT("Y01DamageNumber RELEASE Controller=%s Count=%d"), *GetNameSafe(Controller.Get()), Entries.Num()); }
    for (auto& Entry : Entries)
    {
        Return(Entry);
        if (IsValid(Entry.Display)) { Entry.Display->ReleaseResources(); Entry.Display->DestroyComponent(); }
        if (IsValid(Entry.Widget)) { Entry.Widget->ReleaseSlateResources(true); }
        if (IsValid(Entry.Actor) && !Entry.Actor->IsActorBeingDestroyed()) { Entry.Actor->Destroy(); }
    }
    Entries.Reset(); Controller.Reset(); PoolWorld.Reset(); NextBorrowSerial = 0;
}
void UY01DamageNumberPoolComponent::EndPlay(EEndPlayReason::Type Reason)
{ Shutdown(); Super::EndPlay(Reason); }
void UY01DamageNumberPoolComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{ Shutdown(); Super::OnComponentDestroyed(bDestroyingHierarchy); }
