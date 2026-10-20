#include "Match/Y01GameFlow.h"
#include "Match/Y01KillRewards.h"
#include "Match/Y01SpawnPoint.h"
#include "Combat/Y01Monster.h"
#include "Combat/Y01Core.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01Character.h"
#include "Player/Y01PlayerController.h"
#include "Data/Y01DataSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "TimerManager.h"

AY01GameState::AY01GameState() { SetNetUpdateFrequency(30); }
AY01Core* AY01GameState::GetMatchCore() const { return MatchCore; }
AY01Core* AY01GameMode::GetMatchCore() const { return GetFlowState() ? GetFlowState()->GetMatchCore() : nullptr; }
bool AY01GameMode::RegisterMatchCore(AY01Core* Core)
{
    if (!HasAuthority() || !GetFlowState() || GetFlowState()->MatchCore || !IsValid(Core)
        || Core->GetWorld() != GetWorld() || !Core->GetSnapshot().bInitialized) { return false; }
    GetFlowState()->MatchCore = Core; GetFlowState()->ForceNetUpdate(); return true;
}
void AY01GameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AY01GameState, Snapshot);
    DOREPLIFETIME(AY01GameState, Presentation);
    DOREPLIFETIME(AY01GameState, MatchCore);
}
void AY01GameState::OnRep_Snapshot()
{
    UE_LOG(LogTemp, Display, TEXT("Y01Flow Rep Phase=%d Wave=%d Completed=%d Revision=%d"),
        static_cast<int32>(Snapshot.Phase), Snapshot.WaveIndex, Snapshot.CompletedWaves, Snapshot.Revision);
}
AY01GameMode::AY01GameMode()
{
    PrimaryActorTick.bCanEverTick = true;
    GameStateClass = AY01GameState::StaticClass(); PlayerStateClass = AY01PlayerState::StaticClass();
    DefaultPawnClass = AY01Character::StaticClass(); PlayerControllerClass = AY01PlayerController::StaticClass();
}
void AY01GameMode::BeginPlay()
{
    Super::BeginPlay();
    if (auto* Kills = GetWorld()->GetSubsystem<UY01KillRewards>())
    { KillRewardHandle = Kills->OnEffectiveKill.AddUObject(this, &AY01GameMode::OnMainlineMonsterKill); }
    if (HasAuthority() && !bConfigured && !LoadMatchConfiguration(ConfigurationError))
    { UE_LOG(LogTemp, Error, TEXT("Y01Flow MissingConfiguration Waiting: %s"), *ConfigurationError); }
}
void AY01GameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    UpdateAutomaticWaves();
}
bool AY01GameMode::IsAutomaticPlayerReady(const AY01PlayerState* Player) const
{
    const auto* Pawn = IsValid(Player) ? Cast<APawn>(Player->GetCombatASC()->GetAvatarActor()) : nullptr;
    return IsValid(Player) && Player->IsCharacterInitialized() && !Player->IsActorBeingDestroyed()
        && IsValid(Pawn) && !Pawn->IsActorBeingDestroyed() && Pawn->GetPlayerState() == Player
        && Pawn->GetController() && Pawn->GetController()->GetPlayerState<AY01PlayerState>() == Player
        && Player->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) > 0;
}
void AY01GameMode::UpdateAutomaticWaves()
{
    if (IsGameplayFrozen() || !HasAuthority() || !bConfigured || !bAutomaticWaves || bTransitioning || bUpdatingAutomatic || !GetFlowState()) { return; }
    auto& S = GetFlowState()->Snapshot;
    if (S.Phase == EY01MatchPhase::Ended) { return; }
    TGuardValue<bool> Updating(bUpdatingAutomatic, true);
    const double Now = GetWorld()->GetTimeSeconds();
    if (S.Phase == EY01MatchPhase::Waiting)
    {
        bool bReady = false;
        for (APlayerState* State : GetFlowState()->PlayerArray)
        { bReady |= IsAutomaticPlayerReady(Cast<AY01PlayerState>(State)); }
        if (!bReady || !CanStartWave()) { return; }
        S.Phase = EY01MatchPhase::Preparing; S.ServerPhaseTime = Now;
        S.NextWaveIndex = 0; S.NextWaveDeadline = Now + Waves[0].PreparationSeconds;
        S.PreparationDeadline = S.NextWaveDeadline;
        Publish(); OnMatchPhaseChanged(EY01MatchPhase::Waiting, S.Phase);
        // 首次准备至少发布一帧；零等待也不在初始化hook或阶段回调里递归生成。
        return;
    }
    bool bChanged = false;
    for (int32 Budget = 0; Budget < Waves.Num() && S.Phase != EY01MatchPhase::Ended
        && Waves.IsValidIndex(S.NextWaveIndex) && Now >= S.NextWaveDeadline; ++Budget)
    {
        if (IsGameplayFrozen() || !CanEnterBattle()) { break; }
        const int32 Index = S.NextWaveIndex;
        auto& Progress = S.WaveStates[Index];
        Progress.bStarted = true; Progress.StartTime = Now; Progress.PendingSpawns = Progress.TotalMonsters;
        LastStartedWave = Index;
        S.WaveIndex = Index; S.Wave = WaveRefs[Index]; S.PendingSpawns = Progress.PendingSpawns; S.bWaveCompleted = false;
        S.NextWaveIndex = Waves.IsValidIndex(Index + 1) ? Index + 1 : INDEX_NONE;
        S.NextWaveDeadline = S.NextWaveIndex != INDEX_NONE ? Now + Waves[S.NextWaveIndex].PreparationSeconds : 0;
        bChanged = true;
        if (S.Phase == EY01MatchPhase::Preparing)
        {
            S.Phase = EY01MatchPhase::Battle; S.ServerPhaseTime = Now;
            OnMatchPhaseChanged(EY01MatchPhase::Preparing, EY01MatchPhase::Battle);
        }
    }
    // 各波批次以自己的实际开始时间计时；开始下一波绝不撤销旧波批次。
    struct FDue { int32 Wave; int32 Entry; double At; };
    TArray<FDue> Due;
    for (int32 Index = 0; Index < AutomaticWaves.Num() && S.Phase == EY01MatchPhase::Battle; ++Index)
    {
        if (!S.WaveStates[Index].bStarted || S.WaveStates[Index].bRewarded) { continue; }
        for (int32 EntryIndex = 0; EntryIndex < AutomaticWaves[Index].SpawnedEntries.Num(); ++EntryIndex)
        {
            const auto& Wave = Waves[Index];
            const double Offset = Wave.Schedule == EY01WaveSchedule::Batches ? Wave.Batches[EntryIndex].OffsetSeconds : Wave.Spawns[EntryIndex].DelaySeconds;
            const double At = S.WaveStates[Index].StartTime + Offset;
            if (!AutomaticWaves[Index].SpawnedEntries[EntryIndex] && Now >= At) { Due.Add({Index, EntryIndex, At}); }
        }
    }
    Due.Sort([](const FDue& A, const FDue& B)
    { return A.At != B.At ? A.At < B.At : A.Wave != B.Wave ? A.Wave < B.Wave : A.Entry < B.Entry; });
    for (const auto& Item : Due)
    {
        if (IsGameplayFrozen() || S.Phase != EY01MatchPhase::Battle) { break; }
        AutomaticWaves[Item.Wave].SpawnedEntries[Item.Entry] = true;
        if (Waves[Item.Wave].Schedule == EY01WaveSchedule::Batches) { SpawnBatch(Item.Wave, Item.Entry); }
        else { SpawnEntry(Waves[Item.Wave].Spawns[Item.Entry], Item.Wave); }
        bChanged = true;
    }
    if (RefreshAutomaticSnapshot() || bChanged) { Publish(); }
}
bool AY01GameMode::RefreshAutomaticSnapshot()
{
    auto& S = GetFlowState()->Snapshot;
    if (!bAutomaticWaves) { return false; }
    bool bChanged = false;
    S.TotalStartedMonsters = S.TotalSpawnedMonsters = S.TotalAliveMonsters = S.TotalPendingSpawns = 0;
    for (int32 Index = 0; Index < AutomaticWaves.Num(); ++Index)
    {
        auto& P = S.WaveStates[Index]; int32 Alive = 0;
        for (const auto& M : AutomaticWaves[Index].Monsters)
        { if (M.IsValid() && !M->IsActorBeingDestroyed() && M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) > 0) { ++Alive; } }
        bChanged |= P.AliveMonsters != Alive; P.AliveMonsters = Alive;
        S.TotalStartedMonsters += P.bStarted ? P.TotalMonsters : 0;
        S.TotalSpawnedMonsters += P.SpawnedMonsters; S.TotalAliveMonsters += Alive; S.TotalPendingSpawns += P.PendingSpawns;
    }
    if (S.WaveStates.IsValidIndex(S.WaveIndex))
    { S.PendingSpawns = S.WaveStates[S.WaveIndex].PendingSpawns; S.bWaveCompleted = S.WaveStates[S.WaveIndex].bRewarded; }
    return bChanged;
}
void AY01GameMode::EndPlay(const EEndPlayReason::Type Reason)
{
    if (auto* Kills = GetWorld()->GetSubsystem<UY01KillRewards>()) { Kills->OnEffectiveKill.Remove(KillRewardHandle); }
    CancelTimers(); Super::EndPlay(Reason);
}
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
    const auto Validation = Catalog.Validate();
    if (OrderedWaves.IsEmpty() || Character.Type != EY01TableKind::Character || !Validation.IsValid())
    {
        OutError = TEXT("MissingConfiguration: 有序波次、角色或完整目录无效");
        for (const auto& E : Validation.Errors)
        { OutError += FString::Printf(TEXT(" | Table=%d Row=%s Field=%s: %s"), static_cast<int32>(E.Source.Type), *E.Source.RowName.ToString(), *E.Field, *E.Message); }
        return false;
    }
    FY01DataValidation Errors;
    if (!Catalog.FindRow<FY01CharacterRow>(Character, Errors))
    { OutError = TEXT("MissingConfiguration: 角色行不存在"); return false; }
    const bool bUseAutomatic = UseAutomaticWaves();
    int64 TotalCount = 0;
    TArray<FY01WaveRow> NewWaves; TArray<FY01RewardPackageRow> NewRewards;
    for (const auto& Ref : OrderedWaves)
    {
        const auto* Wave = Catalog.FindRow<FY01WaveRow>(Ref, Errors);
        const auto* Reward = Wave ? Catalog.FindRow<FY01RewardPackageRow>(Wave->Reward, Errors) : nullptr;
        if (!Wave || !Reward || Reward->Domain != EY01RewardDomain::MatchPersonal
            || Reward->Items.ContainsByPredicate([](const FY01RewardEntry& E) { return E.Item.Type != EY01TableKind::Consumable || E.Quantity <= 0; }))
        { OutError = TEXT("MissingConfiguration: 波次或奖励非法；波次奖励物品仅支持数量为正的消耗道具，不能忽略物品"); return false; }
        int64 Count = 0;
        if (Wave->Schedule == EY01WaveSchedule::Batches)
        { for (const auto& Batch : Wave->Batches) { for (const auto& M : Batch.Monsters) { Count += M.Count; } } }
        else { for (const auto& Entry : Wave->Spawns) { Count += Entry.Count; } }
        TotalCount += Count;
        if (Count > MAX_int32 || (bUseAutomatic && TotalCount > MAX_int32)) { OutError = TEXT("波次总刷怪数量溢出"); return false; }
        NewWaves.Add(*Wave); NewRewards.Add(*Reward);
    }
    if (NewWaves.ContainsByPredicate([](const FY01WaveRow& W) { return W.Schedule == EY01WaveSchedule::Batches; }))
    {
        TMap<FName, TWeakObjectPtr<AY01SpawnPoint>> Points; FString RegistryError;
        const bool bRegistry = AY01SpawnPoint::BuildRegistry(GetWorld(), Points, RegistryError);
        for (int32 I = 0; I < NewWaves.Num(); ++I)
        {
            for (const auto& B : NewWaves[I].Batches)
            {
                const auto* Found = Points.Find(B.PointTag); auto* Point = Found ? Found->Get() : nullptr;
                if (!bRegistry || !IsValid(Point) || Point->PointKind != B.PointKind)
                {
                    OutError += FString::Printf(TEXT("Table=Wave Row=%s Batch=%s Point=%s Kind=%d Object=%s: %s | "),
                        *OrderedWaves[I].RowName.ToString(), *B.BatchId.ToString(), *B.PointTag.ToString(), static_cast<int32>(B.PointKind),
                        *GetPathNameSafe(Point), bRegistry ? TEXT("缺失或type mismatch，禁止回退原点") : *RegistryError);
                }
            }
        }
        if (!OutError.IsEmpty()) { return false; }
    }
    MatchCatalog = Catalog; CharacterRef = Character;
    FY01DataValidation PresentationErrors;
    FY01DataRef RulesRef; RulesRef.Type = EY01TableKind::MatchRules; RulesRef.RowName = TEXT("Default");
    if (const auto* Rules = Catalog.FindRow<FY01MatchRulesRow>(RulesRef, PresentationErrors))
    {
        GetFlowState()->Presentation.Values = Rules->Presentation;
        DefaultCritMultiplier = Rules->DefaultCritMultiplier;
    }
    else
    {
        GetFlowState()->Presentation.Values = FY01MatchPresentation();
        DefaultCritMultiplier = 2;
    }
    ++GetFlowState()->Presentation.Revision;
    for (APlayerState* State : GetFlowState()->PlayerArray)
    {
        if (!InitializePlayer(Cast<AY01PlayerState>(State), OutError)) { return false; }
    }
    Waves = MoveTemp(NewWaves); Rewards = MoveTemp(NewRewards); WaveRefs = OrderedWaves;
    bAutomaticWaves = bUseAutomatic;
    auto& S = GetFlowState()->Snapshot;
    S.bAutomaticWaves = bAutomaticWaves; S.TotalWaves = Waves.Num();
    if (bAutomaticWaves)
    {
        S.TotalMonsters = static_cast<int32>(TotalCount);
        AutomaticWaves.SetNum(Waves.Num()); S.WaveStates.SetNum(Waves.Num());
        for (int32 Index = 0; Index < Waves.Num(); ++Index)
        {
            auto& P = S.WaveStates[Index]; P.WaveKey = Index; P.Wave = WaveRefs[Index];
            const auto& W = Waves[Index];
            if (W.Schedule == EY01WaveSchedule::Batches)
            {
                for (const auto& B : W.Batches) { for (const auto& M : B.Monsters) { P.TotalMonsters += M.Count; } }
                AutomaticWaves[Index].SpawnedEntries.Init(false, W.Batches.Num());
            }
            else
            {
                for (const auto& Entry : W.Spawns) { P.TotalMonsters += Entry.Count; }
                AutomaticWaves[Index].SpawnedEntries.Init(false, W.Spawns.Num());
            }
        }
    }
    bConfigured = true; ConfigurationError.Reset(); Publish();
    return true;
}
bool AY01GameMode::InitializePlayer(AY01PlayerState* Player, FString& OutError)
{
    if (!IsValid(Player)) { OutError = TEXT("玩家状态类型无效"); return false; }
    if (!Player->TryInitializeCharacter(MatchCatalog, CharacterRef, OutError)) { return false; }
    OnPlayerInitialized(Player);
    return true;
}
void AY01GameMode::Publish()
{
    ++GetFlowState()->Snapshot.Revision;
    GetFlowState()->ForceNetUpdate();
}
bool AY01GameMode::TryStartNextWave()
{
    if (IsGameplayFrozen() || !HasAuthority() || !bConfigured || bAutomaticWaves || bTransitioning || !GetFlowState()) { return false; }
    auto& S = GetFlowState()->Snapshot;
    if (!(S.Phase == EY01MatchPhase::Waiting || (S.Phase == EY01MatchPhase::Battle && S.bWaveCompleted))
        || !Waves.IsValidIndex(S.WaveIndex + 1)) { return false; }
    TGuardValue<bool> Guard(bTransitioning, true);
    if (!CanStartWave()) { return false; }
    CancelTimers(); WaveMonsters.Reset();
    const EY01MatchPhase Previous = S.Phase;
    ++S.WaveIndex; S.Wave = WaveRefs[S.WaveIndex]; S.bWaveCompleted = false;
    S.Phase = EY01MatchPhase::Preparing; S.ServerPhaseTime = GetWorld()->GetTimeSeconds();
    S.PreparationDeadline = S.ServerPhaseTime + Waves[S.WaveIndex].PreparationSeconds;
    S.PendingSpawns = 0;
    const auto& W = Waves[S.WaveIndex];
    if (W.Schedule == EY01WaveSchedule::Batches)
    { for (const auto& B : W.Batches) { for (const auto& M : B.Monsters) { S.PendingSpawns += M.Count; } } }
    else { for (const auto& Entry : W.Spawns) { S.PendingSpawns += Entry.Count; } }
    Publish();
    GetWorldTimerManager().SetTimer(PreparationTimer, this, &AY01GameMode::EnterBattle,
        FMath::Max(Waves[S.WaveIndex].PreparationSeconds, 0.001f), false);
    OnMatchPhaseChanged(Previous, S.Phase);
    return true;
}
void AY01GameMode::EnterBattle()
{
    if (IsGameplayFrozen() || !HasAuthority() || !GetFlowState() || GetFlowState()->Snapshot.Phase != EY01MatchPhase::Preparing) { return; }
    if (bTransitioning || !CanEnterBattle())
    { GetWorldTimerManager().SetTimer(PreparationTimer, this, &AY01GameMode::EnterBattle, 0.05f, false); return; }
    auto& S = GetFlowState()->Snapshot;
    const EY01MatchPhase Previous = S.Phase;
    LastStartedWave = S.WaveIndex;
    S.Phase = EY01MatchPhase::Battle; S.ServerPhaseTime = GetWorld()->GetTimeSeconds(); Publish();
    OnMatchPhaseChanged(Previous, S.Phase);
    const int32 Index = S.WaveIndex;
    if (Waves[Index].Schedule == EY01WaveSchedule::Batches)
    {
        TArray<int32> Order;
        for (int32 I = 0; I < Waves[Index].Batches.Num(); ++I) { Order.Add(I); }
        Order.StableSort([&](int32 A, int32 B) { return Waves[Index].Batches[A].OffsetSeconds < Waves[Index].Batches[B].OffsetSeconds; });
        for (int32 I : Order)
        {
            if (IsGameplayFrozen() || S.Phase != EY01MatchPhase::Battle) { break; }
            const double Offset = Waves[Index].Batches[I].OffsetSeconds;
            if (Offset == 0) { SpawnBatch(Index, I); }
            else
            {
                FTimerHandle Handle;
                GetWorldTimerManager().SetTimer(Handle, FTimerDelegate::CreateUObject(this, &AY01GameMode::SpawnBatch, Index, I), Offset, false);
                SpawnTimers.Add(Handle);
            }
        }
        return;
    }
    for (const auto& Entry : Waves[Index].Spawns)
    {
        if (IsGameplayFrozen() || S.Phase != EY01MatchPhase::Battle) { break; }
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
    if (IsGameplayFrozen() || !HasAuthority() || !GetFlowState()) { return; }
    auto& S = GetFlowState()->Snapshot;
    if (IsGameplayFrozen() || S.Phase != EY01MatchPhase::Battle) { return; }
    if (bAutomaticWaves)
    {
        if (!S.WaveStates.IsValidIndex(WaveIndex) || !S.WaveStates[WaveIndex].bStarted || S.WaveStates[WaveIndex].bRewarded) { return; }
    }
    else if (S.WaveIndex != WaveIndex || S.bWaveCompleted) { return; }
    for (int32 I = 0; I < Entry.Count; ++I)
    {
        FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Monster = GetWorld()->SpawnActor<AY01Monster>(AY01Monster::StaticClass(), GetMonsterSpawnTransform(WaveMonsters.Num()), Params);
        FString Error;
        if (!Monster || !Monster->TryInitializeMonster(MatchCatalog, Entry.Monster, Error))
        {
            if (Monster) { Monster->Destroy(); }
            FailSpawn(FString::Printf(TEXT("Table=Wave Row=%s Monster=%s Legacy: %s"), *WaveRefs[WaveIndex].RowName.ToString(), *Entry.Monster.RowName.ToString(), *Error)); return;
        }
        RegisterWaveMonster(Monster, WaveIndex);
        if (IsGameplayFrozen() || S.Phase != EY01MatchPhase::Battle) { return; }
    }
    Publish();
}
void AY01GameMode::FailSpawn(const FString& Error)
{
    ConfigurationError = Error;
    UE_LOG(LogTemp, Error, TEXT("Y01Flow SpawnFailure: %s"), *ConfigurationError);
    TryEndMatch(false);
}
void AY01GameMode::RegisterWaveMonster(AY01Monster* Monster, int32 WaveIndex)
{
    auto& S = GetFlowState()->Snapshot; WaveMonsters.Add(Monster);
    if (bAutomaticWaves)
    {
        AutomaticWaves[WaveIndex].Monsters.Add(Monster);
        --S.WaveStates[WaveIndex].PendingSpawns; ++S.WaveStates[WaveIndex].SpawnedMonsters;
        RefreshAutomaticSnapshot();
    }
    else { --S.PendingSpawns; }
    OnWaveMonsterSpawned(Monster);
    GetWorld()->GetSubsystem<UY01KillRewards>()->RegisterMonster(Monster);
}
void AY01GameMode::SpawnBatch(int32 WaveIndex, int32 BatchIndex)
{
    if (IsGameplayFrozen() || !HasAuthority() || !GetFlowState() || !Waves.IsValidIndex(WaveIndex) || !Waves[WaveIndex].Batches.IsValidIndex(BatchIndex)) { return; }
    auto& S = GetFlowState()->Snapshot;
    if (S.Phase != EY01MatchPhase::Battle || (bAutomaticWaves ? !S.WaveStates[WaveIndex].bStarted || S.WaveStates[WaveIndex].bRewarded
        : S.WaveIndex != WaveIndex || S.bWaveCompleted)) { return; }
    const auto& B = Waves[WaveIndex].Batches[BatchIndex];
    const FString Context = FString::Printf(TEXT("Table=Wave Row=%s WaveKey=%d Batch=%s Offset=%.17g Point=%s Kind=%d"),
        *WaveRefs[WaveIndex].RowName.ToString(), WaveIndex, *B.BatchId.ToString(), B.OffsetSeconds, *B.PointTag.ToString(), static_cast<int32>(B.PointKind));
    TMap<FName, TWeakObjectPtr<AY01SpawnPoint>> Points; FString Error;
    if (!AY01SpawnPoint::BuildRegistry(GetWorld(), Points, Error)) { FailSpawn(Context + TEXT(" ") + Error); return; }
    auto* Point = Points.FindRef(B.PointTag).Get();
    if (!IsValid(Point) || Point->IsActorBeingDestroyed() || Point->PointKind != B.PointKind)
    { FailSpawn(Context + FString::Printf(TEXT(" Object=%s 缺失或type mismatch"), *GetPathNameSafe(Point))); return; }
    int32 Cursor = 0;
    const double Start = bAutomaticWaves ? S.WaveStates[WaveIndex].StartTime : S.ServerPhaseTime;
    for (const auto& M : B.Monsters)
    {
        for (int32 I = 0; I < M.Count; ++I)
        {
            FTransform Transform;
            const FString Item = Context + FString::Printf(TEXT(" Monster=%s Count=%d Index=%d Object=%s"),
                *M.Monster.RowName.ToString(), M.Count, I, *GetPathNameSafe(Point));
            if (!Point->TryFindMonsterTransform(Cursor, Transform, Error)) { FailSpawn(Item + TEXT(" ") + Error); return; }
            FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::DontSpawnIfColliding;
            auto* Monster = GetWorld()->SpawnActor<AY01Monster>(AY01Monster::StaticClass(), Transform, Params);
            if (!Monster || !Monster->TryInitializeMonster(MatchCatalog, M.Monster, Error))
            {
                if (Monster) { Monster->Destroy(); }
                FailSpawn(Item + TEXT(" SpawnActor/初始化失败: ") + Error); return;
            }
            UE_LOG(LogTemp, Display, TEXT("Y01Flow BATCH %s Start=%.17g Actual=%.17g Elapsed=%.17g Position=%s Actor=%s"),
                *Item, Start, GetWorld()->GetTimeSeconds(), GetWorld()->GetTimeSeconds() - Start, *Transform.GetLocation().ToString(), *Monster->GetName());
            RegisterWaveMonster(Monster, WaveIndex);
            if (IsGameplayFrozen() || S.Phase != EY01MatchPhase::Battle) { return; }
        }
    }
    Publish();
}
bool AY01GameMode::TryCompleteWave()
{
    if (IsGameplayFrozen() || !HasAuthority() || !bConfigured || bTransitioning || bUpdatingAutomatic || !GetFlowState()) { return false; }
    auto& S = GetFlowState()->Snapshot;
    if (bAutomaticWaves)
    {
        if (S.Phase != EY01MatchPhase::Battle) { return false; }
        TGuardValue<bool> Guard(bTransitioning, true);
        bool bCommitted = false; const bool bChanged = RefreshAutomaticSnapshot();
        for (int32 Index = 0; Index < AutomaticWaves.Num(); ++Index)
        {
            const auto& P = S.WaveStates[Index];
            if (P.bStarted && !P.bRewarded && P.PendingSpawns == 0 && P.AliveMonsters == 0)
            { bCommitted |= CommitWaveReward(Index); }
        }
        if (bChanged) { Publish(); }
        return bCommitted;
    }
    if (S.Phase != EY01MatchPhase::Battle || S.PendingSpawns != 0 || S.bWaveCompleted || RewardedWaveKeys.Contains(S.WaveIndex)) { return false; }
    TGuardValue<bool> Guard(bTransitioning, true);
    if (!CanCompleteWave()) { return false; }
    return CommitWaveReward(S.WaveIndex);
}
bool AY01GameMode::CommitWaveReward(int32 WaveIndex)
{
    auto& S = GetFlowState()->Snapshot;
    if (IsGameplayFrozen() || !Rewards.IsValidIndex(WaveIndex) || RewardedWaveKeys.Contains(WaveIndex)) { return false; }
    const auto& Reward = Rewards[WaveIndex];
    TArray<AY01PlayerState*> Recipients;
    TArray<double> GoldAmounts;
    for (APlayerState* State : GetFlowState()->PlayerArray)
    {
        auto* Player = Cast<AY01PlayerState>(State);
        if (!IsValid(Player) || !Player->IsCharacterInitialized()) { continue; }
        FY01ResolvedAttributes Resolved; double Gain = 0;
        if (Player->GetSourceEffects()->ObserveAttributes(Resolved).IsOk()) { Gain = Resolved.GoldGainPercent; }
        const double Gold = Reward.Gold * (1. + Gain);
        // 经验仍走整数预检；金币经小数钱包（floor 部分容量并入校验）。
        if (!Player->CanAddMatchReward(0, Reward.Experience)) { return false; }
        if (FMath::FloorToDouble(Gold + Player->GetRewardRemainder()) > static_cast<double>(MAX_int32) - Player->GetGold()) { return false; }
        Recipients.Add(Player); GoldAmounts.Add(Gold);
    }
    // 服务器局内单调波次号作为幂等key；没有客户端key入口。预校验到提交之间不调用外部政策或委托。
    RewardedWaveKeys.Add(WaveIndex);
    for (int32 I = 0; I < Recipients.Num(); ++I)
    {
        const bool bApplied = Recipients[I]->TryAddFractionalGold(GoldAmounts[I])
            && Recipients[I]->TryAddMatchReward(0, Reward.Experience);
        check(bApplied);
        auto* Loadout = Recipients[I]->GetLoadout();
        if (Reward.Items.IsEmpty() || !Loadout || !Loadout->IsInitialized()) { continue; }
        TArray<FY01ItemGrant> Grants;
        for (const auto& Entry : Reward.Items)
        { for (int32 Q = 0; Q < Entry.Quantity; ++Q) { Grants.Add({FGuid::NewGuid(), Entry.Item}); } }
        // 金币经验已结清；道具入包失败（如背包满）只记录，不回滚波次奖励。
        const auto ItemResult = Loadout->AddItems(MatchCatalog, Grants);
        if (!ItemResult.IsOk())
        { UE_LOG(LogTemp, Warning, TEXT("Y01Flow WAVE_ITEM_REJECT Player=%s Wave=%d: %s"), *Recipients[I]->GetName(), WaveIndex, *ItemResult.Message); }
    }
    if (bAutomaticWaves) { S.WaveStates[WaveIndex].bRewarded = true; RefreshAutomaticSnapshot(); }
    else { S.bWaveCompleted = true; }
    ++S.CompletedWaves; Publish();
    OnWaveRewardCommitted();
    return true;
}
bool AY01GameMode::ResolveReviveRule(int32& OutWaveIndex, double& OutWaitSeconds) const
{
    OutWaveIndex = LastStartedWave == INDEX_NONE ? 0 : LastStartedWave;
    OutWaitSeconds = 0;
    if (!HasAuthority() || !bConfigured || !Waves.IsValidIndex(OutWaveIndex)) { return false; }
    OutWaitSeconds = Waves[OutWaveIndex].RespawnSeconds;
    return FMath::IsFinite(OutWaitSeconds) && OutWaitSeconds > 0;
}
bool AY01GameMode::TryEndMatch(bool bSuccess)
{
    if (!HasAuthority() || !bConfigured || bTransitioning || !GetFlowState()) { return false; }
    auto& S = GetFlowState()->Snapshot;
    if ((S.Phase == EY01MatchPhase::Waiting && !(IsCoreDefenseEnabled() && IsGameplayFrozen() && !bSuccess))
        || S.Phase == EY01MatchPhase::Ended || (bSuccess && !S.bWaveCompleted)) { return false; }
    if (bAutomaticWaves && bSuccess)
    {
        RefreshAutomaticSnapshot();
        if (S.CompletedWaves != Waves.Num() || S.WaveIndex != Waves.Num() - 1
            || S.TotalAliveMonsters != 0 || S.TotalPendingSpawns != 0) { return false; }
    }
    TGuardValue<bool> Guard(bTransitioning, true);
    const EY01MatchPhase Previous = S.Phase;
    CancelTimers(); S.PendingSpawns = 0; S.Phase = EY01MatchPhase::Ended;
    S.NextWaveIndex = INDEX_NONE; S.NextWaveDeadline = 0;
    if (bAutomaticWaves)
    {
        for (auto& P : S.WaveStates) { P.PendingSpawns = 0; }
        RefreshAutomaticSnapshot();
    }
    S.Result = bSuccess ? EY01MatchResult::Success : EY01MatchResult::Failure;
    S.ServerPhaseTime = GetWorld()->GetTimeSeconds(); Publish();
    OnMatchPhaseChanged(Previous, S.Phase);
    return true;
}
void AY01GameMode::CancelTimers()
{
    GetWorldTimerManager().ClearTimer(PreparationTimer);
    for (auto& Handle : SpawnTimers) { GetWorldTimerManager().ClearTimer(Handle); }
    SpawnTimers.Reset();
}
// R37第四期：主线怪死亡金币在局玩家均分；金币获取%逐人放大，小数经Fractional钱包入账。
void AY01GameMode::OnMainlineMonsterKill(const FY01KillEvent& Event)
{
    auto* Monster = Event.Monster.Get();
    if (!HasAuthority() || !bConfigured || !Monster || !GetWaveMonsters().Contains(Monster)) { return; }
    FY01DataValidation Query;
    const auto* Row = MatchCatalog.FindRow<FY01MonsterRow>(Event.Definition, Query);
    if (!Row || Row->Gold <= 0) { return; }
    TArray<AY01PlayerState*> Present;
    for (APlayerState* State : GetFlowState()->PlayerArray)
    {
        auto* P = Cast<AY01PlayerState>(State);
        if (!IsValid(P) || !P->IsCharacterInitialized()) { continue; }
        const auto* PC = Cast<APlayerController>(P->GetOwner());
        if (!PC || !IsValid(PC)) { continue; }   // 断线不含；死亡等待与练功房玩家仍在局
        Present.Add(P);
    }
    if (Present.IsEmpty()) { return; }
    const double Share = static_cast<double>(Row->Gold) / Present.Num();
    for (auto* P : Present)
    {
        FY01ResolvedAttributes Resolved; double Gain = 0;
        if (P->GetSourceEffects()->ObserveAttributes(Resolved).IsOk()) { Gain = Resolved.GoldGainPercent; }
        else { UE_LOG(LogTemp, Display, TEXT("Y01R37 GOLD_SPLIT_OBSERVE_FAIL Player=%s"), *P->GetName()); }
        const double Amount = Share * (1. + Gain);
        if (!P->TryAddFractionalGold(Amount))
        { UE_LOG(LogTemp, Display, TEXT("Y01R37 GOLD_SPLIT_REJECT Player=%s Gold=%d Share=%.17g"), *P->GetName(), Row->Gold, Amount); }
    }
}
