#pragma once
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "Data/Y01DataCatalog.h"
#include "Y01GameFlow.generated.h"

class AY01Monster;
class AY01PlayerState;
UENUM()
enum class EY01MatchPhase : uint8 { Waiting, Preparing, Battle, Ended };
UENUM()
enum class EY01MatchResult : uint8 { None, Success, Failure };

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
    UPROPERTY() int32 Revision = 0;
};

UCLASS()
class Y01_API AY01GameState : public AGameStateBase
{
    GENERATED_BODY()
public:
    AY01GameState();
    const FY01WaveSnapshot& GetWaveSnapshot() const { return Snapshot; }
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
private:
    friend class AY01GameMode;
    UPROPERTY(ReplicatedUsing=OnRep_Snapshot) FY01WaveSnapshot Snapshot;
    UFUNCTION() void OnRep_Snapshot();
};

UCLASS()
class Y01_API AY01GameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AY01GameMode();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void PostLogin(APlayerController* NewPlayer) override;
    bool TryConfigureMatch(const FY01DataCatalog& Catalog, const TArray<FY01DataRef>& OrderedWaves,
        const FY01DataRef& Character, FString& OutError);
    bool TryStartNextWave();
    bool TryCompleteWave();
    bool TryEndMatch(bool bSuccess);
    bool IsMatchConfigured() const { return bConfigured; }
    const FString& GetConfigurationError() const { return ConfigurationError; }
    const TArray<TObjectPtr<AY01Monster>>& GetWaveMonsters() const { return WaveMonsters; }
    AY01GameState* GetFlowState() const { return GetGameState<AY01GameState>(); }
protected:
    virtual bool LoadMatchConfiguration(FString& OutError);
    virtual bool CanStartWave() const { return true; }
    virtual bool CanCompleteWave() const { return true; }
    virtual bool CanEnterBattle() const { return true; }
    virtual FTransform GetMonsterSpawnTransform(int32 SpawnOrdinal) const;
    virtual void OnWaveMonsterSpawned(AY01Monster* Monster) {}
    virtual void OnWaveRewardCommitted() {}
private:
    void EnterBattle();
    void SpawnEntry(FY01SpawnEntry Entry, int32 WaveIndex);
    void Publish();
    void CancelTimers();
    bool InitializePlayer(AY01PlayerState* Player, FString& OutError);
    UPROPERTY() TArray<FY01WaveRow> Waves;
    UPROPERTY() TArray<FY01RewardPackageRow> Rewards;
    UPROPERTY() TArray<FY01DataRef> WaveRefs;
    UPROPERTY() FY01DataRef CharacterRef;
    UPROPERTY() TArray<TObjectPtr<AY01Monster>> WaveMonsters;
    FY01DataCatalog MatchCatalog;
    TArray<FTimerHandle> SpawnTimers;
    FTimerHandle PreparationTimer;
    TSet<int32> RewardedWaveKeys;
    bool bConfigured = false;
    bool bTransitioning = false;
    FString ConfigurationError;
};
