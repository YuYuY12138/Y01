#include "Playground/Y01PlaygroundVerification.h"
#include "Playground/Y01PlaygroundController.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "TimerManager.h"
#include "InputKeyEventArgs.h"
#include "EnhancedPlayerInput.h"

bool AY01PlaygroundController::IsVerificationEnabled()
{ return FParse::Param(FCommandLine::Get(), TEXT("Y01PlaygroundVerify")); }
void AY01PlaygroundController::VerifyPress(const FKey& Key)
{
    InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Pressed, 1.f));
    VerifyReleaseKeys.AddUnique(Key);
}
void AY01PlaygroundController::ClientVerificationAdvance_Implementation()
{
    if (!IsVerificationEnabled()) { return; }
    VerifyPress(EKeys::Enter); ++VerifyEnterCount;
}
void AY01PlaygroundController::TickVerification()
{
    if (!IsVerificationEnabled() || !IsLocalController() || !Observer.bReady || !GetPawn() || AcknowledgedPawn != GetPawn()) { return; }
    for (const auto& Key : VerifyReleaseKeys) { InputKey(FInputKeyEventArgs::CreateSimulated(Key, IE_Released, 0.f)); }
    VerifyReleaseKeys.Reset();
    const auto* GS = GetWorld()->GetGameState<AY01GameState>();
    const auto* State = GetPlayerState<AY01PlayerState>();
    if (!GS || !State || Observer.Slots.Num() != 4) { return; }
    const double Now = GS->GetServerWorldTimeSeconds();
    if (VerifyMoveStart < 0)
    {
        VerifyMoveStart = GetWorld()->GetTimeSeconds(); VerifyStartPosition = GetPawn()->GetActorLocation();
        const bool Accepted = InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W, IE_Pressed, 1.f));
        UE_LOG(LogTemp, Display, TEXT("Y01Playground INPUT_BEGIN Accepted=%d ServerTime=%.3f LocalTime=%.3f Position=%s"), Accepted, Now, GetWorld()->GetTimeSeconds(), *VerifyStartPosition.ToString());
    }
    if (!bVerifyMoveReleased && GetWorld()->GetTimeSeconds() - VerifyMoveStart > 0.25)
    {
        InputKey(FInputKeyEventArgs::CreateSimulated(EKeys::W, IE_Released, 0.f));
        bVerifyMoveReleased = true;
        UE_LOG(LogTemp, Display, TEXT("Y01Playground INPUT_RELEASE ServerTime=%.3f LocalTime=%.3f Travel=%.1f"), Now, GetWorld()->GetTimeSeconds(), FVector::Dist2D(VerifyStartPosition, GetPawn()->GetActorLocation()));
    }
    if (VerifyMoveStart >= 0 && GetWorld()->GetTimeSeconds() - VerifyMoveStart < 0.4)
    {
        const auto* Enhanced = Cast<UEnhancedPlayerInput>(PlayerInput);
        UE_LOG(LogTemp, Display, TEXT("Y01Playground INPUT_STATE Down=%d Ignored=%d Ack=%d Role=%d Enhanced=%d Value=%s"),
            IsInputKeyDown(EKeys::W), IsMoveInputIgnored(), AcknowledgedPawn == GetPawn(), static_cast<int32>(GetPawn()->GetLocalRole()),
            Enhanced != nullptr, Enhanced ? *Enhanced->GetActionValue(GetMoveAction()).ToString() : TEXT("none"));
    }
    if (bVerifyMoveReleased && FVector::Dist2D(VerifyStartPosition, GetPawn()->GetActorLocation()) > 20) { bInputMoved = true; }
    if (Now < NextVerifyTick) { return; }
    NextVerifyTick = Now + 0.15;
    const auto& S = GS->GetWaveSnapshot();
    if (FMath::Fmod(Now, 5.0) < 0.16)
    {
        UE_LOG(LogTemp, Display, TEXT("Y01Playground OBSERVE Phase=%d Result=%d Wave=%d Complete=%d Gold=%d XP=%d HP=%.0f Move=%d Damage=%d CD=%.3f InputMoved=%d Enter=%d Notice=%d"),
            static_cast<int32>(S.Phase), static_cast<int32>(S.Result), S.WaveIndex, S.bWaveCompleted,
            State->GetGold(), State->GetExperience(), Y01Playground::Health(State->GetCombatASC()), bObservedMovement, bObservedDamage, LastObservedCooldown, bInputMoved, VerifyEnterCount, !Observer.Notice.IsEmpty());
    }
    for (TActorIterator<AY01PlaygroundVisual> It(GetWorld()); It; ++It)
    {
        AY01Monster* Monster = It->Target;
        if (!IsValid(Monster) || Y01Playground::Health(Monster->GetCombatASC()) <= 0) { continue; }
        const FVector Location = Monster->GetActorLocation();
        if (const auto* Before = ObservedPositions.Find(Monster))
        {
            const double Travel = FVector::Dist2D(*Before, Location);
            if (Travel > 2 && Travel < 100 && Location.Z > 70) { bObservedMovement = true; }
        }
        ObservedPositions.Add(Monster, Location);
    }
    for (APlayerState* Item : GS->PlayerArray)
    {
        const auto* Other = Cast<AY01PlayerState>(Item);
        if (!Other) { continue; }
        const auto* ASC = Other->GetCombatASC();
        if (Y01Playground::Health(ASC) > 0 && Y01Playground::Health(ASC) < ASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()))
        { bObservedDamage = true; }
    }
    if (Observer.Slots[0].Deadline > Now + 0.05)
    { LastObservedCooldown = Observer.Slots[0].Deadline; }
    if (!bScreenshotRequested && S.Phase == EY01MatchPhase::Battle && Now - S.ServerPhaseTime > 4 && FParse::Param(FCommandLine::Get(), TEXT("Y01PlaygroundScreenshot")))
    {
        const FString Filename = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots") /
            FString::Printf(TEXT("Playground-%u.png"), FPlatformProcess::GetCurrentProcessId()));
        FScreenshotRequest::RequestScreenshot(Filename, true, false);
        UE_LOG(LogTemp, Display, TEXT("Y01Playground SCREENSHOT_REQUEST %s"), *Filename); bScreenshotRequested = true;
    }
    auto Screenshot = [&](FName Stage)
    {
        if (!FParse::Param(FCommandLine::Get(), TEXT("Y01PlaygroundScreenshot")) || VerifyScreenshots.Contains(Stage)) { return; }
        const FString File = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Screenshots") /
            FString::Printf(TEXT("Playground-%u-%s.png"), FPlatformProcess::GetCurrentProcessId(), *Stage.ToString()));
        FScreenshotRequest::RequestScreenshot(File, true, false); VerifyScreenshots.Add(Stage);
        UE_LOG(LogTemp, Display, TEXT("Y01Playground SCREENSHOT_REQUEST %s"), *File);
    };
    if (S.Phase == EY01MatchPhase::Waiting) { Screenshot(TEXT("Waiting")); }
    if (S.Phase == EY01MatchPhase::Battle && S.WaveIndex == 0 && Observer.Slots[0].Deadline > Now + 0.2) { Screenshot(TEXT("Cooldown")); }
    if (S.Phase == EY01MatchPhase::Battle && S.WaveIndex == 1 && Now - S.ServerPhaseTime > 2) { Screenshot(TEXT("Boss")); }
    if (S.Phase == EY01MatchPhase::Ended)
    {
        Screenshot(TEXT("Ended"));
        if (!bVerifyRestartPressed) { VerifyPress(EKeys::F5); bVerifyRestartPressed = true; }
        if (!Observer.Notice.IsEmpty()) { Screenshot(TEXT("F5")); }
    }
    FY01PlaygroundEvidence E;
    E.Phase = static_cast<int32>(S.Phase); E.Gold = State->GetGold(); E.XP = State->GetExperience();
    E.MaxHealth = State->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute());
    E.Attack = State->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute());
    E.bPassive = Observer.Slots[2].bEffectApplied; E.CooldownDeadline = LastObservedCooldown;
    E.bMoved = bObservedMovement; E.bDamage = bObservedDamage;
    E.bInputMoved = bInputMoved; E.bRestartNotice = !Observer.Notice.IsEmpty();
    if (bInputMoved) { ServerVerifyObservation(E); }
    if (S.Phase == EY01MatchPhase::Battle && !S.bWaveCompleted && Now - S.ServerPhaseTime > 2 && bObservedMovement && bObservedDamage)
    { VerifyPress(EKeys::One); VerifyPress(EKeys::Two); }
}
void AY01PlaygroundController::ServerVerifyObservation_Implementation(FY01PlaygroundEvidence Evidence)
{
    if (!IsVerificationEnabled() || !HasAuthority()) { return; }
    const auto* Mode = GetWorld()->GetAuthGameMode<AY01PlaygroundGameMode>();
    const auto* State = GetPlayerState<AY01PlayerState>();
    if (!Mode || !State || !Mode->GetFlowState() || !Mode->IsMatchConfigured()) { return; }
    const auto* ASC = State->GetCombatASC();
    const bool bAttributesMatch = FMath::IsNearlyEqual(Evidence.MaxHealth, ASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()))
        && FMath::IsNearlyEqual(Evidence.Attack, ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()));
    bool bPassiveMatches = false;
    double Deadline = 0;
    for (const auto& Grant : ASC->GetGrantedSources())
    {
        if (Grant.Source.Type != EY01TableKind::Skill) { continue; }
        if (Grant.Slot == 2) { bPassiveMatches = Grant.bEffectApplied == Evidence.bPassive; }
        if (Grant.Slot == 0) { Deadline = Grant.NextActivationTime; }
    }
    if (!bAttributesMatch || !bPassiveMatches) { return; }
    if (!bVerifyReady && Evidence.Phase == static_cast<int32>(EY01MatchPhase::Waiting))
    {
        bVerifyReady = true;
        UE_LOG(LogTemp, Display, TEXT("Y01Playground READY_ACK Peer=%d HP=%.0f Attack=%.0f Passive=%d"), State->GetPlayerId(), Evidence.MaxHealth, Evidence.Attack, Evidence.bPassive);
    }
    const auto& S = Mode->GetFlowState()->GetWaveSnapshot();
    const bool bFinal = S.Phase == EY01MatchPhase::Ended && S.Result == EY01MatchResult::Success;
    if (!bVerifyComplete && bFinal && Evidence.Phase == static_cast<int32>(EY01MatchPhase::Ended)
        && Evidence.Gold == State->GetGold() && Evidence.XP == State->GetExperience()
        && Evidence.bMoved && Evidence.bDamage && Evidence.bInputMoved && Evidence.bRestartNotice && !Observer.Notice.IsEmpty() && Evidence.CooldownDeadline > 0 && Evidence.CooldownDeadline <= Deadline + 0.001)
    {
        bVerifyComplete = true;
        UE_LOG(LogTemp, Display, TEXT("Y01Playground FINAL_ACK Peer=%d Gold=%d XP=%d Move=%d Damage=%d ObservedCD=%.3f ServerCD=%.3f"),
            State->GetPlayerId(), Evidence.Gold, Evidence.XP, Evidence.bMoved, Evidence.bDamage, Evidence.CooldownDeadline, Deadline);
    }
}
void AY01PlaygroundController::ClientVerificationFinished_Implementation()
{
    if (!IsVerificationEnabled()) { return; }
    if (bInputMoved && VerifyEnterCount > 0 && !Observer.Notice.IsEmpty() && LastObservedCooldown > 0)
    { UE_LOG(LogTemp, Display, TEXT("Y01Playground INPUT_PASS WMove=1 Enter=%d SkillKeys=1,2 F5Notice=1 (engine input path; not OS keyboard)"), VerifyEnterCount); }
    UE_LOG(LogTemp, Display, TEXT("Y01Playground VERIFY_CLIENT_PASS Peer=%d Move=%d Damage=%d CD=%.3f"),
        PlayerState ? PlayerState->GetPlayerId() : -1, bObservedMovement, bObservedDamage, LastObservedCooldown);
    FTimerHandle Handle;
    GetWorldTimerManager().SetTimer(Handle, [] { FPlatformMisc::RequestExitWithStatus(false, 0); }, 2.f, false);
}
AY01PlaygroundVerification::AY01PlaygroundVerification() { PrimaryActorTick.bCanEverTick = true; }
void AY01PlaygroundVerification::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bFinished || !HasAuthority() || !AY01PlaygroundController::IsVerificationEnabled()) { return; }
    auto* Mode = GetWorld()->GetAuthGameMode<AY01PlaygroundGameMode>();
    if (!Mode || !Mode->IsMatchConfigured() || !Mode->GetFlowState()) { return; }
    int32 ExpectedPeers = 1; FParse::Value(FCommandLine::Get(), TEXT("Y01PlaygroundPeers="), ExpectedPeers);
    TArray<AY01PlaygroundController*> Controllers;
    for (TActorIterator<AY01PlaygroundController> It(GetWorld()); It; ++It) { Controllers.Add(*It); }
    if (Controllers.Num() != ExpectedPeers || Controllers.ContainsByPredicate([](const auto* PC) { return !PC->bVerifyReady; })) { return; }
    const auto& S = Mode->GetFlowState()->GetWaveSnapshot();
    if ((S.Phase == EY01MatchPhase::Waiting || (S.Phase == EY01MatchPhase::Battle && S.bWaveCompleted)) && GetWorld()->GetTimeSeconds() > NextAdvance)
    {
        NextAdvance = GetWorld()->GetTimeSeconds() + 1;
        for (auto* PC : Controllers) { PC->ClientVerificationAdvance(); }
    }
    if (S.Phase == EY01MatchPhase::Ended && !Controllers.ContainsByPredicate([](const auto* PC) { return !PC->bVerifyComplete; }))
    {
        bFinished = true;
        UE_LOG(LogTemp, Display, TEXT("Y01Playground VERIFY_SERVER_PASS Peers=%d Completed=%d"), Controllers.Num(), S.CompletedWaves);
        for (auto* PC : Controllers) { PC->ClientVerificationFinished(); }
        if (GetNetMode() == NM_DedicatedServer)
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, [] { FPlatformMisc::RequestExitWithStatus(false, 0); }, 3.f, false);
        }
    }
}
