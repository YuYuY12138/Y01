#pragma once
#include "Player/Y01PlayerController.h"
#include "Playground/Y01PlaygroundRuntime.h"
#include "Combat/Y01Combat.h"
#include "Y01PlaygroundController.generated.h"

USTRUCT()
struct FY01PlaygroundSlotView
{
    GENERATED_BODY()
    UPROPERTY() FName Row;
    UPROPERTY() bool bActive = false;
    UPROPERTY() bool bEffectApplied = false;
    UPROPERTY() double Deadline = 0;
    UPROPERTY() FString Description;
    UPROPERTY() float DamageCoefficient = 0;
};

USTRUCT()
struct FY01PlaygroundEquipmentView
{
    GENERATED_BODY()
    UPROPERTY() FName Row;
    UPROPERTY() EY01Quality Quality = EY01Quality::Green;
    UPROPERTY() float AttackBonus = 0;
    UPROPERTY() bool bBonusActive = false;
    UPROPERTY() bool bDebugLocked = false;
};

USTRUCT()
struct FY01PlaygroundObserver
{
    GENERATED_BODY()
    UPROPERTY() bool bReady = false;
    // -1表示尚未采样；只复制服务器游戏时间，不在客户端外推。
    UPROPERTY() double ServerSampleTime = -1;
    UPROPERTY() double PreparationDeadline = 0;
    // 同一次服务器采样的完整波次状态；旧Alive/Total在自动模式中统一为已开始主线汇总。
    UPROPERTY() FY01WaveSnapshot Waves;
    UPROPERTY() bool bTrainingAvailable = false;
    UPROPERTY() FY01TrainingView Training;
    UPROPERTY() FY01GrowthView Growth;
    UPROPERTY() bool bAutoAttackEnabled = false;
    UPROPERTY() FY01UnitCombatStats UnitStats;
    UPROPERTY() FY01UnitCombatStats TargetStats;
    UPROPERTY() double AutoAttackDeadline = 0;
    UPROPERTY() double TargetAutoAttackDeadline = 0;
    UPROPERTY() float TargetHealth = 0;
    UPROPERTY() float Health = 0;
    UPROPERTY() float MaxHealth = 0;
    UPROPERTY() float AttackBase = 0;
    UPROPERTY() float AttackCurrent = 0;
    UPROPERTY() float MoveSpeed = 0;
    UPROPERTY() int32 Gold = 0;
    UPROPERTY() int32 XP = 0;
    UPROPERTY() FString Error;
    UPROPERTY() FString ProfilePath;
    UPROPERTY() FString Notice;
    UPROPERTY() TArray<FY01PlaygroundSlotView> Slots;
    UPROPERTY() TArray<FY01PlaygroundEquipmentView> Equipment;
    UPROPERTY() FString EquipmentResult;
    UPROPERTY() bool bEquipmentSucceeded = false;
    UPROPERTY() TObjectPtr<AY01Monster> Target;
    UPROPERTY() FY01DataRef TargetRow;
    UPROPERTY() float TargetSpeed = 0;
    UPROPERTY() float TargetRange = 0;
    UPROPERTY() int32 Alive = 0;
    UPROPERTY() int32 Total = 0;
    UPROPERTY() int32 RewardGold = 0;
    UPROPERTY() int32 RewardXP = 0;
};

USTRUCT()
struct FY01PlaygroundEvidence
{
    GENERATED_BODY()
    UPROPERTY() int32 Phase = 0;
    UPROPERTY() float MaxHealth = 0;
    UPROPERTY() float Attack = 0;
    UPROPERTY() int32 Gold = 0;
    UPROPERTY() int32 XP = 0;
    UPROPERTY() double CooldownDeadline = 0;
    UPROPERTY() bool bPassive = false;
    UPROPERTY() bool bMoved = false;
    UPROPERTY() bool bDamage = false;
    UPROPERTY() bool bInputMoved = false;
    UPROPERTY() bool bRestartNotice = false;
};

UCLASS()
class AY01PlaygroundController : public AY01PlayerController
{
    GENERATED_BODY()
public:
    AY01PlaygroundController();
    virtual void Tick(float DeltaSeconds) override;
    virtual void BeginPlay() override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    void CaptureObserver(const AY01PlaygroundGameMode* Mode);
    const FY01PlaygroundObserver& GetObserver() const { return Observer; }
    TOptional<double> GetSnapshotRemaining(double Deadline) const;
    void ShowRestartNotice();
    UFUNCTION(Server, Reliable) void ServerPlaygroundAction(int32 Action);
    UFUNCTION(Server, Reliable) void ServerVerifyObservation(FY01PlaygroundEvidence Evidence);
    UFUNCTION(Client, Reliable) void ClientVerificationFinished();
    UFUNCTION(Client, Reliable) void ClientVerificationAdvance();
    bool bVerifyReady = false;
    bool bVerifyComplete = false;
    static bool IsVerificationEnabled();
protected:
    virtual void SetupInputComponent() override;
private:
    void Advance();
    void Restart();
    void SkillPressed(const FInputActionValue& Value, int32 Slot);
    void TickVerification();
    void VerifyPress(const FKey& Key);
    TArray<FKey> VerifyReleaseKeys;
    FVector VerifyStartPosition = FVector::ZeroVector;
    double VerifyMoveStart = -1;
    bool bVerifyMoveReleased = false;
    bool bInputMoved = false;
    bool bVerifyRestartPressed = false;
    int32 VerifyEnterCount = 0;
    TSet<FName> VerifyScreenshots;
    double NextVerifyTick = 0;
    double NextTrainingTrace = 0;
    double LastObservedCooldown = 0;
    bool bObservedMovement = false;
    bool bObservedDamage = false;
    bool bScreenshotRequested = false;
    TMap<TWeakObjectPtr<AY01Monster>, FVector> ObservedPositions;
    UPROPERTY(Replicated) FY01PlaygroundObserver Observer;
};
