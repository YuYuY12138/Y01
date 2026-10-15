#include "Misc/AutomationTest.h"
#include "Playground/Y01R36UI.h"
#include "Playground/Y01R36Runtime.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/SizeBox.h"
#include "Components/HorizontalBox.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/ButtonSlot.h"
#include "Playground/Y01R36Verification.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Layout/ArrangedChildren.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"
#include "Types/PaintArgs.h"
#include "Input/HittestGrid.h"
#include "Rendering/DrawElements.h"
#include "Widgets/Views/STableViewBase.h"
#include "GameFramework/HUD.h"
#include "UObject/StrongObjectPtr.h"
#include "UnrealClient.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/SceneComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "UObject/UnrealType.h"
#include "Misc/ScopeExit.h"
#include "Framework/Application/SlateApplication.h"
#include "Layout/WidgetPath.h"
#include "Input/DragAndDrop.h"
#include "Slate/UMGDragDropOp.h"
#include "Slate/SObjectWidget.h"
#include "Blueprint/WidgetBlueprintLibrary.h"
#include "Widgets/Input/SButton.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/SlateRenderer.h"
#if WITH_EDITOR
#include "GenericPlatform/GenericWindow.h"
#include "Widgets/SVirtualWindow.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Components/ScrollBox.h"
#include "ImageUtils.h"
#include "TextureResource.h"
#include "RenderingThread.h"
#include "DynamicRHI.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
using Z = EY01R36Zone;
struct FWidgetWorld
{
    UWorld* World;
    TStrongObjectPtr<UY01R36Panel> Panel;
    TSharedPtr<SWidget> Slate;
    FY01CommittedView View;
    FWidgetWorld(bool bExpand = true)
    {
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        // 无本地玩家的对象层夹具直接以World为Outer，避免未初始化GameInstance丢失World。
        Panel.Reset(CreateWidget<UY01R36Panel>(World)); Slate = Panel->TakeWidget();
        if (bExpand)
        { Panel->ToggleCharacter(); Panel->ToggleBackpack(); Panel->ToggleShop(); }
        View.MatchId = FGuid::NewGuid(); View.ConnectionId = FGuid::NewGuid(); View.JoinSequence = 1;
        View.ViewRevision = 1; View.ConfigRevision = 2; View.Health = 100; View.EquipmentSlots.SetNum(6); View.Skills.SetNum(4);
        for (int32 I = 0; I < 4; ++I) { View.Skills[I].Slot = I; }
        for (int32 I = 0; I < 3; ++I)
        {
            auto& Item = View.Items.AddDefaulted_GetRef(); Item.ItemId = FGuid::NewGuid(); Item.EntityRevision = 1;
            Item.AcquisitionOrder = I + 1; Item.Definition.Type = EY01TableKind::Equipment; Item.Definition.RowName = TEXT("HiddenRow");
            Item.Display.Name = FText::FromString(TEXT("受控测试装备")); Item.Display.Quality = static_cast<EY01Quality>(I);
        }
    }
    ~FWidgetWorld() { Slate.Reset(); Panel.Reset(); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    void Present() { Panel->Present(View, EY01MatchPhase::Preparing, 11); }
};
FString Texts(UUserWidget* Widget)
{
    FString Result;
    if (Widget && Widget->WidgetTree) { Widget->WidgetTree->ForEachWidget([&](UWidget* W)
        { if (auto* Text = Cast<UTextBlock>(W)) { Result += Text->GetText().ToString(); } }); }
    return Result;
}
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36WidgetTest, "Y01.Framework.R36.UI.Widget",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36WidgetTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("IndependentHUD"), TEXT("SixFour"), TEXT("TileView"), TEXT("TenThousand"), TEXT("StableObjects"),
        TEXT("DeleteProjection"), TEXT("EntryRebind"), TEXT("MouseDrag"), TEXT("EquipmentDrag"), TEXT("NoLearnedDrag"),
        TEXT("ForeignPanel"), TEXT("Confirmation"), TEXT("SlotCooldown"), TEXT("Placeholder"), TEXT("NoDebugControls"), TEXT("WaveObservation") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36WidgetTest::RunTest(const FString& Case)
{
    if (Case == TEXT("IndependentHUD"))
    { TestEqual(TEXT("新模式不展示旧调试Canvas"), GetDefault<AY01R36GameMode>()->HUDClass.Get(), AHUD::StaticClass()); return true; }
    FWidgetWorld F; F.Present(); auto* Panel = F.Panel.Get(); auto* Bag = Panel->GetBackpack();
    if (Case == TEXT("SixFour"))
    {
        for (int32 I = 0; I < 6; ++I) { TestNotNull(TEXT("六个装备真实控件"), Panel->GetCell(Z::Equipment, I)); }
        for (int32 I = 0; I < 4; ++I) { TestNotNull(TEXT("四个技能真实控件"), Panel->GetCell(Z::Skill, I)); }
        TestNull(TEXT("没有额外槽"), Panel->GetCell(Z::Skill, 4)); return true;
    }
    if (Case == TEXT("TileView") || Case == TEXT("TenThousand") || Case == TEXT("StableObjects") || Case == TEXT("DeleteProjection"))
    {
        if (!TestNotNull(TEXT("真实UTileView"), Bag)) { return false; }
        TestEqual(TEXT("每件一项"), Bag->GetNumItems(), 3);
        if (Case == TEXT("TenThousand"))
        {
            for (int32 I = 3; I < 10000; ++I) { auto Item = F.View.Items[0]; Item.ItemId = FGuid::NewGuid(); Item.AcquisitionOrder = I + 1; F.View.Items.Add(Item); }
            ++F.View.ViewRevision; F.Present(); TestEqual(TEXT("一万件未截断"), Bag->GetNumItems(), 10000);
            auto* Last = Cast<UY01R36TileItem>(Bag->GetItemAt(9999)); TestTrue(TEXT("末项真实列表数据"), Last && Last->ItemId == F.View.Items.Last().ItemId);
            TestTrue(TEXT("不是一次生成一万控件"), Bag->GetDisplayedEntryWidgets().Num() < 1000);
        }
        if (Case == TEXT("StableObjects"))
        {
            auto* First = Bag->GetItemAt(0); ++F.View.ViewRevision; F.View.Items[0].Display.Name = FText::FromString(TEXT("更新名称")); F.Present();
            TestEqual(TEXT("同实例沿用列表对象"), Bag->GetItemAt(0), First);
        }
        if (Case == TEXT("DeleteProjection"))
        {
            F.View.Items.RemoveAt(1); ++F.View.ViewRevision; F.Present(); TestEqual(TEXT("完整提交删除一项"), Bag->GetNumItems(), 2);
        }
        return true;
    }
    if (Case == TEXT("Confirmation"))
    {
        auto* Box = Cast<UBorder>(Panel->WidgetTree->FindWidget(TEXT("Confirmation")));
        if (!TestNotNull(TEXT("真实确认框"), Box)) { return false; }
        TestEqual(TEXT("无票据不弹出"), Box->GetVisibility(), ESlateVisibility::Collapsed);
        FY01ItemRequest T; T.TicketId = FGuid::NewGuid(); F.View.Tickets.Add(T); F.View.Response.TicketId = T.TicketId;
        ++F.View.ViewRevision; F.Present(); TestEqual(TEXT("权威票据弹出"), Box->GetVisibility(), ESlateVisibility::Visible);
        TestNotNull(TEXT("确认按钮"), Cast<UButton>(Panel->WidgetTree->FindWidget(TEXT("Confirm"))));
        TestNotNull(TEXT("取消按钮"), Cast<UButton>(Panel->WidgetTree->FindWidget(TEXT("Cancel")))); return true;
    }
    if (Case == TEXT("WaveObservation"))
    {
        auto* GS = F.World->SpawnActor<AY01GameState>(); F.World->SetGameState(GS);
        auto* Property = FindFProperty<FStructProperty>(GS->GetClass(), TEXT("Snapshot"));
        auto* Snapshot = Property->ContainerPtrToValuePtr<FY01WaveSnapshot>(GS);
        Snapshot->Phase = EY01MatchPhase::Battle; Snapshot->WaveIndex = 3;
        Snapshot->bAutomaticWaves = true; Snapshot->TotalWaves = 4; Snapshot->TotalPendingSpawns = 1;
        Snapshot->Wave.RowName = TEXT("ArbitraryFourth"); F.Present();
        const auto Text = Texts(Panel);
        TestTrue(TEXT("顶部只显示正常末波汇总"), Text.Contains(TEXT("全部波次已出")) && Text.Contains(TEXT("待生成 1")));
        TestFalse(TEXT("不再展示调参观测块或内部ID"), Text.Contains(TEXT("波次观测")) || Text.Contains(TEXT("Arbitrary")));
        return true;
    }
    if (Case == TEXT("NoDebugControls"))
    {
        const auto Text = Texts(Panel); TestFalse(TEXT("正常HUD不展示测试标题"), Text.Contains(TEXT("程序占位")));
        TestFalse(TEXT("无领物按钮"), Text.Contains(TEXT("领物"))); TestFalse(TEXT("无旧定义调试"), Text.Contains(TEXT("EQ_0001"))); return true;
    }
    if (Case == TEXT("SlotCooldown") || Case == TEXT("NoLearnedDrag"))
    {
        auto& S = F.View.Skills[0]; S.LearnedId = FGuid::NewGuid(); S.SkillId = TEXT("HiddenSkill"); S.Display.Name = FText::FromString(TEXT("测试技能"));
        S.Display.SkillType = EY01SkillType::Active; S.Deadline = 18; ++F.View.ViewRevision; F.Present();
        auto* Cell = Panel->GetCell(Z::Skill, 0); if (!TestNotNull(TEXT("真实技能槽"), Cell)) { return false; }
        TestTrue(TEXT("槽内CD"), Texts(Cell).Contains(TEXT("7.0"))); TestFalse(TEXT("不显示内部身份"), Texts(Cell).Contains(TEXT("Hidden")));
        UDragDropOperation* Drag = nullptr; Cell->NativeOnDragDetected(FGeometry(), FPointerEvent(), Drag); TestNull(TEXT("已学不可拖回包"), Drag); return true;
    }
    TStrongObjectPtr<UY01R36Cell> Cell(CreateWidget<UY01R36Cell>(F.World)); const auto CellSlate = Cell->TakeWidget();
    auto* Item = NewObject<UY01R36TileItem>(); Item->ItemId = F.View.Items[0].ItemId; Item->Panel = Panel;
    Cell->NativeOnListItemObjectSet(Item);
    if (Case == TEXT("EntryRebind"))
    {
        Item->ItemId = F.View.Items[1].ItemId; Cell->NativeOnListItemObjectSet(Item);
        TestEqual(TEXT("条目重绑最新身份"), Cell->GetItemId(), Item->ItemId); return true;
    }
    if (Case == TEXT("Placeholder"))
    { TestTrue(TEXT("缺图显示名称和类型"), Texts(Cell.Get()).Contains(TEXT("受控测试装备")) && Texts(Cell.Get()).Contains(TEXT("装备"))); return true; }
    if (Case == TEXT("EquipmentDrag"))
    {
        F.View.Items[0].Location = EY01ItemLocation::EquipmentSlot; F.View.Items[0].Slot = 0; F.View.EquipmentSlots[0] = Item->ItemId;
        ++F.View.ViewRevision; F.Present(); Cell->Configure(Panel, Z::Equipment, 0); Cell->Refresh(11);
    }
    const FPointerEvent Mouse(0, FVector2D(20), FVector2D(20), TSet<FKey>{EKeys::LeftMouseButton}, EKeys::LeftMouseButton, 0, FModifierKeysState());
    TestTrue(TEXT("真实左键检测拖动"), Cell->NativeOnMouseButtonDown(FGeometry(), Mouse).IsEventHandled());
    UDragDropOperation* Raw = nullptr; Cell->NativeOnDragDetected(FGeometry(), Mouse, Raw); auto* Drag = Cast<UY01R36DragOperation>(Raw);
    if (!TestNotNull(TEXT("真实UMG拖动操作"), Drag)) { return false; }
    TestEqual(TEXT("捕获真实实例"), Drag->Identity.ItemId, Item->ItemId); TestNotNull(TEXT("程序拖动可视项"), Drag->DefaultDragVisual.Get());
    if (Case == TEXT("ForeignPanel"))
    { Drag->Origin.Reset(); TestFalse(TEXT("其他面板拖动拒绝"), Panel->Accept(Drag, Z::Equipment, 1)); }
    TestEqual(TEXT("本地拖动不移动"), Panel->GetModel().View().Items.Num(), 3); return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36PanelLifecycleTest, "Y01.Framework.R36.UI.Lifecycle",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36PanelLifecycleTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("LocalInstall"), TEXT("NoLocalPlayer"), TEXT("LateViewport"), TEXT("Idempotent"), TEXT("EndPlay"), TEXT("LostLocalPlayer") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36PanelLifecycleTest::RunTest(const FString& Case)
{
    // 无可见窗口的真实Viewport/UMG生命周期，不是鼠标或网络验证。
    FWidgetWorld F; F.World->InitializeActorsForPlay(FURL());
    TStrongObjectPtr<UGameViewportClient> Client(NewObject<UGameViewportClient>(GEngine));
    auto Overlay = SNew(SOverlay); Client->SetViewportOverlayWidget(nullptr, Overlay);
    auto* Context = GEngine->GetWorldContextFromWorld(F.World); Context->GameViewport = Client.Get();
    auto* PC = F.World->SpawnActor<AY01R36Controller>();
    if (Case == TEXT("NoLocalPlayer"))
    { PC->Tick(0); TestNull(TEXT("没有本地玩家不创建UI"), PC->GetPanel()); Context->GameViewport = nullptr; return true; }
    auto* Local = NewObject<ULocalPlayer>(GEngine); Local->PlayerController = PC; Local->ViewportClient = Client.Get(); PC->Player = Local;
    if (Case == TEXT("LateViewport"))
    { Context->GameViewport = nullptr; PC->Tick(0); TestNull(TEXT("等候实际视口"), PC->GetPanel()); Context->GameViewport = Client.Get(); }
    PC->Tick(0); auto* Panel = PC->GetPanel();
    if (TestNotNull(TEXT("本地控制器安装R36面板"), Panel))
    {
        TestEqual(TEXT("唯一OwningPlayer"), Panel->GetOwningPlayer(), static_cast<APlayerController*>(PC));
        TestTrue(TEXT("真正加入Viewport"), Panel->IsInViewport());
        if (Case == TEXT("Idempotent"))
        { PC->Tick(0); TestEqual(TEXT("不重复创建"), PC->GetPanel(), Panel); TestEqual(TEXT("只有一个屏幕容器"), Overlay->GetChildren()->Num(), 1); }
        if (Case == TEXT("EndPlay"))
        { PC->EndPlay(EEndPlayReason::Quit); PC->Tick(0); TestNull(TEXT("结束后不重建"), PC->GetPanel()); TestFalse(TEXT("旧面板移出Viewport"), Panel->IsInViewport()); }
        if (Case == TEXT("LostLocalPlayer"))
        { PC->Player = nullptr; PC->Tick(0); TestNull(TEXT("失去本地身份释放"), PC->GetPanel()); TestFalse(TEXT("旧私有面板移除"), Panel->IsInViewport()); }
    }
    PC->EndPlay(EEndPlayReason::Quit); Context->GameViewport = nullptr; return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R36NativeTileTest, "Y01.Framework.R36.UI.NativeTile.RealSlate",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R36NativeTileTest::RunTest(const FString&)
{
    FWidgetWorld F; F.Present();
    auto* Bag = F.Panel->GetBackpack(); const auto Widget = Bag->TakeWidget();
    // 引擎通过模板SNew注册类型名，实际STileView的诊断名称使用模板别名。
    if (!TestEqual(TEXT("真实列表必须构建STileView而非错误文本"), Widget->GetTypeAsString(), FString(TEXT("TileViewT<ItemType>")))) { return true; }
    auto Table = StaticCastSharedRef<STableViewBase>(Widget);
    Table->SlatePrepass(1.f);
    Table->Tick(FGeometry::MakeRoot(FVector2D(468, 510), FSlateLayoutTransform()), 0., .016f);
    const auto& Entries = Bag->GetDisplayedEntryWidgets();
    TestEqual(TEXT("实际布局生成三件背包条目"), Entries.Num(), 3);
    for (auto* Entry : Entries)
    {
        const auto* Cell = Cast<UY01R36Cell>(Entry);
        TestTrue(TEXT("真实条目继承程序Cell且绑定已提交物品"), Cell && F.Panel->GetModel().Find(Cell->GetItemId()));
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R36CellWidthTest, "Y01.Framework.R36.UI.NativeTile.LabelFits",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R36CellWidthTest::RunTest(const FString&)
{
    FWidgetWorld F;
    F.View.Items[0].Display.Name = FText::FromString(TEXT("程序测试装备名称较长仍须完整换行")); F.Present();
    auto* Cell = CreateWidget<UY01R36Cell>(F.World); Cell->Configure(F.Panel.Get(), Z::Backpack, INDEX_NONE, F.View.Items[0].ItemId);
    auto* Label = Cast<UTextBlock>(Cell->WidgetTree->FindWidget(TEXT("Label")));
    auto* Row = Label ? Cast<UHorizontalBox>(Label->GetParent()) : nullptr;
    if (!TestNotNull(TEXT("真实原生条目文本容器"), Row)) { return true; }
    auto Slate = Row->TakeWidget(); Slate->SlatePrepass(1.f);
    FArrangedChildren Children(EVisibility::Visible);
    Slate->ArrangeChildren(FGeometry::MakeRoot(FVector2D(136, 94), FSlateLayoutTransform()), Children);
    if (TestEqual(TEXT("图标和名称真实布局"), Children.Num(), 2))
    {
        const auto& G = Children[1].Geometry;
        TestTrue(TEXT("名称约束在格内剩余宽度，不能按未换行期望宽度横向溢出"), G.Position.X + G.GetLocalSize().X <= 136.01f);
        TestTrue(TEXT("名称仍有可用宽度"), G.GetLocalSize().X >= 100.f);
    }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R36AdvanceBoundsTest, "Y01.Framework.R36.UI.NativeTile.TrainingFits",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R36AdvanceBoundsTest::RunTest(const FString&)
{
    FWidgetWorld F; F.Present();
    TestNull(TEXT("正常HUD无手动下一波按钮"), F.Panel->WidgetTree->FindWidget(TEXT("Advance")));
    auto* Button = Cast<UButton>(F.Panel->WidgetTree->FindWidget(TEXT("Training")));
    if (!TestNotNull(TEXT("边缘练功房按钮"), Button)) { return true; }
    auto* Text = Cast<UTextBlock>(Button->GetContent()); auto* Slot = Cast<UCanvasPanelSlot>(Button->Slot);
    if (!TestTrue(TEXT("真实按钮文字及固定布局"), Text && Slot)) { return true; }
    auto* ContentSlot = Cast<UButtonSlot>(Text->Slot);
    if (!TestNotNull(TEXT("真实按钮内容槽"), ContentSlot)) { return true; }
    const FVector2D Size = Slot->GetSize();
    const FMargin Padding = Button->GetStyle().NormalPadding + ContentSlot->GetPadding();
    // 按真实按钮内容可用宽度测量换行后的文字，不以未换行首帧DesiredSize代替。
    Text->SetWrapTextAt(Size.X - Padding.Left - Padding.Right);
    auto Slate = Text->TakeWidget(); Slate->SlatePrepass(1.f);
    AddInfo(FString::Printf(TEXT("R36按钮测量 Width=%g Height=%g TextHeight=%g PaddingY=%g"),
        Size.X, Size.Y, Slate->GetDesiredSize().Y, Padding.Top + Padding.Bottom));
    TestTrue(TEXT("全部文字和按钮内边距必须容纳在背景高度内"),
        Slate->GetDesiredSize().Y + Padding.Top + Padding.Bottom <= Size.Y + .01f);
    TestTrue(TEXT("按钮底部不覆盖状态区"), Slot->GetPosition().Y + Size.Y <= 746.f);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R36TraceBoundsTest, "Y01.Framework.R36.UI.NativeTile.TraceBounds",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R36TraceBoundsTest::RunTest(const FString&)
{
    FWidgetWorld F; F.Present();
    TSharedPtr<FJsonObject> J; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Y01R36Trace::PanelRecord(F.Panel.Get())), J);
    if (!TestTrue(TEXT("真实面板观测可解析"), J.IsValid())) { return true; }
    auto Check = [&](const TSharedPtr<FJsonObject>& Geometry)
    {
        const TArray<TSharedPtr<FJsonValue>>* Rect = nullptr;
        if (TestTrue(TEXT("每个几何对象必须提供四边矩形而非仅中心点"), Geometry->TryGetArrayField(TEXT("rect"), Rect) && Rect && Rect->Num() == 4))
        { for (const auto& Value : *Rect) { TestEqual(TEXT("未布局时矩形为零，不能伪造可见范围"), Value->AsNumber(), 0.); } }
        bool LaidOut = true; Geometry->TryGetBoolField(TEXT("laidOut"), LaidOut);
        TestFalse(TEXT("无真实Viewport不能宣称已布局"), LaidOut);
    };
    Check(J->GetObjectField(TEXT("bagGeometry"))); Check(J->GetObjectField(TEXT("Training")));
    for (const auto& Cell : J->GetArrayField(TEXT("cells"))) { Check(Cell->AsObject()); }
    TestEqual(TEXT("只读几何不改变提交版本"), F.Panel->GetModel().View().ViewRevision, F.View.ViewRevision);
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36EconomyWidgetTest, "Y01.Framework.R36.RuntimeIntegration.UIWidget",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36EconomyWidgetTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("ShopList"), TEXT("Purchased"), TEXT("NoFunds"), TEXT("TalentList"), TEXT("Chosen"),
        TEXT("MaterialMode"), TEXT("FiveMaterials"), TEXT("Waiting"), TEXT("Ended"), TEXT("Dead"), TEXT("NoOwner"), TEXT("StableOrder")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36EconomyWidgetTest::RunTest(const FString& Case)
{
    FWidgetWorld F; auto* Panel = F.Panel.Get(); auto& V = F.View; V.Gold = 200;
    while (V.Items.Num() < 5) { auto Item = V.Items[0]; Item.ItemId = FGuid::NewGuid(); V.Items.Add(Item); }
    for (auto& Item : V.Items) { Item.Display.Quality = EY01Quality::Green; }
    for (int32 I = 0; I < 2; ++I)
    {
        auto& Offer = V.ShopOffers.AddDefaulted_GetRef(); Offer.OfferId = I ? TEXT("HiddenAlpha") : TEXT("HiddenZulu");
        Offer.DisplayName = FText::FromString(I ? TEXT("另一个商品") : TEXT("测试购买装备")); Offer.Price = I ? 50 : 10;
    }
    V.SynthesisRecipes.AddDefaulted_GetRef().RecipeId = TEXT("HiddenRecipe");
    V.TalentChoice.ChoiceId = FGuid::NewGuid();
    for (int32 I = 0; I < 3; ++I)
    {
        auto& T = V.TalentChoice.Candidates.AddDefaulted_GetRef(); T.TalentId = FName(*FString::Printf(TEXT("HiddenTalent%d"), I));
        T.DisplayName = FText::FromString(FString::Printf(TEXT("测试天赋%d"), I + 1)); T.Quality = EY01Quality::Blue;
        T.Effects.Add({TEXT("HiddenEffect"), EY01SourceEffectKind::GoldPerSecond, 1.});
    }
    if (Case == TEXT("Purchased")) { V.ShopOffers[0].bPurchased = true; }
    if (Case == TEXT("Chosen")) { V.TalentChoice.SelectedTalent = V.TalentChoice.Candidates[1].TalentId; }
    if (Case == TEXT("NoFunds")) { V.Gold = 0; }
    if (Case == TEXT("Dead")) { V.Health = 0; }
    const auto Phase = Case == TEXT("Waiting") ? EY01MatchPhase::Waiting : Case == TEXT("Ended") ? EY01MatchPhase::Ended : EY01MatchPhase::Preparing;
    Panel->Present(V, Phase, 11);
    auto Button = [&](const TCHAR* Name) { auto* B = Cast<UButton>(Panel->WidgetTree->FindWidget(Name)); TestNotNull(TEXT("经济真实按钮"), B); return B; };
    auto* Buy = Button(TEXT("Purchase_0")); auto* Talent = Button(TEXT("Talent_0"));
    auto* Mode = Button(TEXT("MaterialMode")); auto* Craft = Button(TEXT("Synthesize"));
    if (!Buy || !Talent || !Mode || !Craft) { return true; }
    const bool Blocked = Case == TEXT("Waiting") || Case == TEXT("Ended") || Case == TEXT("Dead");
    TestEqual(TEXT("商品按钮按权威限购余额和阶段禁用"), Buy->GetIsEnabled(), !Blocked && Case != TEXT("Purchased") && Case != TEXT("NoFunds"));
    TestEqual(TEXT("天赋一次选择禁用"), Talent->GetIsEnabled(), !Blocked && Case != TEXT("Chosen"));
    TestFalse(TEXT("未选择五件不能合成"), Craft->GetIsEnabled());
    const auto* BuyText = Cast<UTextBlock>(Buy->GetContent());
    if (TestNotNull(TEXT("购买显示名称与价格"), BuyText))
    {
        TestTrue(TEXT("保留Profile顺序不按ID排序"), BuyText->GetText().ToString().Contains(TEXT("测试购买装备")));
        TestTrue(TEXT("显示10金币价格"), BuyText->GetText().ToString().Contains(TEXT("10")));
        TestFalse(TEXT("不显示内部商品ID"), BuyText->GetText().ToString().Contains(TEXT("Hidden")));
    }
    if (Case == TEXT("StableOrder"))
    { ++V.ViewRevision; ++V.Gold; Panel->Present(V, Phase, 11); TestEqual(TEXT("收入刷新不重建商品按钮"), Button(TEXT("Purchase_0")), Buy); }
    if (Case == TEXT("TalentList") || Case == TEXT("Chosen"))
    {
        for (int32 I = 0; I < 3; ++I)
        {
            auto* B = Button(*FString::Printf(TEXT("Talent_%d"), I)); if (!B) { continue; }
            const auto* Text = Cast<UTextBlock>(B->GetContent());
            TestTrue(TEXT("天赋显示名称品质和说明"), Text && Text->GetText().ToString().Contains(TEXT("测试天赋"))
                && Text->GetText().ToString().Contains(TEXT("蓝")) && B->GetToolTipText().ToString().Contains(TEXT("金币/秒")));
            TestEqual(TEXT("任一选择后全组禁用"), B->GetIsEnabled(), Case != TEXT("Chosen"));
        }
    }
    if (Case == TEXT("NoOwner"))
    {
        Buy->OnClicked.Broadcast(); Talent->OnClicked.Broadcast();
        TestFalse(TEXT("无owner不能发送或进入假等待"), Panel->GetModel().IsPending());
        TestEqual(TEXT("无owner不改金币"), Panel->GetModel().View().Gold, 200);
    }
    if (Case == TEXT("MaterialMode") || Case == TEXT("FiveMaterials"))
    {
        TestTrue(TEXT("默认保留拖动"), Panel->CanDrag()); Mode->OnClicked.Broadcast();
        TestFalse(TEXT("显式材料选择与拖动输入互斥"), Panel->CanDrag());
        const FPointerEvent Mouse(0, FVector2D(20), FVector2D(20), TSet<FKey>{EKeys::LeftMouseButton}, EKeys::LeftMouseButton, 0, FModifierKeysState());
        for (const auto& Item : V.Items)
        {
            auto* Cell = CreateWidget<UY01R36Cell>(F.World); Cell->Configure(Panel, Z::Backpack, INDEX_NONE, Item.ItemId);
            const auto CellSlate = Cell->TakeWidget();
            TestTrue(TEXT("真实Cell左键选择材料"), Cell->NativeOnMouseButtonDown(FGeometry(), Mouse).IsEventHandled());
        }
        TestEqual(TEXT("五件真实选择身份"), Panel->GetModel().Materials().Num(), 5);
        TestTrue(TEXT("五件后免费合成按钮可用"), Craft->GetIsEnabled());
        TestTrue(TEXT("按钮明确免费"), CastChecked<UTextBlock>(Craft->GetContent())->GetText().ToString().Contains(TEXT("免费")));
        Craft->OnClicked.Broadcast(); TestEqual(TEXT("无owner不提前耗材"), Panel->GetBackpack()->GetNumItems(), 5);
        Mode->OnClicked.Broadcast(); TestTrue(TEXT("退出选择恢复真实拖动"), Panel->CanDrag());
    }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36PresentationWidgetTest, "Y01.Framework.R36.Presentation.Widget",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36PresentationWidgetTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("FlightLayer"), TEXT("UnlaidHint"), TEXT("HiddenHint"), TEXT("UnscrolledHint"),
        TEXT("ArrivalFrame"), TEXT("RebindFrame"), TEXT("RepeatPublish"), TEXT("NoAuthorityNoFlash")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36PresentationWidgetTest::RunTest(const FString& Case)
{
    FWidgetWorld F; auto* P = F.Panel.Get(); auto& V = F.View;
    const auto First = V.Items[0].ItemId;
    if (Case != TEXT("NoAuthorityNoFlash")) { V.Items[0].ArrivalDeliveryId = FGuid::NewGuid(); }
    if (Case == TEXT("UnscrolledHint"))
    {
        for (int32 I = 3; I < 500; ++I)
        { auto Item = V.Items[1]; Item.ItemId = FGuid::NewGuid(); Item.AcquisitionOrder = I + 1; V.Items.Add(Item); }
        V.Items[0].ArrivalDeliveryId = {}; V.Items.Last().ArrivalDeliveryId = FGuid::NewGuid();
    }
    P->Present(V, EY01MatchPhase::Preparing, 20);
    if (Case == TEXT("FlightLayer"))
    {
        auto* Layer = P->WidgetTree->FindWidget(TEXT("DeliveryFlightLayer"));
        if (TestNotNull(TEXT("真实UMG品质飞行层"), Layer))
        { TestEqual(TEXT("飞行与轨迹无输入碰撞"), Layer->GetVisibility(), ESlateVisibility::HitTestInvisible); }
        return true;
    }
    if (Case == TEXT("UnlaidHint") || Case == TEXT("HiddenHint"))
    {
        if (Case == TEXT("HiddenHint")) { P->SetVisibility(ESlateVisibility::Hidden); }
        P->Present(V, EY01MatchPhase::Preparing, 30);
        TestTrue(TEXT("面板显示未查看到货提示而非内部身份"), Texts(P).Contains(TEXT("待显示到货 1")));
        TestEqual(TEXT("未真正显示不能消费提示"), P->GetModel().UnseenArrivals(), 1);
        TestEqual(TEXT("未布局不闪"), P->GetModel().FlashRemaining(First, 30), 0.); return true;
    }
    // 真实Slate绘制缓存几何；不把生成条目本身当作已显示，也不冒充D3D12截图。
    auto Window = SNew(SWindow).ClientSize(FVector2D(1280,800));
    auto Slate = P->TakeWidget(); Window->SetContent(Slate); FHittestGrid Grid;
    Grid.SetHittestArea(FVector2D::ZeroVector, FVector2D(1280,800));
    for (int32 I = 0; I < 3; ++I)
    {
        Slate->SlatePrepass(1.f); FSlateWindowElementList Elements(Window);
        FPaintArgs Args(&Window.Get(), Grid, FVector2D::ZeroVector, 20 + I * .016, .016f);
        Slate->Paint(Args, FGeometry::MakeRoot(FVector2D(1280,800), FSlateLayoutTransform()),
            FSlateRect(0,0,1280,800), Elements, 0, FWidgetStyle(), true);
    }
    P->Present(V, EY01MatchPhase::Preparing, 20);
    if (Case == TEXT("UnscrolledHint"))
    {
        TestEqual(TEXT("500件末件未滚到仍保留"), P->GetModel().UnseenArrivals(), 1);
        TestTrue(TEXT("未滚到提示实际显示"), Texts(P).Contains(TEXT("待显示到货 1"))); return true;
    }
    UY01R36Cell* Cell = nullptr;
    for (auto* W : P->GetBackpack()->GetDisplayedEntryWidgets())
    { auto* C = Cast<UY01R36Cell>(W); if (C && C->GetItemId() == First) { Cell = C; break; } }
    if (!TestNotNull(TEXT("真实布局产生目标包格"), Cell)) { return true; }
    const bool Arrival = Case != TEXT("NoAuthorityNoFlash");
    TestEqual(TEXT("只有权威到货首次可见闪0.6秒"), P->GetModel().FlashRemaining(First, 20), Arrival ? .6 : 0.);
    auto* Frame = Cast<UBorder>(Cell->WidgetTree->RootWidget);
    if (!TestNotNull(TEXT("真实包格背景"), Frame)) { return true; }
    const auto Flash = Frame->GetBrushColor();
    if (Case == TEXT("RebindFrame"))
    {
        Cell->NativeOnListItemObjectSet(P->GetBackpack()->GetItemAt(1));
        TestNotEqual(TEXT("复用条目不携带前件闪光背景"), Frame->GetBrushColor(), Flash);
        Cell->NativeOnListItemObjectSet(P->GetBackpack()->GetItemAt(0)); Cell->Refresh(21);
    }
    ++V.ViewRevision; P->Present(V, EY01MatchPhase::Preparing, 20.6); Cell->Refresh(20.6);
    if (Arrival) { TestNotEqual(TEXT("0.6秒后背景恢复品质暗色"), Frame->GetBrushColor(), Flash); }
    P->Present(V, EY01MatchPhase::Preparing, 22);
    TestEqual(TEXT("重复视图不重闪"), P->GetModel().FlashRemaining(First, 22), 0.);
    TestEqual(TEXT("表现不发奖或更改库存"), P->GetModel().View().Items.Num(), 3);
    TestFalse(TEXT("动画不发送交易RPC"), P->GetModel().IsPending()); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R36FlightWidgetsTest, "Y01.Framework.R36.Presentation.FlightWidgets",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R36FlightWidgetsTest::RunTest(const FString&)
{
    FWidgetWorld F; F.Present();
    auto* Layer = NewObject<UY01R36FlightLayer>(F.Panel.Get());
    TArray<FY01R36ProjectedFlight> Flights;
    for (int32 Q = 0; Q < 5; ++Q)
    {
        auto& P = Flights.AddDefaulted_GetRef(); P.Flight.Record.DeliveryId = FGuid::NewGuid();
        P.Flight.Record.Quality = static_cast<EY01Quality>(Q); P.Flight.Alpha = .5;
        P.Origin = FVector2D(100,300); P.Target = FVector2D(500,300);
    }
    Layer->Present(Flights);
    TestEqual(TEXT("品质彩点及四段轨迹均为UMG图像"), Layer->GetChildrenCount(), 25);
    const FLinearColor Colors[] = {{.12f,.65f,.28f}, {.15f,.4f,.9f}, {.58f,.22f,.85f}, {.9f,.68f,.12f}, {.85f,.14f,.2f}};
    for (int32 Q = 0; Q < 5; ++Q)
    {
        auto* Head = Cast<UImage>(Layer->GetChildAt(Q * 5));
        if (!TestNotNull(TEXT("每品质真实彩点"), Head)) { continue; }
        TestEqual(TEXT("品质颜色不丢失"), Head->GetColorAndOpacity(), Colors[Q]);
        TestEqual(TEXT("彩点无输入碰撞"), Head->GetVisibility(), ESlateVisibility::HitTestInvisible);
        auto* Slot = CastChecked<UCanvasPanelSlot>(Head->Slot);
        TestTrue(TEXT("半程位于投影起终点之间并有弧线"), FMath::IsNearlyEqual(Slot->GetPosition().X + 8, 300.)
            && Slot->GetPosition().Y + 8 < 300);
    }
    Layer->Present(Flights); TestEqual(TEXT("重复Record表现不重复图元"), Layer->GetChildrenCount(), 25);
    Layer->Present({});
    for (auto* W : Layer->GetAllChildren()) { TestEqual(TEXT("禁播清空可见图元"), W->GetVisibility(), ESlateVisibility::Collapsed); }
    TestEqual(TEXT("重复播及禁播不改变库存"), F.Panel->GetModel().View().Items.Num(), 3);
    auto* Toggle = Cast<UButton>(F.Panel->WidgetTree->FindWidget(TEXT("ToggleFlights")));
    if (TestNotNull(TEXT("只有本地表现开关"), Toggle))
    {
        Toggle->OnClicked.Broadcast();
        TestTrue(TEXT("开关明确禁播状态"), CastChecked<UTextBlock>(Toggle->GetContent())->GetText().ToString().Contains(TEXT("关")));
        TestFalse(TEXT("禁播不发交易RPC"), F.Panel->GetModel().IsPending());
    }
    return true;
}
namespace
{
// 无窗口尺寸夹具；相机和世界投影仍使用引擎真实路径，不作为可见证据。
class FFlightViewport : public FViewport
{
public:
    FFlightViewport() : FViewport(nullptr) {}
    virtual FIntPoint GetSizeXY() const override { return FIntPoint(1280,800); }
    virtual void* GetWindow() override { return nullptr; }
    virtual void MoveWindow(int32, int32, int32, int32) override {}
    virtual void Destroy() override {}
    virtual void InvalidateDisplay() override {}
    virtual FViewportFrame* GetViewportFrame() override { return nullptr; }
    virtual bool SetUserFocus(bool) override { return false; }
    virtual bool KeyState(FKey) const override { return false; }
    virtual int32 GetMouseX() const override { return 0; }
    virtual int32 GetMouseY() const override { return 0; }
    virtual void GetMousePos(FIntPoint& P, bool) override { P = FIntPoint::ZeroValue; }
    virtual void SetMouse(int32, int32) override {}
    virtual void ProcessInput(float) override {}
    virtual FVector2D VirtualDesktopPixelToViewport(FIntPoint) const override { return FVector2D::ZeroVector; }
    virtual FIntPoint ViewportToVirtualDesktopPixel(FVector2D) const override { return FIntPoint::ZeroValue; }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36FlightProjectionTest, "Y01.Framework.R36.Presentation.Projection",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36FlightProjectionTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("OriginTarget"), TEXT("LayerTransform"), TEXT("FollowOwner"), TEXT("MissingPawn"),
        TEXT("HiddenPawn"), TEXT("Offscreen"), TEXT("Behind"), TEXT("NoLocal"), TEXT("Unlaid"), TEXT("Duplicate")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36FlightProjectionTest::RunTest(const FString& Case)
{
    FWidgetWorld F; F.World->InitializeActorsForPlay(FURL());
    auto* PC = F.World->SpawnActor<AY01R36Controller>(); auto* Pawn = F.World->SpawnActor<APawn>();
    auto* Root = NewObject<USceneComponent>(Pawn); Pawn->SetRootComponent(Root); Root->RegisterComponent();
    Pawn->SetActorLocation(FVector(500,100,100)); PC->Possess(Pawn);
    FFlightViewport Viewport;
    TStrongObjectPtr<UGameViewportClient> Client(NewObject<UGameViewportClient>(GEngine));
    Client->Viewport = &Viewport;
    auto* Local = NewObject<ULocalPlayer>(GEngine); Local->PlayerController = PC; Local->ViewportClient = Client.Get();
    Local->Origin = FVector2D::ZeroVector; Local->Size = FVector2D(1,1); PC->Player = Local;
    FMinimalViewInfo Camera; Camera.Location = FVector(0,0,100); Camera.Rotation = FRotator::ZeroRotator;
    Camera.FOV = 90; Camera.AspectRatio = 1.6f; F.World->TimeSeconds = 1;
    PC->PlayerCameraManager->FillCameraCache(Camera);
    F.View.Delivery.Records.AddDefaulted(); auto& R = F.View.Delivery.Records.Last();
    R.DeliveryId = FGuid::NewGuid(); R.ItemId = FGuid::NewGuid(); R.MatchId = F.View.MatchId; R.JoinSequence = 1;
    R.Origin = FVector(500,-100,100); R.StartTime = 10; R.ETA = 11; R.Quality = EY01Quality::Gold;
    F.Panel->Present(F.View, EY01MatchPhase::Battle, 10.5);
    auto Flights = F.Panel->GetModel().Flights(10.5, true);
    if (!TestEqual(TEXT("夹具完整视图产生一个合法Pending样本"), Flights.Num(), 1))
    { Client->Viewport = nullptr; PC->Player = nullptr; return false; }
    FVector2D FixtureOrigin, FixtureTarget;
    TestTrue(TEXT("夹具引擎起点可投影"), PC->ProjectWorldLocationToScreen(R.Origin, FixtureOrigin));
    TestTrue(TEXT("夹具引擎收件角色可投影"), PC->ProjectWorldLocationToScreen(Pawn->GetActorLocation(), FixtureTarget));
    if (Case == TEXT("Duplicate")) { const auto Copy = Flights[0]; Flights.Add(Copy); }
    if (Case == TEXT("MissingPawn")) { PC->UnPossess(); }
    if (Case == TEXT("HiddenPawn")) { Pawn->SetActorHiddenInGame(true); }
    if (Case == TEXT("Offscreen")) { Pawn->SetActorLocation(FVector(500,10000,100)); }
    if (Case == TEXT("Behind")) { Flights[0].Record.Origin.X = -500; }
    if (Case == TEXT("NoLocal")) { PC->Player = nullptr; }
    if (Case == TEXT("FollowOwner")) { Pawn->SetActorLocation(FVector(500,200,100)); }
    const auto Host = FGeometry::MakeRoot(FVector2D(1280,800), FSlateLayoutTransform(1.f,FVector2f(40,60)));
    const auto Layer = Case == TEXT("Unlaid") ? FGeometry() : Case == TEXT("LayerTransform")
        ? FGeometry::MakeRoot(FVector2D(640,400), FSlateLayoutTransform(2.f,FVector2f(50,80))) : Host;
    const auto Projected = UY01R36FlightLayer::Project(PC, Flights, Host, Layer);
    const bool Visible = Case == TEXT("OriginTarget") || Case == TEXT("LayerTransform") || Case == TEXT("FollowOwner") || Case == TEXT("Duplicate");
    TestEqual(TEXT("投影只对有效可见收件角色且Record去重"), Projected.Num(), Visible ? 1 : 0);
    if (Projected.Num() == 1)
    {
        FVector2D Origin, Target;
        TestTrue(TEXT("夹具引擎起点可投影"), PC->ProjectWorldLocationToScreen(R.Origin, Origin));
        TestTrue(TEXT("夹具引擎收件角色可投影"), PC->ProjectWorldLocationToScreen(Pawn->GetActorLocation(), Target));
        TestTrue(TEXT("冻结怪物起点转为UMG局部位置"), Projected[0].Origin.Equals(Layer.AbsoluteToLocal(Host.LocalToAbsolute(Origin)), .01));
        TestTrue(TEXT("只追踪当前收件角色并处理DPI与偏移"), Projected[0].Target.Equals(Layer.AbsoluteToLocal(Host.LocalToAbsolute(Target)), .01));
        TestEqual(TEXT("不改权威ETA"), Projected[0].Flight.Record.ETA, 11.);
        TestEqual(TEXT("同步时间半程"), Projected[0].Flight.Alpha, .5);
    }
    TestEqual(TEXT("投影不改变包"), F.Panel->GetModel().View().Items.Num(), 3);
    Client->Viewport = nullptr; PC->Player = nullptr; return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36FlightPanelTest, "Y01.Framework.R36.Presentation.Panel",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36FlightPanelTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Active"), TEXT("Repeat"), TEXT("Suppressed"), TEXT("Dead"), TEXT("Ended"),
        TEXT("LostOwner"), TEXT("NoGameState"), TEXT("EmptyView"), TEXT("Arrival"), TEXT("HiddenPawn"),
        TEXT("NewSession"), TEXT("Clock"), TEXT("Busy"), TEXT("HostDPI")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36FlightPanelTest::RunTest(const FString& Case)
{
    // 仅表现接线夹具：整份提交经真实读取入口、Slate Tick和GameState时钟消费，不冒充业务击杀或可见场。
    FWidgetWorld F; F.World->InitializeActorsForPlay(FURL()); F.View.Items.Reset();
    auto* PC = F.World->SpawnActor<AY01R36Controller>(); auto* Pawn = F.World->SpawnActor<APawn>();
    auto* Root = NewObject<USceneComponent>(Pawn); Pawn->SetRootComponent(Root); Root->RegisterComponent();
    Pawn->SetActorLocation(FVector(500,100,100)); PC->Possess(Pawn);
    auto* PS = F.World->SpawnActor<AY01PlayerState>(); PS->SetOwner(PC); PC->PlayerState = PS;
    auto* GS = F.World->SpawnActor<AY01GameState>(); F.World->SetGameState(GS);
    auto* Snapshot = FindFProperty<FStructProperty>(GS->GetClass(), TEXT("Snapshot"))->ContainerPtrToValuePtr<FY01WaveSnapshot>(GS);
    Snapshot->Phase = EY01MatchPhase::Battle;
    auto* Committed = FindFProperty<FStructProperty>(PS->GetLoadout()->GetClass(), TEXT("CommittedView"))
        ->ContainerPtrToValuePtr<FY01CommittedView>(PS->GetLoadout());
    FFlightViewport Viewport;
    TStrongObjectPtr<UGameViewportClient> Client(NewObject<UGameViewportClient>(GEngine)); Client->Viewport = &Viewport;
    auto* Local = NewObject<ULocalPlayer>(GEngine); Local->PlayerController = PC; Local->ViewportClient = Client.Get();
    Local->Origin = FVector2D::ZeroVector; Local->Size = FVector2D(1,1); PC->Player = Local;
    F.Panel->SetOwningPlayer(PC);
    ON_SCOPE_EXIT { F.Panel->SetOwningPlayer(nullptr); Client->Viewport = nullptr; PC->Player = nullptr; F.World->SetGameState(nullptr); };
    FMinimalViewInfo Camera; Camera.Location = FVector(0,0,100); Camera.Rotation = FRotator::ZeroRotator;
    Camera.FOV = 90; Camera.AspectRatio = 1.6f; F.World->TimeSeconds = 10.5;
    PC->PlayerCameraManager->FillCameraCache(Camera);
    auto& R = F.View.Delivery.Records.AddDefaulted_GetRef();
    R.DeliveryId = FGuid::NewGuid(); R.ItemId = FGuid::NewGuid(); R.MatchId = F.View.MatchId; R.JoinSequence = 1;
    R.Origin = FVector(500,-100,100); R.StartTime = 10; R.ETA = 11; R.Quality = EY01Quality::Gold;
    *Committed = F.View;
    auto* Layer = CastChecked<UY01R36FlightLayer>(F.Panel->WidgetTree->FindWidget(TEXT("DeliveryFlightLayer")));
    const float Scale = Case == TEXT("HostDPI") ? 2.f : 1.f;
    const auto Host = FGeometry::MakeRoot(FVector2D(1280 / Scale,800 / Scale), FSlateLayoutTransform(Scale,FVector2f(40,60)));
    auto Window = SNew(SWindow).ClientSize(FVector2D(1280,800)); auto Slate = F.Panel->TakeWidget(); Window->SetContent(Slate);
    FHittestGrid Grid; Grid.SetHittestArea(FVector2D::ZeroVector, FVector2D(1400,900));
    for (int32 I = 0; I < 3; ++I)
    {
        Slate->SlatePrepass(Scale); FSlateWindowElementList Elements(Window);
        FPaintArgs Args(&Window.Get(), Grid, FVector2D::ZeroVector, 100 + I * .016, .016f);
        Slate->Paint(Args, Host, FSlateRect(0,0,1400,900), Elements, 0, FWidgetStyle(), true);
    }
    auto Tick = [&]() { Slate->Tick(Host, 999., .016f); };
    auto Visible = [&]() { int32 N = 0; for (auto* W : Layer->GetAllChildren()) { N += W->GetVisibility() != ESlateVisibility::Collapsed; } return N; };
    Tick();
    TestEqual(TEXT("面板真实Tick消费整份提交并产生头点和四段轨迹"), Visible(), 5);
    TestEqual(TEXT("只使用GameState同步时钟而非Slate墙钟"), F.Panel->GetServerTime(), 10.5);
    if (Visible() == 5)
    {
        const auto Expected = UY01R36FlightLayer::Project(PC, F.Panel->GetModel().Flights(10.5, true), Host, Layer->GetCachedGeometry());
        if (TestEqual(TEXT("实际几何可投影"), Expected.Num(), 1))
        {
            const auto& P = Expected[0]; FVector2D Center = FMath::Lerp(P.Origin, P.Target, .5);
            Center.Y -= FMath::Min(100., FVector2D::Distance(P.Origin, P.Target) * .25);
            const auto* CanvasSlot = CastChecked<UCanvasPanelSlot>(Layer->GetChildAt(0)->Slot);
            TestTrue(TEXT("面板及飞行层DPI换算一致"), (CanvasSlot->GetPosition() + FVector2D(8)).Equals(Center,.01));
        }
    }
    if (Case == TEXT("Suppressed")) { F.Panel->ToggleFlights(); }
    if (Case == TEXT("Dead")) { Committed->Health = 0; ++Committed->ViewRevision; }
    if (Case == TEXT("Ended")) { Snapshot->Phase = EY01MatchPhase::Ended; }
    if (Case == TEXT("LostOwner")) { PS->SetOwner(nullptr); }
    if (Case == TEXT("NoGameState")) { F.World->SetGameState(nullptr); }
    if (Case == TEXT("EmptyView")) { Committed->ViewRevision = 0; }
    if (Case == TEXT("HiddenPawn")) { Pawn->SetActorHiddenInGame(true); }
    if (Case == TEXT("NewSession"))
    { Committed->MatchId = FGuid::NewGuid(); ++Committed->JoinSequence; Committed->Delivery.Records.Reset(); ++Committed->ViewRevision; }
    if (Case == TEXT("Arrival"))
    {
        Committed->Delivery.Records[0].State = EY01DeliveryState::Delivered;
        auto& Item = Committed->Items.AddDefaulted_GetRef(); Item.ItemId = R.ItemId; Item.ArrivalDeliveryId = R.DeliveryId;
        ++Committed->ViewRevision;
    }
    if (Case == TEXT("Clock")) { F.World->TimeSeconds = 11; }
    if (Case == TEXT("Repeat")) { ++Committed->ViewRevision; }
    if (Case == TEXT("Busy")) { Committed->Response.Code = EY01ItemError::Busy; ++Committed->ViewRevision; }
    Tick();
    const bool StillVisible = Case == TEXT("Active") || Case == TEXT("Repeat") || Case == TEXT("Busy") || Case == TEXT("HostDPI");
    TestEqual(TEXT("到期禁播死亡结束及失去可信输入清空旧图元，重复提交与忙不增生"), Visible(), StillVisible ? 5 : 0);
    TestEqual(TEXT("动画只显示权威物品，不由ETA或回调发奖"), F.Panel->GetModel().View().Items.Num(), Case == TEXT("Arrival") ? 1 : 0);
    TestFalse(TEXT("面板播放不构造领奖交易"), F.Panel->GetModel().IsPending());
    if (Case == TEXT("Suppressed")) { F.Panel->ToggleFlights(); Tick(); TestEqual(TEXT("本地恢复只重播表现"), Visible(), 5); }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36TooltipTest, "Y01.Framework.R36.Presentation.Tooltip",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36TooltipTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Repeat"), TEXT("Publish"), TEXT("Empty"), TEXT("LostPanel"), TEXT("Growth"), TEXT("Rebind")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36TooltipTest::RunTest(const FString& Case)
{
    FWidgetWorld F; auto& V = F.View;
    auto& G = V.Items[0].Growth.AddDefaulted_GetRef(); G.Delta = 1; G.Accumulated = 3;
    V.Items[1].Growth.AddDefaulted_GetRef().Delta = 1;
    F.Present();
    TStrongObjectPtr<UY01R36Cell> Cell(CreateWidget<UY01R36Cell>(F.World));
    Cell->Configure(F.Panel.Get(), Z::Backpack, INDEX_NONE, V.Items[0].ItemId);
    const auto Slate = Cell->TakeWidget(); const auto Before = Slate->GetToolTip();
    if (!TestTrue(TEXT("夹具真实Slate已持有实例累计3提示"), Before.IsValid() && Cell->GetToolTipText().ToString().Contains(TEXT("实例累计 3")))) { return false; }
    if (Case == TEXT("Publish")) { ++V.ViewRevision; ++V.Gold; F.Present(); }
    if (Case == TEXT("Growth")) { V.Items[0].Growth[0].Accumulated = 4; ++V.ViewRevision; F.Present(); }
    if (Case == TEXT("Empty")) { Cell->Configure(F.Panel.Get(), Z::Equipment, 0); }
    if (Case == TEXT("LostPanel")) { Cell->Configure(nullptr, Z::Backpack, INDEX_NONE); }
    if (Case == TEXT("Rebind")) { Cell->Configure(F.Panel.Get(), Z::Backpack, INDEX_NONE, V.Items[1].ItemId); }
    Cell->Refresh(11.1);
    if (Case == TEXT("Repeat") || Case == TEXT("Publish"))
    { TestTrue(TEXT("同内容刷新必须保留Slate提示对象，不能每帧重新打开"), Slate->GetToolTip() == Before); }
    else if (Case == TEXT("Empty") || Case == TEXT("LostPanel"))
    { TestTrue(TEXT("空槽及失去面板清除旧实例提示"), Cell->GetToolTipText().IsEmpty() && !Slate->GetToolTip().IsValid()); }
    else
    { TestTrue(TEXT("成长变化或另一实例重绑显示最新累计"), Cell->GetToolTipText().ToString().Contains(Case == TEXT("Growth") ? TEXT("实例累计 4") : TEXT("实例累计 0"))); }
    TestFalse(TEXT("提示不显示内部身份"), Cell->GetToolTipText().ToString().Contains(TEXT("Hidden")));
    TestFalse(TEXT("提示没有构造领奖或其他交易"), F.Panel->GetModel().IsPending());
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36PanelToggleTest, "Y01.Framework.R36.UI.PanelToggle",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36PanelToggleTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Default"), TEXT("Retain"), TEXT("Pending"), TEXT("Confirmation"), TEXT("Dragging"), TEXT("HiddenHint"), TEXT("HiddenGeometry")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36PanelToggleTest::RunTest(const FString& Case)
{
    FWidgetWorld F(false); auto* P = F.Panel.Get(); auto& V = F.View;
    V.Gold = 120; V.IncomeRate = 2.5; V.SynthesisRecipes.AddDefaulted_GetRef().RecipeId = TEXT("Recipe");
    V.ShopOffers.AddDefaulted_GetRef().OfferId = TEXT("Offer"); F.Present();
    TestTrue(TEXT("三面板收起不禁用常驻区域"), P->CanDrag());
    TestFalse(TEXT("默认隐藏背包不能作为拖动源"), P->CanDragFrom(Z::Backpack));
    auto* Toggle = Cast<UButton>(P->WidgetTree->FindWidget(TEXT("ToggleBackpack")));
    auto* Details = P->WidgetTree->FindWidget(TEXT("BackpackPanel"));
    auto* Income = Cast<UTextBlock>(P->WidgetTree->FindWidget(TEXT("TopVitals")));
    if (!TestNotNull(TEXT("常驻背包开关"), Toggle) || !TestNotNull(TEXT("独立背包父容器"), Details)
        || !TestNotNull(TEXT("常驻金币生命"), Income)) { return true; }
    auto Click = [&]() { Toggle->OnClicked.Broadcast(); };
    auto Collapsed = [&]() { return Details->GetVisibility() == ESlateVisibility::Collapsed; };
    TestTrue(TEXT("背包默认Collapsed"), Collapsed());
    TestTrue(TEXT("开关和金币在面板外层"), Toggle->GetParent() == Income->GetParent() && Income->GetParent() != Details);
    TestTrue(TEXT("金币生命加大"), Income->GetFont().Size >= 24);
    TestTrue(TEXT("顶部显示金币不带每秒收入"), Income->GetText().ToString().Contains(TEXT("120")) && !Income->GetText().ToString().Contains(TEXT("金币/秒")));
    TestFalse(TEXT("人物和商店默认独立关闭"), P->IsCharacterExpanded() || P->IsShopExpanded());
    TestNull(TEXT("没有开始按钮"), P->WidgetTree->FindWidget(TEXT("Advance")));
    if (Case == TEXT("Default"))
    {
        ++V.ViewRevision; V.Gold = 125; F.Present();
        TestTrue(TEXT("经济刷新不能突破父容器折叠"), Collapsed());
        TestTrue(TEXT("收起仍更新收入"), Income->GetText().ToString().Contains(TEXT("125")));
        TestEqual(TEXT("模型保留最新完整视图"), P->GetModel().View().ViewRevision, V.ViewRevision);
        UDragDropOperation* Drag = nullptr; P->GetCell(Z::Equipment, 0)->NativeOnDragDetected(FGeometry(), FPointerEvent(), Drag);
        TestNull(TEXT("空装备槽不能构造拖动"), Drag);
        auto& Item = V.Items[0]; Item.Location = EY01ItemLocation::EquipmentSlot; Item.Slot = 0;
        V.EquipmentSlots[0] = Item.ItemId; ++V.ViewRevision; F.Present();
        P->GetCell(Z::Equipment, 0)->NativeOnDragDetected(FGeometry(), FPointerEvent(), Drag);
        TestNotNull(TEXT("背包关闭仍可从常驻装备槽拖动"), Drag); return true;
    }
    Click(); TestFalse(TEXT("真实按钮展开"), Collapsed());
    auto* Bag = P->GetBackpack(); auto* First = Bag->GetItemAt(0); const auto SlateBag = Bag->TakeWidget();
    if (Case == TEXT("Retain"))
    {
        P->MaterialMode(); TestTrue(TEXT("展开可选材料"), P->ToggleMaterial(V.Items[0].ItemId));
        Bag->SetScrollOffset(1); const auto Offset = Bag->GetScrollOffset(); Click();
        TestTrue(TEXT("再次点击收起"), Collapsed());
        TestFalse(TEXT("隐藏不能变更选材"), P->ToggleMaterial(V.Items[1].ItemId));
        ++V.ViewRevision; V.Gold += 5; F.Present(); Click();
        TestFalse(TEXT("可再次展开"), Collapsed());
        TestEqual(TEXT("不重建虚拟列表"), P->GetBackpack(), Bag);
        TestTrue(TEXT("不重建Slate列表"), Bag->TakeWidget() == SlateBag);
        TestEqual(TEXT("同物品对象保留"), Bag->GetItemAt(0), First);
        TestEqual(TEXT("滚动位置保留"), Bag->GetScrollOffset(), Offset);
        TestEqual(TEXT("选材身份保留"), P->GetModel().Materials().Num(), 1);
        TestTrue(TEXT("选材模式保留"), P->IsSelectingMaterials());
        TestFalse(TEXT("显隐不发送交易"), P->GetModel().IsPending()); return true;
    }
    if (Case == TEXT("Dragging"))
    {
        auto& App = FSlateApplication::Get();
        if (!TestFalse(TEXT("夹具不覆盖既有拖放"), App.IsDragDropping())) { return false; }
        auto Window = SNew(SWindow); TArray<FWidgetAndPointer> Widgets;
        Widgets.Emplace(FArrangedWidget(Window, FGeometry::MakeRoot(FVector2D(1280,800), FSlateLayoutTransform())));
        FWidgetPath Path{MakeArrayView(Widgets)}; FPointerEvent Mouse;
        auto* Operation = NewObject<UY01R36DragOperation>(); Operation->Origin = P; Operation->Identity.Zone = Z::Backpack;
        const auto Drag = FUMGDragDropOp::New(Operation, Mouse.GetPointerIndex(), FVector2D::ZeroVector,
            FVector2D::ZeroVector, 1.f, StaticCastSharedPtr<SObjectWidget>(F.Slate));
        App.ProcessReply(Path, FReply::Handled().BeginDragDrop(Drag), &Path, &Mouse);
        ON_SCOPE_EXIT { App.CancelDragDrop(); };
        if (!TestTrue(TEXT("夹具进入实际UMG拖动"), UWidgetBlueprintLibrary::IsDragDropping())) { return false; }
        F.Present(); TestFalse(TEXT("真实Slate拖动期间禁用收起"), Toggle->GetIsEnabled()); Click();
        TestFalse(TEXT("拖动不能隐藏中央容器"), Collapsed());
        TestTrue(TEXT("收起尝试不取消拖动"), App.IsDragDropping());
        P->ToggleCharacter(); P->ToggleShop(); P->ToggleCharacter(); P->ToggleShop();
        TestFalse(TEXT("拖动期间无关面板可独立开关"), P->IsCharacterExpanded() || P->IsShopExpanded());
        App.CancelDragDrop(); F.Present(); Click();
        TestTrue(TEXT("拖动结束后可正常收起"), Collapsed()); return true;
    }
    if (Case == TEXT("Pending") || Case == TEXT("Confirmation"))
    {
        FY01ItemRequest R; R.TicketId = FGuid::NewGuid(); R.Operation = EY01ItemOperation::PrepareLearn;
        if (Case == TEXT("Pending")) { const_cast<FY01R36UIModel&>(P->GetModel()).Sent(R); }
        else { V.Tickets.Add(R); V.Response.TicketId = R.TicketId; ++V.ViewRevision; }
        F.Present(); TestFalse(TEXT("有待回执或确认时禁用收起"), Toggle->GetIsEnabled()); Click();
        TestFalse(TEXT("不能隐藏未完成交易"), Collapsed());
        TestTrue(TEXT("不把收起转为取消"), Case == TEXT("Pending") ? P->GetModel().IsPending() : P->GetModel().NeedsConfirmation());
        TestFalse(TEXT("禁用有明确说明"), Toggle->GetToolTipText().IsEmpty());
        P->ToggleCharacter(); P->ToggleShop(); P->ToggleCharacter(); P->ToggleShop();
        TestFalse(TEXT("相关交易不锁无关人物和商店"), P->IsCharacterExpanded() || P->IsShopExpanded()); return true;
    }
    auto Window = SNew(SWindow).ClientSize(FVector2D(1280,800)); auto Slate = P->TakeWidget(); Window->SetContent(Slate);
    FHittestGrid Grid; Grid.SetHittestArea(FVector2D::ZeroVector, FVector2D(1280,800));
    for (int32 I = 0; I < 3; ++I)
    {
        Slate->SlatePrepass(1.f); FSlateWindowElementList Elements(Window);
        FPaintArgs Args(&Window.Get(), Grid, FVector2D::ZeroVector, 20 + I * .016, .016f);
        Slate->Paint(Args, FGeometry::MakeRoot(FVector2D(1280,800), FSlateLayoutTransform()), FSlateRect(0,0,1280,800), Elements, 0, FWidgetStyle(), true);
    }
    TestTrue(TEXT("展开曾真实布局，具备旧缓存几何"), Bag->GetCachedGeometry().GetLocalSize().X > 0);
    Click(); V.Items[0].ArrivalDeliveryId = FGuid::NewGuid(); ++V.ViewRevision; F.Present();
    TestEqual(TEXT("隐藏不能消费到货提示"), P->GetModel().UnseenArrivals(), 1);
    TestEqual(TEXT("隐藏不能开始闪光计时"), P->GetModel().FlashRemaining(V.Items[0].ItemId, 11), 0.);
    if (Case == TEXT("HiddenGeometry"))
    {
        TStrongObjectPtr<UGameViewportClient> Client(NewObject<UGameViewportClient>(GEngine));
        auto* Context = GEngine->GetWorldContextFromWorld(F.World); Context->GameViewport = Client.Get();
        ON_SCOPE_EXIT { Context->GameViewport = nullptr; };
        TSharedPtr<FJsonObject> J; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Y01R36Trace::PanelRecord(P)), J);
        if (!TestTrue(TEXT("真实面板观测可解析"), J.IsValid())) { return true; }
        TestFalse(TEXT("隐藏背包缓存不可宣称已布局"), J->GetObjectField(TEXT("bagGeometry"))->GetBoolField(TEXT("laidOut")));
        int32 Permanent = 0;
        for (const auto& Cell : J->GetArrayField(TEXT("cells")))
        {
            const auto Row = Cell->AsObject();
            if (Row->GetNumberField(TEXT("zone")) == static_cast<int32>(Z::Backpack))
            { TestFalse(TEXT("隐藏背包格不可宣称已布局"), Row->GetBoolField(TEXT("laidOut"))); }
            else { ++Permanent; TestTrue(TEXT("十底槽不误报隐藏"), Row->GetBoolField(TEXT("visible"))); }
        }
        TestEqual(TEXT("保留十个常驻槽记录"), Permanent, 10);
        TestFalse(TEXT("隐藏商品不可宣称已布局"), J->GetObjectField(TEXT("Purchase_0"))->GetBoolField(TEXT("laidOut")));
    }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01HUD08ContractTest, "Y01.Framework.R36.UI.HUD08",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01HUD08ContractTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Vitals"), TEXT("IndependentPanels"), TEXT("PermanentSlots"), TEXT("NoLegacyUI"),
        TEXT("AttributeRows"), TEXT("Countdown"), TEXT("FinalWave"), TEXT("Ended"), TEXT("UnknownCooldown"), TEXT("ConfirmationLayer"),
        TEXT("Layout720"), TEXT("Layout1080"), TEXT("LayoutDPI"), TEXT("PurchaseLock"), TEXT("TalentLock"), TEXT("ObserverHeld"), TEXT("ObserverUnknown")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01HUD08ContractTest::RunTest(const FString& Case)
{
    FWidgetWorld F(false); auto* P = F.Panel.Get(); auto& V = F.View;
    V.Gold = 317; V.Health = 250; V.Stats.Health = 300; V.IncomeRate = 2.75;
    V.Stats.AttackDamage = 73; V.Stats.Armor = 9; V.Stats.Agility = 12; V.Stats.Wisdom = 14;
    auto* GS = F.World->SpawnActor<AY01GameState>(); F.World->SetGameState(GS);
    auto& S = *FindFProperty<FStructProperty>(GS->GetClass(), TEXT("Snapshot"))->ContainerPtrToValuePtr<FY01WaveSnapshot>(GS);
    S.bAutomaticWaves = true; S.TotalWaves = 5; S.NextWaveIndex = 2; S.NextWaveDeadline = 25;
    S.PreparationDeadline = 999; S.Phase = EY01MatchPhase::Battle; S.Revision = 1;
    F.Present();
    auto Find = [&](const TCHAR* Name) { return P->WidgetTree->FindWidget(Name); };
    auto Text = [&](const TCHAR* Name) { auto* W = Cast<UTextBlock>(Find(Name)); return W ? W->GetText().ToString() : FString(); };
    auto Click = [&](const TCHAR* Name) { auto* B = Cast<UButton>(Find(Name)); if (TestNotNull(TEXT("独立开关存在"), B)) { B->OnClicked.Broadcast(); } };
    auto Visible = [&](UWidget* W)
    {
        if (!W) { return false; }
        for (; W; W = W->GetParent()) { if (!W->IsVisible()) { return false; } }
        return true;
    };
    if (Case.StartsWith(TEXT("Observer")))
    {
        F.World->InitializeActorsForPlay(FURL());
        auto* PC = F.World->SpawnActor<AY01R36Controller>();
        auto* Local = NewObject<ULocalPlayer>(GEngine); Local->PlayerController = PC; PC->Player = Local;
        PC->SetRole(ROLE_AutonomousProxy); P->SetOwningPlayer(PC);
        ON_SCOPE_EXIT { P->SetOwningPlayer(nullptr); PC->SetRole(ROLE_Authority); };
        auto& O = *FindFProperty<FStructProperty>(PC->GetClass(), TEXT("Observer"))->ContainerPtrToValuePtr<FY01PlaygroundObserver>(PC);
        O.bReady = true; O.Waves = S; O.ServerSampleTime = Case == TEXT("ObserverUnknown") ? -1 : 21;
        auto& Skill = V.Skills[0]; Skill.LearnedId = FGuid::NewGuid(); Skill.Display.SkillType = EY01SkillType::Active; Skill.Deadline = 30;
        ++V.ViewRevision; P->Present(V, S.Phase, 9000);
        F.World->TimeSeconds = 9999; S.NextWaveDeadline = 20000;
        P->Present(V, S.Phase, 10000);
        if (Case == TEXT("ObserverHeld"))
        {
            TestEqual(TEXT("延迟客户端保持同源样本，不使用本地或异源GS"), Text(TEXT("WaveCountdown")), FString(TEXT("第 3 波还有 4 秒")));
            TestEqual(TEXT("CD同样保持服务器样本"), P->GetServerTime(), 21.);
            TestEqual(TEXT("Trace时钟明确保持"), P->GetWaveClockSource(), FName(TEXT("ObserverHeld")));
            TestTrue(TEXT("技能显示同源CD"), Texts(P->GetCell(Z::Skill, 0)).Contains(TEXT("9.0")));
            O.ServerSampleTime = 24.2; P->Present(V, S.Phase, 10000);
            TestEqual(TEXT("只在收到新服务器样本后更新"), Text(TEXT("WaveCountdown")), FString(TEXT("第 3 波还有 1 秒")));
        }
        else
        {
            TestEqual(TEXT("无可靠样本时保持未知"), P->GetWaveServerTime(), -1.);
            TestTrue(TEXT("未知技能不伪报Ready"), Texts(P->GetCell(Z::Skill, 0)).Contains(TEXT("同步")));
        }
        return true;
    }
    if (Case == TEXT("PurchaseLock") || Case == TEXT("TalentLock"))
    {
        P->ToggleCharacter(); P->ToggleBackpack(); P->ToggleShop();
        FY01ItemRequest R; R.Operation = Case == TEXT("PurchaseLock") ? EY01ItemOperation::Purchase : EY01ItemOperation::ChooseTalent;
        const_cast<FY01R36UIModel&>(P->GetModel()).Sent(R); F.Present();
        P->ToggleCharacter(); P->ToggleBackpack(); P->ToggleShop();
        TestFalse(TEXT("无关背包可关闭"), P->IsBackpackExpanded());
        TestEqual(TEXT("只有人物相关请求锁人物"), P->IsCharacterExpanded(), Case == TEXT("TalentLock"));
        TestEqual(TEXT("只有商店相关请求锁商店"), P->IsShopExpanded(), Case == TEXT("PurchaseLock"));
        TestTrue(TEXT("显隐不能取消pending"), P->GetModel().IsPending()); return true;
    }
    if (Case.StartsWith(TEXT("Layout")))
    {
        P->ToggleCharacter(); P->ToggleBackpack(); P->ToggleShop();
        const FVector2D Pixels = Case == TEXT("Layout1080") ? FVector2D(1920,1080) : FVector2D(1280,720);
        const float DPI = Case == TEXT("LayoutDPI") ? 1.5f : 1.f;
        const auto Host = FGeometry::MakeRoot(Pixels / DPI, FSlateLayoutTransform(DPI));
        auto Window = SNew(SWindow).ClientSize(Pixels); auto Slate = P->TakeWidget(); Window->SetContent(Slate);
        FHittestGrid Grid; Grid.SetHittestArea(FVector2D::ZeroVector, Pixels);
        for (int32 I = 0; I < 4; ++I)
        {
            Slate->SlatePrepass(DPI); FSlateWindowElementList Elements(Window);
            FPaintArgs Args(&Window.Get(), Grid, FVector2D::ZeroVector, 20 + I * .016, .016f);
            Slate->Paint(Args, Host, FSlateRect(0,0,Pixels.X,Pixels.Y), Elements, 0, FWidgetStyle(), true);
        }
        auto Rect = [](UWidget* W) { return W->GetCachedGeometry().GetLayoutBoundingRect(); };
        auto Fits = [&](UWidget* W)
        {
            const auto R = Rect(W);
            TestTrue(TEXT("真实Slate控件在窗口内且非零尺寸"), R.Right > R.Left && R.Bottom > R.Top && R.Left >= -.1f
                && R.Top >= -.1f && R.Right <= Pixels.X + .1f && R.Bottom <= Pixels.Y + .1f);
            return R;
        };
        const TCHAR* Parents[] = {TEXT("CharacterPanel"), TEXT("BackpackPanel"), TEXT("ShopPanel")};
        for (const auto* Name : Parents) { Fits(Find(Name)); }
        for (const auto Zone : {Z::Equipment, Z::Skill})
        {
            for (int32 I = 0; I < (Zone == Z::Equipment ? 6 : 4); ++I)
            {
                const auto R = Fits(P->GetCell(Zone, I));
                for (const auto* Name : Parents) { TestTrue(TEXT("三面板同时展开不遮底槽"), Rect(Find(Name)).Bottom <= R.Top); }
                TestTrue(TEXT("底槽采用减半后的实际布局高度"), FMath::IsNearlyEqual(R.Bottom - R.Top, 52.f * static_cast<float>(Pixels.X / 1280.), .1f));
            }
        }
        float Previous = -1;
        for (const auto* Name : {TEXT("ToggleCharacter"), TEXT("ToggleBackpack"), TEXT("ToggleShop")})
        {
            const auto R = Fits(Find(Name)); TestTrue(TEXT("三个开关竖排错开"), R.Top > Previous); Previous = R.Bottom;
            for (const auto* Parent : Parents) { TestTrue(TEXT("开关与面板分列"), R.Left > Rect(Find(Parent)).Right); }
        }
        Fits(Find(TEXT("TopVitals"))); Fits(Find(TEXT("WaveCountdown"))); return true;
    }
    if (Case == TEXT("Vitals"))
    {
        const auto T = Text(TEXT("TopVitals"));
        TestTrue(TEXT("左上真实金币与当前/最大生命"), T.Contains(TEXT("317")) && T.Contains(TEXT("250/300")));
        TestFalse(TEXT("左上不显示每秒或其它属性"), T.Contains(TEXT("金币/秒")) || T.Contains(TEXT("攻击")));
        V.Health = 181; V.Stats.Health = 420; ++V.ViewRevision; F.Present();
        TestTrue(TEXT("生命比值随完整视图变化，不写死"), Text(TEXT("TopVitals")).Contains(TEXT("181/420"))); return true;
    }
    if (Case == TEXT("IndependentPanels"))
    {
        const TCHAR* Parents[] = {TEXT("CharacterPanel"), TEXT("BackpackPanel"), TEXT("ShopPanel")};
        for (const auto* Name : Parents) { auto* W = Find(Name); if (TestNotNull(TEXT("三个独立父容器"), W)) { TestFalse(TEXT("默认收起"), Visible(W)); } }
        Click(TEXT("ToggleCharacter")); TestTrue(TEXT("人物可单独打开"), Visible(Find(Parents[0])));
        TestFalse(TEXT("人物不打开背包"), Visible(Find(Parents[1])));
        Click(TEXT("ToggleBackpack")); Click(TEXT("ToggleShop"));
        for (const auto* Name : Parents) { TestTrue(TEXT("三面板可同时打开"), Visible(Find(Name))); }
        Click(TEXT("ToggleBackpack")); ++V.ViewRevision; V.ShopOffers.AddDefaulted(); F.Present();
        TestFalse(TEXT("经济刷新不突破背包父容器"), Visible(Find(Parents[1])));
        TestTrue(TEXT("无关人物商店保持展开"), Visible(Find(Parents[0])) && Visible(Find(Parents[2]))); return true;
    }
    if (Case == TEXT("PermanentSlots"))
    {
        const FString LongName = TEXT("超长测试名称\n完整信息不能因紧凑布局丢失");
        auto& Item = V.Items[0]; Item.Location = EY01ItemLocation::EquipmentSlot; Item.Slot = 0;
        Item.Display.Name = FText::FromString(LongName); V.EquipmentSlots[0] = Item.ItemId;
        auto& Skill = V.Skills[0]; Skill.LearnedId = FGuid::NewGuid(); Skill.Display.Name = Item.Display.Name;
        Skill.Display.SkillType = EY01SkillType::Active; Skill.Deadline = 18; ++V.ViewRevision; F.Present();
        auto CheckSlots = [&]()
        {
            int32 Count = 0;
            for (const auto Zone : {Z::Equipment, Z::Skill})
            {
                const bool Equipment = Zone == Z::Equipment;
                const int32 Slots = Equipment ? 6 : 4;
                for (int32 I = 0; I < Slots; ++I)
                {
                    auto* Cell = P->GetCell(Zone, I);
                    if (!TestNotNull(TEXT("十个常驻槽必须存在"), Cell)) { continue; }
                    ++Count;
                    auto* Slot = Cast<UCanvasPanelSlot>(Cell->Slot);
                    if (!TestNotNull(TEXT("底槽使用实际Canvas布局而非渲染缩放"), Slot)) { continue; }
                    TestEqual(TEXT("十槽同一Y并按原分组单排排列"), Slot->GetPosition(), FVector2D(Equipment ? 16. + I * 108 : 668. + I * 148, 650));
                    TestEqual(TEXT("槽宽高均为原来一半，底边距18"), Slot->GetSize(), FVector2D(Equipment ? 104 : 144, 52));
                    TestEqual(TEXT("不以RenderTransform伪缩小"), Cell->GetRenderTransform().Scale, FVector2D(1,1));
                    TestTrue(TEXT("常驻槽在三面板之外"), Cell->GetParent() == Find(TEXT("HUDCanvas")));
                    TestTrue(TEXT("面板显隐不影响底槽交互"), Visible(Cell) && Cell->GetIsEnabled() && P->CanUseZone(Zone));
                    TestTrue(TEXT("底槽Slate仍允许子控件命中"), Cell->TakeWidget()->GetVisibility().AreChildrenHitTestVisible());
                    auto* Label = Cast<UTextBlock>(Cell->WidgetTree->FindWidget(TEXT("Label")));
                    if (!TestNotNull(TEXT("复用原标签"), Label)) { continue; }
                    TArray<FString> Lines; Label->GetText().ToString().ParseIntoArrayLines(Lines);
                    TestEqual(TEXT("空槽及有内容槽均压为两行"), Lines.Num(), 2);
                    TestTrue(TEXT("保持可读字号，不整体缩到7px"), Label->GetFont().Size >= 13);
                    TestEqual(TEXT("长名称由实际可用宽度省略"), Label->GetTextOverflowPolicy(), ETextOverflowPolicy::Ellipsis);
                }
                TestNull(TEXT("不增加额外槽位"), P->GetCell(Zone, Slots));
            }
            TestEqual(TEXT("总共十槽"), Count, 10);
        };
        CheckSlots(); P->ToggleCharacter(); P->ToggleBackpack(); P->ToggleShop(); CheckSlots();
        P->ToggleCharacter(); P->ToggleBackpack(); P->ToggleShop(); CheckSlots();
        auto* Equipment = P->GetCell(Z::Equipment, 0); auto* Active = P->GetCell(Z::Skill, 0);
        if (!Equipment || !Active) { return false; }
        TestTrue(TEXT("装备Tooltip保留未截断原文和品质"), Equipment->GetToolTipText().ToString().Contains(P->GetModel().ItemLabel(Item.ItemId)));
        TestTrue(TEXT("技能Tooltip保留完整原文、类型品质和CD"), Active->GetToolTipText().ToString().Contains(P->GetModel().SkillLabel(0, 11)));
        TestTrue(TEXT("主动CD在槽内独占第二行"), Texts(Active).EndsWith(TEXT("\nCD 7.0 秒")));
        P->Present(V, S.Phase, 17); TestTrue(TEXT("CD跟随同一权威时间而非本地起点"), Texts(Active).EndsWith(TEXT("\nCD 1.0 秒")));
        P->Present(V, S.Phase, -1); TestTrue(TEXT("未知时间仍在槽内显示待同步"), Texts(Active).EndsWith(TEXT("\nCD 待同步")));
        P->Present(V, S.Phase, 18); TestTrue(TEXT("到期仍在槽内显示就绪"), Texts(Active).EndsWith(TEXT("\n就绪")));
        const FPointerEvent Mouse(0, FVector2D(20), FVector2D(20), TSet<FKey>{EKeys::LeftMouseButton}, EKeys::LeftMouseButton, 0, FModifierKeysState());
        TestTrue(TEXT("紧凑装备仍处理原生拖动按下"), Equipment->NativeOnMouseButtonDown(FGeometry(), Mouse).IsEventHandled());
        UDragDropOperation* Drag = nullptr; Equipment->NativeOnDragDetected(FGeometry(), Mouse, Drag);
        TestNotNull(TEXT("背包关闭仍可从紧凑装备拖动"), Drag);
        TStrongObjectPtr<UY01R36Cell> Reused(CreateWidget<UY01R36Cell>(F.World));
        Reused->Configure(P, Z::Equipment, 0); Reused->Configure(P, Z::Backpack, INDEX_NONE, V.Items[1].ItemId);
        auto* Label = Cast<UTextBlock>(Reused->WidgetTree->FindWidget(TEXT("Label")));
        auto* Frame = Cast<UBorder>(Reused->WidgetTree->RootWidget);
        auto* Row = Frame ? Cast<UHorizontalBox>(Frame->GetContent()) : nullptr;
        auto* IconBox = Row ? Cast<USizeBox>(Row->GetChildAt(0)) : nullptr;
        if (!TestTrue(TEXT("共享Cell保留原背包组件结构"), Label && Frame && IconBox)) { return false; }
        TestTrue(TEXT("背包字号与边距不受紧凑样式污染"), Label->GetFont().Size == 15 && Frame->GetPadding() == FMargin(6));
        TestTrue(TEXT("背包图标仍为24方形"), IconBox->GetWidthOverride() == 24 && IconBox->GetHeightOverride() == 24);
        TestEqual(TEXT("背包不沿用省略策略"), Label->GetTextOverflowPolicy(), ETextOverflowPolicy::Clip);
        TestEqual(TEXT("背包保留原完整文案"), Texts(Reused.Get()), P->GetModel().ItemLabel(V.Items[1].ItemId));
        TestTrue(TEXT("常驻区域不受背包门禁"), P->CanDrag()); return true;
    }
    if (Case == TEXT("NoLegacyUI"))
    {
        for (const auto* Name : {TEXT("TogglePanels"), TEXT("CentralPanels"), TEXT("Advance"), TEXT("Title"), TEXT("Summary"), TEXT("WaveObservation")})
        { TestNull(TEXT("旧总开关/标题摘要/手动波次控件已移除"), Find(Name)); }
        const auto T = Texts(P);
        TestFalse(TEXT("正常HUD没有旧说明"), T.Contains(TEXT("Enter")) || T.Contains(TEXT("波次观测")) || T.Contains(TEXT("程序占位界面"))); return true;
    }
    if (Case == TEXT("AttributeRows"))
    {
        Click(TEXT("ToggleCharacter"));
        for (const auto* Key : {TEXT("Health"), TEXT("MaxHealth"), TEXT("AttackDamage"), TEXT("AttackInterval"), TEXT("AttackType"),
            TEXT("Range"), TEXT("MeleeAngle"), TEXT("ProjectileSpeed"), TEXT("Armor"), TEXT("MoveSpeed"), TEXT("HealthRegen"),
            TEXT("Strength"), TEXT("Agility"), TEXT("Wisdom"), TEXT("Experience"), TEXT("IncomeRate")})
        {
            auto* Row = Cast<UBorder>(Find(*FString::Printf(TEXT("StatRow_%s"), Key)));
            TestNotNull(TEXT("每项属性独立背景横条"), Row);
            TestTrue(TEXT("每项有独立高对比标签与值"), Find(*FString::Printf(TEXT("StatLabel_%s"), Key)) && Find(*FString::Printf(TEXT("StatValue_%s"), Key)));
        }
        TestEqual(TEXT("每秒收入归人物面板"), Text(TEXT("StatValue_IncomeRate")), FString(TEXT("2.75"))); return true;
    }
    if (Case == TEXT("Countdown"))
    {
        TestEqual(TEXT("下一波0基转1基且不使用阶段deadline"), Text(TEXT("WaveCountdown")), FString(TEXT("第 3 波还有 14 秒")));
        P->Present(V, S.Phase, 24.2); TestEqual(TEXT("同源服务器时钟向上取整"), Text(TEXT("WaveCountdown")), FString(TEXT("第 3 波还有 1 秒"))); return true;
    }
    if (Case == TEXT("FinalWave"))
    {
        S.NextWaveIndex = INDEX_NONE; S.WaveIndex = 4; S.TotalAliveMonsters = 7; S.TotalPendingSpawns = 2; F.Present();
        TestTrue(TEXT("末波开始后展示全局存活与待生成"), Text(TEXT("WaveCountdown")).Contains(TEXT("剩余 7 只")) && Text(TEXT("WaveCountdown")).Contains(TEXT("待生成 2"))); return true;
    }
    if (Case == TEXT("Ended"))
    {
        S.Phase = EY01MatchPhase::Ended; S.Result = EY01MatchResult::Failure; F.Present();
        TestTrue(TEXT("结束结果优先于遗留deadline"), Text(TEXT("WaveCountdown")).Contains(TEXT("失败")));
        TestFalse(TEXT("结束后不显示下一波"), Text(TEXT("WaveCountdown")).Contains(TEXT("还有"))); return true;
    }
    if (Case == TEXT("UnknownCooldown"))
    {
        auto& Skill = V.Skills[0]; Skill.LearnedId = FGuid::NewGuid(); Skill.Display.SkillType = EY01SkillType::Active; Skill.Deadline = 0;
        ++V.ViewRevision; P->Present(V, S.Phase, -1);
        const auto T = Texts(P->GetCell(Z::Skill, 0));
        TestFalse(TEXT("未知时间不能假就绪"), T.Contains(TEXT("就绪")));
        TestTrue(TEXT("未知时间明确待同步"), T.Contains(TEXT("同步"))); return true;
    }
    FY01ItemRequest Ticket; Ticket.TicketId = FGuid::NewGuid(); V.Tickets.Add(Ticket); V.Response.TicketId = Ticket.TicketId;
    ++V.ViewRevision; F.Present();
    TestTrue(TEXT("三面板收起也不能藏掉权威覆盖确认"), Visible(Find(TEXT("Confirmation")))); return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01HUD08FixTest, "Y01.Framework.R36.UI.HUD08Fix",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01HUD08FixTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("ManualWave"), TEXT("FocusedToggleConsumesEnter"), TEXT("Text720"), TEXT("Text1080"), TEXT("TextDPI")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01HUD08FixTest::RunTest(const FString& Case)
{
    if (Case == TEXT("ManualWave"))
    {
        FY01WaveSnapshot S; S.Phase = EY01MatchPhase::Battle; S.WaveIndex = 2; S.PendingSpawns = 7;
        TestEqual(TEXT("手动波次只报告实际维护的待生成字段"), FY01R36UIModel::WaveLabel(S, 10), FString(TEXT("第 3 波 · 待生成 7 只")));
        S.TotalAliveMonsters = 999;
        TestEqual(TEXT("手动文案不读取自动计数"), FY01R36UIModel::WaveLabel(S, 10), FString(TEXT("第 3 波 · 待生成 7 只")));
        S.PendingSpawns = 0;
        TestEqual(TEXT("待生成归零不误称无存活怪"), FY01R36UIModel::WaveLabel(S, 10), FString(TEXT("第 3 波 · 待生成 0 只")));
        return true;
    }
    FWidgetWorld F; F.View.SynthesisRecipes.AddDefaulted(); F.Present(); auto* P = F.Panel.Get();
    if (Case == TEXT("FocusedToggleConsumesEnter"))
    {
        auto* B = Cast<UButton>(P->WidgetTree->FindWidget(TEXT("ToggleShop")));
        const auto SlateButton = StaticCastSharedRef<SButton>(B->TakeWidget());
        TestTrue(TEXT("保留普通开关的键盘可访问性"), SlateButton->SupportsKeyboardFocus());
        const FKeyEvent Enter(EKeys::Enter, FModifierKeysState(), 0, false, 13, 13);
        TestTrue(TEXT("最小真实Slate重现：按钮处理Enter按下"), SlateButton->OnKeyDown(FGeometry(), Enter).IsEventHandled());
        TestTrue(TEXT("最小真实Slate重现：按钮处理Enter释放"), SlateButton->OnKeyUp(FGeometry(), Enter).IsEventHandled());
        TestFalse(TEXT("旧输入路径的Enter实际关闭商店而非交给游戏"), P->IsShopExpanded());
        TestEqual(TEXT("恢复视口的目标文本不拦截原生点击"), P->WidgetTree->FindWidget(TEXT("TopVitals"))->GetVisibility(), ESlateVisibility::HitTestInvisible);
        return true;
    }
    const FVector2D Pixels = Case == TEXT("Text1080") ? FVector2D(1920,1080) : FVector2D(1280,720);
    const float DPI = Case == TEXT("TextDPI") ? 1.5f : 1.f;
    const auto Host = FGeometry::MakeRoot(Pixels / DPI, FSlateLayoutTransform(DPI));
    auto Window = SNew(SWindow).ClientSize(Pixels); auto Slate = P->TakeWidget(); Window->SetContent(Slate);
    FHittestGrid Grid; Grid.SetHittestArea(FVector2D::ZeroVector, Pixels);
    const auto Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
    for (int32 Variant = 0; Variant < 2; ++Variant)
    {
        if (Variant) { P->ToggleFlights(); P->MaterialMode(); }
        for (int32 I = 0; I < 12; ++I)
        {
            Slate->SlatePrepass(DPI); FSlateWindowElementList Elements(Window);
            FPaintArgs Args(&Window.Get(), Grid, FVector2D::ZeroVector, 20 + I * .016, .016f);
            Slate->Paint(Args, Host, FSlateRect(0,0,Pixels.X,Pixels.Y), Elements, 0, FWidgetStyle(), true);
        }
        for (const auto* Name : {TEXT("ToggleCharacter"), TEXT("ToggleBackpack"), TEXT("ToggleShop"), TEXT("ToggleFlights"), TEXT("Training"), TEXT("MaterialMode"), TEXT("Synthesize")})
        {
            auto* B = Cast<UButton>(P->WidgetTree->FindWidget(Name));
            auto* T = B ? Cast<UTextBlock>(B->GetContent()) : nullptr;
            if (!TestNotNull(TEXT("保留按钮直接TextBlock及动态更新"), T)) { continue; }
            const FVector2D Natural = Measure->Measure(T->GetText(), T->GetFont());
            const auto Allocated = T->GetCachedGeometry().GetLocalSize();
            const auto BR = B->GetCachedGeometry().GetLayoutBoundingRect();
            const auto TR = T->GetCachedGeometry().GetLayoutBoundingRect();
            AddInfo(FString::Printf(TEXT("HUD08_TEXT %s %s variant=%d font=%.1f natural=(%.2f,%.2f) allocated=(%.2f,%.2f) button=(%.2f,%.2f,%.2f,%.2f) text=(%.2f,%.2f,%.2f,%.2f)"),
                *Case, Name, Variant, T->GetFont().Size, Natural.X, Natural.Y, Allocated.X, Allocated.Y,
                BR.Left, BR.Top, BR.Right, BR.Bottom, TR.Left, TR.Top, TR.Right, TR.Bottom));
            TestTrue(FString::Printf(TEXT("%s完整单行文本宽度必须有实际分配空间"), Name), Natural.X <= Allocated.X + .5f);
            TestTrue(FString::Printf(TEXT("%s文字不得溢出按钮上下左右"), Name), TR.Left >= BR.Left && TR.Top >= BR.Top && TR.Right <= BR.Right && TR.Bottom <= BR.Bottom);
        }
    }
    return true;
}
#if WITH_EDITOR
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01HUD08OffscreenTest, "Y01.Graphics.HUD08.OffscreenFixture",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FY01HUD08OffscreenTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("1280x720"), TEXT("1920x1080")}) { Names.Add(N); Commands.Add(N); }
}
bool FY01HUD08OffscreenTest::RunTest(const FString& Case)
{
    // 专用显式离屏夹具；普通自动化不渲染，也不能把跳过报告为视觉通过。
    if (!FParse::Param(FCommandLine::Get(), TEXT("Y01HUD08Offscreen")))
    { AddInfo(TEXT("HUD08_OFFSCREEN_SKIP: 未显式启用；不创建渲染目标，不提供视觉证据。")); return true; }
    if (!FApp::IsUnattended() || !FSlateApplication::IsInitialized()
        || !FSlateApplication::Get().IsRenderingOffScreen() || !FApp::CanEverRender()
        || !GDynamicRHI || FString(GDynamicRHI->GetName()) != TEXT("D3D12"))
    { AddError(TEXT("离屏保护拒绝：必须unattended、RenderOffscreen及真实D3D12。")); return false; }
    FString Output, ProfilePath;
    FParse::Value(FCommandLine::Get(), TEXT("Y01HUD08Output="), Output);
    FParse::Value(FCommandLine::Get(), TEXT("Y01HUD08Profile="), ProfilePath);
    Output = FPaths::ConvertRelativePathToFull(Output); FPaths::NormalizeDirectoryName(Output);
    const FString Allowed = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()) + TEXT("HUD08_Offscreen_");
    if (!Output.StartsWith(Allowed) || ProfilePath.IsEmpty())
    { AddError(TEXT("必须提供Saved/HUD08_Offscreen_唯一目录及只读Profile路径。")); return false; }
    IFileManager::Get().MakeDirectory(*Output, true);
    TStrongObjectPtr<UY01R36Profile> Profile(LoadObject<UY01R36Profile>(nullptr, *ProfilePath));
    if (!TestNotNull(TEXT("只读加载实际Profile，不生成或保存资产"), Profile.Get())) { return false; }
    FWidgetWorld F(false); auto* P = F.Panel.Get(); auto& V = F.View;
    // 复用完整视图夹具；商店/配方/天赋名称只读来自Profile，数值与CD是受控样本，不是游戏会话。
    V.Gold = 317; V.Health = 250; V.Stats.Health = 300; V.IncomeRate = 2.75;
    V.Stats.AttackDamage = 73; V.Stats.Armor = 9; V.Stats.Agility = 12; V.Stats.Wisdom = 14;
    V.Items[0].Location = EY01ItemLocation::EquipmentSlot; V.Items[0].Slot = 0; V.EquipmentSlots[0] = V.Items[0].ItemId;
    V.Skills[0].LearnedId = FGuid::NewGuid(); V.Skills[0].Display.Name = FText::FromString(TEXT("离屏测试主动技能"));
    V.Skills[0].Display.SkillType = EY01SkillType::Active; V.Skills[0].Deadline = 14;
    FY01DataValidation Validation; const auto Catalog = Profile->BuildCatalog();
    for (const auto& Ref : Profile->ContentShopOffers)
    {
        const auto* Row = Catalog.FindRow<FY01ShopOfferRow>(Ref, Validation);
        if (!TestNotNull(TEXT("Profile商品引用可解析"), Row)) { return false; }
        auto& Offer = V.ShopOffers.AddDefaulted_GetRef(); Offer.OfferId = Ref.RowName;
        Offer.DisplayName = Row->DisplayName; Offer.Price = Row->Price; Offer.ShopKind = Row->ShopKind; Offer.Quantity = Row->Quantity;
    }
    for (const auto& Ref : Profile->ContentRecipes)
    {
        const auto* Row = Catalog.FindRow<FY01SynthesisRow>(Ref, Validation);
        if (!TestNotNull(TEXT("Profile配方引用可解析"), Row)) { return false; }
        auto& Recipe = V.SynthesisRecipes.AddDefaulted_GetRef(); Recipe.RecipeId = Ref.RowName; Recipe.Recipe = *Row;
    }
    if (const auto* Choice = Catalog.FindRow<FY01TalentChoiceRow>(Profile->TalentChoice, Validation))
    {
        V.TalentChoice.ChoiceId = FGuid::NewGuid(); V.TalentChoice.Definition = Profile->TalentChoice;
        for (const auto& Ref : Choice->Candidates)
        {
            const auto* Row = Catalog.FindRow<FY01TalentRow>(Ref, Validation);
            if (!TestNotNull(TEXT("Profile天赋引用可解析"), Row)) { return false; }
            auto& Talent = V.TalentChoice.Candidates.AddDefaulted_GetRef(); Talent.TalentId = Ref.RowName;
            Talent.DisplayName = Row->DisplayName; Talent.Quality = Row->Quality;
        }
    }
    if (!TestTrue(TEXT("真实配置包含商店与配方"), !V.ShopOffers.IsEmpty() && !V.SynthesisRecipes.IsEmpty())) { return false; }
    auto* GS = F.World->SpawnActor<AY01GameState>(); F.World->SetGameState(GS);
    auto& S = *FindFProperty<FStructProperty>(GS->GetClass(), TEXT("Snapshot"))->ContainerPtrToValuePtr<FY01WaveSnapshot>(GS);
    S.Phase = EY01MatchPhase::Battle; S.WaveIndex = 2; S.PendingSpawns = 7; S.bAutomaticWaves = false;
    F.Present();
    const FVector2D Pixels = Case == TEXT("1920x1080") ? FVector2D(1920,1080) : FVector2D(1280,720);
    FWidgetRenderer Renderer(true);
    TStrongObjectPtr<UTextureRenderTarget2D> Target(FWidgetRenderer::CreateTargetFor(Pixels, TF_Bilinear, true));
    if (!TestNotNull(TEXT("真实D3D12离屏RT"), Target.Get())) { return false; }
    // Slate shader已执行显示gamma；RT不得再使用sRGB写入转换，读回字节直接导出PNG。
    Target->bForceLinearGamma = true;
    Target->ClearColor = FLinearColor(.008f,.012f,.02f,1.f);
    Target->UpdateResource(); Target->UpdateResourceImmediate(true);
    TestEqual(TEXT("离屏字节RT格式"), Target->GetFormat(), PF_B8G8R8A8);
    TestFalse(TEXT("RT不重复执行sRGB转换"), Target->IsSRGB());
    AddInfo(FString::Printf(TEXT("HUD08_OFFSCREEN_RT format=%d srgb=%d widgetGammaCorrection=1"), int32(Target->GetFormat()), Target->IsSRGB()));
    auto Window = SNew(SVirtualWindow).Size(Pixels); Window->SetContent(F.Slate.ToSharedRef()); FHittestGrid Grid;
    if (!TestTrue(TEXT("虚拟窗口无操作系统窗口句柄"), !Window->GetNativeWindow().IsValid() || Window->GetNativeWindow()->GetOSWindowHandle() == nullptr)
        || !TestFalse(TEXT("虚拟窗口不支持键盘焦点"), Window->SupportsKeyboardFocus())) { return false; }
    ON_SCOPE_EXIT { FlushRenderingCommands(); };
    const auto Measure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();
    auto Find = [&](const TCHAR* Name) { return P->WidgetTree->FindWidget(Name); };
    auto Rect = [](UWidget* W) { return W->GetCachedGeometry().GetLayoutBoundingRect(); };
    auto Contains = [](const FSlateRect& Outer, const FSlateRect& Inner)
    { return Inner.Right > Inner.Left && Inner.Bottom > Inner.Top && Inner.Left >= Outer.Left - .1f
        && Inner.Top >= Outer.Top - .1f && Inner.Right <= Outer.Right + .1f && Inner.Bottom <= Outer.Bottom + .1f; };
    auto RectJson = [](const FSlateRect& R)
    { return TArray<TSharedPtr<FJsonValue>>{MakeShared<FJsonValueNumber>(R.Left), MakeShared<FJsonValueNumber>(R.Top),
        MakeShared<FJsonValueNumber>(R.Right), MakeShared<FJsonValueNumber>(R.Bottom)}; };
    int32 Longest = 0; float LongestWidth = -1;
    for (int32 I = 0; I < V.ShopOffers.Num(); ++I)
    {
        auto* B = CastChecked<UButton>(Find(*FString::Printf(TEXT("Purchase_%d"), I)));
        auto* T = CastChecked<UTextBlock>(B->GetContent());
        const float Width = Measure->Measure(V.ShopOffers[I].DisplayName, T->GetFont()).X;
        if (Width > LongestWidth) { Longest = I; LongestWidth = Width; }
    }
    const auto LongName = FString::Printf(TEXT("Purchase_%d"), Longest);
    for (int32 Variant = 0; Variant < 3; ++Variant)
    {
        const FString State = Variant == 0 ? TEXT("default") : Variant == 1 ? TEXT("three-open") : TEXT("details-gold-long-shop");
        if (Variant == 1) { P->ToggleCharacter(); P->ToggleBackpack(); P->ToggleShop(); }
        if (Variant == 2)
        {
            P->ToggleFlights(); P->MaterialMode();
            CastChecked<UScrollBox>(Find(TEXT("AttributeList")))->ScrollToEnd();
            CastChecked<UScrollBox>(Find(TEXT("ShopList")))->ScrollWidgetIntoView(Find(*LongName), false, EDescendantScrollDestination::Center);
            S.bAutomaticWaves = true; S.TotalWaves = 5; S.NextWaveIndex = 3; S.NextWaveDeadline = 25;
            P->Present(V, S.Phase, 11.6);
        }
        for (int32 Frame = 0; Frame < 12; ++Frame)
        { Renderer.DrawWindow(Target.Get(), Grid, Window, 1.f, Pixels, .016f); FlushRenderingCommands(); }
        TestTrue(TEXT("绘制后仍无操作系统窗口句柄"), !Window->GetNativeWindow().IsValid() || Window->GetNativeWindow()->GetOSWindowHandle() == nullptr);
        TArray<TSharedPtr<FJsonValue>> Metrics;
        auto CheckText = [&](const FString& Name, UTextBlock* T, UWidget* Container)
        {
            if (!TestNotNull(TEXT("真实文本控件"), T) || !TestNotNull(TEXT("真实文本容器"), Container)) { return; }
            const float Scale = T->GetCachedGeometry().GetAccumulatedLayoutTransform().GetScale();
            const FVector2D Natural = Measure->Measure(T->GetText(), T->GetFont(), Scale);
            const FVector2D Desired = T->TakeWidget()->GetDesiredSize() * Scale;
            const FVector2D Allocated = T->GetCachedGeometry().GetLocalSize() * Scale;
            const auto TR = Rect(T), CR = Rect(Container);
            const bool Fits = Desired.X <= Allocated.X + .5 && Desired.Y <= Allocated.Y + .5
                && Contains(CR, TR) && Contains(FSlateRect(0,0,Pixels.X,Pixels.Y), TR);
            auto M = MakeShared<FJsonObject>(); M->SetStringField(TEXT("name"), Name); M->SetStringField(TEXT("text"), T->GetText().ToString());
            M->SetNumberField(TEXT("fontSize"), T->GetFont().Size); M->SetNumberField(TEXT("fontScale"), Scale);
            M->SetNumberField(TEXT("naturalWidthPx"), Natural.X); M->SetNumberField(TEXT("naturalHeightPx"), Natural.Y);
            M->SetNumberField(TEXT("desiredWidthPx"), Desired.X); M->SetNumberField(TEXT("desiredHeightPx"), Desired.Y);
            M->SetNumberField(TEXT("allocatedWidthPx"), Allocated.X); M->SetNumberField(TEXT("allocatedHeightPx"), Allocated.Y);
            M->SetArrayField(TEXT("textRectPx"), RectJson(TR)); M->SetArrayField(TEXT("containerRectPx"), RectJson(CR));
            M->SetBoolField(TEXT("fullyContained"), Fits); Metrics.Add(MakeShared<FJsonValueObject>(M));
            AddInfo(FString::Printf(TEXT("HUD08_OFFSCREEN_TEXT %s %s %s font=%g scale=%g desired=(%.2f,%.2f) allocated=(%.2f,%.2f) contained=%d"),
                *Case, *State, *Name, T->GetFont().Size, Scale, Desired.X, Desired.Y, Allocated.X, Allocated.Y, Fits));
            TestTrue(Name + TEXT("同尺寸字体期望几何完全包含"), Fits);
        };
        for (const auto* Name : {TEXT("ToggleCharacter"), TEXT("ToggleBackpack"), TEXT("ToggleShop"), TEXT("ToggleFlights"), TEXT("Training")})
        { auto* B = CastChecked<UButton>(Find(Name)); CheckText(Name, Cast<UTextBlock>(B->GetContent()), B); }
        CheckText(TEXT("TopVitals"), Cast<UTextBlock>(Find(TEXT("TopVitals"))), Find(TEXT("TopVitals")));
        CheckText(TEXT("WaveCountdown"), Cast<UTextBlock>(Find(TEXT("WaveCountdown"))), Find(TEXT("WaveCountdown")));
        for (const auto Zone : {Z::Equipment, Z::Skill})
        { for (int32 I = 0; I < (Zone == Z::Equipment ? 6 : 4); ++I)
            { auto* C = P->GetCell(Zone,I); CheckText(C->GetName(), Cast<UTextBlock>(C->WidgetTree->FindWidget(TEXT("Label"))), C); } }
        if (Variant)
        {
            for (const auto* Name : {TEXT("MaterialMode"), TEXT("Synthesize")})
            { auto* B = CastChecked<UButton>(Find(Name)); CheckText(Name, Cast<UTextBlock>(B->GetContent()), B); }
            const auto OfferName = Variant == 2 ? LongName : FString(TEXT("Purchase_0"));
            auto* B = CastChecked<UButton>(Find(*OfferName)); CheckText(OfferName, Cast<UTextBlock>(B->GetContent()), B);
            TestTrue(TEXT("目标商品完整包含在商店滚动区域"), Contains(Rect(Find(TEXT("ShopList"))), Rect(B)));
        }
        if (Variant == 2)
        {
            for (const auto* Name : {TEXT("StatLabel_IncomeRate"), TEXT("StatValue_IncomeRate")})
            { CheckText(Name, Cast<UTextBlock>(Find(Name)), Find(TEXT("StatRow_IncomeRate"))); }
            TestTrue(TEXT("金币每秒整行滚入属性可见区域"), Contains(Rect(Find(TEXT("AttributeList"))), Rect(Find(TEXT("StatRow_IncomeRate")))));
        }
        TArray<FColor> Colors;
        if (!TestTrue(TEXT("真实RT像素读回"), Target->GameThread_GetRenderTargetResource()->ReadPixels(Colors))) { return false; }
        int32 Bright = 0; TSet<uint32> Distinct;
        for (const auto& C : Colors) { Bright += C.R > 150 && C.G > 150 && C.B > 150; Distinct.Add(C.DWColor()); }
        TestTrue(TEXT("图像有真实高亮文字及颜色变化，不是空白RT"), Bright > 500 && Distinct.Num() > 100);
        const FString Stem = Output / (TEXT("OffscreenFixture-") + Case + TEXT("-") + State);
        TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Pixels.X, Pixels.Y, MakeArrayView(Colors), PNG);
        TestTrue(TEXT("写出原始尺寸PNG"), FFileHelper::SaveArrayToFile(PNG, *(Stem + TEXT(".png"))));
        auto Record = MakeShared<FJsonObject>(); Record->SetStringField(TEXT("kind"), TEXT("D3D12 offscreen UMG fixture; not natural gameplay or native input"));
        Record->SetStringField(TEXT("profileReadOnly"), ProfilePath); Record->SetStringField(TEXT("rhi"), GDynamicRHI->GetName());
        Record->SetStringField(TEXT("pixelFormat"), TEXT("PF_B8G8R8A8")); Record->SetBoolField(TEXT("rtSRGB"), Target->IsSRGB());
        Record->SetBoolField(TEXT("widgetGammaCorrection"), true);
        Record->SetBoolField(TEXT("osWindowHandleNull"), !Window->GetNativeWindow().IsValid() || Window->GetNativeWindow()->GetOSWindowHandle() == nullptr);
        Record->SetBoolField(TEXT("windowSupportsKeyboardFocus"), Window->SupportsKeyboardFocus());
        Record->SetStringField(TEXT("state"), State); Record->SetStringField(TEXT("png"), Stem + TEXT(".png"));
        Record->SetStringField(TEXT("fixtureData"), TEXT("FWidgetWorld完整视图；生命250/300、金币317、收入2.75、物品和技能/CD为受控样本；商品价格名称顺序/配方/天赋名来自只读Profile，未运行交易。"));
        Record->SetNumberField(TEXT("width"), Pixels.X); Record->SetNumberField(TEXT("height"), Pixels.Y);
        Record->SetNumberField(TEXT("brightPixels"), Bright); Record->SetNumberField(TEXT("distinctColors"), Distinct.Num());
        Record->SetNumberField(TEXT("shopOfferCount"), V.ShopOffers.Num()); Record->SetNumberField(TEXT("longestShopIndex"), Longest);
        Record->SetStringField(TEXT("longestShopName"), V.ShopOffers[Longest].DisplayName.ToString());
        Record->SetArrayField(TEXT("metrics"), Metrics);
        FString Json; FJsonSerializer::Serialize(Record, TJsonWriterFactory<>::Create(&Json));
        TestTrue(TEXT("保存同一次离屏绘制的字体及几何事实"), FFileHelper::SaveStringToFile(Json, *(Stem + TEXT(".json"))));
        AddInfo(TEXT("HUD08_OFFSCREEN_PNG ") + Stem + TEXT(".png RHI=D3D12"));
    }
    TestFalse(TEXT("视觉夹具从未发送交易"), P->GetModel().IsPending());
    return true;
}
#endif
#endif
