#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Y01DamageEvents.generated.h"

class UY01CombatAbilitySystem;
class AY01PlayerController;

UENUM()
enum class EY01DamageEventType : uint8 { Damage, Healing };

// 纯值载荷；不引用受击者或攻击者，死亡销毁后仍可消费。
USTRUCT()
struct Y01_API FY01DamageEvent
{
    GENERATED_BODY()
    UPROPERTY() FGuid WorldEpoch;
    UPROPERTY() uint64 EventId = 0;
    UPROPERTY() int32 RegionId = INDEX_NONE;
    UPROPERTY() float ActualDamage = 0;
    UPROPERTY() float DisplayAmount = 0;
    // 暴击标记：飘字展示与后续表现消费；不参与金额校验。
    UPROPERTY() bool bCritical = false;
    UPROPERTY() EY01DamageEventType Type = EY01DamageEventType::Damage;
    UPROPERTY() FVector WorldPosition = FVector::ZeroVector;
    bool HasValidAmount() const
    {
        if (!FMath::IsFinite(ActualDamage) || !FMath::IsFinite(DisplayAmount) || DisplayAmount <= 0) { return false; }
        return Type == EY01DamageEventType::Damage ? ActualDamage > 0 && DisplayAmount + KINDA_SMALL_NUMBER >= ActualDamage
            : Type == EY01DamageEventType::Healing && ActualDamage == 0;
    }
};

// Generation属于拥有者Controller，显式切区递增；WorldEpoch区分不同World。
USTRUCT()
struct Y01_API FY01DamageAudience
{
    GENERATED_BODY()
    UPROPERTY() FGuid WorldEpoch;
    UPROPERTY() int32 RegionId = INDEX_NONE;
    UPROPERTY() uint64 Generation = 0;
    // 仅自动死亡返回允许这一个旧区身份自然到期，不开放整个旧区。
    UPROPERTY() uint64 FinalEventId = 0;
    UPROPERTY() int32 FinalRegionId = INDEX_NONE;
    bool IsFinalEvent(const FY01DamageEvent& Event) const
    { return FinalEventId != 0 && RegionId == 0 && FinalRegionId > 0 && Event.WorldEpoch == WorldEpoch && Event.EventId == FinalEventId && Event.RegionId == FinalRegionId; }
    bool operator==(const FY01DamageAudience& Other) const
    { return WorldEpoch == Other.WorldEpoch && RegionId == Other.RegionId && Generation == Other.Generation && FinalEventId == Other.FinalEventId && FinalRegionId == Other.FinalRegionId; }
};

struct FY01DamageRecipient
{
    TWeakObjectPtr<AY01PlayerController> Controller;
    FY01DamageAudience Audience;
};

struct FY01PendingDamage
{
    FY01DamageEvent Event;
    TArray<FY01DamageRecipient> Recipients;
};

DECLARE_MULTICAST_DELEGATE_OneParam(FY01DamageEventDelegate, const FY01DamageEvent&);
DECLARE_DELEGATE_RetVal_ThreeParams(bool, FY01DamageAudiencePolicy, const UY01CombatAbilitySystem*, const UY01CombatAbilitySystem*, FY01PendingDamage&);

// 只负责已落实伤害的World身份与发布，不参与死亡、奖励或成长结算。
UCLASS()
class Y01_API UY01DamageEvents : public UWorldSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    virtual void OnWorldEndPlay(UWorld& InWorld) override;
    FGuid GetWorldEpoch() const { return WorldEpoch; }
    FY01PendingDamage Capture(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target);
    bool ResolveAudience(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target, FY01PendingDamage& Pending);
    void Commit(const FY01PendingDamage& Pending);
    void StageFinalDamage(const FY01PendingDamage& Pending, const UY01CombatAbilitySystem* Target);
    FY01DamageEventDelegate OnDamageCommitted;
    // 仅在扣血前解析一次；缺省没有网络受众，绝不回退广播。
    FY01DamageAudiencePolicy AudiencePolicy;
private:
    FGuid WorldEpoch;
    uint64 NextEventId = 0;
};
