#include "Playground/Y01PlaygroundController.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "Playground/Y01TestSkill.h"
#include "Playground/Y01MonsterHealthBarVerification.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "GameFramework/PlayerInput.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

AY01PlaygroundController::AY01PlaygroundController() { PrimaryActorTick.bCanEverTick = true; bDamageEventsEnabled = true; bHealthBarsEnabled = true; }
void AY01PlaygroundController::BeginPlay()
{
    Super::BeginPlay();
    if (IsLocalController())
    {
        bShowMouseCursor = true; bEnableClickEvents = true;
        FInputModeGameAndUI Mode; Mode.SetHideCursorDuringCapture(false); Mode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
        SetInputMode(Mode);
        AY01MonsterHealthBarVerification::Install(GetWorld());
    }
}
void AY01PlaygroundController::SetupInputComponent()
{
    Super::SetupInputComponent();
    if (PlayerInput)
    {
        // 仅保留本测试实例的F5重开提示，不同时执行引擎默认调试视图命令。
        PlayerInput->DebugExecBindings.RemoveAll([](const FKeyBind& Bind) { return Bind.Key == EKeys::F5; });
    }
    if (auto* Input = Cast<UEnhancedInputComponent>(InputComponent))
    {
        // 仅替换原四槽回调，保留原WASD与Mapping，避免同键双发。
        for (int32 I = Input->GetActionEventBindings().Num() - 1; I >= 0; --I)
        {
            const auto* Action = Input->GetActionEventBindings()[I]->GetAction();
            for (int32 Slot = 0; Slot < 4; ++Slot)
            { if (Action == GetSkillAction(Slot)) { Input->RemoveActionEventBinding(I); break; } }
        }
        for (int32 Slot = 0; Slot < 4; ++Slot)
        { Input->BindAction(GetSkillAction(Slot), ETriggerEvent::Started, this, &ThisClass::SkillPressed, Slot); }
    }
    InputComponent->BindKey(EKeys::Enter, IE_Pressed, this, &ThisClass::Advance);
    InputComponent->BindKey(EKeys::F5, IE_Pressed, this, &ThisClass::Restart);
}
void AY01PlaygroundController::Advance() { ServerPlaygroundAction(4); }
void AY01PlaygroundController::Restart() { ServerPlaygroundAction(5); }
void AY01PlaygroundController::SkillPressed(const FInputActionValue&, int32 Slot) { ServerPlaygroundAction(Slot); }
void AY01PlaygroundController::ServerPlaygroundAction_Implementation(int32 Action)
{
    auto* Mode = GetWorld()->GetAuthGameMode<AY01PlaygroundGameMode>();
    auto* State = GetPlayerState<AY01PlayerState>();
    if (!Mode || !State || !HasAuthority()) { return; }
    if (Action == 4) { Mode->RequestAdvance(this); }
    else if (Action == 5) { Mode->RequestRestart(this); }
    else if (Action == 6 || Action == 7)
    {
        const bool bAccepted = Mode->GetTraining()->Request(this, Action == 6);
        UE_LOG(LogTemp, Display, TEXT("Y01Training UI_RPC Player=%s Action=%d Accepted=%d"), *State->GetName(), Action, bAccepted);
        CaptureObserver(Mode); ForceNetUpdate();
    }
    else if (Action == 8 || Action == 9)
    {
        FY01DataRef Equipment;
        if (Action == 8) { Equipment.Type = EY01TableKind::Equipment; Equipment.RowName = TEXT("EQ_0001"); }
        FString Error;
        const auto* Profile = Mode->GetProfile();
        bool bAccepted = false;
        if (!Mode->IsMatchConfigured() || !Profile || !Mode->IsPlayerPresent(State)) { Error = TEXT("配置未就绪或玩家不在会话"); }
        else { bAccepted = State->TrySetEquipmentSlot(Profile->BuildCatalog(), this, 0, Equipment, Error); }
        Observer.bEquipmentSucceeded = bAccepted;
        Observer.EquipmentResult = bAccepted ? (Action == 8 ? TEXT("成功：第1槽 EQ_0001（重复不叠加）") : TEXT("成功：第1槽已卸下（重复不扣减）")) : TEXT("失败：") + Error;
        CaptureObserver(Mode); ForceNetUpdate();
        UE_LOG(LogTemp, Display, TEXT("Y01R34 EQ_REQUEST Source=%s DS=%.17g Action=%d Slot=0 Row=%s Accepted=%d Base=%g Current=%g Result=%s"),
            *State->GetName(), Observer.ServerSampleTime, Action, *Equipment.RowName.ToString(), bAccepted,
            Observer.AttackBase, Observer.AttackCurrent, *Observer.EquipmentResult);
    }
    else if (Action >= 0 && Action < 4 && (Mode->IsCombatRunning() || Mode->GetTraining()->Observe(State).bInside) && GetPawn() && GetPawn()->GetController() == this)
    { Mode->RefreshTarget(State); RequestSkillSlot(Action); }
}
void AY01PlaygroundController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority()) { CaptureObserver(GetWorld()->GetAuthGameMode<AY01PlaygroundGameMode>()); }
    TickVerification();
    if (IsLocalController() && Observer.bReady && FParse::Param(FCommandLine::Get(), TEXT("Y01TrainingVerify")) && GetWorld()->GetTimeSeconds() >= NextTrainingTrace)
    {
        NextTrainingTrace = GetWorld()->GetTimeSeconds() + 0.2;
        const auto* State = GetPlayerState<AY01PlayerState>(); const auto& T = Observer.Training;
        UE_LOG(LogTemp, Display, TEXT("Y01Training OBSERVE Inside=%d Room=%d Session=%d Stage=%d Alive=%d Cap=%d Kills=%d HP=%.0f Gold=%d XP=%d X=%.0f Z=%.0f"),
            T.bInside, T.Room, T.Session, T.Stage, T.Alive, T.MaxAlive, T.Kills, State ? Y01Playground::Health(State->GetCombatASC()) : 0,
            State ? State->GetGold() : -1, State ? State->GetExperience() : -1, GetPawn() ? GetPawn()->GetActorLocation().X : 0, GetPawn() ? GetPawn()->GetActorLocation().Z : 0);
    }
}
void AY01PlaygroundController::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION(AY01PlaygroundController, Observer, COND_OwnerOnly);
}
TOptional<double> AY01PlaygroundController::GetSnapshotRemaining(double Deadline) const
{
    if (!Observer.bReady || Observer.ServerSampleTime < 0) { return {}; }
    return FMath::Max(0., Deadline - Observer.ServerSampleTime);
}
void AY01PlaygroundController::CaptureObserver(const AY01PlaygroundGameMode* Mode)
{
    if (!HasAuthority() || !Mode) { return; }
    Mode->RefreshHealthBarAuthorization(this);
    Observer.ServerSampleTime = GetWorld()->GetTimeSeconds();
    Observer.Waves = Mode->GetFlowState() ? Mode->GetFlowState()->GetWaveSnapshot() : FY01WaveSnapshot();
    Observer.PreparationDeadline = Observer.Waves.PreparationDeadline;
    Observer.Error = Mode->GetConfigurationError();
    const auto* State = GetPlayerState<AY01PlayerState>();
    const auto* Profile = Mode->GetProfile();
    Observer.bReady = Mode->IsMatchConfigured() && State && State->IsCharacterInitialized();
    Observer.bTrainingAvailable = Profile && Profile->bTrainingEnabled;
    Observer.Training = Mode->GetTraining()->Observe(State);
    Observer.Growth = Mode->GetGrowth()->Observe(State);
    const auto* PlayerASC = State ? State->GetCombatASC() : nullptr;
    Observer.bAutoAttackEnabled = Profile && Profile->bR34AutoAttackEnabled;
    Observer.UnitStats = PlayerASC ? PlayerASC->GetUnitStats() : FY01UnitCombatStats();
    Observer.AutoAttackDeadline = PlayerASC ? PlayerASC->GetNextAutoAttackTime() : 0;
    Observer.TargetStats = FY01UnitCombatStats(); Observer.TargetHealth = 0; Observer.TargetAutoAttackDeadline = 0;
    Observer.Health = Y01Playground::Health(PlayerASC);
    Observer.MaxHealth = PlayerASC ? PlayerASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()) : 0;
    Observer.AttackBase = PlayerASC ? PlayerASC->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute()) : 0;
    Observer.AttackCurrent = PlayerASC ? PlayerASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()) : 0;
    Observer.MoveSpeed = Observer.bAutoAttackEnabled ? Observer.UnitStats.MoveSpeed : PlayerASC ? PlayerASC->GetInitialMoveSpeed() : 0;
    Observer.Gold = State ? State->GetGold() : 0; Observer.XP = State ? State->GetExperience() : 0;
    Observer.ProfilePath = Profile ? Profile->GetPathName() : TEXT("(missing Profile)");
    Observer.Alive = Observer.Waves.bAutomaticWaves ? Observer.Waves.TotalAliveMonsters : Mode->AliveCount();
    Observer.Total = Observer.Waves.bAutomaticWaves ? Observer.Waves.TotalStartedMonsters : Mode->GetWaveMonsters().Num();
    Observer.Slots.SetNum(4);
    for (auto& Slot : Observer.Slots) { Slot = FY01PlaygroundSlotView(); }
    Observer.Equipment.Reset();
    if (State)
    {
        for (const auto& Ref : State->GetEquipmentSlots())
        {
            const int32 Index = Observer.Equipment.Num();
            auto& Item = Observer.Equipment.AddDefaulted_GetRef(); Item.Row = Ref.RowName;
            const auto* Grant = PlayerASC->GetGrantedSources().FindByPredicate([&](const auto& G)
            { return G.Source.Type == EY01TableKind::Equipment && G.Slot == Index && G.Source.RowName == Ref.RowName; });
            if (Grant)
            {
                Item.Quality = Grant->EquipmentQuality; Item.AttackBonus = Grant->AttackBonus;
                Item.bBonusActive = Grant->AttackBonus == 0 || PlayerASC->GetActiveGameplayEffect(Grant->EquipmentAttackEffect) != nullptr;
                Item.bDebugLocked = Grant->EffectClass != nullptr || !Item.bBonusActive;
            }
        }
    }
    Observer.Target = nullptr;
    if (!Observer.bReady) { return; }
    const auto* ASC = State->GetCombatASC();
    for (const auto& Grant : ASC->GetGrantedSources())
    {
        if (Grant.Source.Type != EY01TableKind::Skill || !Observer.Slots.IsValidIndex(Grant.Slot)) { continue; }
        auto& Slot = Observer.Slots[Grant.Slot];
        Slot.Row = Grant.Source.RowName; Slot.bActive = Grant.SkillType == EY01SkillType::Active;
        Slot.Deadline = Grant.NextActivationTime;
        const auto& C = Grant.TestSkill;
        Slot.DamageCoefficient = C.Mode != EY01TestSkillMode::None ? C.DamageCoefficient : Slot.bActive ? 1 : 0;
        if (C.Mode == EY01TestSkillMode::Area)
        {
            Slot.Description = FString::Printf(TEXT("测试范围：固定释放点；Actor中心判定 XY半径%gcm / Z±%gcm；持续%gs\n红圆柱=真实判定；可空放/不查墙；每怪一次；提交Attack×%g；CD%gs"),
                C.Radius, C.HalfHeight, C.ActiveSeconds, C.DamageCoefficient, Grant.CooldownSeconds);
        }
        else if (C.Mode == EY01TestSkillMode::Projectile)
        {
            Slot.Description = FString::Printf(TEXT("测试球：%gcm内最近合法怪；直线不追踪；半径%gcm / 速度%gcm/s / 寿命%gs\n首个合法碰撞伤害；撞墙消失/无目标拒绝；提交Attack×%g；CD%gs；绿球反馈%gs\n绿色标记=命中反馈，非碰撞范围"),
                C.TargetRange, C.Radius, C.Speed, C.Lifetime, C.DamageCoefficient, Grant.CooldownSeconds, C.HitFeedbackSeconds);
        }
        else if (Slot.bActive) { Slot.Description = FString::Printf(TEXT("legacy即时基础攻击（测试）：原距离保护；CD%gs"), Grant.CooldownSeconds); }
        Slot.bEffectApplied = Grant.bEffectApplied && (!Grant.Effect.IsValid() || ASC->GetActiveGameplayEffect(Grant.Effect) != nullptr);
    }
    Observer.Target = Cast<AY01Monster>(Observer.bAutoAttackEnabled ? AY01TestAttack::FindAutoTarget(ASC) : ASC->GetCombatTarget());
    FY01DataValidation Query;
    if (Observer.Target && Profile)
    {
        const auto* TargetASC = Observer.Target->GetCombatASC();
        Observer.TargetRow = TargetASC->GetDefinition(); Observer.TargetStats = TargetASC->GetUnitStats();
        Observer.TargetHealth = Y01Playground::Health(TargetASC); Observer.TargetAutoAttackDeadline = TargetASC->GetNextAutoAttackTime();
        const auto* Row = Profile->BuildCatalog().FindRow<FY01MonsterRow>(Observer.TargetRow, Query);
        if (Row) { Observer.TargetSpeed = Row->MoveSpeed; Observer.TargetRange = Row->Range; }
    }
    Observer.RewardGold = 0; Observer.RewardXP = 0;
    if (Profile && Mode->GetFlowState())
    {
        const int32 Index = FMath::Max(0, Mode->GetFlowState()->GetWaveSnapshot().WaveIndex);
        if (Profile->OrderedWaves.IsValidIndex(Index))
        {
            const auto Catalog = Profile->BuildCatalog();
            const auto* Wave = Catalog.FindRow<FY01WaveRow>(Profile->OrderedWaves[Index], Query);
            const auto* Reward = Wave ? Catalog.FindRow<FY01RewardPackageRow>(Wave->Reward, Query) : nullptr;
            if (Reward) { Observer.RewardGold = Reward->Gold; Observer.RewardXP = Reward->Experience; }
            if (Wave && !Observer.Waves.bAutomaticWaves) { Observer.Total = 0; for (const auto& Entry : Wave->Spawns) { Observer.Total += Entry.Count; } }
        }
    }
}
void AY01PlaygroundController::ShowRestartNotice()
{ Observer.Notice = TEXT("Save tables. Stop this session (launcher Ctrl+C / PIE Stop), then run again to reload disk data."); }
