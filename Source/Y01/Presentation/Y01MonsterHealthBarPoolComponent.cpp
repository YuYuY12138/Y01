#include "Presentation/Y01MonsterHealthBarPoolComponent.h"
#include "Presentation/Y01MonsterHealthBarWidget.h"
#include "Combat/Y01Monster.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "SceneView.h"

UY01MonsterHealthBarPoolComponent::UY01MonsterHealthBarPoolComponent()
{
    PrimaryComponentTick.bCanEverTick = true; PrimaryComponentTick.bStartWithTickEnabled = false;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork; SetIsReplicatedByDefault(false);
}
bool UY01MonsterHealthBarPoolComponent::InitializePool()
{
#if UE_SERVER
    return false;
#else
    auto* PC = Cast<AY01PlayerController>(GetOwner());
    if (bShutdown || !PC || !PC->IsLocalController() || !PC->GetLocalPlayer() || !GetWorld()
        || GetWorld()->bIsTearingDown || GetNetMode() == NM_DedicatedServer || !PC->GetHealthBarAuthorization().IsValidScope()) { return false; }
    if (PoolWorld.IsValid()) { return PoolWorld.Get() == GetWorld(); }
    Settings = GetDefault<UY01MonsterHealthBarSettings>()->Snapshot();
    if (!Settings.bEnabled) { return false; }
    Controller = PC; PoolWorld = GetWorld(); Authorization = PC->GetHealthBarAuthorization(); Entries.Reserve(Settings.Capacity);
    AuthorizationHandle = PC->OnHealthBarAuthorizationChanged.AddUObject(this, &ThisClass::AuthorizationChanged);
    CleanupHandle = FWorldDelegates::OnWorldCleanup.AddUObject(this, &ThisClass::WorldCleanup);
    LevelHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(this, &ThisClass::LevelAdded);
    SpawnHandle = GetWorld()->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateUObject(this, &ThisClass::RegisterCandidate));
    DestroyHandle = GetWorld()->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateUObject(this, &ThisClass::RemoveCandidate));
    // 先监听再补扫：覆盖初始化前已有Actor以及稍后完成登记/网络快照的Actor。
    for (TActorIterator<AY01Monster> It(GetWorld()); It; ++It) { RegisterCandidate(*It); }
    for (int32 I = 0; I < Settings.PrewarmCount; ++I) { if (!Grow()) { Shutdown(); return false; } }
    SetComponentTickEnabled(true); return true;
#endif
}
bool UY01MonsterHealthBarPoolComponent::Grow()
{
    auto* World = PoolWorld.Get(); auto* PC = Controller.Get();
    if (bShutdown || !World || World != GetWorld() || World->bIsTearingDown || !PC || World->GetNetMode() == NM_DedicatedServer || Entries.Num() >= Settings.Capacity) { return false; }
    FActorSpawnParameters P; P.Owner = PC; P.ObjectFlags |= RF_Transient;
    P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Actor = World->SpawnActor<AActor>(FVector::ZeroVector, FRotator::ZeroRotator, P);
    if (!Actor) { return false; }
    Actor->SetReplicates(false); Actor->PrimaryActorTick.bCanEverTick = false;
    Actor->SetActorHiddenInGame(true); Actor->SetActorEnableCollision(false);
    auto* Display = NewObject<UY01MonsterHealthBarWorldWidgetComponent>(Actor, TEXT("MonsterHealthBarWorldWidget"), RF_Transient);
    Actor->AddInstanceComponent(Display); Actor->SetRootComponent(Display);
    Display->SetOwnerPlayer(PC->GetLocalPlayer()); Display->SetVisibility(false); Display->RegisterComponent();
    auto* Widget = CreateWidget<UY01MonsterHealthBarWidget>(PC, UY01MonsterHealthBarWidget::StaticClass());
    if (!Widget) { Display->DestroyComponent(); Actor->Destroy(); return false; }
    Widget->TakeWidget(); Display->SetWidget(Widget);
    auto& Entry = Entries.AddDefaulted_GetRef(); Entry.Actor = Actor; Entry.Display = Display; Entry.Widget = Widget;
    Return(Entry); return true;
}
void UY01MonsterHealthBarPoolComponent::RegisterCandidate(AActor* Actor)
{
    auto* M = Cast<AY01Monster>(Actor);
    if (bShutdown || !IsValid(M) || M->IsActorBeingDestroyed() || M->GetWorld() != PoolWorld.Get() || Candidates.Contains(M)) { return; }
    auto& Candidate = Candidates.Add(M); Candidate.Sequence = ++NextCandidateSequence;
    Candidate.ChangedHandle = M->OnHealthBarPresentationChanged.AddUObject(this, &ThisClass::CandidateChanged, TWeakObjectPtr<AY01Monster>(M));
}
void UY01MonsterHealthBarPoolComponent::RemoveCandidate(AActor* Actor)
{
    auto* M = Cast<AY01Monster>(Actor); if (!M) { return; }
    for (auto& Lease : Entries) { if (Lease.Target == M) { Return(Lease); } }
    if (auto* C = Candidates.Find(M)) { M->OnHealthBarPresentationChanged.Remove(C->ChangedHandle); Candidates.Remove(M); }
}
void UY01MonsterHealthBarPoolComponent::CandidateChanged(TWeakObjectPtr<AY01Monster> Target)
{
    if (bShutdown) { return; }
    // 通知只撤销/更新已有租约，新增资格在10Hz筛选时参与竞争。
    for (auto& Lease : Entries)
    { if (Lease.Target == Target && Lease.Version && ValidateLease(Lease)) { Update(Lease, 0); } }
}
void UY01MonsterHealthBarPoolComponent::AuthorizationChanged()
{
    if (bShutdown) { return; }
    const auto Scope = Controller.IsValid() ? Controller->GetHealthBarAuthorization() : FY01HealthBarAuthorization();
    for (auto& Lease : Entries) { Return(Lease); }
    Authorization = Scope; NextSelection = Clock;
}
void UY01MonsterHealthBarPoolComponent::LevelAdded(ULevel* Level, UWorld* World)
{
    if (bShutdown || World != PoolWorld.Get() || !Level) { return; }
    for (const auto& Actor : Level->Actors) { RegisterCandidate(Actor.Get()); }
}
FVector UY01MonsterHealthBarPoolComponent::Anchor(const AY01Monster* Target) const
{ return Target->GetActorLocation() + FVector(0,0,Target->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 20); }
bool UY01MonsterHealthBarPoolComponent::BuildView(FMatrix& OutViewProjection, FVector& OutEye) const
{
    const auto* PC = Controller.Get(); const auto* Local = PC ? PC->GetLocalPlayer() : nullptr;
    if (!Local || !Local->ViewportClient || !Local->ViewportClient->Viewport) { return false; }
    FSceneViewProjectionData Data;
    if (!Local->GetProjectionData(Local->ViewportClient->Viewport, Data)) { return false; }
    OutViewProjection = Data.ComputeViewProjectionMatrix(); OutEye = Data.ViewOrigin;
    return !OutViewProjection.ContainsNaN() && !OutEye.ContainsNaN();
}
bool UY01MonsterHealthBarPoolComponent::InView(const AY01Monster* Target, const FMatrix& ViewProjection) const
{
    const FVector Position = Anchor(Target);
    if (Position.ContainsNaN()) { return false; }
    const auto Clip = ViewProjection.TransformFVector4(FVector4(Position,1));
    // 使用真实视锥的齐次裁剪，不用屏幕固定像素，也不添加额外距离上限。
    return FMath::IsFinite(Clip.W) && Clip.W > 0 && FMath::Abs(Clip.X) <= Clip.W && FMath::Abs(Clip.Y) <= Clip.W && Clip.Z >= 0 && Clip.Z <= Clip.W;
}
bool UY01MonsterHealthBarPoolComponent::ValidateLease(FY01MonsterHealthBarLease& Lease)
{
    auto* M = Lease.Target.Get(); auto* PC = Controller.Get();
    const auto Scope = PC ? PC->GetHealthBarAuthorization() : FY01HealthBarAuthorization();
    const auto State = M ? M->GetHealthBarPresentationState() : FY01MonsterPresentationState();
    FMatrix View; FVector Eye;
    if (!Lease.Version || !IsValid(M) || M->IsActorBeingDestroyed() || M->GetWorld() != PoolWorld.Get()
        || !IsReadyFor(GetWorld()) || !Scope.Matches(State) || Scope.Generation != Lease.AuthorizationGeneration
        || State.Kind != EY01MonsterPresentationKind::Normal || !State.bEverActuallyDamaged
        || !BuildView(View,Eye) || !InView(M,View)) { Return(Lease); return false; }
    if (State.bDead || M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) <= 0)
    {
        if (!Lease.bDeathHolding) { Lease.bDeathHolding = true; Lease.DeathDeadline = Clock + Settings.DeathHoldSeconds; }
        if (Clock + 1.e-6 >= Lease.DeathDeadline) { Return(Lease); return false; }
    }
    return true;
}
void UY01MonsterHealthBarPoolComponent::SelectCandidates()
{
    FMatrix View; FVector Eye;
    if (!Authorization.IsValidScope() || !BuildView(View, Eye)) { return; }
    struct FChoice { TWeakObjectPtr<AY01Monster> Target; double Distance; uint64 Sequence; };
    TArray<FChoice> Choices;
    for (auto It = Candidates.CreateIterator(); It; ++It)
    {
        auto* M = It.Key().Get();
        if (!IsValid(M) || M->IsActorBeingDestroyed() || M->GetWorld() != PoolWorld.Get())
        { if (M) { M->OnHealthBarPresentationChanged.Remove(It.Value().ChangedHandle); } It.RemoveCurrent(); continue; }
        const auto State = M->GetHealthBarPresentationState();
        if (!Authorization.Matches(State) || State.Kind != EY01MonsterPresentationKind::Normal || !State.bEverActuallyDamaged || !InView(M,View)) { continue; }
        const auto* Held = Entries.FindByPredicate([&](const auto& E) { return E.Version && E.Target == M; });
        const float HP = M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
        const float Max = M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute());
        if (!FMath::IsFinite(HP) || !FMath::IsFinite(Max) || Max <= 0) { continue; }
        if ((State.bDead || HP <= 0) && (!Held || !Held->bDeathHolding || Clock + 1.e-6 >= Held->DeathDeadline)) { continue; }
        Choices.Add({M, FVector::DistSquared(Eye, M->GetActorLocation()), It.Value().Sequence});
    }
    Choices.Sort([](const FChoice& A, const FChoice& B) { return A.Distance == B.Distance ? A.Sequence < B.Sequence : A.Distance < B.Distance; });
    if (Choices.Num() > Settings.Capacity) { Choices.SetNum(Settings.Capacity); }
    TSet<TWeakObjectPtr<AY01Monster>> Selected;
    for (const auto& C : Choices) { Selected.Add(C.Target); }
    for (auto& Lease : Entries) { if (Lease.Version && !Selected.Contains(Lease.Target)) { Return(Lease); } }
    for (const auto& Choice : Choices)
    {
        if (Entries.ContainsByPredicate([&](const auto& E) { return E.Version && E.Target == Choice.Target; })) { continue; }
        auto* M = Choice.Target.Get();
        if (!M || !M->GetHealthBarPresentationState().HasLiveHealthBarEligibility()
            || M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) <= 0) { continue; }
        int32 Index = Entries.IndexOfByPredicate([](const auto& E) { return E.Version == 0; });
        if (Index == INDEX_NONE && Grow()) { Index = Entries.Num() - 1; }
        if (Index != INDEX_NONE) { Borrow(Index, M); }
    }
}
void UY01MonsterHealthBarPoolComponent::Borrow(int32 Index, AY01Monster* Target)
{
    auto& Lease = Entries[Index]; Return(Lease);
    Lease.Target = Target; Lease.Version = ++NextLeaseVersion; Lease.AuthorizationGeneration = Authorization.Generation;
    auto* ASC = Target->GetCombatASC();
    Lease.HealthHandle = ASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddUObject(this, &ThisClass::AttributeChanged, Index, Lease.Target, Lease.Version, Lease.AuthorizationGeneration);
    Lease.MaxHealthHandle = ASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetMaxHealthAttribute()).AddUObject(this, &ThisClass::AttributeChanged, Index, Lease.Target, Lease.Version, Lease.AuthorizationGeneration);
    if (ValidateLease(Lease)) { Update(Lease, 0); }
}
void UY01MonsterHealthBarPoolComponent::Return(FY01MonsterHealthBarLease& Lease)
{
    const auto PreviousTarget = Lease.Target;
    // 先使所有已排队/复制的旧回调失效，再退订、清显示，最后允许复用。
    Lease.Version = 0; Lease.Target.Reset(); Lease.AuthorizationGeneration = 0;
    if (auto* M = PreviousTarget.Get())
    {
        auto* ASC = M->GetCombatASC();
        ASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).Remove(Lease.HealthHandle);
        ASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetMaxHealthAttribute()).Remove(Lease.MaxHealthHandle);
    }
    Lease.HealthHandle.Reset(); Lease.MaxHealthHandle.Reset(); Lease.DeathDeadline = 0; Lease.bDeathHolding = false;
    if (IsValid(Lease.Display)) { Lease.Display->SetVisibility(false); Lease.Display->SetComponentTickEnabled(false); }
    if (IsValid(Lease.Widget)) { Lease.Widget->ResetBar(); }
    if (IsValid(Lease.Actor))
    { Lease.Actor->SetActorHiddenInGame(true); Lease.Actor->SetActorTransform(FTransform::Identity); Lease.Actor->SetOwner(Controller.Get()); }
}
void UY01MonsterHealthBarPoolComponent::Update(FY01MonsterHealthBarLease& Lease, float DeltaSeconds)
{
    auto* M = Lease.Target.Get(); auto* PC = Controller.Get();
    if (!M || !PC || !IsValid(Lease.Actor) || !IsValid(Lease.Display) || !IsValid(Lease.Widget)) { Return(Lease); return; }
    auto* ASC = M->GetCombatASC();
    Lease.Widget->SetHealth(Lease.bDeathHolding ? 0 : ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), ASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()));
    FVector Eye; FRotator Rotation; PC->GetPlayerViewPoint(Eye,Rotation);
    const auto Position = Anchor(M);
    Lease.Actor->SetOwner(PC->GetViewTarget()); Lease.Actor->SetActorLocationAndRotation(Position, (Eye-Position).Rotation());
    Lease.Actor->SetActorScale3D(FVector(.5));
    // 第一次显示前，绑定、ASC快照、位置/朝向及新纹理均已准备；不会闪现上任目标。
    if (Lease.Display->DrawPrepared(DeltaSeconds)) { Lease.Actor->SetActorHiddenInGame(false); Lease.Display->SetVisibility(true); }
}
void UY01MonsterHealthBarPoolComponent::AttributeChanged(const FOnAttributeChangeData&, int32 Index, TWeakObjectPtr<AY01Monster> Target, uint64 Version, uint64 Generation)
{
    if (bShutdown || !Entries.IsValidIndex(Index) || !Controller.IsValid() || !IsReadyFor(GetWorld())) { return; }
    auto& Lease = Entries[Index];
    if (!Version || Lease.Version != Version || Lease.Target != Target || !Target.IsValid()
        || Lease.AuthorizationGeneration != Generation || Controller->GetHealthBarAuthorization().Generation != Generation) { return; }
    // 不相信回调中的旧数值；通过版本检查后重新读取ASC唯一权威。
    if (ValidateLease(Lease)) { Update(Lease,0); }
}
void UY01MonsterHealthBarPoolComponent::TickComponent(float DeltaSeconds, ELevelTick TickType, FActorComponentTickFunction* TickFunction)
{
    Super::TickComponent(DeltaSeconds,TickType,TickFunction);
    if (bShutdown) { return; }
    if (!Controller.IsValid() || !Controller->IsLocalController() || !Controller->GetLocalPlayer()
        || !IsReadyFor(GetWorld()) || GetWorld()->bIsTearingDown || GetNetMode() == NM_DedicatedServer) { Shutdown(); return; }
    if (!FMath::IsFinite(DeltaSeconds) || DeltaSeconds < 0) { return; }
    Clock += DeltaSeconds;
    const auto Scope = Controller->GetHealthBarAuthorization();
    if (Scope.Generation != Authorization.Generation || Scope.WorldEpoch != Authorization.WorldEpoch || Scope.bEnabled != Authorization.bEnabled) { AuthorizationChanged(); }
    for (auto& Lease : Entries) { if (Lease.Version && ValidateLease(Lease)) { Update(Lease,DeltaSeconds); } }
    if (Clock + 1.e-6 >= NextSelection) { NextSelection = Clock + .1; SelectCandidates(); }
}
void UY01MonsterHealthBarPoolComponent::WorldCleanup(UWorld* World, bool, bool)
{ if (World == PoolWorld.Get()) { Shutdown(); DestroyComponent(); } }
void UY01MonsterHealthBarPoolComponent::Shutdown()
{
    if (bShutdown) { return; }
    bShutdown = true; SetComponentTickEnabled(false);
    if (auto* PC = Controller.Get()) { PC->OnHealthBarAuthorizationChanged.Remove(AuthorizationHandle); }
    if (auto* World = PoolWorld.Get()) { World->RemoveOnActorSpawnedHandler(SpawnHandle); World->RemoveOnActorDestroyedHandler(DestroyHandle); }
    FWorldDelegates::OnWorldCleanup.Remove(CleanupHandle); FWorldDelegates::LevelAddedToWorld.Remove(LevelHandle);
    for (const auto& C : Candidates) { if (auto* M = C.Key.Get()) { M->OnHealthBarPresentationChanged.Remove(C.Value.ChangedHandle); } }
    Candidates.Reset();
    for (auto& Lease : Entries)
    {
        Return(Lease);
        if (IsValid(Lease.Display)) { Lease.Display->ReleaseResources(); Lease.Display->DestroyComponent(); }
        if (IsValid(Lease.Widget)) { Lease.Widget->ReleaseSlateResources(true); }
        if (IsValid(Lease.Actor) && !Lease.Actor->IsActorBeingDestroyed()) { Lease.Actor->Destroy(); }
    }
    Entries.Reset(); Controller.Reset(); PoolWorld.Reset(); Authorization = {};
}
void UY01MonsterHealthBarPoolComponent::EndPlay(EEndPlayReason::Type Reason)
{ Shutdown(); Super::EndPlay(Reason); }
void UY01MonsterHealthBarPoolComponent::OnComponentDestroyed(bool bDestroyingHierarchy)
{ Shutdown(); Super::OnComponentDestroyed(bDestroyingHierarchy); }
