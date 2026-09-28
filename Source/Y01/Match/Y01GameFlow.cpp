#include "Match/Y01GameFlow.h"
#include "Combat/Y01Monster.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01Character.h"
#include "Player/Y01PlayerController.h"
#include "Data/Y01DataSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

AY01GameState::AY01GameState() { SetNetUpdateFrequency(30); }
void AY01GameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AY01GameState, Snapshot);
}
void AY01GameState::OnRep_Snapshot()
{
    UE_LOG(LogTemp, Display, TEXT("Y01Flow Rep Phase=%d Wave=%d Completed=%d Revision=%d"),
        static_cast<int32>(Snapshot.Phase), Snapshot.WaveIndex, Snapshot.CompletedWaves, Snapshot.Revision);
}
AY01GameMode::AY01GameMode()
{
    GameStateClass = AY01GameState::StaticClass(); PlayerStateClass = AY01PlayerState::StaticClass();
    DefaultPawnClass = AY01Character::StaticClass(); PlayerControllerClass = AY01PlayerController::StaticClass();
}
void AY01GameMode::BeginPlay()
{
    Super::BeginPlay();
    if (HasAuthority() && !bConfigured && !LoadMatchConfiguration(ConfigurationError))
    { UE_LOG(LogTemp, Error, TEXT("Y01Flow MissingConfiguration Waiting: %s"), *ConfigurationError); }
}
void AY01GameMode::EndPlay(const EEndPlayReason::Type Reason)
{ CancelTimers(); Super::EndPlay(Reason); }
void AY01GameMode::PostLogin(APlayerController* NewPlayer)
{
    Super::PostLogin(NewPlayer);
    if (bConfigured)
    {
        FString Error;
        if (!InitializePlayer(NewPlayer->GetPlayerState<AY01PlayerState>(), Error))
        { UE_LOG(LogTemp, Error, TEXT("Y01Flow PlayerInitialization: %s"), *Error); TryEndMatch(false); }
    }
}
bool AY01GameMode::LoadMatchConfiguration(FString& OutError)
{
    const auto* Data = GetGameInstance() ? GetGameInstance()->GetSubsystem<UY01DataSubsystem>() : nullptr;
    if (!Data || !Data->GetCatalog()) { OutError = TEXT("MissingConfiguration: 数据目录未就绪"); return false; }
    const auto* Settings = GetDefault<UY01DataSettings>();
    return TryConfigureMatch(*Data->GetCatalog(), Settings->OrderedWaves, Settings->MatchCharacter, OutError);
}
bool AY01GameMode::TryConfigureMatch(const FY01DataCatalog& Catalog, const TArray<FY01DataRef>& OrderedWaves,
    const FY01DataRef& Character, FString& OutError)
{
    OutError.Reset();
    if (!HasAuthority() || bTransitioning || bConfigured || !GetFlowState() || GetFlowState()->Snapshot.Phase != EY01MatchPhase::Waiting)
    { OutError = TEXT("配置需要未配置的权威Waiting状态"); return false; }
    TGuardValue<bool> Guard(bTransitioning, true);
    if (OrderedWaves.IsEmpty() || Character.Type != EY01TableKind::Character || !Catalog.Validate().IsValid())
    { OutError = TEXT("MissingConfiguration: 有序波次、角色或完整目录无效"); return false; }
    FY01DataValidation Errors;
    if (!Catalog.FindRow<FY01CharacterRow>(Character, Errors))
    { OutError = TEXT("MissingConfiguration: 角色行不存在"); return false; }
    TArray<FY01WaveRow> NewWaves; TArray<FY01RewardPackageRow> NewRewards;
    for (const auto& Ref : OrderedWaves)
    {
        const auto* Wave = Catalog.FindRow<FY01WaveRow>(Ref, Errors);
        const auto* Reward = Wave ? Catalog.FindRow<FY01RewardPackageRow>(Wave->Reward, Errors) : nullptr;
        if (!Wave || !Reward || Reward->Domain != EY01RewardDomain::MatchPersonal || !Reward->Items.IsEmpty())
        { OutError = TEXT("MissingConfiguration: 波次或奖励非法；当前奖励提交仅支持金币与经验，不能忽略物品"); return false; }
        int64 Count = 0;
        for (const auto& Entry : Wave->Spawns) { Count += Entry.Count; }
        if (Count > MAX_int32) { OutError = TEXT("波次总刷怪数量溢出"); return false; }
        NewWaves.Add(*Wave); NewRewards.Add(*Reward);
    }
    MatchCatalog = Catalog; CharacterRef = Character;
    for (APlayerState* State : GetFlowState()->PlayerArray)
    {
        if (!InitializePlayer(Cast<AY01PlayerState>(State), OutError)) { return false; }
    }
    Waves = MoveTemp(NewWaves); Rewards = MoveTemp(NewRewards); WaveRefs = OrderedWaves;
    bConfigured = true; ConfigurationError.Reset();
    return true;
}
bool AY01GameMode::InitializePlayer(AY01PlayerState* Player, FString& OutError)
{
    if (!IsValid(Player)) { OutError = TEXT("玩家状态类型无效"); return false; }
    return Player->TryInitializeCharacter(MatchCatalog, CharacterRef, OutError);
}
void AY01GameMode::Publish()
{
    ++GetFlowState()->Snapshot.Revision;
    GetFlowState()->ForceNetUpdate();
}
bool AY01GameMode::TryStartNextWave()
{
    if (!HasAuthority() || !bConfigured || bTransitioning || !GetFlowState()) { return false; }
    auto& S = GetFlowState()->Snapshot;
    if (!(S.Phase == EY01MatchPhase::Waiting || (S.Phase == EY01MatchPhase::Battle && S.bWaveCompleted))
        || !Waves.IsValidIndex(S.WaveIndex + 1)) { return false; }
    TGuardValue<bool> Guard(bTransitioning, true);
    if (!CanStartWave()) { return false; }
    CancelTimers(); WaveMonsters.Reset();
    ++S.WaveIndex; S.Wave = WaveRefs[S.WaveIndex]; S.bWaveCompleted = false;
    S.Phase = EY01MatchPhase::Preparing; S.ServerPhaseTime = GetWorld()->GetTimeSeconds();
    S.PreparationDeadline = S.ServerPhaseTime + Waves[S.WaveIndex].PreparationSeconds;
    S.PendingSpawns = 0;
    for (const auto& Entry : Waves[S.WaveIndex].Spawns) { S.PendingSpawns += Entry.Count; }
    Publish();
    GetWorldTimerManager().SetTimer(PreparationTimer, this, &AY01GameMode::EnterBattle,
        FMath::Max(Waves[S.WaveIndex].PreparationSeconds, 0.001f), false);
    return true;
}
void AY01GameMode::EnterBattle()
{
    if (!HasAuthority() || !GetFlowState() || GetFlowState()->Snapshot.Phase != EY01MatchPhase::Preparing) { return; }
    if (bTransitioning || !CanEnterBattle())
    { GetWorldTimerManager().SetTimer(PreparationTimer, this, &AY01GameMode::EnterBattle, 0.05f, false); return; }
    auto& S = GetFlowState()->Snapshot;
    S.Phase = EY01MatchPhase::Battle; S.ServerPhaseTime = GetWorld()->GetTimeSeconds(); Publish();
    const int32 Index = S.WaveIndex;
    for (const auto& Entry : Waves[Index].Spawns)
    {
        if (S.Phase != EY01MatchPhase::Battle) { break; }
        if (Entry.DelaySeconds == 0) { SpawnEntry(Entry, Index); }
        else
        {
            FTimerHandle Handle;
            GetWorldTimerManager().SetTimer(Handle, FTimerDelegate::CreateUObject(this, &AY01GameMode::SpawnEntry, Entry, Index), Entry.DelaySeconds, false);
            SpawnTimers.Add(Handle);
        }
    }
}
FTransform AY01GameMode::GetMonsterSpawnTransform(int32) const { return FTransform::Identity; }
void AY01GameMode::SpawnEntry(FY01SpawnEntry Entry, int32 WaveIndex)
{
    if (!HasAuthority() || !GetFlowState()) { return; }
    auto& S = GetFlowState()->Snapshot;
    if (S.Phase != EY01MatchPhase::Battle || S.WaveIndex != WaveIndex || S.bWaveCompleted) { return; }
    for (int32 I = 0; I < Entry.Count; ++I)
    {
        FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Monster = GetWorld()->SpawnActor<AY01Monster>(AY01Monster::StaticClass(), GetMonsterSpawnTransform(WaveMonsters.Num()), Params);
        FString Error;
        if (!Monster || !Monster->TryInitializeMonster(MatchCatalog, Entry.Monster, Error))
        {
            if (Monster) { Monster->Destroy(); }
            UE_LOG(LogTemp, Error, TEXT("Y01Flow SpawnFailure: %s"), *Error); TryEndMatch(false); return;
        }
        WaveMonsters.Add(Monster); --S.PendingSpawns;
        OnWaveMonsterSpawned(Monster);
        if (S.Phase != EY01MatchPhase::Battle) { return; }
    }
    Publish();
}
bool AY01GameMode::TryCompleteWave()
{
    if (!HasAuthority() || !bConfigured || bTransitioning || !GetFlowState()) { return false; }
    auto& S = GetFlowState()->Snapshot;
    if (S.Phase != EY01MatchPhase::Battle || S.PendingSpawns != 0 || S.bWaveCompleted || RewardedWaveKeys.Contains(S.WaveIndex)) { return false; }
    TGuardValue<bool> Guard(bTransitioning, true);
    if (!CanCompleteWave()) { return false; }
    const auto& Reward = Rewards[S.WaveIndex];
    TArray<AY01PlayerState*> Recipients;
    for (APlayerState* State : GetFlowState()->PlayerArray)
    {
        auto* Player = Cast<AY01PlayerState>(State);
        if (!IsValid(Player) || !Player->IsCharacterInitialized() || !Player->CanAddMatchReward(Reward.Gold, Reward.Experience)) { return false; }
        Recipients.AddUnique(Player);
    }
    // 服务器局内单调波次号作为幂等key；没有客户端key入口。预校验到提交之间不调用外部政策或委托。
    RewardedWaveKeys.Add(S.WaveIndex);
    for (auto* Player : Recipients) { const bool bApplied = Player->TryAddMatchReward(Reward.Gold, Reward.Experience); check(bApplied); }
    S.bWaveCompleted = true; ++S.CompletedWaves; Publish();
    OnWaveRewardCommitted();
    return true;
}
bool AY01GameMode::TryEndMatch(bool bSuccess)
{
    if (!HasAuthority() || !bConfigured || bTransitioning || !GetFlowState()) { return false; }
    auto& S = GetFlowState()->Snapshot;
    if (S.Phase == EY01MatchPhase::Waiting || S.Phase == EY01MatchPhase::Ended || (bSuccess && !S.bWaveCompleted)) { return false; }
    TGuardValue<bool> Guard(bTransitioning, true);
    CancelTimers(); S.PendingSpawns = 0; S.Phase = EY01MatchPhase::Ended;
    S.Result = bSuccess ? EY01MatchResult::Success : EY01MatchResult::Failure;
    S.ServerPhaseTime = GetWorld()->GetTimeSeconds(); Publish();
    return true;
}
void AY01GameMode::CancelTimers()
{
    GetWorldTimerManager().ClearTimer(PreparationTimer);
    for (auto& Handle : SpawnTimers) { GetWorldTimerManager().ClearTimer(Handle); }
    SpawnTimers.Reset();
}
