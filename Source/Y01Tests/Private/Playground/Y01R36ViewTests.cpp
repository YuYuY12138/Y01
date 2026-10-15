#include "Misc/AutomationTest.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01EconomyComponent.h"
#include "Player/Y01PlayerController.h"
#include "Player/Y01Character.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Y01MatchFixture.h"
#include "Match/Y01GameFlow.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "Net/RepLayout.h"
#include "Engine/PackageMapClient.h"
#include "IpNetDriver.h"
#include "IpConnection.h"
#include "Engine/ActorChannel.h"
#include "Net/DataReplication.h"
#include "UObject/UnrealType.h"
#include "Misc/ScopeExit.h"
#include "UObject/GarbageCollection.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Playground/Y01R36Verification.h"
#include "Misc/CommandLine.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
using K = EY01TableKind;
struct FViewWorld
{
    FY01MatchFixture Data;
    UWorld* World;
    AY01GameMode* Mode;
    AY01PlayerController* PC;
    AY01PlayerState* P;
    FGuid Match = FGuid::NewGuid();
    FString Error;
    FViewWorld()
    {
        auto& C = Data.Row<FY01CharacterRow>(K::Character);
        C.InitialEquipment.Reset(); C.InitialSkills.Reset(); C.AttackDamage = 30;
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01GameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01GameMode>(); PC = Player(); P = PC->GetPlayerState<AY01PlayerState>();
        Mode->TryConfigureMatch(FY01DataCatalog(Data.Tables), Data.Waves(), FY01MatchFixture::Ref(K::Character), Error);
        FY01SourceEffectRow E; E.Kind = EY01SourceEffectKind::AttackFlat; E.Magnitude = 10; Data.Add(K::SourceEffect, E);
        FY01EquipmentRow I; I.DisplayName = FText::FromString(TEXT("真实装备名称")); I.Quality = EY01Quality::Purple;
        I.Icon = FSoftObjectPath(TEXT("/Game/Display/Equipment.Equipment")); I.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect)};
        Data.Add(K::Equipment, I);
        FY01SkillRow S; S.DisplayName = FText::FromString(TEXT("真实技能名称")); S.Type = EY01SkillType::Active;
        S.Ability = UY01BasicAttackAbility::StaticClass(); S.CooldownSeconds = 8; S.EffectIds = I.EffectIds;
        Data.Add(K::Skill, S); Data.Add(K::Skill, S, TEXT("Other"));
    }
    ~FViewWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    AY01PlayerController* Player()
    {
        auto* C = World->SpawnActor<AY01PlayerController>(); auto* S = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(0, 0, 100), FRotator::ZeroRotator);
        S->SetOwner(C); C->PlayerState = S; C->Possess(Pawn); S->GetCombatASC()->InitAbilityActorInfo(S, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(S); return C;
    }
    UY01LoadoutComponent* L() const { return P->GetLoadout(); }
    FY01CommittedView View() const { FY01CommittedView V; L()->ReadCommittedView(V); return V; }
    FY01ItemGrant Grant(K Type = K::Equipment, FName Name = TEXT("Default")) const
    { FY01ItemGrant G; G.ItemId = FGuid::NewGuid(); G.Definition = FY01MatchFixture::Ref(Type, Name); return G; }
    FY01ItemResult Add(const TArray<FY01ItemGrant>& G) { return L()->AddItems(FY01DataCatalog(Data.Tables), G); }
    FY01ItemRequest Request(FGuid Id, int32 Slot, EY01ItemOperation Op = EY01ItemOperation::Equip) const
    {
        FY01LoadoutSnapshot V; L()->Observe(V); FY01ItemInstance I; L()->FindItem(Id, I);
        FY01ItemRequest R; R.MatchId = Match; R.JoinSequence = 1; R.Seq = V.ExpectedSeq;
        R.ItemId = Id; R.ExpectedEntityRevision = I.EntityRevision; R.Slot = Slot; R.Operation = Op; R.ConfigRevision = V.ConfigRevision;
        if (Op == EY01ItemOperation::Equip || Op == EY01ItemOperation::Unequip)
        {
            R.ExpectedSlotItemId = V.EquipmentSlots[Slot]; FY01ItemInstance Old; L()->FindItem(R.ExpectedSlotItemId, Old);
            R.ExpectedSlotEntityRevision = R.ExpectedSlotItemId.IsValid() ? Old.EntityRevision : 0;
        }
        else { R.ExpectedLearnedId = V.Skills[Slot].LearnedId; R.ExpectedLearnedRevision = V.Skills[Slot].EntityRevision; }
        return R;
    }
};
// 网络夹具只经引擎原生struct ops、FastArray和FRepLayout位编码，未注入库存权威状态。
struct FWireCB : INetSerializeCB
{
    void NetSerializeStruct(FNetDeltaSerializeInfo& P) override
    {
        bool Unmapped = false; auto* S = CastChecked<UScriptStruct>(P.Struct);
        FBitArchive& Ar = P.Writer ? static_cast<FBitArchive&>(*P.Writer) : static_cast<FBitArchive&>(*P.Reader);
        FRepLayout::CreateFromStruct(S)->SerializePropertiesForStruct(S, Ar, P.Map, P.Data, Unmapped);
        P.bOutHasMoreUnmapped = Unmapped;
    }
    void GatherGuidReferencesForFastArray(FFastArrayDeltaSerializeParams&) override {}
    bool MoveGuidToUnmappedForFastArray(FFastArrayDeltaSerializeParams&) override { return false; }
    void UpdateUnmappedGuidsForFastArray(FFastArrayDeltaSerializeParams&) override {}
    bool NetDeltaSerializeForFastArray(FFastArrayDeltaSerializeParams&) override { return false; }
};
struct FWireRoute
{
    FWireCB CB;
    TSharedPtr<INetDeltaBaseState> Base;
    FY01ViewWire Client;
    TStrongObjectPtr<UPackageMapClient> Map{NewObject<UPackageMapClient>()};
    int64 LastBits = 0;
    bool Send(FY01ViewWire& Server, bool Drop = false, UObject* Receiver = nullptr)
    {
        FNetBitWriter Writer(Map.Get(), 0); TSharedPtr<INetDeltaBaseState> Next;
        FNetDeltaSerializeInfo P; P.Writer = &Writer; P.OldState = Base.Get(); P.NewState = &Next;
        P.Struct = FY01ViewWire::StaticStruct(); P.NetSerializeCB = &CB; P.Map = Map.Get();
        auto* Ops = FY01ViewWire::StaticStruct()->GetCppStructOps();
        if (!Ops->NetDeltaSerialize(P, &Server) || Writer.IsError()) { return false; }
        LastBits = Writer.GetNumBits();
        if (Drop) { return true; }
        FNetBitReader Reader(Map.Get(), Writer.GetData(), Writer.GetNumBits());
        FNetDeltaSerializeInfo Q; Q.Reader = &Reader; Q.Struct = FY01ViewWire::StaticStruct(); Q.NetSerializeCB = &CB; Q.Object = Receiver; Q.Map = Map.Get();
        auto* Destination = Receiver ? FindFProperty<FStructProperty>(Receiver->GetClass(), TEXT("ViewWire"))->ContainerPtrToValuePtr<FY01ViewWire>(Receiver) : &Client;
        const bool Ok = Ops->NetDeltaSerialize(Q, Destination) && !Reader.IsError();
        if (Ok) { Base = Next; } return Ok;
    }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36ViewTest, "Y01.Framework.R36.View",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36ViewTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("DefaultDisabled"), TEXT("InitialCommit"), TEXT("DisplayOwned"), TEXT("EquipmentAtomic"),
        TEXT("BusyObservation"), TEXT("FailedMutation"), TEXT("LearnAtomic"), TEXT("CooldownCommit"), TEXT("TicketSurvivesCD"),
        TEXT("ConfigCommit"), TEXT("TrustedPublish"), TEXT("Receipt64"), TEXT("OwnerOnlyRoute"), TEXT("NoAuthorityObjects"),
        TEXT("Wire1000"), TEXT("Wire10000"), TEXT("WireLostWindow"), TEXT("WireConcurrent"), TEXT("WireDeltaDelete"),
        TEXT("WireAckScope"), TEXT("WireDuplicate"), TEXT("WireReorder"), TEXT("WireCorrupt"), TEXT("WireCrossSession"),
        TEXT("ComponentReceive"), TEXT("ComponentAck"), TEXT("ComponentAckOwner"), TEXT("ComponentOwnerChange"), TEXT("SourceMismatch"),
        TEXT("WireTombstone"), TEXT("WireRapidReplace"), TEXT("WirePartial"), TEXT("WireCrossPending"), TEXT("WireResync"), TEXT("WireStaleWindow"), TEXT("WireRebind"), TEXT("WireResetSession"),
        TEXT("EngineOwnerOnly"), TEXT("EngineReceive"), TEXT("EngineReceive10000"), TEXT("EngineLostWindow"),
        TEXT("Codec1000"), TEXT("Codec10000"), TEXT("CodecFields"), TEXT("PurchaseAtomic"),
                TEXT("HeaderGold"), TEXT("HeaderCatalog"), TEXT("HeaderArrays"), TEXT("HeaderMerge"), TEXT("HeaderResync"), TEXT("HeaderFailure")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36ViewTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    ON_SCOPE_EXIT
    {
        if (Case.StartsWith(TEXT("Engine")))
        {
            // 世界和复制器先退出，再收集通道，避免把生命周期缺口留给后续用例。
            CollectGarbage(RF_NoFlags);
            AddInfo(TEXT("引擎夹具退出后的GC已完成"));
        }
    };
    FViewWorld F;
    auto Ok = [&](const TCHAR* M, const FY01ItemResult& R) { if (!R.IsOk()) { AddInfo(R.Message); } return TestTrue(M, R.IsOk()); };
    if (Case == TEXT("DefaultDisabled"))
    { FY01CommittedView V; TestFalse(TEXT("默认不发布新视图"), F.L()->ReadCommittedView(V)); TestFalse(TEXT("默认受信发布仍拒绝"), F.L()->PublishCommittedView().IsOk()); return true; }
    if (!Ok(TEXT("初始化真实注册表"), F.L()->Initialize(F.Match, 1))) { return false; }
    if (Case == TEXT("PurchaseAtomic"))
    {
        auto& Pool = F.Data.Row<FY01LocalItemPoolRow>(K::LocalItemPool); Pool.Quality = EY01Quality::Purple;
        auto& Offer = F.Data.Row<FY01ShopOfferRow>(K::ShopOffer); Offer.Quality = Pool.Quality; Offer.Price = 100; Offer.Quantity = 3;
        const auto Ref = FY01MatchFixture::Ref(K::ShopOffer);
        if (!Ok(TEXT("真实商品快照初始化"), F.P->GetEconomy()->Initialize(F.Match, 1, 1, FY01DataCatalog(F.Data.Tables), {Ref}))) { return false; }
        F.Mode->TryStartNextWave(); F.P->TryAddMatchReward(500, 0); F.L()->PublishCommittedView();
        FY01ViewSender Sender; FY01ViewReceiver Receiver; FY01ViewWire Wire; FWireRoute Route; FString Error;
        auto Transfer = [&](const FY01CommittedView& Target)
        {
            const auto Old = Receiver.Get();
            if (!TestTrue(TEXT("完整业务提交生成"), Sender.Offer(Target, Wire, Error))) { return false; }
            int32 Windows = 0;
            while (!Wire.Items.IsEmpty() && ++Windows < 100)
            {
                if (!TestTrue(TEXT("原生FastArray位编码路由"), Route.Send(Wire))) { return false; }
                FY01ViewAck Ack;
                if (!TestTrue(TEXT("生产接收器验证购买清单"), Receiver.Receive(Route.Client, Ack, Error))) { AddInfo(Error); return false; }
                if (Receiver.Get().ViewRevision != Target.ViewRevision)
                { TestTrue(TEXT("未齐窗口钱货回执仍为原整份视图"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Old, &Receiver.Get(), 0)); }
                if (!TestTrue(TEXT("原确认推进"), Sender.Acknowledge(Ack, Wire, Error))) { return false; }
            }
            return TestTrue(TEXT("客户端钱货回执完整相等"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Target, &Receiver.Get(), 0));
        };
        if (!Transfer(F.View())) { return false; }
        FY01ItemRequest Q; Q.MatchId = F.Match; Q.JoinSequence = 1; Q.Seq = 1; Q.ConfigRevision = 1;
        Q.Operation = EY01ItemOperation::Purchase; Q.Offer = Ref;
        const auto Result = F.L()->SubmitRequest(F.PC, Q); if (!Ok(TEXT("真实统一购买入口"), Result)) { return false; }
        TestEqual(TEXT("权威同次钱"), F.View().Gold, 400); TestEqual(TEXT("权威同次货"), F.View().Items.Num(), 3);
        if (!Transfer(F.View())) { return false; }
        TestEqual(TEXT("客户端余额400"), Receiver.Get().Gold, 400); TestEqual(TEXT("客户端三件包内"), Receiver.Get().Items.Num(), 3);
        TestEqual(TEXT("客户端商品意图"), Receiver.Get().Response.Request.Offer.RowName, Ref.RowName);
        TestEqual(TEXT("客户端原成功回执"), Receiver.Get().Receipts[0].Code, EY01ItemError::None);
        const auto Replay = F.L()->SubmitRequest(F.PC, Q);
        TestTrue(TEXT("重放完整原业务结果"), FY01ItemResult::StaticStruct()->CompareScriptStruct(&Result, &Replay, 0));
        return Transfer(F.View());
    }
    if (Case == TEXT("OwnerOnlyRoute"))
    {
        F.L()->GetClass()->SetUpRuntimeReplicationData(); TArray<FLifetimeProperty> Props; F.L()->GetLifetimeReplicatedProps(Props);
        auto* P = FindFProperty<FStructProperty>(F.L()->GetClass(), TEXT("ViewWire"));
        TestTrue(TEXT("真实组件开启复制"), F.L()->GetIsReplicated());
        TestTrue(TEXT("仅OwnerOnly注册FastArray"), P && P->Struct->GetCppStructOps()->HasNetDeltaSerializer()
            && Props.ContainsByPredicate([&](const auto& V) { return V.RepIndex == P->RepIndex && V.Condition == COND_OwnerOnly; }));
        auto* Ack = F.L()->FindFunction(TEXT("ServerAckView"));
        TestTrue(TEXT("推进走真实可靠服务器RPC"), Ack && Ack->HasAllFunctionFlags(FUNC_Net | FUNC_NetServer | FUNC_NetReliable)); return true;
    }
    if (Case == TEXT("NoAuthorityObjects"))
    {
        for (auto* S : {FY01ViewItem::StaticStruct(), FY01ViewSkill::StaticStruct(), FY01ViewDisplay::StaticStruct(), FY01ViewReceipt::StaticStruct()})
        { for (TFieldIterator<FProperty> I(S); I; ++I) { TestFalse(TEXT("视图无对象权限字段"), I->IsA<FObjectPropertyBase>() || I->GetFName() == TEXT("GAHandle") || I->GetFName() == TEXT("Ability")); } }
        return true;
    }
    if (!TestTrue(TEXT("初始化即有完整业务视图"), F.View().ViewRevision > 0)) { return false; }
    if (Case == TEXT("InitialCommit"))
    {
        const auto V = F.View(); TestEqual(TEXT("会话身份"), V.MatchId, F.Match); TestEqual(TEXT("连接序号"), V.JoinSequence, 1);
        TestTrue(TEXT("独立连接代次"), V.ConnectionId.IsValid()); TestEqual(TEXT("六槽"), V.EquipmentSlots.Num(), 6); TestEqual(TEXT("四槽"), V.Skills.Num(), 4);
        TestEqual(TEXT("余额同次捕获"), V.Gold, F.P->GetGold()); TestEqual(TEXT("攻击同次捕获"), V.Stats.AttackDamage, 30.f); return true;
    }
    auto A = F.Grant(), B = F.Grant();
    if (Case.Contains(TEXT("Learn")) || Case.Contains(TEXT("Cooldown")) || Case == TEXT("TicketSurvivesCD"))
    { A = F.Grant(K::Skill); B = F.Grant(K::Skill, TEXT("Other")); }
    TArray<FY01ItemGrant> Grants{A, B};
    if (Case.StartsWith(TEXT("Wire")) || Case.StartsWith(TEXT("Codec")) || (Case.StartsWith(TEXT("Engine")) && Case != TEXT("EngineOwnerOnly")))
    { const int32 Count = Case.EndsWith(TEXT("10000")) ? 10000 : 1000; for (int32 I = 2; I < Count; ++I) { Grants.Add(F.Grant()); } }
    if (!Ok(TEXT("批量入包"), F.Add(Grants))) { return false; }
    if (!TestEqual(TEXT("服务器已提交视图必须包含本批全部实例"), F.View().Items.Num(), Grants.Num())) { return false; }
    F.Mode->TryStartNextWave();
    if (Case.StartsWith(TEXT("Header")))
    {
        auto Initial = F.View();
        for (int32 I = 0; I < 20; ++I)
        {
            auto& O = Initial.ShopOffers.AddDefaulted_GetRef(); O.OfferId = FName(*FString::Printf(TEXT("Offer%d"), I));
            O.DisplayName = FText::FromString(FString::ChrN(160, TEXT('X'))); O.Price = I + 1;
        }
        auto& Skill = Initial.Skills[0]; Skill.LearnedId = FGuid::NewGuid(); Skill.SkillId = TEXT("HeaderSkill");
        Skill.Slot = 0; Skill.EntityRevision = 1; Skill.Display.Name = FText::FromString(FString::ChrN(160, TEXT('S')));
        auto& Recipe = Initial.SynthesisRecipes.AddDefaulted_GetRef(); Recipe.RecipeId = TEXT("Recipe");
        Recipe.Recipe.InputType = K::Equipment; Recipe.Recipe.InputQuality = EY01Quality::Green;
        Recipe.Recipe.InputCount = 5; Recipe.Recipe.OutputPool = FY01MatchFixture::Ref(K::LocalItemPool);
        for (int32 I = 0; I < 64; ++I)
        {
            auto& R = Initial.Receipts.AddDefaulted_GetRef(); R.Request = F.Request(A.ItemId, 0); R.Request.Seq = I + 1;
            R.Message = TEXT("旧回执不应反复发送"); R.Revision = Initial.Revision;
        }
        Initial.ExpectedSeq = 65;
        FY01ViewSender Sender; FY01ViewReceiver Receiver; FY01ViewWire Wire; FString Error;
        auto Equal = [&](const FY01CommittedView& V)
        { return TestTrue(TEXT("完整发布逐字段相等"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&V, &Receiver.Get(), 0)); };
        auto Drain = [&]()
        {
            int32 Windows = 0;
            while (!Wire.Items.IsEmpty() && ++Windows <= 100)
            {
                FY01ViewAck Ack;
                if (!Receiver.Receive(Wire, Ack, Error) || !Sender.Acknowledge(Ack, Wire, Error)) { AddError(Error); return false; }
            }
            return TestTrue(TEXT("普通短场窗口有界结束"), Wire.Items.IsEmpty());
        };
        if (!TestTrue(TEXT("首次完整发送"), Sender.Offer(Initial, Wire, Error))) { AddInfo(Error); return false; }
        const int32 FullBytes = Wire.Items[0].TotalBytes;
        TestTrue(TEXT("首次静态目录确实是多窗"), FullBytes > 4096);
        if (!Drain() || !Equal(Initial)) { return false; }
        auto Next = Initial; ++Next.ViewRevision; ++Next.Gold; Next.SampleTime += .1;
        if (Case == TEXT("HeaderCatalog"))
        {
            Next.ShopOffers.RemoveAt(1); Next.ShopOffers[0].Price += 7;
            auto Added = Next.ShopOffers[0]; Added.OfferId = TEXT("AddedOffer"); Next.ShopOffers.Add(Added);
            Swap(Next.ShopOffers[0], Next.ShopOffers[2]); ++Next.ConfigRevision;
            Next.SynthesisRecipes[0].Recipe.OutputPool.RowName = TEXT("ChangedPool");
        }
        if (Case == TEXT("HeaderArrays"))
        { Next.Skills[0].Deadline = 123; Next.Tickets[0] = F.Request(A.ItemId, 0); Next.Tickets[0].TicketId = FGuid::NewGuid(); }
        if (Case == TEXT("HeaderFailure"))
        {
            Next.Receipts.RemoveAt(0); auto R = Initial.Receipts.Last(); R.Request.Seq = 65;
            R.Code = EY01ItemError::StaleEntity; R.Message = TEXT("真实失败回执显示，不制造成功");
            Next.Receipts.Add(R); Next.Response = R; ++Next.ResponseSerial; ++Next.ExpectedSeq;
        }
        if (!TestTrue(TEXT("相对已确认基线发送差量"), Sender.Offer(Next, Wire, Error))) { AddInfo(Error); return false; }
        const int32 DeltaBytes = Wire.Items[0].TotalBytes;
        TestTrue(TEXT("普通Header变更无需重复完整目录回执"), DeltaBytes < 4096);
        if (Case == TEXT("HeaderGold")) { TestTrue(TEXT("仅金币及时间版本小于512字节"), DeltaBytes < 512); }
        if (Case == TEXT("HeaderResync"))
        {
            auto Scope = Wire.Items[0].Scope, Bad = Scope; ++Bad.WindowSequence;
            TestFalse(TEXT("错ACK不得推进已确认基线"), Sender.Acknowledge(Bad, Wire, Error));
            TestFalse(TEXT("错重同步不得清基线"), Sender.Restart(Bad, Wire, Error));
            Receiver.Reset(); TestTrue(TEXT("正确身份重同步"), Sender.Restart(Scope, Wire, Error));
            TestTrue(TEXT("重同步恢复完整静态内容"), Wire.Items[0].TotalBytes >= FullBytes);
        }
        if (Case == TEXT("HeaderMerge"))
        {
            auto Latest = Next; ++Latest.ViewRevision; Latest.ShopOffers.Reset();
            TestTrue(TEXT("在途保留，最新清目录合并"), Sender.Offer(Latest, Wire, Error));
            ++Latest.ViewRevision; ++Latest.Gold; Latest.Skills[0] = {}; Latest.Receipts.Reset();
            TestTrue(TEXT("再一次合并只保留真正最新"), Sender.Offer(Latest, Wire, Error));
            TestEqual(TEXT("仍然发送冻结版本"), Wire.Items[0].Scope.ViewRevision, Next.ViewRevision);
            if (!Drain()) { return false; } return Equal(Latest);
        }
        if (!Drain() || !Equal(Next)) { return false; }
        if (Case == TEXT("HeaderArrays") || Case == TEXT("HeaderCatalog"))
        {
            ++Next.ViewRevision; Next.ShopOffers.Reset(); Next.SynthesisRecipes.Reset(); Next.Receipts.Reset();
            Next.Skills[0] = {}; Next.Tickets[0] = {};
            if (!TestTrue(TEXT("清空不是未变化"), Sender.Offer(Next, Wire, Error))) { return false; }
            if (!Drain()) { return false; } return Equal(Next);
        }
        AddInfo(FString::Printf(TEXT("Header FullBytes=%d DeltaBytes=%d"), FullBytes, DeltaBytes)); return true;
    }
    if (Case.StartsWith(TEXT("Codec")))
    {
        auto Initial = F.View();
        if (Case == TEXT("CodecFields"))
        {
            // 仅丰富传输值覆盖，不声称执行S3成长或跨进程业务。
            Initial.Gold = 137; Initial.Experience = 29; Initial.IncomeRate = 2.25;
            Initial.Items[0].Location = EY01ItemLocation::EquipmentSlot; Initial.Items[0].Slot = 0;
            Initial.EquipmentSlots[0] = Initial.Items[0].ItemId;
            FY01ItemGrowth Growth; Growth.RuleId = TEXT("传输成长快照"); Growth.Delta = .125;
            Growth.AppliedCount = 13; Growth.Accumulated = 1.625; Initial.Items[0].Growth.Add(Growth);
            auto& Skill = Initial.Skills[0]; Skill.LearnedId = FGuid::NewGuid(); Skill.SkillId = TEXT("传输技能");
            Skill.Slot = 0; Skill.EntityRevision = 7; Skill.Deadline = 12345.625; Skill.Display = Initial.Items[0].Display;
            Skill.Display.SkillType = EY01SkillType::Active; Skill.Display.CooldownSeconds = 60;
            Skill.Effects = Initial.Items[0].Effects; Skill.Growth.Add(Growth);
            auto Request = F.Request(B.ItemId, 0, EY01ItemOperation::ConfirmLearn);
            Request.ExpectedLearnedId = Skill.LearnedId; Request.ExpectedLearnedRevision = Skill.EntityRevision;
            Request.TicketId = FGuid::NewGuid(); Initial.Tickets[0] = Request;
            auto& Receipt = Initial.Receipts.AddDefaulted_GetRef(); Receipt.Request = Request;
            Receipt.Code = EY01ItemError::StaleTicket; Receipt.Message = TEXT("编码往返：取消不耗书");
            Receipt.Revision = Initial.Revision; Receipt.TicketId = Request.TicketId;
            Receipt.LearnedId = Skill.LearnedId; Receipt.RemovedLearnedId = FGuid::NewGuid();
            Initial.Response = Receipt; Initial.ResponseSerial = 9;
        }
        FY01ViewCommit Expected; Expected.Header = Initial; Expected.Header.Items.Reset(); Expected.ChangedItems = Initial.Items;
        for (const auto& Item : Initial.Items)
        {
            auto& M = Expected.Manifest.AddDefaulted_GetRef(); M.ItemId = Item.ItemId;
            M.EntityRevision = Item.EntityRevision; M.LastChangedRevision = Item.LastChangedRevision;
        }
        auto Encode = [&](bool Binary, TArray<uint8>& Bytes, double& Milliseconds)
        {
            const double Start = FPlatformTime::Seconds();
            FMemoryWriter Writer(Bytes, true); FObjectAndNameAsStringProxyArchive Ar(Writer, false);
            Ar.SetWantBinaryPropertySerialization(Binary);
            FY01ViewCommit::StaticStruct()->SerializeBin(Ar, &Expected);
            Milliseconds = (FPlatformTime::Seconds() - Start) * 1000;
            return !Ar.IsError();
        };
        auto RoundTrip = [&](bool Binary, const TArray<uint8>& Bytes)
        {
            FY01ViewCommit Decoded; FMemoryReader Reader(Bytes, true); FObjectAndNameAsStringProxyArchive Ar(Reader, false);
            Ar.SetWantBinaryPropertySerialization(Binary); FY01ViewCommit::StaticStruct()->SerializeBin(Ar, &Decoded);
            return !Ar.IsError() && Reader.Tell() == Bytes.Num()
                && FY01ViewCommit::StaticStruct()->CompareScriptStruct(&Expected, &Decoded, 0);
        };
        TArray<uint8> Tagged, Binary; double TaggedMs = 0, BinaryMs = 0;
        if (!TestTrue(TEXT("对照编码可运行"), Encode(false, Tagged, TaggedMs) && Encode(true, Binary, BinaryMs))) { return false; }
        TestTrue(TEXT("属性标签对照完整往返"), RoundTrip(false, Tagged));
        TestTrue(TEXT("嵌套二进制对照全部字段完整往返"), RoundTrip(true, Binary));
        FY01ViewSender Sender; FY01ViewWire Wire; FY01ViewReceiver Receiver; FWireRoute Route; FString Error;
        const double OfferStart = FPlatformTime::Seconds();
        if (!TestTrue(TEXT("生产提交编码"), Sender.Offer(Initial, Wire, Error)) || Wire.Items.IsEmpty()) { return false; }
        const double OfferMs = (FPlatformTime::Seconds() - OfferStart) * 1000;
        const int32 PayloadBytes = Wire.Items[0].TotalBytes;
        // 生产线格式在FY01ViewCommit二进制体前携带固定uint32版本封套。
        TestEqual(TEXT("生产提交不逐实例重复属性标签且不省略任何字段"), PayloadBytes, Binary.Num() + int32(sizeof(uint32)));
        FY01ViewAck Ack; int32 Windows = 0; int64 Bits = 0; double RouteMs = 0, ReceiveMs = 0, AckMs = 0;
        while (!Wire.Items.IsEmpty() && ++Windows < 10000)
        {
            double Start = FPlatformTime::Seconds(); const bool Sent = Route.Send(Wire);
            RouteMs += (FPlatformTime::Seconds() - Start) * 1000; Bits += Route.LastBits;
            if (!TestTrue(TEXT("生产FastArray每窗有界往返"), Sent && Route.LastBits > 0 && Route.LastBits < 128 * 1024)) { return false; }
            Start = FPlatformTime::Seconds(); const bool Received = Receiver.Receive(Route.Client, Ack, Error);
            ReceiveMs += (FPlatformTime::Seconds() - Start) * 1000;
            if (!TestTrue(TEXT("生产接收器验证完整清单"), Received)) { AddInfo(Error); return false; }
            if (Windows * FY01ViewWire::ChunkBytes * FY01ViewWire::WindowChunks < PayloadBytes)
            { if (!TestEqual(TEXT("末窗之前不发布半视图"), Receiver.Get().ViewRevision, uint64(0))) { return false; } }
            Start = FPlatformTime::Seconds(); const bool Advanced = Sender.Acknowledge(Ack, Wire, Error);
            AckMs += (FPlatformTime::Seconds() - Start) * 1000;
            if (!TestTrue(TEXT("生产ACK推进"), Advanced)) { AddInfo(Error); return false; }
        }
        TestTrue(TEXT("所有实例完成而非容量截断"), Wire.Items.IsEmpty() && Windows < 10000);
        TestTrue(TEXT("生产完整发布全部反射字段一致"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Initial, &Receiver.Get(), 0));
        double Start = FPlatformTime::Seconds();
        for (int32 I = 0; I < 5; ++I) { if (!Ok(TEXT("不变完整发布采样"), F.L()->PublishCommittedView())) { return false; } }
        const double PublishMs = (FPlatformTime::Seconds() - Start) * 1000 / 5;
        Start = FPlatformTime::Seconds();
        for (int32 I = 0; I < 5; ++I) { TestFalse(TEXT("完整Trace摘要存在"), Y01R36Trace::ViewRecord(Initial).IsEmpty()); }
        const double TraceMs = (FPlatformTime::Seconds() - Start) * 1000 / 5;
        AddInfo(FString::Printf(TEXT("编码测量：Items=%d TaggedBytes=%d BinaryBytes=%d ProductionBytes=%d TaggedMs=%.3f BinaryMs=%.3f OfferMs=%.3f Windows=%d WireBits=%lld RouteMs=%.3f ReceiveMs=%.3f AckMs=%.3f PublishMeanMs=%.3f TraceMeanMs=%.3f；内存测量非实际网络延迟"),
            Initial.Items.Num(), Tagged.Num(), Binary.Num(), PayloadBytes, TaggedMs, BinaryMs, OfferMs, Windows, Bits, RouteMs, ReceiveMs, AckMs, PublishMs, TraceMs));
        return true;
    }
    if (Case.StartsWith(TEXT("Engine")))
    {
        // 使用引擎连接/ActorChannel/ObjectReplicator；不启动录制、不写地图或网络Runtime。
        auto* Driver = NewObject<UIpNetDriver>(); Driver->SetWorld(F.World); FString Error;
        if (!TestTrue(TEXT("初始化引擎复制驱动"), Driver->UNetDriver::InitBase(false, nullptr, FURL(), false, Error))) { return false; }
        auto* Connection = NewObject<UIpConnection>(Driver); Connection->InitConnection(Driver, USOCK_Open, FURL());
        Connection->InitSendBuffer(); Driver->AddClientConnection(Connection);
        // 由连接选择空闲索引并登记Channels/OpenChannels，清理必须走同一生命周期。
        auto* Channel = Cast<UActorChannel>(Connection->CreateChannelByName(NAME_Actor, EChannelCreateFlags::OpenedLocally));
        ON_SCOPE_EXIT
        {
            if (Channel) { Channel->Actor = nullptr; }
            Connection->CleanUp();
            TestTrue(TEXT("服务器通道随连接解除登记"), Connection->OpenChannels.IsEmpty() && (!Channel || Channel->Connection == nullptr));
            Driver->SetWorld(nullptr);
        };
        if (!TestNotNull(TEXT("创建已登记服务器通道"), Channel)) { return false; }
        Channel->Actor = F.P;
        if (Case != TEXT("EngineOwnerOnly"))
        {
            auto* ClientState = F.World->SpawnActor<AY01PlayerState>(); ClientState->SetRole(ROLE_SimulatedProxy);
            auto* ClientDriver = NewObject<UIpNetDriver>(); ClientDriver->SetWorld(F.World);
            if (!TestTrue(TEXT("初始化客户端复制驱动"), ClientDriver->UNetDriver::InitBase(true, nullptr, FURL(), false, Error))) { return false; }
            auto* ClientConnection = NewObject<UIpConnection>(ClientDriver); ClientConnection->InitConnection(ClientDriver, USOCK_Open, FURL());
            ClientConnection->InitSendBuffer(); ClientDriver->ServerConnection = ClientConnection;
            auto* ClientChannel = Cast<UActorChannel>(ClientConnection->CreateChannelByName(NAME_Actor, EChannelCreateFlags::None));
            ON_SCOPE_EXIT
            {
                if (ClientChannel) { ClientChannel->Actor = nullptr; }
                ClientConnection->CleanUp();
                TestTrue(TEXT("客户端通道随连接解除登记"), ClientConnection->OpenChannels.IsEmpty() && (!ClientChannel || ClientChannel->Connection == nullptr));
                ClientDriver->SetWorld(nullptr);
            };
            if (!TestNotNull(TEXT("创建已登记客户端通道"), ClientChannel)) { return false; }
            ClientChannel->Actor = ClientState;
            FObjectReplicator Send, Receive; Send.InitWithObject(F.L(), Connection); Send.StartReplicating(Channel);
            // 夹具只测已创建子对象的复制，显式登记实际服务器分配的NetGUID，不模拟Actor出生握手。
            ClientDriver->GetNetGuidCache()->RegisterNetGUID_Client(Send.ObjectNetGUID, ClientState->GetLoadout());
            Receive.InitWithObject(ClientState->GetLoadout(), ClientConnection); Receive.StartReplicating(ClientChannel);
            auto& Wire = *FindFProperty<FStructProperty>(F.L()->GetClass(), TEXT("ViewWire"))->ContainerPtrToValuePtr<FY01ViewWire>(F.L());
            auto* AckFunction = F.L()->FindFunction(TEXT("ServerAckView")); int32 Windows = 0; int64 MaxBits = 0;
            while (!Wire.Items.IsEmpty() && ++Windows < 10000)
            {
                ++Driver->ReplicationFrame; FReplicationFlags Flags; Flags.bNetInitial = Windows == 1; bool Skipped = false;
                FNetBitWriter Stranger(Connection->PackageMap, 0); Send.ReplicateCustomDeltaProperties(Stranger, Flags, Skipped);
                if (!TestTrue(TEXT("每一窗非Owner均被引擎过滤"), Skipped && Stranger.GetNumBits() == 0)) { return false; }
                Flags.bNetOwner = true; FNetBitWriter Owner(Connection->PackageMap, 0); Send.ReplicateCustomDeltaProperties(Owner, Flags, Skipped);
                MaxBits = FMath::Max(MaxBits, Owner.GetNumBits());
                if (!TestTrue(TEXT("引擎每窗非空且有界"), Owner.GetNumBits() > 0 && Owner.GetNumBits() < 128 * 1024 && !Owner.IsError()))
                { AddInfo(FString::Printf(TEXT("实际=%lld bit"), Owner.GetNumBits())); return false; }
                FPacketIdRange Packet(Windows); Send.PostSendBunch(Packet, false);
                if (Case == TEXT("EngineLostWindow") && Windows == 2) { Send.ReceivedNak(Windows); continue; }
                FNetBitReader Reader(ClientConnection->PackageMap, Owner.GetData(), Owner.GetNumBits()); bool Unmapped = false;
                if (!TestTrue(TEXT("引擎接收器解码到生产组件回调"), Receive.ReceivedBunch(Reader, Flags, false, Unmapped) && !Reader.IsError() && !Unmapped)) { return false; }
                Receive.PostReceivedBunch(); Connection->OutAckPacketId = Windows;
                auto Ack = Wire.Items[0].Scope; F.L()->ProcessEvent(AckFunction, &Ack);
            }
            FY01CommittedView View; TestTrue(TEXT("引擎组装完成发布"), ClientState->GetLoadout()->ReadCommittedView(View));
            TestTrue(TEXT("确实多窗且最终完成"), Windows > 1 && Windows < 10000 && Wire.Items.IsEmpty());
            TestEqual(TEXT("引擎全量无截断"), View.Items.Num(), Grants.Num()); TestEqual(TEXT("引擎最终同版本"), View.ViewRevision, F.View().ViewRevision);
            TestTrue(TEXT("确实走默认inner delta结构编码"), EnumHasAnyFlags(Wire.GetDeltaSerializationFlags(), EFastArraySerializerDeltaFlags::IsUsingDeltaSerialization));
            AddInfo(FString::Printf(TEXT("引擎完整发布：%d实例，%d窗，最大%lld bit；ACK由夹具回送RPC实现，非跨进程验收"), Grants.Num(), Windows, MaxBits));
            return true;
        }
        {
            FObjectReplicator Replicator; Replicator.InitWithObject(F.L(), Connection); Replicator.StartReplicating(Channel);
            if (!TestTrue(TEXT("普通驱动初始化FastArray增量历史"), Replicator.ChangelistMgr->GetRepChangelistState()->CustomDeltaChangelistState.IsValid())) { return false; }
            ++Driver->ReplicationFrame; // 与ServerReplicateActors一致，先推进复制帧再比较增量历史。
            FReplicationFlags Flags; Flags.bNetInitial = true; Flags.bNetOwner = false; bool Skipped = false;
            FNetBitWriter Stranger(Connection->PackageMap, 0); Replicator.ReplicateCustomDeltaProperties(Stranger, Flags, Skipped);
            TestTrue(TEXT("真实复制器跳过非Owner"), Skipped); TestEqual(TEXT("非Owner视图零bit"), Stranger.GetNumBits(), int64(0));
            Flags.bNetOwner = true; FNetBitWriter Owner(Connection->PackageMap, 0); Replicator.ReplicateCustomDeltaProperties(Owner, Flags, Skipped);
            TestFalse(TEXT("Owner不被过滤"), Skipped); TestTrue(TEXT("真实复制器调用FastArray编码"), Owner.GetNumBits() > 0 && !Owner.IsError());
            AddInfo(FString::Printf(TEXT("引擎Owner窗口实际编码=%lld bit，上界=%d bit"), Owner.GetNumBits(), 128 * 1024));
            TestTrue(TEXT("真实复制器窗口仍有界"), Owner.GetNumBits() < 128 * 1024);
        }
        return true;
    }
    if (Case.StartsWith(TEXT("Component")))
    {
        auto* Prop = FindFProperty<FStructProperty>(F.L()->GetClass(), TEXT("ViewWire"));
        auto& Wire = *Prop->ContainerPtrToValuePtr<FY01ViewWire>(F.L());
        auto* AckFunction = F.L()->FindFunction(TEXT("ServerAckView"));
        if (Case == TEXT("ComponentOwnerChange"))
        {
            const auto Old = F.View(); F.P->SetOwner(nullptr); Ok(TEXT("无Owner暂停传输"), F.L()->PublishCommittedView());
            F.P->SetOwner(F.PC); Ok(TEXT("重新绑定连接"), F.L()->PublishCommittedView());
            TestNotEqual(TEXT("重绑不复用旧连接身份"), F.View().ConnectionId, Old.ConnectionId); return true;
        }
        auto Ack = Wire.Items[0].Scope;
        if (Case == TEXT("ComponentAckOwner"))
        {
            auto* Foreign = F.Player(); F.P->SetOwner(Foreign);
            F.L()->ProcessEvent(AckFunction, &Ack);
            TestEqual(TEXT("不匹配Owner不能推进ACK"), Wire.Items[0].Scope.TransferId, Ack.TransferId);
            TestFalse(TEXT("Owner鉴权失败明确诊断"), F.L()->GetViewDiagnostic().IsEmpty()); return true;
        }
        if (Case == TEXT("ComponentAck"))
        {
            auto Bad = Ack; ++Bad.JoinSequence; F.L()->ProcessEvent(AckFunction, &Bad);
            TestEqual(TEXT("错误Join不推进"), Wire.Items[0].Scope.TransferId, Ack.TransferId);
            F.L()->ProcessEvent(AckFunction, &Ack);
            TestTrue(TEXT("真实RPC实现推进窗口"), Wire.Items.IsEmpty() || Wire.Items[0].Scope.TransferId != Ack.TransferId
                || Wire.Items[0].Scope.WindowSequence != Ack.WindowSequence); return true;
        }
        auto* ClientState = F.World->SpawnActor<AY01PlayerState>(); ClientState->SetRole(ROLE_SimulatedProxy);
        FWireRoute Route; int32 Windows = 0;
        while (!Wire.Items.IsEmpty() && ++Windows < 100)
        {
            Ack = Wire.Items[0].Scope;
            if (!TestTrue(TEXT("组件真实FastArray接收回调"), Route.Send(Wire, false, ClientState->GetLoadout()))) { return false; }
            F.L()->ProcessEvent(AckFunction, &Ack);
        }
        FY01CommittedView V;
        TestTrue(TEXT("不手调纯接收器也发布"), ClientState->GetLoadout()->ReadCommittedView(V));
        TestTrue(TEXT("生产ACK不会永久等待"), Windows < 100);
        TestEqual(TEXT("组件收到完整全部库存"), V.Items.Num(), Grants.Num());
        TestEqual(TEXT("组件收到最新版本"), V.ViewRevision, F.View().ViewRevision); return true;
    }
    if (Case == TEXT("SourceMismatch"))
    {
        Ok(TEXT("真实穿戴"), F.L()->TryEquip(F.PC, F.Request(A.ItemId, 0)));
        const auto Before = F.View(); auto* Sources = F.P->GetSourceEffects(); FY01SourceSnapshot Snapshot; Sources->Observe(Snapshot);
        const auto* Source = Snapshot.Effects.FindByPredicate([](const auto& S) { return S.Source.Kind == EY01RuntimeSourceKind::Equipment; });
        if (!TestNotNull(TEXT("真实装备来源"), Source)) { return false; }
        FY01ResolvedSource Changed; Changed.Kind = Source->Source.Kind; Changed.LocalKey = Source->Source.LocalKey;
        Changed.Effects.Add({TEXT("Default"), EY01SourceEffectKind::AttackFlat, 20});
        FY01PreparedMutation Plan; TestTrue(TEXT("受信接口准备错配来源"), Sources->PrepareMutation({Changed}, {Source->Source}, Plan).IsOk());
        FY01SourceTransaction Tx; TestTrue(TEXT("真实暂存"), Sources->StageMutation(Plan, Tx).IsOk());
        TestTrue(TEXT("真实来源提交"), Sources->CommitMutation(Tx).IsOk());
        TestFalse(TEXT("同ID但效果错配拒绝发布"), F.L()->PublishCommittedView().IsOk());
        TestEqual(TEXT("不发布错配属性"), F.View().ViewRevision, Before.ViewRevision); return true;
    }
    if (Case.StartsWith(TEXT("Wire")))
    {
        FY01ViewSender Sender; FY01ViewReceiver Receiver; FY01ViewWire Wire; FWireRoute Route; FString Error;
        auto Initial = F.View();
        if (!TestTrue(TEXT("从真实注册表捕获构造网络提交"), Sender.Offer(Initial, Wire, Error))) { return false; }
        FY01ViewAck Ack; int32 Windows = 0; auto Last = Initial; FY01ViewWire FirstWindow;
        if (Case == TEXT("WireCrossPending"))
        {
            TestTrue(TEXT("旧会话首窗口"), Receiver.Receive(Wire, Ack, Error));
            auto Foreign = Initial; Foreign.MatchId = FGuid::NewGuid(); Foreign.ConnectionId = FGuid::NewGuid();
            FY01ViewSender Other; FY01ViewWire OtherWire; TestTrue(TEXT("另一合法会话"), Other.Offer(Foreign, OtherWire, Error));
            TestFalse(TEXT("未完成基线也不能混会话"), Receiver.Receive(OtherWire, Ack, Error)); return true;
        }
        if (Case == TEXT("WireResync"))
        {
            TestTrue(TEXT("先接收首窗口"), Receiver.Receive(Wire, Ack, Error)); TestTrue(TEXT("确认首窗口"), Sender.Acknowledge(Ack, Wire, Error));
            Receiver.Reset(); TestFalse(TEXT("丢失组装态不得发布中段"), Receiver.Receive(Wire, Ack, Error));
            auto Scope = Wire.Items[0].Scope; auto Bad = Scope; ++Bad.JoinSequence;
            TestFalse(TEXT("重同步需要正确身份"), Sender.Restart(Bad, Wire, Error));
            if (!TestTrue(TEXT("鉴权重同步退回完整基线"), Sender.Restart(Scope, Wire, Error))) { return false; }
            Route.Base.Reset();
        }
        if (Case == TEXT("WireRapidReplace"))
        {
            for (int32 I = 0; I < 30; ++I)
            { Ok(TEXT("真实快速替换"), F.L()->TryEquip(F.PC, F.Request(I % 2 ? B.ItemId : A.ItemId, 0))); Last = F.View(); TestTrue(TEXT("仅合并最新状态"), Sender.Offer(Last, Wire, Error)); }
        }
        if (Case == TEXT("WireConcurrent"))
        {
            Ok(TEXT("基线传送期间继续真实穿戴"), F.L()->TryEquip(F.PC, F.Request(A.ItemId, 0))); Last = F.View();
            TestTrue(TEXT("新提交排入最新状态不替换在途"), Sender.Offer(Last, Wire, Error));
        }
        while (!Wire.Items.IsEmpty() && ++Windows < 10000)
        {
            TestTrue(TEXT("有界窗口不是容量"), Wire.Items.Num() <= FY01ViewWire::WindowChunks);
            for (const auto& Chunk : Wire.Items) { TestTrue(TEXT("每片明确字节界"), Chunk.Bytes.Num() <= FY01ViewWire::ChunkBytes); }
            if (Case == TEXT("WireLostWindow")) { TestTrue(TEXT("真实编码后丢包"), Route.Send(Wire, true)); }
            if (!TestTrue(TEXT("真实NetDeltaSerialize路由编码解码"), Route.Send(Wire))) { return false; }
            TestTrue(TEXT("真实编码位数保持有界"), Route.LastBits < 128 * 1024);
            auto Copy = Route.Client;
            if (Windows == 1) { FirstWindow = Copy; }
            if (Case == TEXT("WirePartial") && Windows == 1)
            { auto Partial = Copy; Partial.Items.Pop(); TestFalse(TEXT("窗口少一片不确认"), Receiver.Receive(Partial, Ack, Error)); TestEqual(TEXT("半窗不发布"), Receiver.Get().ViewRevision, uint64(0)); }
            if (Case == TEXT("WireStaleWindow") && Windows == 3)
            { TestFalse(TEXT("过期窗口不回退暂存"), Receiver.Receive(FirstWindow, Ack, Error)); }
            if (Case == TEXT("WireReorder")) { Algo::Reverse(Copy.Items); }
            if (Case == TEXT("WireCorrupt") && Windows == 1)
            { Copy.Items[0].Bytes[0] ^= 1; TestFalse(TEXT("损坏分片不发布"), Receiver.Receive(Copy, Ack, Error)); TestEqual(TEXT("损坏不发布半状态"), Receiver.Get().ViewRevision, uint64(0)); return true; }
            if (!TestTrue(TEXT("客户端暂存完整窗口"), Receiver.Receive(Copy, Ack, Error))) { AddInfo(Error); return false; }
            if (Case == TEXT("WireDuplicate")) { TestTrue(TEXT("重复窗口幂等"), Receiver.Receive(Copy, Ack, Error)); }
            if (Case == TEXT("WireAckScope"))
            {
                auto Bad = Ack; Bad.ConnectionId = FGuid::NewGuid(); TestFalse(TEXT("错误连接ACK拒绝"), Sender.Acknowledge(Bad, Wire, Error));
                Bad = Ack; ++Bad.WindowSequence; TestFalse(TEXT("跳号ACK拒绝"), Sender.Acknowledge(Bad, Wire, Error));
                Bad = Ack; Bad.MatchId = FGuid::NewGuid(); TestFalse(TEXT("跨Match ACK拒绝"), Sender.Acknowledge(Bad, Wire, Error));
            }
            if (!TestTrue(TEXT("ACK推进下一窗口"), Sender.Acknowledge(Ack, Wire, Error))) { AddInfo(Error); return false; }
        }
        TestTrue(TEXT("初始大量库存确实分批"), Windows > 1 && Windows < 10000);
        TestEqual(TEXT("最终版本不丢失"), Receiver.Get().ViewRevision, Last.ViewRevision);
        TestEqual(TEXT("全量实例无截断"), Receiver.Get().Items.Num(), Grants.Num());
        TestEqual(TEXT("无半次装备属性"), Receiver.Get().Stats.AttackDamage, Last.Stats.AttackDamage);
        if (Case == TEXT("WireRebind") || Case == TEXT("WireResetSession"))
        {
            const auto OldWindow = FirstWindow; F.P->SetOwner(nullptr); Ok(TEXT("断开Owner"), F.L()->PublishCommittedView());
            F.P->SetOwner(F.PC); Ok(TEXT("重绑Owner"), F.L()->PublishCommittedView()); Last = F.View();
            if (Case == TEXT("WireResetSession")) { Receiver.Reset(); Last.MatchId = FGuid::NewGuid(); ++Last.JoinSequence; }
            Sender.Reset(); TestTrue(TEXT("新连接从完整基线开始"), Sender.Offer(Last, Wire, Error));
            while (!Wire.Items.IsEmpty())
            { if (!Route.Send(Wire) || !Receiver.Receive(Route.Client, Ack, Error) || !Sender.Acknowledge(Ack, Wire, Error)) { AddError(Error); return false; } }
            TestEqual(TEXT("接收新连接完整视图"), Receiver.Get().ConnectionId, Last.ConnectionId);
            TestEqual(TEXT("接收新会话身份"), Receiver.Get().MatchId, Last.MatchId);
            TestFalse(TEXT("新连接后拒绝旧窗口"), Receiver.Receive(OldWindow, Ack, Error));
        }
        if (Case == TEXT("WireCrossSession"))
        {
            auto Foreign = Last; Foreign.MatchId = FGuid::NewGuid(); Foreign.ConnectionId = FGuid::NewGuid(); Foreign.JoinSequence = 2;
            TestFalse(TEXT("旧sender不能混另一会话"), Sender.Offer(Foreign, Wire, Error));
        }
        if (Case == TEXT("WireTombstone"))
        {
            auto Book = F.Grant(K::Skill); Ok(TEXT("发布可见书基线"), F.Add({Book})); TestTrue(TEXT("入书提交"), Sender.Offer(F.View(), Wire, Error));
            while (!Wire.Items.IsEmpty()) { if (!Route.Send(Wire) || !Receiver.Receive(Route.Client, Ack, Error) || !Sender.Acknowledge(Ack, Wire, Error)) { AddError(Error); return false; } }
            TestEqual(TEXT("客户端确实见过此书"), Receiver.Get().Items.Last().ItemId, Book.ItemId);
            Ok(TEXT("真实耗书删除"), F.L()->PrepareLearn(F.PC, F.Request(Book.ItemId, 0, EY01ItemOperation::PrepareLearn)));
            TestTrue(TEXT("墓碑提交"), Sender.Offer(F.View(), Wire, Error));
            while (!Wire.Items.IsEmpty()) { if (!Route.Send(Wire) || !Receiver.Receive(Route.Client, Ack, Error) || !Sender.Acknowledge(Ack, Wire, Error)) { AddError(Error); return false; } }
            TestEqual(TEXT("墓碑删除已可见书"), Receiver.Get().Items.Num(), Grants.Num());
            TestTrue(TEXT("删除与已学同次发布"), Receiver.Get().Skills[0].LearnedId.IsValid());
        }
        if (Case == TEXT("WireDeltaDelete"))
        {
            auto Book = F.Grant(K::Skill); Ok(TEXT("基线后入书"), F.Add({Book}));
            // 增后删在尚未Offer的中间版本中合并，不等待永不到达的实体。
            Ok(TEXT("新书立即消费"), F.L()->PrepareLearn(F.PC, F.Request(Book.ItemId, 0, EY01ItemOperation::PrepareLearn)));
            Last = F.View(); TestTrue(TEXT("合并增后删提交"), Sender.Offer(Last, Wire, Error));
            while (!Wire.Items.IsEmpty())
            { if (!Route.Send(Wire) || !Receiver.Receive(Route.Client, Ack, Error) || !Sender.Acknowledge(Ack, Wire, Error)) { AddError(Error); return false; } }
            TestEqual(TEXT("合并后无幽灵书"), Receiver.Get().Items.Num(), Grants.Num());
            TestTrue(TEXT("已学槽同版本到达"), Receiver.Get().Skills[0].LearnedId.IsValid());
        }
        return true;
    }
    if (Case == TEXT("DisplayOwned"))
    {
        F.Data.Row<FY01EquipmentRow>(K::Equipment).DisplayName = FText::FromString(TEXT("后来改名"));
        const auto I = F.View().Items[0]; TestEqual(TEXT("装备捕获真实名称"), I.Display.Name.ToString(), FString(TEXT("真实装备名称")));
        TestEqual(TEXT("装备捕获真实品质"), I.Display.Quality, EY01Quality::Purple);
        TestEqual(TEXT("装备捕获真实Icon"), I.Display.Icon.ToString(), FString(TEXT("/Game/Display/Equipment.Equipment"))); return true;
    }
    if (Case == TEXT("ConfigCommit"))
    { const auto V = F.View(); Ok(TEXT("配置发布"), F.L()->SetConfigRevision(2)); TestTrue(TEXT("配置推进业务视图版本"), F.View().ViewRevision > V.ViewRevision); TestEqual(TEXT("配置同版本"), F.View().ConfigRevision, uint64(2)); return true; }
    if (Case == TEXT("TrustedPublish"))
    {
        TestTrue(TEXT("真实余额增加"), F.P->TryAddMatchReward(7, 3)); Ok(TEXT("受信发布"), F.L()->PublishCommittedView());
        TestEqual(TEXT("只捕获唯一钱包"), F.View().Gold, 7); TestEqual(TEXT("经验同次捕获"), F.View().Experience, 3); return true;
    }
    if (Case == TEXT("Receipt64"))
    {
        for (int32 I = 0; I < 70; ++I) { auto R = F.Request(A.ItemId, 0); R.Slot = 8; TestFalse(TEXT("失败回执"), F.L()->TryEquip(F.PC, R).IsOk()); }
        TestEqual(TEXT("业务视图带最近64份回执"), F.View().Receipts.Num(), 64); TestEqual(TEXT("可读下个序号"), F.View().ExpectedSeq, uint64(71)); return true;
    }
    if (A.Definition.Type == K::Skill)
    {
        Ok(TEXT("真实消耗学习"), F.L()->PrepareLearn(F.PC, F.Request(A.ItemId, 0, EY01ItemOperation::PrepareLearn)));
        TestEqual(TEXT("学习槽与耗书同版本"), F.View().Items.Num(), 1); TestTrue(TEXT("学习显示身份"), F.View().Skills[0].LearnedId.IsValid());
        if (Case == TEXT("LearnAtomic")) { TestEqual(TEXT("技能来源和库存同版本"), F.View().Stats.AttackDamage, 40.f); return true; }
        auto Q = F.Request(B.ItemId, 0, EY01ItemOperation::PrepareLearn); auto Ticket = F.L()->PrepareLearn(F.PC, Q);
        auto* Target = F.Player(); F.Data.Row<FY01CharacterRow>(K::Character).Health = 1000;
        Target->GetPlayerState<AY01PlayerState>()->TryInitializeCharacter(FY01DataCatalog(F.Data.Tables), FY01MatchFixture::Ref(K::Character), F.Error);
        Target->GetPawn()->SetActorLocation(F.PC->GetPawn()->GetActorLocation() + FVector(100, 0, 0)); F.P->GetCombatASC()->SetCombatTarget(Target->GetPawn());
        const auto Before = F.View(); TestTrue(TEXT("真实GA释放提交"), F.P->TryActivateSkillSlot(0));
        TestTrue(TEXT("CD推进可见提交版本"), F.View().ViewRevision > Before.ViewRevision);
        TestEqual(TEXT("CD同次显示截止"), F.View().Skills[0].Deadline, double(F.World->GetTimeSeconds()) + 8);
        if (Case == TEXT("TicketSurvivesCD"))
        { FY01LoadoutSnapshot V; F.L()->Observe(V); Q.Seq = V.ExpectedSeq; Q.Operation = EY01ItemOperation::ConfirmLearn; Q.TicketId = Ticket.TicketId; Ok(TEXT("CD不废旧确认"), F.L()->ConfirmLearn(F.PC, Q)); }
        return true;
    }
    const auto Before = F.View(); int32 Calls = 0; FDelegateHandle H;
    if (Case == TEXT("BusyObservation"))
    {
        H = F.P->GetCombatASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda([&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle)
        {
            ++Calls; const auto During = F.View(); TestEqual(TEXT("Busy只可读旧完整版本"), During.ViewRevision, Before.ViewRevision);
            TestEqual(TEXT("Busy不读半属性"), During.Stats.AttackDamage, 30.f);
            TestEqual(TEXT("Busy拒绝新发布"), F.L()->PublishCommittedView().Code, EY01ItemError::Busy);
        });
    }
    if (Case == TEXT("FailedMutation"))
    { F.P->GetCombatASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda([](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { return false; })); }
    const auto Result = F.L()->TryEquip(F.PC, F.Request(A.ItemId, 0));
    F.P->GetCombatASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(H); F.P->GetCombatASC()->GameplayEffectApplicationQueries.Reset();
    if (Case == TEXT("FailedMutation"))
    { TestFalse(TEXT("真实GE失败"), Result.IsOk()); TestEqual(TEXT("失败视图不穿戴"), F.View().EquipmentSlots[0], FGuid()); TestEqual(TEXT("失败视图不带半GE"), F.View().Stats.AttackDamage, 30.f); return true; }
    Ok(TEXT("真实穿戴"), Result); TestEqual(TEXT("穿戴来源和库存同版本"), F.View().Stats.AttackDamage, 40.f);
    TestEqual(TEXT("装备槽同次"), F.View().EquipmentSlots[0], A.ItemId);
    if (Case == TEXT("BusyObservation")) { TestTrue(TEXT("真实GAS重入已覆盖"), Calls > 0); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R36TimingBoundaryTest, "Y01.Framework.R36.Timing.Boundaries",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R36TimingBoundaryTest::RunTest(const FString&)
{
    struct FCapture : FOutputDevice
    {
        TArray<TSharedPtr<FJsonObject>> Rows;
        void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName&) override
        {
            const FString Line(Text), Prefix(TEXT("Y01R36_TRACE "));
            if (!Line.StartsWith(Prefix) || Rows.Num() >= 128) { return; }
            TSharedPtr<FJsonObject> J;
            if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Line.Mid(Prefix.Len())), J)
                && J->GetStringField(TEXT("kind")) == TEXT("TIMING")) { Rows.Add(J); }
        }
    } Capture;
    const FString CommandLine(FCommandLine::Get());
    ON_SCOPE_EXIT { GLog->RemoveOutputDevice(&Capture); FCommandLine::Set(*CommandLine); };
    GLog->AddOutputDevice(&Capture);
    auto Transfer = [&]()
    {
        FViewWorld F; F.L()->Initialize(F.Match, 1);
        TArray<FY01ItemGrant> Grants; for (int32 I = 0; I < 17; ++I) { Grants.Add(F.Grant()); }
        F.Add(Grants);
        FY01ViewSender Sender; FY01ViewReceiver Receiver; FY01ViewWire Wire; FString Error;
        TestTrue(TEXT("原发送器未被遥测改变"), Sender.Offer(F.View(), Wire, Error));
        int32 Windows = 0;
        while (!Wire.Items.IsEmpty() && ++Windows < 100)
        {
            FY01ViewAck Ack;
            if (!TestTrue(TEXT("原接收器完整接收"), Receiver.Receive(Wire, Ack, Error))) { break; }
            TestTrue(TEXT("原确认推进"), Sender.Acknowledge(Ack, Wire, Error));
        }
        const auto Expected = F.View();
        TestTrue(TEXT("遥测不改变业务快照"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Expected, &Receiver.Get(), 0));
        if (Capture.Rows.Num())
        {
            TSharedPtr<FJsonObject> Full;
            FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Y01R36Trace::ViewRecord(Expected)), Full);
            TestTrue(TEXT("事件摘要与完整视图采样一致且不依赖采样命中"), Capture.Rows.ContainsByPredicate([&](const auto& J)
            {
                FString Digest;
                return J->GetStringField(TEXT("stage")) == TEXT("COMMIT")
                    && J->GetNumberField(TEXT("revision")) == Expected.ViewRevision
                    && J->TryGetStringField(TEXT("digest"), Digest) && Full.IsValid() && Digest == Full->GetStringField(TEXT("digest"));
            }));
        }
    };
    FCommandLine::Set(TEXT("-R36Timing")); Transfer();
    TestEqual(TEXT("没有显式R36Trace不输出"), Capture.Rows.Num(), 0);
    FCommandLine::Set(TEXT("-R36Trace -R36Timing")); Transfer();
    for (const TCHAR* Stage : {TEXT("COMMIT"), TEXT("ENCODE"), TEXT("SEND_WINDOW"), TEXT("RECEIVE_WINDOW"),
        TEXT("ASSEMBLED"), TEXT("PUBLISHED"), TEXT("ACK_WINDOW")})
    {
        TestTrue(FString(TEXT("真实边界：")) + Stage, Capture.Rows.ContainsByPredicate([&](const auto& J)
            { return J->GetStringField(TEXT("stage")) == Stage; }));
    }
    for (const auto& J : Capture.Rows)
    {
        TestFalse(TEXT("版本连接身份非空"), J->GetStringField(TEXT("connection")).IsEmpty());
        const double Raw = FCString::Strtoui64(*J->GetStringField(TEXT("qpc")), nullptr, 10) * J->GetNumberField(TEXT("secondsPerCycle"));
        TestTrue(TEXT("时钟夹取顺序"), J->GetNumberField(TEXT("time")) <= J->GetNumberField(TEXT("clockEnd")));
        TestTrue(TEXT("实测UE与QPC同一计时源"), FMath::Abs(J->GetNumberField(TEXT("time")) - Raw - 16777216.) < .01);
    }
    return true;
}
#endif
