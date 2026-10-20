#pragma once
#include "Player/Y01ViewTypes.h"
#include "Match/Y01GameFlow.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/IUserObjectListEntry.h"
#include "Blueprint/DragDropOperation.h"
#include "Components/TileView.h"
#include "Components/Button.h"
#include "Components/CanvasPanel.h"
#include "Y01R36UI.generated.h"

// 只投影完整视图并构造意图，不拥有库存或决定交易成功。
// Devoured为只读展示格，不参与拖放或请求。
enum class EY01R36Zone : uint8 { Backpack, Equipment, Skill, Devoured };
enum class EY01R36PanelKind : uint8 { Character, Backpack, Shop, Devoured };
struct FY01R36Drag
{
    EY01R36Zone Zone = EY01R36Zone::Backpack;
    FGuid MatchId, ConnectionId, ItemId;
    int32 JoinSequence = 0, Slot = INDEX_NONE;
    uint64 EntityRevision = 0;
};
struct FY01R36Flight
{
    FY01DeliveryRecord Record;
    double Alpha = 0;
};
struct FY01R36ProjectedFlight
{
    FY01R36Flight Flight;
    FVector2D Origin = FVector2D::ZeroVector, Target = FVector2D::ZeroVector;
};
UCLASS()
class UY01R36FlightLayer : public UCanvasPanel
{
    GENERATED_BODY()
public:
    void Present(const TArray<FY01R36ProjectedFlight>& Flights);
    static TArray<FY01R36ProjectedFlight> Project(APlayerController* Owner, const TArray<FY01R36Flight>& Flights,
        const FGeometry& ViewportGeometry, const FGeometry& LayerGeometry);
};
struct FY01R36LogSegment
{
    FString Text;
    FLinearColor Color = FLinearColor::White;
};
struct FY01R36LogEntry
{
    TArray<FY01R36LogSegment> Segments;
    bool bFailure = false;
    FString ToString() const;
};
class FY01R36UIModel
{
public:
    static constexpr int32 MaxLogEntries = 3;
    bool Sync(const FY01CommittedView& Next);
    const FY01CommittedView& View() const { return Committed; }
    bool CanInteract(EY01MatchPhase Phase) const;
    bool Drag(EY01R36Zone Zone, FGuid Item, int32 Slot, FY01R36Drag& Out) const;
    bool Drop(const FY01R36Drag& Payload, EY01R36Zone Target, int32 Slot, EY01MatchPhase Phase, FY01ItemRequest& Out) const;
    bool Remove(int32 Slot, EY01MatchPhase Phase, FY01ItemRequest& Out) const;
    bool Decide(bool Confirm, EY01MatchPhase Phase, FY01ItemRequest& Out) const;
    bool Purchase(FName Offer, EY01MatchPhase Phase, FY01ItemRequest& Out) const;
    bool ChooseTalent(FName Talent, EY01MatchPhase Phase, FY01ItemRequest& Out) const;
    // 包内最低可合品质优先，同品质先装备后技能，同组取最早获得的五件。
    bool PickMaterials(TArray<FY01ItemConsume>& Out, FName& Recipe) const;
    bool Synthesize(EY01MatchPhase Phase, FY01ItemRequest& Out) const;
    // 本地预检只用于给出灰色LOG原因；成功与否仍以服务器事务为准。
    bool Devour(FGuid Pill, int32 Slot, EY01MatchPhase Phase, FY01ItemRequest& Out, FString* Reason = nullptr) const;
    const FY01ViewItem* FindDevoured(FGuid Id) const;
    void Sent(const FY01ItemRequest& Request);
    const TArray<FY01R36LogEntry>& GameLog() const { return Log; }
    uint64 GetLogRevision() const { return LogRevision; }
    void LogNotice(const FString& Text);
    bool IsPending() const { return bPending; }
    bool BlocksClosing(EY01R36PanelKind Panel) const;
    static FString WaveLabel(const FY01WaveSnapshot& Snapshot, double ServerTime);
    bool NeedsConfirmation() const;
    const FY01ViewItem* Find(FGuid Id) const;
    TArray<FGuid> Backpack() const;
    // 装卸在途时的纯显示预览；权威视图、拖动和请求仍只用 View()/Backpack()。
    bool IsPreviewing() const;
    FGuid DisplaySlot(int32 Slot) const;
    TArray<FGuid> DisplayBackpack() const;
    FString ItemLabel(FGuid Id) const;
    FString SkillLabel(int32 Slot, double ServerTime) const;
    FString Status() const;
    static FString ErrorText(EY01ItemError Code);
    // 日志区下方的提示行：只显示待视图、覆盖确认等阻塞状态，不重复成功回执。
    FString Hint() const;
    static double Remaining(const FY01ViewSkill& Skill, double ServerTime);
    TArray<FY01R36Flight> Flights(double ServerTime, bool Enabled) const;
    void VisualTick(double ServerTime, const TArray<FGuid>& VisibleItems);
    double FlashRemaining(FGuid Item, double ServerTime) const;
    int32 UnseenArrivals() const;
private:
    struct FArrivalVisual { FGuid DeliveryId; double FirstVisible = 0; bool bSeen = false; };
    TMap<FGuid, FArrivalVisual> Arrivals;
    FY01CommittedView Committed;
    TMap<FGuid, int32> ItemIndices;
    FY01ItemRequest Pending;
    FY01ViewReceipt DisplayResponse;
    uint64 SentAtResponse = 0;
    bool bPending = false;
    TArray<FY01R36LogEntry> Log;
    uint64 LogRevision = 0;
    TSet<FGuid> LoggedDeliveries;
    FY01ItemRequest Base(EY01ItemOperation Operation, int32 Slot) const;
    void AddLog(FY01R36LogEntry Entry);
    void LogReceipt(const FY01ViewReceipt& Receipt, const FY01CommittedView& Next);
};

class UY01R36Panel;
class UTextBlock;
class UProgressBar;
class UBorder;
class UImage;
class UScrollBox;
class UUniformGridPanel;
UCLASS()
class UY01R36EconomyButton : public UButton
{
    GENERATED_BODY()
public:
    void Bind(UY01R36Panel* Owner, EY01ItemOperation Operation, FName Definition);
private:
    UFUNCTION() void Activate();
    TWeakObjectPtr<UY01R36Panel> Panel;
    EY01ItemOperation Intent = EY01ItemOperation::Purchase;
    FName Id;
};
UCLASS()
class UY01R36TileItem : public UObject
{
    GENERATED_BODY()
public:
    FGuid ItemId;
    TWeakObjectPtr<UY01R36Panel> Panel;
};
UCLASS()
class UY01R36DragOperation : public UDragDropOperation
{
    GENERATED_BODY()
public:
    FY01R36Drag Identity;
    TWeakObjectPtr<UY01R36Panel> Origin;
};
UCLASS()
class UY01R36TileView : public UTileView
{
    GENERATED_BODY()
public:
    UY01R36TileView(const FObjectInitializer& Init);
    virtual void PostInitProperties() override;
};
UCLASS()
class UY01R36Cell : public UUserWidget, public IUserObjectListEntry
{
    GENERATED_BODY()
public:
    void Configure(UY01R36Panel* InPanel, EY01R36Zone InZone, int32 InSlot, FGuid InItem = {});
    void Refresh(double ServerTime);
    FGuid GetItemId() const { return ItemId; }
    EY01R36Zone GetZone() const { return Zone; }
    int32 GetSlotIndex() const { return SlotIndex; }
    virtual void NativeOnListItemObjectSet(UObject* Item) override;
    virtual FReply NativeOnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override;
    virtual void NativeOnDragDetected(const FGeometry& Geometry, const FPointerEvent& Event, UDragDropOperation*& Operation) override;
    virtual bool NativeOnDrop(const FGeometry& Geometry, const FDragDropEvent& Event, UDragDropOperation* Operation) override;
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
private:
    TWeakObjectPtr<UY01R36Panel> Panel;
    EY01R36Zone Zone = EY01R36Zone::Backpack;
    int32 SlotIndex = INDEX_NONE;
    FGuid ItemId;
    UPROPERTY() TObjectPtr<UTextBlock> Label;
    // Frame为品质色边框，Fill为暗色底；到货闪光只改边框与底色，不改尺寸。
    UPROPERTY() TObjectPtr<UBorder> Frame;
    UPROPERTY() TObjectPtr<UBorder> Fill;
    UPROPERTY() TObjectPtr<UBorder> Tip;
    UPROPERTY() TObjectPtr<UTextBlock> TipName;
    UPROPERTY() TObjectPtr<UTextBlock> TipBody;
};
UCLASS()
class UY01R36Panel : public UUserWidget
{
    GENERATED_BODY()
public:
    bool Present(const FY01CommittedView& View, EY01MatchPhase Phase, double ServerTime);
    void PresentCoreAlert(const struct FY01CoreSnapshot& Snapshot);
    void PresentRevive(const struct FY01ReviveSnapshot& Snapshot);
    const FY01R36UIModel& GetModel() const { return Model; }
    UY01R36TileView* GetBackpack() const { return Bag; }
    UY01R36Cell* GetCell(EY01R36Zone Zone, int32 Slot) const;
    bool Accept(const UY01R36DragOperation* Operation, EY01R36Zone Target, int32 Slot);
    bool RequestRemove(int32 Slot);
    bool RequestSkill(int32 Slot);
    bool RequestPurchase(FName Offer);
    bool RequestTalent(FName Talent);
    // 点吞噬丹进入/退出吞噬模式；模式内点装备槽发起吞噬，右键取消。
    bool ToggleDevour(FGuid Pill);
    void CancelDevour();
    bool RequestDevour(int32 TargetSlot);
    FGuid GetDevourPill() const { return DevourPillId; }
    // 背包换位只改本机显示顺序，不发服务器事务；新会话恢复获得顺序。
    bool ReorderBackpack(const UY01R36DragOperation* Operation, FGuid Before);
    UFUNCTION() void Synthesize();
    UFUNCTION() void Confirm();
    UFUNCTION() void Cancel();
    UFUNCTION() void Advance();
    UFUNCTION() void Training();
    UFUNCTION() void ToggleCharacter();
    UFUNCTION() void ToggleBackpack();
    UFUNCTION() void ToggleShop();
    UFUNCTION() void ToggleDevoured();
    bool IsCharacterExpanded() const { return bCharacterExpanded; }
    bool IsBackpackExpanded() const { return bBackpackExpanded; }
    bool IsShopExpanded() const { return bShopExpanded; }
    bool IsDevouredExpanded() const { return bDevouredExpanded; }
    const TArray<TObjectPtr<UY01R36Cell>>& GetDevouredCells() const { return DevouredCells; }
    bool CanTogglePanel(EY01R36PanelKind Panel) const;
    bool CanUseZone(EY01R36Zone Zone) const;
    bool CanDrag() const;
    bool CanDragFrom(EY01R36Zone Zone) const { return CanDrag() && CanUseZone(Zone); }
    const FY01WaveSnapshot& GetDisplayedWaves() const { return DisplayedWaves; }
    double GetWaveServerTime() const { return WaveServerTime; }
    FName GetWaveClockSource() const { return WaveClockSource; }
    double GetServerTime() const { return SampleTime; }
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeTick(const FGeometry& Geometry, float DeltaSeconds) override;
    virtual bool NativeOnDrop(const FGeometry& Geometry, const FDragDropEvent& Event, UDragDropOperation* Operation) override;
private:
    bool Send(const FY01ItemRequest& Request);
    void Refresh(bool bRefreshStatic = true);
    void RefreshEconomy();
    void RefreshWaves();
    void RefreshCoreAlert();
    void RefreshRevive();
    void RefreshGameLog();
    void RefreshAttributes();
    void RefreshDevoured();
    void RebuildBag();
    void TogglePanel(EY01R36PanelKind Panel);
    bool& Expanded(EY01R36PanelKind Panel);
    bool IsExpanded(EY01R36PanelKind Panel) const;
    bool bCharacterExpanded = false, bBackpackExpanded = false, bShopExpanded = false, bDevouredExpanded = false;
    FGuid DevourPillId;
    FY01WaveSnapshot DisplayedWaves;
    double WaveServerTime = -1;
    FName WaveClockSource = TEXT("Unknown");
    TArray<FGuid> BackpackOrder;
    FGuid OrderMatchId, OrderConnectionId;
    int32 OrderJoinSequence = 0;
    uint64 ShownLogRevision = MAX_uint64;
    UPROPERTY() TObjectPtr<UCanvasPanel> CharacterPanel;
    UPROPERTY() TObjectPtr<UCanvasPanel> BackpackPanel;
    UPROPERTY() TObjectPtr<UCanvasPanel> ShopPanel;
    UPROPERTY() TObjectPtr<UCanvasPanel> DevouredPanel;
    UPROPERTY() TObjectPtr<UButton> CharacterButton;
    UPROPERTY() TObjectPtr<UButton> BackpackButton;
    UPROPERTY() TObjectPtr<UButton> ShopButton;
    UPROPERTY() TObjectPtr<UButton> DevouredButton;
    UPROPERTY() TObjectPtr<class UWrapBox> DevouredList;
    UPROPERTY() TObjectPtr<UTextBlock> DevouredEmpty;
    UPROPERTY() TArray<TObjectPtr<UY01R36Cell>> DevouredCells;
    UPROPERTY() TObjectPtr<UTextBlock> TopVitals; // 左上金币文本（原金币+生命合并文本，生命改由下方血条展示）
    UPROPERTY() TObjectPtr<UProgressBar> HealthBar;
    UPROPERTY() TObjectPtr<UProgressBar> ShieldBar;
    UPROPERTY() TObjectPtr<UTextBlock> HealthText;
    UPROPERTY() TObjectPtr<UTextBlock> WaveCountdown;
    UPROPERTY() TObjectPtr<UTextBlock> CoreAlert;
    UPROPERTY() TObjectPtr<UTextBlock> ReviveCountdown;
    UPROPERTY() TObjectPtr<UTextBlock> BackpackHint;
    UPROPERTY() TObjectPtr<UTextBlock> GrowthSummary;
    UPROPERTY() TObjectPtr<class UVerticalBox> GameLog;
    UPROPERTY() TMap<FName, TObjectPtr<UTextBlock>> StatValues;
    UPROPERTY() TObjectPtr<UY01R36FlightLayer> FlightLayer;
    FY01R36UIModel Model;
    EY01MatchPhase Phase = EY01MatchPhase::Waiting;
    double SampleTime = -1;
    UPROPERTY() TObjectPtr<UY01R36TileView> Bag;
    UPROPERTY() TArray<TObjectPtr<UY01R36Cell>> Equipment;
    UPROPERTY() TArray<TObjectPtr<UY01R36Cell>> Skills;
    UPROPERTY() TMap<FGuid, TObjectPtr<UY01R36TileItem>> Items;
    UPROPERTY() TObjectPtr<UTextBlock> StatusText;
    UPROPERTY() TObjectPtr<UBorder> Confirmation;
    UPROPERTY() TObjectPtr<UButton> ConfirmButton;
    UPROPERTY() TObjectPtr<UButton> CancelButton;
    UPROPERTY() TObjectPtr<UScrollBox> ShopList;
    UPROPERTY() TObjectPtr<UUniformGridPanel> EquipmentBoxes;
    UPROPERTY() TObjectPtr<UUniformGridPanel> SkillBoxes;
    UPROPERTY() TObjectPtr<UScrollBox> TalentList;
    UPROPERTY() TObjectPtr<UButton> SynthesisButton;
    UPROPERTY() TArray<TObjectPtr<UY01R36EconomyButton>> OfferButtons;
    UPROPERTY() TArray<TObjectPtr<UY01R36EconomyButton>> TalentButtons;
};
