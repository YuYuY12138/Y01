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
    LocalItemPool, ShopOffer, Synthesis, Quality, MetaItem, WarehouseEvent, TalentChoice
};

UENUM(BlueprintType)
enum class EY01Quality : uint8 { Green, Blue, Purple, Gold, Red };
UENUM(BlueprintType)
enum class EY01WaveType : uint8 { Normal, Boss };
UENUM(BlueprintType)
enum class EY01SkillType : uint8 { Active, Passive };
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
struct Y01_API FY01SpawnEntry
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Monster;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Count = 1;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float DelaySeconds = 0;
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
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01WaveType Type = EY01WaveType::Normal;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float PreparationSeconds = 10;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01SpawnEntry> Spawns;
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
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01SpawnEntry> Spawns;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Reward;
};

USTRUCT(BlueprintType)
struct Y01_API FY01MonsterRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Health = 100;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float AttackDamage = 10;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Range = 150;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float MoveSpeed = 300;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> Skills;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Experience = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Gold = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef DropReward;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<USkeletalMesh> Mesh;
};

USTRUCT(BlueprintType)
struct Y01_API FY01SkillRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01SkillType Type = EY01SkillType::Active;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float CooldownSeconds = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftClassPtr<UGameplayAbility> Ability;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftClassPtr<UGameplayEffect> Effect;
};

USTRUCT(BlueprintType)
struct Y01_API FY01TalentRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftClassPtr<UGameplayAbility> Ability;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftClassPtr<UGameplayEffect> Effect;
};

USTRUCT(BlueprintType)
struct Y01_API FY01EquipmentRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftClassPtr<UGameplayEffect> Effect;
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
struct Y01_API FY01MatchRulesRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 MaxLevel = 1;
};

USTRUCT(BlueprintType)
struct Y01_API FY01CharacterRow : public FTableRowBase
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Health = 100;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float AttackDamage = 10;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float Range = 150;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) float MoveSpeed = 600;
    // 数组索引对应六个通用槽；此处不定义同名装备是否可重复。
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> InitialEquipment;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TArray<FY01DataRef> InitialSkills;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) TSoftObjectPtr<USkeletalMesh> Mesh;
};

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
    UPROPERTY(EditAnywhere, BlueprintReadOnly) FY01DataRef Pool;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01Quality Quality = EY01Quality::Green;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Price = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly) int32 Quantity = 1;
};

USTRUCT(BlueprintType)
struct Y01_API FY01SynthesisRow : public FTableRowBase
{
    GENERATED_BODY()
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
