#include "Y01Smoke.h"
#include "Combat/Y01Monster.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01Character.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "Engine/NetConnection.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformMisc.h"

namespace
{
float Health(const AY01Monster* Monster)
{ return IsValid(Monster) ? Monster->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) : -999; }
FString Slots(const TArray<FY01DataRef>& Refs)
{
    FString S = FString::Printf(TEXT("%d["), Refs.Num());
    for (const auto& R : Refs) { S += FString::Printf(TEXT("%d:%s;"), static_cast<int32>(R.Type), *R.RowName.ToString()); }
    return S + TEXT("]");
}
}
AY01SmokeFloor::AY01SmokeFloor()
{
    bReplicates = true; bAlwaysRelevant = true; SetReplicateMovement(true);
    GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
    GetStaticMeshComponent()->SetIsReplicated(true);
}
AY01SmokeProbe::AY01SmokeProbe()
{ bReplicates = true; bAlwaysRelevant = true; SetNetUpdateFrequency(30); }
void AY01SmokeProbe::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AY01SmokeProbe, Sequence); DOREPLIFETIME(AY01SmokeProbe, Core); DOREPLIFETIME(AY01SmokeProbe, Monsters);
    DOREPLIFETIME(AY01SmokeProbe, Floor);
}
FString AY01SmokeProbe::Observe(UWorld* World, const AY01SmokeProbe* Probe)
{
    const auto* GS = World ? World->GetGameState<AY01GameState>() : nullptr;
    if (!GS || !Probe) { return FString(); }
    const auto& S = GS->GetWaveSnapshot();
    FString Text = FString::Printf(TEXT("Seq=%d Phase=%s Result=%s Wave=%d Ref=%d:%s Time=%.6f Deadline=%.6f Completed=%d Pending=%d Done=%d Revision=%d Core=%.1f"),
        Probe->Sequence, *StaticEnum<EY01MatchPhase>()->GetNameStringByValue(static_cast<int64>(S.Phase)),
        *StaticEnum<EY01MatchResult>()->GetNameStringByValue(static_cast<int64>(S.Result)), S.WaveIndex,
        static_cast<int32>(S.Wave.Type), *S.Wave.RowName.ToString(), S.ServerPhaseTime, S.PreparationDeadline,
        S.CompletedWaves, S.PendingSpawns, S.bWaveCompleted, S.Revision, Health(Probe->Core));
    const auto* Floor = Probe->Floor.Get();
    const bool bFloorReady = Floor && Floor->GetStaticMeshComponent()->GetStaticMesh()
        && Floor->GetStaticMeshComponent()->GetStaticMesh()->GetPathName() == TEXT("/Engine/BasicShapes/Cube.Cube")
        && Floor->GetStaticMeshComponent()->GetCollisionEnabled() == ECollisionEnabled::QueryAndPhysics
        && Floor->GetActorScale3D().Equals(FVector(40, 40, 1), 0.01) && Floor->GetActorLocation().Equals(FVector(0, 0, -50), 0.01);
    Text += FString::Printf(TEXT(" FloorReady=%d Monsters=%d["), bFloorReady, Probe->Monsters.Num());
    for (AY01Monster* Monster : Probe->Monsters) { Text += FString::Printf(TEXT("%.1f;"), Health(Monster)); }
    Text += TEXT("]");
    TArray<AY01PlayerState*> Players;
    for (APlayerState* State : GS->PlayerArray) { if (auto* PS = Cast<AY01PlayerState>(State)) { Players.Add(PS); } }
    Players.Sort([](const AY01PlayerState& A, const AY01PlayerState& B) { return A.GetPlayerId() < B.GetPlayerId(); });
    Text += FString::Printf(TEXT(" Players=%d"), Players.Num());
    for (const auto* PS : Players)
    {
        Text += FString::Printf(TEXT(" {Id=%d Gold=%d XP=%d Health=%.1f Equipment=%s Skills=%s}"), PS->GetPlayerId(), PS->GetGold(), PS->GetExperience(),
            PS->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), *Slots(PS->GetEquipmentSlots()), *Slots(PS->GetSkillSlots()));
    }
    return Text;
}
void AY01SmokeController::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority() || !IsLocalController() || !AY01SmokeGameMode::IsOptedIn(FCommandLine::Get()) || !PlayerState) { return; }
    for (TActorIterator<AY01SmokeProbe> It(GetWorld()); It; ++It)
    {
        if (It->Sequence <= 0) { continue; }
        const FString Observation = AY01SmokeProbe::Observe(GetWorld(), *It);
        if (!Observation.IsEmpty() && Observation != LastObservation)
        {
            LastObservation = Observation;
            UE_LOG(LogTemp, Display, TEXT("Y01Smoke CLIENT_OBS Peer=%d %s"), PlayerState->GetPlayerId(), *Observation);
            ServerObservation(It->Sequence, PlayerState->GetPlayerId(), Observation);
        }
    }
}
void AY01SmokeController::ServerObservation_Implementation(int32 Sequence, int32 ObserverId, const FString& Observation)
{
    if (auto* Mode = GetWorld()->GetAuthGameMode<AY01SmokeGameMode>())
    { Mode->AcceptObservation(this, Sequence, ObserverId, Observation); }
}
void AY01SmokeController::ClientSmokeFinished_Implementation()
{
    if (!HasAuthority() && AY01SmokeGameMode::IsOptedIn(FCommandLine::Get()))
    {
        UE_LOG(LogTemp, Display, TEXT("Y01Smoke CLIENT_FINISHED Peer=%d"), PlayerState ? PlayerState->GetPlayerId() : -1);
        FPlatformMisc::RequestExitWithStatus(false, 0);
    }
}
AY01SmokeGameMode::AY01SmokeGameMode()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = false;
    if (IsOptedIn(FCommandLine::Get())) { PlayerControllerClass = AY01SmokeController::StaticClass(); }
}
bool AY01SmokeGameMode::IsOptedIn(const TCHAR* CommandLine) { return FParse::Param(CommandLine, TEXT("Y01Smoke")); }
bool AY01SmokeGameMode::LoadMatchConfiguration(FString& Error)
{
    bEnabled = IsOptedIn(FCommandLine::Get());
    if (!bEnabled) { return Super::LoadMatchConfiguration(Error); }
    FParse::Value(FCommandLine::Get(), TEXT("Y01SmokeScenario="), Scenario);
    if (Scenario != TEXT("Success") && Scenario != TEXT("Failure")) { Error = TEXT("Smoke场景必须是Success或Failure"); return false; }
    Fixture = MakeUnique<FY01MatchFixture>();
    PlayerControllerClass = AY01SmokeController::StaticClass();
    return TryConfigureMatch(FY01DataCatalog(Fixture->Tables), Fixture->Waves(), FY01MatchFixture::Ref(EY01TableKind::Character), Error);
}
void AY01SmokeGameMode::BeginPlay()
{
    Super::BeginPlay();
    if (!bEnabled || !IsMatchConfigured()) { return; }
    StartedAt = FPlatformTime::Seconds();
    SetActorTickEnabled(true);
    auto* Floor = GetWorld()->SpawnActor<AY01SmokeFloor>();
    Floor->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
    auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    if (!Require(Cube != nullptr, TEXT("引擎碰撞cube存在"))) { return; }
    Floor->GetStaticMeshComponent()->SetStaticMesh(Cube);
    Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
    Floor->SetActorLocation(FVector(0, 0, -50)); Floor->SetActorScale3D(FVector(40, 40, 1));
    Probe = GetWorld()->SpawnActor<AY01SmokeProbe>();
    Probe->Floor = Floor;
    FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    Probe->Core = GetWorld()->SpawnActor<AY01Monster>(FVector(0, 300, 100), FRotator::ZeroRotator, Params);
    FString Error;
    if (!Require(Probe->Core && Probe->Core->TryInitializeMonster(FY01DataCatalog(Fixture->Tables), FY01MatchFixture::Ref(EY01TableKind::Monster), Error), TEXT("核心ASC初始化"))) { return; }
    Probe->Core->bAlwaysRelevant = true;
    UE_LOG(LogTemp, Display, TEXT("Y01Smoke ENABLED Scenario=%s Tables=%d"), *Scenario, Fixture->Tables.Num());
}
void AY01SmokeGameMode::RestartPlayer(AController* Player)
{
    if (!bEnabled) { Super::RestartPlayer(Player); return; }
    const int32 Id = Player->PlayerState ? Player->PlayerState->GetPlayerId() : 0;
    RestartPlayerAtTransform(Player, FTransform(FVector(0, (Id % 2 == 0 ? -150 : 150), 100)));
}
TArray<AY01SmokeController*> AY01SmokeGameMode::Peers() const
{
    TArray<AY01SmokeController*> Result;
    for (auto It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    {
        auto* PC = Cast<AY01SmokeController>(It->Get());
        if (PC && PC->GetNetConnection() && PC->PlayerState && PC->GetPawn()) { Result.Add(PC); }
    }
    Result.Sort([](const AY01SmokeController& A, const AY01SmokeController& B) { return A.PlayerState->GetPlayerId() < B.PlayerState->GetPlayerId(); });
    return Result;
}
bool AY01SmokeGameMode::CanStartWave() const
{ return bEnabled && Peers().Num() == 2 && Acknowledged.Num() == 2; }
bool AY01SmokeGameMode::CanEnterBattle() const
{ return bEnabled && CheckpointName == TEXT("Preparing") && Acknowledged.Num() == 2; }
bool AY01SmokeGameMode::CanCompleteWave() const
{
    if (!bEnabled || !Probe || Health(Probe->Core) <= 0) { return false; }
    for (AY01Monster* Monster : GetWaveMonsters()) { if (!IsValid(Monster) || Health(Monster) > 0) { return false; } }
    return true;
}
FTransform AY01SmokeGameMode::GetMonsterSpawnTransform(int32 I) const
{ return FTransform(FVector(150 + I * 100, 0, 100)); }
void AY01SmokeGameMode::OnWaveMonsterSpawned(AY01Monster* Monster)
{ Monster->bAlwaysRelevant = true; Monster->SetNetUpdateFrequency(30); }
bool AY01SmokeGameMode::Require(bool Value, const TCHAR* Message)
{
    if (!Value)
    {
        UE_LOG(LogTemp, Error, TEXT("Y01Smoke FAIL Scenario=%s Step=%d Reason=%s"), *Scenario, Step, Message);
        bFinished = true;
        // Automation夹具不结束测试宿主；真实DS失败以非零码退出。
        if (GetNetMode() == NM_DedicatedServer) { FPlatformMisc::RequestExitWithStatus(false, 1); }
    }
    return Value;
}
void AY01SmokeGameMode::Checkpoint(const TCHAR* Name)
{
    CheckpointName = Name; Acknowledged.Reset(); ++Probe->Sequence;
    Expected = AY01SmokeProbe::Observe(GetWorld(), Probe);
    Probe->ForceNetUpdate();
    UE_LOG(LogTemp, Display, TEXT("Y01Smoke EXPECT Scenario=%s Checkpoint=%s %s"), *Scenario, Name, *Expected);
}
void AY01SmokeGameMode::AcceptObservation(AY01SmokeController* PC, int32 Sequence, int32 ObserverId, const FString& Observation)
{
    if (!bEnabled || bFinished || !Probe || Sequence != Probe->Sequence || Observation.Len() > 16384) { return; }
    const auto Connected = Peers();
    if (!Connected.Contains(PC) || !PC->PlayerState || PC->PlayerState->GetPlayerId() != ObserverId || Acknowledged.Contains(ObserverId)) { return; }
    if (Observation != Expected)
    {
        UE_LOG(LogTemp, Display, TEXT("Y01Smoke WAIT_REPLICATION Peer=%d Seq=%d"), ObserverId, Sequence); return;
    }
    Acknowledged.Add(ObserverId);
    UE_LOG(LogTemp, Display, TEXT("Y01Smoke ACK Scenario=%s Peer=%d Connection=%s Checkpoint=%s %s"),
        *Scenario, ObserverId, *PC->GetNetConnection()->GetName(), *CheckpointName, *Observation);
}
void AY01SmokeGameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!bEnabled || !Probe) { return; }
    if (bFinished)
    {
        if (FinishAt > 0 && FPlatformTime::Seconds() >= FinishAt) { FPlatformMisc::RequestExitWithStatus(false, 0); }
        return;
    }
    if (!Require(FPlatformTime::Seconds() - StartedAt < 100, TEXT("双客户端复制ACK超时"))) { return; }
    const auto Connected = Peers();
    const auto& S = GetFlowState()->GetWaveSnapshot();
    if (Step == 0)
    {
        if (Connected.Num() != 2) { return; }
        if (!Require(Connected[0]->PlayerState->GetPlayerId() != Connected[1]->PlayerState->GetPlayerId()
            && Connected[0]->GetNetConnection() != Connected[1]->GetNetConnection(), TEXT("两个唯一真实连接"))) { return; }
        for (auto* PC : Connected)
        {
            auto* PS = PC->GetPlayerState<AY01PlayerState>();
            if (!Require(PS && PS->IsCharacterInitialized() && PS->GetGold() == 0 && PS->GetExperience() == 0
                && PS->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) == 100
                && PS->GetEquipmentSlots().Num() == 6 && PS->GetSkillSlots().Num() == 4, TEXT("初始角色状态来自表"))) { return; }
        }
        Checkpoint(TEXT("Waiting")); Step = 1; return;
    }
    if (Acknowledged.Num() != 2 || Connected.Num() != 2) { return; }
    if (Step == 1)
    {
        if (!Require(TryStartNextWave(), TEXT("Waiting转Preparing"))) { return; }
        Checkpoint(TEXT("Preparing")); Step = 2; return;
    }
    if (Step == 2)
    {
        if (S.Phase != EY01MatchPhase::Battle || S.PendingSpawns != 0) { return; }
        Probe->Monsters = GetWaveMonsters();
        if (!Require(Probe->Monsters.Num() == (S.WaveIndex == 0 ? 2 : 1), TEXT("normal和boss按表Count刷出"))) { return; }
        for (AY01Monster* Monster : Probe->Monsters) { if (!Require(Health(Monster) == 20, TEXT("怪物初始生命20"))) { return; } }
        Checkpoint(TEXT("Battle")); Step = 3; return;
    }
    if (Step == 3)
    {
        if (Scenario == TEXT("Success"))
        {
            for (int32 I = 0; I < Probe->Monsters.Num(); ++I)
            {
                auto* PC = Connected[I % 2]; auto* PS = PC->GetPlayerState<AY01PlayerState>(); auto* Mob = Probe->Monsters[I].Get();
                PC->GetPawn()->SetActorLocation(Mob->GetActorLocation() - FVector(100, 0, 0));
                if (!Require(PS->GetCombatASC()->SetCombatTarget(Mob) && PS->GetCombatASC()->TryActivateBasicAttack() && Health(Mob) == 0, TEXT("玩家真实GA伤害20至0"))) { return; }
                UE_LOG(LogTemp, Display, TEXT("Y01Smoke GA Player=%d Monster=%d Health=20->0"), PS->GetPlayerId(), I);
            }
        }
        else
        {
            auto* Mob = Probe->Monsters[0].Get(); Mob->SetActorLocation(Probe->Core->GetActorLocation() - FVector(100, 0, 0));
            if (!Require(Mob->GetCombatASC()->SetCombatTarget(Probe->Core) && Mob->GetCombatASC()->TryActivateBasicAttack() && Health(Probe->Core) == 0, TEXT("怪物真实GA伤害核心20至0"))) { return; }
            UE_LOG(LogTemp, Display, TEXT("Y01Smoke GA Monster->Core Health=20->0"));
        }
        // 不Destroy死亡Actor，直到所有客户端观察并ACK真实ASC生命。
        Checkpoint(TEXT("Damage")); Step = 4; return;
    }
    if (Step == 4)
    {
        if (Scenario == TEXT("Failure"))
        {
            if (!Require(!TryCompleteWave() && TryEndMatch(false), TEXT("核心归零失败且无成功奖"))) { return; }
            Checkpoint(TEXT("Ended")); Step = 6; return;
        }
        if (!Require(TryCompleteWave() && !TryCompleteWave(), TEXT("成功波奖励幂等"))) { return; }
        for (auto* PC : Connected)
        {
            auto* PS = PC->GetPlayerState<AY01PlayerState>();
            if (!Require(PS->GetGold() == 10 * S.CompletedWaves && PS->GetExperience() == 5 * S.CompletedWaves, TEXT("每人每波10金币5经验"))) { return; }
        }
        Checkpoint(TEXT("Reward")); Step = 5; return;
    }
    if (Step == 5)
    {
        if (S.CompletedWaves == 2)
        {
            if (!Require(TryEndMatch(true), TEXT("最终波结束"))) { return; }
            Checkpoint(TEXT("Ended")); Step = 6;
        }
        else
        {
            if (!Require(TryStartNextWave(), TEXT("进入boss准备"))) { return; }
            Checkpoint(TEXT("Preparing")); Step = 2;
        }
        return;
    }
    if (Step == 6)
    {
        for (auto* PC : Connected)
        {
            auto* PS = PC->GetPlayerState<AY01PlayerState>();
            if (!Require(PS->GetGold() == (Scenario == TEXT("Success") ? 20 : 0)
                && PS->GetExperience() == (Scenario == TEXT("Success") ? 10 : 0), TEXT("终态奖励正确"))) { return; }
        }
        UE_LOG(LogTemp, Display, TEXT("Y01Smoke PASS Scenario=%s Clients=2 LastSequence=%d ACKs=2"), *Scenario, Probe->Sequence);
        bFinished = true; FinishAt = FPlatformTime::Seconds() + 2;
        for (auto* PC : Connected) { PC->ClientSmokeFinished(); }
    }
}
