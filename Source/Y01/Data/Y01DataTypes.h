#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "Abilities/GameplayAbility.h"
#include "GameplayEffect.h"
#include "Y01DataTypes.generated.h"

namespace Y01Data
{
    inline constexpr int32 EquipmentSlotCount = 6;
    inline constexpr int32 SkillSlotCount = 4;
    inline constexpr int32 SynthesisInputCount = 5;
    inline constexpr int32 TalentCandidateCount = 3;
    inline constexpr int32 TalentRefreshPerCandidate = 1;
}

UENUM(BlueprintType)
enum class EY01TableKind : uint8
{
    None, Wave, TowerFloor, TrainingStage, Monster, Skill, Talent, Equipment,
    LevelExperience, MetaLootPool, MatchRules, Character, RewardPackage,
    LocalItemPool, ShopOffer, Synthesis, Quality, MetaItem, WarehouseEvent, TalentChoice, SourceEffect,
    GrowthRule = 21, QualityRollPool = 22, AttributeMapping = 23, DamageEffect = 24, Consumable = 25
};

UENUM(BlueprintType)
enum class EY01Quality : uint8 { Green, Blue, Purple, Gold, Red };
UENUM(BlueprintType)
enum class EY01WaveType : uint8 { Normal, Boss };
UENUM(BlueprintType)
enum class EY01WaveSchedule : uint8 { Legacy, Batches };
UENUM(BlueprintType)
enum class EY01SpawnPointKind : uint8 { Normal, Boss };

namespace Y01Data
{
    inline bool ValidSpawnPointTag(EY01SpawnPointKind Kind, FName Tag)
    {
        if (Kind == EY01SpawnPointKind::Boss) { return Tag == FName(TEXT("Boss")); }
        if (Kind != EY01SpawnPointKind::Normal || Tag.IsNone()) { return false; }
        const FString Text = Tag.ToString();
        if (Text.IsEmpty() || Text[0] < TEXT('1') || Text[0] > TEXT('9')) { return false; }
        int64 Value = 0;
        for (TCHAR C : Text)
        {
            if (C < TEXT('0') || C > TEXT('9')) { return false; }
            Value = Value * 10 + C - TEXT('0');
            if (Value > MAX_int32) { return false; }
        }
        return true;
    }
}
UENUM(BlueprintType)
enum class EY01AttackType : uint8 { Melee, Ranged };
UENUM(BlueprintType)
enum class EY01SkillType : uint8 { Active, Passive };
UENUM(BlueprintType)
enum class EY01SourceEffectKind : uint8
{ AttackFlat = 0, GoldPerSecond = 1, AttributeFlat = 2, MappingCoefficientAdd = 3, AttributePercent = 4,
  CritChance = 5, CritMultiplierAdd = 6, Lifesteal = 7, ShieldPeriodic = 8, ShieldOnCast = 9 };
UENUM(BlueprintType)
enum class EY01Attribute : uint8 { None, Strength, Agility, Wisdom, AttackDamage, MaxHealth, Armor, HealthRegen, GoldPerSecond, AttackRate, GoldGain };
UENUM(BlueprintType)
enum class EY01EffectCondition : uint8 { None, HealthBelow, HealthAbove };

namespace Y01Data
{
    inline bool IsPrimaryAttribute(EY01Attribute A)
    { return A == EY01Attribute::Strength || A == EY01Attribute::Agility || A == EY01Attribute::Wisdom; }
    inline bool IsDerivedAttribute(EY01Attribute A)
    {
        switch (A)
        {
        case EY01Attribute::AttackDamage: case EY01Attribute::MaxHealth: case EY01Attribute::Armor:
        case EY01Attribute::HealthRegen: case EY01Attribute::GoldPerSecond: case EY01Attribute::AttackRate: return true;
        default: return false;
        }
    }
    inline bool IsConditionalTarget(EY01Attribute A)
    {
        return A == EY01Attribute::AttackDamage || A == EY01Attribute::Armor || A == EY01Attribute::HealthRegen
            || A == EY01Attribute::AttackRate || A == EY01Attribute::GoldGain;
    }
    template<typename T> bool ValidSourceEffect(const T& E)
    {
        if (!FMath::IsFinite(E.Magnitude) || E.Magnitude < 0 || !FMath::IsFinite(E.Amount) || E.Amount < 0
            || !FMath::IsFinite(E.ConditionThreshold) || E.ConditionThreshold < 0 || E.ConditionThreshold > 100
            || !FMath::IsFinite(E.DurationSeconds) || !FMath::IsFinite(E.LayerCap) || !FMath::IsFinite(E.IntervalSeconds)) { return false; }
        switch (E.Condition)
        {
        case EY01EffectCondition::None:
            if (E.ConditionThreshold != 0) { return false; }
            break;
        case EY01EffectCondition::HealthBelow:
        case EY01EffectCondition::HealthAbove:
            if ((E.Kind != EY01SourceEffectKind::AttributeFlat && E.Kind != EY01SourceEffectKind::AttributePercent)
                || !IsConditionalTarget(E.TargetAttribute)) { return false; }
            break;
        default: return false;
        }
        switch (E.Kind)
        {
        case EY01SourceEffectKind::AttackFlat:
        case EY01SourceEffectKind::GoldPerSecond:
            return E.Amount == 0 && E.SourceAttribute == EY01Attribute::None && E.TargetAttribute == EY01Attribute::None;
        case EY01SourceEffectKind::AttributeFlat:
            return E.Magnitude == 0 && E.SourceAttribute == EY01Attribute::None
                && (IsPrimaryAttribute(E.TargetAttribute) || IsDerivedAttribute(E.TargetAttribute));
        case EY01SourceEffectKind::AttributePercent:
            return E.Magnitude == 0 && E.SourceAttribute == EY01Attribute::None
                && (IsPrimaryAttribute(E.TargetAttribute) || E.TargetAttribute == EY01Attribute::GoldGain
                    || (IsDerivedAttribute(E.TargetAttribute) && E.TargetAttribute != EY01Attribute::GoldPerSecond));
        case EY01SourceEffectKind::MappingCoefficientAdd:
            return E.Magnitude == 0 && IsPrimaryAttribute(E.SourceAttribute) && IsDerivedAttribute(E.TargetAttribute);
        case EY01SourceEffectKind::CritChance:
        case EY01SourceEffectKind::CritMultiplierAdd:
        case EY01SourceEffectKind::Lifesteal:
            return E.Magnitude == 0 && E.SourceAttribute == EY01Attribute::None && E.TargetAttribute == EY01Attribute::None
                && E.DurationSeconds == 0 && E.LayerCap == 0 && E.IntervalSeconds == 0;
        case EY01SourceEffectKind::ShieldPeriodic:
            return E.Magnitude == 0 && E.SourceAttribute == EY01Attribute::None && E.TargetAttribute == EY01Attribute::None
                && E.Amount > 0 && E.DurationSeconds > 0 && E.LayerCap > 0 && E.IntervalSeconds > 0;
        case EY01SourceEffectKind::ShieldOnCast:
            return E.Magnitude == 0 && E.SourceAttribute == EY01Attribute::None && E.TargetAttribute == EY01Attribute::None
                && E.Amount > 0 && E.DurationSeconds > 0 && E.LayerCap > 0 && E.IntervalSeconds == 0;
        default: return false;
        }
    }
}
UENUM(BlueprintType)
enum class EY01ShopKind : uint8 { RandomPool, DirectEffect, RandomTier = 2, DirectItem = 3 };
UENUM(BlueprintType)
enum class EY01ConsumableKind : uint8 { DevourPill };
UENUM(BlueprintType)
enum class EY01GrowthTrigger : uint8 { EffectiveKill, CombatTime, BasicAttack };
UENUM(BlueprintType)
enum class EY01GrowthTarget : uint8 { EquipmentInstance, Player };
UENUM(BlueprintType)
enum class EY01GrowthAttribute : uint8 { AttackDamage, Strength, MaxHealth, Agility, Wisdom };
UENUM(BlueprintType)
enum class EY01RewardDomain : uint8 { MatchPersonal, MetaPublic };
// 槽位仅定义身份与数量，不预先决定未批准的装备部位命名。
UENUM(BlueprintType)
enum class EY01EquipmentSlot : uint8 { Slot1, Slot2, Slot3, Slot4, Slot5, Slot6 };

USTRUCT(BlueprintType)
struct Y01_API FY01DataRef
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01TableKind Type = EY01TableKind::None;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FName RowName;
};

USTRUCT(BlueprintType)
struct Y01_API FY01SourceEffectRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01SourceEffectKind Kind = EY01SourceEffectKind::AttackFlat;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double Magnitude = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Attribute SourceAttribute = EY01Attribute::None;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Attribute TargetAttribute = EY01Attribute::None;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double Amount = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01EffectCondition Condition = EY01EffectCondition::None;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double ConditionThreshold = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double DurationSeconds = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double LayerCap = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double IntervalSeconds = 0;
};

USTRUCT(BlueprintType)
struct Y01_API FY01AttributeMappingRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) bool bBaseEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Attribute SourceAttribute = EY01Attribute::Strength;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Attribute TargetAttribute = EY01Attribute::AttackDamage;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double Coefficient = 0;
    bool IsValid() const
    {
        return Y01Data::IsPrimaryAttribute(SourceAttribute) && Y01Data::IsDerivedAttribute(TargetAttribute)
            && FMath::IsFinite(Coefficient) && Coefficient >= 0;
    }
};

UENUM(BlueprintType)
enum class EY01DamageTrigger : uint8 { Periodic, Active, Bound };
UENUM(BlueprintType)
enum class EY01DamageTargeting : uint8 { NearbyCount, SelfCircle, RemoteCircle };
UENUM(BlueprintType)
enum class EY01DamageOrder : uint8 { Nearest, Random };
UENUM(BlueprintType)
enum class EY01DamageCenter : uint8 { NearestEnemy, RandomEnemy, RandomGround };
UENUM(BlueprintType)
enum class EY01DamageProduct : uint8 { Instant, Projectile, AttachedDot, IndependentArea, BoundArea };
UENUM(BlueprintType)
enum class EY01DamagePosition : uint8 { Fixed, FollowPlayer };
UENUM(BlueprintType)
enum class EY01DamageStacking : uint8 { Refresh, Independent };

USTRUCT(BlueprintType)
struct Y01_API FY01DamageTerm
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Attribute Attribute = EY01Attribute::AttackDamage;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double Coefficient = 0;
};

// 距离均为XY圆半径cm，秒数彼此独立；倍率0.5表示50%。
USTRUCT(BlueprintType)
struct Y01_API FY01DamageEffectRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01DamageTrigger Trigger = EY01DamageTrigger::Periodic;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01DamageTargeting Targeting = EY01DamageTargeting::NearbyCount;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01DamageOrder Order = EY01DamageOrder::Nearest;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01DamageCenter Center = EY01DamageCenter::NearestEnemy;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01DamageProduct Product = EY01DamageProduct::Instant;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01DamagePosition Position = EY01DamagePosition::Fixed;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01DamageStacking Stacking = EY01DamageStacking::Refresh;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 TargetCount = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double SearchRadius = 800;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double Radius = 150;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double FixedDamage = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DamageTerm> Terms;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double PeriodSeconds = 3;
    // 主动CD沿Skill.CooldownSeconds；一组主动效果共享一次GA提交。
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double Lifetime = 5;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double TickSeconds = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double Speed = 400;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 MaxStacks = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) bool bLifesteal = false;
    bool IsValid() const;
};

USTRUCT()
struct Y01_API FY01ItemDamageEffect
{
    GENERATED_BODY()
    UPROPERTY() FName EffectId;
    UPROPERTY() FY01DamageEffectRow Config;
};

USTRUCT(BlueprintType)
struct Y01_API FY01GrowthRuleRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01GrowthTrigger Trigger = EY01GrowthTrigger::EffectiveKill;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01GrowthTarget Target = EY01GrowthTarget::EquipmentInstance;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01GrowthAttribute Attribute = EY01GrowthAttribute::AttackDamage;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double Delta = 0;
};

USTRUCT(BlueprintType)
struct Y01_API FY01QualityRollEntry
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Pool;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double Weight = 1;
};

USTRUCT(BlueprintType)
struct Y01_API FY01QualityRollPoolRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01TableKind ItemType = EY01TableKind::Equipment;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01QualityRollEntry> Entries;
};

USTRUCT(BlueprintType)
struct Y01_API FY01SpawnEntry
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Monster;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Count = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float DelaySeconds = 0;
};

USTRUCT(BlueprintType)
struct Y01_API FY01BatchMonster
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Monster;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(ClampMin="1")) int32 Count = 1;
};

USTRUCT(BlueprintType)
struct Y01_API FY01SpawnBatch
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FName BatchId;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(ClampMin="0")) float OffsetSeconds = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01SpawnPointKind PointKind = EY01SpawnPointKind::Normal;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FName PointTag;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01BatchMonster> Monsters;
};

USTRUCT(BlueprintType)
struct Y01_API FY01RewardEntry
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Item;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Quantity = 1;
};

USTRUCT(BlueprintType)
struct Y01_API FY01WeightedEntry
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Item;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Weight = 1;
};

USTRUCT(BlueprintType)
struct Y01_API FY01WaveRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) bool Enabled = true;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Order = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01WaveType Type = EY01WaveType::Normal;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float PreparationSeconds = 10;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float RespawnSeconds = 10;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01WaveSchedule Schedule = EY01WaveSchedule::Legacy;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01SpawnEntry> Spawns;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01SpawnBatch> Batches;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Reward;
};

USTRUCT(BlueprintType)
struct Y01_API FY01TowerFloorRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Floor = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01SpawnEntry> Spawns;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Reward;
};

USTRUCT(BlueprintType)
struct Y01_API FY01TrainingStageRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Stage = 1;
    // 阶段生效起点；不指定正式计时基准或旧怪处理方式。
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float StartSeconds = 0;
    // 通用默认兼容既有数据；程序测试配置显式设置周期3秒、上限6只。
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float SpawnInterval = 10;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 MaxAlive = 256;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01SpawnEntry> Spawns;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Reward;
};

USTRUCT(BlueprintType)
struct Y01_API FY01MonsterRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Health = 100;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float AttackDamage = 10;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float AttackInterval = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01AttackType AttackType = EY01AttackType::Melee;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float MeleeAngle = 90;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float ProjectileSpeed = 400;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Armor = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float HealthRegen = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Range = 150;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float MoveSpeed = 300;
    // 兼容旧调试数据的正默认，不代表正式平衡值；单位cm，0关闭主动发现。
    UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(ClampMin="0", Units="cm")) float DetectionRadius = 2000;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> Skills;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Experience = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Gold = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef DropReward;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double ItemDropChance = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<USkeletalMesh> Mesh;
};

// 仅程序测试技能使用；None 保留原即时GA行为，不代表正式技能分类。
UENUM(BlueprintType)
enum class EY01TestSkillMode : uint8 { None, Area, Projectile };

USTRUCT(BlueprintType)
struct Y01_API FY01TestSkillConfig
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01TestSkillMode Mode = EY01TestSkillMode::None;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float DamageCoefficient = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Radius = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float HalfHeight = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float ActiveSeconds = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float TargetRange = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Speed = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Lifetime = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float HitFeedbackSeconds = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) bool bLifesteal = true;

    bool IsValid() const
    {
        for (float Value : {DamageCoefficient, Radius, HalfHeight, ActiveSeconds, TargetRange, Speed, Lifetime, HitFeedbackSeconds})
        { if (!FMath::IsFinite(Value) || Value < 0) { return false; } }
        switch (Mode)
        {
        case EY01TestSkillMode::None: return true;
        case EY01TestSkillMode::Area: return Radius > 0 && ActiveSeconds > 0;
        case EY01TestSkillMode::Projectile: return Radius > 0 && TargetRange > 0 && Speed > 0 && Lifetime > 0;
        default: return false;
        }
    }
};

USTRUCT(BlueprintType)
struct Y01_API FY01SkillRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<UTexture2D> Icon;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01SkillType Type = EY01SkillType::Active;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float CooldownSeconds = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="程序测试配置") FY01TestSkillConfig TestSkill;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> DamageEffectIds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftClassPtr<UGameplayAbility> Ability;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftClassPtr<UGameplayEffect> Effect;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> EffectIds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> GrowthRuleIds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FText DisplayName;
};

USTRUCT(BlueprintType)
struct Y01_API FY01TalentRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftClassPtr<UGameplayAbility> Ability;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftClassPtr<UGameplayEffect> Effect;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> EffectIds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> GrowthRuleIds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FText DisplayName;
};

USTRUCT(BlueprintType)
struct Y01_API FY01EquipmentRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<UTexture2D> Icon;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(ClampMin="0")) float AttackBonus = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> DamageEffectIds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftClassPtr<UGameplayEffect> Effect;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> EffectIds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> GrowthRuleIds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FText DisplayName;
    bool IsValid() const
    {
        return static_cast<uint8>(Quality) <= static_cast<uint8>(EY01Quality::Red)
            && FMath::IsFinite(AttackBonus) && AttackBonus >= 0;
    }
};

// 局内可叠加道具；同行定义在背包内合并为一格计数。
USTRUCT(BlueprintType)
struct Y01_API FY01ConsumableRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01ConsumableKind Kind = EY01ConsumableKind::DevourPill;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Red;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<UTexture2D> Icon;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FText DisplayName;
    bool IsValid() const
    {
        return Kind == EY01ConsumableKind::DevourPill && static_cast<uint8>(Quality) <= static_cast<uint8>(EY01Quality::Red)
            && !DisplayName.IsEmpty();
    }
};

USTRUCT(BlueprintType)
struct Y01_API FY01LevelExperienceRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Level = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 RequiredExperience = 100;
};

USTRUCT(BlueprintType)
struct Y01_API FY01MetaLootPoolRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01WeightedEntry> Entries;
};

USTRUCT(BlueprintType)
struct Y01_API FY01CoreSettings
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float MaxHealth = 1000;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float HealthRegen = 5;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float HitAlertInterval = 3;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float CriticalHealthRatio = .3f;
    bool IsValid() const
    {
        return FMath::IsFinite(MaxHealth) && MaxHealth > 0
            && FMath::IsFinite(HealthRegen) && HealthRegen >= 0
            && FMath::IsFinite(HitAlertInterval) && HitAlertInterval > 0
            && FMath::IsFinite(CriticalHealthRatio) && CriticalHealthRatio > 0 && CriticalHealthRatio <= 1;
    }
};

USTRUCT(BlueprintType)
struct Y01_API FY01MatchStartup
{
    GENERATED_BODY()
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly) int32 SchemaVersion = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Character;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) bool bTrainingEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) bool bAutoAttackEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) bool bEconomyEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) bool bCoreDefenseEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef TalentChoice;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 InitialGold = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double BaseGoldPerSecond = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(ClampMin="0")) double DropFlightSeconds = 1;
};

USTRUCT(BlueprintType)
struct Y01_API FY01MatchPresentation
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor DamageColor = FLinearColor::Red;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor HealingColor = FLinearColor::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, meta=(ClampMin="0")) double RegenMergeSeconds = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor CriticalColor = FLinearColor(1.f, .45f, 0.f, 1.f);
};

USTRUCT(BlueprintType)
struct Y01_API FY01MatchRulesRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 MaxLevel = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01CoreSettings Core;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01MatchStartup Startup;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01MatchPresentation Presentation;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double DefaultCritMultiplier = 2;
};

// R33程序测试授予，非正式平衡；正式角色默认完全关闭。
USTRUCT(BlueprintType)
struct Y01_API FY01R33TestGrowth
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) bool bEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double AttackPerEffectiveSecond = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double AttackPerEffectiveKill = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) double GoldPerEffectiveSecond = 0;
    bool IsValid() const
    {
        for (double Rate : {AttackPerEffectiveSecond, AttackPerEffectiveKill, GoldPerEffectiveSecond})
        { if (!FMath::IsFinite(Rate) || Rate < 0) { return false; } }
        return true;
    }
};

USTRUCT(BlueprintType)
struct Y01_API FY01CharacterRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Health = 100;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float AttackDamage = 10;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float AttackInterval = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01AttackType AttackType = EY01AttackType::Melee;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float MeleeAngle = 90;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float ProjectileSpeed = 400;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Armor = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float HealthRegen = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Range = 150;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float MoveSpeed = 600;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Strength = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Agility = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Wisdom = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R33测试成长（非正式平衡）") FY01R33TestGrowth R33TestGrowth;
    // 数组索引对应六个通用槽；此处不定义同名装备是否可重复。
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> InitialEquipment;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> InitialSkills;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<USkeletalMesh> Mesh;
};

namespace Y01Data
{
    template <typename T> bool ValidUnitStats(const T& Row)
    {
        for (float V : {Row.Health, Row.AttackDamage, Row.Range, Row.MoveSpeed, Row.AttackInterval,
            Row.MeleeAngle, Row.ProjectileSpeed, Row.Armor, Row.HealthRegen})
        { if (!FMath::IsFinite(V) || V < 0) { return false; } }
        return Row.Health > 0 && Row.AttackInterval > 0 && Row.MeleeAngle > 0 && Row.MeleeAngle <= 360
            && Row.ProjectileSpeed > 0 && (Row.AttackType == EY01AttackType::Melee || Row.AttackType == EY01AttackType::Ranged);
    }
}

USTRUCT(BlueprintType)
struct Y01_API FY01RewardPackageRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01RewardDomain Domain = EY01RewardDomain::MatchPersonal;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Experience = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Gold = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01RewardEntry> Items;
};

USTRUCT(BlueprintType)
struct Y01_API FY01LocalItemPoolRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01TableKind ItemType = EY01TableKind::Equipment;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01WeightedEntry> Entries;
};

USTRUCT(BlueprintType)
struct Y01_API FY01ShopOfferRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) bool Enabled = true;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 SortOrder = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Pool;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef TierPool;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Price = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Quantity = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01ShopKind ShopKind = EY01ShopKind::RandomPool;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> EffectIds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> GrowthRuleIds;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FText DisplayName;
};

USTRUCT(BlueprintType)
struct Y01_API FY01SynthesisRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) bool Enabled = true;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 SortOrder = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01TableKind InputType = EY01TableKind::Equipment;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality InputQuality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 InputCount = Y01Data::SynthesisInputCount;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef OutputPool;
};

USTRUCT(BlueprintType)
struct Y01_API FY01QualityRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FText DisplayName;
};

USTRUCT(BlueprintType)
struct Y01_API FY01MetaItemRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FText DisplayName;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<UTexture2D> Icon;
};

USTRUCT(BlueprintType)
struct Y01_API FY01WarehouseEventRow : public FTableRowBase
{
    GENERATED_BODY()
    // 提升现有仓库物品的件数，不定义目标挑选策略。
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 UpgradeCount = 1;
};

USTRUCT(BlueprintType)
struct Y01_API FY01TalentChoiceRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> Candidates;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 RefreshesPerCandidate = Y01Data::TalentRefreshPerCandidate;
};
