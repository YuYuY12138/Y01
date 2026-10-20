#pragma once

#include "Components/ActorComponent.h"
#include "Data/Y01DataCatalog.h"
#include "Player/Y01ItemTransactions.h"
#include "Player/Y01SourceRuntimeTypes.h"
#include "Player/Y01ViewTypes.h"
#include "Y01LoadoutComponent.generated.h"

class AY01PlayerState;
class AY01GameState;
class AController;
class UNetConnection;

// PlayerState唯一物品注册表。默认关闭；仅已初始化业务视图OwnerOnly复制，无Tick或客户端领物入口。
UCLASS()
class Y01_API UY01LoadoutComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01LoadoutComponent();
    bool IsInitialized() const { return bInitialized; }
    FY01ItemResult Initialize(const FGuid& MatchId, int32 JoinSequence);
    // 仅服务器内部购买/掉落调用方提供预分配身份；在入口验证并复制静态快照。
    FY01ItemResult AddItems(const FY01DataCatalog& Catalog, const TArray<FY01ItemGrant>& Grants);
    // Controller只提交自身会话意图；复用原事务，响应随完整视图发布。
    FY01ItemResult SubmitRequest(AController* Requester, const FY01ItemRequest& Request);
    FY01ItemResult TryEquip(AController* Requester, const FY01ItemRequest& Request);
    FY01ItemResult TryUnequip(AController* Requester, const FY01ItemRequest& Request);
    FY01ItemResult PrepareLearn(AController* Requester, const FY01ItemRequest& Request);
    FY01ItemResult ConfirmLearn(AController* Requester, const FY01ItemRequest& Request);
    FY01ItemResult CancelLearn(AController* Requester, const FY01ItemRequest& Request);
    FY01ItemResult RemoveSkill(AController* Requester, const FY01ItemRequest& Request);
    // 仅内部服务器配置发布者推进纪元；不替换既有书快照，不是RPC。
    FY01ItemResult SetConfigRevision(uint64 NewRevision);
    bool IsBusy() const { return Transactions.bBusy; }
    FY01ItemResult Observe(FY01LoadoutSnapshot& OutSnapshot) const;
    FY01ItemResult FindItem(const FGuid& ItemId, FY01ItemInstance& OutItem) const;
    // 只读Id解析：持有物Id+来源种类→运行时来源Id；未持有返回空Id，供战斗侧桥接来源定位。
    FY01RuntimeSourceId SourceId(const FGuid& ItemId, EY01RuntimeSourceKind Kind = EY01RuntimeSourceKind::Equipment) const;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    // 唯一UI读取入口；返回自有副本，不拼接旧Gold/ASC属性复制。
    bool ReadCommittedView(FY01CommittedView& Out) const;
    // UI先读轻量会话/连接/版本身份，只有原子发布发生变化时才复制整份视图。
    bool ReadCommittedScope(FY01ViewAck& Out) const;
    FY01ItemResult PublishCommittedView();
    const FString& GetViewDiagnostic() const { return ViewDiagnostic; }
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& Out) const override;
    virtual void PreReplication(IRepChangedPropertyTracker& Tracker) override;
private:
    friend class UY01SourceEffectsComponent;
    friend class UY01DamageEffectsComponent;
    friend class UY01EconomyComponent;
    friend class UY01GrowthComponent;
    friend class UY01DropDeliveryComponent;
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FY01R36EconomyTestAccess;
    friend struct FY01R36DeliveryTestAccess;
#endif
    struct FPreparedBatch
    {
        TMap<FGuid, FY01ItemInstance> Items;
        TArray<FGuid> Order;
        // 叠加道具的预分配Id → 实际合并到的背包格Id。
        TMap<FGuid, FGuid> Merged;
        FY01ItemResult Result;
        uint64 BeforeRevision = 0;
        uint64 AcquisitionSerial = 0;
    };
    FY01ItemResult PrepareBatch(const FY01DataCatalog& Catalog, const TArray<FY01ItemGrant>& Grants,
        const TArray<FY01ItemConsume>& Consumes, FPreparedBatch& Out);
    void ApplyBatch(FPreparedBatch&& Batch);
    FY01ItemResult CommitBatch(AController* Requester, const FY01ItemRequest* Request, FPreparedBatch&& Batch,
        const TArray<FY01ResolvedSource>& Additions, const TArray<FY01RuntimeSourceId>& Removals, double Now,
        TFunctionRef<FY01ItemResult()> CheckCommit, TFunctionRef<void()> CommitOwner, bool bPassiveDelivery = false);
    friend struct FY01ViewWire;
    void ReceiveViewWindow();
    UFUNCTION(Server, Reliable) void ServerAckView(FY01ViewAck Ack);
    UFUNCTION(Server, Reliable) void ServerRestartView(FY01ViewAck Scope);
    bool CheckViewPeer();
    UPROPERTY(Replicated) FY01ViewWire ViewWire;
    UPROPERTY() FY01CommittedView CommittedView;
    UPROPERTY() FY01ViewReceipt LatestResponse;
    uint64 ResponseSerial = 0;
    bool bSubmittingRequest = false;
    FY01ViewSender ViewSender;
    FY01ViewReceiver ViewReceiver;
    FGuid ViewConnectionId;
    TWeakObjectPtr<AController> ViewOwner;
    TWeakObjectPtr<UNetConnection> ViewNetConnection;
    FString ViewDiagnostic;
    friend class UY01CombatAbilitySystem;
    friend class UY01LoadoutAbility;
    bool CanActivate(FGameplayAbilitySpecHandle Handle) const;
    bool BeginAutoAttack() { return !IsBusy() && CanActivate(FGameplayAbilitySpecHandle()) && Transactions.Enter(); }
    void FinishAutoAttack() { Transactions.Leave(); }
    bool BeginDamageExecution() { return !IsBusy() && CanActivate({}) && Transactions.Enter(); }
    void FinishDamageExecution() { Transactions.Leave(); }
    bool BeginActivation(FGameplayAbilitySpecHandle Handle);
    void FinishActivation(FGameplayAbilitySpecHandle Handle);
    FY01ItemResult CheckSkillEntity(const FY01ItemRequest& Request) const;
    FY01ItemResult MutateSkill(AController* Requester, const FY01ItemRequest& Request);
    FY01ItemResult MutateDevour(AController* Requester, const FY01ItemRequest& Request, double Now);
    FY01ItemResult ResolveAbility(const FY01ItemInstance& Book, TSubclassOf<UGameplayAbility>& OutClass) const;
    FY01ItemResult Fail(EY01ItemError Code, const TCHAR* Message) const;
    FY01ItemResult FromSource(const FY01RuntimeResult& Result) const;
    FY01ItemResult CheckOwner() const;
    FY01ItemResult CheckRequester(const AController* Requester) const;
    FY01ItemResult CheckActive() const;
    FY01ItemResult CheckEntity(const FY01ItemRequest& Request) const;
    FY01ItemResult Resolve(const FY01DataCatalog& Catalog, const FY01DataRef& Definition, FY01ItemInstance& Out) const;
    FY01ItemResult Execute(AController* Requester, const FY01ItemRequest& Request, EY01ItemOperation Operation);
    FY01ItemResult Mutate(AController* Requester, const FY01ItemRequest& Request, EY01ItemOperation Operation);
    FY01ItemTransactions Transactions;
    TWeakObjectPtr<AY01PlayerState> Player;
    TWeakObjectPtr<AY01GameState> GameState;
    FGuid MatchId;
    int32 JoinSequence = 0;
    uint64 Revision = 0;
    // 顺序数组仅维护获取顺序；所有Id寻址都是哈希表查找，无背包容量截断。
    UPROPERTY() TMap<FGuid, FY01ItemInstance> Items;
    TArray<FGuid> Order;
    TArray<FGuid> EquipmentSlots;
    UPROPERTY() TArray<FY01LearnedSkill> Skills;
    // 本局已吞噬红装快照：按吞噬顺序，定义行名局内唯一；仅新局重新初始化时清空。
    UPROPERTY() TArray<FY01ItemInstance> Devoured;
    // 每槽最多一张活跃票据，重发仍由64条原结果账本提供原票据。
    TArray<FY01ItemRequest> Tickets;
    uint64 AcquisitionSerial = 0;
    uint64 ConfigRevision = 1;
    FGameplayAbilitySpecHandle ActivatingHandle;
    bool bInitialized = false;
    bool bEnded = false;
};
