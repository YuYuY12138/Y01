#include "Playground/Y01PlaygroundController.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "GameFramework/PlayerInput.h"

AY01PlaygroundController::AY01PlaygroundController() { PrimaryActorTick.bCanEverTick = true; }
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
    else if (Action >= 0 && Action < 4 && Mode->IsCombatRunning() && GetPawn() && GetPawn()->GetController() == this)
    { Mode->RefreshTarget(State); RequestSkillSlot(Action); }
}
void AY01PlaygroundController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority()) { CaptureObserver(GetWorld()->GetAuthGameMode<AY01PlaygroundGameMode>()); }
    TickVerification();
}
void AY01PlaygroundController::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION(AY01PlaygroundController, Observer, COND_OwnerOnly);
}
void AY01PlaygroundController::CaptureObserver(const AY01PlaygroundGameMode* Mode)
{
    if (!HasAuthority() || !Mode) { return; }
    Observer.Error = Mode->GetConfigurationError();
    const auto* State = GetPlayerState<AY01PlayerState>();
    const auto* Profile = Mode->GetProfile();
    Observer.bReady = Mode->IsMatchConfigured() && State && State->IsCharacterInitialized();
    Observer.ProfilePath = Profile ? Profile->GetPathName() : TEXT("(missing Profile)");
    Observer.Alive = Mode->AliveCount();
    Observer.Total = Mode->GetWaveMonsters().Num();
    Observer.Slots.SetNum(4);
    for (auto& Slot : Observer.Slots) { Slot = FY01PlaygroundSlotView(); }
    Observer.Target = nullptr;
    if (!Observer.bReady) { return; }
    const auto* ASC = State->GetCombatASC();
    for (const auto& Grant : ASC->GetGrantedSources())
    {
        if (Grant.Source.Type != EY01TableKind::Skill || !Observer.Slots.IsValidIndex(Grant.Slot)) { continue; }
        auto& Slot = Observer.Slots[Grant.Slot];
        Slot.Row = Grant.Source.RowName; Slot.bActive = Grant.SkillType == EY01SkillType::Active;
        Slot.Deadline = Grant.NextActivationTime;
        Slot.bEffectApplied = Grant.bEffectApplied && (!Grant.Effect.IsValid() || ASC->GetActiveGameplayEffect(Grant.Effect) != nullptr);
    }
    Observer.Target = Cast<AY01Monster>(ASC->GetCombatTarget());
    FY01DataValidation Query;
    if (Observer.Target && Profile)
    {
        Observer.TargetRow = Observer.Target->GetCombatASC()->GetDefinition();
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
            if (Wave) { Observer.Total = 0; for (const auto& Entry : Wave->Spawns) { Observer.Total += Entry.Count; } }
        }
    }
}
void AY01PlaygroundController::ShowRestartNotice()
{ Observer.Notice = TEXT("Save tables. Stop this session (launcher Ctrl+C / PIE Stop), then run again to reload disk data."); }
