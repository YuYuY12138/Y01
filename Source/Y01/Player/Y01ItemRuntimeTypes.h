#pragma once

#include "Data/Y01DataTypes.h"
#include "GameplayAbilitySpecHandle.h"
#include "Y01ItemRuntimeTypes.generated.h"

#define Y01_LOADOUT_LEARNING 1

UENUM()
enum class EY01ItemLocation : uint8 { Backpack, EquipmentSlot };
UENUM()
enum class EY01ItemOperation : uint8 { Equip, Unequip, PrepareLearn, ConfirmLearn, CancelLearn, RemoveSkill, Purchase, Synthesize, ChooseTalent, Devour };
UENUM()
enum class EY01ItemChangeKind : uint8 { Added, Changed, Removed };
UENUM()
enum class EY01ItemError : uint8
{
    None, Disabled, Authority, Session, Busy, Stopped, Phase, Dead, InvalidOperation,
    InvalidDefinition, DuplicateItem, MissingItem, InvalidSlot, InvalidLocation, StaleEntity, StaleSlot,
    PayloadConflict, ExpiredSequence, SequenceGap, InvalidSequence, Precision, Range,
    EffectRejected, HandleLost, Interrupted, SourceFailure, DuplicateSkill, StaleTicket, StaleConfig, AbilityRejected,
    InsufficientFunds, PurchaseLimit
};

USTRUCT()
struct Y01_API FY01ItemConsume
{
    GENERATED_BODY()
    UPROPERTY() FGuid ItemId;
    UPROPERTY() uint64 ExpectedEntityRevision = 0;
    bool operator==(const FY01ItemConsume& Other) const
    { return ItemId == Other.ItemId && ExpectedEntityRevision == Other.ExpectedEntityRevision; }
};

// 仅意图载荷；没有客户端价格、属性、随机种子或其他owner。
USTRUCT()
struct Y01_API FY01ItemRequest
{
    GENERATED_BODY()
    UPROPERTY() FGuid MatchId;
    UPROPERTY() int32 JoinSequence = 0;
    UPROPERTY() uint64 Seq = 0;
    UPROPERTY() EY01ItemOperation Operation = EY01ItemOperation::Equip;
    UPROPERTY() FGuid ItemId;
    UPROPERTY() uint64 ExpectedEntityRevision = 0;
    UPROPERTY() int32 Slot = INDEX_NONE;
    UPROPERTY() FGuid ExpectedSlotItemId;
    UPROPERTY() uint64 ExpectedSlotEntityRevision = 0;
    UPROPERTY() FGuid ExpectedLearnedId;
    UPROPERTY() uint64 ExpectedLearnedRevision = 0;
    UPROPERTY() uint64 ConfigRevision = 0;
    UPROPERTY() FGuid TicketId;
    UPROPERTY() FY01DataRef Offer;
    UPROPERTY() TArray<FY01ItemConsume> Materials;
    UPROPERTY() FY01DataRef Recipe;
    UPROPERTY() FY01DataRef Talent;
    UPROPERTY() FGuid TalentChoiceId;
};

USTRUCT()
struct Y01_API FY01ItemEffect
{
    GENERATED_BODY()
    UPROPERTY() FName EffectId;
    UPROPERTY() EY01SourceEffectKind Kind = EY01SourceEffectKind::AttackFlat;
    UPROPERTY() double Magnitude = 0;
    UPROPERTY() EY01Attribute SourceAttribute = EY01Attribute::None;
    UPROPERTY() EY01Attribute TargetAttribute = EY01Attribute::None;
    UPROPERTY() double Amount = 0;
    UPROPERTY() EY01EffectCondition Condition = EY01EffectCondition::None;
    UPROPERTY() double ConditionThreshold = 0;
    UPROPERTY() bool bGrowthContribution = false;
    UPROPERTY() double DurationSeconds = 0;
    UPROPERTY() double LayerCap = 0;
    UPROPERTY() double IntervalSeconds = 0;
};

// 每实例独立拥有规则快照和累计账本；本子步不绑定成长触发器。
USTRUCT()
struct Y01_API FY01ItemGrowth
{
    GENERATED_BODY()
    UPROPERTY() FName RuleId;
    UPROPERTY() EY01GrowthTrigger Trigger = EY01GrowthTrigger::EffectiveKill;
    UPROPERTY() EY01GrowthTarget Target = EY01GrowthTarget::EquipmentInstance;
    UPROPERTY() EY01GrowthAttribute Attribute = EY01GrowthAttribute::AttackDamage;
    UPROPERTY() double Delta = 0;
    UPROPERTY() uint64 AppliedCount = 0;
    UPROPERTY() double Accumulated = 0;
};

// 玩家成长与来源静态规则分离；移除来源不追扣局内累计。
USTRUCT()
struct Y01_API FY01PlayerGrowthEntry
{
    GENERATED_BODY()
    UPROPERTY() FString SourceId;
    UPROPERTY() FY01ItemGrowth Rule;
};
USTRUCT()
struct Y01_API FY01EffectiveKillGrowthLedger
{
    GENERATED_BODY()
    UPROPERTY() bool bEnabled = false;
    UPROPERTY() bool bEnded = false;
    UPROPERTY() uint64 AppliedKills = 0;
    UPROPERTY() double Strength = 0;
    UPROPERTY() double Agility = 0;
    UPROPERTY() double Wisdom = 0;
    UPROPERTY() double MaxHealth = 0;
    UPROPERTY() TArray<FY01PlayerGrowthEntry> PlayerRules;
    UPROPERTY() FGuid LastKillEventId;
    UPROPERTY() FGuid FaultKillEventId;
    UPROPERTY() EY01ItemError GrowthFault = EY01ItemError::None;
    UPROPERTY() FString FaultMessage;
};

USTRUCT()
struct Y01_API FY01ShopOfferOdds
{
    GENERATED_BODY()
    UPROPERTY() EY01Quality Quality = EY01Quality::Green;
    UPROPERTY() double Weight = 0;
};

// 商店自有静态快照与限购状态；只记录规则，本子步不触发成长。
USTRUCT()
struct Y01_API FY01ShopOfferState
{
    GENERATED_BODY()
    UPROPERTY() FName OfferId;
    UPROPERTY() FText DisplayName;
    UPROPERTY() EY01ShopKind ShopKind = EY01ShopKind::RandomPool;
    UPROPERTY() int32 Price = 0;
    UPROPERTY() int32 Quantity = 1;
    UPROPERTY() EY01TableKind ItemType = EY01TableKind::None;
    UPROPERTY() TArray<FY01ShopOfferOdds> Odds;
    UPROPERTY() bool bPurchased = false;
    UPROPERTY() FString SourceKey;
    UPROPERTY() FString SourceId;
    UPROPERTY() TArray<FY01ItemEffect> Effects;
    UPROPERTY() TArray<FY01ItemGrowth> Growth;
};

USTRUCT()
struct Y01_API FY01SynthesisState
{
    GENERATED_BODY()
    UPROPERTY() FName RecipeId;
    UPROPERTY() FY01SynthesisRow Recipe;
};
USTRUCT()
struct Y01_API FY01TalentCandidate
{
    GENERATED_BODY()
    UPROPERTY() FName TalentId;
    UPROPERTY() FText DisplayName;
    UPROPERTY() EY01Quality Quality = EY01Quality::Green;
    UPROPERTY() TArray<FY01ItemEffect> Effects;
    UPROPERTY() TArray<FY01ItemGrowth> Growth;
};
USTRUCT()
struct Y01_API FY01TalentChoiceState
{
    GENERATED_BODY()
    UPROPERTY() FGuid ChoiceId;
    UPROPERTY() FY01DataRef Definition;
    UPROPERTY() TArray<FY01TalentCandidate> Candidates;
    UPROPERTY() FName SelectedTalent;
    UPROPERTY() FString SourceKey;
    UPROPERTY() FString SourceId;
};

// 入包时复制的静态技能数据；不持有目录或DataTable行指针。
USTRUCT()
struct Y01_API FY01ItemSkill
{
    GENERATED_BODY()
    UPROPERTY() EY01SkillType Type = EY01SkillType::Passive;
    UPROPERTY() EY01Quality Quality = EY01Quality::Green;
    UPROPERTY() FText DisplayName;
    UPROPERTY() TSoftObjectPtr<UTexture2D> Icon;
    UPROPERTY() TSoftClassPtr<UGameplayAbility> Ability;
    UPROPERTY() FY01TestSkillConfig TestSkill;
    UPROPERTY() float CooldownSeconds = 0;
};

USTRUCT()
struct Y01_API FY01LearnedSkill
{
    GENERATED_BODY()
    UPROPERTY() FGuid LearnedId;
    UPROPERTY() FName SkillId;
    UPROPERTY() int32 Slot = INDEX_NONE;
    UPROPERTY() uint64 EntityRevision = 0;
    UPROPERTY() FGameplayAbilitySpecHandle GAHandle;
    UPROPERTY() double Deadline = 0;
    UPROPERTY() FY01ItemSkill Skill;
    UPROPERTY() TArray<FY01ItemDamageEffect> DamageEffects;
    UPROPERTY() TArray<FY01ItemEffect> Effects;
    UPROPERTY() TArray<FY01ItemGrowth> Growth;
};

USTRUCT()
struct Y01_API FY01ItemInstance
{
    GENERATED_BODY()
    UPROPERTY() FGuid ItemId;
    UPROPERTY() FGuid ArrivalDeliveryId;
    UPROPERTY() FY01DataRef Definition;
    UPROPERTY() EY01ItemLocation Location = EY01ItemLocation::Backpack;
    UPROPERTY() int32 Slot = INDEX_NONE;
    UPROPERTY() uint64 EntityRevision = 1;
    // 最近一次穿戴时的实体版本；此后槽内成长只推进EntityRevision。
    UPROPERTY() uint64 PlacementRevision = 0;
    UPROPERTY() uint64 LastChangedRevision = 0;
    UPROPERTY() uint64 AcquisitionOrder = 0;
    UPROPERTY() FText DisplayName;
    UPROPERTY() EY01Quality Quality = EY01Quality::Green;
    UPROPERTY() TSoftObjectPtr<UTexture2D> Icon;
    // 仅可叠加道具大于1；装备与技能书恒为1。
    UPROPERTY() int32 Count = 1;
    UPROPERTY() FY01ItemSkill Skill;
    UPROPERTY() TArray<FY01ItemDamageEffect> DamageEffects;
    UPROPERTY() TArray<FY01ItemEffect> Effects;
    UPROPERTY() TArray<FY01ItemGrowth> Growth;
};

USTRUCT()
struct Y01_API FY01ItemChange
{
    GENERATED_BODY()
    UPROPERTY() EY01ItemChangeKind Kind = EY01ItemChangeKind::Changed;
    // 删除时仍携带ItemId及删除版本；本子步没有消耗或删除业务。
    UPROPERTY() FY01ItemInstance Item;
};

USTRUCT()
struct Y01_API FY01ItemResult
{
    GENERATED_BODY()
    UPROPERTY() EY01ItemError Code = EY01ItemError::None;
    UPROPERTY() FString Message;
    UPROPERTY() uint64 Revision = 0;
    UPROPERTY() TArray<FY01ItemChange> Changes;
    UPROPERTY() FGuid TicketId;
    UPROPERTY() FY01LearnedSkill Learned;
    UPROPERTY() FGuid RemovedLearnedId;
    UPROPERTY() FGuid ProductItemId;
    UPROPERTY() FGuid TalentChoiceId;
    UPROPERTY() FName SelectedTalent;
    bool IsOk() const { return Code == EY01ItemError::None; }
    static FY01ItemResult Fail(EY01ItemError Code, uint64 Revision, const TCHAR* Message)
    { FY01ItemResult R; R.Code = Code; R.Revision = Revision; R.Message = Message; return R; }
};

struct Y01_API FY01ItemGrant
{
    FGuid ItemId;
    FY01DataRef Definition;
};

USTRUCT()
struct Y01_API FY01LoadoutSnapshot
{
    GENERATED_BODY()
    UPROPERTY() uint64 Revision = 0;
    UPROPERTY() uint64 ExpectedSeq = 1;
    UPROPERTY() TArray<FY01ItemInstance> Items;
    UPROPERTY() TArray<FGuid> EquipmentSlots;
    UPROPERTY() TArray<FY01LearnedSkill> Skills;
    UPROPERTY() TArray<FY01ItemInstance> Devoured;
    UPROPERTY() uint64 ConfigRevision = 0;
};
