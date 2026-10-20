#include "Playground/Y01TrainingRooms.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01TestSkill.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01AttributeResolver.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Combat/Y01Monster.h"
#include "Combat/Y01Core.h"
#include "Match/Y01KillRewards.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameplayEffectExtension.h"
#include "Engine/World.h"
#include "Engine/StaticMesh.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

AY01TrainingRoomGeometry::AY01TrainingRoomGeometry()
{
    bReplicates = true; bAlwaysRelevant = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    const FVector Positions[] = { {0,0,-50}, {1000,0,250}, {-1000,0,250}, {0,1000,250}, {0,-1000,250} };
    const FVector Scales[] = { {21,21,1}, {1,21,6}, {1,21,6}, {21,1,6}, {21,1,6} };
    for (int32 I = 0; I < 5; ++I)
    {
        auto* Shape = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("Boundary%d"), I));
        Shape->SetupAttachment(RootComponent); Shape->SetStaticMesh(Cube.Object);
        Shape->SetRelativeLocation(Positions[I]); Shape->SetRelativeScale3D(Scales[I]);
        Shape->SetCollisionProfileName(TEXT("BlockAll"));
    }
}
UY01TrainingRooms::UY01TrainingRooms() { PrimaryComponentTick.bCanEverTick = false; }
FVector UY01TrainingRooms::Center(const FY01TrainingSession& Session) const
{ return FVector(12000 + Session.View.Room * 4000, 0, 0); }
FVector UY01TrainingRooms::BaseReturnLocation(const ACharacter* Pawn, const FVector& Fallback) const
{
    const auto* Mode = Cast<AY01PlaygroundGameMode>(GetOwner());
    const AY01Core* Core = Mode ? Mode->GetMatchCore() : nullptr;
    const auto* Capsule = Pawn->GetCapsuleComponent();
    const float Radius = Capsule->GetScaledCapsuleRadius(), Half = Capsule->GetScaledCapsuleHalfHeight();
    FCollisionQueryParams Query(SCENE_QUERY_STAT(Y01TrainingReturn), false, Pawn);
    const auto Standable = [&](const FVector& XY, FVector& Out)
    {
        FHitResult Hit;
        if (!GetWorld()->LineTraceSingleByObjectType(Hit, XY + FVector(0, 0, 500), XY - FVector(0, 0, 1500),
                FCollisionObjectQueryParams(FCollisionObjectQueryParams::InitType::AllStaticObjects), Query)
            || Hit.ImpactNormal.Z < Pawn->GetCharacterMovement()->GetWalkableFloorZ()) { return false; }
        Out = Hit.ImpactPoint + FVector(0, 0, Half + 2);
        return !GetWorld()->OverlapBlockingTestByProfile(Out, FQuat::Identity, Capsule->GetCollisionProfileName(),
            FCollisionShape::MakeCapsule(Radius, Half), Query);
    };
    FVector Out;
    if (IsValid(Core))
    {
        FBox Bounds(ForceInit);
        TInlineComponentArray<UPrimitiveComponent*> Components; Core->GetComponents(Components);
        for (const auto* Component : Components)
        { if (Component->IsA<UCapsuleComponent>() || Component->IsA<UStaticMeshComponent>()) { Bounds += Component->Bounds.GetBox(); } }
        // 俯视镜头偏航为0，画面“上方”即世界+X；沿该方向取首个可站立点，不落入核心占地。
        const FVector Anchor = Core->GetActorLocation();
        const double Start = (Bounds.IsValid ? Bounds.Max.X - Anchor.X : 0) + Radius + 8;
        for (int32 Step = 0; Step < 8; ++Step)
        { if (Standable(Anchor + FVector(Start + Step * 100, 0, 0), Out)) { return Out; } }
    }
    for (TActorIterator<APlayerStart> It(GetWorld()); It; ++It)
    { if (Standable(It->GetActorLocation(), Out)) { return Out; } }
    return Fallback;
}
const FY01TrainingStageRow* UY01TrainingRooms::StageFor(const FY01TrainingSession& Session) const
{
    const auto* Mode = Cast<AY01PlaygroundGameMode>(GetOwner());
    const auto* Profile = Mode ? Mode->GetProfile() : nullptr;
    const UDataTable* Table = Profile ? Profile->Tables.FindRef(EY01TableKind::TrainingStage).Get() : nullptr;
    if (!Table || !Session.View.bInside) { return nullptr; }
    const double Elapsed = GetWorld()->GetTimeSeconds() - Session.View.EnteredAt;
    const FY01TrainingStageRow* Found = nullptr;
    for (const auto& Pair : Table->GetRowMap())
    {
        const auto* Row = reinterpret_cast<const FY01TrainingStageRow*>(Pair.Value);
        if (Row->StartSeconds <= Elapsed && (!Found || Row->StartSeconds > Found->StartSeconds)) { Found = Row; }
    }
    return Found;
}
FY01TrainingView UY01TrainingRooms::Observe(const AY01PlayerState* Player) const
{
    const auto* Session = Sessions.Find(Player);
    if (!Session) { return {}; }
    auto View = Session->View; View.Alive = 0;
    for (const auto& Record : Session->Records)
    { if (Record.Monster.IsValid() && Y01Playground::Health(Record.Monster->GetCombatASC()) > 0) { ++View.Alive; } }
    if (const auto* Row = StageFor(*Session)) { View.Stage = Row->Stage; View.MaxAlive = Row->MaxAlive; }
    return View;
}
TArray<AY01Monster*> UY01TrainingRooms::Monsters(const AY01PlayerState* Player) const
{
    TArray<AY01Monster*> Result;
    if (const auto* Session = Sessions.Find(Player))
    { for (const auto& Record : Session->Records) { if (Record.Monster.IsValid()) { Result.Add(Record.Monster.Get()); } } }
    return Result;
}
bool UY01TrainingRooms::Request(AY01PlaygroundController* PC, bool bEnter)
{
    auto* Mode = Cast<AY01PlaygroundGameMode>(GetOwner());
    auto* Player = PC ? PC->GetPlayerState<AY01PlayerState>() : nullptr;
    APawn* Pawn = PC ? PC->GetPawn() : nullptr;
    if (bStopped || !Mode || !Mode->HasAuthority() || !PC || PC->GetWorld() != GetWorld() || !Player || !Pawn
        || Pawn->GetController() != PC || Player->GetCombatASC()->GetAvatarActor() != Pawn
        || !Mode->GetFlowState() || !Mode->GetFlowState()->PlayerArray.Contains(Player)) { return false; }
    auto& Session = Sessions.FindOrAdd(Player);
    if (Mode->IsCoreDefenseEnabled() && (Mode->IsGameplayFrozen() || Y01Playground::Health(Player->GetCombatASC()) <= 0)) { return false; }
    if (!bEnter) { PC->SetDamageRegion(0); Leave(Player, Session, true); return true; }
    if (Mode->IsGameplayFrozen() || !Mode->IsMatchConfigured() || !Mode->GetProfile() || !Mode->GetProfile()->bTrainingEnabled
        || !Player->IsCharacterInitialized() || Y01Playground::Health(Player->GetCombatASC()) <= 0
        || Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended)
    { Session.View.Error = TEXT("训练未启用、玩家已死亡或对局已结束；请停止重开恢复"); return false; }
    if (Session.View.bInside) { return true; }
    if (!Session.Geometry.IsValid())
    {
        Session.View.Room = ++NextRoom;
        Session.Geometry = GetWorld()->SpawnActor<AY01TrainingRoomGeometry>(Center(Session), FRotator::ZeroRotator);
        if (!Session.Geometry.IsValid()) { Session.View.Error = TEXT("练功房地面生成失败"); return false; }
    }
    Session.ReturnTransform = Pawn->GetActorTransform();
    Player->GetCombatASC()->InvalidateCombatSession(); BindCombat(Player->GetCombatASC());
    if (auto* Character = Cast<ACharacter>(Pawn)) { Character->GetCharacterMovement()->StopMovementImmediately(); }
    if (!Pawn->TeleportTo(Center(Session) + FVector(0, 0, 100), Pawn->GetActorRotation(), false, true))
    { Session.View.Error = TEXT("练功房传送失败"); return false; }
    Session.View.bInside = true; ++Session.View.Session; Session.View.EnteredAt = GetWorld()->GetTimeSeconds(); Session.View.Error.Reset();
    PC->SetDamageRegion(Session.View.Room);
    PC->SetHealthBarRegion(Session.View.Room, Session.View.Session);
    const auto* First = StageFor(Session);
    if (bImmediateFirstBatchForTest || !First) { Batch(Player, Session.View.Session); }
    else
    {
        Session.View.NextBatchAt = GetWorld()->GetTimeSeconds() + First->SpawnInterval;
        GetWorld()->GetTimerManager().SetTimer(Session.BatchTimer, FTimerDelegate::CreateUObject(this, &ThisClass::Batch,
            TWeakObjectPtr<AY01PlayerState>(Player), Session.View.Session), First->SpawnInterval, false);
    }
    UE_LOG(LogTemp, Display, TEXT("Y01Training ENTER Player=%s Room=%d Session=%d Alive=%d"), *Player->GetName(), Session.View.Room, Session.View.Session, Observe(Player).Alive);
    return true;
}
void UY01TrainingRooms::Leave(AY01PlayerState* Player, FY01TrainingSession& Session, bool bReturn)
{
    const bool bWasInside = Session.View.bInside;
    if (bWasInside && Player)
    {
        for (auto It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
        {
            if (auto* PC = Cast<AY01PlaygroundController>(It->Get()); PC && PC->PlayerState == Player)
            {
                PC->RevokeHealthBarAuthorization();
            }
        }
    }
    if (bWasInside && bReturn && Player)
    {
        for (auto It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
        { if (auto* PC = Cast<AY01PlaygroundController>(It->Get()); PC && PC->PlayerState == Player)
            { PC->SetDamageRegion(0, Y01Playground::Health(Player->GetCombatASC()) <= 0); } }
    }
    // 先使会话失效，再清理计时器和Actor；任何回调都不能产生奖励或再刷。
    Session.View.bInside = false; ++Session.View.Session; Session.View.Stage = 0; Session.View.NextBatchAt = 0;
    // 恢复主线必须经过统一生命周期策略，不能在GameMode关闭时重新授权。
    if (bWasInside && bReturn && Player && !bStopped)
    {
        const auto* Mode = Cast<AY01PlaygroundGameMode>(GetOwner());
        for (auto It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
        { if (auto* PC = Cast<AY01PlaygroundController>(It->Get()); Mode && PC && PC->PlayerState == Player)
            { Mode->RefreshHealthBarAuthorization(PC); } }
    }
    auto& Timers = GetWorld()->GetTimerManager(); Timers.ClearTimer(Session.BatchTimer);
    for (auto& Handle : Session.PendingTimers) { Timers.ClearTimer(Handle); }
    Session.PendingTimers.Reset(); Session.Pending = 0;
    for (auto& Record : Session.Records)
    {
        if (Record.Monster.IsValid()) { Record.Monster->GetCombatASC()->InvalidateCombatSession(); Record.Monster->Destroy(); }
        if (Record.Visual.IsValid()) { Record.Visual->Destroy(); }
    }
    Session.Records.Reset();
    if (Player && bWasInside)
    {
        Player->GetCombatASC()->InvalidateCombatSession();
        if (auto* Pawn = Cast<ACharacter>(Player->GetCombatASC()->GetAvatarActor()))
        {
            Pawn->GetCharacterMovement()->StopMovementImmediately();
            if (bReturn)
            { Pawn->TeleportTo(BaseReturnLocation(Pawn, Session.ReturnTransform.GetLocation()), Pawn->GetActorRotation(), false, true); }
        }
        UE_LOG(LogTemp, Display, TEXT("Y01Training LEAVE Player=%s Kills=%d Gold=%d XP=%d"), *Player->GetName(), Session.View.Kills, Player->GetGold(), Player->GetExperience());
    }
}
void UY01TrainingRooms::Batch(TWeakObjectPtr<AY01PlayerState> Player, int32 SessionId)
{
    auto* Session = Sessions.Find(Player);
    if (!Player.IsValid() || !Session || !Session->View.bInside || Session->View.Session != SessionId || bStopped) { return; }
    const auto* Mode = CastChecked<AY01PlaygroundGameMode>(GetOwner());
    if (Mode->IsGameplayFrozen()) { return; }
    if (Y01Playground::Health(Player->GetCombatASC()) <= 0 || Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended)
    { Leave(Player.Get(), *Session, true); return; }
    const auto* Row = StageFor(*Session);
    if (!Row) { Session->View.Error = TEXT("当前经过时间无可用训练阶段"); Leave(Player.Get(), *Session, true); return; }
    FY01DataValidation Query;
    const auto* Reward = Mode->GetProfile()->BuildCatalog().FindRow<FY01RewardPackageRow>(Row->Reward, Query);
    if (!Reward || !Reward->Items.IsEmpty() || Reward->Domain != EY01RewardDomain::MatchPersonal)
    { Session->View.Error = TEXT("训练奖励不受支持"); Leave(Player.Get(), *Session, true); return; }
    auto& Timers = GetWorld()->GetTimerManager();
    Session->PendingTimers.RemoveAll([&](const FTimerHandle& Handle) { return !Timers.IsTimerActive(Handle); });
    for (auto Entry : Row->Spawns)
    {
        if (Mode->IsGameplayFrozen()) { return; }
        const int32 Space = FMath::Max(0, Row->MaxAlive - Observe(Player.Get()).Alive - Session->Pending);
        Entry.Count = FMath::Min(Entry.Count, Space);
        if (Entry.Count <= 0) { continue; }
        Session->Pending += Entry.Count;
        if (Entry.DelaySeconds <= 0) { Spawn(Player, SessionId, Entry, *Reward); }
        else
        {
            FTimerHandle Handle;
            Timers.SetTimer(Handle, FTimerDelegate::CreateUObject(this, &ThisClass::Spawn, Player, SessionId, Entry, *Reward), Entry.DelaySeconds, false);
            Session->PendingTimers.Add(Handle);
        }
    }
    Session->View.NextBatchAt = GetWorld()->GetTimeSeconds() + Row->SpawnInterval;
    Timers.SetTimer(Session->BatchTimer, FTimerDelegate::CreateUObject(this, &ThisClass::Batch, Player, SessionId), Row->SpawnInterval, false);
    UE_LOG(LogTemp, Display, TEXT("Y01Training BATCH Player=%s Room=%d Stage=%d Alive=%d Interval=%g Cap=%d"),
        *Player->GetName(), Session->View.Room, Row->Stage, Observe(Player.Get()).Alive, Row->SpawnInterval, Row->MaxAlive);
}
void UY01TrainingRooms::Spawn(TWeakObjectPtr<AY01PlayerState> Player, int32 SessionId, FY01SpawnEntry Entry, FY01RewardPackageRow Reward)
{
    auto* Session = Sessions.Find(Player);
    if (!Player.IsValid() || !Session || !Session->View.bInside || Session->View.Session != SessionId || bStopped) { return; }
    Session->Pending -= Entry.Count;
    const auto* Mode = CastChecked<AY01PlaygroundGameMode>(GetOwner());
    if (Mode->IsGameplayFrozen()) { return; }
    if (Y01Playground::Health(Player->GetCombatASC()) <= 0 || Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { return; }
    const auto* Row = StageFor(*Session); if (!Row) { return; }
    const int32 Count = FMath::Min(Entry.Count, FMath::Max(0, Row->MaxAlive - Observe(Player.Get()).Alive));
    for (int32 I = 0; I < Count; ++I)
    {
        if (Mode->IsGameplayFrozen()) { return; }
        const double Angle = Session->Records.Num() * UE_PI / 3;
        FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Monster = GetWorld()->SpawnActor<AY01Monster>(Center(*Session) + FVector(350 * FMath::Cos(Angle), 350 * FMath::Sin(Angle), 100), FRotator::ZeroRotator, Params);
        FString Error;
        if (!Monster || !Monster->TryInitializeMonster(Mode->GetProfile()->BuildCatalog(), Entry.Monster, Error))
        { if (Monster) { Monster->Destroy(); } Session->View.Error = Error; continue; }
        Monster->GetCharacterMovement()->bRunPhysicsWithNoController = true;
        Monster->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        auto* Visual = GetWorld()->SpawnActor<AY01PlaygroundVisual>(); Visual->Target = Monster; Visual->Definition = Entry.Monster;
        Session->Records.Add({Monster, Visual, Reward, false}); BindCombat(Monster->GetCombatASC());
        GetWorld()->GetSubsystem<UY01KillRewards>()->RegisterMonster(Monster);
        Monster->RegisterHealthBarPresentation(Session->View.Room, Session->View.Session);
        Mode->GetGrowth()->RegisterMonster(Monster, Player.Get());
        Monster->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddWeakLambda(this,
            [this, Player, SessionId, WeakMonster = TWeakObjectPtr<AY01Monster>(Monster)](const FOnAttributeChangeData& Data)
            { OnHealth(Player, SessionId, WeakMonster, Data); });
    }
}
void UY01TrainingRooms::OnHealth(TWeakObjectPtr<AY01PlayerState> Player, int32 SessionId, TWeakObjectPtr<AY01Monster> Monster, const FOnAttributeChangeData& Data)
{
    if (!Player.IsValid() || !Monster.IsValid() || Data.OldValue <= 0 || Data.NewValue > 0 || !Data.GEModData) { return; }
    auto* Session = Sessions.Find(Player);
    if (!Session || !Session->View.bInside || Session->View.Session != SessionId) { return; }
    const auto* Killer = Cast<UY01CombatAbilitySystem>(Data.GEModData->EffectSpec.GetContext().GetOriginalInstigatorAbilitySystemComponent());
    if (Killer != Player->GetCombatASC() || !PairAllowed(Killer, Monster->GetCombatASC())) { return; }
    auto* Record = Session->Records.FindByPredicate([&](const auto& Item) { return Item.Monster == Monster; });
    if (!Record || Record->bSettled) { return; }
    Record->bSettled = true;
    // R37第四期：练功房金币按归属者金币获取%折算后经小数钱包入账；经验保持整数原值。
    FY01ResolvedAttributes Resolved; double Gain = 0;
    if (Player->GetSourceEffects()->ObserveAttributes(Resolved).IsOk()) { Gain = Resolved.GoldGainPercent; }
    if (Player->TryAddFractionalGold(Record->Reward.Gold * (1. + Gain))
        && Player->TryAddMatchReward(0, Record->Reward.Experience))
    { ++Session->View.Kills; }
    else { Session->View.Error = TEXT("训练奖励溢出或非法，未发放"); }
}
bool UY01TrainingRooms::PairAllowed(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target) const
{
    if (bStopped || !Source || !Target) { return false; }
    const auto* Mode = Cast<AY01PlaygroundGameMode>(GetOwner());
    if (!Mode || Mode->IsGameplayFrozen() || !Mode->GetFlowState() || Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { return false; }
    if (Cast<AY01Core>(Source->GetAvatarActor()) || Cast<AY01Core>(Target->GetAvatarActor()))
    {
        const auto* Core = Mode->GetMatchCore();
        const auto* MainlineMonster = Cast<AY01Monster>(Source->GetAvatarActor());
        return Mode->IsCoreDefenseEnabled() && Mode->IsCombatRunning() && IsValid(Core)
            && Target == Core->GetCombatASC() && Target->GetOwner() == Core && Target->GetAvatarActor() == Core
            && IsValid(MainlineMonster) && !MainlineMonster->IsActorBeingDestroyed()
            && Source == MainlineMonster->GetCombatASC() && Source->GetOwner() == MainlineMonster
            && Mode->GetWaveMonsters().Contains(MainlineMonster);
    }
    const auto* Player = Cast<AY01PlayerState>(Source->GetOwner());
    const auto* Monster = Cast<AY01Monster>(Target->GetAvatarActor());
    if (!Player) { Player = Cast<AY01PlayerState>(Target->GetOwner()); Monster = Cast<AY01Monster>(Source->GetAvatarActor()); }
    if (!Player || !Monster) { return false; }
    if ((Mode->IsCoreDefenseEnabled() || (Mode->GetProfile() && Mode->GetProfile()->bR34AutoAttackEnabled))
        && !Mode->IsPlayerPresent(Player)) { return false; }
    const auto* Session = Sessions.Find(Player);
    if (Session && Session->View.bInside)
    { return Session->Records.ContainsByPredicate([&](const auto& Record) { return Record.Monster.Get() == Monster; }); }
    return Mode->IsCombatRunning() && Mode->GetWaveMonsters().Contains(Monster);
}
void UY01TrainingRooms::BindCombat(UY01CombatAbilitySystem* ASC)
{
    if (!ASC) { return; }
    const TWeakObjectPtr<UY01TrainingRooms> WeakThis(this);
    ASC->TargetPermission = [WeakThis](const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target)
    { return WeakThis.IsValid() && WeakThis->PairAllowed(Source, Target); };
}
void UY01TrainingRooms::Update()
{
    auto* Mode = Cast<AY01PlaygroundGameMode>(GetOwner());
    if (bStopped || !Mode || Mode->IsGameplayFrozen() || !Mode->GetFlowState()) { return; }
    if (Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { StopAll(); return; }
    struct FRoomUpdate
    {
        TWeakObjectPtr<AY01PlayerState> Player;
        int32 Room;
        int32 SessionId;
    };
    // 回调可删除或新增Sessions；外层也不能跨攻击持有TMap迭代器。
    TArray<FRoomUpdate> Rooms;
    for (const auto& Pair : Sessions) { Rooms.Add({Pair.Key, Pair.Value.View.Room, Pair.Value.View.Session}); }
    for (const auto& Room : Rooms)
    {
        if (bStopped || !IsValid(Mode) || Mode->IsGameplayFrozen() || !Mode->GetFlowState()
            || Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { return; }
        if (!Room.Player.IsValid())
        {
            FY01TrainingSession Removed;
            if (Sessions.RemoveAndCopyValue(Room.Player, Removed))
            { Leave(nullptr, Removed, false); if (Removed.Geometry.IsValid()) { Removed.Geometry->Destroy(); } }
            continue;
        }
        TArray<TWeakObjectPtr<AY01Monster>> Monsters;
        TArray<FY01TrainingMonsterRecord> Retired;
        {
            auto* Session = Sessions.Find(Room.Player);
            if (!Session || !Session->View.bInside || Session->View.Room != Room.Room || Session->View.Session != Room.SessionId) { continue; }
            if (Y01Playground::Health(Room.Player->GetCombatASC()) <= 0) { Leave(Room.Player.Get(), *Session, true); continue; }
            Session->Records.RemoveAll([&](const auto& Record)
            {
                if (Record.Monster.IsValid() && Y01Playground::Health(Record.Monster->GetCombatASC()) > 0) { return false; }
                Retired.Add(Record); return true;
            });
            for (const auto& Record : Session->Records) { Monsters.Add(Record.Monster); }
        }
        // 先移除记录，再销毁Actor；不在销毁或伤害回调期间持有容器内引用。
        for (const auto& Record : Retired)
        {
            if (Record.Monster.IsValid()) { Record.Monster->Destroy(); }
            if (Record.Visual.IsValid()) { Record.Visual->Destroy(); }
        }
        const auto IsRoomActive = [&]
        {
            if (bStopped || !IsValid(Mode) || Mode->IsGameplayFrozen() || !Mode->GetFlowState()
                || Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended
                || !Room.Player.IsValid() || Room.Player->IsActorBeingDestroyed()
                || Y01Playground::Health(Room.Player->GetCombatASC()) <= 0) { return false; }
            const auto* Session = Sessions.Find(Room.Player);
            return Session && Session->View.bInside && Session->View.Room == Room.Room && Session->View.Session == Room.SessionId;
        };
        for (const auto& WeakMonster : Monsters)
        {
            if (!IsRoomActive()) { break; }
            auto* Monster = WeakMonster.Get();
            if (!IsValid(Monster) || Monster->IsActorBeingDestroyed()) { continue; }
            auto* ASC = Monster->GetCombatASC();
            TArray<AActor*> Candidates;
            if (Mode->IsPlayerPresent(Room.Player.Get())) { Candidates.Add(Room.Player->GetCombatASC()->GetAvatarActor()); }
            AActor* Avatar = Monster->RefreshDetectionTarget(Candidates);
            const bool bAuto = Mode->GetProfile() && Mode->GetProfile()->bR34AutoAttackEnabled;
            if (bAuto) { ASC->UpdateHealthRegen(true); }
            if (!IsRoomActive()) { break; }
            if (!WeakMonster.IsValid() || Monster->IsActorBeingDestroyed()) { continue; }
            if (Avatar && (bAuto ? ASC->IsAutoAttackInRange(Avatar) : ASC->IsValidCombatTarget(Avatar)))
            {
                Monster->GetCharacterMovement()->StopMovementImmediately();
                if (!IsRoomActive()) { break; }
                if (bAuto) { AY01TestAttack::TryAutoAttack(ASC); } else { ASC->TryActivateSkillSlot(0); }
                if (!IsRoomActive()) { break; }
            }
            else if (IsValid(Avatar))
            {
                const float MaxSpeed = Monster->GetCharacterMovement()->MaxWalkSpeed;
                const FVector Desired = (Avatar->GetActorLocation() - Monster->GetActorLocation()).GetSafeNormal2D() * MaxSpeed;
                if (MaxSpeed > KINDA_SMALL_NUMBER) { Monster->AddMovementInput(Monster->ResolveMovementVelocity(Desired) / MaxSpeed); }
            }
        }
    }
}
void UY01TrainingRooms::Remove(AY01PlayerState* Player)
{
    if (auto* Session = Sessions.Find(Player))
    { Leave(Player, *Session, false); if (Session->Geometry.IsValid()) { Session->Geometry->Destroy(); } Sessions.Remove(Player); }
}
void UY01TrainingRooms::ReturnPlayer(AY01PlayerState* Player)
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) { return; }
    if (auto* Session = Sessions.Find(Player); Session && Session->View.bInside) { Leave(Player, *Session, true); }
}
void UY01TrainingRooms::StopAll()
{ for (auto& Pair : Sessions) { Leave(Pair.Key.Get(), Pair.Value, true); } }
void UY01TrainingRooms::EndPlay(const EEndPlayReason::Type Reason)
{
    bStopped = true; StopAll();
    for (auto& Pair : Sessions) { if (Pair.Value.Geometry.IsValid()) { Pair.Value.Geometry->Destroy(); } }
    Sessions.Reset(); Super::EndPlay(Reason);
}
