#include "Misc/AutomationTest.h"
#include "Playground/Y01R36Verification.h"
#include "Y01MatchFixture.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/UnrealType.h"
#include "Playground/Y01R36UI.h"
#include "Engine/LocalPlayer.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/ScopeExit.h"
#include "Components/CanvasPanelSlot.h"
#include "Widgets/SWindow.h"
#include "Types/PaintArgs.h"
#include "Input/HittestGrid.h"
#include "Rendering/DrawElements.h"
#include "Slate/UMGDragDropOp.h"
#include "Slate/SObjectWidget.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
using K = EY01TableKind;
using O = EY01ItemOperation;
using E = EY01ItemError;
// 真实权威对象的本地路由夹具，不宣称建立网络连接或执行网络RPC。
struct FR36ControllerWorld
{
    FY01MatchFixture Data;
    UWorld* World;
    AY01R36RuntimeProbe* Mode;
    AY01R36Controller* PC;
    AY01R36Controller* Other;
    FString Error;
    bool Ready = false;
    FR36ControllerWorld(bool Economy = false)
    {
        auto& C = Data.Row<FY01CharacterRow>(K::Character);
        C.InitialEquipment.Reset(); C.InitialSkills.Reset(); C.AttackDamage = 30;
        FY01SourceEffectRow Effect; Effect.Magnitude = 10; Data.Add(K::SourceEffect, Effect);
        auto& Equipment = Data.Row<FY01EquipmentRow>(K::Equipment);
        Equipment.DisplayName = FText::FromString(TEXT("程序装备")); Equipment.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect)};
        auto& Skill = Data.Row<FY01SkillRow>(K::Skill);
        Skill.DisplayName = FText::FromString(TEXT("程序同名技能")); Skill.CooldownSeconds = 8;
        Data.Add(K::Skill, Skill, TEXT("Second"));
        auto* Profile = NewObject<UY01R36Profile>();
        Profile->bEvidenceMode = true;
        Profile->Character = FY01MatchFixture::Ref(K::Character); Profile->OrderedWaves = Data.Waves();
        Profile->ContentEquipment = {FY01MatchFixture::Ref(K::Equipment)};
        Profile->ContentSkills = {FY01MatchFixture::Ref(K::Skill), FY01MatchFixture::Ref(K::Skill, TEXT("Second"))};
        if (Economy)
        {
            auto& Blue = Data.Row<FY01EquipmentRow>(K::Equipment, TEXT("Blue"));
            Blue.DisplayName = FText::FromString(TEXT("蓝色产物")); Blue.EffectIds = Equipment.EffectIds;
            auto& Offer = Data.Row<FY01ShopOfferRow>(K::ShopOffer); Offer.Price = 10; Offer.DisplayName = FText::FromString(TEXT("购买装备"));
            FY01ShopOfferRow Direct; Direct.Price = 50; Direct.ShopKind = EY01ShopKind::DirectEffect;
            Direct.DisplayName = FText::FromString(TEXT("直接增益")); Direct.EffectIds = Equipment.EffectIds; Data.Add(K::ShopOffer, Direct, TEXT("Direct"));
            FY01TalentChoiceRow Choice;
            for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Two")), FName(TEXT("Three"))})
            {
                FY01TalentRow T; T.DisplayName = FText::FromString(TEXT("天赋")); T.EffectIds = Equipment.EffectIds;
                Data.Add(K::Talent, T, Name); Choice.Candidates.Add(Data.Ref(K::Talent, Name));
            }
            Data.Add(K::TalentChoice, Choice);
            Profile->bEconomyEnabled = true; Profile->InitialGold = 200; Profile->BaseGoldPerSecond = 1;
            Profile->ContentShopOffers = {Data.Ref(K::ShopOffer), Data.Ref(K::ShopOffer, TEXT("Direct"))};
            Profile->ContentRecipes = {Data.Ref(K::Synthesis)}; Profile->TalentChoice = Data.Ref(K::TalentChoice);
        }
        for (const auto& Pair : Data.Tables) { Profile->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01R36RuntimeProbe::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01R36RuntimeProbe>(); PC = AddPlayer(); Other = AddPlayer();
        World->SpawnActor<AY01R36Config>()->Content = Profile;
        Ready = Mode->LoadMatchConfiguration(Error) && L()->IsInitialized() && Mode->TryStartNextWave();
    }
    ~FR36ControllerWorld()
    { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    AY01R36Controller* AddPlayer()
    {
        auto* Controller = World->SpawnActor<AY01R36Controller>(); auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(0, 0, 100), FRotator::ZeroRotator);
        PS->SetOwner(Controller); Controller->PlayerState = PS; Controller->Possess(Pawn);
        PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn); Pawn->GetCharacterMovement()->DisableMovement();
        Mode->GetFlowState()->AddPlayerState(PS); return Controller;
    }
    UY01LoadoutComponent* L() const { return PC->GetPlayerState<AY01PlayerState>()->GetLoadout(); }
    FY01CommittedView View() const { FY01CommittedView V; L()->ReadCommittedView(V); return V; }
    FGuid Add(K Type = K::Equipment, FName Name = TEXT("Default"))
    {
        FY01ItemGrant G; G.ItemId = FGuid::NewGuid(); G.Definition = FY01MatchFixture::Ref(Type, Name);
        if (!L()->AddItems(FY01DataCatalog(Data.Tables), {G}).IsOk()) { return {}; }
        return G.ItemId;
    }
    FY01ItemRequest Request(FGuid Id, int32 Slot = 0, O Op = O::Equip) const
    {
        auto V = View(); FY01ItemRequest R; R.MatchId = V.MatchId; R.JoinSequence = V.JoinSequence;
        R.Seq = V.ExpectedSeq; R.ConfigRevision = V.ConfigRevision; R.ItemId = Id; R.Slot = Slot; R.Operation = Op;
        const auto* Item = V.Items.FindByPredicate([&](const auto& I) { return I.ItemId == Id; });
        R.ExpectedEntityRevision = Item ? Item->EntityRevision : 0;
        if (Op == O::Equip || Op == O::Unequip)
        {
            R.ExpectedSlotItemId = V.EquipmentSlots[Slot];
            const auto* Occupant = V.Items.FindByPredicate([&](const auto& I) { return I.ItemId == R.ExpectedSlotItemId; });
            R.ExpectedSlotEntityRevision = Occupant ? Occupant->EntityRevision : 0;
        }
        else { R.ExpectedLearnedId = V.Skills[Slot].LearnedId; R.ExpectedLearnedRevision = V.Skills[Slot].EntityRevision; }
        return R;
    }
    void Send(const FY01ItemRequest& R) { PC->ServerItemRequest_Implementation(R); }
};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36ControllerTest, "Y01.Framework.R36.Controller",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36ControllerTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Contract13"), TEXT("ReliableRPC"), TEXT("DirectReceipt"), TEXT("Equip"), TEXT("Replace"),
        TEXT("Unequip"), TEXT("Replay"), TEXT("PayloadConflict"), TEXT("Session"), TEXT("SequenceGap"), TEXT("InvalidSequence"),
        TEXT("InvalidOperation"), TEXT("ForeignItem"), TEXT("MissingPawn"), TEXT("Dead"), TEXT("Ended"), TEXT("Learn"),
        TEXT("Confirm"), TEXT("Cancel"), TEXT("Remove"), TEXT("DuplicateSkill"), TEXT("BoundedReceipts"),
        TEXT("LegacyActionIsolation"), TEXT("LegacyRestartPreserved"), TEXT("ResponseTransport") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36ControllerTest::RunTest(const FString& Case)
{
    if (Case == TEXT("Contract13"))
    {
        TSet<FName> Fields;
        for (TFieldIterator<FProperty> It(FY01ItemRequest::StaticStruct()); It; ++It) { Fields.Add(It->GetFName()); }
        TestEqual(TEXT("仅追加经济身份意图，不扩展奖励/Owner"), Fields.Num(), 18);
        for (const TCHAR* Name : {TEXT("Recipe"), TEXT("Talent"), TEXT("TalentChoiceId")})
        { TestEqual(TEXT("新增合成天赋身份恰好一个"), Fields.Remove(Name), 1); }
        TestEqual(TEXT("商品意图恰好一个"), Fields.Remove(TEXT("Offer")), 1);
        TestEqual(TEXT("材料意图恰好一个"), Fields.Remove(TEXT("Materials")), 1);
        TestEqual(TEXT("保留原13字段契约"), Fields.Num(), 13);
        for (const TCHAR* Name : {TEXT("MatchId"), TEXT("JoinSequence"), TEXT("Seq"), TEXT("Operation"), TEXT("ItemId"),
            TEXT("ExpectedEntityRevision"), TEXT("Slot"), TEXT("ExpectedSlotItemId"), TEXT("ExpectedSlotEntityRevision"),
            TEXT("ExpectedLearnedId"), TEXT("ExpectedLearnedRevision"), TEXT("ConfigRevision"), TEXT("TicketId")})
        { TestTrue(TEXT("原意图字段不可替换或删除"), Fields.Contains(Name)); }
        const auto* Offer = FindFProperty<FStructProperty>(FY01ItemRequest::StaticStruct(), TEXT("Offer"));
        TestTrue(TEXT("商品只能是静态引用"), Offer && Offer->Struct == FY01DataRef::StaticStruct());
        for (const TCHAR* Name : {TEXT("Recipe"), TEXT("Talent")})
        { const auto* Ref = FindFProperty<FStructProperty>(FY01ItemRequest::StaticStruct(), Name);
            TestTrue(TEXT("合成天赋只有静态身份"), Ref && Ref->Struct == FY01DataRef::StaticStruct()); }
        const auto* Choice = FindFProperty<FStructProperty>(FY01ItemRequest::StaticStruct(), TEXT("TalentChoiceId"));
        TestTrue(TEXT("候选机会由服务器Guid绑定"), Choice && Choice->Struct == TBaseStructure<FGuid>::Get());
        const auto* Materials = FindFProperty<FArrayProperty>(FY01ItemRequest::StaticStruct(), TEXT("Materials"));
        const auto* Material = Materials ? CastField<FStructProperty>(Materials->Inner) : nullptr;
        TestTrue(TEXT("材料只能是实体意图数组"), Material && Material->Struct == FY01ItemConsume::StaticStruct());
        auto Count = [](UScriptStruct* Type)
        { int32 N = 0; for (TFieldIterator<FProperty> It(Type); It; ++It) { ++N; } return N; };
        TestEqual(TEXT("静态引用无价格结果种子"), Count(FY01DataRef::StaticStruct()), 2);
        TestNotNull(TEXT("引用类型"), FindFProperty<FEnumProperty>(FY01DataRef::StaticStruct(), TEXT("Type")));
        TestNotNull(TEXT("引用行名"), FindFProperty<FNameProperty>(FY01DataRef::StaticStruct(), TEXT("RowName")));
        TestEqual(TEXT("材料无成长位置Owner"), Count(FY01ItemConsume::StaticStruct()), 2);
        const auto* Id = FindFProperty<FStructProperty>(FY01ItemConsume::StaticStruct(), TEXT("ItemId"));
        TestTrue(TEXT("材料身份为Guid"), Id && Id->Struct == TBaseStructure<FGuid>::Get());
        TestNotNull(TEXT("材料只带期望版本"), FindFProperty<FUInt64Property>(FY01ItemConsume::StaticStruct(), TEXT("ExpectedEntityRevision")));
        return true;
    }
    if (Case == TEXT("ReliableRPC"))
    {
        const auto* Function = AY01R36Controller::StaticClass()->FindFunctionByName(TEXT("ServerItemRequest"));
        TestTrue(TEXT("真实反射Server Reliable契约"), Function && Function->HasAllFunctionFlags(FUNC_Net | FUNC_NetServer | FUNC_NetReliable));
        return true;
    }
    FR36ControllerWorld F;
    if (!TestTrue(TEXT("独立模式夹具必须先成功"), F.Ready)) { AddError(F.Error); return false; }
    if (Case == TEXT("LegacyActionIsolation"))
    {
        F.PC->ServerPlaygroundAction_Implementation(8); F.PC->ServerPlaygroundAction_Implementation(9);
        TestTrue(TEXT("R36不执行旧定义调试入口或旧回执"), F.PC->GetObserver().EquipmentResult.IsEmpty()); return true;
    }
    if (Case == TEXT("LegacyRestartPreserved"))
    { F.PC->ServerPlaygroundAction_Implementation(5); TestFalse(TEXT("保留旧重开提示"), F.PC->GetObserver().Notice.IsEmpty()); return true; }
    const auto Id = F.Add(); TestTrue(TEXT("内部夹具入包成功，不是购买"), Id.IsValid());
    auto R = F.Request(Id); auto Before = F.View(); E Expected = E::None;
    if (Case == TEXT("Session")) { R.MatchId = FGuid::NewGuid(); Expected = E::Session; }
    if (Case == TEXT("SequenceGap")) { R.Seq += 5; Expected = E::SequenceGap; }
    if (Case == TEXT("InvalidSequence")) { R.Seq = 0; Expected = E::InvalidSequence; }
    if (Case == TEXT("InvalidOperation")) { R.Operation = static_cast<O>(255); Expected = E::InvalidOperation; }
    if (Case == TEXT("ForeignItem"))
    {
        FY01ItemGrant G; G.ItemId = FGuid::NewGuid(); G.Definition = FY01MatchFixture::Ref(K::Equipment);
        auto* Other = F.Other->GetPlayerState<AY01PlayerState>()->GetLoadout();
        TestTrue(TEXT("其他玩家内部入包"), Other->AddItems(FY01DataCatalog(F.Data.Tables), {G}).IsOk());
        R.ItemId = G.ItemId; R.ExpectedEntityRevision = 1; Expected = E::MissingItem;
    }
    if (Case == TEXT("MissingPawn")) { F.PC->UnPossess(); Expected = E::Authority; }
    if (Case == TEXT("Dead"))
    { F.PC->GetPlayerState<AY01PlayerState>()->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); Expected = E::Dead; }
    if (Case == TEXT("Ended")) { F.Mode->TryEndMatch(false); Expected = E::Phase; }
    if (Case == TEXT("DirectReceipt")) { F.L()->SubmitRequest(F.PC, R); }
    else { F.Send(R); }
    auto V = F.View();
    TestEqual(TEXT("完整视图携带每次响应序号"), V.ResponseSerial, uint64(1));
    TestEqual(TEXT("完整视图权威响应码"), V.Response.Code, Expected);
    TestTrue(TEXT("完整回执精确绑定13字段请求"), FY01ItemRequest::StaticStruct()->CompareScriptStruct(&V.Response.Request, &R, 0));
    TestTrue(TEXT("响应更新独立ViewRevision"), V.ViewRevision > Before.ViewRevision);
    if (Expected != E::None)
    {
        TestEqual(TEXT("拒绝不换装"), V.EquipmentSlots[0], FGuid()); TestEqual(TEXT("拒绝不动实体版本"), V.Revision, Before.Revision);
        const bool Admission = Expected == E::Session || Expected == E::SequenceGap || Expected == E::InvalidSequence || Expected == E::Authority;
        TestEqual(TEXT("原入场防重语义保持"), V.ExpectedSeq, Before.ExpectedSeq + (Admission ? 0 : 1)); return true;
    }
    TestEqual(TEXT("实际穿戴由权威决定"), V.EquipmentSlots[0], Id);
    if (Case == TEXT("Replay") || Case == TEXT("PayloadConflict"))
    {
        if (Case == TEXT("PayloadConflict")) { R.Slot = 1; }
        F.Send(R); const auto Again = F.View();
        TestEqual(TEXT("重发也有新响应而不复制物品"), Again.ResponseSerial, uint64(2));
        TestEqual(TEXT("重发保留实体版本"), Again.Revision, V.Revision); TestEqual(TEXT("重发不推进Seq"), Again.ExpectedSeq, V.ExpectedSeq);
        TestEqual(TEXT("同号异载荷不冒充旧成功"), Again.Response.Code, Case == TEXT("Replay") ? E::None : E::PayloadConflict);
    }
    if (Case == TEXT("Replace") || Case == TEXT("Unequip"))
    {
        const auto Next = Case == TEXT("Replace") ? F.Add() : Id;
        F.Send(F.Request(Next, 0, Case == TEXT("Replace") ? O::Equip : O::Unequip)); V = F.View();
        TestEqual(TEXT("替换/卸下权威槽"), V.EquipmentSlots[0], Case == TEXT("Replace") ? Next : FGuid());
        const auto* Old = V.Items.FindByPredicate([&](const auto& I) { return I.ItemId == Id; });
        TestTrue(TEXT("旧件确实回包"), Old && Old->Location == EY01ItemLocation::Backpack);
    }
    const bool SkillCase = Case == TEXT("Learn") || Case == TEXT("Confirm") || Case == TEXT("Cancel") || Case == TEXT("Remove") || Case == TEXT("DuplicateSkill");
    if (SkillCase)
    {
        const auto Book = F.Add(K::Skill); F.Send(F.Request(Book, 0, O::PrepareLearn)); V = F.View();
        TestTrue(TEXT("空槽真实学习"), V.Skills[0].LearnedId.IsValid());
        TestFalse(TEXT("成功消耗原书"), V.Items.ContainsByPredicate([&](const auto& I) { return I.ItemId == Book; }));
        const auto Learned = V.Skills[0].LearnedId;
        if (Case == TEXT("Remove"))
        {
            F.Send(F.Request({}, 0, O::RemoveSkill)); V = F.View();
            TestFalse(TEXT("移除不保留技能"), V.Skills[0].LearnedId.IsValid()); TestEqual(TEXT("移除不返书"), V.Items.Num(), 1);
        }
        if (Case == TEXT("DuplicateSkill"))
        {
            const auto Duplicate = F.Add(K::Skill); F.Send(F.Request(Duplicate, 1, O::PrepareLearn)); V = F.View();
            TestEqual(TEXT("同ID跨槽拒绝"), V.Response.Code, E::DuplicateSkill); TestEqual(TEXT("原身份不变"), V.Skills[0].LearnedId, Learned);
        }
        if (Case == TEXT("Confirm") || Case == TEXT("Cancel"))
        {
            const auto Second = F.Add(K::Skill, TEXT("Second")); F.Send(F.Request(Second, 0, O::PrepareLearn)); V = F.View();
            TestTrue(TEXT("占用槽生成原票据"), V.Response.TicketId.IsValid()); TestEqual(TEXT("确认前不移动"), V.Skills[0].LearnedId, Learned);
            auto Ticket = V.Tickets[0]; Ticket.Seq = V.ExpectedSeq; Ticket.Operation = Case == TEXT("Confirm") ? O::ConfirmLearn : O::CancelLearn;
            F.Send(Ticket); V = F.View(); TestEqual(TEXT("确认/取消原接口成功"), V.Response.Code, E::None);
            TestEqual(TEXT("不同ID同名可覆盖，取消不变"), V.Skills[0].LearnedId == Learned, Case == TEXT("Cancel"));
            TestEqual(TEXT("只有确认消耗第二书"), V.Items.ContainsByPredicate([&](const auto& I) { return I.ItemId == Second; }), Case == TEXT("Cancel"));
        }
    }
    if (Case == TEXT("BoundedReceipts"))
    {
        for (int32 I = 0; I < 70; ++I) { F.Send(F.Request(FGuid::NewGuid())); }
        V = F.View(); TestEqual(TEXT("仍只有64份规范化账本"), V.Receipts.Num(), 64);
        F.Send(R); V = F.View(); TestEqual(TEXT("过期序列完整拒绝回执"), V.Response.Code, E::ExpiredSequence);
        TestEqual(TEXT("不推进过期请求"), V.ExpectedSeq, uint64(72));
    }
    if (Case == TEXT("ResponseTransport"))
    {
        R.Seq += 10; F.Send(R); V = F.View(); FY01ViewSender Sender; FY01ViewReceiver Receiver; FY01ViewWire Wire; FString Error;
        TestTrue(TEXT("响应进入原有分窗通道"), Sender.Offer(V, Wire, Error));
        int32 Windows = 0;
        while (!Wire.Items.IsEmpty() && ++Windows < 500)
        { FY01ViewAck Ack; if (!Receiver.Receive(Wire, Ack, Error) || !Sender.Acknowledge(Ack, Wire, Error)) { AddError(Error); break; } }
        TestEqual(TEXT("收齐才读到失败完整回执"), Receiver.Get().Response.Code, E::SequenceGap);
        TestEqual(TEXT("响应序号不丢失"), Receiver.Get().ResponseSerial, V.ResponseSerial);
    }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36UIRequestChainTest, "Y01.Framework.R36.UI.RequestChain",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36UIRequestChainTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Equip"), TEXT("Replace"), TEXT("Unequip"), TEXT("Learn"), TEXT("Confirm"),
        TEXT("Cancel"), TEXT("Remove"), TEXT("Duplicate"), TEXT("DeathRace"), TEXT("NoDirectSwap") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36UIRequestChainTest::RunTest(const FString& Case)
{
    // 原生控件事件到Controller包装入口的本地权威链，不冒充跨进程RPC或真实鼠标。
    FR36ControllerWorld F; if (!TestTrue(TEXT("夹具成功"), F.Ready)) { AddError(F.Error); return false; }
    F.World->SetGameInstance(nullptr);
    auto* Local = NewObject<ULocalPlayer>(GEngine); Local->PlayerController = F.PC; F.PC->Player = Local;
    TStrongObjectPtr<UY01R36Panel> Panel(CreateWidget<UY01R36Panel>(F.PC)); const auto Slate = Panel->TakeWidget();
    for (const auto* Name : {TEXT("ToggleBackpack"), TEXT("ToggleCharacter"), TEXT("ToggleShop")})
    {
        auto* Toggle = Cast<UButton>(Panel->WidgetTree->FindWidget(Name));
        if (!TestNotNull(TEXT("交互用例通过三个真实按钮独立展开"), Toggle)) { return false; }
        Toggle->OnClicked.Broadcast();
    }
    TestTrue(TEXT("三面板均可展开"), Panel->IsBackpackExpanded() && Panel->IsCharacterExpanded() && Panel->IsShopExpanded());
    auto Present = [&]() { Panel->Present(F.View(), EY01MatchPhase::Preparing, 11); };
    auto Drop = [&](FGuid Id, EY01R36Zone Target, int32 Index)
    {
        TStrongObjectPtr<UY01R36Cell> Source(CreateWidget<UY01R36Cell>(F.World)); const auto SourceSlate = Source->TakeWidget();
        const auto* Item = Panel->GetModel().Find(Id); if (!Item) { return false; }
        Source->Configure(Panel.Get(), Item->Location == EY01ItemLocation::Backpack ? EY01R36Zone::Backpack : EY01R36Zone::Equipment, Item->Slot, Id);
        UDragDropOperation* Drag = nullptr; Source->NativeOnDragDetected(FGeometry(), FPointerEvent(), Drag);
        if (!TestNotNull(TEXT("原生拖动捕获身份"), Drag)) { return false; }
        if (Target == EY01R36Zone::Backpack) { return Panel->Accept(Cast<UY01R36DragOperation>(Drag), Target, Index); }
        return Panel->GetCell(Target, Index)->NativeOnDrop(FGeometry(), FDragDropEvent(FPointerEvent(), nullptr), Drag);
    };
    const bool Skill = Case == TEXT("Learn") || Case == TEXT("Confirm") || Case == TEXT("Cancel") || Case == TEXT("Remove") || Case == TEXT("Duplicate");
    const auto Id = F.Add(Skill ? K::Skill : K::Equipment); Present();
    if (Case == TEXT("DeathRace")) { F.PC->GetPlayerState<AY01PlayerState>()->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    const auto Target = Skill ? EY01R36Zone::Skill : EY01R36Zone::Equipment;
    TestTrue(TEXT("控件发送真实意图"), Drop(Id, Target, 0));
    TestTrue(TEXT("未刷新完整视图前仍在等待"), Panel->GetModel().IsPending());
    TestEqual(TEXT("装备在途只做显示预览，技能书不预览"), Panel->GetBackpack()->GetNumItems(), Skill ? 1 : 0);
    if (!Skill) { TestEqual(TEXT("预览槽显示被拖装备"), Panel->GetCell(Target, 0)->GetItemId(), Id); }
    Present(); TestFalse(TEXT("权威回执解除等待"), Panel->GetModel().IsPending());
    auto V = F.View();
    if (Case == TEXT("DeathRace"))
    {
        TestEqual(TEXT("服务器死亡拒绝"), V.Response.Code, E::Dead); TestTrue(TEXT("失败文字展示"), Panel->GetModel().Status().Contains(TEXT("死亡")));
        TestEqual(TEXT("拒绝后物品回到背包原格"), Panel->GetBackpack()->GetNumItems(), 1);
        TestFalse(TEXT("拒绝后装备槽还原为空"), Panel->GetCell(Target, 0)->GetItemId().IsValid()); return true;
    }
    TestEqual(TEXT("实际权威成功"), V.Response.Code, E::None); TestEqual(TEXT("权威提交后格才消失"), Panel->GetBackpack()->GetNumItems(), 0);
    if (!Skill)
    {
        TestEqual(TEXT("装备槽与权威一致"), Panel->GetCell(Target, 0)->GetItemId(), Id);
        if (Case == TEXT("NoDirectSwap")) { TestFalse(TEXT("身上直换不发送"), Drop(Id, Target, 1)); }
        if (Case == TEXT("Replace") || Case == TEXT("Unequip"))
        {
            const auto Next = Case == TEXT("Replace") ? F.Add() : Id; Present();
            TestTrue(TEXT("替换或回包意图"), Drop(Next, Case == TEXT("Replace") ? Target : EY01R36Zone::Backpack, 0)); Present();
            TestEqual(TEXT("旧件权威回包"), Panel->GetModel().Find(Id)->Location, EY01ItemLocation::Backpack);
        }
        return true;
    }
    const auto Learned = V.Skills[0].LearnedId;
    if (Case == TEXT("Remove"))
    {
        const FPointerEvent Right(0, FVector2D(20), FVector2D(20), TSet<FKey>{EKeys::RightMouseButton}, EKeys::RightMouseButton, 0, FModifierKeysState());
        TestTrue(TEXT("原生右键移除"), Panel->GetCell(Target, 0)->NativeOnMouseButtonDown(FGeometry(), Right).IsEventHandled()); Present();
        TestFalse(TEXT("已学移除"), F.View().Skills[0].LearnedId.IsValid()); TestEqual(TEXT("不返书"), Panel->GetBackpack()->GetNumItems(), 0);
    }
    if (Case == TEXT("Confirm") || Case == TEXT("Cancel") || Case == TEXT("Duplicate"))
    {
        const auto Book = F.Add(K::Skill, Case == TEXT("Duplicate") ? TEXT("Default") : TEXT("Second")); Present();
        TestTrue(TEXT("第二本书投递"), Drop(Book, Target, Case == TEXT("Duplicate") ? 1 : 0)); Present();
        TestEqual(TEXT("确认前原技能不变"), F.View().Skills[0].LearnedId, Learned);
        if (Case == TEXT("Duplicate")) { TestEqual(TEXT("服务器同ID拒绝"), F.View().Response.Code, E::DuplicateSkill); }
        else
        {
            TestTrue(TEXT("权威票据弹框"), Panel->GetModel().NeedsConfirmation());
            CastChecked<UButton>(Panel->WidgetTree->FindWidget(Case == TEXT("Confirm") ? TEXT("Confirm") : TEXT("Cancel")))->OnClicked.Broadcast(); Present();
            TestFalse(TEXT("响应关闭确认"), Panel->GetModel().NeedsConfirmation());
            TestEqual(TEXT("确认才替换"), F.View().Skills[0].LearnedId == Learned, Case == TEXT("Cancel"));
            TestEqual(TEXT("取消保留原书"), Panel->GetBackpack()->GetNumItems(), Case == TEXT("Cancel") ? 1 : 0);
        }
    }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36BurstTest, "Y01.Framework.R36.Controller.Burst",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36BurstTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("IntentPlan"), TEXT("InvalidBase"), TEXT("OneShotDispatch"), TEXT("RequestRecord") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36BurstTest::RunTest(const FString& Case)
{
    FR36ControllerWorld F;
    if (!TestTrue(TEXT("先核真实权威夹具"), F.Ready)) { AddError(F.Error); return false; }
    TArray<FGuid> Equipment;
    for (int32 I = 0; I < 6; ++I) { Equipment.Add(F.Add()); }
    const FGuid BookA = F.Add(K::Skill), BookB = F.Add(K::Skill, TEXT("Second"));
    const auto Base = F.View();
    if (Case == TEXT("IntentPlan"))
    {
        const auto Plan = Y01R36Trace::BuildBurst(Base, EY01MatchPhase::Preparing);
        if (!TestEqual(TEXT("六装配加两学习意图"), Plan.Num(), 8)) { return false; }
        for (int32 I = 0; I < 8; ++I)
        {
            auto Expected = F.Request(I < 6 ? Equipment[I] : I == 6 ? BookA : BookB, I < 6 ? I : I - 6,
                I < 6 ? O::Equip : O::PrepareLearn); Expected.Seq += I;
            TestTrue(TEXT("仅现有13字段及原始实体版本"), FY01ItemRequest::StaticStruct()->CompareScriptStruct(&Expected, &Plan[I], 0));
        }
        auto After = F.View();
        TestTrue(TEXT("构造意图不乐观改动权威"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Base, &After, 0));
        return true;
    }
    if (Case == TEXT("InvalidBase"))
    {
        for (int32 I = 0; I < 6; ++I)
        {
            auto Bad = Base;
            if (I == 0) { Bad.Items.Pop(); }
            if (I == 1) { Bad.EquipmentSlots[0] = Equipment[0]; }
            if (I == 2) { Bad.Health = 0; }
            if (I == 3) { Bad.ExpectedSeq = MAX_uint64 - 6; }
            if (I == 4) { Bad.Items.Last().Definition = Bad.Items[6].Definition; }
            TestTrue(TEXT("不足/占用/死亡/溢出/同ID/等待必须整批不发送"),
                Y01R36Trace::BuildBurst(Bad, I == 5 ? EY01MatchPhase::Waiting : EY01MatchPhase::Preparing).IsEmpty());
        }
        TestEqual(TEXT("合法基线仍可构造，拒绝测试不是永远空"), Y01R36Trace::BuildBurst(Base, EY01MatchPhase::Battle).Num(), 8);
        return true;
    }
    if (Case == TEXT("OneShotDispatch"))
    {
        bool Sent = false; int32 Calls = 0;
        auto Send = [&](const FY01ItemRequest& R) { ++Calls; F.Send(R); };
        TestTrue(TEXT("未显式启用不发送"), Y01R36Trace::TryBurst(Base, EY01MatchPhase::Preparing, false, Sent, Send).IsEmpty());
        TestEqual(TEXT("默认零调用"), Calls, 0); TestFalse(TEXT("等待不占用一次机会"), Sent);
        const FString Record = Y01R36Trace::TryBurst(Base, EY01MatchPhase::Preparing, true, Sent, Send);
        TestEqual(TEXT("同一次调度提交八份意图"), Calls, 8); TestTrue(TEXT("一次性标记"), Sent);
        TSharedPtr<FJsonObject> Json;
        if (TestTrue(TEXT("发送记录必须可解析"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Record), Json) && Json.IsValid()))
        {
            TestEqual(TEXT("明确压力而非鼠标"), Json->GetStringField(TEXT("kind")), FString(TEXT("BURST_SENT")));
            TestEqual(TEXT("八份意图摘要"), Json->GetArrayField(TEXT("requests")).Num(), 8);
        }
        Y01R36Trace::TryBurst(Base, EY01MatchPhase::Preparing, true, Sent, Send);
        TestEqual(TEXT("后续Tick不重发"), Calls, 8);
        const auto Final = F.View();
        TestEqual(TEXT("原路由连续Seq成功"), Final.ExpectedSeq, Base.ExpectedSeq + 8);
        TestEqual(TEXT("原学习事务真实耗两书"), Final.Items.Num(), 6);
        TestEqual(TEXT("末次权威成功"), Final.Response.Code, E::None);
        return true;
    }
    auto Request = F.Request(BookA, 0, O::PrepareLearn); F.Send(Request);
    TSharedPtr<FJsonObject> Json;
    const FString Record = Y01R36Trace::RequestRecord(F.PC);
    if (!TestTrue(TEXT("每请求完整回执Trace可解析"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Record), Json) && Json.IsValid())) { return false; }
    TestEqual(TEXT("请求提交类型"), Json->GetStringField(TEXT("kind")), FString(TEXT("REQUEST_COMMIT")));
    TestFalse(TEXT("实际完整注册表中书已删除"), Json->GetBoolField(TEXT("requestPresent")));
    const auto& View = Json->GetObjectField(TEXT("view"));
    TestEqual(TEXT("确为请求结束后完整版本"), View->GetNumberField(TEXT("revision")), double(F.View().ViewRevision));
    TestEqual(TEXT("原意图Seq"), View->GetNumberField(TEXT("seq")), double(Request.Seq));
    const auto* Property = FindFProperty<FStructProperty>(F.L()->GetClass(), TEXT("ViewWire"));
    const auto* Wire = Property->ContainerPtrToValuePtr<FY01ViewWire>(F.L());
    TestEqual(TEXT("来自真实FastArray在途版本"), Json->GetNumberField(TEXT("inFlightRevision")), double(Wire->Items[0].Scope.ViewRevision));
    TestTrue(TEXT("保留服务器帧号"), Json->HasField(TEXT("frame")));
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36BurstWiringTest, "Y01.Framework.R36.Controller.BurstWiring",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36BurstWiringTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("ClientOnce"), TEXT("Waiting"), TEXT("Disabled"), TEXT("InvalidFlag"),
        TEXT("Authority"), TEXT("NoOwner"), TEXT("MissingView"), TEXT("Ended"), TEXT("CommitSuccess"),
        TEXT("CommitFailure"), TEXT("CommitDisabled") }) { Names.Add(N); Commands.Add(N); }
}
bool FY01R36BurstWiringTest::RunTest(const FString& Case)
{
    const FString Previous = FCommandLine::Get(); ON_SCOPE_EXIT { FCommandLine::Set(*Previous); };
    FCommandLine::Set(TEXT(""));
    FR36ControllerWorld F;
    if (!TestTrue(TEXT("接线夹具先成功，不冒充真客户端"), F.Ready)) { AddError(F.Error); return false; }
    for (int32 I = 0; I < 6; ++I) { TestTrue(TEXT("装配夹具入包"), F.Add().IsValid()); }
    const auto Book = F.Add(K::Skill); F.Add(K::Skill, TEXT("Second"));
    const auto Base = F.View();
    auto FindRecord = [](const TArray<FString>& Rows, const FString& Kind)
    {
        TSharedPtr<FJsonObject> Found;
        for (const auto& Row : Rows)
        {
            TSharedPtr<FJsonObject> Json;
            if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Row), Json) && Json
                && Json->GetStringField(TEXT("kind")) == Kind) { Found = Json; }
        }
        return Found;
    };
    if (Case.StartsWith(TEXT("Commit")))
    {
        FCommandLine::Set(Case == TEXT("CommitDisabled") ? TEXT("") : TEXT("-R36Trace"));
        auto Request = F.Request(Book, 0, O::PrepareLearn);
        if (Case == TEXT("CommitFailure")) { Request.Seq += 10; }
        F.PC->ServerItemRequest(Request);
        const auto Record = FindRecord(F.PC->GetTrace(), TEXT("REQUEST_COMMIT"));
        if (Case == TEXT("CommitDisabled"))
        { TestFalse(TEXT("默认不记录逐请求压力Trace"), Record.IsValid()); return true; }
        if (!TestTrue(TEXT("可靠RPC包装路由结束后实际记录每请求完整提交"), Record.IsValid())) { return false; }
        const auto& View = Record->GetObjectField(TEXT("view"));
        TestEqual(TEXT("记录当前完整权威版本"), View->GetNumberField(TEXT("revision")), double(F.View().ViewRevision));
        TestEqual(TEXT("记录该请求序号"), View->GetNumberField(TEXT("seq")), double(Request.Seq));
        TestEqual(TEXT("失败也记录真实权威结果"), View->GetNumberField(TEXT("code")),
            double(Case == TEXT("CommitFailure") ? E::SequenceGap : E::None));
        TestEqual(TEXT("只有成功学习才实际删书"), Record->GetBoolField(TEXT("requestPresent")), Case == TEXT("CommitFailure"));
        return true;
    }
    auto* Probe = F.World->SpawnActor<AY01R36BurstControllerProbe>();
    auto* State = F.PC->GetPlayerState<AY01PlayerState>();
    Probe->PlayerState = State; State->SetOwner(Probe);
    auto* Local = NewObject<ULocalPlayer>(GEngine); Local->PlayerController = Probe; Probe->Player = Local;
    Probe->SetRole(Case == TEXT("Authority") ? ROLE_Authority : ROLE_AutonomousProxy);
    ON_SCOPE_EXIT { Probe->SetRole(ROLE_Authority); State->SetOwner(F.PC); };
    auto& Phase = *FindFProperty<FStructProperty>(F.Mode->GetFlowState()->GetClass(), TEXT("Snapshot"))
        ->ContainerPtrToValuePtr<FY01WaveSnapshot>(F.Mode->GetFlowState());
    if (Case == TEXT("Waiting")) { Phase.Phase = EY01MatchPhase::Waiting; }
    if (Case == TEXT("Ended")) { Probe->EndPlay(EEndPlayReason::Quit); }
    if (Case == TEXT("NoOwner")) { State->SetOwner(F.Other); }
    if (Case == TEXT("MissingView")) { Probe->PlayerState = F.World->SpawnActor<AY01PlayerState>(); Probe->PlayerState->SetOwner(Probe); }
    FCommandLine::Set(Case == TEXT("Disabled") ? TEXT("-R36Trace") : Case == TEXT("InvalidFlag")
        ? TEXT("-R36Trace -R36Burst=natural") : TEXT("-R36Trace -R36Burst=controlled"));
    Probe->Tick(.1f);
    if (Case == TEXT("Waiting"))
    {
        TestEqual(TEXT("Waiting不提前占用发送机会"), Probe->CapturedRequests.Num(), 0);
        Phase.Phase = EY01MatchPhase::Preparing; Probe->Tick(.1f);
    }
    const bool Enabled = Case == TEXT("ClientOnce") || Case == TEXT("Waiting");
    TestEqual(TEXT("仅显式本地客户端完整视图与允许阶段才调用八次RPC包装"), Probe->CapturedRequests.Num(), Enabled ? 8 : 0);
    TestEqual(TEXT("发送同次留下BURST_SENT"), FindRecord(Probe->GetTrace(), TEXT("BURST_SENT")).IsValid(), Enabled);
    if (Enabled && Probe->CapturedRequests.Num() == 8)
    {
        const auto Expected = Y01R36Trace::BuildBurst(Base, EY01MatchPhase::Preparing);
        for (int32 I = 0; I < 8; ++I)
        { TestTrue(TEXT("实际包装入口保持全部13字段"), FY01ItemRequest::StaticStruct()->CompareScriptStruct(&Expected[I], &Probe->CapturedRequests[I], 0)); }
        Probe->Tick(.1f); TestEqual(TEXT("下一Tick不能重发"), Probe->CapturedRequests.Num(), 8);
    }
    auto After = F.View();
    TestTrue(TEXT("截获包装入口不直接执行组件或乐观变更"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Base, &After, 0));
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36EconomyWidgetRouteTest, "Y01.Framework.R36.RuntimeIntegration.UIRoute",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36EconomyWidgetRouteTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Purchase"), TEXT("Talent"), TEXT("Direct"), TEXT("Synthesize"), TEXT("DeathRace"), TEXT("EndedRace"),
        TEXT("FundsRace"), TEXT("LimitRace"), TEXT("ForeignView"), TEXT("ForeignOwner"), TEXT("FullEquipmentSlots"), TEXT("MaterialStale")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36EconomyWidgetRouteTest::RunTest(const FString& Case)
{
    const FString Previous = FCommandLine::Get(); FCommandLine::Set(TEXT("")); ON_SCOPE_EXIT { FCommandLine::Set(*Previous); };
    FR36ControllerWorld F(true); if (!TestTrue(TEXT("真实经济路由夹具加载"), F.Ready)) { AddError(F.Error); return false; }
    F.World->SetGameInstance(nullptr); auto* Local = NewObject<ULocalPlayer>(GEngine); Local->PlayerController = F.PC; F.PC->Player = Local;
    auto* PS = F.PC->GetPlayerState<AY01PlayerState>(); auto* Other = F.Other->GetPlayerState<AY01PlayerState>();
    ON_SCOPE_EXIT { PS->SetOwner(F.PC); };
    TStrongObjectPtr<UY01R36Panel> Panel(CreateWidget<UY01R36Panel>(F.PC)); const auto Slate = Panel->TakeWidget();
    for (const auto* Name : {TEXT("ToggleBackpack"), TEXT("ToggleCharacter"), TEXT("ToggleShop")})
    {
        auto* Toggle = Cast<UButton>(Panel->WidgetTree->FindWidget(Name));
        if (!TestNotNull(TEXT("交互用例通过三个真实按钮独立展开"), Toggle)) { return false; }
        Toggle->OnClicked.Broadcast();
    }
    TestTrue(TEXT("三面板均可展开"), Panel->IsBackpackExpanded() && Panel->IsCharacterExpanded() && Panel->IsShopExpanded());
    auto Present = [&]() { Panel->Present(F.View(), EY01MatchPhase::Preparing, 11); };
    auto Click = [&](const TCHAR* Name)
    { auto* B = Cast<UButton>(Panel->WidgetTree->FindWidget(Name)); if (!TestNotNull(TEXT("实际程序按钮入口"), B)) { return false; } B->OnClicked.Broadcast(); return true; };
    if (Case == TEXT("FullEquipmentSlots"))
    { for (int32 I = 0; I < 6; ++I) { const auto Id = F.Add(); F.Send(F.Request(Id, I)); } }
    const bool Craft = Case == TEXT("Synthesize") || Case == TEXT("MaterialStale");
    if (Craft) { for (int32 I = 0; I < 5; ++I) { F.Add(); } }
    Present(); const auto Before = F.View(); FY01CommittedView OtherBefore; Other->GetLoadout()->ReadCommittedView(OtherBefore);
    if (Case == TEXT("ForeignView")) { Panel->Present(OtherBefore, EY01MatchPhase::Preparing, 11); }
    if (Case == TEXT("ForeignOwner")) { PS->SetOwner(F.Other); }
    if (Case == TEXT("DeathRace")) { PS->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    if (Case == TEXT("EndedRace"))
    {
        TestTrue(TEXT("显示后真实Ended"), F.Mode->TryEndMatch(false));
        FY01CommittedView EndedView; TestTrue(TEXT("全局结束后的其他owner完整视图"), Other->GetLoadout()->ReadCommittedView(EndedView));
        TestTrue(TEXT("全局Ended同步停止其他owner成长"), EndedView.Growth.bEnabled && EndedView.Growth.bEnded);
        TestTrue(TEXT("结束前其他owner交付已启用且未结束"), OtherBefore.Delivery.bEnabled && !OtherBefore.Delivery.bEnded);
        TestTrue(TEXT("全局Ended同步停止其他owner交付"), EndedView.Delivery.bEnabled && EndedView.Delivery.bEnded);
        auto ExpectedEnded = OtherBefore; ExpectedEnded.Growth.bEnded = true; ExpectedEnded.Delivery.bEnded = true;
        ExpectedEnded.ViewRevision = EndedView.ViewRevision; ExpectedEnded.SampleTime = EndedView.SampleTime;
        TestTrue(TEXT("全局结束仅追加成长与交付停止状态，不污染其他owner业务"),
            FY01CommittedView::StaticStruct()->CompareScriptStruct(&ExpectedEnded, &EndedView, 0));
        // 后续按钮请求仍须与结束后的完整快照逐字段相等，不能把全局生命周期误作跨owner污染。
        OtherBefore = MoveTemp(EndedView);
    }
    if (Case == TEXT("FundsRace") || Case == TEXT("LimitRace"))
    {
        for (int32 I = 0; I < (Case == TEXT("FundsRace") ? 20 : 1); ++I)
        {
            const auto V = F.View(); FY01ItemRequest R; R.MatchId = V.MatchId; R.JoinSequence = V.JoinSequence;
            R.ConfigRevision = V.ConfigRevision; R.Seq = V.ExpectedSeq; R.Operation = O::Purchase;
            R.Offer = F.Data.Ref(K::ShopOffer, Case == TEXT("FundsRace") ? TEXT("Default") : TEXT("Direct")); F.Send(R);
            TestEqual(TEXT("竞态夹具先提交合法支出"), F.View().Response.Code, E::None);
        }
        auto Stale = Before; Stale.ExpectedSeq = F.View().ExpectedSeq; ++Stale.ViewRevision;
        Panel->Present(Stale, EY01MatchPhase::Preparing, 11);
    }
    if (Craft)
    {
        // 一键合成：客户端按最低可合品质自动选包内五件，不再手动选材。
        TArray<FY01ItemConsume> Picked; FName Recipe;
        TestTrue(TEXT("自动选出五件真实材料"), Panel->GetModel().PickMaterials(Picked, Recipe));
        TestEqual(TEXT("合成前自动选择五件真实材料"), Picked.Num(), 5);
        if (Case == TEXT("MaterialStale")) { F.Send(F.Request(Before.Items[0].ItemId)); }
        Click(TEXT("Synthesize"));
    }
    else { Click(Case == TEXT("Talent") ? TEXT("Talent_0") : Case == TEXT("Direct") || Case == TEXT("LimitRace") ? TEXT("Purchase_1") : TEXT("Purchase_0")); }
    if (Case.StartsWith(TEXT("Foreign")))
    {
        TestFalse(TEXT("外来完整视图或owner禁止发送及假等待"), Panel->GetModel().IsPending());
        TestEqual(TEXT("错误owner不污染本人响应账本"), F.View().ResponseSerial, Before.ResponseSerial); return true;
    }
    TestTrue(TEXT("等完整回执前保留pending"), Panel->GetModel().IsPending());
    TestEqual(TEXT("按钮不乐观耗材或发奖"), Panel->GetModel().View().Items.Num(), Before.Items.Num());
    Present(); const auto After = F.View(); TestFalse(TEXT("精确权威回执解除pending"), Panel->GetModel().IsPending());
    const E Expected = Case == TEXT("DeathRace") ? E::Dead : Case == TEXT("EndedRace") ? E::Phase : Case == TEXT("FundsRace") ? E::InsufficientFunds
        : Case == TEXT("LimitRace") ? E::PurchaseLimit : Case == TEXT("MaterialStale") ? E::ExpiredSequence : E::None;
    if (Case == TEXT("MaterialStale")) { TestFalse(TEXT("材料版本或序号竞态拒绝而非合成"), After.Response.Code == E::None); }
    else { TestEqual(TEXT("实际服务器成功或明确拒绝"), After.Response.Code, Expected); }
    if (Expected == E::None)
    {
        const int32 Delta = Craft ? -4 : Case == TEXT("Talent") || Case == TEXT("Direct") ? 0 : 1;
        TestEqual(TEXT("只有权威提交后才更新包"), After.Items.Num(), Before.Items.Num() + Delta);
        TestEqual(TEXT("免费合成天赋与购买价格"), After.Gold, Before.Gold - (Craft || Case == TEXT("Talent") ? 0 : Case == TEXT("Direct") ? 50 : 10));
        TestEqual(TEXT("买入或合成不自动替换槽"), After.EquipmentSlots, Before.EquipmentSlots);
        if (Case != TEXT("Talent"))
        {
            const auto& Log = Panel->GetModel().GameLog();
            const FString Last = Log.IsEmpty() ? FString() : Log.Last().ToString();
            TestTrue(TEXT("权威成功后游戏内LOG记录真实获得"), Craft ? Last.StartsWith(TEXT("消耗：")) && Last.Contains(TEXT("；合成：")) : Last.StartsWith(TEXT("购买：")));
        }
    }
    else if (Panel->GetModel().GameLog().Num())
    { TestTrue(TEXT("失败只记灰色原因"), Panel->GetModel().GameLog().Last().bFailure); }
    FY01CommittedView OtherAfter; Other->GetLoadout()->ReadCommittedView(OtherAfter);
    TestTrue(TEXT("其他owner完整视图不变"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&OtherBefore, &OtherAfter, 0));
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01HUD08RouteTest, "Y01.Framework.R36.UI.HUD08Route",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01HUD08RouteTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("SkillClicks"), TEXT("HiddenSource"), TEXT("HiddenTarget"), TEXT("HiddenRoot"), TEXT("RemoveCollapsed")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01HUD08RouteTest::RunTest(const FString& Case)
{
    FR36ControllerWorld F; if (!TestTrue(TEXT("真实路由夹具就绪"), F.Ready)) { return false; }
    const bool SkillCase = Case == TEXT("SkillClicks") || Case == TEXT("RemoveCollapsed");
    const auto Id = F.Add(SkillCase ? K::Skill : K::Equipment);
    if (SkillCase) { F.Send(F.Request(Id, 0, O::PrepareLearn)); }
    else if (Case != TEXT("HiddenSource")) { F.Send(F.Request(Id)); }
    F.World->SetGameInstance(nullptr);
    auto* Probe = F.World->SpawnActor<AY01R36BurstControllerProbe>(); auto* State = F.PC->GetPlayerState<AY01PlayerState>();
    Probe->PlayerState = State; State->SetOwner(Probe);
    auto* Local = NewObject<ULocalPlayer>(GEngine); Local->PlayerController = Probe; Probe->Player = Local;
    ON_SCOPE_EXIT { State->SetOwner(F.PC); };
    TStrongObjectPtr<UY01R36Panel> P(CreateWidget<UY01R36Panel>(Probe)); const auto Slate = P->TakeWidget();
    auto V = F.View();
    if (Case == TEXT("SkillClicks"))
    {
        for (int32 I = 1; I < 4; ++I) { V.Skills[I] = V.Skills[0]; V.Skills[I].Slot = I; V.Skills[I].LearnedId = FGuid::NewGuid(); }
    }
    P->Present(V, EY01MatchPhase::Battle, 11);
    const FPointerEvent Left(0, FVector2D(20), FVector2D(20), TSet<FKey>{EKeys::LeftMouseButton}, EKeys::LeftMouseButton, 0, FModifierKeysState());
    if (Case == TEXT("SkillClicks"))
    {
        for (int32 I = 0; I < 4; ++I)
        { TestTrue(TEXT("三面板收起仍可点击技能"), P->GetCell(EY01R36Zone::Skill, I)->NativeOnMouseButtonDown(FGeometry(), Left).IsEventHandled()); }
        TestEqual(TEXT("仅转发现有Action0至3，不声称施法成功"), Probe->CapturedActions, TArray<int32>({0,1,2,3}));
        TestFalse(TEXT("技能点击不占物品pending"), P->GetModel().IsPending());
        TestTrue(TEXT("技能点击不发物品交易"), Probe->CapturedRequests.IsEmpty()); return true;
    }
    if (Case == TEXT("RemoveCollapsed"))
    {
        const FPointerEvent Right(0, FVector2D(20), FVector2D(20), TSet<FKey>{EKeys::RightMouseButton}, EKeys::RightMouseButton, 0, FModifierKeysState());
        TestTrue(TEXT("收起仍保留右键移除入口"), P->GetCell(EY01R36Zone::Skill, 0)->NativeOnMouseButtonDown(FGeometry(), Right).IsEventHandled());
        if (TestEqual(TEXT("只有一个物品意图"), Probe->CapturedRequests.Num(), 1))
        { TestEqual(TEXT("原移除协议"), Probe->CapturedRequests[0].Operation, O::RemoveSkill); }
        return true;
    }
    P->ToggleBackpack();
    const auto Zone = Case == TEXT("HiddenSource") ? EY01R36Zone::Backpack : EY01R36Zone::Equipment;
    TStrongObjectPtr<UY01R36Cell> Source(CreateWidget<UY01R36Cell>(F.World)); const auto SourceSlate = Source->TakeWidget();
    Source->Configure(P.Get(), Zone, Case == TEXT("HiddenSource") ? INDEX_NONE : 0, Id);
    UDragDropOperation* Raw = nullptr; Source->NativeOnDragDetected(FGeometry(), Left, Raw);
    TStrongObjectPtr<UY01R36DragOperation> Drag(Cast<UY01R36DragOperation>(Raw));
    if (!TestNotNull(TEXT("先捕获真实可见来源"), Drag.Get())) { return false; }
    auto Window = SNew(SWindow).ClientSize(FVector2D(1280,720)); Window->SetContent(Slate);
    FHittestGrid Grid; Grid.SetHittestArea(FVector2D::ZeroVector, FVector2D(1280,720));
    const auto Host = FGeometry::MakeRoot(FVector2D(1280,720), FSlateLayoutTransform());
    for (int32 I = 0; I < 3; ++I)
    {
        Slate->SlatePrepass(1); FSlateWindowElementList Elements(Window);
        FPaintArgs Args(&Window.Get(), Grid, FVector2D::ZeroVector, 20 + I * .016, .016f);
        Slate->Paint(Args, Host, FSlateRect(0,0,1280,720), Elements, 0, FWidgetStyle(), true);
    }
    const auto BagGeometry = P->GetBackpack()->GetCachedGeometry();
    TestTrue(TEXT("拥有真实旧缓存几何"), BagGeometry.GetLocalSize().X > 0);
    const FVector2D At = BagGeometry.LocalToAbsolute(BagGeometry.GetLocalSize() * .5);
    const FPointerEvent Pointer(0, At, At, TSet<FKey>(), EKeys::LeftMouseButton, 0, FModifierKeysState());
    const auto SlateDrag = FUMGDragDropOp::New(Drag.Get(), Pointer.GetPointerIndex(), At, At, 1.f, StaticCastSharedRef<SObjectWidget>(Slate));
    const FDragDropEvent DropEvent(Pointer, SlateDrag);
    P->ToggleBackpack();
    if (Case == TEXT("HiddenSource"))
    {
        TestFalse(TEXT("隐藏来源旧payload不能投常驻槽"), P->GetCell(EY01R36Zone::Equipment, 0)->NativeOnDrop(Host, FDragDropEvent(Pointer, nullptr), Drag.Get()));
        Source->NativeOnDragDetected(Host, Left, Raw); TestNull(TEXT("隐藏源不能重新捕获"), Raw);
    }
    else
    {
        TestFalse(TEXT("隐藏背包拒绝target"), P->Accept(Drag.Get(), EY01R36Zone::Backpack, INDEX_NONE));
        Source->Configure(P.Get(), EY01R36Zone::Backpack, INDEX_NONE, Id);
        TestFalse(TEXT("背包格drop也有可见性保护"), Source->NativeOnDrop(Host, FDragDropEvent(Pointer, nullptr), Drag.Get()));
        TestFalse(TEXT("root不能用旧几何吃掉drop"), Slate->OnDrop(Host, DropEvent).IsEventHandled());
    }
    TestTrue(TEXT("隐藏路径零交易"), Probe->CapturedRequests.IsEmpty());
    P->ToggleBackpack();
    const bool Sent = Case == TEXT("HiddenRoot") ? Slate->OnDrop(Host, DropEvent).IsEventHandled()
        : P->Accept(Drag.Get(), Case == TEXT("HiddenSource") ? EY01R36Zone::Equipment : EY01R36Zone::Backpack, 0);
    TestTrue(TEXT("重新打开原身份仍可发送"), Sent);
    TestEqual(TEXT("只发送一次合法意图"), Probe->CapturedRequests.Num(), 1); return true;
}
#endif
