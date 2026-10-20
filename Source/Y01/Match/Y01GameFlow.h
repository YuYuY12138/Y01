#pragma once
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "Data/Y01DataCatalog.h"
#include "Y01GameFlow.generated.h"

class AY01Monster;
class AY01Core;
class AY01PlayerState;
struct FY01KillEvent;
UENUM()
enum class EY01MatchPhase : uint8 { Waiting, Preparing, Battle, Ended };
UENUM()
enum class EY01MatchResult : uint8 { None, Success, Failure };

USTRUCT()
struct Y01_API FY01WaveProgress
{
    GENERATED_BODY()
    UPROPERTY() int32 WaveKey = INDEX_NONE;
    UPROPERTY() FY01DataRef Wave;
    UPROPERTY() bool bStarted = false;
    // 权威世界游戏时间；-1表示尚未开始生成，不是准备开始时间。
    UPROPERTY() double StartTime = -1;
    UPROPERTY() int32 TotalMonsters = 0;
    UPROPERTY() int32 SpawnedMonsters = 0;
    UPROPERTY() int32 AliveMonsters = 0;
    UPROPERTY() int32 PendingSpawns = 0;
    UPROPERTY() bool bRewarded = false;
};

USTRUCT()
struct Y01_API FY01WaveSnapshot
{
    GENERATED_BODY()
    UPROPERTY() EY01MatchPhase Phase = EY01MatchPhase::Waiting;
    UPROPERTY() EY01MatchResult Result = EY01MatchResult::None;
    UPROPERTY() int32 WaveIndex = INDEX_NONE;
    UPROPERTY() FY01DataRef Wave;
    UPROPERTY() double ServerPhaseTime = 0;
    UPROPERTY() double PreparationDeadline = 0;
    UPROPERTY() int32 CompletedWaves = 0;
    UPROPERTY() int32 PendingSpawns = 0;
    UPROPERTY() bool bWaveCompleted = false;
    UPROPERTY() bool bAutomaticWaves = false;
    UPROPERTY() int32 NextWaveIndex = INDEX_NONE;
    // 与ServerPhaseTime同为服务器世界游戏秒；无下一波时为0，须同时判断NextWaveIndex。
    UPROPERTY() double NextWaveDeadline = 0;
    UPROPERTY() int32 TotalWaves = 0;
    UPROPERTY() TArray<FY01WaveProgress> WaveStates;
    UPROPERTY() int32 TotalMonsters = 0;
    UPROPERTY() int32 TotalStartedMonsters = 0;
    UPROPERTY() int32 TotalSpawnedMonsters = 0;
    UPROPERTY() int32 TotalAliveMonsters = 0;
    UPROPERTY() int32 TotalPendingSpawns = 0;
    UPROPERTY() int32 Revision = 0;
};

USTRUCT()
struct Y01_API FY01ReplicatedPresentation
{
    GENERATED_BODY()
    UPROPERTY() FY01MatchPresentation Values;
    UPROPERTY() int32 Revision = 0;
};

UCLASS()
class Y01_API AY01GameState : public AGameStateBase
{
    GENERATED_BODY()
public:
    AY01GameState();
    const FY01WaveSnapshot& GetWaveSnapshot() const { return Snapshot; }
    const FY01ReplicatedPresentation& GetPresentation() const { return Presentation; }
    AY01Core* GetMatchCore() const;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
private:
    friend class AY01GameMode;
    UPROPERTY(ReplicatedUsing=OnRep_Snapshot) FY01WaveSnapshot Snapshot;
    UPROPERTY(Replicated) FY01ReplicatedPresentation Presentation;
    UPROPERTY(Replicated) TObjectPtr<AY01Core> MatchCore;
    UFUNCTION() void OnRep_Snapshot();
};

UCLASS()
class Y01_API AY01GameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AY01GameMode();
    virtual void BeginPlay() override;
    virtual void Tick(float DeltaSeconds) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void PostLogin(APlayerController* NewPlayer) override;
    bool TryConfigureMatch(const FY01DataCatalog& Catalog, const TArray<FY01DataRef>& OrderedWaves,
        const FY01DataRef& Character, FString& OutError);
    bool TryStartNextWave();
    bool TryCompleteWave();
    virtual bool TryEndMatch(bool bSuccess);
    bool IsMatchConfigured() const { return bConfigured; }
    bool IsAutomaticWaves() const { return bAutomaticWaves; }
    virtual bool IsGameplayFrozen() const { return false; }
    virtual bool IsCoreDefenseEnabled() const { return false; }
    bool ResolveReviveRule(int32& OutWaveIndex, double& OutWaitSeconds) const;
    virtual void OnPlayerDeath(AY01PlayerState* Player) {}
    AY01Core* GetMatchCore() const;
    const FString& GetConfigurationError() const { return ConfigurationError; }
    const TArray<TObjectPtr<AY01Monster>>& GetWaveMonsters() const { return WaveMonsters; }
    AY01GameState* GetFlowState() const { return GetGameState<AY01GameState>(); }
    // 暴击基础倍率来自MatchRules表；未读到行时保持2。
    double GetDefaultCritMultiplier() const { return DefaultCritMultiplier; }
protected:
    bool RegisterMatchCore(AY01Core* Core);
    virtual bool LoadMatchConfiguration(FString& OutError);
    virtual bool UseAutomaticWaves() const { return false; }
    virtual bool IsAutomaticPlayerReady(const AY01PlayerState* Player) const;
    virtual bool CanStartWave() const { return true; }
    virtual bool CanCompleteWave() const { return true; }
    virtual bool CanEnterBattle() const { return true; }
    virtual FTransform GetMonsterSpawnTransform(int32 SpawnOrdinal) const;
    virtual void OnWaveMonsterSpawned(AY01Monster* Monster) {}
    virtual void OnWaveRewardCommitted() {}
    virtual void OnPlayerInitialized(AY01PlayerState* Player) {}
    virtual void OnMatchPhaseChanged(EY01MatchPhase Previous, EY01MatchPhase Current) {}
private:
    void UpdateAutomaticWaves();
    bool RefreshAutomaticSnapshot();
    bool CommitWaveReward(int32 WaveIndex);
    void EnterBattle();
    void SpawnEntry(FY01SpawnEntry Entry, int32 WaveIndex);
    void SpawnBatch(int32 WaveIndex, int32 BatchIndex);
    void RegisterWaveMonster(AY01Monster* Monster, int32 WaveIndex);
    void FailSpawn(const FString& Error);
    void Publish();
    void CancelTimers();
    // R37第四期：主线怪死亡金币在局玩家均分；非主线怪或无效行静默忽略。
    void OnMainlineMonsterKill(const FY01KillEvent& Event);
    bool InitializePlayer(AY01PlayerState* Player, FString& OutError);
    UPROPERTY() TArray<FY01WaveRow> Waves;
    UPROPERTY() TArray<FY01RewardPackageRow> Rewards;
    UPROPERTY() TArray<FY01DataRef> WaveRefs;
    UPROPERTY() FY01DataRef CharacterRef;
    UPROPERTY() TArray<TObjectPtr<AY01Monster>> WaveMonsters;
    FY01DataCatalog MatchCatalog;
    TArray<FTimerHandle> SpawnTimers;
    FTimerHandle PreparationTimer;
    FDelegateHandle KillRewardHandle;
    TSet<int32> RewardedWaveKeys;
    struct FAutomaticWave
    {
        TArray<TWeakObjectPtr<AY01Monster>> Monsters;
        TArray<bool> SpawnedEntries;
    };
    TArray<FAutomaticWave> AutomaticWaves;
    bool bAutomaticWaves = false;
    bool bUpdatingAutomatic = false;
    bool bConfigured = false;
    bool bTransitioning = false;
    FString ConfigurationError;
    int32 LastStartedWave = INDEX_NONE;
    double DefaultCritMultiplier = 2;
};
