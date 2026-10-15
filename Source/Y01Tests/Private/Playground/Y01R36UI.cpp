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
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/ScaleBox.h"
#include "Components/ScrollBox.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Playground/Y01R36Runtime.h"
#include "Player/Y01PlayerState.h"
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
FString EffectText(const TArray<FY01ItemEffect>& Effects, const TArray<FY01ItemGrowth>& Growth)
{
    FString Text;
    for (const auto& Effect : Effects)
    {
        FY01SourceEffectRow Row; Row.Kind = Effect.Kind; Row.Magnitude = Effect.Magnitude;
        Row.SourceAttribute = Effect.SourceAttribute; Row.TargetAttribute = Effect.TargetAttribute; Row.Amount = Effect.Amount;
        Text += FY01AttributeResolver::Describe(Row) + TEXT("\n");
    }
    for (const auto& Rule : Growth)
    {
        if (Rule.Target == EY01GrowthTarget::EquipmentInstance)
        { Text += FString::Printf(TEXT("有效击杀：装备攻击 +%g · 实例累计 %g\n"), Rule.Delta, Rule.Accumulated); }
        else { Text += FString::Printf(TEXT("有效击杀：玩家力量 +%g · 局内累计保留\n"), Rule.Delta); }
    }
    return Text;
}
FLinearColor QualityColor(EY01Quality Quality)
{
    const FLinearColor Colors[] = { {.12f,.65f,.28f}, {.15f,.4f,.9f}, {.58f,.22f,.85f}, {.9f,.68f,.12f}, {.85f,.14f,.2f} };
    const int32 Index = static_cast<int32>(Quality); return Index >= 0 && Index < UE_ARRAY_COUNT(Colors) ? Colors[Index] : FLinearColor::Gray;
}
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
    EntryWidgetClass = UY01R36Cell::StaticClass(); EntryWidth = 148; EntryHeight = 106;
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
        Frame = WidgetTree->ConstructWidget<UBorder>(); Frame->SetPadding(FMargin(6)); WidgetTree->RootWidget = Frame;
        auto* Row = WidgetTree->ConstructWidget<UHorizontalBox>(); Frame->SetContent(Row);
        auto* IconBox = WidgetTree->ConstructWidget<USizeBox>(); IconBox->SetWidthOverride(24); IconBox->SetHeightOverride(24);
        Icon = WidgetTree->ConstructWidget<UImage>(); IconBox->SetContent(Icon); Row->AddChild(IconBox);
        Label = MakeText(WidgetTree, TEXT("Label"), TEXT("空槽"), 15);
        Row->AddChildToHorizontalBox(Label)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
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
    const bool Compact = Zone == Z::Equipment || Zone == Z::Skill;
    Frame->SetPadding(FMargin(Compact ? 4 : 6));
    auto* IconBox = CastChecked<USizeBox>(Icon->GetParent());
    IconBox->SetWidthOverride(Compact ? 16 : 24); IconBox->SetHeightOverride(Compact ? 16 : 24);
    CastChecked<UHorizontalBoxSlot>(IconBox->Slot)->SetVerticalAlignment(Compact ? VAlign_Center : VAlign_Fill);
    CastChecked<UHorizontalBoxSlot>(Label->Slot)->SetVerticalAlignment(Compact ? VAlign_Center : VAlign_Fill);
    Label->SetAutoWrapText(!Compact);
    Label->SetTextOverflowPolicy(Compact ? ETextOverflowPolicy::Ellipsis : ETextOverflowPolicy::Clip);
    if (!Panel.IsValid()) { Label->SetText(FText::GetEmpty()); SetToolTipText(FText::GetEmpty()); return; }
    const auto& Model = Panel->GetModel(); const auto& V = Model.View(); const FY01ViewDisplay* Display = nullptr;
    FString Text;
    if (Zone == Z::Equipment)
    {
        ItemId = V.EquipmentSlots.IsValidIndex(SlotIndex) ? V.EquipmentSlots[SlotIndex] : FGuid();
        Text = FString::Printf(TEXT("装备槽 %d\n"), SlotIndex + 1) + Model.ItemLabel(ItemId);
    }
    else if (Zone == Z::Skill)
    {
        ItemId = {};
        Text = FString::Printf(TEXT("技能槽 %d\n"), SlotIndex + 1) + Model.SkillLabel(SlotIndex, ServerTime);
        if (V.Skills.IsValidIndex(SlotIndex) && V.Skills[SlotIndex].LearnedId.IsValid()) { Display = &V.Skills[SlotIndex].Display; }
    }
    else { Text = Model.ItemLabel(ItemId); }
    if (const auto* Item = Model.Find(ItemId)) { Display = &Item->Display; }
    const bool Flash = Zone == Z::Backpack && Model.FlashRemaining(ItemId, ServerTime) > 0;
    if (Flash) { Text += TEXT("\n到货"); }
    const FString FullText = Text;
    if (Compact)
    {
        if (Display)
        {
            FString Name = Display->Name.IsEmpty() ? TEXT("未命名内容") : Display->Name.ToString();
            Name.ReplaceInline(TEXT("\r"), TEXT(" ")); Name.ReplaceInline(TEXT("\n"), TEXT(" "));
            // 只压缩槽内表现；技能末行复用原文，CD仍由同一服务器样本计算。
            int32 LastLine = INDEX_NONE; FullText.FindLastChar(TEXT('\n'), LastLine);
            Text = Name + TEXT("\n") + (Zone == Z::Skill ? FullText.Mid(LastLine + 1)
                : FString::Printf(TEXT("装备%d · %s"), SlotIndex + 1, *QualityText(Display->Quality)));
        }
        else { Text = FString::Printf(TEXT("%s %d\n空槽"), Zone == Z::Equipment ? TEXT("装备") : TEXT("技能"), SlotIndex + 1); }
    }
    auto Font = Label->GetFont(); const int32 FontSize = Zone == Z::Skill ? 14 : Compact ? 13 : 15;
    if (Font.Size != FontSize) { Font.Size = FontSize; Label->SetFont(Font); }
    Label->SetText(FText::FromString(Text));
    const bool Selected = Model.Materials().ContainsByPredicate([&](const auto& M) { return M.ItemId == ItemId; });
    Frame->SetBrushColor(Flash ? FLinearColor(.72f,.62f,.2f,1) : Selected ? FLinearColor(.5f,.4f,.08f,1) : Display ? QualityColor(Display->Quality) * FLinearColor(.3f,.3f,.3f,1) : FLinearColor(.05f,.07f,.1f,1));
    FString Tooltip;
    if (const auto* Item = Model.Find(ItemId))
    { Tooltip = FullText + TEXT("\n") + EffectText(Item->Effects, Item->Growth) + Y01Damage::Describe(Item->DamageEffects); }
    else if (Zone == Z::Skill && V.Skills.IsValidIndex(SlotIndex) && V.Skills[SlotIndex].LearnedId.IsValid())
    {
        const auto& Skill = V.Skills[SlotIndex];
        Tooltip = FullText + TEXT("\n") + EffectText(Skill.Effects, Skill.Growth) + Y01Damage::Describe(Skill.DamageEffects);
    }
    // Slate按提示对象身份重新打开；同内容不重建，空槽重绑必须清除旧实例。
    if (GetToolTipText().ToString() != Tooltip) { SetToolTipText(FText::FromString(Tooltip)); }
    auto* Texture = Display && !Display->Icon.IsNull() ? Cast<UTexture2D>(Display->Icon.TryLoad()) : nullptr;
    if (Texture) { Icon->SetBrushFromTexture(Texture); Icon->SetColorAndOpacity(FLinearColor::White); }
    else { Icon->SetBrush(FSlateBrush()); Icon->SetColorAndOpacity(Display ? QualityColor(Display->Quality) : FLinearColor::Gray); }
}
FReply UY01R36Cell::NativeOnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event)
{
    if (!Panel.IsValid() || !VisibleInTree(this) || !Panel->CanUseZone(Zone)) { return FReply::Unhandled(); }
    if (Zone == Z::Skill && Event.GetEffectingButton() == EKeys::LeftMouseButton)
    { return Panel->RequestSkill(SlotIndex) ? FReply::Handled() : FReply::Unhandled(); }
    if (Zone == Z::Backpack && Panel->IsSelectingMaterials() && Event.GetEffectingButton() == EKeys::LeftMouseButton)
    { Panel->ToggleMaterial(ItemId); return FReply::Handled(); }
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
    Visual->SetDesiredSizeInViewport(FVector2D(148,106)); Visual->SetVisibility(ESlateVisibility::HitTestInvisible);
    Drag->DefaultDragVisual = Visual; Operation = Drag;
}
bool UY01R36Cell::NativeOnDrop(const FGeometry&, const FDragDropEvent&, UDragDropOperation* Operation)
{ return VisibleInTree(this) && Panel.IsValid() && Panel->CanUseZone(Zone) && Panel->Accept(Cast<UY01R36DragOperation>(Operation), Zone, SlotIndex); }
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
        CharacterPanel = Panel(TEXT("CharacterPanel"), TEXT("人物属性"), {16,96}, {340,350});
        BackpackPanel = Panel(TEXT("BackpackPanel"), TEXT("背包 · 拖回卸下 / 滚轮查看"), {368,96}, {472,350});
        ShopPanel = Panel(TEXT("ShopPanel"), TEXT("商店"), {852,96}, {264,350});
        FlightLayer = WidgetTree->ConstructWidget<UY01R36FlightLayer>(UY01R36FlightLayer::StaticClass(), TEXT("DeliveryFlightLayer"));
        FlightLayer->SetVisibility(ESlateVisibility::HitTestInvisible);
        auto* FlightSlot = Root->AddChildToCanvas(FlightLayer); FlightSlot->SetAnchors(FAnchors(0,0,1,1));
        FlightSlot->SetOffsets(FMargin(0)); FlightSlot->SetZOrder(0);
        TopVitals = MakeText(WidgetTree, TEXT("TopVitals"), TEXT("等待服务器"), 24);
        TopVitals->SetColorAndOpacity(FSlateColor(FLinearColor(1.f,.88f,.32f,1.f)));
        Position(Canvas, TopVitals, {16,12}, {410,80});
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
        auto* Stats = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("AttributeList"));
        Position(CharacterPanel, Stats, {8,44}, {324,192});
        const TCHAR* Keys[] = {TEXT("Health"), TEXT("MaxHealth"), TEXT("AttackDamage"), TEXT("AttackInterval"), TEXT("AttackType"),
            TEXT("Range"), TEXT("MeleeAngle"), TEXT("ProjectileSpeed"), TEXT("Armor"), TEXT("MoveSpeed"), TEXT("HealthRegen"),
            TEXT("Strength"), TEXT("Agility"), TEXT("Wisdom"), TEXT("Experience"), TEXT("IncomeRate")};
        const TCHAR* Labels[] = {TEXT("当前生命"), TEXT("生命上限"), TEXT("攻击"), TEXT("攻击间隔"), TEXT("攻击类型"),
            TEXT("攻击范围"), TEXT("近战角度"), TEXT("弹道速度"), TEXT("护甲"), TEXT("移动速度"), TEXT("生命恢复"),
            TEXT("力量"), TEXT("敏捷"), TEXT("智慧"), TEXT("经验"), TEXT("金币/秒")};
        for (int32 I = 0; I < UE_ARRAY_COUNT(Keys); ++I)
        {
            auto* RowSize = WidgetTree->ConstructWidget<USizeBox>(); RowSize->SetHeightOverride(38);
            auto* Row = WidgetTree->ConstructWidget<UBorder>(UBorder::StaticClass(), FName(*FString::Printf(TEXT("StatRow_%s"), Keys[I])));
            Row->SetBrushColor(I % 2 ? FLinearColor(.09f,.15f,.23f,1) : FLinearColor(.055f,.1f,.17f,1)); Row->SetPadding(FMargin(8,5));
            RowSize->SetContent(Row); Stats->AddChild(RowSize);
            auto* Pair = WidgetTree->ConstructWidget<UHorizontalBox>(); Row->SetContent(Pair);
            auto* Label = MakeText(WidgetTree, FName(*FString::Printf(TEXT("StatLabel_%s"), Keys[I])), Labels[I], 16);
            Label->SetColorAndOpacity(FSlateColor(FLinearColor(.88f,.94f,1.f,1)));
            Pair->AddChildToHorizontalBox(Label)->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
            auto* Value = MakeText(WidgetTree, FName(*FString::Printf(TEXT("StatValue_%s"), Keys[I])), TEXT("—"), 16);
            Value->SetColorAndOpacity(FSlateColor(FLinearColor(1.f,.88f,.4f,1))); Value->SetJustification(ETextJustify::Right);
            Pair->AddChildToHorizontalBox(Value)->SetSize(FSlateChildSize(ESlateSizeRule::Fill)); StatValues.Add(Keys[I], Value);
        }
        Bag = WidgetTree->ConstructWidget<UY01R36TileView>(UY01R36TileView::StaticClass(), TEXT("Backpack"));
        Position(BackpackPanel, Bag, {8,44}, {456,232});
        BackpackHint = MakeText(WidgetTree, TEXT("BackpackHint"), TEXT(""), 13); Position(BackpackPanel, BackpackHint, {8,278}, {456,24});
        for (int32 I = 0; I < 6; ++I)
        {
            auto* Cell = CreateWidget<UY01R36Cell>(GetWorld()); Cell->Configure(this, Z::Equipment, I); Equipment.Add(Cell);
            Cell->Rename(*FString::Printf(TEXT("EquipmentSlot_%d"), I), WidgetTree);
            Position(Canvas, Cell, {16. + I * 108, 650}, {104,52});
        }
        for (int32 I = 0; I < 4; ++I)
        {
            auto* Cell = CreateWidget<UY01R36Cell>(GetWorld()); Cell->Configure(this, Z::Skill, I); Skills.Add(Cell);
            Cell->Rename(*FString::Printf(TEXT("SkillSlot_%d"), I), WidgetTree);
            Position(Canvas, Cell, {668. + I * 148, 650}, {144,52});
        }
        StatusText = MakeText(WidgetTree, TEXT("Status"), Model.Status(), 14); Position(Canvas, StatusText, {16,450}, {1100,32});
        auto Button = [&](FName Name, const TCHAR* Text, FVector2D At, FVector2D Size, UCanvasPanel* Parent)
        {
            auto* B = WidgetTree->ConstructWidget<UButton>(UButton::StaticClass(), Name);
            auto* Label = MakeText(WidgetTree, NAME_None, Text, Size.X <= 134 ? 14 : 16);
            Label->SetAutoWrapText(false); Label->SetJustification(ETextJustify::Center);
            B->SetContent(Label);
            CastChecked<UButtonSlot>(Label->Slot)->SetHorizontalAlignment(HAlign_Fill);
            Position(Parent, B, At, Size); return B;
        };
        CharacterButton = Button(TEXT("ToggleCharacter"), TEXT("人物属性"), {1130,96}, {134,44}, Canvas);
        BackpackButton = Button(TEXT("ToggleBackpack"), TEXT("背包"), {1130,148}, {134,44}, Canvas);
        ShopButton = Button(TEXT("ToggleShop"), TEXT("商店"), {1130,200}, {134,44}, Canvas);
        CharacterButton->OnClicked.AddDynamic(this, &ThisClass::ToggleCharacter);
        BackpackButton->OnClicked.AddDynamic(this, &ThisClass::ToggleBackpack);
        ShopButton->OnClicked.AddDynamic(this, &ThisClass::ToggleShop);
        FlightButton = Button(TEXT("ToggleFlights"), TEXT("品质飞行开"), {1130,346}, {134,44}, Canvas);
        FlightButton->OnClicked.AddDynamic(this, &ThisClass::ToggleFlights);
        auto* Room = Button(TEXT("Training"), TEXT("练功房"), {1130,398}, {134,48}, Canvas); Room->OnClicked.AddDynamic(this, &ThisClass::Training);
        Room->SetToolTipText(FText::FromString(TEXT("进入 / 返回练功房")));
        ShopList = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("ShopList"));
        Position(ShopPanel, ShopList, {8,44}, {248,298});
        MaterialButton = Button(TEXT("MaterialMode"), TEXT("选择合成材料"), {8,306}, {224,36}, BackpackPanel);
        MaterialButton->OnClicked.AddDynamic(this, &ThisClass::MaterialMode);
        SynthesisButton = Button(TEXT("Synthesize"), TEXT("免费合成 0/5"), {240,306}, {224,36}, BackpackPanel);
        SynthesisButton->OnClicked.AddDynamic(this, &ThisClass::Synthesize);
        Position(CharacterPanel, MakeText(WidgetTree, TEXT("TalentTitle"), TEXT("天赋"), 16), {8,240}, {324,24});
        TalentList = WidgetTree->ConstructWidget<UScrollBox>(UScrollBox::StaticClass(), TEXT("TalentList"));
        Position(CharacterPanel, TalentList, {8,266}, {324,76});
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
    if (Changed)
    {
        TMap<FGuid, TObjectPtr<UY01R36TileItem>> Next; TArray<UObject*> List;
        for (const auto& Id : Model.Backpack())
        {
            auto* Existing = Items.Find(Id); auto* Item = Existing ? Existing->Get() : NewObject<UY01R36TileItem>(this);
            Item->ItemId = Id; Item->Panel = this; Next.Add(Id, Item); List.Add(Item);
        }
        const bool ChangedList = List != Bag->GetListItems(); Items = MoveTemp(Next);
        if (ChangedList) { Bag->SetListItems(List); }
    }
    Refresh(Changed);
    if (Changed) { Y01ViewTiming::Mark(TEXT("UI_PRESENT"), Y01ViewTiming::Scope(Model.View()), TimingStart, 0, Model.View().Response.Request.Seq, Items.Num()); }
    return Changed;
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
{ return VisibleInTree(this) && !(bBackpackExpanded && bSelectingMaterials) && Model.CanInteract(Phase) && !Model.NeedsConfirmation(); }
bool UY01R36Panel::CanTogglePanel(EY01R36PanelKind Panel) const
{
    const bool Expanded = Panel == EY01R36PanelKind::Character ? bCharacterExpanded : Panel == EY01R36PanelKind::Backpack ? bBackpackExpanded : bShopExpanded;
    if (!Expanded) { return true; }
    const auto* Drag = Cast<UY01R36DragOperation>(UWidgetBlueprintLibrary::GetDragDroppingContent());
    const bool RelatedDrag = Panel == EY01R36PanelKind::Backpack && Drag && Drag->Origin.Get() == this
        && (Drag->Identity.Zone == Z::Backpack || Drag->Identity.Zone == Z::Equipment);
    return !Model.BlocksClosing(Panel) && !RelatedDrag;
}
void UY01R36Panel::TogglePanel(EY01R36PanelKind Panel)
{
    if (!Bag) { TakeWidget(); }
    if (!CanTogglePanel(Panel)) { Refresh(); return; }
    bool& Expanded = Panel == EY01R36PanelKind::Character ? bCharacterExpanded : Panel == EY01R36PanelKind::Backpack ? bBackpackExpanded : bShopExpanded;
    Expanded = !Expanded;
    auto* Container = Panel == EY01R36PanelKind::Character ? CharacterPanel.Get() : Panel == EY01R36PanelKind::Backpack ? BackpackPanel.Get() : ShopPanel.Get();
    Container->SetVisibility(Expanded ? ESlateVisibility::SelfHitTestInvisible : ESlateVisibility::Collapsed); Refresh();
}
void UY01R36Panel::ToggleCharacter() { TogglePanel(EY01R36PanelKind::Character); }
void UY01R36Panel::ToggleBackpack() { TogglePanel(EY01R36PanelKind::Backpack); }
void UY01R36Panel::ToggleShop() { TogglePanel(EY01R36PanelKind::Shop); }
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
bool UY01R36Panel::ToggleMaterial(FGuid Item)
{ const bool Changed = CanUseZone(Z::Backpack) && bSelectingMaterials && Model.ToggleMaterial(Item, Phase); Refresh(); return Changed; }
void UY01R36Panel::MaterialMode()
{
    if (CanUseZone(Z::Backpack) && Model.CanInteract(Phase) && !Model.NeedsConfirmation() && !Model.View().SynthesisRecipes.IsEmpty())
    { bSelectingMaterials = !bSelectingMaterials; Refresh(); }
}
void UY01R36Panel::Synthesize()
{ FY01ItemRequest R; if (CanUseZone(Z::Backpack) && Model.Synthesize(Phase, R)) { Send(R); } }
bool UY01R36Panel::Send(const FY01ItemRequest& Request)
{
    if (!VisibleInTree(this)) { return false; }
    auto* PC = Cast<AY01R36Controller>(GetOwningPlayer()); if (!PC || !PC->IsLocalController()) { return false; }
    const auto* State = PC->GetPlayerState<AY01PlayerState>(); FY01CommittedView OwnerView;
    if (!IsValid(State) || State->GetOwner() != PC || !State->GetLoadout()->ReadCommittedView(OwnerView)
        || Request.MatchId != OwnerView.MatchId || Request.JoinSequence != OwnerView.JoinSequence
        || Model.View().ConnectionId != OwnerView.ConnectionId) { return false; }
    Y01ViewTiming::Mark(TEXT("REQUEST_SENT"), Y01ViewTiming::Scope(OwnerView), 0, 0, Request.Seq, OwnerView.Items.Num());
    Model.Sent(Request); PC->ServerItemRequest(Request); Refresh(); return true;
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
void UY01R36Panel::ToggleFlights()
{
    bFlightsEnabled = !bFlightsEnabled;
    if (!bFlightsEnabled && FlightLayer) { FlightLayer->Present({}); }
    if (FlightButton) { CastChecked<UTextBlock>(FlightButton->GetContent())->SetText(FText::FromString(
        bFlightsEnabled ? TEXT("品质飞行开") : TEXT("品质飞行关"))); }
}
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
        const bool Show = bFlightsEnabled && Model.View().Health > 0 && Phase != EY01MatchPhase::Ended;
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
    if (auto* VitalsSlot = Cast<UCanvasPanelSlot>(TopVitals->Slot)) { VitalsSlot->SetSize(FVector2D(410, Show ? 56 : 80)); }
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
    StatusText->SetText(FText::FromString(Model.Status())); Confirmation->SetVisibility(Model.NeedsConfirmation() ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    ConfirmButton->SetIsEnabled(Model.CanInteract(Phase)); CancelButton->SetIsEnabled(Model.CanInteract(Phase));
    const auto& V = Model.View(); const auto& S = V.Stats;
    if (bRefreshStatic) { TopVitals->SetText(FText::FromString(FString::Printf(TEXT("金币 %d\n生命 %g/%g"), V.Gold, V.Health, S.Health))); }
    auto Toggle = [&](UButton* B, EY01R36PanelKind Kind, bool Expanded, const TCHAR* Name)
    {
        const bool Allowed = CanTogglePanel(Kind); B->SetIsEnabled(Allowed);
        B->SetToolTipText(FText::FromString(Allowed ? TEXT("独立展开或收起；不影响业务状态") : TEXT("相关拖动、请求或覆盖确认中，暂不能收起")));
        CastChecked<UTextBlock>(B->GetContent())->SetText(FText::FromString(Name));
        B->SetBackgroundColor(Expanded ? FLinearColor(.25f,.55f,.85f,1) : FLinearColor::White);
    };
    Toggle(CharacterButton, EY01R36PanelKind::Character, bCharacterExpanded, TEXT("人物属性"));
    Toggle(BackpackButton, EY01R36PanelKind::Backpack, bBackpackExpanded, TEXT("背包"));
    Toggle(ShopButton, EY01R36PanelKind::Shop, bShopExpanded, TEXT("商店"));
    if (bRefreshStatic)
    {
        // 属性文本只由已发布视图决定；视图未变时不重复格式化。
        auto Stat = [&](const TCHAR* Key, double Value) { StatValues[Key]->SetText(FText::FromString(FString::Printf(TEXT("%g"), Value))); };
        Stat(TEXT("Health"), V.Health); Stat(TEXT("MaxHealth"), S.Health); Stat(TEXT("AttackDamage"), S.AttackDamage);
        Stat(TEXT("AttackInterval"), S.AttackInterval); Stat(TEXT("Range"), S.Range); Stat(TEXT("MeleeAngle"), S.MeleeAngle);
        Stat(TEXT("ProjectileSpeed"), S.ProjectileSpeed); Stat(TEXT("Armor"), S.Armor); Stat(TEXT("MoveSpeed"), S.MoveSpeed);
        Stat(TEXT("HealthRegen"), S.HealthRegen); Stat(TEXT("Strength"), S.Strength); Stat(TEXT("Agility"), S.Agility);
        Stat(TEXT("Wisdom"), S.Wisdom); Stat(TEXT("Experience"), V.Experience); Stat(TEXT("IncomeRate"), V.IncomeRate);
        StatValues[TEXT("AttackType")]->SetText(FText::FromString(S.AttackType == EY01AttackType::Ranged ? TEXT("远程") : TEXT("近战")));
    }
    BackpackHint->SetText(FText::FromString(FString::Printf(TEXT("%d 件 · 待显示到货 %d"), Items.Num(), Model.UnseenArrivals())));
    WaveCountdown->SetText(FText::FromString(FY01R36UIModel::WaveLabel(DisplayedWaves, WaveServerTime)));
    if (V.Growth.GrowthFault != EY01ItemError::None)
    { StatusText->SetText(FText::FromString(Model.Status() + TEXT(" · 成长故障，请检查配置"))); }
    RefreshEconomy();
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
    for (int32 I = 0; I < V.ShopOffers.Num(); ++I)
    {
        const auto& O = V.ShopOffers[I]; auto* B = OfferButtons[I].Get(); B->Bind(this, EY01ItemOperation::Purchase, O.OfferId);
        const FString Text = FString::Printf(TEXT("%s\n%d 金币 · %s"), *O.DisplayName.ToString(), O.Price, O.bPurchased ? TEXT("已购买") : V.Gold < O.Price ? TEXT("金币不足") : TEXT("购买"));
        CastChecked<UTextBlock>(B->GetContent())->SetText(FText::FromString(Text));
        B->SetToolTipText(FText::FromString(Text + TEXT("\n") + (O.ShopKind == EY01ShopKind::DirectEffect
            ? TEXT("直接生效，不占背包和槽\n") : TEXT("随机获得物品，仅入背包，不自动装配\n")) + EffectText(O.Effects, O.Growth)));
        B->SetIsEnabled(Model.Purchase(O.OfferId, Phase, Request));
    }
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
    MaterialButton->SetVisibility(Recipes ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    SynthesisButton->SetVisibility(Recipes ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
    MaterialButton->SetIsEnabled(Model.CanInteract(Phase) && !Model.NeedsConfirmation());
    CastChecked<UTextBlock>(MaterialButton->GetContent())->SetText(FText::FromString(bSelectingMaterials ? TEXT("选材中 · 恢复拖动") : TEXT("选择合成材料")));
    CastChecked<UTextBlock>(SynthesisButton->GetContent())->SetText(FText::FromString(FString::Printf(TEXT("免费合成 %d/5"), Model.Materials().Num())));
    SynthesisButton->SetToolTipText(FText::FromString(TEXT("选取五件包内同类同品质物品，免费合成高一品质；不返还材料")));
    SynthesisButton->SetIsEnabled(Model.Synthesize(Phase, Request));
}

bool FY01R36UIModel::Sync(const FY01CommittedView& Next)
{
    if (!Next.ViewRevision || !Next.MatchId.IsValid() || Next.JoinSequence <= 0
        || Next.EquipmentSlots.Num() != Y01Data::EquipmentSlotCount || Next.Skills.Num() != Y01Data::SkillSlotCount) { return false; }
    const bool NewSession = Next.MatchId != Committed.MatchId || Next.JoinSequence != Committed.JoinSequence || Next.ConnectionId != Committed.ConnectionId;
    if (!NewSession && Next.ViewRevision <= Committed.ViewRevision) { return false; }
    if (NewSession) { bPending = false; Pending = {}; SentAtResponse = 0; SelectedMaterials.Reset(); Arrivals.Reset(); }
    if (NewSession || (!bPending && (Next.ResponseSerial != Committed.ResponseSerial
        || Next.Response.TicketId != Committed.Response.TicketId))) { DisplayResponse = Next.Response; }
    if (bPending && Next.ResponseSerial > SentAtResponse)
    {
        const auto Matches = [&](const FY01ViewReceipt& Receipt)
        { return FY01ItemRequest::StaticStruct()->CompareScriptStruct(&Receipt.Request, &Pending, 0); };
        const auto* Receipt = Matches(Next.Response) ? &Next.Response : Next.Receipts.FindByPredicate(Matches);
        if (Receipt) { DisplayResponse = *Receipt; bPending = false; }
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
    SelectedMaterials.RemoveAll([&](const FY01ItemConsume& Material)
    {
        const auto* Item = Find(Material.ItemId);
        return !Item || Item->EntityRevision != Material.ExpectedEntityRevision || Item->Location != EY01ItemLocation::Backpack;
    });
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
    if ((Operation == EY01ItemOperation::Equip || Operation == EY01ItemOperation::Unequip) && Committed.EquipmentSlots.IsValidIndex(Slot))
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
    if (!I || I->EntityRevision != Payload.EntityRevision || I->Slot != Payload.Slot) { return false; }
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
bool FY01R36UIModel::ToggleMaterial(FGuid Id, EY01MatchPhase Phase)
{
    if (!CanInteract(Phase) || NeedsConfirmation()) { return false; }
    const int32 Selected = SelectedMaterials.IndexOfByPredicate([&](const auto& M) { return M.ItemId == Id; });
    if (Selected != INDEX_NONE) { SelectedMaterials.RemoveAt(Selected); return true; }
    const auto* Item = Find(Id);
    if (!Item || Item->Location != EY01ItemLocation::Backpack || SelectedMaterials.Num() >= Y01Data::SynthesisInputCount
        || (Item->Definition.Type != EY01TableKind::Equipment && Item->Definition.Type != EY01TableKind::Skill)) { return false; }
    if (!SelectedMaterials.IsEmpty())
    {
        const auto* First = Find(SelectedMaterials[0].ItemId);
        if (!First || Item->Definition.Type != First->Definition.Type || Item->Display.Quality != First->Display.Quality) { return false; }
    }
    FY01ItemConsume Material; Material.ItemId = Id; Material.ExpectedEntityRevision = Item->EntityRevision;
    SelectedMaterials.Add(Material); return true;
}
bool FY01R36UIModel::Synthesize(EY01MatchPhase Phase, FY01ItemRequest& Out) const
{
    if (!CanInteract(Phase) || NeedsConfirmation() || SelectedMaterials.Num() != Y01Data::SynthesisInputCount) { return false; }
    const auto* First = Find(SelectedMaterials[0].ItemId); if (!First) { return false; }
    for (const auto& Material : SelectedMaterials)
    {
        const auto* Item = Find(Material.ItemId);
        if (!Item || Item->EntityRevision != Material.ExpectedEntityRevision || Item->Location != EY01ItemLocation::Backpack
            || Item->Definition.Type != First->Definition.Type || Item->Display.Quality != First->Display.Quality) { return false; }
    }
    const auto* Recipe = Committed.SynthesisRecipes.FindByPredicate([&](const auto& R)
    { return R.Recipe.InputType == First->Definition.Type && R.Recipe.InputQuality == First->Display.Quality
        && R.Recipe.InputCount == Y01Data::SynthesisInputCount; });
    if (!Recipe) { return false; }
    Out = Base(EY01ItemOperation::Synthesize, INDEX_NONE); Out.Materials = SelectedMaterials;
    Out.Recipe.Type = EY01TableKind::Synthesis; Out.Recipe.RowName = Recipe->RecipeId; return true;
}
void FY01R36UIModel::Sent(const FY01ItemRequest& Request)
{ Pending = Request; SentAtResponse = Committed.ResponseSerial; bPending = true; }
TArray<FGuid> FY01R36UIModel::Backpack() const
{
    TArray<FGuid> Result; Result.Reserve(Committed.Items.Num());
    for (const auto& Item : Committed.Items) { if (Item.Location == EY01ItemLocation::Backpack) { Result.Add(Item.ItemId); } }
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
    const auto* I = Find(Id); return I ? DisplayLabel(I->Display, I->Definition.Type == EY01TableKind::Skill ? TEXT("技能书") : TEXT("装备")) : TEXT("空槽");
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
    if (bPending) { return TEXT("等待服务器确认；尚未移动物品"); }
    if (NeedsConfirmation()) { return TEXT("覆盖已学技能？确认耗书，取消不改变原技能"); }
    if (!Committed.ResponseSerial) { return TEXT("拖动物品到槽；技能右键移除不返书"); }
    switch (DisplayResponse.Code)
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
