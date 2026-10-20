#include "Playground/Y01R36UI.h"
#include "Combat/Y01DamageEffects.h"
#include "Combat/Y01Core.h"
#include "Components/Border.h"
#include "Components/TextBlock.h"
#include "Components/Image.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ProgressBar.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/ScaleBox.h"
#include "Components/ScrollBox.h"
#include "Components/UniformGridPanel.h"
#include "Components/UniformGridSlot.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WrapBox.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Playground/Y01R36Runtime.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01ShieldComponent.h"
#include "Player/Y01ReviveComponent.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01AttributeResolver.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#if WITH_EDITOR
#include "WidgetBlueprint.h"
#include "Blueprint/WidgetBlueprintGeneratedClass.h"
#include "Kismet2/KismetEditorUtilities.h"
#endif

namespace
{
using Z = EY01R36Zone;
bool VisibleInTree(const UWidget* Widget)
{
    for (const UWidget* W = Widget; W; W = W->GetParent()) { if (!W->IsVisible()) { return false; } }
    return Widget != nullptr;
}
UTextBlock* MakeText(UWidgetTree* Tree, FName Name, const FString& Text, int32 Size = 16)
{
    auto* W = Tree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), Name);
    auto Font = W->GetFont(); Font.Size = Size; W->SetFont(Font); W->SetText(FText::FromString(Text));
    W->SetAutoWrapText(true); W->SetVisibility(ESlateVisibility::HitTestInvisible); return W;
}
void Position(UCanvasPanel* Canvas, UWidget* W, FVector2D At, FVector2D Size, int32 Layer = 0)
{ auto* S = Canvas->AddChildToCanvas(W); S->SetPosition(At); S->SetSize(Size); S->SetZOrder(Layer); }
FString QualityText(EY01Quality Quality)
{
    const TCHAR* Names[] = {TEXT("绿"), TEXT("蓝"), TEXT("紫"), TEXT("金"), TEXT("红")};
    const int32 Q = static_cast<int32>(Quality); return Q >= 0 && Q < UE_ARRAY_COUNT(Names) ? Names[Q] : TEXT("未知品质");
}
FString GrowthAttributeText(EY01GrowthAttribute Attribute)
{
    switch (Attribute)
    {
    case EY01GrowthAttribute::AttackDamage: return TEXT("攻击");
    case EY01GrowthAttribute::Strength: return TEXT("力量");
    case EY01GrowthAttribute::MaxHealth: return TEXT("生命上限");
    case EY01GrowthAttribute::Agility: return TEXT("敏捷");
    case EY01GrowthAttribute::Wisdom: return TEXT("智慧");
    default: return TEXT("未知属性");
    }
}
// 成长量与概率不属于伤害显示截断范围，保留完整小数。
FString GrowthNumber(double Value) { return FString::SanitizeFloat(Value, 0); }
FString EffectText(const TArray<FY01ItemEffect>& Effects, const TArray<FY01ItemGrowth>& Growth)
{
    FString Text;
    for (const auto& Effect : Effects)
    {
        FY01SourceEffectRow Row; Row.Kind = Effect.Kind; Row.Magnitude = Effect.Magnitude;
        Row.SourceAttribute = Effect.SourceAttribute; Row.TargetAttribute = Effect.TargetAttribute; Row.Amount = Effect.Amount;
        Row.Condition = Effect.Condition; Row.ConditionThreshold = Effect.ConditionThreshold;
        Text += FY01AttributeResolver::Describe(Row) + TEXT("\n");
    }
    for (const auto& Rule : Growth)
    {
        const TCHAR* Trigger = Rule.Trigger == EY01GrowthTrigger::CombatTime ? TEXT("战斗每秒")
            : Rule.Trigger == EY01GrowthTrigger::BasicAttack ? TEXT("每次普攻") : TEXT("有效击杀");
        if (Rule.Target == EY01GrowthTarget::EquipmentInstance)
        { Text += FString::Printf(TEXT("%s：装备%s +%s · 实例累计 %s\n"), Trigger, *GrowthAttributeText(Rule.Attribute), *GrowthNumber(Rule.Delta), *GrowthNumber(Rule.Accumulated)); }
        else { Text += FString::Printf(TEXT("%s：玩家%s +%s · 局内累计保留\n"), Trigger, *GrowthAttributeText(Rule.Attribute), *GrowthNumber(Rule.Delta)); }
    }
    return Text;
}
FLinearColor QualityColor(EY01Quality Quality)
{
    const FLinearColor Colors[] = { {.12f,.65f,.28f}, {.15f,.4f,.9f}, {.58f,.22f,.85f}, {.9f,.68f,.12f}, {.85f,.14f,.2f} };
    const int32 Index = static_cast<int32>(Quality); return Index >= 0 && Index < UE_ARRAY_COUNT(Colors) ? Colors[Index] : FLinearColor::Gray;
}
// 暗底上的名称用提亮的品质色，边框保留原品质色。
FLinearColor QualityTextColor(EY01Quality Quality)
{ auto C = FMath::Lerp(QualityColor(Quality), FLinearColor::White, .3f); C.A = 1; return C; }
const FLinearColor LogGrey(.55f,.58f,.62f,1);
FString ItemName(const FY01ViewItem& Item)
{ return Item.Display.Name.IsEmpty() ? TEXT("未命名内容") : Item.Display.Name.ToString(); }
}
TArray<FY01R36ProjectedFlight> UY01R36FlightLayer::Project(APlayerController* Owner, const TArray<FY01R36Flight>& Flights,
    const FGeometry& ViewportGeometry, const FGeometry& LayerGeometry)
{
    TArray<FY01R36ProjectedFlight> Result;
    const APawn* Pawn = IsValid(Owner) && Owner->GetLocalPlayer() ? Owner->GetPawn().Get() : nullptr;
    const auto HostSize = ViewportGeometry.GetLocalSize(), LayerSize = LayerGeometry.GetLocalSize();
    if (!IsValid(Pawn) || Pawn->IsHidden() || HostSize.X <= 0 || HostSize.Y <= 0 || LayerSize.X <= 0 || LayerSize.Y <= 0) { return Result; }
    int32 Width = 0, Height = 0; Owner->GetViewportSize(Width, Height);
    if (Width <= 0 || Height <= 0) { return Result; }
    auto Screen = [&](const FVector& World, FVector2D& Local)
    {
        FVector2D Pixel;
        if (World.ContainsNaN() || !Owner->ProjectWorldLocationToScreen(World, Pixel)
            || !FMath::IsFinite(Pixel.X) || !FMath::IsFinite(Pixel.Y)
            || Pixel.X < 0 || Pixel.Y < 0 || Pixel.X >= Width || Pixel.Y >= Height) { return false; }
        Local = LayerGeometry.AbsoluteToLocal(ViewportGeometry.LocalToAbsolute(Pixel * FVector2D(HostSize.X / Width, HostSize.Y / Height)));
        return FMath::IsFinite(Local.X) && FMath::IsFinite(Local.Y);
    };
    FVector2D Target;
    if (!Screen(Pawn->GetActorLocation(), Target)) { return Result; }
    TSet<FGuid> Seen;
    for (const auto& Flight : Flights)
    {
        FVector2D Origin;
        if (Seen.Contains(Flight.Record.DeliveryId) || !Screen(Flight.Record.Origin, Origin)) { continue; }
        Seen.Add(Flight.Record.DeliveryId); Result.Add({Flight, Origin, Target});
    }
    return Result;
}
void UY01R36FlightLayer::Present(const TArray<FY01R36ProjectedFlight>& Flights)
{
    int32 Used = 0;
    for (const auto& P : Flights)
    {
        for (int32 Trail = 0; Trail < 5; ++Trail)
        {
            if (Used == GetChildrenCount())
            {
                auto* Image = NewObject<UImage>(this);
                Image->SetBrush(FSlateRoundedBoxBrush(FLinearColor::White, FVector2f(16,16)));
                AddChildToCanvas(Image);
            }
            auto* Image = CastChecked<UImage>(GetChildAt(Used++));
            const double T = FMath::Clamp(P.Flight.Alpha - Trail * .035, 0., 1.);
            const double Radius = Trail ? 5. - Trail * .65 : 8.;
            FVector2D Center = FMath::Lerp(P.Origin, P.Target, T);
            Center.Y -= 4 * T * (1 - T) * FMath::Min(100., FVector2D::Distance(P.Origin, P.Target) * .25);
            auto* CanvasSlot = CastChecked<UCanvasPanelSlot>(Image->Slot);
            CanvasSlot->SetPosition(Center - FVector2D(Radius)); CanvasSlot->SetSize(FVector2D(Radius * 2));
            CanvasSlot->SetZOrder(5 - Trail);
            auto Color = QualityColor(P.Flight.Record.Quality); Color.A = Trail ? 1.f - Trail * .18f : 1.f;
            Image->SetColorAndOpacity(Color); Image->SetVisibility(ESlateVisibility::HitTestInvisible);
        }
    }
    for (int32 I = Used; I < GetChildrenCount(); ++I) { GetChildAt(I)->SetVisibility(ESlateVisibility::Collapsed); }
}
UY01R36TileView::UY01R36TileView(const FObjectInitializer& Init) : Super(Init)
{
    // 背包宽456，88方格保证一行最多五格。
    EntryWidgetClass = UY01R36Cell::StaticClass(); EntryWidth = 88; EntryHeight = 88;
    bAllowDragging = true; bAllowDragDrop = false; bEnableRightClickScrolling = false;
    SetSelectionMode(ESelectionMode::None);
}
void UY01R36TileView::PostInitProperties()
{
    Super::PostInitProperties();
#if WITH_EDITOR
    if (!IsTemplate())
    {
        // Editor列表要求真实Blueprint类；仅临时纯继承，布局和交互仍全部来自程序Cell。
        static TWeakObjectPtr<UWidgetBlueprint> Cached;
        if (!Cached.IsValid())
        {
            auto* BP = Cast<UWidgetBlueprint>(FKismetEditorUtilities::CreateBlueprint(UY01R36Cell::StaticClass(),
                GetTransientPackage(), MakeUniqueObjectName(GetTransientPackage(), UWidgetBlueprint::StaticClass(), TEXT("R36CellEntry")),
                BPTYPE_Normal, UWidgetBlueprint::StaticClass(), UWidgetBlueprintGeneratedClass::StaticClass()));
            // 对象构造尚未返回，不能让编译器在这里主动GC未挂到Panel的列表。
            if (BP) { BP->SetFlags(RF_Transient); FKismetEditorUtilities::CompileBlueprint(BP, EBlueprintCompileOptions::SkipGarbageCollection); Cached = BP; }
        }
        EntryWidgetClass = Cached.IsValid() && Cached->Status != BS_Error ? Cached->GeneratedClass : nullptr;
        if (!EntryWidgetClass) { UE_LOG(LogTemp, Error, TEXT("R36临时列表条目编译失败，不能展示背包")); }
    }
#endif
}
TSharedRef<SWidget> UY01R36Cell::RebuildWidget()
{
    if (!Frame)
    {
        Frame = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Frame")); Frame->SetPadding(FMargin(2));
        WidgetTree->RootWidget = Frame;
        Fill = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Fill")); Fill->SetPadding(FMargin(3));
        Fill->SetHorizontalAlignment(HAlign_Fill); Fill->SetVerticalAlignment(VAlign_Center); Frame->SetContent(Fill);
        Label = MakeText(WidgetTree, TEXT("Label"), TEXT(""), 13); Label->SetJustification(ETextJustify::Center); Fill->SetContent(Label);
        // 浮窗不挂在格子根下，仅作为提示内容；名称大字号品质色，正文完整效果。
        Tip = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Tip"));
        Tip->SetBrushColor(FLinearColor(.02f,.03f,.05f,.96f)); Tip->SetPadding(FMargin(10,8));
        auto* Lines = WidgetTree->ConstructWidget<UVerticalBox>(); Tip->SetContent(Lines);
        TipName = MakeText(WidgetTree, TEXT("TipName"), TEXT(""), 18); TipName->SetAutoWrapText(false);
        Lines->AddChildToVerticalBox(TipName);
        TipBody = MakeText(WidgetTree, TEXT("TipBody"), TEXT(""), 13); TipBody->SetAutoWrapText(false); TipBody->SetWrapTextAt(340);
        Lines->AddChildToVerticalBox(TipBody)->SetPadding(FMargin(0,4,0,0));
    }
    return Super::RebuildWidget();
}
void UY01R36Cell::Configure(UY01R36Panel* InPanel, Z InZone, int32 InSlot, FGuid InItem)
{ Panel = InPanel; Zone = InZone; SlotIndex = InSlot; ItemId = InItem; Refresh(Panel.IsValid() ? Panel->GetServerTime() : 0); }
void UY01R36Cell::NativeOnListItemObjectSet(UObject* Object)
{
    IUserObjectListEntry::NativeOnListItemObjectSet(Object);
    auto* Item = Cast<UY01R36TileItem>(Object); Configure(Item ? Item->Panel.Get() : nullptr, Z::Backpack, INDEX_NONE, Item ? Item->ItemId : FGuid());
}
void UY01R36Cell::Refresh(double ServerTime)
{
    if (!Label) { TakeWidget(); }
    const bool Compact = Zone == Z::Equipment || Zone == Z::Skill || Zone == Z::Devoured;
    Label->SetTextOverflowPolicy(Compact ? ETextOverflowPolicy::Ellipsis : ETextOverflowPolicy::Clip);
    auto Font = Label->GetFont(); const int32 FontSize = Compact ? 11 : 13;
    if (Font.Size != FontSize) { Font.Size = FontSize; Label->SetFont(Font); }
    auto SetTip = [&](const FString& Tooltip, const FLinearColor& NameColor)
    {
        // Slate按提示对象身份重新打开；同内容不重建，空槽重绑必须清除旧实例。
        if (GetToolTipText().ToString() == Tooltip) { return; }
        int32 Break = INDEX_NONE; Tooltip.FindChar(TEXT('\n'), Break);
        TipName->SetText(FText::FromString(Break == INDEX_NONE ? Tooltip : Tooltip.Left(Break)));
        TipName->SetColorAndOpacity(FSlateColor(NameColor));
        TipBody->SetText(FText::FromString(Break == INDEX_NONE ? FString() : Tooltip.Mid(Break + 1).TrimEnd()));
        SetToolTipText(FText::FromString(Tooltip)); SetToolTip(Tooltip.IsEmpty() ? nullptr : Tip.Get());
    };
    if (!Panel.IsValid()) { Label->SetText(FText::GetEmpty()); SetTip(FString(), FLinearColor::White); return; }
    const auto& Model = Panel->GetModel(); const auto& V = Model.View(); const FY01ViewDisplay* Display = nullptr;
    FString Tooltip;
    if (Zone == Z::Equipment) { ItemId = Model.DisplaySlot(SlotIndex); }
    else if (Zone == Z::Skill) { ItemId = {}; }
    const FY01ViewItem* Shown = Zone == Z::Devoured ? Model.FindDevoured(ItemId) : Model.Find(ItemId);
    const bool Consumable = Shown && Shown->Definition.Type == EY01TableKind::Consumable;
    if (Shown)
    {
        Display = &Shown->Display;
        Tooltip = Model.ItemLabel(ItemId) + TEXT("\n");
        if (Consumable)
        { Tooltip += FString::Printf(TEXT("数量：%d\n点击后再点装备栏中的红色装备吞噬；再次点击或右键取消\n吞噬后该装备全部效果保留至本局结束\n"), Shown->Count); }
        else if (Zone == Z::Devoured) { Tooltip += TEXT("已吞噬：本局常驻生效，不可卸下\n"); }
        Tooltip += EffectText(Shown->Effects, Shown->Growth) + Y01Damage::Describe(Shown->DamageEffects);
    }
    auto CellName = [](const FY01ViewDisplay& D)
    { return D.Name.IsEmpty() ? FString(TEXT("未命名内容")) : D.Name.ToString().Replace(TEXT("\r"), TEXT("")).Replace(TEXT("\n"), TEXT(" ")); };
    FString Text;
    if (Zone == Z::Skill && V.Skills.IsValidIndex(SlotIndex) && V.Skills[SlotIndex].LearnedId.IsValid())
    {
        const auto& Skill = V.Skills[SlotIndex]; Display = &Skill.Display;
        // 槽内末行显示同一服务器样本的CD；浮窗不含实时CD，避免逐帧重建提示。
        const FString Label36 = Model.SkillLabel(SlotIndex, ServerTime);
        int32 LastLine = INDEX_NONE; Label36.FindLastChar(TEXT('\n'), LastLine);
        Text = CellName(*Display) + TEXT("\n") + Label36.Mid(LastLine + 1);
        Tooltip = Label36.Left(LastLine) + TEXT("\n") + EffectText(Skill.Effects, Skill.Growth) + Y01Damage::Describe(Skill.DamageEffects);
    }
    else if (Display) { Text = CellName(*Display); if (Consumable) { Text += FString::Printf(TEXT("×%d"), Shown->Count); } }
    else if (Compact) { Text = FString::Printf(TEXT("%s %d"), Zone == Z::Equipment ? TEXT("装备") : TEXT("技能"), SlotIndex + 1); }
    Label->SetText(FText::FromString(Text));
    Label->SetColorAndOpacity(FSlateColor(Display ? QualityTextColor(Display->Quality) : LogGrey));
    const bool Flash = Zone == Z::Backpack && Model.FlashRemaining(ItemId, ServerTime) > 0;
    const bool Selected = Zone == Z::Backpack && ItemId.IsValid() && Panel->GetDevourPill() == ItemId;
    Frame->SetBrushColor(Selected ? FLinearColor(.3f,1.f,.95f,1) : Flash ? FLinearColor(1.f,.86f,.3f,1)
        : Display ? QualityColor(Display->Quality) : FLinearColor(.16f,.19f,.24f,1));
    Fill->SetBrushColor(Selected ? FLinearColor(.05f,.22f,.24f,1) : Flash ? FLinearColor(.32f,.27f,.08f,1) : FLinearColor(.03f,.04f,.06f,.96f));
    SetTip(Tooltip, Display ? QualityTextColor(Display->Quality) : FLinearColor::White);
}
FReply UY01R36Cell::NativeOnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (!Panel.IsValid() || !VisibleInTree(this) || Zone == Z::Devoured || !Panel->CanUseZone(Zone)) { return FReply::Unhandled(); }
    const bool Left = Event.GetEffectingButton() == EKeys::LeftMouseButton;
    if (Panel->GetDevourPill().IsValid())
    {
        if (Event.GetEffectingButton() == EKeys::RightMouseButton) { Panel->CancelDevour(); return FReply::Handled(); }
        if (Left && Zone == Z::Equipment) { Panel->RequestDevour(SlotIndex); return FReply::Handled(); }
    }
    const auto* Item = Panel->GetModel().Find(ItemId);
    if (Left && Zone == Z::Backpack && Item && Item->Definition.Type == EY01TableKind::Consumable)
    { return Panel->ToggleDevour(ItemId) ? FReply::Handled() : FReply::Unhandled(); }
    if (Zone == Z::Skill && Left)
    { return Panel->RequestSkill(SlotIndex) ? FReply::Handled() : FReply::Unhandled(); }
    if (Zone == Z::Skill && Event.GetEffectingButton() == EKeys::RightMouseButton)
    { return Panel->RequestRemove(SlotIndex) ? FReply::Handled() : FReply::Unhandled(); }
    FY01R36Drag Identity;
    if (Panel->CanDragFrom(Zone) && Panel->GetModel().Drag(Zone, ItemId, SlotIndex, Identity))
    { return UWidgetBlueprintLibrary::DetectDragIfPressed(Event, this, EKeys::LeftMouseButton).NativeReply; }
    return Super::NativeOnMouseButtonDown(Geometry, Event);
}
void UY01R36Cell::NativeOnDragDetected(const FGeometry&, const FPointerEvent&, UDragDropOperation*& Operation)
{
    Operation = nullptr; FY01R36Drag Identity;
    if (!Panel.IsValid() || !VisibleInTree(this) || !Panel->CanDragFrom(Zone) || !Panel->GetModel().Drag(Zone, ItemId, SlotIndex, Identity)) { return; }
    auto* Drag = NewObject<UY01R36DragOperation>(); Drag->Identity = Identity; Drag->Origin = Panel; Drag->Pivot = EDragPivot::MouseDown;
    auto* Visual = CreateWidget<UY01R36Cell>(GetWorld()); Visual->Configure(Panel.Get(), Zone, SlotIndex, ItemId);
    const double Side = Zone == Z::Backpack ? 88 : 64;
    Visual->SetDesiredSizeInViewport(FVector2D(Side)); Visual->SetVisibility(ESlateVisibility::HitTestInvisible);
    Drag->DefaultDragVisual = Visual; Operation = Drag;
}
bool UY01R36Cell::NativeOnDrop(const FGeometry&, const FDragDropEvent&, UDragDropOperation* Operation)
{
    if (!VisibleInTree(this) || !Panel.IsValid() || Zone == Z::Devoured || !Panel->CanUseZone(Zone)) { return false; }
    const auto* Drag = Cast<UY01R36DragOperation>(Operation);
    if (Zone == Z::Backpack && Drag && Drag->Identity.Zone == Z::Backpack) { return Panel->ReorderBackpack(Drag, ItemId); }
    return Panel->Accept(Drag, Zone, SlotIndex);
}
TSharedRef<SWidget> UY01R36Panel::RebuildWidget()
{
    if (!Bag)
    {
        auto* Root = WidgetTree->ConstructWidget<UCanvasPanel>(); WidgetTree->RootWidget = Root;
        Root->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
        auto* Scale = WidgetTree->ConstructWidget<UScaleBox>(UScaleBox::StaticClass(), TEXT("HUDScale"));
        Scale->SetStretch(EStretch::ScaleToFit); Scale->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
        auto* ScaleSlot = Root->AddChildToCanvas(Scale); ScaleSlot->SetAnchors(FAnchors(0,0,1,1)); ScaleSlot->SetOffsets(FMargin(0)); ScaleSlot->SetZOrder(10);
        auto* DesignSize = WidgetTree->ConstructWidget<USizeBox>(); DesignSize->SetWidthOverride(1280); DesignSize->SetHeightOverride(720);
        DesignSize->SetVisibility(ESlateVisibility::SelfHitTestInvisible); Scale->SetContent(DesignSize);
        auto* Canvas = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("HUDCanvas"));
        Canvas->SetVisibility(ESlateVisibility::SelfHitTestInvisible); DesignSize->SetContent(Canvas);
        auto Panel = [&](const TCHAR* Name, const TCHAR* Title, FVector2D At, FVector2D Size)
        {
            auto* Parent = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), Name);
            Position(Canvas, Parent, At, Size); Parent->SetVisibility(ESlateVisibility::Collapsed);
            auto* Background = WidgetTree->ConstructWidget<UBorder>(); Background->SetBrushColor(FLinearColor(.025f,.04f,.065f,.97f));
            Background->SetVisibility(ESlateVisibility::HitTestInvisible); Position(Parent, Background, {0,0}, Size);
            Position(Parent, MakeText(WidgetTree, NAME_None, Title, 20), {10,8}, {Size.X-20,30}); return Parent;
        };
        CharacterPanel = Panel(TEXT("CharacterPanel"), TEXT("天赋"), {852,452}, {264,188});
        BackpackPanel = Panel(TEXT("BackpackPanel"), TEXT("背包 · 拖动换位 / 拖回卸下"), {368,96}, {472,350});
        ShopPanel = Panel(TEXT("ShopPanel"), TEXT("商店"), {852,96}, {264,350});
        DevouredPanel = Panel(TEXT("DevouredPanel"), TEXT("已吞噬装备"), {368,452}, {472,188});
        auto* DevouredScroll = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("DevouredScroll"));
        Position(DevouredPanel, DevouredScroll, {8,44}, {456,136});
        DevouredList = WidgetTree->ConstructWidget<UWrapBox>(UWrapBox::StaticClass(), TEXT("DevouredList"));
        DevouredList->SetInnerSlotPadding(FVector2D(6, 6)); DevouredScroll->AddChild(DevouredList);
        DevouredEmpty = MakeText(WidgetTree, TEXT("DevouredEmpty"), TEXT("暂无已吞噬装备"), 14);
        DevouredEmpty->SetColorAndOpacity(FSlateColor(LogGrey)); Position(DevouredPanel, DevouredEmpty, {12,48}, {448,24});
        FlightLayer = WidgetTree->ConstructWidget<UY01R36FlightLayer>(UY01R36FlightLayer::StaticClass(), TEXT("DeliveryFlightLayer"));
        FlightLayer->SetVisibility(ESlateVisibility::HitTestInvisible);
        auto* FlightSlot = Root->AddChildToCanvas(FlightLayer); FlightSlot->SetAnchors(FAnchors(0,0,1,1));
        FlightSlot->SetOffsets(FMargin(0)); FlightSlot->SetZOrder(0);
        // 左上改为金币文本 + 生命血条 + 护盾覆盖条；金币行固定一行高，血量不再拼进文本。
        TopVitals = MakeText(WidgetTree, TEXT("TopVitals"), TEXT("等待服务器"), 20);
        TopVitals->SetColorAndOpacity(FSlateColor(FLinearColor(1.f,.88f,.32f,1.f)));
        Position(Canvas, TopVitals, {16,12}, {410,24});
        auto MakeBar = [&](const TCHAR* Name, FLinearColor Fill, int32 Z)
        {
            auto* Bar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), Name);
            Bar->SetFillColorAndOpacity(Fill);
            Bar->WidgetStyle.BackgroundImage.TintColor = FSlateColor(FLinearColor(0,0,0,.55f));
            Bar->SetVisibility(ESlateVisibility::HitTestInvisible);
            Position(Canvas, Bar, {16,44}, {410,20}, Z);
            return Bar;
        };
        HealthBar = MakeBar(TEXT("HealthBar"), FLinearColor(.85f,.1f,.1f,1), 1);
        ShieldBar = MakeBar(TEXT("ShieldBar"), FLinearColor(.25f,.55f,1.f,.65f), 2);
        ShieldBar->SetPercent(0);
        HealthText = MakeText(WidgetTree, TEXT("HealthText"), TEXT(""), 12);
        HealthText->SetAutoWrapText(false); HealthText->SetJustification(ETextJustify::Center);
        HealthText->SetShadowOffset(FVector2D(1,1)); HealthText->SetShadowColorAndOpacity(FLinearColor(0,0,0,.9f));
        Position(Canvas, HealthText, {16,44}, {410,20}, 3);
        ReviveCountdown = MakeText(WidgetTree, TEXT("ReviveCountdown"), TEXT(""), 18);
        ReviveCountdown->SetAutoWrapText(false);
        ReviveCountdown->SetColorAndOpacity(FSlateColor(FLinearColor(1.f,.88f,.32f,1.f)));
        Position(Canvas, ReviveCountdown, {16,68}, {410,24}, 20);
        ReviveCountdown->SetVisibility(ESlateVisibility::Collapsed);
        WaveCountdown = MakeText(WidgetTree, TEXT("WaveCountdown"), TEXT("等待波次同步"), 24);
        Position(Canvas, WaveCountdown, {440,18}, {824,68});
        CoreAlert = MakeText(WidgetTree, TEXT("CoreAlert"), TEXT(""), 20);
        CoreAlert->SetAutoWrapText(false); CoreAlert->SetJustification(ETextJustify::Center);
        CoreAlert->SetColorAndOpacity(FSlateColor(FLinearColor(1,.12f,.08f,1)));
        Position(Canvas, CoreAlert, {440,58}, {824,30}, 20); CoreAlert->SetVisibility(ESlateVisibility::Collapsed);
        // 基础属性常驻左侧：无底色小字号，阴影保证场景上可读；行仍可悬停看来源明细。
        auto* Stats = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("AttributeList"));
        Position(Canvas, Stats, {16,96}, {340,290});
        const TCHAR* Keys[] = {TEXT("AttackDamage"), TEXT("AttackInterval"), TEXT("Range"), TEXT("Armor"), TEXT("HealthRegen"),
            TEXT("Strength"), TEXT("Agility"), TEXT("Wisdom"), TEXT("IncomeRate"), TEXT("GoldGain")};
        const TCHAR* Labels[] = {TEXT("攻击"), TEXT("攻击间隔"), TEXT("攻击范围"), TEXT("护甲"), TEXT("生命恢复"),
            TEXT("力量"), TEXT("敏捷"), TEXT("智慧"), TEXT("金币/秒"), TEXT("金币获取")};
        auto Shadow = [](UTextBlock* T) { T->SetShadowOffset(FVector2D(1,1)); T->SetShadowColorAndOpacity(FLinearColor(0,0,0,.85f)); };
        for (int32 I = 0; I < UE_ARRAY_COUNT(Keys); ++I)
        {
            auto* RowSize = WidgetTree->ConstructWidget<USizeBox>(); RowSize->SetHeightOverride(17);
            auto* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), FName(*FString::Printf(TEXT("StatRow_%s"), Keys[I])));
            Row->SetBrushColor(FLinearColor::Transparent); Row->SetPadding(FMargin(2,0));
            RowSize->SetContent(Row); Stats->AddChild(RowSize);
            auto* Pair = WidgetTree->ConstructWidget<UHorizontalBox>(); Row->SetContent(Pair);
            auto* Label = MakeText(WidgetTree, FName(*FString::Printf(TEXT("StatLabel_%s"), Keys[I])), Labels[I], 12);
            Label->SetColorAndOpacity(FSlateColor(FLinearColor(.88f,.94f,1.f,1))); Label->SetAutoWrapText(false); Shadow(Label);
            Pair->AddChildToHorizontalBox(Label)->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
            auto* Value = MakeText(WidgetTree, FName(*FString::Printf(TEXT("StatValue_%s"), Keys[I])), TEXT("—"), 12);
            Value->SetColorAndOpacity(FSlateColor(FLinearColor(1.f,.88f,.4f,1))); Value->SetAutoWrapText(false); Shadow(Value);
            Pair->AddChildToHorizontalBox(Value)->SetPadding(FMargin(8,0,0,0));
            CastChecked<UHorizontalBoxSlot>(Value->Slot)->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); StatValues.Add(Keys[I], Value);
        }
        GrowthSummary = MakeText(WidgetTree, TEXT("GrowthSummary"), TEXT(""), 12);
        GrowthSummary->SetColorAndOpacity(FSlateColor(FLinearColor(.6f,1.f,.7f,1))); Shadow(GrowthSummary);
        Position(Canvas, GrowthSummary, {16,390}, {340,40}); GrowthSummary->SetVisibility(ESlateVisibility::Collapsed);
        GameLog = WidgetTree->ConstructWidget<UVerticalBox>(UVerticalBox::StaticClass(), TEXT("GameLog"));
        GameLog->SetVisibility(ESlateVisibility::HitTestInvisible); Position(Canvas, GameLog, {16,434}, {340,152});
        Bag = WidgetTree->ConstructWidget<UY01R36TileView>(UY01R36TileView::StaticClass(), TEXT("Backpack"));
        Position(BackpackPanel, Bag, {8,44}, {456,264});
        Bag->SetHorizontalEntrySpacing(0); Bag->SetVerticalEntrySpacing(0);
        BackpackHint = MakeText(WidgetTree, TEXT("BackpackHint"), TEXT(""), 13); Position(BackpackPanel, BackpackHint, {8,314}, {316,24});
        // 装备栏与技能栏同背包方格规则：品质色边框、格内名称；容量仍为6装备+4技能。
        for (int32 I = 0; I < 6; ++I)
        {
            auto* Cell = CreateWidget<UY01R36Cell>(GetWorld()); Cell->Configure(this, Z::Equipment, I); Equipment.Add(Cell);
            Cell->Rename(*FString::Printf(TEXT("EquipmentSlot_%d"), I), WidgetTree);
            Position(Canvas, Cell, {16. + I * 70, 648}, {64,64});
        }
        for (int32 I = 0; I < 4; ++I)
        {
            auto* Cell = CreateWidget<UY01R36Cell>(GetWorld()); Cell->Configure(this, Z::Skill, I); Skills.Add(Cell);
            Cell->Rename(*FString::Printf(TEXT("SkillSlot_%d"), I), WidgetTree);
            Position(Canvas, Cell, {466. + I * 70, 648}, {64,64});
        }
        StatusText = MakeText(WidgetTree, TEXT("Status"), Model.Hint(), 12); Shadow(StatusText);
        StatusText->SetColorAndOpacity(FSlateColor(FLinearColor(.85f,.9f,1.f,1))); Position(Canvas, StatusText, {16,590}, {340,52});
        auto Button = [&](FName Name, const TCHAR* Text, FVector2D At, FVector2D Size, UCanvasPanel* Parent)
        {
            auto* B = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
            auto* Label = MakeText(WidgetTree, NAME_None, Text, Size.X <= 134 ? 14 : 16);
            Label->SetAutoWrapText(false); Label->SetJustification(ETextJustify::Center);
            B->SetContent(Label);
            CastChecked<UButtonSlot>(Label->Slot)->SetHorizontalAlignment(HAlign_Fill);
            Position(Parent, B, At, Size); return B;
        };
        CharacterButton = Button(TEXT("ToggleCharacter"), TEXT("天赋"), {1130,96}, {134,44}, Canvas);
        BackpackButton = Button(TEXT("ToggleBackpack"), TEXT("背包"), {1130,148}, {134,44}, Canvas);
        ShopButton = Button(TEXT("ToggleShop"), TEXT("商店"), {1130,200}, {134,44}, Canvas);
        DevouredButton = Button(TEXT("ToggleDevoured"), TEXT("已吞噬"), {1130,252}, {134,44}, Canvas);
        CharacterButton->OnClicked.AddDynamic(this, &ThisClass::ToggleCharacter);
        BackpackButton->OnClicked.AddDynamic(this, &ThisClass::ToggleBackpack);
        ShopButton->OnClicked.AddDynamic(this, &ThisClass::ToggleShop);
        DevouredButton->OnClicked.AddDynamic(this, &ThisClass::ToggleDevoured);
        auto* Room = Button(TEXT("Training"), TEXT("练功房"), {1130,398}, {134,48}, Canvas); Room->OnClicked.AddDynamic(this, &ThisClass::Training);
        Room->SetToolTipText(FText::FromString(TEXT("进入 / 返回练功房")));
        ShopList = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("ShopList"));
        Position(ShopPanel, ShopList, {8,44}, {248,298});
        for (auto* Grid : {&EquipmentBoxes, &SkillBoxes})
        {
            *Grid = WidgetTree->ConstructWidget<UUniformGridPanel>(UUniformGridPanel::StaticClass(),
                Grid == &EquipmentBoxes ? TEXT("ShopEquipmentBoxes") : TEXT("ShopSkillBoxes"));
            (*Grid)->SetSlotPadding(FMargin(2)); (*Grid)->SetVisibility(ESlateVisibility::Collapsed); ShopList->AddChild(*Grid);
        }
        SynthesisButton = Button(TEXT("Synthesize"), TEXT("合成"), {332,310}, {132,34}, BackpackPanel);
        SynthesisButton->OnClicked.AddDynamic(this, &ThisClass::Synthesize);
        TalentList = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("TalentList"));
        Position(CharacterPanel, TalentList, {8,44}, {248,136});
        Confirmation = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), TEXT("Confirmation"));
        Confirmation->SetBrushColor(FLinearColor(.04f,.08f,.13f,1)); Confirmation->SetPadding(FMargin(12));
        auto* Dialog = WidgetTree->ConstructWidget<UCanvasPanel>(); Confirmation->SetContent(Dialog);
        Position(Dialog, MakeText(WidgetTree, TEXT("ConfirmationText"), TEXT("覆盖已学技能？确认消耗新书，旧技能不返书。")), {0,0}, {590,34});
        ConfirmButton = Button(TEXT("Confirm"), TEXT("确认覆盖"), {16,48}, {250,40}, Dialog); ConfirmButton->OnClicked.AddDynamic(this, &ThisClass::Confirm);
        CancelButton = Button(TEXT("Cancel"), TEXT("取消"), {304,48}, {250,40}, Dialog); CancelButton->OnClicked.AddDynamic(this, &ThisClass::Cancel);
        Position(Canvas, Confirmation, {328,264}, {624,126}, 100); Confirmation->SetVisibility(ESlateVisibility::Collapsed);
    }
    return Super::RebuildWidget();
}
bool UY01R36Panel::Present(const FY01CommittedView& View, EY01MatchPhase InPhase, double ServerTime)
{
    const double TimingStart = Y01ViewTiming::Now();
    if (!Bag) { TakeWidget(); }
    Phase = InPhase; SampleTime = FMath::IsFinite(ServerTime) && ServerTime >= 0 ? ServerTime : -1;
    RefreshWaves(); RefreshCoreAlert(); RefreshRevive(); const bool Changed = Model.Sync(View);
    if (Changed) { RebuildBag(); }
    Refresh(Changed);
    if (Changed) { Y01ViewTiming::Mark(TEXT("UI_PRESENT"), Y01ViewTiming::Scope(Model.View()), TimingStart, 0, Model.View().Response.Request.Seq, Items.Num()); }
    return Changed;
}
void UY01R36Panel::RebuildBag()
{
    const auto& V = Model.View();
    if (V.MatchId != OrderMatchId || V.JoinSequence != OrderJoinSequence || V.ConnectionId != OrderConnectionId)
    { BackpackOrder.Reset(); OrderMatchId = V.MatchId; OrderJoinSequence = V.JoinSequence; OrderConnectionId = V.ConnectionId; }
    TArray<FGuid> Order = Model.DisplayBackpack();
    // 未手动换位时完全跟随服务器获得顺序；换位后保留本机顺序，新入包追加末尾。
    if (!BackpackOrder.IsEmpty())
    {
        TArray<FGuid> Merged; Merged.Reserve(Order.Num()); const TSet<FGuid> Current(Order);
        for (const auto& Id : BackpackOrder) { if (Current.Contains(Id)) { Merged.Add(Id); } }
        const TSet<FGuid> Kept(Merged);
        for (const auto& Id : Order) { if (!Kept.Contains(Id)) { Merged.Add(Id); } }
        // 预览期间不改写本机顺序，被拒绝时物品回到原位置。
        Order = MoveTemp(Merged); if (!Model.IsPreviewing()) { BackpackOrder = Order; }
    }
    TMap<FGuid, TObjectPtr<UY01R36TileItem>> Next; TArray<UObject*> List;
    for (const auto& Id : Order)
    {
        auto* Existing = Items.Find(Id); auto* Item = Existing ? Existing->Get() : NewObject<UY01R36TileItem>(this);
        Item->ItemId = Id; Item->Panel = this; Next.Add(Id, Item); List.Add(Item);
    }
    const bool ChangedList = List != Bag->GetListItems(); Items = MoveTemp(Next);
    if (ChangedList) { Bag->SetListItems(List); }
}
bool UY01R36Panel::ReorderBackpack(const UY01R36DragOperation* Operation, FGuid Before)
{
    if (!Operation || Operation->Origin.Get() != this || Operation->Identity.Zone != Z::Backpack || !CanUseZone(Z::Backpack)) { return false; }
    const auto& V = Model.View(); const auto& Id = Operation->Identity;
    const auto* Item = Model.Find(Id.ItemId);
    if (Id.MatchId != V.MatchId || Id.JoinSequence != V.JoinSequence || Id.ConnectionId != V.ConnectionId
        || !Item || Item->Location != EY01ItemLocation::Backpack || Id.ItemId == Before) { return false; }
    TArray<FGuid> Order;
    for (const auto* Object : Bag->GetListItems()) { if (const auto* Tile = Cast<UY01R36TileItem>(Object)) { Order.Add(Tile->ItemId); } }
    const int32 From = Order.IndexOfByKey(Id.ItemId);
    if (From == INDEX_NONE) { return false; }
    // 放到目标格即占据目标位置：向后拖插在目标之后，向前拖插在目标之前；空白处放到末尾。
    const int32 Target = Before.IsValid() ? Order.IndexOfByKey(Before) : INDEX_NONE;
    Order.RemoveAt(From);
    Order.Insert(Id.ItemId, Target == INDEX_NONE ? Order.Num() : Target);
    BackpackOrder = MoveTemp(Order); RebuildBag(); Refresh(false); return true;
}
UY01R36Cell* UY01R36Panel::GetCell(Z Zone, int32 Index) const
{
    if (Zone == Z::Equipment && Equipment.IsValidIndex(Index)) { return Equipment[Index]; }
    if (Zone == Z::Skill && Skills.IsValidIndex(Index)) { return Skills[Index]; }
    return nullptr;
}
bool UY01R36Panel::CanUseZone(Z Zone) const
{ return VisibleInTree(this) && (Zone != Z::Backpack || (bBackpackExpanded && VisibleInTree(BackpackPanel) && VisibleInTree(Bag))); }
bool UY01R36Panel::CanDrag() const
{ return VisibleInTree(this) && Model.CanInteract(Phase) && !Model.NeedsConfirmation(); }
bool& UY01R36Panel::Expanded(EY01R36PanelKind Panel)
{
    switch (Panel)
    {
    case EY01R36PanelKind::Character: return bCharacterExpanded;
    case EY01R36PanelKind::Backpack: return bBackpackExpanded;
    case EY01R36PanelKind::Shop: return bShopExpanded;
    default: return bDevouredExpanded;
    }
}
bool UY01R36Panel::IsExpanded(EY01R36PanelKind Panel) const { return const_cast<UY01R36Panel*>(this)->Expanded(Panel); }
bool UY01R36Panel::CanTogglePanel(EY01R36PanelKind Panel) const
{
    if (!IsExpanded(Panel)) { return true; }
    const auto* Drag = Cast<UY01R36DragOperation>(UWidgetBlueprintLibrary::GetDragDroppingContent());
    const bool RelatedDrag = Panel == EY01R36PanelKind::Backpack && Drag && Drag->Origin.Get() == this
        && (Drag->Identity.Zone == Z::Backpack || Drag->Identity.Zone == Z::Equipment);
    return !Model.BlocksClosing(Panel) && !RelatedDrag;
}
void UY01R36Panel::TogglePanel(EY01R36PanelKind Panel)
{
    if (!Bag) { TakeWidget(); }
    if (!CanTogglePanel(Panel)) { Refresh(); return; }
    bool& Flag = Expanded(Panel);
    Flag = !Flag;
    UCanvasPanel* Containers[] = { CharacterPanel.Get(), BackpackPanel.Get(), ShopPanel.Get(), DevouredPanel.Get() };
    Containers[static_cast<uint8>(Panel)]->SetVisibility(Flag ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed);
    // 收起背包时吞噬丹不可见，退出吞噬模式避免误点装备槽。
    if (Panel == EY01R36PanelKind::Backpack && !Flag) { DevourPillId = {}; }
    Refresh();
}
void UY01R36Panel::ToggleCharacter() { TogglePanel(EY01R36PanelKind::Character); }
void UY01R36Panel::ToggleBackpack() { TogglePanel(EY01R36PanelKind::Backpack); }
void UY01R36Panel::ToggleShop() { TogglePanel(EY01R36PanelKind::Shop); }
void UY01R36Panel::ToggleDevoured() { TogglePanel(EY01R36PanelKind::Devoured); }
bool UY01R36Panel::ToggleDevour(FGuid Pill)
{
    if (!CanDragFrom(Z::Backpack) || !Pill.IsValid()) { return false; }
    DevourPillId = DevourPillId == Pill ? FGuid() : Pill; Refresh(false); return true;
}
void UY01R36Panel::CancelDevour() { if (DevourPillId.IsValid()) { DevourPillId = {}; Refresh(false); } }
bool UY01R36Panel::RequestDevour(int32 TargetSlot)
{
    if (!DevourPillId.IsValid()) { return false; }
    FY01ItemRequest R; FString Reason;
    // 非法目标只记灰色LOG并保留吞噬模式，不发请求、不消耗任何物品。
    if (!Model.Devour(DevourPillId, TargetSlot, Phase, R, &Reason))
    { if (!Reason.IsEmpty()) { Model.LogNotice(Reason); } Refresh(false); return false; }
    DevourPillId = {}; return Send(R);
}
void UY01R36EconomyButton::Bind(UY01R36Panel* Owner, EY01ItemOperation Operation, FName Definition)
{ Panel = Owner; Intent = Operation; Id = Definition; OnClicked.AddUniqueDynamic(this, &ThisClass::Activate); }
void UY01R36EconomyButton::Activate()
{
    if (!Panel.IsValid() || !GetIsEnabled()) { return; }
    if (Intent == EY01ItemOperation::Purchase) { Panel->RequestPurchase(Id); }
    else if (Intent == EY01ItemOperation::ChooseTalent) { Panel->RequestTalent(Id); }
}
bool UY01R36Panel::RequestPurchase(FName Offer)
{ FY01ItemRequest R; return bShopExpanded && VisibleInTree(ShopPanel) && Model.Purchase(Offer, Phase, R) && Send(R); }
bool UY01R36Panel::RequestTalent(FName Talent)
{ FY01ItemRequest R; return bCharacterExpanded && VisibleInTree(CharacterPanel) && Model.ChooseTalent(Talent, Phase, R) && Send(R); }
void UY01R36Panel::Synthesize()
{
    if (!CanUseZone(Z::Backpack)) { return; }
    FY01ItemRequest R;
    if (Model.Synthesize(Phase, R)) { Send(R); }
    else if (Model.CanInteract(Phase) && !Model.NeedsConfirmation()) { Model.LogNotice(TEXT("没有可以合成的")); Refresh(false); }
}
bool UY01R36Panel::Send(const FY01ItemRequest& Request)
{
    if (!VisibleInTree(this)) { return false; }
    auto* PC = Cast<AY01R36Controller>(GetOwningPlayer()); if (!PC || !PC->IsLocalController()) { return false; }
    const auto* State = PC->GetPlayerState<AY01PlayerState>(); FY01CommittedView OwnerView;
    if (!IsValid(State) || State->GetOwner() != PC || !State->GetLoadout()->ReadCommittedView(OwnerView)
        || Request.MatchId != OwnerView.MatchId || Request.JoinSequence != OwnerView.JoinSequence
        || Model.View().ConnectionId != OwnerView.ConnectionId) { return false; }
    Y01ViewTiming::Mark(TEXT("REQUEST_SENT"), Y01ViewTiming::Scope(OwnerView), 0, 0, Request.Seq, OwnerView.Items.Num());
    Model.Sent(Request); PC->ServerItemRequest(Request);
    if (Model.IsPreviewing()) { RebuildBag(); }
    Refresh(); return true;
}
bool UY01R36Panel::Accept(const UY01R36DragOperation* Operation, Z Target, int32 Index)
{
    FY01ItemRequest Request;
    return CanDrag() && CanUseZone(Target) && Operation && Operation->Origin.Get() == this && CanUseZone(Operation->Identity.Zone)
        && Model.Drop(Operation->Identity, Target, Index, Phase, Request) && Send(Request);
}
bool UY01R36Panel::RequestRemove(int32 Index)
{ FY01ItemRequest Request; return CanUseZone(Z::Skill) && Model.Remove(Index, Phase, Request) && Send(Request); }
bool UY01R36Panel::RequestSkill(int32 Index)
{
    auto* PC = Cast<AY01R36Controller>(GetOwningPlayer()); const auto& V = Model.View(); FY01CommittedView OwnerView;
    const auto* State = PC ? PC->GetPlayerState<AY01PlayerState>() : nullptr;
    if (!CanUseZone(Z::Skill) || !PC || !PC->IsLocalController() || !IsValid(State) || State->GetOwner() != PC
        || !V.Skills.IsValidIndex(Index) || !V.Skills[Index].LearnedId.IsValid() || Model.NeedsConfirmation()
        || !State->GetLoadout()->ReadCommittedView(OwnerView) || V.MatchId != OwnerView.MatchId
        || V.ConnectionId != OwnerView.ConnectionId || V.JoinSequence != OwnerView.JoinSequence) { return false; }
    PC->ServerPlaygroundAction(Index); return true;
}
void UY01R36Panel::Confirm() { FY01ItemRequest R; if (Model.Decide(true, Phase, R)) { Send(R); } }
void UY01R36Panel::Cancel() { FY01ItemRequest R; if (Model.Decide(false, Phase, R)) { Send(R); } }
void UY01R36Panel::Advance() { if (auto* PC = Cast<AY01R36Controller>(GetOwningPlayer())) { PC->ServerPlaygroundAction(4); } }
void UY01R36Panel::Training()
{ if (auto* PC = Cast<AY01R36Controller>(GetOwningPlayer())) { PC->ServerPlaygroundAction(PC->GetObserver().Training.bInside ? 7 : 6); } }
void UY01R36Panel::NativeTick(const FGeometry& Geometry, float DeltaSeconds)
{
    Super::NativeTick(Geometry, DeltaSeconds);
    RefreshCoreAlert(); RefreshRevive();
    auto* PC = GetOwningPlayer(); const auto* State = PC ? PC->GetPlayerState<AY01PlayerState>() : nullptr;
    const auto* GS = GetWorld() ? GetWorld()->GetGameState<AY01GameState>() : nullptr;
    FY01CommittedView View; FY01ViewAck Scope;
    const auto& Shown = Model.View();
    // 会话、连接与版本均未变时已发布视图必与模型副本相同，不再逐帧复制整份视图。
    const bool Unchanged = State && State->GetLoadout()->ReadCommittedScope(Scope) && Scope.ViewRevision == Shown.ViewRevision
        && Scope.MatchId == Shown.MatchId && Scope.JoinSequence == Shown.JoinSequence && Scope.ConnectionId == Shown.ConnectionId;
    if (State && State->GetOwner() == PC && GS && (Unchanged || State->GetLoadout()->ReadCommittedView(View)))
    {
        const auto* R36 = Cast<AY01R36Controller>(PC);
        const double Time = PC->HasAuthority() ? GS->GetServerWorldTimeSeconds()
            : R36 && R36->GetObserver().bReady ? R36->GetObserver().ServerSampleTime : -1;
        Present(Unchanged ? Shown : View, GS->GetWaveSnapshot().Phase, Time);
        const bool Show = Model.View().Health > 0 && Phase != EY01MatchPhase::Ended;
        if (FlightLayer) { FlightLayer->Present(UY01R36FlightLayer::Project(PC, Model.Flights(SampleTime, Show), Geometry, FlightLayer->GetCachedGeometry())); }
    }
    else
    {
        RefreshWaves();
        if (WaveCountdown) { WaveCountdown->SetText(FText::FromString(FY01R36UIModel::WaveLabel(DisplayedWaves, WaveServerTime))); }
        if (FlightLayer) { FlightLayer->Present({}); }
    }
}
void UY01R36Panel::PresentCoreAlert(const FY01CoreSnapshot& Snapshot)
{
    if (!CoreAlert) { TakeWidget(); }
    const FString Text = Snapshot.AlertText();
    CoreAlert->SetText(FText::FromString(Text));
    CoreAlert->SetVisibility(Text.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
    // 仅警报显示时给顶部第二行让位，不改人物/背包/底部槽位布局。
    if (auto* WaveSlot = Cast<UCanvasPanelSlot>(WaveCountdown->Slot)) { WaveSlot->SetSize(FVector2D(824, Text.IsEmpty() ? 68 : 36)); }
    WaveCountdown->SetAutoWrapText(Text.IsEmpty());
}
void UY01R36Panel::RefreshCoreAlert()
{
    const auto* GS = GetWorld() ? GetWorld()->GetGameState<AY01GameState>() : nullptr;
    const auto* Core = GS ? GS->GetMatchCore() : nullptr;
    // 与玩家区域、库存视图是否就绪无关；只消费同包服务器时间，不用客户端时钟外推。
    PresentCoreAlert(IsValid(Core) ? Core->GetSnapshot() : FY01CoreSnapshot());
}
void UY01R36Panel::PresentRevive(const FY01ReviveSnapshot& S)
{
    if (!ReviveCountdown) { TakeWidget(); }
    const bool Show = S.DeathSequence > 0 && S.bWaiting && !S.bCancelled
        && FMath::IsFinite(S.DeathTime) && FMath::IsFinite(S.Deadline) && FMath::IsFinite(S.ServerSampleTime)
        && S.DeathTime >= 0 && S.Deadline >= S.DeathTime && S.ServerSampleTime >= S.DeathTime;
    const double Remaining = Show ? FMath::Max(0., S.Deadline - S.ServerSampleTime) : 0;
    const FString Text = !Show ? FString() : Remaining > 0
        ? FString::Printf(TEXT("复活倒计时 %.1f 秒"), Remaining) : FString(TEXT("等待安全复活"));
    ReviveCountdown->SetText(FText::FromString(Text));
    ReviveCountdown->SetVisibility(Show ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
    // 左上改为固定三行（金币 12-36、血条/护盾 44-64），复活倒计时固定在 {16,68} 起始，不再压缩金币行让位。
}
void UY01R36Panel::RefreshRevive()
{
    const auto* PC = GetOwningPlayer();
    const auto* P = PC ? PC->GetPlayerState<AY01PlayerState>() : nullptr;
    // 仅消费本连接的整包权威样本；客户端不递减计时，不触发复活。
    PresentRevive(IsValid(P) && P->GetOwner() == PC ? P->GetRevive()->GetSnapshot() : FY01ReviveSnapshot());
}
void UY01R36Panel::RefreshWaves()
{
    const auto* PC = Cast<AY01R36Controller>(GetOwningPlayer());
    const auto* GS = GetWorld() ? GetWorld()->GetGameState<AY01GameState>() : nullptr;
    DisplayedWaves = {}; WaveServerTime = -1; WaveClockSource = TEXT("Unknown");
    if (PC && !PC->HasAuthority())
    {
        // 客户端只使用同一次Observer服务器样本；不拿本地时钟推算deadline。
        const auto& O = PC->GetObserver();
        if (O.bReady && FMath::IsFinite(O.ServerSampleTime) && O.ServerSampleTime >= 0)
        { DisplayedWaves = O.Waves; WaveServerTime = O.ServerSampleTime; WaveClockSource = TEXT("ObserverHeld"); }
        SampleTime = WaveServerTime; Phase = DisplayedWaves.Phase;
    }
    else if (GS)
    { DisplayedWaves = GS->GetWaveSnapshot(); WaveServerTime = SampleTime; WaveClockSource = TEXT("AuthoritySample"); }
}
bool UY01R36Panel::NativeOnDrop(const FGeometry&, const FDragDropEvent& Event, UDragDropOperation* Operation)
{
    return CanUseZone(Z::Backpack) && Bag && Bag->GetCachedGeometry().IsUnderLocation(Event.GetScreenSpacePosition())
        && Accept(Cast<UY01R36DragOperation>(Operation), Z::Backpack, INDEX_NONE);
}
void UY01R36Panel::Refresh(bool bRefreshStatic)
{
    if (!Bag) { return; }
    if (DevourPillId.IsValid())
    {
        const auto* Pill = Model.Find(DevourPillId);
        if (!Pill || Pill->Location != EY01ItemLocation::Backpack || Pill->Definition.Type != EY01TableKind::Consumable
            || !CanDragFrom(Z::Backpack)) { DevourPillId = {}; }
    }
    TArray<FGuid> Visible;
    if (CanUseZone(Z::Backpack) && !Model.NeedsConfirmation())
    {
        const auto PanelRect = GetCachedGeometry().GetLayoutBoundingRect();
        const auto BagRect = Bag->GetCachedGeometry().GetLayoutBoundingRect();
        auto Contains = [](const FSlateRect& Outer, const FSlateRect& Inner)
        {
            return Inner.Right > Inner.Left && Inner.Bottom > Inner.Top
                && Outer.Left <= Inner.Left && Outer.Top <= Inner.Top && Outer.Right >= Inner.Right && Outer.Bottom >= Inner.Bottom;
        };
        for (auto* W : Bag->GetDisplayedEntryWidgets())
        {
            auto* Cell = Cast<UY01R36Cell>(W); if (!Cell || !Cell->IsVisible()) { continue; }
            const auto Rect = Cell->GetCachedGeometry().GetLayoutBoundingRect();
            if (Contains(PanelRect, Rect) && Contains(BagRect, Rect)) { Visible.Add(Cell->GetItemId()); }
        }
    }
    Model.VisualTick(SampleTime, Visible);
    for (const auto& Cell : Equipment) { Cell->Refresh(SampleTime); }
    for (const auto& Cell : Skills) { Cell->Refresh(SampleTime); }
    for (auto* W : Bag->GetDisplayedEntryWidgets()) { if (auto* Cell = Cast<UY01R36Cell>(W)) { Cell->Refresh(SampleTime); } }
    StatusText->SetText(FText::FromString(DevourPillId.IsValid()
        ? FString(TEXT("吞噬模式：点击装备栏中的红色装备吞噬；再次点击吞噬丹或右键取消")) : Model.Hint()));
    Confirmation->SetVisibility(Model.NeedsConfirmation() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    ConfirmButton->SetIsEnabled(Model.CanInteract(Phase)); CancelButton->SetIsEnabled(Model.CanInteract(Phase));
    const auto& V = Model.View(); const auto& S = V.Stats;
    // 金币文本与血条每帧跟随最新已提交视图刷新，不再挂在静态刷新分支：血量/护盾随战斗频繁变动。
    TopVitals->SetText(FText::FromString(FString::Printf(TEXT("金币 %d"), V.Gold)));
    const double Max = FMath::Max(1e-6, static_cast<double>(S.Health));
    HealthBar->SetPercent(FMath::Clamp(static_cast<float>(V.Health / Max), 0.f, 1.f));
    HealthText->SetText(FText::FromString(FY01AttributeResolver::WholeText(V.Health) + TEXT("/") + FY01AttributeResolver::WholeText(S.Health)));
    double ShieldTotal = 0;
    if (const auto* PC = Cast<AY01R36Controller>(GetOwningPlayer()))
    { if (const auto* P = PC->GetPlayerState<AY01PlayerState>()) { ShieldTotal = P->GetShield() ? P->GetShield()->GetTotal() : 0.; } }
    ShieldBar->SetPercent(ShieldTotal > 0 ? FMath::Clamp(static_cast<float>(ShieldTotal / Max), 0.f, 1.f) : 0.f);
    ShieldBar->SetVisibility(ShieldTotal > 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
    auto Toggle = [&](UButton* B, EY01R36PanelKind Kind, bool Expanded, const TCHAR* Name)
    {
        const bool Allowed = CanTogglePanel(Kind); B->SetIsEnabled(Allowed);
        B->SetToolTipText(FText::FromString(Allowed ? TEXT("独立展开或收起；不影响业务状态") : TEXT("相关拖动、请求或覆盖确认中，暂不能收起")));
        CastChecked<UTextBlock>(B->GetContent())->SetText(FText::FromString(Name));
        B->SetBackgroundColor(Expanded ? FLinearColor(.25f,.55f,.85f,1) : FLinearColor::White);
    };
    Toggle(CharacterButton, EY01R36PanelKind::Character, bCharacterExpanded, TEXT("天赋"));
    Toggle(BackpackButton, EY01R36PanelKind::Backpack, bBackpackExpanded, TEXT("背包"));
    Toggle(ShopButton, EY01R36PanelKind::Shop, bShopExpanded, TEXT("商店"));
    Toggle(DevouredButton, EY01R36PanelKind::Devoured, bDevouredExpanded, TEXT("已吞噬"));
    if (bRefreshStatic)
    {
        RefreshDevoured();
        // 新提交缺少明细时也要清除旧提示；视图未变时不重复格式化。
        for (const auto& Pair : StatValues)
        {
            Pair.Value->SetToolTipText(FText::GetEmpty());
            if (auto* Row = Cast<UBorder>(Pair.Value->GetParent()->GetParent())) { Row->SetToolTipText(FText::GetEmpty()); }
        }
        StatValues[TEXT("GoldGain")]->SetText(FText::FromString(TEXT("0%")));
        auto Stat = [&](const TCHAR* Key, double Value) { StatValues[Key]->SetText(FText::FromString(FString::Printf(TEXT("%g"), Value))); };
        Stat(TEXT("AttackDamage"), S.AttackDamage); Stat(TEXT("AttackInterval"), S.AttackInterval); Stat(TEXT("Range"), S.Range);
        Stat(TEXT("Armor"), S.Armor); Stat(TEXT("HealthRegen"), S.HealthRegen); Stat(TEXT("Strength"), S.Strength);
        Stat(TEXT("Agility"), S.Agility); Stat(TEXT("Wisdom"), S.Wisdom); Stat(TEXT("IncomeRate"), V.IncomeRate);
        // 明细仅说明来源结算，不能覆盖实际Stats（包括外部GE）；GoldGain是百分点观测值。
        for (const auto& B : V.AttributeBreakdowns)
        {
            const TCHAR* Key = nullptr;
            switch (B.Attribute)
            {
            case EY01Attribute::Strength: Key = TEXT("Strength"); break;
            case EY01Attribute::Agility: Key = TEXT("Agility"); break;
            case EY01Attribute::Wisdom: Key = TEXT("Wisdom"); break;
            case EY01Attribute::AttackDamage: Key = TEXT("AttackDamage"); break;
            case EY01Attribute::Armor: Key = TEXT("Armor"); break;
            case EY01Attribute::HealthRegen: Key = TEXT("HealthRegen"); break;
            case EY01Attribute::AttackRate: Key = TEXT("AttackInterval"); break;
            case EY01Attribute::GoldPerSecond: Key = TEXT("IncomeRate"); break;
            case EY01Attribute::GoldGain: Key = TEXT("GoldGain"); break;
            default: continue;
            }
            FString Detail = FY01AttributeResolver::DescribeBreakdown(B);
            if (B.Attribute == EY01Attribute::AttackRate) { Detail = TEXT("攻击间隔（秒） = 1 / 攻击次数/秒\n") + Detail; }
            const FText Tooltip = FText::FromString(Detail);
            auto* Value = StatValues[Key].Get(); Value->SetToolTipText(Tooltip);
            if (auto* Row = Cast<UBorder>(Value->GetParent()->GetParent())) { Row->SetToolTipText(Tooltip); }
            if (B.Attribute == EY01Attribute::GoldGain) { Value->SetText(FText::FromString(FString::Printf(TEXT("%g%%"), B.Final))); }
        }
        RefreshAttributes();
    }
    RefreshGameLog();
    BackpackHint->SetText(FText::FromString(FString::Printf(TEXT("%d 件 · 待显示到货 %d"), Items.Num(), Model.UnseenArrivals())));
    WaveCountdown->SetText(FText::FromString(FY01R36UIModel::WaveLabel(DisplayedWaves, WaveServerTime)));
    if (V.Growth.GrowthFault != EY01ItemError::None)
    { StatusText->SetText(FText::FromString(Model.Hint() + TEXT(" · 成长故障，请检查配置"))); }
    RefreshEconomy();
}
void UY01R36Panel::RefreshAttributes()
{
    const auto& V = Model.View();
    struct FRate { EY01GrowthAttribute Attribute; const TCHAR* Key; double PerSecond = 0; };
    FRate Rates[] = { {EY01GrowthAttribute::Strength, TEXT("Strength")}, {EY01GrowthAttribute::Agility, TEXT("Agility")},
        {EY01GrowthAttribute::Wisdom, TEXT("Wisdom")}, {EY01GrowthAttribute::AttackDamage, TEXT("AttackDamage")} };
    TMap<EY01GrowthAttribute, double> Kill, Attack;
    auto Collect = [&](const TArray<FY01ItemGrowth>& Rules)
    {
        for (const auto& R : Rules)
        {
            if (R.Target != EY01GrowthTarget::Player || !FMath::IsFinite(R.Delta) || R.Delta == 0) { continue; }
            if (R.Trigger == EY01GrowthTrigger::CombatTime)
            { for (auto& Rate : Rates) { if (Rate.Attribute == R.Attribute) { Rate.PerSecond += R.Delta; } } }
            else if (R.Trigger == EY01GrowthTrigger::EffectiveKill) { Kill.FindOrAdd(R.Attribute) += R.Delta; }
            else if (R.Trigger == EY01GrowthTrigger::BasicAttack) { Attack.FindOrAdd(R.Attribute) += R.Delta; }
        }
    };
    // 只统计当前生效来源：已装备、已学技能、已购直效商品、已吞噬装备与已选天赋；背包内物品不生效。
    for (const auto& Id : V.EquipmentSlots) { if (const auto* I = Model.Find(Id)) { Collect(I->Growth); } }
    for (const auto& S : V.Skills) { if (S.LearnedId.IsValid()) { Collect(S.Growth); } }
    for (const auto& O : V.ShopOffers) { if (O.bPurchased) { Collect(O.Growth); } }
    for (const auto& D : V.Devoured) { Collect(D.Growth); }
    for (const auto& T : V.TalentChoice.Candidates) { if (T.TalentId == V.TalentChoice.SelectedTalent) { Collect(T.Growth); } }
    for (const auto& Rate : Rates)
    {
        if (Rate.PerSecond == 0) { continue; }
        if (UTextBlock* Value = StatValues.FindRef(FName(Rate.Key)).Get())
        { Value->SetText(FText::FromString(Value->GetText().ToString() + FString::Printf(TEXT("（+%s/秒）"), *GrowthNumber(Rate.PerSecond)))); }
    }
    auto Line = [](const TCHAR* Title, const TMap<EY01GrowthAttribute, double>& Map)
    {
        if (Map.IsEmpty()) { return FString(); }
        FString Text = Title;
        bool First = true;
        for (const auto& Pair : Map)
        { Text += FString::Printf(TEXT("%s%s +%s"), First ? TEXT("") : TEXT("、"), *GrowthAttributeText(Pair.Key), *GrowthNumber(Pair.Value)); First = false; }
        return Text;
    };
    FString Summary = Line(TEXT("击杀成长："), Kill);
    const FString AttackLine = Line(TEXT("普攻成长："), Attack);
    if (!AttackLine.IsEmpty()) { Summary += (Summary.IsEmpty() ? TEXT("") : TEXT("\n")) + AttackLine; }
    GrowthSummary->SetText(FText::FromString(Summary));
    GrowthSummary->SetVisibility(Summary.IsEmpty() ? ESlateVisibility::Collapsed : ESlateVisibility::HitTestInvisible);
}
void UY01R36Panel::RefreshDevoured()
{
    if (!DevouredList) { return; }
    const auto& List = Model.View().Devoured;
    while (DevouredCells.Num() < List.Num())
    {
        auto* Cell = CreateWidget<UY01R36Cell>(GetWorld());
        Cell->Rename(*FString::Printf(TEXT("DevouredSlot_%d"), DevouredCells.Num()), WidgetTree);
        auto* Size = WidgetTree->ConstructWidget<USizeBox>(); Size->SetWidthOverride(64); Size->SetHeightOverride(64);
        Size->SetContent(Cell); DevouredList->AddChildToWrapBox(Size); DevouredCells.Add(Cell);
    }
    for (int32 I = 0; I < DevouredCells.Num(); ++I)
    {
        auto* Cell = DevouredCells[I].Get(); const bool Used = List.IsValidIndex(I);
        Cell->Configure(Used ? this : nullptr, Z::Devoured, I, Used ? List[I].ItemId : FGuid());
        Cell->GetParent()->SetVisibility(Used ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    }
    DevouredEmpty->SetVisibility(List.IsEmpty() ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}
void UY01R36Panel::RefreshGameLog()
{
    if (!GameLog || ShownLogRevision == Model.GetLogRevision()) { return; }
    ShownLogRevision = Model.GetLogRevision();
    GameLog->ClearChildren();
    for (const auto& Entry : Model.GameLog())
    {
        auto* Wrap = WidgetTree->ConstructWidget<UWrapBox>();
        Wrap->SetVisibility(ESlateVisibility::HitTestInvisible);
        for (const auto& Segment : Entry.Segments)
        {
            auto* T = MakeText(WidgetTree, NAME_None, Segment.Text, 12);
            T->SetAutoWrapText(false); T->SetShadowOffset(FVector2D(1, 1)); T->SetShadowColorAndOpacity(FLinearColor(0, 0, 0, .85f));
            T->SetColorAndOpacity(FSlateColor(Entry.bFailure ? LogGrey : Segment.Color));
            Wrap->AddChildToWrapBox(T);
        }
        if (auto* S = GameLog->AddChildToVerticalBox(Wrap)) { S->SetPadding(FMargin(0, 0, 0, 4)); }
    }
}
void UY01R36Panel::RefreshEconomy()
{
    if (!ShopList || !TalentList) { return; }
    const auto& V = Model.View(); FY01ItemRequest Request;
    auto Ensure = [&](TArray<TObjectPtr<UY01R36EconomyButton>>& Buttons, UScrollBox* List, int32 Count, const TCHAR* Prefix)
    {
        while (Buttons.Num() < Count)
        {
            auto* B = WidgetTree->ConstructWidget<UY01R36EconomyButton>(UY01R36EconomyButton::StaticClass(),
                FName(*FString::Printf(TEXT("%s_%d"), Prefix, Buttons.Num())));
            auto* Label = MakeText(WidgetTree, NAME_None, TEXT(""), 14);
            B->SetContent(Label);
            CastChecked<UButtonSlot>(Label->Slot)->SetHorizontalAlignment(HAlign_Fill);
            auto* Size = WidgetTree->ConstructWidget<USizeBox>(); Size->SetMinDesiredHeight(64); Size->SetContent(B);
            List->AddChild(Size); Buttons.Add(B);
        }
        for (int32 I = 0; I < Buttons.Num(); ++I)
        { Buttons[I]->GetParent()->SetVisibility(I < Count ? ESlateVisibility::Visible : ESlateVisibility::Collapsed); }
        List->SetVisibility(Count ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    };
    Ensure(OfferButtons, ShopList, V.ShopOffers.Num(), TEXT("Purchase"));
    int32 BoxCount[2] = {0, 0};
    for (int32 I = 0; I < V.ShopOffers.Num(); ++I)
    {
        const auto& O = V.ShopOffers[I]; auto* B = OfferButtons[I].Get(); B->Bind(this, EY01ItemOperation::Purchase, O.OfferId);
        auto* Size = CastChecked<USizeBox>(B->GetParent()); auto* Label = CastChecked<UTextBlock>(B->GetContent());
        const bool Book = O.ItemType == EY01TableKind::Skill;
        const bool Box = O.ShopKind == EY01ShopKind::RandomTier && (Book || O.ItemType == EY01TableKind::Equipment);
        double Total = 0; uint8 Base = static_cast<uint8>(EY01Quality::Red);
        for (const auto& Odd : O.Odds) { Total += Odd.Weight; Base = FMath::Min(Base, static_cast<uint8>(Odd.Quality)); }
        FString Odds;
        for (const auto& Odd : O.Odds)
        {
            Odds += FString::Printf(TEXT("%s%s %s%%"), Odds.IsEmpty() ? TEXT("") : TEXT("、"), *QualityText(Odd.Quality),
                *GrowthNumber(FMath::RoundToDouble(Odd.Weight / Total * 10000) / 100));
        }
        const TCHAR* Kind = O.ShopKind == EY01ShopKind::DirectEffect ? TEXT("直接生效商品，不占背包和槽")
            : O.ShopKind == EY01ShopKind::DirectItem ? TEXT("道具，直接入背包并叠加显示")
            : Box ? (Book ? TEXT("技能书幸运箱，随机获得技能书，仅入背包") : TEXT("装备幸运箱，随机获得装备，仅入背包，不自动装配"))
            : Book ? TEXT("技能书，仅入背包") : O.ItemType == EY01TableKind::Equipment ? TEXT("装备，仅入背包，不自动装配") : TEXT("随机获得物品，仅入背包");
        const TCHAR* State = O.bPurchased ? TEXT("已购买") : V.Gold < O.Price ? TEXT("金币不足") : TEXT("购买");
        FString Tooltip = FString::Printf(TEXT("%s\n类型：%s\n价格：%d 金币"), *O.DisplayName.ToString(), Kind, O.Price);
        if (!Odds.IsEmpty()) { Tooltip += TEXT("\n可能品质：") + Odds; }
        if (O.Quantity > 1) { Tooltip += FString::Printf(TEXT("\n数量：%d"), O.Quantity); }
        const FString Effects = EffectText(O.Effects, O.Growth);
        if (!Effects.IsEmpty()) { Tooltip += TEXT("\n") + Effects; }
        Tooltip += FString::Printf(TEXT("\n%s"), State);
        Size->ClearMinDesiredHeight(); Size->ClearHeightOverride();
        if (Box)
        {
            auto* Grid = Book ? SkillBoxes.Get() : EquipmentBoxes.Get(); int32& Index = BoxCount[Book];
            if (Size->GetParent() != Grid) { Size->RemoveFromParent(); Grid->AddChildToUniformGrid(Size, Index / 4, Index % 4); }
            else { auto* GridSlot = CastChecked<UUniformGridSlot>(Size->Slot); GridSlot->SetRow(Index / 4); GridSlot->SetColumn(Index % 4); }
            ++Index; Size->SetHeightOverride(58);
            Label->SetText(FText::FromString(FString::Printf(TEXT("%s·%s\n%d金"), *QualityText(static_cast<EY01Quality>(Base)), Book ? TEXT("技能") : TEXT("装备"), O.Price)));
            Label->SetColorAndOpacity(FSlateColor(QualityTextColor(static_cast<EY01Quality>(Base))));
            Label->SetJustification(ETextJustify::Center); auto Font = Label->GetFont(); Font.Size = 12; Label->SetFont(Font);
        }
        else
        {
            if (Size->GetParent() != ShopList) { Size->RemoveFromParent(); ShopList->AddChild(Size); }
            Size->SetMinDesiredHeight(64);
            Label->SetText(FText::FromString(FString::Printf(TEXT("%s\n%d 金币 · %s"), *O.DisplayName.ToString(), O.Price, State)));
            Label->SetColorAndOpacity(FSlateColor(FLinearColor::White));
            Label->SetJustification(ETextJustify::Left); auto Font = Label->GetFont(); Font.Size = 14; Label->SetFont(Font);
        }
        B->SetToolTipText(FText::FromString(Tooltip));
        B->SetIsEnabled(Model.Purchase(O.OfferId, Phase, Request));
    }
    EquipmentBoxes->SetVisibility(BoxCount[0] ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    SkillBoxes->SetVisibility(BoxCount[1] ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    Ensure(TalentButtons, TalentList, V.TalentChoice.Candidates.Num(), TEXT("Talent"));
    for (int32 I = 0; I < V.TalentChoice.Candidates.Num(); ++I)
    {
        const auto& T = V.TalentChoice.Candidates[I]; auto* B = TalentButtons[I].Get(); B->Bind(this, EY01ItemOperation::ChooseTalent, T.TalentId);
        const FString Text = T.DisplayName.ToString() + TEXT(" · ") + QualityText(T.Quality)
            + (V.TalentChoice.SelectedTalent == T.TalentId ? TEXT("\n已选择") : TEXT("\n三选一 · 本局一次"));
        CastChecked<UTextBlock>(B->GetContent())->SetText(FText::FromString(Text));
        B->SetToolTipText(FText::FromString(Text + TEXT("\n") + EffectText(T.Effects, T.Growth)));
        B->SetIsEnabled(Model.ChooseTalent(T.TalentId, Phase, Request));
    }
    const bool Recipes = !V.SynthesisRecipes.IsEmpty();
    SynthesisButton->SetVisibility(Recipes ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    CastChecked<UTextBlock>(SynthesisButton->GetContent())->SetText(FText::FromString(TEXT("合成")));
    SynthesisButton->SetToolTipText(FText::FromString(TEXT("自动选背包内最低可合品质的五件同类物品，合成一件高一品质；每次一组，不消耗已装备物品")));
    // 无可合材料时仍可点击，用于在游戏内LOG提示“没有可以合成的”。
    SynthesisButton->SetIsEnabled(Model.CanInteract(Phase) && !Model.NeedsConfirmation());
}

bool FY01R36UIModel::Sync(const FY01CommittedView& Next)
{
    if (!Next.ViewRevision || !Next.MatchId.IsValid() || Next.JoinSequence <= 0
        || Next.EquipmentSlots.Num() != Y01Data::EquipmentSlotCount || Next.Skills.Num() != Y01Data::SkillSlotCount) { return false; }
    const bool NewSession = Next.MatchId != Committed.MatchId || Next.JoinSequence != Committed.JoinSequence || Next.ConnectionId != Committed.ConnectionId;
    if (!NewSession && Next.ViewRevision <= Committed.ViewRevision) { return false; }
    if (NewSession)
    {
        bPending = false; Pending = {}; SentAtResponse = 0; Arrivals.Reset();
        Log.Reset(); LoggedDeliveries.Reset(); ++LogRevision;
        // 新会话已有的到货不补记日志，只记录此后新到的掉落。
        for (const auto& Item : Next.Items) { if (Item.ArrivalDeliveryId.IsValid()) { LoggedDeliveries.Add(Item.ArrivalDeliveryId); } }
    }
    if (NewSession || (!bPending && (Next.ResponseSerial != Committed.ResponseSerial
        || Next.Response.TicketId != Committed.Response.TicketId))) { DisplayResponse = Next.Response; }
    if (bPending && Next.ResponseSerial > SentAtResponse)
    {
        const auto Matches = [&](const FY01ViewReceipt& Receipt)
        { return FY01ItemRequest::StaticStruct()->CompareScriptStruct(&Receipt.Request, &Pending, 0); };
        const auto* Receipt = Matches(Next.Response) ? &Next.Response : Next.Receipts.FindByPredicate(Matches);
        if (Receipt) { DisplayResponse = *Receipt; bPending = false; LogReceipt(*Receipt, Next); }
    }
    for (const auto& Item : Next.Items)
    {
        if (!Item.ArrivalDeliveryId.IsValid() || LoggedDeliveries.Contains(Item.ArrivalDeliveryId)) { continue; }
        LoggedDeliveries.Add(Item.ArrivalDeliveryId);
        FY01R36LogEntry Entry; Entry.Segments.Add({TEXT("掉落："), FLinearColor::White});
        Entry.Segments.Add({ItemName(Item), QualityTextColor(Item.Display.Quality)}); AddLog(MoveTemp(Entry));
    }
    Committed = Next; ItemIndices.Reset(); ItemIndices.Reserve(Committed.Items.Num());
    for (int32 I = 0; I < Committed.Items.Num(); ++I)
    {
        const auto& Item = Committed.Items[I]; ItemIndices.Add(Item.ItemId, I);
        if (Item.ItemId.IsValid() && Item.ArrivalDeliveryId.IsValid())
        {
            auto& A = Arrivals.FindOrAdd(Item.ItemId);
            if (A.DeliveryId != Item.ArrivalDeliveryId) { A = {}; A.DeliveryId = Item.ArrivalDeliveryId; }
        }
    }
    for (auto It = Arrivals.CreateIterator(); It; ++It)
    {
        const auto* Item = Find(It.Key());
        if (!Item || Item->ArrivalDeliveryId != It.Value().DeliveryId) { It.RemoveCurrent(); }
    }
    return true;
}
TArray<FY01R36Flight> FY01R36UIModel::Flights(double ServerTime, bool Enabled) const
{
    TArray<FY01R36Flight> Result;
    if (!Enabled || !FMath::IsFinite(ServerTime)) { return Result; }
    TSet<FGuid> Seen;
    for (const auto& R : Committed.Delivery.Records)
    {
        if (R.State != EY01DeliveryState::Pending || R.MatchId != Committed.MatchId || R.JoinSequence != Committed.JoinSequence
            || !R.DeliveryId.IsValid() || !R.ItemId.IsValid() || Seen.Contains(R.DeliveryId)
            || !FMath::IsFinite(R.StartTime) || !FMath::IsFinite(R.ETA) || R.Origin.ContainsNaN()
            || R.ETA <= R.StartTime || ServerTime < R.StartTime || ServerTime >= R.ETA) { continue; }
        Seen.Add(R.DeliveryId); Result.Add({R, (ServerTime - R.StartTime) / (R.ETA - R.StartTime)});
    }
    return Result;
}
void FY01R36UIModel::VisualTick(double ServerTime, const TArray<FGuid>& VisibleItems)
{
    if (!FMath::IsFinite(ServerTime) || ServerTime < 0) { return; }
    for (const auto& Id : VisibleItems)
    {
        auto* A = Arrivals.Find(Id); const auto* Item = Find(Id);
        if (A && !A->bSeen && Item && Item->Location == EY01ItemLocation::Backpack)
        { A->bSeen = true; A->FirstVisible = ServerTime; }
    }
}
double FY01R36UIModel::FlashRemaining(FGuid Item, double ServerTime) const
{
    const auto* A = Arrivals.Find(Item);
    return A && A->bSeen && FMath::IsFinite(ServerTime)
        ? FMath::Clamp(.6 - (ServerTime - A->FirstVisible), 0., .6) : 0.;
}
int32 FY01R36UIModel::UnseenArrivals() const
{
    int32 Count = 0;
    for (const auto& Pair : Arrivals)
    {
        const auto* Item = Find(Pair.Key);
        if (!Pair.Value.bSeen && Item && Item->Location == EY01ItemLocation::Backpack) { ++Count; }
    }
    return Count;
}
bool FY01R36UIModel::BlocksClosing(EY01R36PanelKind Panel) const
{
    if (Panel == EY01R36PanelKind::Backpack && NeedsConfirmation()) { return true; }
    if (!bPending) { return false; }
    if (Pending.Operation == EY01ItemOperation::Purchase) { return Panel == EY01R36PanelKind::Shop; }
    if (Pending.Operation == EY01ItemOperation::ChooseTalent) { return Panel == EY01R36PanelKind::Character; }
    if (Pending.Operation == EY01ItemOperation::RemoveSkill) { return false; }
    return Panel == EY01R36PanelKind::Backpack;
}
FString FY01R36UIModel::WaveLabel(const FY01WaveSnapshot& S, double ServerTime)
{
    if (S.Phase == EY01MatchPhase::Ended)
    { return S.Result == EY01MatchResult::Success ? TEXT("胜利") : S.Result == EY01MatchResult::Failure ? TEXT("失败") : TEXT("本局已结束"); }
    if (S.bAutomaticWaves && S.WaveIndex >= 0)
    {
        // 以当前波待生成批次为准，不要求场上怪物清空。
        const auto* P = S.WaveStates.FindByPredicate([&](const FY01WaveProgress& W) { return W.WaveKey == S.WaveIndex; });
        const bool Spawning = P ? P->PendingSpawns > 0 : S.PendingSpawns > 0;
        if (Spawning) { return TEXT("这波还在进行中"); }
    }
    if (S.bAutomaticWaves && S.NextWaveIndex >= 0)
    {
        if (!FMath::IsFinite(ServerTime) || ServerTime < 0 || !FMath::IsFinite(S.NextWaveDeadline) || S.NextWaveDeadline < 0)
        { return TEXT("等待波次时间同步"); }
        return FString::Printf(TEXT("第 %d 波还有 %.0f 秒"), S.NextWaveIndex + 1, FMath::CeilToDouble(FMath::Max(0., S.NextWaveDeadline - ServerTime)));
    }
    if (S.bAutomaticWaves && S.TotalWaves > 0 && S.WaveIndex >= S.TotalWaves - 1)
    {
        FString Text = FString::Printf(TEXT("全部波次已出，剩余 %d 只"), S.TotalAliveMonsters);
        if (S.TotalPendingSpawns > 0) { Text += FString::Printf(TEXT(" · 待生成 %d"), S.TotalPendingSpawns); }
        return Text;
    }
    return S.Phase == EY01MatchPhase::Waiting ? TEXT("等待波次同步") : FString::Printf(TEXT("第 %d 波 · 待生成 %d 只"), S.WaveIndex + 1, S.PendingSpawns);
}
bool FY01R36UIModel::CanInteract(EY01MatchPhase Phase) const
{
    return !bPending && Committed.ViewRevision && Committed.Health > 0
        && (Phase == EY01MatchPhase::Preparing || Phase == EY01MatchPhase::Battle);
}
const FY01ViewItem* FY01R36UIModel::Find(FGuid Id) const
{ const auto* Index = ItemIndices.Find(Id); return Index ? &Committed.Items[*Index] : nullptr; }
bool FY01R36UIModel::Drag(EY01R36Zone Zone, FGuid Item, int32 Slot, FY01R36Drag& Out) const
{
    const auto* I = Find(Item); if (bPending || !I || Zone == EY01R36Zone::Skill) { return false; }
    if (Zone == EY01R36Zone::Backpack && I->Location != EY01ItemLocation::Backpack) { return false; }
    if (Zone == EY01R36Zone::Equipment && (I->Location != EY01ItemLocation::EquipmentSlot || I->Slot != Slot)) { return false; }
    Out = {}; Out.Zone = Zone; Out.ItemId = Item; Out.EntityRevision = I->EntityRevision; Out.Slot = I->Slot;
    Out.MatchId = Committed.MatchId; Out.JoinSequence = Committed.JoinSequence; Out.ConnectionId = Committed.ConnectionId; return true;
}
FY01ItemRequest FY01R36UIModel::Base(EY01ItemOperation Operation, int32 Slot) const
{
    FY01ItemRequest R; R.MatchId = Committed.MatchId; R.JoinSequence = Committed.JoinSequence;
    R.Seq = Committed.ExpectedSeq; R.ConfigRevision = Committed.ConfigRevision; R.Operation = Operation; R.Slot = Slot;
    if ((Operation == EY01ItemOperation::Equip || Operation == EY01ItemOperation::Unequip || Operation == EY01ItemOperation::Devour)
        && Committed.EquipmentSlots.IsValidIndex(Slot))
    {
        R.ExpectedSlotItemId = Committed.EquipmentSlots[Slot];
        if (const auto* I = Find(R.ExpectedSlotItemId)) { R.ExpectedSlotEntityRevision = I->EntityRevision; }
    }
    else if (Committed.Skills.IsValidIndex(Slot))
    { R.ExpectedLearnedId = Committed.Skills[Slot].LearnedId; R.ExpectedLearnedRevision = Committed.Skills[Slot].EntityRevision; }
    return R;
}
bool FY01R36UIModel::Drop(const FY01R36Drag& Payload, EY01R36Zone Target, int32 Slot, EY01MatchPhase Phase, FY01ItemRequest& Out) const
{
    if (!CanInteract(Phase) || NeedsConfirmation() || Payload.MatchId != Committed.MatchId
        || Payload.JoinSequence != Committed.JoinSequence || Payload.ConnectionId != Committed.ConnectionId) { return false; }
    const auto* I = Find(Payload.ItemId);
    // 槽内成长会在拖动期间推进版本；仍在原槽的同一实例不作废卸下。
    const bool bSameRevision = Payload.Zone == EY01R36Zone::Equipment && I && I->Location == EY01ItemLocation::EquipmentSlot
        ? I->EntityRevision >= Payload.EntityRevision : I && I->EntityRevision == Payload.EntityRevision;
    if (!I || !bSameRevision || I->Slot != Payload.Slot) { return false; }
    EY01ItemOperation Operation;
    if (Payload.Zone == EY01R36Zone::Equipment && I->Location == EY01ItemLocation::EquipmentSlot && Target == EY01R36Zone::Backpack)
    { Operation = EY01ItemOperation::Unequip; Slot = I->Slot; }
    else if (Payload.Zone == EY01R36Zone::Backpack && I->Location == EY01ItemLocation::Backpack)
    {
        if (Target == EY01R36Zone::Equipment && I->Definition.Type == EY01TableKind::Equipment && Committed.EquipmentSlots.IsValidIndex(Slot))
        { Operation = EY01ItemOperation::Equip; }
        else if (Target == EY01R36Zone::Skill && I->Definition.Type == EY01TableKind::Skill && Committed.Skills.IsValidIndex(Slot))
        { Operation = EY01ItemOperation::PrepareLearn; }
        else { return false; }
    }
    else { return false; }
    Out = Base(Operation, Slot); Out.ItemId = I->ItemId; Out.ExpectedEntityRevision = I->EntityRevision; return true;
}
bool FY01R36UIModel::Remove(int32 Slot, EY01MatchPhase Phase, FY01ItemRequest& Out) const
{
    if (!CanInteract(Phase) || NeedsConfirmation() || !Committed.Skills.IsValidIndex(Slot) || !Committed.Skills[Slot].LearnedId.IsValid()) { return false; }
    Out = Base(EY01ItemOperation::RemoveSkill, Slot); return true;
}
bool FY01R36UIModel::NeedsConfirmation() const
{
    return DisplayResponse.TicketId.IsValid() && Committed.Tickets.ContainsByPredicate([&](const auto& T)
        { return T.TicketId == DisplayResponse.TicketId; });
}
bool FY01R36UIModel::Decide(bool Confirm, EY01MatchPhase Phase, FY01ItemRequest& Out) const
{
    if (!CanInteract(Phase) || !NeedsConfirmation()) { return false; }
    const auto* T = Committed.Tickets.FindByPredicate([&](const auto& Ticket) { return Ticket.TicketId == DisplayResponse.TicketId; });
    Out = *T; Out.Seq = Committed.ExpectedSeq; Out.Operation = Confirm ? EY01ItemOperation::ConfirmLearn : EY01ItemOperation::CancelLearn; return true;
}
bool FY01R36UIModel::Purchase(FName Offer, EY01MatchPhase Phase, FY01ItemRequest& Out) const
{
    if (!CanInteract(Phase) || NeedsConfirmation()) { return false; }
    const auto* Entry = Committed.ShopOffers.FindByPredicate([&](const auto& O) { return O.OfferId == Offer; });
    if (!Entry || Entry->bPurchased || Entry->Price < 0 || Committed.Gold < Entry->Price) { return false; }
    Out = Base(EY01ItemOperation::Purchase, INDEX_NONE); Out.Offer.Type = EY01TableKind::ShopOffer; Out.Offer.RowName = Offer;
    return true;
}
bool FY01R36UIModel::ChooseTalent(FName Talent, EY01MatchPhase Phase, FY01ItemRequest& Out) const
{
    const auto& Choice = Committed.TalentChoice;
    if (!CanInteract(Phase) || NeedsConfirmation() || !Choice.ChoiceId.IsValid() || !Choice.SelectedTalent.IsNone()
        || !Choice.Candidates.ContainsByPredicate([&](const auto& C) { return C.TalentId == Talent; })) { return false; }
    Out = Base(EY01ItemOperation::ChooseTalent, INDEX_NONE); Out.Talent.Type = EY01TableKind::Talent;
    Out.Talent.RowName = Talent; Out.TalentChoiceId = Choice.ChoiceId; return true;
}
bool FY01R36UIModel::PickMaterials(TArray<FY01ItemConsume>& Out, FName& Recipe) const
{
    Out.Reset(); Recipe = NAME_None;
    TArray<const FY01ViewItem*> Candidates;
    for (const auto& Item : Committed.Items)
    {
        if (Item.Location == EY01ItemLocation::Backpack
            && (Item.Definition.Type == EY01TableKind::Equipment || Item.Definition.Type == EY01TableKind::Skill)) { Candidates.Add(&Item); }
    }
    Candidates.StableSort([](const FY01ViewItem& A, const FY01ViewItem& B) { return A.AcquisitionOrder < B.AcquisitionOrder; });
    for (int32 Q = static_cast<int32>(EY01Quality::Green); Q <= static_cast<int32>(EY01Quality::Red); ++Q)
    {
        for (const EY01TableKind Type : {EY01TableKind::Equipment, EY01TableKind::Skill})
        {
            const auto Quality = static_cast<EY01Quality>(Q);
            const auto* Match = Committed.SynthesisRecipes.FindByPredicate([&](const auto& R)
            { return R.Recipe.InputType == Type && R.Recipe.InputQuality == Quality && R.Recipe.InputCount == Y01Data::SynthesisInputCount; });
            if (!Match) { continue; }
            for (const auto* Item : Candidates)
            {
                if (Item->Definition.Type != Type || Item->Display.Quality != Quality) { continue; }
                FY01ItemConsume Material; Material.ItemId = Item->ItemId; Material.ExpectedEntityRevision = Item->EntityRevision;
                Out.Add(Material);
                if (Out.Num() == Y01Data::SynthesisInputCount) { Recipe = Match->RecipeId; return true; }
            }
            Out.Reset();
        }
    }
    return false;
}
bool FY01R36UIModel::Synthesize(EY01MatchPhase Phase, FY01ItemRequest& Out) const
{
    if (!CanInteract(Phase) || NeedsConfirmation()) { return false; }
    TArray<FY01ItemConsume> Materials; FName Recipe;
    if (!PickMaterials(Materials, Recipe)) { return false; }
    Out = Base(EY01ItemOperation::Synthesize, INDEX_NONE); Out.Materials = MoveTemp(Materials);
    Out.Recipe.Type = EY01TableKind::Synthesis; Out.Recipe.RowName = Recipe; return true;
}
bool FY01R36UIModel::Devour(FGuid PillId, int32 Slot, EY01MatchPhase Phase, FY01ItemRequest& Out, FString* Reason) const
{
    auto Reject = [&](const TCHAR* Text) { if (Reason) { *Reason = Text; } return false; };
    if (!CanInteract(Phase) || NeedsConfirmation()) { return Reject(TEXT("当前不能吞噬；未消耗吞噬丹")); }
    const auto* Pill = Find(PillId);
    if (!Pill || Pill->Location != EY01ItemLocation::Backpack || Pill->Definition.Type != EY01TableKind::Consumable || Pill->Count < 1)
    { return Reject(TEXT("吞噬丹已失效；请重新选择")); }
    const auto* Target = Committed.EquipmentSlots.IsValidIndex(Slot) ? Find(Committed.EquipmentSlots[Slot]) : nullptr;
    if (!Target) { return Reject(TEXT("请选择装备栏中的装备；未消耗吞噬丹")); }
    if (Target->Definition.Type != EY01TableKind::Equipment || Target->Display.Quality != EY01Quality::Red)
    { return Reject(TEXT("只能吞噬红色装备；未消耗吞噬丹")); }
    if (Committed.Devoured.ContainsByPredicate([&](const auto& D) { return D.Definition.RowName == Target->Definition.RowName; }))
    { return Reject(TEXT("同一装备本局只能吞噬一次；未消耗吞噬丹")); }
    Out = Base(EY01ItemOperation::Devour, Slot); Out.ItemId = Pill->ItemId; Out.ExpectedEntityRevision = Pill->EntityRevision;
    return true;
}
const FY01ViewItem* FY01R36UIModel::FindDevoured(FGuid Id) const
{ return Id.IsValid() ? Committed.Devoured.FindByPredicate([&](const auto& D) { return D.ItemId == Id; }) : nullptr; }
void FY01R36UIModel::Sent(const FY01ItemRequest& Request)
{ Pending = Request; SentAtResponse = Committed.ResponseSerial; bPending = true; }
TArray<FGuid> FY01R36UIModel::Backpack() const
{
    TArray<FGuid> Result; Result.Reserve(Committed.Items.Num());
    for (const auto& Item : Committed.Items) { if (Item.Location == EY01ItemLocation::Backpack) { Result.Add(Item.ItemId); } }
    return Result;
}
bool FY01R36UIModel::IsPreviewing() const
{
    return bPending && (Pending.Operation == EY01ItemOperation::Equip || Pending.Operation == EY01ItemOperation::Unequip)
        && Committed.EquipmentSlots.IsValidIndex(Pending.Slot) && Find(Pending.ItemId);
}
FGuid FY01R36UIModel::DisplaySlot(int32 Slot) const
{
    if (!Committed.EquipmentSlots.IsValidIndex(Slot)) { return {}; }
    if (IsPreviewing() && Pending.Slot == Slot) { return Pending.Operation == EY01ItemOperation::Equip ? Pending.ItemId : FGuid(); }
    return Committed.EquipmentSlots[Slot];
}
TArray<FGuid> FY01R36UIModel::DisplayBackpack() const
{
    auto Result = Backpack(); if (!IsPreviewing()) { return Result; }
    const FGuid Occupant = Committed.EquipmentSlots[Pending.Slot];
    if (Pending.Operation == EY01ItemOperation::Equip)
    { Result.Remove(Pending.ItemId); if (Occupant.IsValid()) { Result.AddUnique(Occupant); } }
    else if (Occupant == Pending.ItemId) { Result.AddUnique(Occupant); }
    return Result;
}
namespace
{
FString DisplayLabel(const FY01ViewDisplay& Display, const TCHAR* Type)
{
    const TCHAR* Qualities[] = { TEXT("绿"), TEXT("蓝"), TEXT("紫"), TEXT("金"), TEXT("红") };
    const int32 Q = static_cast<int32>(Display.Quality);
    return FString::Printf(TEXT("%s\n%s · %s"), Display.Name.IsEmpty() ? TEXT("未命名内容") : *Display.Name.ToString(),
        Type, Q >= 0 && Q < UE_ARRAY_COUNT(Qualities) ? Qualities[Q] : TEXT("未知品质"));
}
}
FString FY01R36UIModel::ItemLabel(FGuid Id) const
{
    if (const auto* I = Find(Id))
    {
        return DisplayLabel(I->Display, I->Definition.Type == EY01TableKind::Skill ? TEXT("技能书")
            : I->Definition.Type == EY01TableKind::Consumable ? TEXT("道具") : TEXT("装备"));
    }
    const auto* D = FindDevoured(Id); return D ? DisplayLabel(D->Display, TEXT("已吞噬装备")) : TEXT("空槽");
}
double FY01R36UIModel::Remaining(const FY01ViewSkill& Skill, double ServerTime)
{ return FMath::IsFinite(ServerTime) && ServerTime >= 0 && FMath::IsFinite(Skill.Deadline) && Skill.Deadline >= 0
    ? FMath::Max(0., Skill.Deadline - ServerTime) : -1.; }
FString FY01R36UIModel::SkillLabel(int32 Slot, double ServerTime) const
{
    if (!Committed.Skills.IsValidIndex(Slot) || !Committed.Skills[Slot].LearnedId.IsValid()) { return TEXT("空技能槽"); }
    const auto& S = Committed.Skills[Slot]; const bool Active = S.Display.SkillType == EY01SkillType::Active;
    const double CD = Remaining(S, ServerTime);
    return DisplayLabel(S.Display, Active ? TEXT("主动") : TEXT("被动")) + TEXT("\n")
        + (Active ? (CD < 0 ? FString(TEXT("CD 待同步")) : CD > 0 ? FString::Printf(TEXT("CD %.1f 秒"), CD) : FString(TEXT("就绪"))) : FString(TEXT("常驻生效")));
}
FString FY01R36UIModel::Status() const
{
    if (!Committed.ViewRevision) { return TEXT("等待完整服务器视图"); }
    if (bPending) { return IsPreviewing() ? TEXT("已移动，等待服务器确认；拒绝会自动还原") : TEXT("等待服务器确认；尚未移动物品"); }
    if (NeedsConfirmation()) { return TEXT("覆盖已学技能？确认耗书，取消不改变原技能"); }
    if (!Committed.ResponseSerial) { return TEXT("拖动物品到槽；技能右键移除不返书"); }
    return ErrorText(DisplayResponse.Code);
}
FString FY01R36UIModel::ErrorText(EY01ItemError Code)
{
    switch (Code)
    {
    case EY01ItemError::None: return TEXT("服务器已确认");
    case EY01ItemError::Busy: return TEXT("服务器忙，请稍后重试");
    case EY01ItemError::PurchaseLimit: return TEXT("已购买该限购商品；未重复扣金");
    case EY01ItemError::InsufficientFunds: return TEXT("金币不足；未扣金或消耗材料");
    case EY01ItemError::DuplicateSkill: return TEXT("已学习相同技能；未消耗技能书");
    case EY01ItemError::Dead: return TEXT("角色死亡，不能操作");
    case EY01ItemError::Phase: return TEXT("当前阶段不能操作");
    case EY01ItemError::StaleEntity: case EY01ItemError::StaleSlot: case EY01ItemError::StaleTicket: case EY01ItemError::StaleConfig:
        return TEXT("内容已变化；请按最新视图重新操作");
    default: return TEXT("服务器拒绝请求；物品状态以完整视图为准");
    }
}
FString FY01R36UIModel::Hint() const
{
    if (!Committed.ViewRevision) { return TEXT("等待完整服务器视图"); }
    if (bPending) { return IsPreviewing() ? TEXT("已移动，等待服务器确认；拒绝会自动还原") : TEXT("等待服务器确认；尚未移动物品"); }
    if (NeedsConfirmation()) { return TEXT("覆盖已学技能？确认耗书，取消不改变原技能"); }
    if (Committed.Health <= 0) { return TEXT("角色死亡，不能操作"); }
    return TEXT("拖动物品到槽；技能右键移除不返书");
}
FString FY01R36LogEntry::ToString() const
{
    FString Text; for (const auto& S : Segments) { Text += S.Text; } return Text;
}
void FY01R36UIModel::AddLog(FY01R36LogEntry Entry)
{
    Log.Add(MoveTemp(Entry));
    if (Log.Num() > MaxLogEntries) { Log.RemoveAt(0, Log.Num() - MaxLogEntries); }
    ++LogRevision;
}
void FY01R36UIModel::LogNotice(const FString& Text)
{
    FY01R36LogEntry Entry; Entry.bFailure = true; Entry.Segments.Add({Text, LogGrey}); AddLog(MoveTemp(Entry));
}
void FY01R36UIModel::LogReceipt(const FY01ViewReceipt& Receipt, const FY01CommittedView& Next)
{
    const auto Op = Receipt.Request.Operation;
    if (Receipt.Code != EY01ItemError::None)
    {
        // 失败仅记录原因，不显示任何获得或消耗。
        if (Op == EY01ItemOperation::Devour)
        {
            LogNotice(Receipt.Code == EY01ItemError::DuplicateItem ? FString(TEXT("同一装备本局只能吞噬一次；未消耗吞噬丹"))
                : ErrorText(Receipt.Code) + TEXT("；未消耗吞噬丹"));
            return;
        }
        LogNotice(ErrorText(Receipt.Code)); return;
    }
    auto ItemSegment = [](const FY01ViewItem& Item) { return FY01R36LogSegment{ItemName(Item), QualityTextColor(Item.Display.Quality)}; };
    const auto* Product = Next.Items.FindByPredicate([&](const auto& I) { return I.ItemId == Receipt.ProductItemId; });
    FY01R36LogEntry Entry;
    if (Op == EY01ItemOperation::Purchase)
    {
        Entry.Segments.Add({TEXT("购买："), FLinearColor::White});
        if (Product) { Entry.Segments.Add(ItemSegment(*Product)); }
        else if (const auto* Offer = Next.ShopOffers.FindByPredicate([&](const auto& O) { return O.OfferId == Receipt.Request.Offer.RowName; }))
        { Entry.Segments.Add({Offer->DisplayName.ToString(), FLinearColor::White}); }
        else { return; }
    }
    else if (Op == EY01ItemOperation::Synthesize && Product)
    {
        Entry.Segments.Add({TEXT("消耗："), FLinearColor::White});
        for (int32 I = 0; I < Receipt.Request.Materials.Num(); ++I)
        {
            if (I) { Entry.Segments.Add({TEXT("、"), FLinearColor::White}); }
            const auto* Material = Find(Receipt.Request.Materials[I].ItemId);
            Entry.Segments.Add(Material ? ItemSegment(*Material) : FY01R36LogSegment{TEXT("未知物品"), LogGrey});
        }
        Entry.Segments.Add({TEXT("；合成："), FLinearColor::White});
        Entry.Segments.Add(ItemSegment(*Product));
    }
    else if (Op == EY01ItemOperation::Devour)
    {
        const auto* Target = Next.Devoured.FindByPredicate([&](const auto& D) { return D.ItemId == Receipt.Request.ExpectedSlotItemId; });
        if (!Target) { Target = Find(Receipt.Request.ExpectedSlotItemId); }
        if (!Target) { return; }
        Entry.Segments.Add({TEXT("吞噬："), FLinearColor::White}); Entry.Segments.Add(ItemSegment(*Target));
    }
    else { return; }
    AddLog(MoveTemp(Entry));
}
