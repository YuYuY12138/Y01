#pragma once

#include "Player/Y01ItemRuntimeTypes.h"
#include "Player/Y01DropDeliveryComponent.h"
#include "Combat/Y01Combat.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "Y01ViewTypes.generated.h"

// 客户端只含显示值和请求身份，不含GA句柄、Ability类或服务器来源对象。
USTRUCT()
struct Y01_API FY01ViewDisplay
{
    GENERATED_BODY()
    UPROPERTY() FText Name;
    UPROPERTY() EY01Quality Quality = EY01Quality::Green;
    UPROPERTY() FSoftObjectPath Icon;
    UPROPERTY() EY01SkillType SkillType = EY01SkillType::Passive;
    UPROPERTY() float CooldownSeconds = 0;
};
USTRUCT()
struct Y01_API FY01ViewItem
{
    GENERATED_BODY()
    UPROPERTY() FGuid ItemId;
    UPROPERTY() FGuid ArrivalDeliveryId;
    UPROPERTY() FY01DataRef Definition;
    UPROPERTY() uint64 EntityRevision = 0;
    UPROPERTY() uint64 LastChangedRevision = 0;
    UPROPERTY() uint64 AcquisitionOrder = 0;
    UPROPERTY() EY01ItemLocation Location = EY01ItemLocation::Backpack;
    UPROPERTY() int32 Slot = INDEX_NONE;
    UPROPERTY() FY01ViewDisplay Display;
    UPROPERTY() TArray<FY01ItemEffect> Effects;
    UPROPERTY() TArray<FY01ItemDamageEffect> DamageEffects;
    UPROPERTY() TArray<FY01ItemGrowth> Growth;
};
USTRUCT()
struct Y01_API FY01ViewSkill
{
    GENERATED_BODY()
    UPROPERTY() FGuid LearnedId;
    UPROPERTY() FName SkillId;
    UPROPERTY() int32 Slot = INDEX_NONE;
    UPROPERTY() uint64 EntityRevision = 0;
    UPROPERTY() double Deadline = 0;
    UPROPERTY() FY01ViewDisplay Display;
    UPROPERTY() TArray<FY01ItemEffect> Effects;
    UPROPERTY() TArray<FY01ItemDamageEffect> DamageEffects;
    UPROPERTY() TArray<FY01ItemGrowth> Growth;
};
USTRUCT()
struct Y01_API FY01ViewReceipt
{
    GENERATED_BODY()
    UPROPERTY() FY01ItemRequest Request;
    UPROPERTY() EY01ItemError Code = EY01ItemError::None;
    UPROPERTY() FString Message;
    UPROPERTY() uint64 Revision = 0;
    UPROPERTY() FGuid TicketId;
    UPROPERTY() FGuid LearnedId;
    UPROPERTY() FGuid RemovedLearnedId;
    UPROPERTY() FGuid ProductItemId;
    UPROPERTY() FGuid TalentChoiceId;
    UPROPERTY() FName SelectedTalent;
};
USTRUCT()
struct Y01_API FY01CommittedView
{
    GENERATED_BODY()
    UPROPERTY() FGuid MatchId;
    UPROPERTY() int32 JoinSequence = 0;
    UPROPERTY() FGuid ConnectionId;
    // 业务视图版本独立于库存实体版本，CD/回执更新不使确认票据失效。
    UPROPERTY() uint64 ViewRevision = 0;
    UPROPERTY() uint64 Revision = 0;
    UPROPERTY() uint64 SourceRevision = 0;
    UPROPERTY() uint64 ConfigRevision = 0;
    UPROPERTY() uint64 ExpectedSeq = 1;
    UPROPERTY() double SampleTime = 0;
    UPROPERTY() int32 Gold = 0;
    UPROPERTY() int32 Experience = 0;
    UPROPERTY() float Health = 0;
    UPROPERTY() FY01UnitCombatStats Stats;
    UPROPERTY() FY01EffectiveKillGrowthLedger Growth;
    UPROPERTY() FY01DeliveryLedger Delivery;
    UPROPERTY() double IncomeRate = 0;
    UPROPERTY() TArray<FGuid> EquipmentSlots;
    UPROPERTY() TArray<FY01ViewSkill> Skills;
    UPROPERTY() TArray<FY01ViewItem> Items;
    UPROPERTY() TArray<FY01ShopOfferState> ShopOffers;
    UPROPERTY() TArray<FY01SynthesisState> SynthesisRecipes;
    UPROPERTY() FY01TalentChoiceState TalentChoice;
    UPROPERTY() TArray<FY01ViewReceipt> Receipts;
    // 最新入口响应独立于64条防重账本；拒绝/重放也能由完整视图确认。
    UPROPERTY() uint64 ResponseSerial = 0;
    UPROPERTY() FY01ViewReceipt Response;
    UPROPERTY() TArray<FY01ItemRequest> Tickets;
};
USTRUCT()
struct Y01_API FY01ViewChange
{
    GENERATED_BODY()
    UPROPERTY() EY01ItemChangeKind Kind = EY01ItemChangeKind::Added;
    UPROPERTY() FGuid ItemId;
    UPROPERTY() uint64 ExpectedEntityRevision = 0;
    UPROPERTY() uint64 EntityRevision = 0;
    UPROPERTY() uint64 LastChangedRevision = 0;
};
USTRUCT()
struct Y01_API FY01ViewCommit
{
    GENERATED_BODY()
    UPROPERTY() uint64 BaseViewRevision = 0;
    UPROPERTY() FY01CommittedView Header;
    UPROPERTY() TArray<FY01ViewChange> Manifest;
    UPROPERTY() TArray<FY01ViewItem> ChangedItems;
};
USTRUCT()
struct Y01_API FY01ViewGuidSlotDelta
{
    GENERATED_BODY()
    UPROPERTY() int32 Index = INDEX_NONE;
    UPROPERTY() FGuid Value;
};
USTRUCT()
struct Y01_API FY01ViewRequestSlotDelta
{
    GENERATED_BODY()
    UPROPERTY() int32 Index = INDEX_NONE;
    UPROPERTY() FY01ItemRequest Value;
};
USTRUCT()
struct Y01_API FY01ViewSkillDelta
{
    GENERATED_BODY()
    UPROPERTY() int32 Index = INDEX_NONE;
    UPROPERTY() uint32 Fields = 0;
    UPROPERTY() FGuid LearnedId;
    UPROPERTY() FName SkillId;
    UPROPERTY() int32 Slot = INDEX_NONE;
    UPROPERTY() uint64 EntityRevision = 0;
    UPROPERTY() double Deadline = 0;
    UPROPERTY() TArray<FY01ViewDisplay> Display;
    UPROPERTY() TArray<FY01ItemEffect> Effects;
    UPROPERTY() TArray<FY01ItemDamageEffect> DamageEffects;
    UPROPERTY() TArray<FY01ItemGrowth> Growth;
};
USTRUCT()
struct Y01_API FY01ViewOfferDelta
{
    GENERATED_BODY()
    UPROPERTY() FName OfferId;
    UPROPERTY() uint32 Fields = 0;
    UPROPERTY() FText DisplayName;
    UPROPERTY() EY01ShopKind ShopKind = EY01ShopKind::RandomPool;
    UPROPERTY() int32 Price = 0;
    UPROPERTY() int32 Quantity = 1;
    UPROPERTY() bool bPurchased = false;
    UPROPERTY() FString SourceKey;
    UPROPERTY() FString SourceId;
    UPROPERTY() TArray<FY01ItemEffect> Effects;
    UPROPERTY() TArray<FY01ItemGrowth> Growth;
};
USTRUCT()
struct Y01_API FY01ViewDeltaCommit
{
    GENERATED_BODY()
    UPROPERTY() uint64 BaseViewRevision = 0;
    UPROPERTY() FGuid MatchId;
    UPROPERTY() int32 JoinSequence = 0;
    UPROPERTY() FGuid ConnectionId;
    UPROPERTY() uint64 ViewRevision = 0;
    UPROPERTY() uint64 Revision = 0;
    UPROPERTY() uint64 SourceRevision = 0;
    UPROPERTY() uint64 ConfigRevision = 0;
    UPROPERTY() uint64 ExpectedSeq = 1;
    UPROPERTY() double SampleTime = 0;
    UPROPERTY() uint32 Fields = 0;
    UPROPERTY() int32 Gold = 0;
    UPROPERTY() int32 Experience = 0;
    UPROPERTY() float Health = 0;
    UPROPERTY() double IncomeRate = 0;
    UPROPERTY() uint64 ResponseSerial = 0;
    UPROPERTY() TArray<FY01UnitCombatStats> Stats;
    UPROPERTY() TArray<FY01EffectiveKillGrowthLedger> Growth;
    UPROPERTY() TArray<FY01DeliveryLedger> Delivery;
    UPROPERTY() TArray<FY01TalentChoiceState> TalentChoice;
    UPROPERTY() TArray<FY01ViewReceipt> Response;
    UPROPERTY() TArray<FY01ViewGuidSlotDelta> EquipmentSlots;
    UPROPERTY() TArray<FY01ViewSkillDelta> Skills;
    UPROPERTY() TArray<FName> ShopOfferOrder;
    UPROPERTY() TArray<FY01ViewOfferDelta> ChangedShopOffers;
    UPROPERTY() TArray<FName> SynthesisRecipeOrder;
    UPROPERTY() TArray<FY01SynthesisState> ChangedSynthesisRecipes;
    UPROPERTY() TArray<uint64> ReceiptOrder;
    UPROPERTY() TArray<FY01ViewReceipt> ChangedReceipts;
    UPROPERTY() TArray<FY01ViewRequestSlotDelta> Tickets;
    UPROPERTY() TArray<FY01ViewChange> Manifest;
    UPROPERTY() TArray<FY01ViewItem> ChangedItems;
};
USTRUCT()
struct Y01_API FY01ViewAck
{
    GENERATED_BODY()
    UPROPERTY() FGuid MatchId;
    UPROPERTY() int32 JoinSequence = 0;
    UPROPERTY() FGuid ConnectionId;
    UPROPERTY() FGuid TransferId;
    UPROPERTY() uint64 ViewRevision = 0;
    UPROPERTY() int32 WindowSequence = 0;
};
USTRUCT()
struct Y01_API FY01ViewChunk : public FFastArraySerializerItem
{
    GENERATED_BODY()
    UPROPERTY() FY01ViewAck Scope;
    UPROPERTY() int32 Offset = 0;
    UPROPERTY() int32 TotalBytes = 0;
    UPROPERTY() uint32 Digest = 0;
    UPROPERTY() uint32 ChunkDigest = 0;
    UPROPERTY() TArray<uint8> Bytes;
};
USTRUCT()
struct Y01_API FY01ViewWire : public FFastArraySerializer
{
    GENERATED_BODY()
    // 默认inner delta还编码逐字节变更句柄；预留其开销，不能仅按原始载荷估算网络上界。
    static constexpr int32 ChunkBytes = 1024;
    static constexpr int32 WindowChunks = 4;
    UPROPERTY() TArray<FY01ViewChunk> Items;
    bool NetDeltaSerialize(FNetDeltaSerializeInfo& Params);
    void PostReplicatedReceive(const FPostReplicatedReceiveParameters& Params);
    TWeakObjectPtr<UObject> Receiver;
};
template<> struct TStructOpsTypeTraits<FY01ViewWire> : TStructOpsTypeTraitsBase2<FY01ViewWire>
{ enum { WithNetDeltaSerializer = true }; };

// 显式R36Trace与R36Timing下的有界只读诊断，不进入复制结构或事务。
namespace Y01ViewTiming
{
    Y01_API double Now();
    Y01_API FY01ViewAck Scope(const FY01CommittedView& View);
    Y01_API void Mark(const TCHAR* Stage, const FY01ViewAck& Scope, double Start = 0,
        int32 Bytes = 0, uint64 Seq = 0, int32 Items = 0, const FY01CommittedView* View = nullptr);
}

// 每连接只持有已确认、冻结在途、最新三份状态，不按提交次数积累历史。
class Y01_API FY01ViewSender
{
public:
    bool Offer(const FY01CommittedView& View, FY01ViewWire& Wire, FString& Error);
    bool Acknowledge(const FY01ViewAck& Ack, FY01ViewWire& Wire, FString& Error);
    bool Restart(const FY01ViewAck& Scope, FY01ViewWire& Wire, FString& Error);
    // 同一已发布版本已在途或已确认时，重复Offer不会改变任何状态。
    bool IsCurrent(const FY01CommittedView& View) const;
    void Reset();
private:
    bool Begin(FY01ViewWire& Wire, FString& Error);
    void FillWindow(FY01ViewWire& Wire);
    FY01CommittedView Acknowledged, InFlight, Latest;
    FY01ViewAck Pending;
    TArray<uint8> Payload;
    int32 Offset = 0;
    uint32 Digest = 0;
};
class Y01_API FY01ViewReceiver
{
public:
    bool Receive(const FY01ViewWire& Wire, FY01ViewAck& Ack, FString& Error);
    const FY01CommittedView& Get() const { return Published; }
    void Reset();
private:
    FY01CommittedView Published;
    FY01ViewAck Pending;
    TArray<uint8> Payload;
    int32 TotalBytes = 0;
    uint32 Digest = 0;
};
