#include "Misc/AutomationTest.h"
#include "Playground/Y01R36UI.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
using Z = EY01R36Zone;
using O = EY01ItemOperation;
constexpr auto Preparing = EY01MatchPhase::Preparing;
FY01CommittedView Fixture()
{
    FY01CommittedView V; V.MatchId = FGuid::NewGuid(); V.ConnectionId = FGuid::NewGuid(); V.JoinSequence = 7;
    V.ViewRevision = 10; V.Revision = 4; V.ConfigRevision = 8; V.ExpectedSeq = 3; V.Health = 100;
    V.EquipmentSlots.SetNum(6); V.Skills.SetNum(4);
    for (int32 I = 0; I < 4; ++I)
    {
        auto& Item = V.Items.AddDefaulted_GetRef(); Item.ItemId = FGuid::NewGuid(); Item.EntityRevision = I + 2;
        Item.AcquisitionOrder = I + 1; Item.Definition.Type = I < 2 ? EY01TableKind::Equipment : EY01TableKind::Skill;
        Item.Definition.RowName = FName(*FString::Printf(TEXT("HiddenDefinition%d"), I));
        Item.Display.Name = FText::FromString(TEXT("同名内容"));
    }
    V.Items[1].Location = EY01ItemLocation::EquipmentSlot; V.Items[1].Slot = 0; V.EquipmentSlots[0] = V.Items[1].ItemId;
    for (int32 I = 0; I < 4; ++I) { V.Skills[I].Slot = I; }
    auto& S = V.Skills[0]; S.LearnedId = FGuid::NewGuid(); S.EntityRevision = 6; S.SkillId = TEXT("HiddenSkill");
    S.Display.Name = FText::FromString(TEXT("同名内容")); S.Display.SkillType = EY01SkillType::Active; S.Deadline = 18;
    return V;
}
FY01ItemRequest Ticket(const FY01CommittedView& V)
{
    FY01ItemRequest R; R.MatchId = V.MatchId; R.JoinSequence = V.JoinSequence; R.ConfigRevision = V.ConfigRevision;
    R.Seq = 2; R.Operation = O::PrepareLearn; R.ItemId = V.Items[2].ItemId; R.ExpectedEntityRevision = V.Items[2].EntityRevision;
    R.Slot = 0; R.ExpectedLearnedId = V.Skills[0].LearnedId; R.ExpectedLearnedRevision = V.Skills[0].EntityRevision;
    R.TicketId = FGuid::NewGuid(); return R;
}
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36UIModelTest, "Y01.Framework.R36.UI.Model",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36UIModelTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("CommittedOnly"), TEXT("Backpack"), TEXT("TenThousand"), TEXT("Equip"), TEXT("Replace"),
        TEXT("Unequip"), TEXT("NoDirectSwap"), TEXT("NoBookEquipment"), TEXT("NoEquipmentSkill"), TEXT("NoLearnedBackpack"),
        TEXT("EmptyLearn"), TEXT("OccupiedLearn"), TEXT("DuplicateIntent"), TEXT("SameNames"), TEXT("StaleDrag"), TEXT("ForeignDrag"),
        TEXT("SixFourBounds"), TEXT("PhaseAlive"), TEXT("PendingNoOptimism"), TEXT("UnrelatedResponse"), TEXT("FailureReceipt"),
        TEXT("ExactResponse"), TEXT("Confirm"), TEXT("Cancel"), TEXT("StaleTicket"), TEXT("Remove"), TEXT("Cooldown"),
        TEXT("RelearnReady"), TEXT("UnknownCooldown"), TEXT("InvalidDeadline"), TEXT("NoInternalIds"), TEXT("SessionReset"), TEXT("CoalescedSuccess"), TEXT("CoalescedFailure"),
        TEXT("CoalescedTicket"), TEXT("CoalescedWrongPayload"), TEXT("CoalescedOldSerial") }) { Names.Add(N); Commands.Add(N); }
}
bool FY01R36UIModelTest::RunTest(const FString& Case)
{
    auto V = Fixture(); FY01R36UIModel M; M.Sync(V); FY01ItemRequest R; FY01R36Drag D;
    if (Case.StartsWith(TEXT("Coalesced")))
    {
        M.Drag(Z::Backpack, V.Items[2].ItemId, -1, D); M.Drop(D, Z::Skill, 0, Preparing, R); M.Sent(R);
        auto& Receipt = V.Receipts.AddDefaulted_GetRef(); Receipt.Request = R;
        const bool Failure = Case == TEXT("CoalescedFailure");
        Receipt.Code = Failure ? EY01ItemError::DuplicateSkill : EY01ItemError::None;
        if (Case == TEXT("CoalescedTicket"))
        {
            V.Tickets.Add(Ticket(V)); Receipt.TicketId = V.Tickets[0].TicketId;
        }
        if (Case == TEXT("CoalescedWrongPayload")) { ++Receipt.Request.Slot; }
        V.Response.Request = R; V.Response.Request.Seq += 10; V.Response.Code = EY01ItemError::SequenceGap;
        V.ResponseSerial = Case == TEXT("CoalescedOldSerial") ? 0 : 2; ++V.ViewRevision;
        const bool Pending = Case == TEXT("CoalescedWrongPayload") || Case == TEXT("CoalescedOldSerial");
        M.Sync(V); TestEqual(TEXT("合并视图仅精确且新的回执解除等待"), M.IsPending(), Pending);
        TestEqual(TEXT("不篡改权威最新响应"), M.View().Response.Code, EY01ItemError::SequenceGap);
        if (!Pending)
        {
            if (Case == TEXT("CoalescedTicket"))
            {
                TestTrue(TEXT("合并后仍展示自己的权威确认票据"), M.NeedsConfirmation());
                TestTrue(TEXT("合并后可取消"), M.Decide(false, Preparing, R));
                TestEqual(TEXT("使用原票据"), R.TicketId, Receipt.TicketId);
                ++V.ViewRevision; M.Sync(V); TestTrue(TEXT("无新响应的视图更新保留待确认"), M.NeedsConfirmation());
                V.Tickets.Reset(); ++V.ViewRevision; M.Sync(V); TestFalse(TEXT("权威失效关闭确认"), M.NeedsConfirmation());
            }
            else { TestTrue(TEXT("展示自己的权威成功或失败"), M.Status().Contains(Failure ? TEXT("已学习相同技能") : TEXT("服务器已确认"))); }
        }
        return true;
    }
    if (Case == TEXT("CommittedOnly"))
    {
        TestEqual(TEXT("投影完整版本"), M.View().ViewRevision, V.ViewRevision);
        auto Old = V; --Old.ViewRevision; Old.Items.Reset(); TestFalse(TEXT("旧版本不覆盖"), M.Sync(Old));
        Old.ViewRevision = 0; TestFalse(TEXT("未发布不展示"), M.Sync(Old)); TestEqual(TEXT("保留完整四件"), M.View().Items.Num(), 4); return true;
    }
    if (Case == TEXT("Backpack") || Case == TEXT("TenThousand"))
    {
        if (Case == TEXT("TenThousand"))
        {
            V.Items.Reset(); V.EquipmentSlots[0] = {};
            for (int32 I = 0; I < 10000; ++I) { auto& Item = V.Items.AddDefaulted_GetRef(); Item.ItemId = FGuid::NewGuid(); Item.AcquisitionOrder = I + 1; }
            ++V.ViewRevision; M.Sync(V);
        }
        auto Items = M.Backpack(); TestEqual(TEXT("不截断不堆叠，只取背包"), Items.Num(), Case == TEXT("TenThousand") ? 10000 : 3);
        if (!Items.IsEmpty()) { TestEqual(TEXT("末件可访问"), Items.Last(), V.Items.Last().ItemId); }
        return true;
    }
    if (Case == TEXT("PhaseAlive"))
    {
        TestTrue(TEXT("准备允许意图"), M.CanInteract(Preparing)); TestTrue(TEXT("战斗允许意图"), M.CanInteract(EY01MatchPhase::Battle));
        TestFalse(TEXT("等待禁用"), M.CanInteract(EY01MatchPhase::Waiting)); TestFalse(TEXT("结束禁用"), M.CanInteract(EY01MatchPhase::Ended));
        V.Health = 0; ++V.ViewRevision; M.Sync(V); TestFalse(TEXT("死亡禁用"), M.CanInteract(Preparing)); return true;
    }
    if (Case == TEXT("Confirm") || Case == TEXT("Cancel") || Case == TEXT("StaleTicket"))
    {
        V.Tickets.Add(Ticket(V)); V.Response.TicketId = V.Tickets[0].TicketId; V.ResponseSerial = 1; ++V.ViewRevision; M.Sync(V);
        TestTrue(TEXT("服务器票据才弹确认"), M.NeedsConfirmation());
        if (Case == TEXT("StaleTicket"))
        {
            V.Tickets.Reset(); ++V.ViewRevision; M.Sync(V); TestFalse(TEXT("失效票据关闭确认"), M.NeedsConfirmation());
            TestFalse(TEXT("没有票据不能凭空确认"), M.Decide(true, Preparing, R)); return true;
        }
        TestTrue(TEXT("确认或取消可构造"), M.Decide(Case == TEXT("Confirm"), Preparing, R));
        auto Expected = V.Tickets[0]; Expected.Seq = V.ExpectedSeq; Expected.Operation = Case == TEXT("Confirm") ? O::ConfirmLearn : O::CancelLearn;
        TestTrue(TEXT("完整原票据，不篡改目标/书/版本"), FY01ItemRequest::StaticStruct()->CompareScriptStruct(&R, &Expected, 0)); return true;
    }
    if (Case == TEXT("Remove"))
    {
        TestTrue(TEXT("已学槽请求移除"), M.Remove(0, Preparing, R)); TestEqual(TEXT("移除意图"), R.Operation, O::RemoveSkill);
        TestEqual(TEXT("绑定已学身份"), R.ExpectedLearnedId, V.Skills[0].LearnedId); TestFalse(TEXT("不携带返书"), R.ItemId.IsValid());
        TestFalse(TEXT("空槽不移除"), M.Remove(1, Preparing, R)); return true;
    }
    if (Case == TEXT("UnknownCooldown") || Case == TEXT("InvalidDeadline"))
    {
        if (Case == TEXT("InvalidDeadline")) { V.Skills[0].Deadline = -1; ++V.ViewRevision; M.Sync(V); }
        const double Time = Case == TEXT("UnknownCooldown") ? -1. : 11.;
        TestTrue(TEXT("未知时间或非法截止不伪报就绪"), M.SkillLabel(0, Time).Contains(TEXT("同步")));
        TestEqual(TEXT("未知剩余使用负值哨兵"), M.Remaining(V.Skills[0], Time), -1.); return true;
    }
    if (Case == TEXT("Cooldown") || Case == TEXT("RelearnReady"))
    {
        TestEqual(TEXT("同步时间而非本地起点"), M.Remaining(V.Skills[0], 11.25), 6.75);
        TestEqual(TEXT("截止后就绪"), M.Remaining(V.Skills[0], 20), 0.);
        if (Case == TEXT("RelearnReady")) { V.Skills[0].LearnedId = FGuid::NewGuid(); V.Skills[0].Deadline = 0; ++V.ViewRevision; M.Sync(V); }
        TestTrue(TEXT("槽内可见CD或就绪"), M.SkillLabel(0, 11.25).Contains(Case == TEXT("RelearnReady") ? TEXT("就绪") : TEXT("6.8"))); return true;
    }
    if (Case == TEXT("NoInternalIds") || Case == TEXT("SameNames"))
    {
        TestTrue(TEXT("名称显示"), M.ItemLabel(V.Items[0].ItemId).Contains(TEXT("同名内容")));
        TestFalse(TEXT("不显示定义身份"), M.ItemLabel(V.Items[0].ItemId).Contains(TEXT("Hidden")));
        TestFalse(TEXT("不显示已学身份"), M.SkillLabel(0, 11).Contains(TEXT("Hidden")));
        TestEqual(TEXT("同名不合并"), M.Backpack().Num(), 3); return true;
    }
    if (Case == TEXT("NoLearnedBackpack"))
    { TestFalse(TEXT("已学技能不生成回包拖动"), M.Drag(Z::Skill, V.Skills[0].LearnedId, 0, D)); TestFalse(TEXT("不伪造回包意图"), M.Drop(D, Z::Backpack, -1, Preparing, R)); return true; }
    const bool Equipped = Case == TEXT("Unequip") || Case == TEXT("NoDirectSwap");
    const bool Book = Case == TEXT("EmptyLearn") || Case == TEXT("OccupiedLearn") || Case == TEXT("NoBookEquipment") || Case == TEXT("DuplicateIntent");
    const int32 ItemIndex = Equipped ? 1 : Book ? 2 : 0;
    TestTrue(TEXT("拖动捕获显示中的实例身份"), M.Drag(Equipped ? Z::Equipment : Z::Backpack, V.Items[ItemIndex].ItemId, Equipped ? 0 : -1, D));
    if (Case == TEXT("StaleDrag") || Case == TEXT("ForeignDrag"))
    {
        if (Case == TEXT("StaleDrag")) { ++V.Items[0].EntityRevision; ++V.ViewRevision; M.Sync(V); }
        else { D.ConnectionId = FGuid::NewGuid(); }
        TestFalse(TEXT("不把旧拖动偷换成新实例意图"), M.Drop(D, Z::Equipment, 1, Preparing, R)); return true;
    }
    if (Case == TEXT("SixFourBounds"))
    {
        TestTrue(TEXT("第六装备槽可用"), M.Drop(D, Z::Equipment, 5, Preparing, R)); TestFalse(TEXT("第七槽不可用"), M.Drop(D, Z::Equipment, 6, Preparing, R));
        M.Drag(Z::Backpack, V.Items[2].ItemId, -1, D); TestTrue(TEXT("第四技能槽可用"), M.Drop(D, Z::Skill, 3, Preparing, R));
        TestFalse(TEXT("第五技能槽不可用"), M.Drop(D, Z::Skill, 4, Preparing, R)); return true;
    }
    const Z Target = Case == TEXT("Unequip") ? Z::Backpack : (Book && Case != TEXT("NoBookEquipment")) || Case == TEXT("NoEquipmentSkill") ? Z::Skill : Z::Equipment;
    const int32 Slot = Case == TEXT("Replace") || Case == TEXT("OccupiedLearn") || Case == TEXT("DuplicateIntent") ? 0 : 1;
    if (Case == TEXT("NoDirectSwap") || Case == TEXT("NoBookEquipment") || Case == TEXT("NoEquipmentSkill"))
    { TestFalse(TEXT("类型/方向禁止"), M.Drop(D, Target, Slot, Preparing, R)); return true; }
    TestTrue(TEXT("合法拖动只构造请求"), M.Drop(D, Target, Slot, Preparing, R));
    TestEqual(TEXT("取权威序号"), R.Seq, V.ExpectedSeq); TestEqual(TEXT("会话"), R.MatchId, V.MatchId);
    TestEqual(TEXT("连接"), R.JoinSequence, V.JoinSequence); TestEqual(TEXT("配置"), R.ConfigRevision, V.ConfigRevision);
    TestEqual(TEXT("实例版本"), R.ExpectedEntityRevision, V.Items[ItemIndex].EntityRevision);
    TestEqual(TEXT("意图类型"), R.Operation, Equipped ? O::Unequip : Book ? O::PrepareLearn : O::Equip);
    if (Case == TEXT("Replace")) { TestEqual(TEXT("旧件身份"), R.ExpectedSlotItemId, V.EquipmentSlots[0]); TestEqual(TEXT("旧件版本"), R.ExpectedSlotEntityRevision, V.Items[1].EntityRevision); }
    if (Case == TEXT("OccupiedLearn") || Case == TEXT("DuplicateIntent"))
    { TestEqual(TEXT("目标已学身份"), R.ExpectedLearnedId, V.Skills[0].LearnedId); TestFalse(TEXT("不能本地假造确认成功"), M.NeedsConfirmation()); }
    if (Case == TEXT("PendingNoOptimism") || Case == TEXT("UnrelatedResponse") || Case == TEXT("FailureReceipt") || Case == TEXT("ExactResponse") || Case == TEXT("SessionReset"))
    {
        M.Sent(R); FY01ItemRequest Blocked; TestTrue(TEXT("发送后等待服务器"), M.IsPending()); TestFalse(TEXT("在途不复用同号"), M.Drop(D, Z::Equipment, 1, Preparing, Blocked));
        TestEqual(TEXT("发送不移动背包"), M.Backpack().Num(), 3);
        TestTrue(TEXT("发送不填槽"), M.View().EquipmentSlots.IsValidIndex(1) && !M.View().EquipmentSlots[1].IsValid());
        if (Case == TEXT("SessionReset")) { V.MatchId = FGuid::NewGuid(); V.ViewRevision = 1; M.Sync(V); TestFalse(TEXT("新会话不残留请求"), M.IsPending()); return true; }
        if (Case != TEXT("PendingNoOptimism"))
        {
            V.Response.Request = R; V.ResponseSerial = 1; ++V.ViewRevision;
            if (Case == TEXT("UnrelatedResponse")) { ++V.Response.Request.Seq; }
            if (Case == TEXT("FailureReceipt")) { V.Response.Code = EY01ItemError::Busy; V.Response.Message = TEXT("HiddenDefinition0"); }
            M.Sync(V); TestEqual(TEXT("仅准确响应解除等待"), M.IsPending(), Case == TEXT("UnrelatedResponse"));
            if (Case == TEXT("FailureReceipt")) { TestTrue(TEXT("权威拒绝可见"), M.Status().Contains(TEXT("忙"))); TestFalse(TEXT("内部诊断不泄漏ID"), M.Status().Contains(TEXT("Hidden"))); }
        }
    }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36EconomyUIModelTest, "Y01.Framework.R36.RuntimeIntegration.UIModel",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36EconomyUIModelTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Purchase"), TEXT("ArbitraryOffer"), TEXT("Purchased"), TEXT("UnknownOffer"), TEXT("NoFunds"),
        TEXT("Talent"), TEXT("Chosen"), TEXT("UnknownTalent"), TEXT("FiveMaterials"), TEXT("FourMaterials"), TEXT("SixthMaterial"),
        TEXT("MixedQuality"), TEXT("MixedType"), TEXT("EquippedMaterial"), TEXT("StaleMaterial"), TEXT("SessionMaterials"),
        TEXT("ToggleOff"), TEXT("NoRecipe"), TEXT("Pending"), TEXT("ExactReceipt"), TEXT("Waiting"), TEXT("Ended"), TEXT("Dead")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36EconomyUIModelTest::RunTest(const FString& Case)
{
    auto V = Fixture(); V.Gold = 200; V.Items.Reset(); V.EquipmentSlots.Init(FGuid(), 6);
    for (int32 I = 0; I < 6; ++I)
    {
        auto& Item = V.Items.AddDefaulted_GetRef(); Item.ItemId = FGuid::NewGuid(); Item.EntityRevision = I + 5;
        Item.Definition.Type = EY01TableKind::Equipment; Item.Definition.RowName = TEXT("HiddenEquipment");
    }
    auto& Offer = V.ShopOffers.AddDefaulted_GetRef(); Offer.OfferId = TEXT("ArbitraryNoActionId"); Offer.Price = 10;
    Offer.DisplayName = FText::FromString(TEXT("测试商品"));
    auto& Recipe = V.SynthesisRecipes.AddDefaulted_GetRef(); Recipe.RecipeId = TEXT("HiddenRecipe");
    V.TalentChoice.ChoiceId = FGuid::NewGuid();
    for (int32 I = 0; I < 3; ++I)
    { auto& T = V.TalentChoice.Candidates.AddDefaulted_GetRef(); T.TalentId = FName(*FString::Printf(TEXT("HiddenTalent%d"), I)); }
    if (Case == TEXT("Purchased")) { Offer.bPurchased = true; }
    if (Case == TEXT("Chosen")) { V.TalentChoice.SelectedTalent = V.TalentChoice.Candidates[0].TalentId; }
    if (Case == TEXT("NoFunds")) { V.Gold = 0; }
    if (Case == TEXT("Dead")) { V.Health = 0; }
    if (Case == TEXT("MixedQuality")) { V.Items[4].Display.Quality = EY01Quality::Blue; }
    if (Case == TEXT("MixedType")) { V.Items[4].Definition.Type = EY01TableKind::Skill; }
    if (Case == TEXT("EquippedMaterial")) { V.Items[4].Location = EY01ItemLocation::EquipmentSlot; }
    if (Case == TEXT("NoRecipe")) { V.SynthesisRecipes.Reset(); }
    FY01R36UIModel M; M.Sync(V); FY01ItemRequest R;
    const auto Phase = Case == TEXT("Waiting") ? EY01MatchPhase::Waiting : Case == TEXT("Ended") ? EY01MatchPhase::Ended : Preparing;
    if (Case == TEXT("Waiting") || Case == TEXT("Ended") || Case == TEXT("Dead"))
    {
        TestFalse(TEXT("禁用阶段不买"), M.Purchase(Offer.OfferId, Phase, R));
        TestFalse(TEXT("禁用阶段不选天赋"), M.ChooseTalent(V.TalentChoice.Candidates[0].TalentId, Phase, R));
        TestFalse(TEXT("禁用阶段不选材料"), M.ToggleMaterial(V.Items[0].ItemId, Phase));
        TestFalse(TEXT("禁用阶段不合成"), M.Synthesize(Phase, R)); return true;
    }
    if (Case == TEXT("Talent") || Case == TEXT("Chosen") || Case == TEXT("UnknownTalent"))
    {
        const bool Ok = M.ChooseTalent(Case == TEXT("UnknownTalent") ? FName(TEXT("Foreign")) : V.TalentChoice.Candidates[0].TalentId, Phase, R);
        TestEqual(TEXT("只选本次未选择候选"), Ok, Case == TEXT("Talent"));
        if (Ok) { TestEqual(TEXT("绑定本人ChoiceId"), R.TalentChoiceId, V.TalentChoice.ChoiceId); TestEqual(TEXT("天赋意图"), R.Operation, O::ChooseTalent); }
        TestTrue(TEXT("候选不提前生效"), M.View().TalentChoice.SelectedTalent.IsNone() == V.TalentChoice.SelectedTalent.IsNone()); return true;
    }
    if (Case == TEXT("Purchase") || Case == TEXT("ArbitraryOffer") || Case == TEXT("Purchased") || Case == TEXT("UnknownOffer")
        || Case == TEXT("NoFunds") || Case == TEXT("Pending") || Case == TEXT("ExactReceipt"))
    {
        const bool Ok = M.Purchase(Case == TEXT("UnknownOffer") ? FName(TEXT("Foreign")) : Offer.OfferId, Phase, R);
        TestEqual(TEXT("商品按已提交数据驱动，不按Action白名单"), Ok, Case != TEXT("Purchased") && Case != TEXT("UnknownOffer") && Case != TEXT("NoFunds"));
        if (!Ok) { return true; }
        TestEqual(TEXT("购买意图"), R.Operation, O::Purchase); TestEqual(TEXT("商品引用"), R.Offer.RowName, Offer.OfferId);
        TestEqual(TEXT("完整视图会话"), R.MatchId, V.MatchId); TestEqual(TEXT("连接"), R.JoinSequence, V.JoinSequence);
        TestEqual(TEXT("配置版本"), R.ConfigRevision, V.ConfigRevision); TestEqual(TEXT("权威序号"), R.Seq, V.ExpectedSeq);
        TestEqual(TEXT("无槽目标"), R.Slot, INDEX_NONE); M.Sent(R);
        TestEqual(TEXT("不乐观扣金"), M.View().Gold, 200); TestEqual(TEXT("不提前奖励"), M.View().Items.Num(), 6);
        FY01ItemRequest Another; TestFalse(TEXT("在途不复用Seq"), M.Purchase(Offer.OfferId, Phase, Another));
        TestFalse(TEXT("在途不选择"), M.ChooseTalent(V.TalentChoice.Candidates[0].TalentId, Phase, Another));
        if (Case == TEXT("ExactReceipt"))
        {
            ++V.ViewRevision; ++V.ResponseSerial; V.Response.Request = R; V.Response.Request.Offer.RowName = TEXT("Foreign"); M.Sync(V);
            TestTrue(TEXT("不同商品回执不解锁"), M.IsPending()); ++V.ViewRevision; ++V.ResponseSerial; V.Response.Request = R;
            V.Response.Code = EY01ItemError::PurchaseLimit; M.Sync(V);
            TestFalse(TEXT("精确拒绝解锁"), M.IsPending()); TestTrue(TEXT("限购拒绝可理解"), M.Status().Contains(TEXT("已购买")));
        }
        return true;
    }
    const int32 Count = Case == TEXT("FourMaterials") ? 4 : 5;
    for (int32 I = 0; I < Count; ++I)
    {
        const bool Valid = I != 4 || (Case != TEXT("MixedQuality") && Case != TEXT("MixedType") && Case != TEXT("EquippedMaterial"));
        TestEqual(TEXT("真实材料选择检查包内同类同品质"), M.ToggleMaterial(V.Items[I].ItemId, Phase), Valid);
    }
    if (Case == TEXT("SixthMaterial")) { TestFalse(TEXT("只能选择五件而非限制背包容量"), M.ToggleMaterial(V.Items[5].ItemId, Phase)); }
    if (Case == TEXT("ToggleOff")) { TestTrue(TEXT("再点取消选择"), M.ToggleMaterial(V.Items[0].ItemId, Phase)); }
    if (Case == TEXT("StaleMaterial")) { ++V.Items[0].EntityRevision; ++V.ViewRevision; M.Sync(V); }
    if (Case == TEXT("SessionMaterials")) { V.ConnectionId = FGuid::NewGuid(); V.ViewRevision = 1; M.Sync(V); }
    const bool Valid = Case == TEXT("FiveMaterials") || Case == TEXT("SixthMaterial");
    const bool Built = M.Synthesize(Phase, R);
    TestEqual(TEXT("五件完整有效材料及已公开配方才合成"), Built, Valid);
    if (Built)
    {
        TestEqual(TEXT("免费合成意图"), R.Operation, O::Synthesize); TestEqual(TEXT("五个实例版本"), R.Materials.Num(), 5);
        TestEqual(TEXT("公开配方"), R.Recipe.RowName, Recipe.RecipeId);
        if (R.Materials.Num() == 5) { TestEqual(TEXT("材料原版本"), R.Materials[0].ExpectedEntityRevision, V.Items[0].EntityRevision); }
        M.Sent(R); TestEqual(TEXT("等待期间材料不消失"), M.View().Items.Num(), 6); TestEqual(TEXT("免费不扣余额"), M.View().Gold, 200);
    }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36PresentationModelTest, "Y01.Framework.R36.Presentation.Model",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36PresentationModelTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("FlightClock"), TEXT("Quality"), TEXT("NoAnimationReward"), TEXT("DisableFlight"),
        TEXT("AuthoritativeArrival"), TEXT("Unopened"), TEXT("Unscrolled"), TEXT("FlashDuration"), TEXT("RepeatPublish"),
        TEXT("WidgetReuse"), TEXT("PrunedReceipt"), TEXT("SessionReset"), TEXT("JoinReset"), TEXT("ConnectionReset"),
        TEXT("RemovedItem"), TEXT("PendingNoFlash"), TEXT("FaultNoFlash"), TEXT("CancelledNoFlash"), TEXT("FiveHundred")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36PresentationModelTest::RunTest(const FString& Case)
{
    auto V = Fixture(); V.Items.Reset(); V.EquipmentSlots.Init(FGuid(), 6); V.Delivery.bEnabled = true;
    auto& R = V.Delivery.Records.AddDefaulted_GetRef(); R.DeliveryId = FGuid::NewGuid(); R.ItemId = FGuid::NewGuid();
    R.KillEventId = FGuid::NewGuid(); R.MatchId = V.MatchId; R.JoinSequence = V.JoinSequence;
    R.StartTime = 10; R.ETA = 11; R.Origin = FVector(12,34,56); R.Quality = EY01Quality::Purple;
    const auto ItemId = R.ItemId; const auto DeliveryId = R.DeliveryId;
    FY01R36UIModel M; M.Sync(V);
    if (Case == TEXT("FlightClock") || Case == TEXT("Quality") || Case == TEXT("DisableFlight") || Case == TEXT("NoAnimationReward"))
    {
        const auto Flights = M.Flights(10.25, true);
        if (TestEqual(TEXT("同步世界时间在ETA前产生单件飞行"), Flights.Num(), 1))
        {
            TestEqual(TEXT("进度只用权威Start/ETA"), Flights[0].Alpha, .25);
            TestEqual(TEXT("保留品质"), Flights[0].Record.Quality, EY01Quality::Purple);
            TestEqual(TEXT("保留怪物世界Origin"), Flights[0].Record.Origin, R.Origin);
        }
        TestTrue(TEXT("禁播只取消视觉"), M.Flights(10.5, false).IsEmpty());
        TestTrue(TEXT("到ETA不重启本地飞行"), M.Flights(11, true).IsEmpty());
        TestTrue(TEXT("未来记录不提前播"), M.Flights(9, true).IsEmpty());
        TestEqual(TEXT("飞行结束不能领奖"), M.View().Items.Num(), 0);
        TestEqual(TEXT("视觉不能发送交易"), M.View().ExpectedSeq, V.ExpectedSeq); return true;
    }
    if (Case == TEXT("PendingNoFlash") || Case == TEXT("FaultNoFlash") || Case == TEXT("CancelledNoFlash"))
    {
        R.State = Case == TEXT("FaultNoFlash") ? EY01DeliveryState::Fault : Case == TEXT("CancelledNoFlash") ? EY01DeliveryState::Cancelled : EY01DeliveryState::Pending;
        ++V.ViewRevision; M.Sync(V); M.VisualTick(20, {ItemId});
        TestEqual(TEXT("仅Record或过ETA不能伪造到货提示"), M.UnseenArrivals(), 0);
        TestEqual(TEXT("无权威物品到货标记不闪"), M.FlashRemaining(ItemId, 20), 0.); return true;
    }
    R.State = EY01DeliveryState::Delivered; R.DeliveredTime = 11;
    auto& Item = V.Items.AddDefaulted_GetRef(); Item.ItemId = ItemId; Item.ArrivalDeliveryId = DeliveryId;
    Item.EntityRevision = 1; Item.AcquisitionOrder = 1; Item.Display.Name = FText::FromString(TEXT("测试到货"));
    if (Case == TEXT("PrunedReceipt")) { V.Delivery.Records.Reset(); }
    ++V.ViewRevision; M.Sync(V);
    TestEqual(TEXT("到货后尚未显示保留一次提示"), M.UnseenArrivals(), 1);
    M.VisualTick(20, {}); TestEqual(TEXT("时间流逝不能吞掉未打开/未滚到提示"), M.UnseenArrivals(), 1);
    if (Case == TEXT("FiveHundred"))
    {
        for (int32 I = 1; I < 500; ++I)
        { auto Copy = V.Items[0]; Copy.ItemId = FGuid::NewGuid(); Copy.ArrivalDeliveryId = FGuid::NewGuid(); V.Items.Add(Copy); }
        ++V.ViewRevision; M.Sync(V); TestEqual(TEXT("500件不设硬容量且逐件保留提示"), M.UnseenArrivals(), 500); return true;
    }
    if (Case == TEXT("RemovedItem"))
    { V.Items.Reset(); ++V.ViewRevision; M.Sync(V); TestEqual(TEXT("已消耗物品不残留提示"), M.UnseenArrivals(), 0); return true; }
    M.VisualTick(20, {ItemId}); TestEqual(TEXT("首次实际可见才启动0.6秒"), M.FlashRemaining(ItemId, 20), .6);
    TestEqual(TEXT("首次显示消费提示"), M.UnseenArrivals(), 0);
    TestTrue(TEXT("0.59秒仍闪"), M.FlashRemaining(ItemId, 20.59) > 0);
    TestEqual(TEXT("0.6秒截止"), M.FlashRemaining(ItemId, 20.6), 0.);
    M.VisualTick(21, {}); ++V.ViewRevision; M.Sync(V); M.VisualTick(22, {ItemId});
    TestEqual(TEXT("Publish和控件复用不能重闪旧物"), M.FlashRemaining(ItemId, 22), 0.);
    if (Case == TEXT("SessionReset") || Case == TEXT("JoinReset") || Case == TEXT("ConnectionReset"))
    {
        if (Case == TEXT("SessionReset")) { V.MatchId = FGuid::NewGuid(); }
        if (Case == TEXT("JoinReset")) { ++V.JoinSequence; }
        if (Case == TEXT("ConnectionReset")) { V.ConnectionId = FGuid::NewGuid(); }
        V.ViewRevision = 1; M.Sync(V); TestEqual(TEXT("新会话不能继承旧显示消费记录"), M.UnseenArrivals(), 1);
    }
    TestEqual(TEXT("所有表现调用不改变库存"), M.View().Items.Num(), 1); return true;
}
#endif
