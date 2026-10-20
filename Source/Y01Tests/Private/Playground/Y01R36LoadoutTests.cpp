#include "Misc/AutomationTest.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01PlayerController.h"
#include "Player/Y01Character.h"
#include "Y01MatchFixture.h"
#include "Match/Y01GameFlow.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformProcess.h"
#include "UObject/UnrealType.h"
#include "Playground/Y01TestSkill.h"
#include "Misc/ScopeExit.h"
#include <limits>
#include "Player/Y01EconomyComponent.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Misc/Crc.h"
#include "Combat/Y01DamageEffects.h"
#include "Player/Y01DamageEffectsComponent.h"
#include "Player/Y01GrowthComponent.h"
#include "Match/Y01KillRewards.h"
#include "Combat/Y01Monster.h"
#include "Combat/Y01DamageEvents.h"
#include "Playground/Y01R36Profile.h"

// 初始红阶段只断言真实PlayerState缺少注册表，不提供任何假业务API。
#if __has_include("Player/Y01LoadoutComponent.h")
#include "Player/Y01LoadoutComponent.h"
#define Y01_LOADOUT_PRESENT 1
#else
#define Y01_LOADOUT_PRESENT 0
#endif
#if WITH_EDITOR
#include "Playground/Y01PlaygroundSeed.h"
#include "Playground/Y01PlaygroundController.h"
#endif

#ifndef Y01_LOADOUT_LEARNING
#define Y01_LOADOUT_LEARNING 0
#endif

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
using K = EY01TableKind;
struct FLoadoutWorld
{
    FY01MatchFixture Data;
    UWorld* World;
    AY01GameMode* Mode;
    AY01PlayerController* PC;
    AY01PlayerState* P;
    FGuid Match = FGuid::NewGuid();
    FString Error;
    FLoadoutWorld()
    {
        auto& C = Data.Row<FY01CharacterRow>(K::Character);
        C.InitialEquipment.Reset(); C.InitialSkills.Reset(); C.AttackDamage = 30;
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01GameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01GameMode>(); PC = Player(); P = PC->GetPlayerState<AY01PlayerState>();
        Mode->TryConfigureMatch(FY01DataCatalog(Data.Tables), Data.Waves(), FY01MatchFixture::Ref(K::Character), Error);
        Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, 10);
        Effect(TEXT("Other"), EY01SourceEffectKind::AttackFlat, 7);
        Equipment(TEXT("Sword"), {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten"))});
        Equipment(TEXT("Double"), {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten")), FY01MatchFixture::Ref(K::SourceEffect, TEXT("Other"))});
        FY01GrowthRuleRow G; G.Delta = 3; Data.Add(K::GrowthRule, G);
        auto& Sword = Data.Row<FY01EquipmentRow>(K::Equipment, TEXT("Sword"));
        Sword.GrowthRuleIds.Add(FY01MatchFixture::Ref(K::GrowthRule));
        FY01EquipmentRow OnlyGrowth; OnlyGrowth.GrowthRuleIds = Sword.GrowthRuleIds;
        Data.Add(K::Equipment, OnlyGrowth, TEXT("Growth"));
    }
    ~FLoadoutWorld()
    { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    AY01PlayerController* Player()
    {
        auto* C = World->SpawnActor<AY01PlayerController>(); auto* S = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(0, 0, 100), FRotator::ZeroRotator);
        C->PlayerState = S; C->Possess(Pawn); S->GetCombatASC()->InitAbilityActorInfo(S, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(S); return C;
    }
    void Effect(FName Name, EY01SourceEffectKind Kind, double Value)
    { FY01SourceEffectRow E; E.Kind = Kind; E.Magnitude = Value; Data.Add(K::SourceEffect, E, Name); }
    void Equipment(FName Name, TArray<FY01DataRef> Refs)
    { FY01EquipmentRow E; E.EffectIds = MoveTemp(Refs); Data.Add(K::Equipment, E, Name); }
    UY01CombatAbilitySystem* ASC() const { return P->GetCombatASC(); }
    float Attack() const { return ASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()); }
#if Y01_LOADOUT_PRESENT
    UY01LoadoutComponent* L() const { return P->GetLoadout(); }
    FY01ItemResult Init() { return L()->Initialize(Match, 1); }
    FY01ItemGrant Grant(FName Name = TEXT("Sword"), K Type = K::Equipment) const
    { FY01ItemGrant G; G.ItemId = FGuid::NewGuid(); G.Definition = FY01MatchFixture::Ref(Type, Name); return G; }
    FY01ItemResult Add(const TArray<FY01ItemGrant>& Grants) { return L()->AddItems(FY01DataCatalog(Data.Tables), Grants); }
    FY01LoadoutSnapshot View() const { FY01LoadoutSnapshot V; L()->Observe(V); return V; }
    FY01ItemInstance Item(FGuid Id) const { FY01ItemInstance I; L()->FindItem(Id, I); return I; }
    FY01ItemRequest Request(FGuid Id, int32 Slot, EY01ItemOperation Op = EY01ItemOperation::Equip) const
    {
        const auto V = View(); FY01ItemRequest R;
        R.MatchId = Match; R.JoinSequence = 1; R.Seq = V.ExpectedSeq; R.Operation = Op;
        R.ItemId = Id; R.ExpectedEntityRevision = Item(Id).EntityRevision; R.Slot = Slot;
        if (V.EquipmentSlots.IsValidIndex(Slot))
        {
            R.ExpectedSlotItemId = V.EquipmentSlots[Slot];
            if (R.ExpectedSlotItemId.IsValid()) { R.ExpectedSlotEntityRevision = Item(R.ExpectedSlotItemId).EntityRevision; }
        }
        return R;
    }
    FY01ItemResult Send(const FY01ItemRequest& R, AController* Sender = nullptr)
    { return R.Operation == EY01ItemOperation::Unequip ? L()->TryUnequip(Sender ? Sender : PC, R) : L()->TryEquip(Sender ? Sender : PC, R); }
#if Y01_LOADOUT_LEARNING
    void Book(FName Id, bool Active = true)
    {
        FY01SkillRow S; S.DisplayName = FText::FromString(TEXT("相同显示名"));
        S.Type = Active ? EY01SkillType::Active : EY01SkillType::Passive;
        if (Active) { S.Ability = UY01BasicAttackAbility::StaticClass(); S.CooldownSeconds = 8; }
        S.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten"))}; Data.Add(K::Skill, S, Id);
    }
    FY01ItemRequest LearnRequest(FGuid Id, int32 Slot, EY01ItemOperation Op = EY01ItemOperation::PrepareLearn) const
    {
        const auto V = View(); FY01ItemRequest R;
        R.MatchId = Match; R.JoinSequence = 1; R.Seq = V.ExpectedSeq; R.Operation = Op;
        R.ItemId = Id; R.ExpectedEntityRevision = Id.IsValid() ? Item(Id).EntityRevision : 0; R.Slot = Slot;
        R.ConfigRevision = V.ConfigRevision;
        if (V.Skills.IsValidIndex(Slot))
        { R.ExpectedLearnedId = V.Skills[Slot].LearnedId; R.ExpectedLearnedRevision = V.Skills[Slot].EntityRevision; }
        return R;
    }
    FY01ItemResult Learn(const FY01ItemRequest& R, AController* Sender = nullptr)
    {
        auto* C = Sender ? Sender : PC;
        switch (R.Operation)
        {
        case EY01ItemOperation::PrepareLearn: return L()->PrepareLearn(C, R);
        case EY01ItemOperation::ConfirmLearn: return L()->ConfirmLearn(C, R);
        case EY01ItemOperation::CancelLearn: return L()->CancelLearn(C, R);
        default: return L()->RemoveSkill(C, R);
        }
    }
    FY01ItemRequest Confirm(FY01ItemRequest R, FGuid Ticket) const
    { R.Seq = View().ExpectedSeq; R.Operation = EY01ItemOperation::ConfirmLearn; R.TicketId = Ticket; return R; }
#endif
#endif
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36LoadoutTest, "Y01.Framework.R36.Loadout",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36LoadoutTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("DefaultDisabled"), TEXT("InitializeIdempotent"), TEXT("SessionBinding"), TEXT("EquipUnequip"),
        TEXT("SixInstances"), TEXT("SameInstanceTwoSlots"), TEXT("SlotNegative"), TEXT("SlotSix"), TEXT("SlotHuge"),
        TEXT("BookCannotEquip"), TEXT("ForeignController"), TEXT("ForeignItem"), TEXT("DirectSlotMove"), TEXT("Replace"),
        TEXT("ReplaySame"), TEXT("ReplayConflict"), TEXT("ReplayRing64"), TEXT("SequenceGap"), TEXT("SequenceZero"),
        TEXT("BusinessFailureCached"), TEXT("CrossJoin"), TEXT("StaleEntity"), TEXT("StaleSlotItem"), TEXT("StaleSlotRevision"),
        TEXT("Waiting"), TEXT("Preparing"), TEXT("Battle"), TEXT("Ended"), TEXT("Dead"), TEXT("Client"),
        TEXT("DerivedGERejected"), TEXT("ReplacementRejected"), TEXT("Reentry.Apply"), TEXT("Reentry.Inhibit"),
        TEXT("Reentry.Remove"), TEXT("ObserveDuringStage"), TEXT("DeathDuringApply"), TEXT("EndDuringApply"),
        TEXT("EndDuringCommitDelete"), TEXT("DeathDuringCommitDelete"), TEXT("IncomeNotReverted"),
        TEXT("Capacity1000"), TEXT("Capacity10000"), TEXT("DuplicateItem"), TEXT("DuplicateBatch"), TEXT("BatchPartialFailure"),
        TEXT("MissingDefinition"), TEXT("WrongDefinitionType"), TEXT("WrongSchema"), TEXT("LegacyAttack"), TEXT("LegacyEffect"),
        TEXT("EffectMissing"), TEXT("EffectDuplicate"), TEXT("EffectNaN"), TEXT("GrowthMissing"), TEXT("GrowthInvalid"),
        TEXT("GrowthOnly"), TEXT("SnapshotsOwned"), TEXT("HugeSafeBackpack"), TEXT("BookNoAbility"), TEXT("ResultChanges"), TEXT("WireSchema"),
        TEXT("LegacyBypass"), TEXT("StaleReplacementItem"), TEXT("StaleReplacementRevision"), TEXT("RingBoundary64"),
        TEXT("ReplayAfterChange"), TEXT("CrossMatch"), TEXT("UnequipBackpack"), TEXT("InvalidOperation"), TEXT("SourceIdentity"),
        TEXT("InitializeInvalidIdentity"), TEXT("ForeignRegistry"), TEXT("PayloadOwned"), TEXT("BookEffectsDormant") })
    { Names.Add(N); Commands.Add(N); }
#if WITH_EDITOR
    Names.Add(TEXT("PersonalRoom")); Commands.Add(TEXT("PersonalRoom"));
#endif
}
bool FY01R36LoadoutTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FLoadoutWorld F;
    if (!TestTrue(TEXT("真实权威Mode目录配置成功"), F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    UActorComponent* Registry = nullptr;
    for (auto* C : F.P->GetComponents())
    { if (C && C->GetClass()->GetFName() == TEXT("Y01LoadoutComponent")) { Registry = C; } }
    if (!TestNotNull(TEXT("PlayerState必须拥有唯一本局物品注册表"), Registry)) { return false; }
#if Y01_LOADOUT_PRESENT
    using E = EY01ItemError;
    auto Ok = [&](const TCHAR* M, const FY01ItemResult& R)
    { if (!R.IsOk()) { AddInfo(R.Message); } return TestTrue(M, R.IsOk()); };
    auto Code = [&](const TCHAR* M, const FY01ItemResult& R, E Expected) { TestEqual(M, R.Code, Expected); };
    auto A = F.Grant(), B = F.Grant();
    if (Case == TEXT("DefaultDisabled"))
    {
        TestFalse(TEXT("默认关闭"), F.L()->IsActive()); TestFalse(TEXT("不Tick"), F.L()->PrimaryComponentTick.bCanEverTick);
        FY01LoadoutSnapshot V; Code(TEXT("默认不可观察"), F.L()->Observe(V), E::Disabled);
        Code(TEXT("默认不可入包"), F.Add({A}), E::Disabled);
        FY01ItemRequest R; Code(TEXT("默认不可装备"), F.Send(R), E::Disabled); return true;
    }
    if (Case == TEXT("SessionBinding"))
    {
        TestTrue(TEXT("已有真实PlayerState来源会话"), F.P->GetSourceEffects()->Initialize(F.Match, 1).IsOk());
        Code(TEXT("不同Match不可覆盖真实会话"), F.L()->Initialize(FGuid::NewGuid(), 1), E::Session);
        Code(TEXT("不同Join不可覆盖真实会话"), F.L()->Initialize(F.Match, 2), E::Session);
        Ok(TEXT("正确会话仍可初始化"), F.Init()); return true;
    }
    if (Case == TEXT("InitializeInvalidIdentity"))
    {
        Code(TEXT("非法Match拒绝"), F.L()->Initialize(FGuid(), 1), E::Session);
        Code(TEXT("非法Join拒绝"), F.L()->Initialize(F.Match, 0), E::Session);
    }
    if (Case == TEXT("ForeignRegistry"))
    {
        auto* Rogue = NewObject<UY01LoadoutComponent>(F.P);
        Code(TEXT("不能在同PlayerState创建第二注册表"), Rogue->Initialize(F.Match, 1), E::Authority); return true;
    }
    if (!Ok(TEXT("显式绑定服务器会话"), F.Init())) { return false; }
    if (Case == TEXT("InitializeInvalidIdentity")) { return true; }
    if (Case == TEXT("WireSchema"))
    {
        TSet<FName> Fields;
        for (TFieldIterator<FProperty> I(FY01ItemRequest::StaticStruct()); I; ++I) { Fields.Add(I->GetFName()); }
#if Y01_LOADOUT_LEARNING
        TestEqual(TEXT("请求只有十八个权威意图字段，无价格属性或owner"), Fields.Num(), 18);
        for (const TCHAR* Name : {TEXT("Recipe"), TEXT("Talent"), TEXT("TalentChoiceId")})
        { TestTrue(TEXT("新增仅为配方天赋和机会身份"), Fields.Contains(Name)); }
        TestTrue(TEXT("商品只是静态引用意图"), Fields.Contains(TEXT("Offer")));
        TestTrue(TEXT("材料只是实体和版本意图"), Fields.Contains(TEXT("Materials")));
        for (const TCHAR* Name : {TEXT("ExpectedLearnedId"), TEXT("ExpectedLearnedRevision"), TEXT("ConfigRevision"), TEXT("TicketId")})
        { TestTrue(TEXT("新增字段仅为技能实体和确认意图"), Fields.Contains(FName(Name))); }
#else
        TestEqual(TEXT("请求只有九个权威意图字段"), Fields.Num(), 9);
#endif
        for (const TCHAR* Name : {TEXT("MatchId"),TEXT("JoinSequence"),TEXT("Seq"),TEXT("Operation"),TEXT("ItemId"),
            TEXT("ExpectedEntityRevision"),TEXT("Slot"),TEXT("ExpectedSlotItemId"),TEXT("ExpectedSlotEntityRevision")})
        { TestTrue(TEXT("请求字段可用于后续RPC"), Fields.Contains(FName(Name))); }
        TestNull(TEXT("内部入包不是RPC"), F.L()->FindFunction(TEXT("AddItems"))); return true;
    }
    if (Case.StartsWith(TEXT("Capacity")))
    {
        const int32 Count = Case == TEXT("Capacity1000") ? 1000 : 10000;
        TArray<FY01ItemGrant> Batch; Batch.Reserve(Count);
        for (int32 I = 0; I < Count; ++I) { Batch.Add(F.Grant()); }
        Ok(TEXT("无玩法容量批量入包"), F.Add(Batch)); const auto V = F.View();
        TestEqual(TEXT("所有条目可枚举"), V.Items.Num(), Count);
        TSet<FGuid> Seen;
        for (int32 I = 0; I < Count; ++I)
        {
            FY01ItemInstance Item;
            Ok(TEXT("每个ItemId独立可访问"), F.L()->FindItem(Batch[I].ItemId, Item)); Seen.Add(Item.ItemId);
            TestEqual(TEXT("稳定获取顺序"), Item.AcquisitionOrder, uint64(I + 1));
            TestEqual(TEXT("全部在包"), Item.Location, EY01ItemLocation::Backpack);
        }
        TestEqual(TEXT("不堆叠不截断"), Seen.Num(), Count); TestEqual(TEXT("入包无攻击"), F.Attack(), 30.f);
        F.Mode->TryStartNextWave(); Ok(TEXT("最后一件也能装备"), F.Send(F.Request(Batch.Last().ItemId, 5))); return true;
    }
    const bool Invalid = Case == TEXT("MissingDefinition") || Case == TEXT("WrongDefinitionType") || Case == TEXT("WrongSchema")
        || Case == TEXT("LegacyAttack") || Case == TEXT("LegacyEffect") || Case.StartsWith(TEXT("Effect")) || Case == TEXT("GrowthMissing") || Case == TEXT("GrowthInvalid")
        || Case == TEXT("DuplicateBatch") || Case == TEXT("BatchPartialFailure");
    if (Invalid)
    {
        auto& Row = F.Data.Row<FY01EquipmentRow>(K::Equipment, TEXT("Sword"));
        if (Case == TEXT("MissingDefinition") || Case == TEXT("BatchPartialFailure")) { B.Definition.RowName = TEXT("Missing"); }
        if (Case == TEXT("WrongDefinitionType")) { B.Definition.Type = K::Monster; }
        if (Case == TEXT("WrongSchema")) { F.Data.Tables[K::Equipment] = F.Data.Tables[K::Skill]; }
        if (Case == TEXT("LegacyAttack")) { Row.AttackBonus = 10; }
        if (Case == TEXT("LegacyEffect")) { Row.Effect = UY01EquipmentAttackEffect::StaticClass(); }
        if (Case == TEXT("EffectMissing")) { Row.EffectIds[0].RowName = TEXT("Missing"); }
        if (Case == TEXT("EffectDuplicate")) { const auto Ref = Row.EffectIds[0]; Row.EffectIds.Add(Ref); }
        if (Case == TEXT("EffectNaN")) { F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, std::numeric_limits<double>::quiet_NaN()); }
        if (Case == TEXT("GrowthMissing")) { Row.GrowthRuleIds[0].RowName = TEXT("Missing"); }
        if (Case == TEXT("GrowthInvalid")) { F.Data.Row<FY01GrowthRuleRow>(K::GrowthRule).Attribute = EY01GrowthAttribute::Strength; }
        if (Case == TEXT("DuplicateBatch")) { B.ItemId = A.ItemId; }
        const auto Before = F.View(); TestFalse(TEXT("整批预检拒绝"), F.Add({A, B}).IsOk());
        TestEqual(TEXT("没有部分入包"), F.View().Items.Num(), 0); TestEqual(TEXT("失败版本不动"), F.View().Revision, Before.Revision);
        TestEqual(TEXT("没有GE"), F.Attack(), 30.f); return true;
    }
    if (Case == TEXT("BookCannotEquip") || Case == TEXT("BookNoAbility") || Case == TEXT("BookEffectsDormant")) { A = F.Grant(TEXT("Default"), K::Skill); }
    if (Case == TEXT("BookEffectsDormant"))
    {
        auto& Book = F.Data.Row<FY01SkillRow>(K::Skill);
        Book.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten"))};
        FY01GrowthRuleRow G; G.Target = EY01GrowthTarget::Player; G.Attribute = EY01GrowthAttribute::Strength; G.Delta = 5;
        F.Data.Add(K::GrowthRule, G, TEXT("BookGrowth")); Book.GrowthRuleIds = {FY01MatchFixture::Ref(K::GrowthRule, TEXT("BookGrowth"))};
    }
    if (Case == TEXT("GrowthOnly")) { A = F.Grant(TEXT("Growth")); }
    if (Case == TEXT("HugeSafeBackpack")) { F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, double(MAX_flt) * 2); }
    if (Case == TEXT("DerivedGERejected")) { A = F.Grant(TEXT("Double")); }
    const bool ChangedReplacement = Case == TEXT("ReplacementRejected") || Case == TEXT("IncomeNotReverted")
        || Case == TEXT("Reentry.Inhibit") || Case == TEXT("Reentry.Remove") || Case == TEXT("EndDuringCommitDelete")
        || Case == TEXT("DeathDuringCommitDelete");
    if (ChangedReplacement) { B = F.Grant(TEXT("Double")); }
    TArray<FGameplayAbilitySpecHandle> BeforeAbilities;
    for (const auto& Spec : F.ASC()->GetActivatableAbilities()) { BeforeAbilities.Add(Spec.Handle); }
    const auto Added = F.Add({A, B}); if (!Ok(TEXT("服务器列表入包"), Added)) { return false; }
    TestEqual(TEXT("两件入包不生效"), F.Attack(), 30.f);
    if (Case == TEXT("InitializeIdempotent"))
    {
        const auto V = F.View(); Ok(TEXT("同会话幂等"), F.Init());
        TestEqual(TEXT("不重置包"), F.View().Items.Num(), 2); TestEqual(TEXT("不重置版本"), F.View().Revision, V.Revision);
        Code(TEXT("跨会话拒绝"), F.L()->Initialize(FGuid::NewGuid(), 2), E::Session); return true;
    }
    if (Case == TEXT("DuplicateItem"))
    { Code(TEXT("已注册Id不重复领物"), F.Add({A}), E::DuplicateItem); TestEqual(TEXT("保持两件"), F.View().Items.Num(), 2); return true; }
    if (Case == TEXT("BookNoAbility") || Case == TEXT("BookEffectsDormant"))
    {
        TestEqual(TEXT("技能书不增加GA"), F.ASC()->GetActivatableAbilities().Num(), BeforeAbilities.Num());
        for (auto Handle : BeforeAbilities) { TestNotNull(TEXT("保留原普攻精确句柄"), F.ASC()->FindAbilitySpecFromHandle(Handle)); }
        if (Case == TEXT("BookEffectsDormant"))
        {
            const auto Book = F.Item(A.ItemId);
            TestEqual(TEXT("书复制常驻快照"), Book.Effects.Num(), 1); TestEqual(TEXT("书复制成长快照"), Book.Growth.Num(), 1);
            TestEqual(TEXT("包内书没有GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 0);
            FY01SourceSnapshot S; TestTrue(TEXT("可观察来源"), F.P->GetSourceEffects()->Observe(S).IsOk());
            TestEqual(TEXT("包内书不注册生效source"), S.Effects.Num(), 0);
            if (Book.Growth.Num() == 1) { TestEqual(TEXT("书未累计成长"), Book.Growth[0].Accumulated, 0.); }
        }
        return true;
    }
    if (Case != TEXT("Waiting")) { TestTrue(TEXT("真实Mode进入Preparing"), F.Mode->TryStartNextWave()); }
    if (Case == TEXT("Battle"))
    {
        for (int32 I = 0; I < 23; ++I) { ++GFrameCounter; F.World->Tick(LEVELTICK_All, .05f); }
        TestEqual(TEXT("真实timer进入Battle"), F.Mode->GetFlowState()->GetWaveSnapshot().Phase, EY01MatchPhase::Battle);
    }
    if (Case == TEXT("Ended")) { TestTrue(TEXT("真实Mode结束"), F.Mode->TryEndMatch(false)); }
    if (Case == TEXT("Dead")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    if (Case == TEXT("Client")) { F.P->SetRole(ROLE_SimulatedProxy); }
    if (Case == TEXT("Waiting") || Case == TEXT("Ended") || Case == TEXT("Dead") || Case == TEXT("Client"))
    {
        TestFalse(TEXT("阶段存活权威拒绝"), F.Send(F.Request(A.ItemId, 0)).IsOk()); F.P->SetRole(ROLE_Authority);
        TestEqual(TEXT("拒绝不生效"), F.Attack(), 30.f); TestEqual(TEXT("拒绝仍在包"), F.Item(A.ItemId).Location, EY01ItemLocation::Backpack); return true;
    }
    auto Request = F.Request(A.ItemId, 0);
    if (Case == TEXT("PayloadOwned"))
    {
        const auto Original = Request; int32 Calls = 0;
        const auto H = F.ASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda([&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle)
        { ++Calls; Request.ItemId = B.ItemId; Request.Slot = 1; });
        const auto R = F.Send(Request); F.ASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(H);
        TestTrue(TEXT("真实GAS回调修改了调用方载荷"), Calls > 0 && Request.ItemId == B.ItemId && Request.Slot == 1);
        Ok(TEXT("按入口载荷完成交易"), R); const auto V = F.View();
        TestEqual(TEXT("入口槽0仍指向A"), V.EquipmentSlots[0], A.ItemId);
        TestFalse(TEXT("回调不能把B注入槽1"), V.EquipmentSlots[1].IsValid());
        TestEqual(TEXT("A实体与入口槽一致"), F.Item(A.ItemId).Slot, 0);
        TestEqual(TEXT("B仍在包中"), F.Item(B.ItemId).Location, EY01ItemLocation::Backpack);
        TestEqual(TEXT("只生效一次"), F.Attack(), 40.f);
        const auto Replay = F.Send(Original); Ok(TEXT("缓存入口原载荷而不是被改载荷"), Replay);
        TestEqual(TEXT("原请求重发保持原提交版本"), Replay.Revision, R.Revision);
        Code(TEXT("改后载荷同号拒绝"), F.Send(Request), E::PayloadConflict); return true;
    }
    if (Case == TEXT("LegacyBypass"))
    {
        F.L()->Deactivate(); FY01EquipmentRow Legacy; Legacy.AttackBonus = 5; F.Data.Add(K::Equipment, Legacy, TEXT("Legacy"));
        FString Error;
        TestFalse(TEXT("组件激活标志不能绕过唯一本局注册表"), F.P->TrySetEquipmentSlot(FY01DataCatalog(F.Data.Tables), F.PC, 0, FY01MatchFixture::Ref(K::Equipment, TEXT("Legacy")), Error));
        TestEqual(TEXT("旧入口不能注入注册表之外的装备GE"), F.Attack(), 30.f); return true;
    }
    if (Case == TEXT("UnequipBackpack"))
    { Request.Operation = EY01ItemOperation::Unequip; Code(TEXT("包内不能卸下"), F.Send(Request), E::InvalidLocation); return true; }
    if (Case == TEXT("InvalidOperation"))
    { Request.Operation = static_cast<EY01ItemOperation>(255); Code(TEXT("非法操作拒绝且缓存"), F.Send(Request), E::InvalidOperation); TestEqual(TEXT("失败推进"), F.View().ExpectedSeq, uint64(2)); return true; }
    if (Case == TEXT("SequenceGap") || Case == TEXT("SequenceZero") || Case == TEXT("CrossJoin") || Case == TEXT("CrossMatch"))
    {
        if (Case == TEXT("CrossMatch")) { Request.MatchId = FGuid::NewGuid(); }
        else if (Case == TEXT("CrossJoin")) { ++Request.JoinSequence; }
        else { Request.Seq = Case == TEXT("SequenceGap") ? 2 : 0; }
        TestFalse(TEXT("序列/会话拒绝"), F.Send(Request).IsOk()); TestEqual(TEXT("不能污染ExpectedSeq"), F.View().ExpectedSeq, uint64(1));
        Ok(TEXT("本会话首号仍能执行"), F.Send(F.Request(A.ItemId, 0))); return true;
    }
    if (Case == TEXT("ForeignController"))
    { auto* Other = F.Player(); Code(TEXT("跨玩家调用拒绝"), F.Send(Request, Other), E::Authority); TestEqual(TEXT("不消耗序号"), F.View().ExpectedSeq, uint64(1)); return true; }
    if (Case == TEXT("ForeignItem")) { Request.ItemId = FGuid::NewGuid(); }
    if (Case == TEXT("SlotNegative")) { Request.Slot = -1; }
    if (Case == TEXT("SlotSix")) { Request.Slot = 6; }
    if (Case == TEXT("SlotHuge")) { Request.Slot = MAX_int32; }
    if (Case == TEXT("StaleEntity")) { ++Request.ExpectedEntityRevision; }
    if (Case == TEXT("StaleSlotItem")) { Request.ExpectedSlotItemId = B.ItemId; Request.ExpectedSlotEntityRevision = 1; }
    if (Case == TEXT("StaleSlotRevision")) { Request.ExpectedSlotEntityRevision = 1; }
    if (Case.StartsWith(TEXT("Slot")) || Case == TEXT("StaleEntity") || Case.StartsWith(TEXT("StaleSlot")) || Case == TEXT("ForeignItem") || Case == TEXT("BookCannotEquip") || Case == TEXT("HugeSafeBackpack"))
    {
        const auto V = F.View(); const auto R = F.Send(Request); TestFalse(TEXT("非法装配拒绝"), R.IsOk());
        if (Case == TEXT("HugeSafeBackpack")) { Code(TEXT("巨大值安全入包但装配精度拒绝"), R, E::Precision); }
        TestEqual(TEXT("库存版本原样"), F.View().Revision, V.Revision); TestEqual(TEXT("实体版本原样"), F.Item(A.ItemId).EntityRevision, uint64(1));
        TestEqual(TEXT("攻击原样"), F.Attack(), 30.f); return true;
    }
    if (Case == TEXT("BusinessFailureCached") || Case == TEXT("ReplayRing64") || Case == TEXT("RingBoundary64"))
    {
        Request.Slot = 6; const auto First = F.Send(Request); TestFalse(TEXT("业务失败"), First.IsOk());
        const int32 Count = Case == TEXT("ReplayRing64") ? 65 : Case == TEXT("RingBoundary64") ? 64 : 1;
        for (int32 I = 1; I < Count; ++I) { auto R = Request; R.Seq = I + 1; TestFalse(TEXT("后续业务失败也消费"), F.Send(R).IsOk()); }
        const auto Again = F.Send(Request);
        Code(TEXT("缓存或过期"), Again, Count == 65 ? E::ExpiredSequence : First.Code);
        TestEqual(TEXT("失败也缓存并推进序号"), F.View().ExpectedSeq, uint64(Count + 1));
        Request.Slot = 0; Code(TEXT("原号不能改payload重试"), F.Send(Request), Count == 65 ? E::ExpiredSequence : E::PayloadConflict); return true;
    }
    if (Case == TEXT("SixInstances"))
    {
        TArray<FY01ItemGrant> Extra; for (int32 I = 0; I < 4; ++I) { Extra.Add(F.Grant()); }
        Ok(TEXT("再入四件同定义"), F.Add(Extra)); TArray<FGuid> Ids{A.ItemId, B.ItemId}; for (const auto& G : Extra) { Ids.Add(G.ItemId); }
        for (int32 I = 0; I < 6; ++I) { Ok(TEXT("六件同定义独立可穿"), F.Send(F.Request(Ids[I], I))); }
        TestEqual(TEXT("六件生效"), F.Attack(), 90.f); return true;
    }
    if (Case == TEXT("DerivedGERejected"))
    {
        int32 Calls = 0; F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda(
            [&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { ++Calls; return false; }));
        const auto Before = F.View(); Code(TEXT("真实汇总GE拒绝"), F.Send(Request), E::EffectRejected);
        F.ASC()->GameplayEffectApplicationQueries.Reset(); TestEqual(TEXT("只查询一次汇总GE"), Calls, 1);
        TestEqual(TEXT("库存未动"), F.View().Revision, Before.Revision); TestEqual(TEXT("实体未动"), F.Item(A.ItemId).EntityRevision, uint64(1));
        TestEqual(TEXT("不留半GE"), F.Attack(), 30.f); TestEqual(TEXT("无残留handle"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 0); return true;
    }
    if (Case == TEXT("SnapshotsOwned"))
    {
        F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, 999);
        F.Data.Row<FY01GrowthRuleRow>(K::GrowthRule).Delta = 999;
    }
    if (Case == TEXT("Reentry.Apply") || Case == TEXT("ObserveDuringStage") || Case == TEXT("DeathDuringApply") || Case == TEXT("EndDuringApply"))
    {
        int32 Calls = 0; const auto Before = F.View();
        auto H = F.ASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda([&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle)
        {
            if (++Calls != 1) { return; }
            FY01LoadoutSnapshot V; Code(TEXT("暂存Observe拒绝Busy"), F.L()->Observe(V), E::Busy);
            if (Case == TEXT("DeathDuringApply")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
            else if (Case == TEXT("EndDuringApply")) { F.L()->EndPlay(EEndPlayReason::Quit); F.P->GetSourceEffects()->EndPlay(EEndPlayReason::Quit); }
            else if (Case == TEXT("Reentry.Apply")) { Code(TEXT("主线串行锁拒绝重入"), F.Send(Request), E::Busy); }
        });
        const auto R = F.Send(Request); F.ASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(H);
        TestTrue(TEXT("真实GAS应用委托"), Calls > 0);
        if (Case == TEXT("ObserveDuringStage")) { Ok(TEXT("只读Busy不使交易中断"), R); TestEqual(TEXT("完整提交"), F.Attack(), 40.f); }
        else { TestFalse(TEXT("重入死亡结束整次拒绝"), R.IsOk()); TestEqual(TEXT("库存不半提交"), F.View().Revision, Before.Revision); TestEqual(TEXT("攻击回滚"), F.Attack(), 30.f); }
        return true;
    }
    const auto First = F.Send(Request); if (!Ok(TEXT("首件装配"), First)) { return false; }
    TestEqual(TEXT("首件攻击"), F.Attack(), Case == TEXT("GrowthOnly") ? 30.f : 40.f);
    if (Case == TEXT("Preparing") || Case == TEXT("Battle")) { return true; }
    if (Case == TEXT("GrowthOnly") || Case == TEXT("SnapshotsOwned"))
    {
        const auto I = F.Item(A.ItemId); TestEqual(TEXT("独立成长规则快照"), I.Growth[0].Delta, 3.);
        TestEqual(TEXT("S3之前不增长"), I.Growth[0].Accumulated, 0.);
        Ok(TEXT("无常驻GE也能按source身份卸下"), F.Send(F.Request(A.ItemId, 0, EY01ItemOperation::Unequip)));
        TestEqual(TEXT("卸下成长账本保留"), F.Item(A.ItemId).Growth[0].Delta, 3.); TestEqual(TEXT("卸下不生效"), F.Attack(), 30.f); return true;
    }
    if (Case == TEXT("SourceIdentity"))
    {
        FY01SourceSnapshot V; TestTrue(TEXT("来源快照"), F.P->GetSourceEffects()->Observe(V).IsOk());
        if (TestEqual(TEXT("一份独立source效果"), V.Effects.Num(), 1))
        {
            const FString Key = A.ItemId.ToString(EGuidFormats::DigitsWithHyphens);
            TestEqual(TEXT("LocalKey只有ItemId不含槽位"), V.Effects[0].Source.LocalKey, Key);
            TestEqual(TEXT("完整Match与Join身份"), V.Effects[0].Source.Id, FString::Printf(TEXT("R36/%s/P1/Equipment/%s"), *F.Match.ToString(EGuidFormats::DigitsWithHyphens), *Key));
        }
        return true;
    }
    if (Case == TEXT("ReplayAfterChange"))
    {
        Ok(TEXT("先卸下"), F.Send(F.Request(A.ItemId, 0, EY01ItemOperation::Unequip)));
        const auto Before = F.View(); const auto Replayed = F.Send(Request); Ok(TEXT("后续交易之后重发原成功"), Replayed);
        TestEqual(TEXT("返回原版本而非当前版本"), Replayed.Revision, First.Revision);
        TestEqual(TEXT("重发不能再穿"), F.Attack(), 30.f); TestEqual(TEXT("当前版本不动"), F.View().Revision, Before.Revision); return true;
    }
    if (Case == TEXT("ReplaySame") || Case == TEXT("ReplayConflict"))
    {
        if (Case == TEXT("ReplayConflict")) { Request.Slot = 1; Code(TEXT("同号异载荷拒绝"), F.Send(Request), E::PayloadConflict); }
        else { const auto R = F.Send(Request); Ok(TEXT("原结果重发"), R); TestEqual(TEXT("原结果版本"), R.Revision, First.Revision); TestEqual(TEXT("原增改清单"), R.Changes.Num(), First.Changes.Num()); }
        TestEqual(TEXT("无重复GE"), F.Attack(), 40.f); return true;
    }
    if (Case == TEXT("SameInstanceTwoSlots") || Case == TEXT("DirectSlotMove"))
    { TestFalse(TEXT("身上实例不能直接换槽或双槽"), F.Send(F.Request(A.ItemId, 1)).IsOk()); TestEqual(TEXT("仍唯一一份GE"), F.Attack(), 40.f); return true; }
    if (Case == TEXT("EquipUnequip"))
    {
        Ok(TEXT("同定义第二实例"), F.Send(F.Request(B.ItemId, 1))); TestEqual(TEXT("两件50"), F.Attack(), 50.f);
        Ok(TEXT("卸第一件"), F.Send(F.Request(A.ItemId, 0, EY01ItemOperation::Unequip))); TestEqual(TEXT("只余40"), F.Attack(), 40.f);
        Ok(TEXT("卸第二件"), F.Send(F.Request(B.ItemId, 1, EY01ItemOperation::Unequip))); TestEqual(TEXT("裸攻30"), F.Attack(), 30.f); return true;
    }
    const auto Before = F.View(); FY01SourceSnapshot SourceBefore; F.P->GetSourceEffects()->Observe(SourceBefore);
    const auto Handles = F.ASC()->GetActiveEffects(FGameplayEffectQuery()); const auto OldItem = F.Item(A.ItemId);
    auto Replacement = F.Request(B.ItemId, 0); int32 Calls = 0; FDelegateHandle H;
    if (Case == TEXT("StaleReplacementItem") || Case == TEXT("StaleReplacementRevision"))
    {
        if (Case == TEXT("StaleReplacementItem")) { Replacement.ExpectedSlotItemId = FGuid::NewGuid(); }
        else { --Replacement.ExpectedSlotEntityRevision; }
        Code(TEXT("真实占用槽拒绝陈旧替换"), F.Send(Replacement), E::StaleSlot);
        TestEqual(TEXT("保留旧件位置"), F.Item(A.ItemId).Slot, 0); TestEqual(TEXT("新件仍在包"), F.Item(B.ItemId).Location, EY01ItemLocation::Backpack);
        TestEqual(TEXT("版本不推进"), F.View().Revision, Before.Revision);
        for (auto Handle : Handles) { TestNotNull(TEXT("原handle仍在"), F.ASC()->GetActiveGameplayEffect(Handle)); }
        return true;
    }
    if (Case == TEXT("ReplacementRejected") || Case == TEXT("IncomeNotReverted"))
    {
        if (Case == TEXT("IncomeNotReverted"))
        {
            FY01PreparedMutation Plan; FY01SourceTransaction Tx;
            TestTrue(TEXT("真实基础收入预检"), F.P->GetSourceEffects()->PrepareMutation({{EY01RuntimeSourceKind::Base, TEXT("Income"), {{TEXT("Rate"), EY01SourceEffectKind::GoldPerSecond, 100}}}}, {}, Plan).IsOk());
            TestTrue(TEXT("真实基础收入暂存"), F.P->GetSourceEffects()->StageMutation(Plan, Tx).IsOk());
            TestTrue(TEXT("真实基础收入提交"), F.P->GetSourceEffects()->CommitMutation(Tx).IsOk());
            F.P->GetSourceEffects()->Observe(SourceBefore); TestTrue(TEXT("真实时钟收入启动"), F.P->GetIncome()->Start().IsOk());
            FPlatformProcess::Sleep(.03f);
        }
        F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda(
            [&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { ++Calls; return false; }));
    }
    if (Case == TEXT("Reentry.Inhibit"))
    { H = F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).AddLambda([&](const FOnAttributeChangeData&)
        { if (++Calls == 1) { Code(TEXT("抑制旧GE期间重入拒绝"), F.Send(Replacement), E::Busy); } }); }
    if (Case == TEXT("Reentry.Remove") || Case == TEXT("EndDuringCommitDelete") || Case == TEXT("DeathDuringCommitDelete"))
    {
        H = F.ASC()->OnAnyGameplayEffectRemovedDelegate().AddLambda([&](const FActiveGameplayEffect&)
        {
            if (++Calls != 1) { return; }
            FY01LoadoutSnapshot V; Code(TEXT("删除通知仍隐藏半事务"), F.L()->Observe(V), E::Busy);
            if (Case == TEXT("EndDuringCommitDelete")) { F.L()->EndPlay(EEndPlayReason::Quit); F.P->GetSourceEffects()->EndPlay(EEndPlayReason::Quit); }
            else if (Case == TEXT("DeathDuringCommitDelete")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
            else { Code(TEXT("删除通知重入不执行"), F.Send(Replacement), E::Busy); }
        });
    }
    const auto R = F.Send(Replacement);
    F.ASC()->GameplayEffectApplicationQueries.Reset();
    if (Case == TEXT("Reentry.Inhibit")) { F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).Remove(H); }
    else { F.ASC()->OnAnyGameplayEffectRemovedDelegate().Remove(H); }
    const bool Rollback = Case == TEXT("ReplacementRejected") || Case == TEXT("IncomeNotReverted") || Case == TEXT("Reentry.Inhibit");
    if (Rollback)
    {
        TestFalse(TEXT("替换失败"), R.IsOk()); TestTrue(TEXT("真实GAS拒绝已触发"), Calls > 0);
        TestEqual(TEXT("整体版本恢复"), F.View().Revision, Before.Revision); TestEqual(TEXT("旧件原槽"), F.Item(A.ItemId).Slot, 0);
        TestEqual(TEXT("新件仍包"), F.Item(B.ItemId).Location, EY01ItemLocation::Backpack);
        TestEqual(TEXT("成长未改"), F.Item(A.ItemId).Growth[0].Accumulated, OldItem.Growth[0].Accumulated);
        TestEqual(TEXT("实体版本恢复"), F.Item(A.ItemId).EntityRevision, OldItem.EntityRevision);
        FY01SourceSnapshot S; F.P->GetSourceEffects()->Observe(S); TestEqual(TEXT("source版本恢复"), S.Revision, SourceBefore.Revision);
        TestEqual(TEXT("base精确保持"), S.BaseAttack, 30.f); TestEqual(TEXT("current精确保持"), S.CurrentAttack, 40.f);
        for (const auto Handle : Handles) { TestNotNull(TEXT("原handle而不是重建"), F.ASC()->GetActiveGameplayEffect(Handle)); }
        TestEqual(TEXT("无残留新GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), Handles.Num());
        if (Case == TEXT("IncomeNotReverted")) { TestTrue(TEXT("旧合法收入已结算不追回"), F.P->GetGold() >= 1); }
        return true;
    }
    if (Case == TEXT("Replace") || Case == TEXT("ResultChanges") || Case == TEXT("Reentry.Remove") || Case == TEXT("EndDuringCommitDelete") || Case == TEXT("DeathDuringCommitDelete"))
    {
        Ok(TEXT("共同提交点之后不再把成功报告成失败"), R);
        TestEqual(TEXT("旧件回包"), F.Item(A.ItemId).Location, EY01ItemLocation::Backpack);
        TestEqual(TEXT("新件在槽"), F.Item(B.ItemId).Slot, 0); TestEqual(TEXT("库存整体一次版本"), F.View().Revision, Before.Revision + 1);
        TestEqual(TEXT("替换增改清单包含两实体"), R.Changes.Num(), 2);
        if (Case == TEXT("ResultChanges"))
        {
            for (const auto& Change : Added.Changes)
            {
                TestEqual(TEXT("入包清单为Added"), Change.Kind, EY01ItemChangeKind::Added);
                TestEqual(TEXT("入包修改版本"), Change.Item.LastChangedRevision, Added.Revision);
            }
            for (const auto& Change : R.Changes)
            {
                TestEqual(TEXT("替换清单为Changed"), Change.Kind, EY01ItemChangeKind::Changed);
                TestEqual(TEXT("同事务LastChangedRevision"), Change.Item.LastChangedRevision, R.Revision);
                TestEqual(TEXT("实体与清单版本一致"), F.Item(Change.Item.ItemId).EntityRevision, Change.Item.EntityRevision);
            }
            TestEqual(TEXT("旧件获取顺序保留"), F.Item(A.ItemId).AcquisitionOrder, uint64(1));
            TestEqual(TEXT("新件获取顺序保留"), F.Item(B.ItemId).AcquisitionOrder, uint64(2));
        }
        TestEqual(TEXT("旧件实体版本+1"), F.Item(A.ItemId).EntityRevision, OldItem.EntityRevision + 1);
        TestEqual(TEXT("无重复攻击"), F.Attack(), Case == TEXT("EndDuringCommitDelete") ? 30.f : ChangedReplacement ? 47.f : 40.f);
        if (Case.Contains(TEXT("During")) || Case == TEXT("Reentry.Remove")) { TestTrue(TEXT("真实删除通知已触发"), Calls > 0); }
        if (Case != TEXT("EndDuringCommitDelete"))
        { const auto Again = F.Send(Replacement); Ok(TEXT("重发仍原结果"), Again); TestEqual(TEXT("唯一提交版本"), Again.Revision, R.Revision); }
        return true;
    }
#if WITH_EDITOR
    if (Case == TEXT("PersonalRoom"))
    {
        TStrongObjectPtr<UY01PlaygroundProfile> Profile(Y01PlaygroundEditor::CreateSeed(GetTransientPackage()));
        auto* C = Profile->Tables[K::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("Loadout"));
        C->InitialEquipment.Reset(); C->InitialSkills.Reset(); C->R33TestGrowth.bEnabled = false; C->AttackDamage = 30;
        Profile->bTrainingEnabled = true;
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
        UWorld* W = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(W); W->InitializeActorsForPlay(FURL()); W->SetGameInstance(NewObject<UGameInstance>(GEngine));
        W->GetWorldSettings()->DefaultGameMode = AY01PlaygroundGameMode::StaticClass(); W->SetGameMode(FURL());
        auto* Mode = W->GetAuthGameMode<AY01PlaygroundGameMode>(); W->SpawnActor<AY01PlaygroundConfig>()->Profile = Profile.Get();
        auto* PC = W->SpawnActor<AY01PlaygroundController>(); auto* PS = W->SpawnActor<AY01PlayerState>();
        auto* Pawn = W->SpawnActor<AY01PlaygroundCharacter>(FVector(0, 0, 100), FRotator::ZeroRotator);
        PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn); Mode->GetFlowState()->AddPlayerState(PS);
        Mode->DispatchBeginPlay(); W->GetWorldSettings()->NotifyBeginPlay(); W->BeginPlay();
        TestTrue(TEXT("权威进入个人房"), Mode->GetTraining()->Request(PC, true));
        TestTrue(TEXT("真实个人房状态"), Mode->GetTraining()->Observe(PS).bInside);
        TestTrue(TEXT("主线进入Preparing"), Mode->TryStartNextWave());
        const FGuid Match = FGuid::NewGuid(); auto* L = PS->GetLoadout(); Ok(TEXT("个人房初始化"), L->Initialize(Match, 1));
        auto G = F.Grant(); Ok(TEXT("个人房入包"), L->AddItems(FY01DataCatalog(F.Data.Tables), {G}));
        FY01ItemRequest Q; Q.MatchId = Match; Q.JoinSequence = 1; Q.Seq = 1; Q.ItemId = G.ItemId; Q.ExpectedEntityRevision = 1; Q.Slot = 0;
        Ok(TEXT("个人房不屏蔽装备"), L->TryEquip(PC, Q));
        TestEqual(TEXT("个人房攻击生效"), PS->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), 40.f);
        FY01ShopOfferRow Direct; Direct.ShopKind = EY01ShopKind::DirectEffect; Direct.Price = 50;
        Direct.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten"))}; F.Data.Add(K::ShopOffer, Direct, TEXT("PersonalDirect"));
        FY01LocalItemPoolRow Pool; Pool.ItemType = K::Equipment; FY01WeightedEntry Entry;
        Entry.Item = FY01MatchFixture::Ref(K::Equipment, TEXT("Sword")); Entry.Weight = 1; Pool.Entries.Add(Entry); F.Data.Add(K::LocalItemPool, Pool);
        FY01QualityRollPoolRow Tier; FY01QualityRollEntry Roll; Roll.Pool = FY01MatchFixture::Ref(K::LocalItemPool); Roll.Weight = 1;
        Tier.Entries.Add(Roll); F.Data.Add(K::QualityRollPool, Tier);
        FY01ShopOfferRow Random; Random.ShopKind = EY01ShopKind::RandomTier; Random.Price = 100; Random.TierPool = FY01MatchFixture::Ref(K::QualityRollPool);
        F.Data.Add(K::ShopOffer, Random, TEXT("PersonalRandom")); PS->SetOwner(PC);
        auto* Economy = PS->GetEconomy();
        Ok(TEXT("个人房真实商店初始化"), Economy->Initialize(Match, 1, 1, FY01DataCatalog(F.Data.Tables),
            {FY01MatchFixture::Ref(K::ShopOffer, TEXT("PersonalDirect")), FY01MatchFixture::Ref(K::ShopOffer, TEXT("PersonalRandom"))}));
        PS->TryAddMatchReward(500, 0); const int32 Gold = PS->GetGold();
        Q = {}; Q.MatchId = Match; Q.JoinSequence = 1; Q.ConfigRevision = 1; Q.Seq = 2; Q.Operation = EY01ItemOperation::Purchase;
        Q.Offer = FY01MatchFixture::Ref(K::ShopOffer, TEXT("PersonalDirect")); Ok(TEXT("个人房直接购买"), Economy->TryPurchase(PC, Q));
        Q.Seq = 3; Q.Offer = FY01MatchFixture::Ref(K::ShopOffer, TEXT("PersonalRandom")); Ok(TEXT("个人房两层抽样购买"), Economy->TryPurchase(PC, Q));
        FY01CommittedView Full; L->ReadCommittedView(Full);
        TestEqual(TEXT("个人房两笔共同扣款"), Full.Gold, Gold - 150); TestEqual(TEXT("随机只入包"), Full.Items.Num(), 2);
        TestEqual(TEXT("直接效果真实生效"), Full.Stats.AttackDamage, 50.f);
        TestTrue(TEXT("个人房同次已购视图"), Full.ShopOffers.ContainsByPredicate([](const auto& S) { return S.OfferId == TEXT("PersonalDirect") && S.bPurchased; }));
        W->EndPlay(EEndPlayReason::Quit); W->DestroyWorld(false); GEngine->DestroyWorldContext(W); return true;
    }
#endif
    AddError(TEXT("未覆盖Loadout测试分支：") + Case); return false;
#else
    return false;
#endif
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36LearnTest, "Y01.Framework.R36.Learn",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36LearnTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("EmptyConsumesOne"), TEXT("Cancel"), TEXT("Confirm"), TEXT("Remove"), TEXT("Passive"),
        TEXT("SameId"), TEXT("SameSlotId"), TEXT("CaseInsensitiveId"), TEXT("SameNameDifferentId"), TEXT("FourSlots"),
        TEXT("SlotNegative"), TEXT("SlotFour"), TEXT("SlotHuge"), TEXT("StaleBook"), TEXT("StaleSlot"), TEXT("StaleConfig"),
        TEXT("StaleMatch"), TEXT("StaleJoin"), TEXT("UnrelatedAdd"), TEXT("TicketReplay"), TEXT("TicketPayload"),
        TEXT("PayloadLearnedId"), TEXT("PayloadLearnedRevision"), TEXT("PayloadConfig"), TEXT("PayloadTicket"),
        TEXT("SnapshotsOwned"), TEXT("MissingGA"), TEXT("WrongGA"), TEXT("UnsupportedGA"), TEXT("LegacyEffect"),
        TEXT("LegacySkillInit"), TEXT("LegacyBypass"), TEXT("Dead"), TEXT("Waiting"), TEXT("Ended"), TEXT("ForeignController"),
        TEXT("DeadDrop"), TEXT("Capacity1000"), TEXT("Capacity10000"), TEXT("FailureCached"), TEXT("ReplayRing64")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36LearnTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FLoadoutWorld F;
#if !Y01_LOADOUT_LEARNING
    TestNotNull(TEXT("真实注册表快照必须支持已学技能而不把书当装备"),
        FindFProperty<FArrayProperty>(FY01LoadoutSnapshot::StaticStruct(), TEXT("Skills")));
    return false;
#else
    using E = EY01ItemError; using O = EY01ItemOperation;
    auto Ok = [&](const TCHAR* M, const FY01ItemResult& R)
    { if (!R.IsOk()) { AddInfo(R.Message); } return TestTrue(M, R.IsOk()); };
    auto Code = [&](const TCHAR* M, const FY01ItemResult& R, E C) { return TestEqual(M, R.Code, C); };
    F.Book(TEXT("A")); F.Book(TEXT("B")); F.Book(TEXT("C"), false); F.Book(TEXT("D"), false);
    if (Case == TEXT("LegacySkillInit"))
    {
        auto* Other = F.Player(); auto* PS = Other->GetPlayerState<AY01PlayerState>();
        F.Data.Row<FY01CharacterRow>(K::Character).InitialSkills = {FY01MatchFixture::Ref(K::Skill)};
        TestTrue(TEXT("旧技能初始化"), PS->TryInitializeCharacter(FY01DataCatalog(F.Data.Tables), FY01MatchFixture::Ref(K::Character), F.Error));
        Code(TEXT("拒绝旧技能混入新会话"), PS->GetLoadout()->Initialize(F.Match, 2), E::InvalidDefinition); return true;
    }
    if (!Ok(TEXT("真实注册表会话"), F.Init())) { return false; }
    if (Case == TEXT("LegacyBypass"))
    {
        TestFalse(TEXT("ASC按定义重复初始化也封闭"), F.ASC()->TryInitializeFromData(FY01DataCatalog(F.Data.Tables), FY01MatchFixture::Ref(K::Character), F.Error));
        return true;
    }
    auto A = F.Grant(TEXT("A"), K::Skill), B = F.Grant(TEXT("B"), K::Skill), A2 = F.Grant(TEXT("a"), K::Skill);
    if (Case == TEXT("Passive")) { A = F.Grant(TEXT("C"), K::Skill); }
    auto& Row = F.Data.Row<FY01SkillRow>(K::Skill, TEXT("A"));
    if (Case == TEXT("MissingGA")) { Row.Ability.Reset(); }
    if (Case == TEXT("WrongGA")) { Row.Ability = FSoftObjectPath(TEXT("/Script/Y01.Y01EquipmentAttackEffect")); }
    if (Case == TEXT("UnsupportedGA")) { Row.Ability = UGameplayAbility::StaticClass(); }
    if (Case == TEXT("LegacyEffect"))
    {
        Row.Effect = UY01EquipmentAttackEffect::StaticClass();
        Code(TEXT("不混用旧Effect"), F.Add({A}), E::InvalidDefinition); return true;
    }
    if (Case.StartsWith(TEXT("Capacity")))
    {
        const int32 Count = Case == TEXT("Capacity1000") ? 1000 : 10000;
        TArray<FY01ItemGrant> Batch;
        for (int32 I = 0; I < Count; ++I) { Batch.Add(F.Grant(TEXT("A"), K::Skill)); }
        Ok(TEXT("批量书入包"), F.Add(Batch)); TestTrue(TEXT("准备阶段"), F.Mode->TryStartNextWave());
        for (int32 I : {0, Count / 2, Count - 1})
        {
            Ok(TEXT("耗掉指定位置书"), F.Learn(F.LearnRequest(Batch[I].ItemId, 0)));
            Ok(TEXT("移除不返还"), F.Learn(F.LearnRequest({}, 0, O::RemoveSkill)));
        }
        const auto Extra = F.Grant(TEXT("B"), K::Skill); Ok(TEXT("删后新入包"), F.Add({Extra}));
        const auto V = F.View(); TestEqual(TEXT("所有未消耗条目可枚举"), V.Items.Num(), Count - 2);
        uint64 Previous = 0; TSet<FGuid> Seen;
        for (const auto& I : V.Items)
        {
            TestTrue(TEXT("获取顺序严格递增"), I.AcquisitionOrder > Previous); Previous = I.AcquisitionOrder;
            TestFalse(TEXT("枚举不重复"), Seen.Contains(I.ItemId)); Seen.Add(I.ItemId);
            TestEqual(TEXT("哈希寻址与遍历一致"), F.Item(I.ItemId).AcquisitionOrder, I.AcquisitionOrder);
        }
        TestEqual(TEXT("删后不得复用序号"), F.Item(Extra.ItemId).AcquisitionOrder, uint64(Count + 1)); return true;
    }
    if (!Ok(TEXT("三本独立书入包"), F.Add({A, B, A2}))) { return false; }
    if (Case == TEXT("SnapshotsOwned"))
    { Row.DisplayName = FText::FromString(TEXT("改变")); Row.CooldownSeconds = 999; Row.Ability.Reset(); F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, 999); }
    if (Case != TEXT("Waiting")) { TestTrue(TEXT("准备阶段"), F.Mode->TryStartNextWave()); }
    if (Case == TEXT("Dead") || Case == TEXT("DeadDrop")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    if (Case == TEXT("DeadDrop")) { Ok(TEXT("内部掉落允许死亡"), F.Add({F.Grant()})); return true; }
    if (Case == TEXT("Ended")) { F.Mode->TryEndMatch(false); }
    auto Q = F.LearnRequest(A.ItemId, 0);
    if (Case.StartsWith(TEXT("Slot"))) { Q.Slot = Case == TEXT("SlotNegative") ? -1 : Case == TEXT("SlotFour") ? 4 : MAX_int32; }
    if (Case == TEXT("ForeignController")) { Code(TEXT("真实控制器鉴权"), F.Learn(Q, F.Player()), E::Authority); return true; }
    if (Case == TEXT("Dead") || Case == TEXT("Waiting") || Case == TEXT("Ended") || Case.StartsWith(TEXT("Slot"))
        || Case.EndsWith(TEXT("GA")))
    {
        const auto Before = F.View(); FY01SourceSnapshot SourcesBefore; F.P->GetSourceEffects()->Observe(SourcesBefore);
        TestFalse(TEXT("非法业务不得假学习成功"), F.Learn(Q).IsOk());
        const auto After = F.View();
        TestEqual(TEXT("拒绝不耗书"), After.Items.Num(), Before.Items.Num()); TestEqual(TEXT("版本不动"), After.Revision, Before.Revision);
        for (const auto& I : Before.Items)
        { const auto Actual = F.Item(I.ItemId); TestTrue(TEXT("拒绝后原书身份与全部快照精确保留"), FY01ItemInstance::StaticStruct()->CompareScriptStruct(&I, &Actual, 0)); }
        FY01SourceSnapshot SourcesAfter; F.P->GetSourceEffects()->Observe(SourcesAfter);
        TestEqual(TEXT("拒绝后来源版本精确保留"), SourcesAfter.Revision, SourcesBefore.Revision);
        TestEqual(TEXT("拒绝后来源数量精确保留"), SourcesAfter.Effects.Num(), SourcesBefore.Effects.Num());
        TestEqual(TEXT("拒绝后实际攻击保持"), F.Attack(), 30.f); return true;
    }
    if (Case == TEXT("FailureCached") || Case == TEXT("ReplayRing64"))
    {
        Q.Slot = 4; const auto R = F.Learn(Q); Code(TEXT("业务失败"), R, E::InvalidSlot);
        const int32 Count = Case == TEXT("ReplayRing64") ? 65 : 1;
        for (int32 I = 1; I < Count; ++I) { auto Next = Q; Next.Seq = I + 1; Code(TEXT("失败消费"), F.Learn(Next), E::InvalidSlot); }
        Code(TEXT("原失败缓存和64窗口"), F.Learn(Q), Count == 65 ? E::ExpiredSequence : R.Code); return true;
    }
    const auto First = F.Learn(Q); if (!Ok(TEXT("空槽直接学习"), First)) { return false; }
    const auto Learned = F.View().Skills[0];
    TestTrue(TEXT("独立LearnedId"), Learned.LearnedId.IsValid() && Learned.LearnedId != A.ItemId);
    TestEqual(TEXT("只耗一本"), F.View().Items.Num(), 2); TestEqual(TEXT("常驻生效"), F.Attack(), 40.f);
    TestEqual(TEXT("立即就绪无学习CD"), F.ASC()->GetSkillCooldownRemaining(0), 0.f);
    if (Case == TEXT("EmptyConsumesOne") || Case == TEXT("SnapshotsOwned") || Case == TEXT("Passive"))
    {
        TestEqual(TEXT("完整复制名称"), Learned.Skill.DisplayName.ToString(), FString(TEXT("相同显示名")));
        TestEqual(TEXT("复制CD"), Learned.Skill.CooldownSeconds, Case == TEXT("Passive") ? 0.f : 8.f);
        if (Case == TEXT("Passive")) { TestFalse(TEXT("被动不授予GA"), Learned.GAHandle.IsValid()); TestFalse(TEXT("被动不能发动"), F.ASC()->TryActivateSkillSlot(0)); }
        else { TestNotNull(TEXT("真实GA存在"), F.ASC()->FindAbilitySpecFromHandle(Learned.GAHandle)); TestTrue(TEXT("新GA就绪"), F.ASC()->IsAbilityReady(Learned.GAHandle)); }
        FY01SourceSnapshot S; TestTrue(TEXT("来源可观察"), F.P->GetSourceEffects()->Observe(S).IsOk());
        if (TestEqual(TEXT("一条常驻来源"), S.Effects.Num(), 1))
        { TestEqual(TEXT("来源不是槽而是LearnedId"), S.Effects[0].Source.LocalKey, Learned.LearnedId.ToString(EGuidFormats::DigitsWithHyphens)); }
        return true;
    }
    if (Case.StartsWith(TEXT("Payload")))
    {
        if (Case == TEXT("PayloadLearnedId")) { Q.ExpectedLearnedId = FGuid::NewGuid(); }
        if (Case == TEXT("PayloadLearnedRevision")) { Q.ExpectedLearnedRevision = 123; }
        if (Case == TEXT("PayloadConfig")) { ++Q.ConfigRevision; }
        if (Case == TEXT("PayloadTicket")) { Q.TicketId = FGuid::NewGuid(); }
        Code(TEXT("每个新增意图参与同号防重"), F.Learn(Q), E::PayloadConflict); return true;
    }
    if (Case == TEXT("Remove"))
    {
        Ok(TEXT("移除已学技能"), F.Learn(F.LearnRequest({}, 0, O::RemoveSkill)));
        TestEqual(TEXT("不返书"), F.View().Items.Num(), 2); TestFalse(TEXT("空槽"), F.View().Skills[0].LearnedId.IsValid());
        TestNull(TEXT("原GA撤销"), F.ASC()->FindAbilitySpecFromHandle(Learned.GAHandle)); TestEqual(TEXT("常驻撤销"), F.Attack(), 30.f);
        AddExpectedMessagePlain(TEXT("TryActivateAbility called with invalid Handle"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
        TestFalse(TEXT("旧handle不能发动"), F.ASC()->TryActivateAbility(Learned.GAHandle, false)); return true;
    }
    if (Case == TEXT("SameId") || Case == TEXT("SameSlotId") || Case == TEXT("CaseInsensitiveId"))
    {
        const auto V = F.View(); Code(TEXT("同ID包含大小写和同槽拒绝"), F.Learn(F.LearnRequest(A2.ItemId, Case == TEXT("SameId") ? 1 : 0)), E::DuplicateSkill);
        TestEqual(TEXT("不耗书"), F.View().Items.Num(), 2); TestEqual(TEXT("不改版本"), F.View().Revision, V.Revision); return true;
    }
    if (Case == TEXT("SameNameDifferentId") || Case == TEXT("FourSlots"))
    {
        Ok(TEXT("同名不同ID共存"), F.Learn(F.LearnRequest(B.ItemId, 1)));
        const auto Both = F.View();
        TestEqual(TEXT("槽0保留原LearnedId"), Both.Skills[0].LearnedId, Learned.LearnedId);
        TestEqual(TEXT("槽0保留原GA"), Both.Skills[0].GAHandle, Learned.GAHandle);
        TestEqual(TEXT("槽0为A"), Both.Skills[0].SkillId, FName(TEXT("A")));
        TestEqual(TEXT("槽1为B"), Both.Skills[1].SkillId, FName(TEXT("B")));
        TestTrue(TEXT("不同ID独立已学身份"), Both.Skills[1].LearnedId.IsValid() && Both.Skills[1].LearnedId != Learned.LearnedId);
        TestTrue(TEXT("不同ID独立GA"), Both.Skills[1].GAHandle.IsValid() && Both.Skills[1].GAHandle != Learned.GAHandle);
        TestEqual(TEXT("确实同名而非不同名称"), Both.Skills[0].Skill.DisplayName.ToString(), Both.Skills[1].Skill.DisplayName.ToString());
        TestNotNull(TEXT("A的实际GA仍存在"), F.ASC()->FindAbilitySpecFromHandle(Both.Skills[0].GAHandle));
        TestNotNull(TEXT("B的实际GA仍存在"), F.ASC()->FindAbilitySpecFromHandle(Both.Skills[1].GAHandle));
        TestEqual(TEXT("两来源同时生效"), F.Attack(), 50.f);
        if (Case == TEXT("FourSlots"))
        {
            for (int32 I = 2; I < 4; ++I)
            { auto G = F.Grant(I == 2 ? TEXT("C") : TEXT("D"), K::Skill); Ok(TEXT("被动书入包"), F.Add({G})); Ok(TEXT("主动被动共四槽"), F.Learn(F.LearnRequest(G.ItemId, I))); }
            TestEqual(TEXT("四槽总增益"), F.Attack(), 70.f); TestEqual(TEXT("恰好四槽"), F.View().Skills.Num(), 4);
        }
        return true;
    }
    auto Prepare = F.LearnRequest(B.ItemId, 0); const auto Prepared = F.Learn(Prepare);
    if (!Ok(TEXT("覆盖准备"), Prepared) || !TestTrue(TEXT("占用槽产生票据"), Prepared.TicketId.IsValid())) { return false; }
    TestEqual(TEXT("准备不耗书"), F.View().Items.Num(), 2); TestEqual(TEXT("准备不改技能"), F.View().Skills[0].LearnedId, Learned.LearnedId);
    auto Confirm = F.Confirm(Prepare, Prepared.TicketId);
    if (Case == TEXT("TicketReplay"))
    { const auto R = F.Learn(Prepare); Ok(TEXT("准备重发返回原票据"), R); TestEqual(TEXT("同一票据"), R.TicketId, Prepared.TicketId); }
    if (Case == TEXT("Cancel"))
    {
        Confirm.Operation = O::CancelLearn; Ok(TEXT("取消"), F.Learn(Confirm));
        TestEqual(TEXT("取消无物品副作用"), F.View().Items.Num(), 2); TestEqual(TEXT("取消无技能副作用"), F.View().Skills[0].LearnedId, Learned.LearnedId);
        Confirm = F.Confirm(Prepare, Prepared.TicketId); Code(TEXT("取消后票据无效"), F.Learn(Confirm), E::StaleTicket); return true;
    }
    if (Case == TEXT("UnrelatedAdd")) { Ok(TEXT("无关入包"), F.Add({F.Grant()})); }
    if (Case == TEXT("StaleSlot")) { Ok(TEXT("目标槽先被移除"), F.Learn(F.LearnRequest({}, 0, O::RemoveSkill))); }
    if (Case == TEXT("StaleBook")) { Ok(TEXT("书先被别槽消耗"), F.Learn(F.LearnRequest(B.ItemId, 1))); }
    if (Case == TEXT("StaleConfig")) { Ok(TEXT("服务器推进配置纪元"), F.L()->SetConfigRevision(2)); }
    Confirm.Seq = F.View().ExpectedSeq;
    if (Case == TEXT("StaleMatch")) { Confirm.MatchId = FGuid::NewGuid(); }
    if (Case == TEXT("StaleJoin")) { ++Confirm.JoinSequence; }
    if (Case == TEXT("TicketPayload")) { Confirm.ItemId = A2.ItemId; }
    if (Case.StartsWith(TEXT("Stale")) || Case == TEXT("TicketPayload"))
    {
        const auto Before = F.View(); TestFalse(TEXT("陈旧或伪造票据拒绝"), F.Learn(Confirm).IsOk());
        TestEqual(TEXT("失败不消耗任何书"), F.View().Items.Num(), Before.Items.Num()); TestEqual(TEXT("不改实体版本"), F.View().Revision, Before.Revision); return true;
    }
    if (Case == TEXT("Confirm") || Case == TEXT("UnrelatedAdd") || Case == TEXT("TicketReplay"))
    {
        const auto R = F.Learn(Confirm); Ok(TEXT("确认才覆盖耗书"), R);
        TestEqual(TEXT("确认只消耗目标书"), F.View().Items.Num(), Case == TEXT("UnrelatedAdd") ? 2 : 1);
        TestTrue(TEXT("覆盖产生新身份"), F.View().Skills[0].LearnedId != Learned.LearnedId);
        TestNull(TEXT("精确旧GA撤销"), F.ASC()->FindAbilitySpecFromHandle(Learned.GAHandle));
        Ok(TEXT("确认重发原结果"), F.Learn(Confirm)); return true;
    }
    AddError(TEXT("未覆盖技能业务分支：") + Case); return false;
#endif
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36LearnFaultTest, "Y01.Framework.R36.LearnFault",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36LearnFaultTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("GERejected"), TEXT("DerivedGERejected"), TEXT("InhibitReentry"), TEXT("ApplyDeath"), TEXT("ApplyEnded"),
        TEXT("GrantReentry"), TEXT("GrantDeath"), TEXT("GrantEnded"), TEXT("GrantCleared"), TEXT("GrantObserve"),
        TEXT("RetireObserve"), TEXT("RetireReentry"), TEXT("RetireDeath"), TEXT("RetireEnded"), TEXT("RemoveReentry"),
        TEXT("ActivationReentry"), TEXT("DirectCommitReentry"), TEXT("CooldownABA"), TEXT("CooldownDuplicate"), TEXT("ConfirmDuplicate"),
        TEXT("PayloadOwned"), TEXT("ConfirmForeign"), TEXT("ConfirmDead"), TEXT("ConfirmEnded"), TEXT("IncomeRetained"),
        TEXT("BookRevision"), TEXT("LearnedRevision"), TEXT("SourceStageActivation"), TEXT("CancelExactCooldown")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36LearnFaultTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FLoadoutWorld F;
    using E = EY01ItemError; using O = EY01ItemOperation;
    auto Ok = [&](const TCHAR* M, const FY01ItemResult& R) { if (!R.IsOk()) { AddInfo(R.Message); } return TestTrue(M, R.IsOk()); };
    auto Code = [&](const TCHAR* M, const FY01ItemResult& R, E C) { TestEqual(M, R.Code, C); };
    ON_SCOPE_EXIT { UY01LearnProbeAbility::Given = {}; UY01LearnProbeAbility::Removed = {}; };
    F.Book(TEXT("A")); F.Book(TEXT("B"));
    F.Data.Row<FY01SkillRow>(K::Skill, TEXT("A")).Ability = UY01LearnProbeAbility::StaticClass();
    F.Data.Row<FY01SkillRow>(K::Skill, TEXT("B")).Ability = UY01LearnProbeAbility::StaticClass();
    if (Case == TEXT("DerivedGERejected") || Case == TEXT("GERejected") || Case == TEXT("IncomeRetained")
        || Case == TEXT("InhibitReentry") || Case == TEXT("ApplyDeath") || Case == TEXT("ApplyEnded") || Case == TEXT("PayloadOwned"))
    { F.Data.Row<FY01SkillRow>(K::Skill, TEXT("B")).EffectIds.Add(FY01MatchFixture::Ref(K::SourceEffect, TEXT("Other"))); }
    auto A = F.Grant(TEXT("A"), K::Skill), B = F.Grant(TEXT("B"), K::Skill), A2 = F.Grant(TEXT("a"), K::Skill);
    if (!Ok(TEXT("初始化"), F.Init()) || !Ok(TEXT("入包"), F.Add({A, B, A2}))) { return false; }
    F.Mode->TryStartNextWave();
    if (!Ok(TEXT("首学A"), F.Learn(F.LearnRequest(A.ItemId, 0)))) { return false; }
    auto* Target = F.Player(); F.Data.Row<FY01CharacterRow>(K::Character).Health = 1000;
    TestTrue(TEXT("真实目标初始化"), Target->GetPlayerState<AY01PlayerState>()->TryInitializeCharacter(FY01DataCatalog(F.Data.Tables), FY01MatchFixture::Ref(K::Character), F.Error));
    Target->GetPawn()->SetActorLocation(F.PC->GetPawn()->GetActorLocation() + FVector(100, 0, 0));
    TestTrue(TEXT("真实ASC选目标"), F.ASC()->SetCombatTarget(Target->GetPawn()));
    if (Case == TEXT("CooldownABA") || Case == TEXT("CooldownDuplicate") || Case == TEXT("CancelExactCooldown"))
    { for (int32 I = 0; I < 40; ++I) { ++GFrameCounter; F.World->Tick(LEVELTICK_All, .25f); } }
    const double CastAt = F.World->GetTimeSeconds();
    auto Q = F.LearnRequest(B.ItemId, 0); int32 Calls = 0;
    if (Case == TEXT("ActivationReentry") || Case == TEXT("DirectCommitReentry"))
    {
        auto& Delegate = Case == TEXT("ActivationReentry") ? F.ASC()->AbilityActivatedCallbacks : F.ASC()->AbilityCommittedCallbacks;
        const auto H = Delegate.AddLambda([&](UGameplayAbility*)
        {
            ++Calls; Code(TEXT("发动与真实广播期间学习Busy"), F.Learn(Q), E::Busy);
            Code(TEXT("移除与发动互斥"), F.L()->RemoveSkill(F.PC, Q), E::Busy);
            FY01LoadoutSnapshot V; Code(TEXT("释放中观察Busy"), F.L()->Observe(V), E::Busy);
        });
        const auto Handle = F.View().Skills[0].GAHandle;
        TestTrue(TEXT("直接GA路径也实际提交"), F.ASC()->TryActivateAbility(Handle, false)); Delegate.Remove(H);
        TestEqual(TEXT("真实广播恰好一次"), Calls, 1); TestEqual(TEXT("未耗书"), F.View().Items.Num(), 2);
        TestEqual(TEXT("CD正常提交"), F.View().Skills[0].Deadline, CastAt + 8); return true;
    }
    TestTrue(TEXT("A真实发动"), F.ASC()->TryActivateSkillSlot(0));
    const auto Old = F.View().Skills[0];
    TestEqual(TEXT("表CD写入Loadout"), Old.Deadline, CastAt + 8);
    if (Case == TEXT("CooldownDuplicate"))
    {
        TestEqual(TEXT("实际时钟t10"), CastAt, 10.);
        Code(TEXT("同槽A重复拒绝"), F.Learn(F.LearnRequest(A2.ItemId, 0)), E::DuplicateSkill);
        TestEqual(TEXT("截止仍18"), F.View().Skills[0].Deadline, 18.); TestEqual(TEXT("同一handle"), F.View().Skills[0].GAHandle, Old.GAHandle); return true;
    }
    if (Case == TEXT("SourceStageActivation"))
    {
        FY01PreparedMutation Plan; FY01SourceTransaction Tx;
        auto* S = F.P->GetSourceEffects();
        TestTrue(TEXT("外层真实来源准备"), S->PrepareMutation({{EY01RuntimeSourceKind::Base, TEXT("ForeignStage"), {{TEXT("E"), EY01SourceEffectKind::AttackFlat, 2}}}}, {}, Plan).IsOk());
        TestTrue(TEXT("外层真实来源暂存"), S->StageMutation(Plan, Tx).IsOk());
        TestFalse(TEXT("来源半状态不得发动普攻"), F.ASC()->IsAbilityReady(F.ASC()->GetBasicAttackHandle()));
        TestTrue(TEXT("恢复来源"), S->RollbackMutation(Tx).IsOk()); return true;
    }
    if (Case == TEXT("CooldownABA"))
    { for (int32 I = 0; I < 4; ++I) { ++GFrameCounter; F.World->Tick(LEVELTICK_All, .25f); } }
    const auto BeforeTicket = F.View(); FY01SourceSnapshot BeforeTicketSource; F.P->GetSourceEffects()->Observe(BeforeTicketSource);
    auto Prepare = F.LearnRequest(B.ItemId, 0); const auto Ticket = F.Learn(Prepare);
    if (!Ok(TEXT("准备覆盖B"), Ticket)) { return false; }
    auto Confirm = F.Confirm(Prepare, Ticket.TicketId);
    if (Case == TEXT("CancelExactCooldown"))
    {
        TestEqual(TEXT("真实t10释放"), CastAt, 10.); TestEqual(TEXT("真实CD截止18"), Old.Deadline, 18.);
        TestTrue(TEXT("覆盖B只创建票据"), Ticket.TicketId.IsValid());
        TestEqual(TEXT("准备不推进实体业务版本"), F.View().Revision, BeforeTicket.Revision);
        Confirm.Operation = O::CancelLearn; Ok(TEXT("取消B覆盖"), F.Learn(Confirm));
        const auto After = F.View(); const auto& Actual = After.Skills[0];
        TestTrue(TEXT("取消后完整旧技能包括handle身份截止精确保持"), FY01LearnedSkill::StaticStruct()->CompareScriptStruct(&Old, &Actual, 0));
        TestEqual(TEXT("取消不推进实体业务版本"), After.Revision, BeforeTicket.Revision);
        TestEqual(TEXT("剩余CD仍8"), F.ASC()->GetSkillCooldownRemaining(0), 8.f);
        TestEqual(TEXT("书数保持"), After.Items.Num(), BeforeTicket.Items.Num());
        for (const auto& I : BeforeTicket.Items)
        { const auto ActualItem = F.Item(I.ItemId); TestTrue(TEXT("取消不换书身份/位置/成长/版本"), FY01ItemInstance::StaticStruct()->CompareScriptStruct(&I, &ActualItem, 0)); }
        FY01SourceSnapshot S; F.P->GetSourceEffects()->Observe(S);
        TestEqual(TEXT("取消来源版本不动"), S.Revision, BeforeTicketSource.Revision);
        TestEqual(TEXT("取消保留原来源数量"), S.Effects.Num(), BeforeTicketSource.Effects.Num());
        for (int32 I = 0; I < FMath::Min(S.Effects.Num(), BeforeTicketSource.Effects.Num()); ++I)
        {
            TestEqual(TEXT("取消来源完整Id"), S.Effects[I].Source.Id, BeforeTicketSource.Effects[I].Source.Id);
            TestEqual(TEXT("取消来源EffectId"), S.Effects[I].EffectId, BeforeTicketSource.Effects[I].EffectId);
            TestEqual(TEXT("取消来源数值"), S.Effects[I].Magnitude, BeforeTicketSource.Effects[I].Magnitude);
        }
        TestNotNull(TEXT("取消保留真实原GA"), F.ASC()->FindAbilitySpecFromHandle(Old.GAHandle));
        TestEqual(TEXT("取消攻击仍40"), F.Attack(), 40.f);
        ++GFrameCounter; F.World->Tick(LEVELTICK_All, .25f);
        TestEqual(TEXT("真实时钟继续减少原CD"), F.ASC()->GetSkillCooldownRemaining(0), 7.75f);
        Confirm = F.Confirm(Prepare, Ticket.TicketId); Code(TEXT("已取消票据不可确认"), F.Learn(Confirm), E::StaleTicket); return true;
    }
    if (Case == TEXT("ConfirmDuplicate"))
    { Ok(TEXT("另槽先学B"), F.Learn(F.LearnRequest(B.ItemId, 1))); Confirm.Seq = F.View().ExpectedSeq; TestFalse(TEXT("确认重验判重和书"), F.Learn(Confirm).IsOk()); return true; }
    if (Case == TEXT("ConfirmForeign")) { Code(TEXT("确认重新鉴权"), F.Learn(Confirm, Target), E::Authority); return true; }
    if (Case == TEXT("ConfirmDead")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    if (Case == TEXT("ConfirmEnded")) { F.Mode->TryEndMatch(false); }
    if (Case == TEXT("BookRevision")) { ++Confirm.ExpectedEntityRevision; }
    if (Case == TEXT("LearnedRevision")) { ++Confirm.ExpectedLearnedRevision; }
    if (Case == TEXT("ConfirmDead") || Case == TEXT("ConfirmEnded") || Case == TEXT("BookRevision") || Case == TEXT("LearnedRevision"))
    { TestFalse(TEXT("确认再次验证实体存活阶段"), F.Learn(Confirm).IsOk()); TestEqual(TEXT("失败不耗书"), F.View().Items.Num(), 2); return true; }
    if (Case == TEXT("IncomeRetained"))
    {
        FY01PreparedMutation Plan; FY01SourceTransaction Tx; auto* S = F.P->GetSourceEffects();
        TestTrue(TEXT("基础收入"), S->PrepareMutation({{EY01RuntimeSourceKind::Base, TEXT("Income"), {{TEXT("Rate"), EY01SourceEffectKind::GoldPerSecond, 100}}}}, {}, Plan).IsOk());
        TestTrue(TEXT("暂存收入"), S->StageMutation(Plan, Tx).IsOk()); TestTrue(TEXT("提交收入"), S->CommitMutation(Tx).IsOk());
        TestTrue(TEXT("启动收入"), F.P->GetIncome()->Start().IsOk()); FPlatformProcess::Sleep(.03f);
    }
    const auto Before = F.View(); FY01SourceSnapshot BeforeSource; F.P->GetSourceEffects()->Observe(BeforeSource);
    const auto Handles = F.ASC()->GetActiveEffects(FGameplayEffectQuery()); const int32 Abilities = F.ASC()->GetActivatableAbilities().Num();
    const auto OriginalConfirm = Confirm;
    auto Observe = [&]
    {
        FY01LoadoutSnapshot V; Code(TEXT("GA生命周期中库存Busy"), F.L()->Observe(V), E::Busy);
        FY01SourceSnapshot S; TestEqual(TEXT("GA生命周期中来源Busy"), F.P->GetSourceEffects()->Observe(S).Code, EY01RuntimeError::Busy);
        TestEqual(TEXT("GA生命周期中收入Observe不发布半状态"), F.P->GetIncome()->Observe().State.Fault.Code, EY01RuntimeError::Busy);
        TestTrue(TEXT("GA生命周期中ASC业务观察隐藏"), F.ASC()->GetGrantedSources().IsEmpty());
    };
    if (Case.StartsWith(TEXT("Grant")))
    {
        UY01LearnProbeAbility::Given = [&](UAbilitySystemComponent* ASC, FGameplayAbilitySpecHandle Handle)
        {
            ++Calls; if (Case == TEXT("GrantObserve")) { Observe(); }
            if (Case == TEXT("GrantReentry")) { Code(TEXT("真实GA授予重入Busy"), F.Learn(Confirm), E::Busy); }
            if (Case == TEXT("GrantDeath")) { ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
            if (Case == TEXT("GrantEnded")) { F.Mode->TryEndMatch(false); }
            if (Case == TEXT("GrantCleared")) { ASC->ClearAbility(Handle); }
        };
    }
    if (Case.StartsWith(TEXT("Retire")))
    {
        UY01LearnProbeAbility::Removed = [&](UAbilitySystemComponent*, FGameplayAbilitySpecHandle Handle)
        {
            if (Handle != Old.GAHandle) { return; } ++Calls;
            if (Case == TEXT("RetireObserve")) { Observe(); }
            if (Case == TEXT("RetireReentry")) { Code(TEXT("退休GA回调重入Busy"), F.Learn(Confirm), E::Busy); }
            if (Case == TEXT("RetireDeath")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
            if (Case == TEXT("RetireEnded")) { F.Mode->TryEndMatch(false); }
        };
    }
    FDelegateHandle H;
    if (Case == TEXT("GERejected") || Case == TEXT("DerivedGERejected") || Case == TEXT("IncomeRetained"))
    { F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda([&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { ++Calls; return false; })); }
    if (Case == TEXT("InhibitReentry") || Case == TEXT("RemoveReentry"))
    {
        if (Case == TEXT("RemoveReentry")) { Confirm = F.LearnRequest({}, 0, O::RemoveSkill); }
        H = F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).AddLambda([&](const FOnAttributeChangeData&)
        { if (++Calls == 1) { Code(TEXT("可逆抑制期间重入Busy"), F.Learn(Confirm), E::Busy); } });
    }
    if (Case == TEXT("ApplyDeath") || Case == TEXT("ApplyEnded") || Case == TEXT("PayloadOwned"))
    {
        H = F.ASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda([&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle)
        {
            if (++Calls != 1) { return; }
            if (Case == TEXT("ApplyDeath")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
            if (Case == TEXT("ApplyEnded")) { F.Mode->TryEndMatch(false); }
            if (Case == TEXT("PayloadOwned")) { Confirm.ItemId = A2.ItemId; Confirm.Slot = 2; }
        });
    }
    const auto R = F.Learn(Confirm);
    UY01LearnProbeAbility::Given = {}; UY01LearnProbeAbility::Removed = {};
    F.ASC()->GameplayEffectApplicationQueries.Reset(); F.ASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(H);
    F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).Remove(H);
    const bool Success = Case.StartsWith(TEXT("Retire")) || Case == TEXT("GrantObserve") || Case == TEXT("PayloadOwned") || Case == TEXT("CooldownABA");
    if (Success)
    {
        Ok(TEXT("共同提交后通知不再报失败"), R); TestTrue(TEXT("新身份"), F.View().Skills[0].LearnedId != Old.LearnedId);
        TestEqual(TEXT("消耗B一本"), F.View().Items.Num(), 1); TestNull(TEXT("旧GA移除"), F.ASC()->FindAbilitySpecFromHandle(Old.GAHandle));
        if (Case == TEXT("CooldownABA"))
        {
            TestEqual(TEXT("B于t11覆盖"), F.World->GetTimeSeconds(), 11.);
            for (int32 I = 0; I < 4; ++I) { ++GFrameCounter; F.World->Tick(LEVELTICK_All, .25f); }
            const auto PA = F.LearnRequest(A2.ItemId, 0); const auto TA = F.Learn(PA);
            Ok(TEXT("t12覆盖回A"), F.Learn(F.Confirm(PA, TA.TicketId))); const auto NewA = F.View().Skills[0];
            TestTrue(TEXT("A新LearnedId"), NewA.LearnedId != Old.LearnedId); TestTrue(TEXT("A新GA句柄"), NewA.GAHandle != Old.GAHandle);
            TestEqual(TEXT("无历史CD"), NewA.Deadline, 0.); TestEqual(TEXT("显示CD立即0"), F.ASC()->GetSkillCooldownRemaining(0), 0.f);
            TestTrue(TEXT("t12实际可发动A"), F.ASC()->TryActivateSkillSlot(0)); TestEqual(TEXT("新CD截止20"), F.View().Skills[0].Deadline, 20.);
        }
        if (Case == TEXT("PayloadOwned")) { Ok(TEXT("原入口payload仍可重放"), F.Learn(OriginalConfirm)); Code(TEXT("别名改写不能污染原结果"), F.Learn(Confirm), E::PayloadConflict); }
    }
    else
    {
        TestFalse(TEXT("故障整笔失败"), R.IsOk()); TestTrue(TEXT("故障确实触发"), Calls > 0);
        if (Case == TEXT("GERejected") || Case == TEXT("DerivedGERejected") || Case == TEXT("IncomeRetained"))
        { TestEqual(TEXT("技能汇总GE只查询一次"), Calls, 1); }
        const auto After = F.View(); TestEqual(TEXT("库存版本精确恢复"), After.Revision, Before.Revision);
        TestEqual(TEXT("保留两本未消费书"), After.Items.Num(), Before.Items.Num()); TestEqual(TEXT("旧LearnedId"), After.Skills[0].LearnedId, Old.LearnedId);
        for (const auto& I : Before.Items)
        { const auto ActualItem = F.Item(I.ItemId); TestTrue(TEXT("技术失败保留每一本原书完整身份及快照"), FY01ItemInstance::StaticStruct()->CompareScriptStruct(&I, &ActualItem, 0)); }
        TestEqual(TEXT("旧实体版本"), After.Skills[0].EntityRevision, Old.EntityRevision); TestEqual(TEXT("旧GA精确handle"), After.Skills[0].GAHandle, Old.GAHandle);
        TestEqual(TEXT("旧CD精确截止"), After.Skills[0].Deadline, Old.Deadline); TestNotNull(TEXT("旧GA仍为原对象"), F.ASC()->FindAbilitySpecFromHandle(Old.GAHandle));
        TestEqual(TEXT("无残余新GA"), F.ASC()->GetActivatableAbilities().Num(), Abilities);
        FY01SourceSnapshot S; F.P->GetSourceEffects()->Observe(S); TestEqual(TEXT("来源版本不变"), S.Revision, BeforeSource.Revision);
        TestEqual(TEXT("base原样"), S.BaseAttack, BeforeSource.BaseAttack); TestEqual(TEXT("current原样"), S.CurrentAttack, BeforeSource.CurrentAttack);
        TestEqual(TEXT("技术失败来源数量精确恢复"), S.Effects.Num(), BeforeSource.Effects.Num());
        for (int32 I = 0; I < FMath::Min(S.Effects.Num(), BeforeSource.Effects.Num()); ++I)
        {
            const auto& ActualEffect = S.Effects[I]; const auto& ExpectedEffect = BeforeSource.Effects[I];
            TestEqual(TEXT("来源Id精确恢复"), ActualEffect.Source.Id, ExpectedEffect.Source.Id); TestEqual(TEXT("来源LocalKey精确恢复"), ActualEffect.Source.LocalKey, ExpectedEffect.Source.LocalKey);
            TestEqual(TEXT("来源Kind精确恢复"), ActualEffect.Source.Kind, ExpectedEffect.Source.Kind); TestEqual(TEXT("来源EffectId精确恢复"), ActualEffect.EffectId, ExpectedEffect.EffectId);
            TestEqual(TEXT("效果种类精确恢复"), ActualEffect.Kind, ExpectedEffect.Kind); TestEqual(TEXT("效果数值精确恢复"), ActualEffect.Magnitude, ExpectedEffect.Magnitude);
        }
        TestEqual(TEXT("无残余GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), Handles.Num());
        for (auto Handle : Handles) { TestNotNull(TEXT("原GE不清除重建"), F.ASC()->GetActiveGameplayEffect(Handle)); }
        if (Case == TEXT("IncomeRetained")) { TestTrue(TEXT("旧合法收入不追回"), F.P->GetGold() >= 1); }
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R36EconomyDefaultTest, "Y01.Framework.R36.Economy.DefaultDisabled",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R36EconomyDefaultTest::RunTest(const FString&)
{
    FLoadoutWorld F;
    int32 Count = 0; UActorComponent* Economy = nullptr;
    for (auto* C : F.P->GetComponents())
    { if (C && C->GetClass()->GetFName() == TEXT("Y01EconomyComponent")) { Economy = C; ++Count; } }
    TestEqual(TEXT("真实PlayerState唯一经济组件"), Count, 1);
    if (!TestNotNull(TEXT("默认经济组件存在但未启用"), Economy)) { return false; }
    TestFalse(TEXT("经济默认关闭"), Economy->IsActive());
    TestFalse(TEXT("经济无Tick"), Economy->PrimaryComponentTick.bCanEverTick);
    TestEqual(TEXT("默认不改金币"), F.P->GetGold(), 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R36EconomySessionTest, "Y01.Framework.R36.Economy.Session",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R36EconomySessionTest::RunTest(const FString&)
{
    FLoadoutWorld F; F.P->SetOwner(F.PC);
    auto* C = F.P->GetEconomy(); const FY01DataCatalog Catalog(F.Data.Tables);
    TestFalse(TEXT("无库存会话不能启用经济"), C->Initialize(F.Match, 1, 1, Catalog, {}).IsOk());
    TestTrue(TEXT("显式库存会话"), F.Init().IsOk());
    TestFalse(TEXT("未显式启用"), C->IsInitialized());
    if (!TestTrue(TEXT("可信服务器显式启用同会话"), C->Initialize(F.Match, 1, 1, Catalog, {}).IsOk())) { return false; }
    TestTrue(TEXT("相同会话重复不重置"), C->Initialize(F.Match, 1, 1, Catalog, {}).IsOk());
    TestEqual(TEXT("错Match拒绝"), C->Initialize(FGuid::NewGuid(), 1, 1, Catalog, {}).Code, EY01ItemError::Session);
    TestEqual(TEXT("错Join拒绝"), C->Initialize(F.Match, 2, 1, Catalog, {}).Code, EY01ItemError::Session);
    TestEqual(TEXT("错配置拒绝"), C->Initialize(F.Match, 1, 2, Catalog, {}).Code, EY01ItemError::StaleConfig);
    auto* Rogue = NewObject<UY01EconomyComponent>(F.P);
    TestEqual(TEXT("第二组件无权"), Rogue->Initialize(F.Match, 1, 1, Catalog, {}).Code, EY01ItemError::Authority);
    TestNull(TEXT("初始化不是RPC"), C->FindFunction(TEXT("Initialize")));
    C->EndPlay(EEndPlayReason::Quit); C->EndPlay(EEndPlayReason::Quit);
    TestFalse(TEXT("结束后停止"), C->IsActive());
    TestEqual(TEXT("结束不可重启"), C->Initialize(F.Match, 1, 1, Catalog, {}).Code, EY01ItemError::Stopped);
    return true;
}

struct FY01R36EconomyTestAccess
{
    static void Clock(UY01IncomeComponent* I, double& Now) { I->TestClock = [&Now] { return Now; }; }
    static void ReentryClock(UY01IncomeComponent* I, TFunction<double()> Clock) { I->TestClock = MoveTemp(Clock); }
    static void Seeds(UY01EconomyComponent* C)
    { C->ShoppingRandom.Initialize(123); C->SynthesisRandom.Initialize(456); C->LootRandom.Initialize(789); }
    static int32 TicketSeed(uint32 Ticket, int32 Draw = 1)
    {
        uint32 Inverse = 1; for (int32 I = 0; I < 5; ++I) { Inverse *= 2u - 196314165u * Inverse; }
        while (Draw-- > 0) { Ticket = (Ticket - 907633515u) * Inverse; }
        return static_cast<int32>(Ticket);
    }
    static void ShoppingSeed(UY01EconomyComponent* C, int32 Seed) { C->ShoppingRandom.Initialize(Seed); }
    static void Config(UY01LoadoutComponent* L) { ++L->ConfigRevision; }
    static TArray<int32> Seeds(const UY01EconomyComponent* C)
    { return {C->ShoppingRandom.GetCurrentSeed(), C->SynthesisRandom.GetCurrentSeed(), C->LootRandom.GetCurrentSeed()}; }
    static void Spending(UY01IncomeComponent* I, bool Sequence)
    { if (Sequence) { I->State.Sequence = MAX_int64; } else { I->State.TotalSpending = MAX_int64; } }
    static FY01ItemResult Batch(UY01LoadoutComponent* L, AController* C, const FY01DataCatalog& Catalog,
        const TArray<FY01ItemGrant>& Grants, const TArray<FY01ItemConsume>& Consumes)
    {
        if (!L->Transactions.Enter()) { return L->Fail(EY01ItemError::Busy, TEXT("重入")); }
        ON_SCOPE_EXIT { L->Transactions.Leave(); L->PublishCommittedView(); };
        UY01LoadoutComponent::FPreparedBatch B;
        const auto R = L->PrepareBatch(Catalog, Grants, Consumes, B); if (!R.IsOk()) { return R; }
        const double Now = 0; const auto S = L->FromSource(L->Player->GetIncome()->Settle());
        if (!S.IsOk()) { return S; }
        return L->CommitBatch(C, nullptr, MoveTemp(B), {}, {}, Now, [] { return FY01ItemResult(); }, [] {});
    }
    static FY01ItemResult Atomic(UY01LoadoutComponent* L, AController* Controller, const FY01DataCatalog& Catalog,
        const FY01ItemRequest& Request, const TArray<FY01ItemGrant>& Grants,
        const TArray<FY01ResolvedSource>& Sources, const TArray<FY01RuntimeSourceId>& Removed, double Now)
    {
        if (!L->Transactions.Enter()) { return L->Fail(EY01ItemError::Busy, TEXT("重入")); }
        ON_SCOPE_EXIT { L->Transactions.Leave(); L->PublishCommittedView(); };
        FY01ItemResult Result; if (!L->Transactions.Admit(Request, L->Revision, Result)) { return Result; }
        auto Work = [&]()
        {
            auto* Income = L->Player->GetIncome(); auto* Economy = L->Player->GetEconomy();
            auto R = L->FromSource(Income->SettleAt(Now)); if (!R.IsOk()) { return R; }
            UY01IncomeComponent::FPreparedSpending Spending;
            R = Income->PrepareSpending(100, Now, Spending); if (!R.IsOk()) { return R; }
            auto Random = Economy->ShoppingRandom; Random.GetFraction();
            UY01LoadoutComponent::FPreparedBatch Batch;
            R = L->PrepareBatch(Catalog, Grants, Request.Materials, Batch); if (!R.IsOk()) { return R; }
            return L->CommitBatch(Controller, &Request, MoveTemp(Batch), Sources, Removed, Now,
                [&] { return Income->ValidateSpending(Spending); },
                [&] { Income->CommitSpending(MoveTemp(Spending)); Economy->ShoppingRandom = Random; });
        };
        Result = Work();
        if (L->Transactions.ExpectedSeq == Request.Seq) { auto Receipt = Result; L->Transactions.Record(Request, MoveTemp(Receipt)); }
        return Result;
    }
    static void Grow(UY01LoadoutComponent* L, FGuid Id)
    { auto& G = L->Items.FindChecked(Id).Growth[0]; G.AppliedCount = 7; G.Accumulated = 21; }
    static void Limit(UY01LoadoutComponent* L, int32 Which)
    {
        if (Which == 0) { L->Revision = MAX_uint64; }
        if (Which == 1) { L->AcquisitionSerial = MAX_uint64; }
        if (Which == 2) { for (auto& P : L->Items) { P.Value.EntityRevision = MAX_uint64; } }
        if (Which == 3) { L->CommittedView.ViewRevision = MAX_uint64; }
    }
};
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36BatchTest, "Y01.Framework.R36.Economy.Batch",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36BatchTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("NetChanges"), TEXT("DuplicateConsume"), TEXT("MissingConsume"), TEXT("StaleConsume"),
        TEXT("EquippedConsume"), TEXT("ForeignConsume"), TEXT("SecondDefinition"), TEXT("SecondIdentity"),
        TEXT("RevisionLimit"), TEXT("AcquisitionLimit"), TEXT("EntityLimit"), TEXT("ViewLimit"), TEXT("LearnedConsume")}) { Names.Add(N); Commands.Add(N); }
}
bool FY01R36BatchTest::RunTest(const FString& Case)
{
    using E = EY01ItemError; FLoadoutWorld F; F.P->SetOwner(F.PC);
    if (!TestTrue(TEXT("初始化"), F.Init().IsOk())) { return false; }
    F.Mode->TryStartNextWave(); auto A = F.Grant(), B = F.Grant(), C = F.Grant(), D = F.Grant();
    TestTrue(TEXT("材料完整入包"), F.Add({A, B}).IsOk());
    TArray<FY01ItemGrant> Grants{C, D}; TArray<FY01ItemConsume> Consumes{{A.ItemId, 1}, {B.ItemId, 1}};
    E Expected = E::None;
    if (Case == TEXT("DuplicateConsume")) { Consumes[1] = Consumes[0]; Expected = E::DuplicateItem; }
    if (Case == TEXT("MissingConsume")) { Consumes[1].ItemId = FGuid::NewGuid(); Expected = E::MissingItem; }
    if (Case == TEXT("StaleConsume")) { ++Consumes[1].ExpectedEntityRevision; Expected = E::StaleEntity; }
    if (Case == TEXT("EquippedConsume"))
    { TestTrue(TEXT("材料穿在身上"), F.Send(F.Request(A.ItemId, 0)).IsOk()); Consumes[0].ExpectedEntityRevision = 2; Expected = E::InvalidLocation; }
    if (Case == TEXT("LearnedConsume"))
    {
        F.Book(TEXT("MaterialBook")); auto Book = F.Grant(TEXT("MaterialBook"), K::Skill);
        if (!TestTrue(TEXT("真实技能书入包"), F.Add({Book}).IsOk())) { return false; }
        if (!TestTrue(TEXT("真实耗书学习"), F.L()->PrepareLearn(F.PC, F.LearnRequest(Book.ItemId, 0)).IsOk())) { return false; }
        const auto Learned = F.View().Skills[0];
        Consumes[1] = {Learned.LearnedId, Learned.EntityRevision}; Expected = E::MissingItem;
    }
    if (Case == TEXT("ForeignConsume"))
    {
        auto* Other = F.Player()->GetPlayerState<AY01PlayerState>(); auto* L = Other->GetLoadout();
        FString Error;
        TestTrue(TEXT("后加入玩家先初始化角色"), Other->TryInitializeCharacter(FY01DataCatalog(F.Data.Tables), FY01MatchFixture::Ref(K::Character), Error));
        TestTrue(TEXT("其他玩家同局"), L->Initialize(F.Match, 2).IsOk());
        TestTrue(TEXT("其他玩家物品"), L->AddItems(FY01DataCatalog(F.Data.Tables), {D}).IsOk());
        Consumes[1].ItemId = D.ItemId; Expected = E::MissingItem;
    }
    if (Case == TEXT("SecondDefinition")) { Grants[1].Definition.RowName = TEXT("Absent"); Expected = E::InvalidDefinition; }
    if (Case == TEXT("SecondIdentity")) { Grants[1].ItemId = Grants[0].ItemId; Expected = E::DuplicateItem; }
    if (Case.EndsWith(TEXT("Limit")))
    {
        FY01R36EconomyTestAccess::Limit(F.L(), Case == TEXT("RevisionLimit") ? 0 : Case == TEXT("AcquisitionLimit") ? 1 : Case == TEXT("ViewLimit") ? 3 : 2);
        if (Case == TEXT("EntityLimit")) { for (auto& I : Consumes) { I.ExpectedEntityRevision = MAX_uint64; } }
        Expected = E::Range;
    }
    const auto Before = F.View();
    FY01CommittedView Baseline; F.L()->ReadCommittedView(Baseline);
    FY01ViewSender Sender; FY01ViewReceiver Receiver; FY01ViewWire Wire; FString WireError;
    auto Transfer = [&](const FY01CommittedView& View)
    {
        if (!TestTrue(TEXT("真实完整视图生成提交清单"), Sender.Offer(View, Wire, WireError))) { return false; }
        int32 Windows = 0;
        while (!Wire.Items.IsEmpty() && ++Windows < 100)
        {
            FY01ViewAck Ack;
            if (!TestTrue(TEXT("生产Codec和接收器处理每窗"), Receiver.Receive(Wire, Ack, WireError))) { return false; }
            if (!TestTrue(TEXT("确认完整窗口"), Sender.Acknowledge(Ack, Wire, WireError))) { return false; }
        }
        return TestTrue(TEXT("客户端完整重建无半次数据"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&View, &Receiver.Get(), 0));
    };
    if (Case == TEXT("NetChanges")) { Transfer(Baseline); }
    const auto R = FY01R36EconomyTestAccess::Batch(F.L(), F.PC, FY01DataCatalog(F.Data.Tables), Grants, Consumes);
    TestEqual(TEXT("真实批次结果"), R.Code, Expected);
    const auto After = F.View();
    if (Expected != E::None)
    {
        TestTrue(TEXT("失败完整保留身份位置成长和获取顺序"), FY01LoadoutSnapshot::StaticStruct()->CompareScriptStruct(&Before, &After, 0));
    }
    else
    {
        TestEqual(TEXT("一次批次一个版本"), After.Revision, Before.Revision + 1);
        TestEqual(TEXT("净增删数量"), After.Items.Num(), 2);
        TestEqual(TEXT("四条变更含墓碑"), R.Changes.Num(), 4);
        if (R.Changes.Num() == 4)
        {
            TestEqual(TEXT("首材料墓碑"), R.Changes[0].Kind, EY01ItemChangeKind::Removed);
            TestEqual(TEXT("墓碑身份"), R.Changes[0].Item.ItemId, A.ItemId);
            TestEqual(TEXT("墓碑版本"), R.Changes[0].Item.EntityRevision, uint64(2));
        }
        TestEqual(TEXT("连续获取序号不复用"), F.Item(C.ItemId).AcquisitionOrder, uint64(3));
        FY01CommittedView V; TestTrue(TEXT("同批完整视图"), F.L()->ReadCommittedView(V));
        TestEqual(TEXT("完整视图版本"), V.Revision, After.Revision);
        TestFalse(TEXT("完整视图删除旧身份"), V.Items.ContainsByPredicate([&](const auto& I) { return I.ItemId == A.ItemId || I.ItemId == B.ItemId; }));
        if (Case == TEXT("NetChanges")) { Transfer(V); TestEqual(TEXT("客户端应用实际墓碑"), Receiver.Get().Items.Num(), 2); }
    }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36PurchaseTest, "Y01.Framework.R36.Economy.Purchase",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36PurchaseTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("SingleReplay"), TEXT("Quantity"), TEXT("Insufficient"), TEXT("UnknownOffer"),
        TEXT("PayloadOffer"), TEXT("PayloadConfig"), TEXT("PayloadMaterials"), TEXT("Ring64"), TEXT("Gap"),
        TEXT("CrossOwner"), TEXT("CrossMatch"), TEXT("CrossJoin"), TEXT("Dead"), TEXT("Waiting"), TEXT("Ended"),
        TEXT("StaleConfig"), TEXT("SpendingLimit"), TEXT("AuditLimit"), TEXT("RevisionLimit"), TEXT("AcquisitionLimit"),
        TEXT("SnapshotOwned"), TEXT("RandomTierRejected"), TEXT("DirectRejected"), TEXT("InvalidPrice"),
        TEXT("PoolNaN"), TEXT("PoolPrecision"), TEXT("OldIncomeKept"), TEXT("ReinitializeKeepsStreams"),
        TEXT("PoolResolution"), TEXT("BusySource"), TEXT("BusyClock"), TEXT("QuantityRange"), TEXT("LegacyPayload"),
        TEXT("ForbiddenIntent"), TEXT("SkillBook"), TEXT("ZeroWeight"), TEXT("ViewLimit"), TEXT("ProductReceipt")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36PurchaseTest::RunTest(const FString& Case)
{
    using E = EY01ItemError; FLoadoutWorld F; F.P->SetOwner(F.PC); double Now = 0;
    FY01R36EconomyTestAccess::Clock(F.P->GetIncome(), Now);
    TestTrue(TEXT("库存显式初始化"), F.Init().IsOk());
    auto* C = F.P->GetEconomy(); auto& Offer = F.Data.Row<FY01ShopOfferRow>(K::ShopOffer);
    Offer.Price = 100; Offer.Quantity = Case == TEXT("Quantity") ? 3 : 1;
    auto& Pool = F.Data.Row<FY01LocalItemPoolRow>(K::LocalItemPool); Pool.Entries[0].Item = FY01MatchFixture::Ref(K::Equipment, TEXT("Sword"));
    FY01WeightedEntry Second; Second.Item = FY01MatchFixture::Ref(K::Equipment, TEXT("Double")); Second.Weight = 1; Pool.Entries.Add(Second);
    const auto Ref = FY01MatchFixture::Ref(K::ShopOffer); const FY01DataCatalog Catalog(F.Data.Tables);
    if (Case == TEXT("RandomTierRejected")) { Offer.ShopKind = EY01ShopKind::RandomTier; }
    if (Case == TEXT("DirectRejected")) { Offer.ShopKind = EY01ShopKind::DirectEffect; }
    if (Case == TEXT("InvalidPrice")) { Offer.Price = -1; }
    if (Case == TEXT("PoolNaN")) { Pool.Entries[1].Weight = std::numeric_limits<float>::quiet_NaN(); }
    if (Case == TEXT("PoolPrecision")) { Pool.Entries[0].Weight = 1.e30f; Pool.Entries[1].Weight = 1.e-30f; }
    if (Case == TEXT("PoolResolution")) { Pool.Entries[0].Weight = 1; Pool.Entries[1].Weight = 1.e-10f; }
    if (Case == TEXT("ZeroWeight")) { Pool.Entries[0].Weight = 0; }
    if (Case == TEXT("QuantityRange")) { Offer.Quantity = MAX_int32; }
    if (Case == TEXT("SkillBook"))
    { F.Book(TEXT("Book")); Pool.ItemType = K::Skill; for (auto& Entry : Pool.Entries) { Entry.Item = FY01MatchFixture::Ref(K::Skill, TEXT("Book")); } Pool.Entries.SetNum(1); }
    const auto Init = C->Initialize(F.Match, 1, 1, Catalog, {Ref});
    // 保留旧fullTestPath：只修改kind的旧夹具仍然非法；现在应在发布而非消费时拒绝。
    if (Case == TEXT("InvalidPrice") || Case.StartsWith(TEXT("Pool")) || Case == TEXT("DirectRejected") || Case == TEXT("RandomTierRejected"))
    { TestFalse(TEXT("无效字段组合不能发布，不等于拒绝合法kind"), Init.IsOk()); TestFalse(TEXT("失败保持关闭"), C->IsInitialized()); return true; }
    if (!TestTrue(TEXT("显式服务器商品快照"), Init.IsOk())) { AddInfo(Init.Message); return false; }
    FY01R36EconomyTestAccess::Seeds(C); const auto Seeds = FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(C));
    F.P->TryAddMatchReward(Case == TEXT("Insufficient") ? 99 : 500, 0);
    if (Case != TEXT("Waiting")) { F.Mode->TryStartNextWave(); }
    FY01ItemRequest Q; Q.MatchId = F.Match; Q.JoinSequence = 1; Q.Seq = 1; Q.ConfigRevision = 1;
    Q.Operation = EY01ItemOperation::Purchase; Q.Offer = Ref;
    E Expected = E::None; bool Admitted = true; AController* Requester = F.PC;
    if (Case == TEXT("Insufficient")) { Expected = E::InsufficientFunds; }
    if (Case == TEXT("UnknownOffer") || Case == TEXT("OldIncomeKept")) { Q.Offer.RowName = TEXT("Unpublished"); Expected = E::InvalidDefinition; }
    if (Case == TEXT("Gap")) { Q.Seq = 2; Expected = E::SequenceGap; Admitted = false; }
    if (Case == TEXT("CrossMatch")) { Q.MatchId = FGuid::NewGuid(); Expected = E::Session; Admitted = false; }
    if (Case == TEXT("CrossJoin")) { Q.JoinSequence = 2; Expected = E::Session; Admitted = false; }
    if (Case == TEXT("CrossOwner")) { Requester = F.Player(); Expected = E::Authority; Admitted = false; }
    if (Case == TEXT("Dead")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); Expected = E::Dead; }
    if (Case == TEXT("Waiting")) { Expected = E::Phase; }
    if (Case == TEXT("Ended")) { F.Mode->TryEndMatch(false); Expected = E::Phase; }
    if (Case == TEXT("StaleConfig")) { F.L()->SetConfigRevision(2); Expected = E::StaleConfig; }
    if (Case == TEXT("SpendingLimit") || Case == TEXT("AuditLimit"))
    { FY01R36EconomyTestAccess::Spending(F.P->GetIncome(), Case == TEXT("AuditLimit")); Expected = E::Range; }
    if (Case == TEXT("RevisionLimit") || Case == TEXT("AcquisitionLimit") || Case == TEXT("ViewLimit"))
    { FY01R36EconomyTestAccess::Limit(F.L(), Case == TEXT("RevisionLimit") ? 0 : Case == TEXT("ViewLimit") ? 3 : 1); Expected = E::Range; }
    if (Case == TEXT("QuantityRange")) { F.Add({F.Grant()}); Expected = E::Range; }
    if (Case == TEXT("ForbiddenIntent")) { Q.Materials.Add({FGuid::NewGuid(), 1}); Expected = E::InvalidOperation; }
    if (Case == TEXT("LegacyPayload"))
    {
        auto Item = F.Grant(); F.Add({Item}); auto Equip = F.Request(Item.ItemId, 0); Equip.Offer = Ref;
        TestEqual(TEXT("旧装配不接受夹带经济意图"), F.Send(Equip).Code, E::InvalidOperation); return true;
    }
    if (Case == TEXT("BusySource"))
    {
        FY01PreparedMutation Plan; FY01SourceTransaction Tx; auto* Sources = F.P->GetSourceEffects();
        TestTrue(TEXT("外部真实来源暂存准备"), Sources->PrepareMutation({}, {}, Plan).IsOk());
        TestTrue(TEXT("外部真实来源暂存"), Sources->StageMutation(Plan, Tx).IsOk());
        TestEqual(TEXT("共享来源忙拒绝"), C->TryPurchase(F.PC, Q).Code, E::Busy);
        Sources->RollbackMutation(Tx);
        TestEqual(TEXT("Busy不耗序号"), F.View().ExpectedSeq, uint64(1));
    }
    if (Case == TEXT("BusyClock"))
    {
        bool Once = false;
        FY01R36EconomyTestAccess::ReentryClock(F.P->GetIncome(), [&]
        { if (!Once) { Once = true; TestEqual(TEXT("时钟重入Busy"), C->TryPurchase(F.PC, Q).Code, E::Busy); } return Now; });
        const auto Interrupted = C->TryPurchase(F.PC, Q);
        FY01R36EconomyTestAccess::Clock(F.P->GetIncome(), Now);
        TestEqual(TEXT("准入后的重入整体失败"), Interrupted.Code, E::Interrupted);
        TestEqual(TEXT("仅外层失败消耗一次序号"), F.View().ExpectedSeq, uint64(2));
        TestEqual(TEXT("重入不扣款"), F.P->GetGold(), 500);
        TestTrue(TEXT("重入不耗三流"), FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(C)) == Seeds); return true;
    }
    if (Case == TEXT("SnapshotOwned")) { Offer.Price = 999; Pool.Entries.Reset(); F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, 999); }
    if (Case == TEXT("OldIncomeKept"))
    {
        F.Effect(TEXT("Rate"), EY01SourceEffectKind::GoldPerSecond, 1); FY01PreparedSource P; FY01SourceTransaction T;
        auto* S = F.P->GetSourceEffects(); TestTrue(TEXT("基础收入来源"), S->PrepareSource(Catalog, EY01RuntimeSourceKind::Base, TEXT("Rate"), {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Rate"))}, P).IsOk());
        TestTrue(TEXT("基础收入暂存"), S->StageSource(P, T).IsOk()); TestTrue(TEXT("基础收入提交"), S->CommitSource(T).IsOk());
        TestTrue(TEXT("启动旧段"), F.P->GetIncome()->Start().IsOk()); Now = 1.25;
    }
    const auto R = C->TryPurchase(Requester, Q);
    TestEqual(TEXT("购买走真实统一鉴权与账本"), R.Code, Expected); if (!R.IsOk()) { AddInfo(R.Message); }
    const auto V = F.View(); TestEqual(TEXT("准入才消耗序号"), V.ExpectedSeq, uint64(Admitted ? 2 : 1));
    const auto AfterSeeds = FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(C));
    TestEqual(TEXT("合成流从未推进"), AfterSeeds[1], Seeds[1]); TestEqual(TEXT("掉落流从未推进"), AfterSeeds[2], Seeds[2]);
    if (Expected != E::None)
    {
        TestEqual(TEXT("失败不耗购物流"), AfterSeeds[0], Seeds[0]); TestEqual(TEXT("失败不发货"), V.Items.Num(), Case == TEXT("QuantityRange") ? 1 : 0);
        TestEqual(TEXT("失败不扣款，旧合法收入保留"), F.P->GetGold(), Case == TEXT("Insufficient") ? 99 : Case == TEXT("OldIncomeKept") ? 501 : 500);
        if (Case == TEXT("OldIncomeKept")) { TestEqual(TEXT("旧段余量保留"), F.P->GetIncome()->Observe().State.Remainder, .25); }
        const auto Replay = C->TryPurchase(Requester, Q);
        TestTrue(TEXT("失败原结果缓存"), FY01ItemResult::StaticStruct()->CompareScriptStruct(&R, &Replay, 0)); return true;
    }
    const int32 Count = Case == TEXT("Quantity") ? 3 : 1;
    TestEqual(TEXT("500购100余额400"), F.P->GetGold(), 400); TestEqual(TEXT("每份独立实例"), V.Items.Num(), Count);
    TestEqual(TEXT("支出累计"), F.P->GetIncome()->Observe().State.TotalSpending, int64(100));
    const auto Income = F.P->GetIncome()->Observe();
    if (TestFalse(TEXT("支出流水存在"), Income.State.Recent.IsEmpty()))
    { TestEqual(TEXT("支出种类"), Income.State.Recent.Last().Kind, EY01IncomeEntryKind::Spending); TestEqual(TEXT("流水扣100"), Income.State.Recent.Last().Delta, int64(-100)); }
    FRandomStream ExpectedRandom(123); TSet<FGuid> Ids;
    for (const auto& I : V.Items)
    {
        FY01DataRef Item; FY01DataValidation Errors;
        if (Case != TEXT("SnapshotOwned")) { Catalog.SamplePool(Ref.Type == K::ShopOffer ? F.Data.Row<FY01ShopOfferRow>(K::ShopOffer).Pool : Ref, ExpectedRandom, Item, Errors); TestTrue(TEXT("真实Catalog逐份独立抽样"), I.Definition.Type == Item.Type && I.Definition.RowName == Item.RowName); }
        Ids.Add(I.ItemId); TestEqual(TEXT("只入包"), I.Location, EY01ItemLocation::Backpack);
    }
    if (Case != TEXT("SnapshotOwned")) { TestEqual(TEXT("购物正式流恰好按份数推进"), AfterSeeds[0], ExpectedRandom.GetCurrentSeed()); }
    if (Case == TEXT("SnapshotOwned"))
    { for (const auto& Item : V.Items) { for (const auto& Effect : Item.Effects) { TestTrue(TEXT("发货效果不读外部改表"), Effect.Magnitude != 999); } } }
    TestEqual(TEXT("FGuid不重复"), Ids.Num(), Count);
    for (const auto& Id : V.EquipmentSlots) { TestFalse(TEXT("不自动穿戴"), Id.IsValid()); }
    for (const auto& S : V.Skills) { TestFalse(TEXT("不自动学习"), S.LearnedId.IsValid()); }
    const auto Replay = C->TryPurchase(F.PC, Q);
    TestTrue(TEXT("成功原回执精确重放"), FY01ItemResult::StaticStruct()->CompareScriptStruct(&R, &Replay, 0));
    TestEqual(TEXT("重放不扣款"), F.P->GetGold(), 400); TestEqual(TEXT("重放不再入包"), F.View().Items.Num(), Count);
    TestTrue(TEXT("重放三流原样"), FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(C)) == AfterSeeds);
    FY01CommittedView Full; TestTrue(TEXT("完整业务视图"), F.L()->ReadCommittedView(Full));
    TestEqual(TEXT("同版本钱"), Full.Gold, 400); TestEqual(TEXT("同版本货"), Full.Items.Num(), Count);
    TestEqual(TEXT("同版本回执"), Full.Response.Code, E::None); TestEqual(TEXT("回执商品"), Full.Response.Request.Offer.RowName, Ref.RowName);
    if (Case == TEXT("ProductReceipt") || Case == TEXT("SkillBook"))
    {
        TestTrue(TEXT("单件购买回执包含已提交产物身份"), R.ProductItemId.IsValid());
        if (Full.Items.Num() == 1) { TestEqual(TEXT("产物是本次入包实例"), R.ProductItemId, Full.Items[0].ItemId); }
        TestEqual(TEXT("完整响应携带相同产物"), Full.Response.ProductItemId, R.ProductItemId);
        if (!Full.Receipts.IsEmpty()) { TestEqual(TEXT("防重账本携带相同产物"), Full.Receipts.Last().ProductItemId, R.ProductItemId); }
    }
    if (Case == TEXT("Quantity")) { TestFalse(TEXT("多件购买不伪称单一产物"), R.ProductItemId.IsValid()); }
    if (Case.StartsWith(TEXT("Payload")))
    {
        auto Conflict = Q;
        if (Case == TEXT("PayloadOffer")) { Conflict.Offer.RowName = TEXT("Other"); }
        if (Case == TEXT("PayloadConfig")) { ++Conflict.ConfigRevision; }
        if (Case == TEXT("PayloadMaterials")) { Conflict.Materials.Add({FGuid::NewGuid(), 1}); }
        TestEqual(TEXT("同号异意图拒绝"), C->TryPurchase(F.PC, Conflict).Code, E::PayloadConflict);
    }
    if (Case == TEXT("Ring64"))
    {
        for (uint64 I = 2; I <= 65; ++I) { auto Bad = Q; Bad.Seq = I; Bad.Offer.RowName = TEXT("Absent"); TestEqual(TEXT("业务拒绝缓存"), C->TryPurchase(F.PC, Bad).Code, E::InvalidDefinition); }
        TestEqual(TEXT("淘汰后不可重执行"), C->TryPurchase(F.PC, Q).Code, E::ExpiredSequence); TestEqual(TEXT("仍只扣一次"), F.P->GetGold(), 400);
    }
    if (Case == TEXT("ReinitializeKeepsStreams"))
    { TestTrue(TEXT("重复初始化"), C->Initialize(F.Match, 1, 1, Catalog, {Ref}).IsOk()); TestTrue(TEXT("不重新播种"), FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(C)) == AfterSeeds); }
    return true;
}

namespace
{
// 红阶段通过反射读取新业务字段；仍由真实生产发布器和Codec传输，未提供假购买实现。
int32 ShopPurchased(const FY01CommittedView& View, FName OfferId)
{
    const auto* Array = FindFProperty<FArrayProperty>(FY01CommittedView::StaticStruct(), TEXT("ShopOffers"));
    const auto* Inner = Array ? CastField<FStructProperty>(Array->Inner) : nullptr;
    if (!Inner) { return -1; }
    const auto* Id = FindFProperty<FNameProperty>(Inner->Struct, TEXT("OfferId"));
    const auto* Bought = FindFProperty<FBoolProperty>(Inner->Struct, TEXT("bPurchased"));
    if (!Id || !Bought) { return -1; }
    FScriptArrayHelper Values(Array, Array->ContainerPtrToValuePtr<void>(&View));
    for (int32 I = 0; I < Values.Num(); ++I)
    { if (Id->GetPropertyValue_InContainer(Values.GetRawPtr(I)) == OfferId) { return Bought->GetPropertyValue_InContainer(Values.GetRawPtr(I)) ? 1 : 0; } }
    return -1;
}
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36ShopTest, "Y01.Framework.R36.Economy.Shop",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36ShopTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Grade : {TEXT("D"), TEXT("C"), TEXT("B"), TEXT("A")})
    { for (const TCHAR* Edge : {TEXT("First"), TEXT("Before"), TEXT("At"), TEXT("Last")})
        { const FString N = FString::Printf(TEXT("Tier.%s.%s"), Grade, Edge); Names.Add(N); Commands.Add(N); } }
    for (const TCHAR* N : {TEXT("Tier.Quantity"), TEXT("Tier.Snapshot"), TEXT("Tier.Zero"), TEXT("Tier.InnerBefore"), TEXT("Tier.InnerAt"),
        TEXT("Tier.Skill"), TEXT("Tier.TableWeights"), TEXT("Direct.Single"), TEXT("Direct.Reinitialize"), TEXT("Direct.OtherOffer"),
        TEXT("Direct.OtherPlayer"), TEXT("Direct.NameCase"), TEXT("Direct.GrowthOnly"), TEXT("Direct.Snapshot"), TEXT("Direct.NoRevoke"),
        TEXT("Direct.DerivedGEFailure"), TEXT("Direct.Reentry"), TEXT("Direct.DeadApply"), TEXT("Direct.EndedApply"), TEXT("Direct.ConfigApply"),
        TEXT("Direct.Reward"), TEXT("Direct.RewardRollback"), TEXT("Direct.RateSwitch"), TEXT("Direct.AttackOverflow"),
        TEXT("Direct.SpendingLimit"), TEXT("Direct.AuditLimit"), TEXT("Direct.ViewLimit"), TEXT("Direct.RevisionLimit"),
        TEXT("Direct.Waiting"), TEXT("Direct.Dead"), TEXT("Direct.Ended"), TEXT("Direct.Battle"), TEXT("Direct.Ring64"),
        TEXT("Invalid.TierMissing"), TEXT("Invalid.TierDuplicate"), TEXT("Invalid.TierNegative"), TEXT("Invalid.TierNaN"),
        TEXT("Invalid.TierZero"), TEXT("Invalid.TierPrecision"), TEXT("Invalid.TierResolution"), TEXT("Invalid.TierType"),
        TEXT("Invalid.PoolType"), TEXT("Invalid.PoolQuality"), TEXT("Invalid.PoolEmpty"), TEXT("Invalid.PoolZero"), TEXT("Invalid.PoolResolution"),
        TEXT("Invalid.TierOldPool"), TEXT("Invalid.TierEffects"), TEXT("Invalid.TierGrowth"), TEXT("Invalid.TierQuality"),
        TEXT("Invalid.DirectEmpty"), TEXT("Invalid.DirectQuantity"), TEXT("Invalid.DirectPool"), TEXT("Invalid.DirectTier"),
        TEXT("Invalid.DirectQuality"), TEXT("Invalid.EffectDuplicate"), TEXT("Invalid.EffectMissing"), TEXT("Invalid.EffectNaN"),
        TEXT("Invalid.EffectKind"), TEXT("Invalid.GrowthDuplicate"), TEXT("Invalid.GrowthTarget"), TEXT("Invalid.GrowthNaN"),
        TEXT("Invalid.SkillFakeClass"), TEXT("Invalid.SkillWrongClass"), TEXT("Invalid.ZeroFakeClass"), TEXT("Invalid.SecondOffer"),
        TEXT("Invalid.ItemAttackOverflow"), TEXT("Invalid.ItemAttackUnderflow"), TEXT("Invalid.ItemEffectKind"),
        TEXT("Direct.GPSOnly"), TEXT("Direct.RateOverflow"), TEXT("Direct.SourceCollision"), TEXT("Direct.OwnedPayload"),
        TEXT("Direct.WireDuplicate"), TEXT("Direct.WireKind"), TEXT("Direct.WireSource"), TEXT("Direct.WireGrowth"), TEXT("Direct.WirePrice"),
        TEXT("Direct.WireRandomPurchased"), TEXT("Direct.WireForeignSession"), TEXT("Direct.WireNaN")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36ShopTest::RunTest(const FString& Case)
{
    using E = EY01ItemError; FLoadoutWorld F; F.P->SetOwner(F.PC); double Now = 0;
    FY01R36EconomyTestAccess::Clock(F.P->GetIncome(), Now);
    if (!TestTrue(TEXT("真实库存初始化"), F.Init().IsOk())) { return false; }
    auto* Economy = F.P->GetEconomy(); auto* Sources = F.P->GetSourceEffects();
    FY01ShopOfferRow Offer; Offer.ShopKind = EY01ShopKind::RandomTier; Offer.Price = 100;
    Offer.TierPool = FY01MatchFixture::Ref(K::QualityRollPool);
    FY01QualityRollPoolRow Tier;
    int32 Grade = Case.StartsWith(TEXT("Tier.C.")) ? 1 : Case.StartsWith(TEXT("Tier.B.")) ? 2 : Case.StartsWith(TEXT("Tier.A.")) ? 3 : 0;
    const double Weight = Grade == 3 ? 90 : 75;
    const bool Skill = Case == TEXT("Tier.Skill") || Case.Contains(TEXT("Class"));
    Tier.ItemType = Skill ? K::Skill : K::Equipment;
    for (int32 I = 0; I < 2; ++I)
    {
        const FName Id(I == 0 ? TEXT("Lower") : TEXT("Upper"));
        const auto Quality = static_cast<EY01Quality>(Grade + I);
        if (Skill) { F.Book(Id); F.Data.Row<FY01SkillRow>(K::Skill, Id).Quality = Quality; }
        else { F.Equipment(Id, {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten"))}); F.Data.Row<FY01EquipmentRow>(K::Equipment, Id).Quality = Quality; }
        FY01LocalItemPoolRow Pool; Pool.ItemType = Tier.ItemType; Pool.Quality = Quality;
        FY01WeightedEntry Entry; Entry.Item = FY01MatchFixture::Ref(Tier.ItemType, Id); Entry.Weight = 1; Pool.Entries.Add(Entry);
        Entry.Weight = 0; Pool.Entries.Add(Entry); F.Data.Add(K::LocalItemPool, Pool, Id);
        FY01QualityRollEntry T; T.Quality = Quality; T.Pool = FY01MatchFixture::Ref(K::LocalItemPool, Id); T.Weight = I == 0 ? Weight : 100 - Weight; Tier.Entries.Add(T);
    }
    const bool Direct = Case.StartsWith(TEXT("Direct.")) || Case.StartsWith(TEXT("Invalid.Direct"))
        || Case.StartsWith(TEXT("Invalid.Effect")) || Case.StartsWith(TEXT("Invalid.Growth"));
    F.Effect(TEXT("Rate"), EY01SourceEffectKind::GoldPerSecond, 4);
    FY01GrowthRuleRow Growth; Growth.Target = EY01GrowthTarget::Player; Growth.Attribute = EY01GrowthAttribute::Strength; Growth.Delta = 3;
    F.Data.Add(K::GrowthRule, Growth, TEXT("Strength"));
    if (Direct)
    {
        Offer.ShopKind = EY01ShopKind::DirectEffect; Offer.Price = 50; Offer.TierPool = {};
        Offer.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten")), FY01MatchFixture::Ref(K::SourceEffect, TEXT("Other")), FY01MatchFixture::Ref(K::SourceEffect, TEXT("Rate"))};
        Offer.GrowthRuleIds = {FY01MatchFixture::Ref(K::GrowthRule, TEXT("Strength"))};
    }
    auto& Pool = F.Data.Row<FY01LocalItemPoolRow>(K::LocalItemPool, TEXT("Lower"));
    if (Case == TEXT("Tier.Quantity")) { Offer.Quantity = 4; }
    if (Case == TEXT("Tier.Zero")) { Tier.Entries[0].Weight = 0; }
    if (Case == TEXT("Tier.TableWeights")) { Tier.Entries[0].Weight = 25; Tier.Entries[1].Weight = 75; }
    if (Case.Contains(TEXT("Inner")))
    {
        Tier.Entries[1].Weight = 0; F.Equipment(TEXT("Inner"), {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Other"))});
        Pool.Entries[1].Item = FY01MatchFixture::Ref(K::Equipment, TEXT("Inner")); Pool.Entries[1].Weight = 1;
        auto Zero = Pool.Entries[0]; Zero.Weight = 0; Pool.Entries.Insert(Zero, 0);
    }
    if (Case == TEXT("Invalid.TierMissing")) { Offer.TierPool.RowName = TEXT("Missing"); }
    if (Case == TEXT("Invalid.TierDuplicate")) { const auto Duplicate = Tier.Entries[0]; Tier.Entries.Add(Duplicate); }
    if (Case == TEXT("Invalid.TierNegative")) { Tier.Entries[0].Weight = -1; }
    if (Case == TEXT("Invalid.TierNaN")) { Tier.Entries[0].Weight = std::numeric_limits<double>::quiet_NaN(); }
    if (Case == TEXT("Invalid.TierZero")) { for (auto& T : Tier.Entries) { T.Weight = 0; } }
    if (Case == TEXT("Invalid.TierPrecision")) { Tier.Entries[0].Weight = 1.e300; Tier.Entries[1].Weight = 1.e-300; }
    if (Case == TEXT("Invalid.TierResolution")) { Tier.Entries[0].Weight = 1; Tier.Entries[1].Weight = 1.e-10; }
    if (Case == TEXT("Invalid.TierType")) { Tier.ItemType = K::MetaItem; }
    if (Case == TEXT("Invalid.PoolType")) { Pool.ItemType = K::Skill; }
    if (Case == TEXT("Invalid.PoolQuality")) { Pool.Quality = EY01Quality::Red; }
    if (Case == TEXT("Invalid.PoolEmpty")) { Pool.Entries.Reset(); }
    if (Case == TEXT("Invalid.PoolZero")) { Pool.Entries[0].Weight = 0; }
    if (Case == TEXT("Invalid.PoolResolution")) { Pool.Entries[1].Weight = 1.e-10f; }
    if (Case == TEXT("Invalid.TierOldPool") || Case == TEXT("Invalid.DirectPool")) { Offer.Pool = FY01MatchFixture::Ref(K::LocalItemPool); }
    if (Case == TEXT("Invalid.TierEffects")) { Offer.EffectIds.Add(FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten"))); }
    if (Case == TEXT("Invalid.TierGrowth")) { Offer.GrowthRuleIds.Add(FY01MatchFixture::Ref(K::GrowthRule, TEXT("Strength"))); }
    if (Case == TEXT("Invalid.TierQuality") || Case == TEXT("Invalid.DirectQuality")) { Offer.Quality = EY01Quality::Blue; }
    if (Case == TEXT("Invalid.DirectEmpty")) { Offer.EffectIds.Reset(); Offer.GrowthRuleIds.Reset(); }
    if (Case == TEXT("Invalid.DirectQuantity")) { Offer.Quantity = 2; }
    if (Case == TEXT("Invalid.DirectTier")) { Offer.TierPool = FY01MatchFixture::Ref(K::QualityRollPool); }
    if (Case == TEXT("Invalid.EffectDuplicate")) { const auto Duplicate = Offer.EffectIds[0]; Offer.EffectIds.Add(Duplicate); }
    if (Case == TEXT("Invalid.EffectMissing")) { Offer.EffectIds[0].RowName = TEXT("Absent"); }
    if (Case == TEXT("Invalid.EffectNaN")) { F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, std::numeric_limits<double>::quiet_NaN()); }
    if (Case == TEXT("Invalid.EffectKind")) { F.Effect(TEXT("Ten"), static_cast<EY01SourceEffectKind>(255), 1); }
    if (Case == TEXT("Invalid.GrowthDuplicate")) { const auto Duplicate = Offer.GrowthRuleIds[0]; Offer.GrowthRuleIds.Add(Duplicate); }
    if (Case == TEXT("Invalid.GrowthTarget")) { F.Data.Row<FY01GrowthRuleRow>(K::GrowthRule, TEXT("Strength")).Target = EY01GrowthTarget::EquipmentInstance; }
    if (Case == TEXT("Invalid.GrowthNaN")) { F.Data.Row<FY01GrowthRuleRow>(K::GrowthRule, TEXT("Strength")).Delta = std::numeric_limits<double>::quiet_NaN(); }
    if (Case == TEXT("Invalid.SkillFakeClass") || Case == TEXT("Invalid.ZeroFakeClass"))
    {
        F.Data.Row<FY01SkillRow>(K::Skill, TEXT("Lower")).Ability = TSoftClassPtr<UGameplayAbility>(FSoftObjectPath(TEXT("/Script/Y01.NoSuchShopAbility")));
        if (Case == TEXT("Invalid.ZeroFakeClass")) { Tier.Entries[0].Weight = 0; }
    }
    if (Case == TEXT("Invalid.SkillWrongClass")) { F.Data.Row<FY01SkillRow>(K::Skill, TEXT("Lower")).Ability = UGameplayAbility::StaticClass(); }
    if (Case == TEXT("Direct.GrowthOnly")) { Offer.EffectIds.Reset(); }
    F.Data.Add(K::QualityRollPool, Tier); F.Data.Add(K::ShopOffer, Offer);
    F.Data.Add(K::ShopOffer, Offer, TEXT("OtherOffer"));
    if (Case == TEXT("Invalid.SecondOffer")) { F.Data.Row<FY01ShopOfferRow>(K::ShopOffer, TEXT("OtherOffer")).Price = -1; }
    if (Case == TEXT("Invalid.ItemAttackOverflow")) { F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, 1.e300); }
    if (Case == TEXT("Invalid.ItemAttackUnderflow")) { F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, 1.e-300); }
    if (Case == TEXT("Invalid.ItemEffectKind")) { F.Effect(TEXT("Ten"), static_cast<EY01SourceEffectKind>(255), 1); }
    if (Case == TEXT("Direct.GPSOnly"))
    { Offer.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Rate"))}; Offer.GrowthRuleIds.Reset(); F.Data.Add(K::ShopOffer, Offer); }
    if (Case == TEXT("Direct.RateOverflow")) { F.Effect(TEXT("Rate"), EY01SourceEffectKind::GoldPerSecond, 1.e308); }
    if (Case == TEXT("Invalid.SkillFakeClass")) { AddExpectedError(TEXT("Failed to find object 'Object /Script/Y01.NoSuchShopAbility'"), EAutomationExpectedErrorFlags::Contains, 0); }
    const auto Ref = FY01MatchFixture::Ref(K::ShopOffer); const FY01DataCatalog Catalog(F.Data.Tables);
    const auto Init = Economy->Initialize(F.Match, 1, 1, Catalog, {Ref, FY01MatchFixture::Ref(K::ShopOffer, TEXT("OtherOffer"))});
    if (Case.StartsWith(TEXT("Invalid.")))
    { TestFalse(TEXT("所有公开商品及零权重依赖真实校验"), Init.IsOk()); TestFalse(TEXT("错误不得部分发布"), Economy->IsInitialized()); return true; }
    if (!TestTrue(TEXT("合法商品发布"), Init.IsOk())) { AddInfo(Init.Message); return false; }
    if (Case.StartsWith(TEXT("Tier.")))
    {
        FY01CommittedView State; F.L()->ReadCommittedView(State);
        if (TestEqual(TEXT("两件商品视图"), State.ShopOffers.Num(), 2))
        {
            const auto& S = State.ShopOffers[0]; TArray<FY01ShopOfferOdds> Want;
            for (const auto& T : Tier.Entries) { if (T.Weight > 0) { Want.Add({T.Quality, T.Weight}); } }
            TestEqual(TEXT("幸运箱视图带物品类型"), int32(S.ItemType), int32(Tier.ItemType));
            if (TestEqual(TEXT("幸运箱视图只列正权重品质"), S.Odds.Num(), Want.Num()))
            {
                for (int32 I = 0; I < Want.Num(); ++I)
                {
                    TestEqual(TEXT("品质按配置顺序"), int32(S.Odds[I].Quality), int32(Want[I].Quality));
                    TestEqual(TEXT("权重为配置原值"), S.Odds[I].Weight, Want[I].Weight);
                }
            }
        }
    }
    FY01R36EconomyTestAccess::Seeds(Economy); int32 Seed = 123;
    if (Case.StartsWith(TEXT("Tier.")))
    {
        const uint32 Boundary = static_cast<uint32>(FMath::CeilToDouble(Weight / 100. * 8388608.));
        uint32 Ticket = Case.EndsWith(TEXT("First")) ? 0 : Case.EndsWith(TEXT("Before")) ? Boundary - 1 : Case.EndsWith(TEXT("At")) ? Boundary : 8388607;
        if (Case == TEXT("Tier.TableWeights")) { Ticket = 4194304; }
        if (Case.Contains(TEXT("Inner"))) { Ticket = Case.EndsWith(TEXT("Before")) ? 4194303 : 4194304; }
        Seed = FY01R36EconomyTestAccess::TicketSeed(Ticket, Case.Contains(TEXT("Inner")) ? 2 : 1);
        FY01R36EconomyTestAccess::ShoppingSeed(Economy, Seed);
    }
    const auto Seeds = FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(Economy));
    F.P->TryAddMatchReward(500, 0); if (Case != TEXT("Direct.Waiting")) { F.Mode->TryStartNextWave(); }
    if (Case == TEXT("Direct.Battle")) { F.Mode->Tick(1); }
    if (Case == TEXT("Direct.Dead")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    if (Case == TEXT("Direct.Ended")) { F.Mode->TryEndMatch(false); }
    FY01ItemRequest Q; Q.MatchId = F.Match; Q.JoinSequence = 1; Q.Seq = 1; Q.ConfigRevision = 1; Q.Operation = EY01ItemOperation::Purchase; Q.Offer = Ref;
    if (Case == TEXT("Direct.NameCase")) { Q.Offer.RowName = TEXT("dEfAuLt"); }
    FRandomStream Expected(Seed); TArray<FName> ExpectedItems;
    if (!Direct)
    {
        for (int32 I = 0; I < Offer.Quantity; ++I)
        {
            double Ticket = Expected.GetFraction() * (Tier.Entries[0].Weight + Tier.Entries[1].Weight);
            const int32 Index = Ticket < Tier.Entries[0].Weight ? 0 : 1;
            FY01DataRef Item; FY01DataValidation Errors; Catalog.SamplePool(Tier.Entries[Index].Pool, Expected, Item, Errors); ExpectedItems.Add(Item.RowName);
        }
    }
    if (Case.EndsWith(TEXT("Snapshot")))
    { F.Data.Row<FY01ShopOfferRow>(K::ShopOffer).Price = 999; F.Data.Row<FY01QualityRollPoolRow>(K::QualityRollPool).Entries.Reset(); Pool.Entries.Reset(); F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, 999); F.Data.Row<FY01GrowthRuleRow>(K::GrowthRule, TEXT("Strength")).Delta = 999; }
    const bool OldIncome = Case == TEXT("Direct.RateSwitch") || Case.StartsWith(TEXT("Direct.Reward"));
    if (Case == TEXT("Direct.SourceCollision") || Case == TEXT("Direct.RateOverflow"))
    {
        FY01CommittedView State; F.L()->ReadCommittedView(State);
        FY01ResolvedSource Source; Source.Kind = Case == TEXT("Direct.SourceCollision") ? EY01RuntimeSourceKind::Shop : EY01RuntimeSourceKind::Base;
        Source.LocalKey = Case == TEXT("Direct.SourceCollision") ? State.ShopOffers[0].SourceKey : TEXT("HugeRate");
        if (Case == TEXT("Direct.SourceCollision"))
        { for (const auto& Efx : State.ShopOffers[0].Effects) { Source.Effects.Add({Efx.EffectId, Efx.Kind, Efx.Magnitude}); } }
        else { Source.Effects.Add({TEXT("HugeRate"), EY01SourceEffectKind::GoldPerSecond, 1.e308}); }
        FY01PreparedMutation Plan; FY01SourceTransaction Tx;
        TestTrue(TEXT("预置真实来源"), Sources->PrepareMutation({Source}, {}, Plan).IsOk());
        TestTrue(TEXT("预置暂存"), Sources->StageMutation(Plan, Tx).IsOk()); TestTrue(TEXT("预置提交"), Sources->CommitMutation(Tx).IsOk());
    }
    if (OldIncome)
    {
        FY01PreparedMutation Plan; FY01SourceTransaction Tx;
        FY01ResolvedSource Old{EY01RuntimeSourceKind::Base, TEXT("Old"), {{TEXT("OldRate"), EY01SourceEffectKind::GoldPerSecond, 1}}};
        if (Case == TEXT("Direct.RewardRollback")) { Old.Effects.Add({TEXT("OldAttack"), EY01SourceEffectKind::AttackFlat, 5}); }
        TestTrue(TEXT("旧收入准备"), Sources->PrepareMutation({Old}, {}, Plan).IsOk());
        TestTrue(TEXT("旧收入暂存"), Sources->StageMutation(Plan, Tx).IsOk()); TestTrue(TEXT("旧收入提交"), Sources->CommitMutation(Tx).IsOk());
        TestTrue(TEXT("旧段启动"), F.P->GetIncome()->Start().IsOk()); Now = 1.25;
    }
    if (Case == TEXT("Direct.AttackOverflow")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), MAX_flt); }
    if (Case == TEXT("Direct.SpendingLimit") || Case == TEXT("Direct.AuditLimit")) { FY01R36EconomyTestAccess::Spending(F.P->GetIncome(), Case == TEXT("Direct.AuditLimit")); }
    F.L()->PublishCommittedView(); FY01CommittedView Before; F.L()->ReadCommittedView(Before);
    FY01SourceSnapshot BeforeSources; Sources->Observe(BeforeSources); const auto BeforeHandles = F.ASC()->GetActiveEffects(FGameplayEffectQuery());
    if (Case == TEXT("Direct.ViewLimit") || Case == TEXT("Direct.RevisionLimit")) { FY01R36EconomyTestAccess::Limit(F.L(), Case == TEXT("Direct.ViewLimit") ? 3 : 0); }
    int32 Applied = 0; bool Once = false, RewardGranted = false, CandidateRemoved = false;
    FActiveGameplayEffectHandle CandidateHandle;
    F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda([&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec& Spec)
    {
        if (!Spec.Def || !Spec.Def->IsA(UY01EquipmentAttackEffect::StaticClass())) { return true; }
        ++Applied; return !(Case == TEXT("Direct.DerivedGEFailure") && Applied == 1);
    }));
    const auto Callback = F.ASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda([&](UAbilitySystemComponent*, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle)
    {
        if (Once || !Spec.Def || !Spec.Def->IsA(UY01EquipmentAttackEffect::StaticClass())) { return; } Once = true;
        FY01CommittedView Hidden; F.L()->ReadCommittedView(Hidden); TestEqual(TEXT("暂存只可读上一完整版本"), Hidden.ViewRevision, Before.ViewRevision);
        if (Case == TEXT("Direct.Reentry")) { TestEqual(TEXT("真实购买重入拒绝"), Economy->TryPurchase(F.PC, Q).Code, E::Busy); }
        if (Case == TEXT("Direct.DeadApply")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        if (Case == TEXT("Direct.EndedApply")) { F.Mode->TryEndMatch(false); }
        if (Case == TEXT("Direct.ConfigApply")) { FY01R36EconomyTestAccess::Config(F.L()); }
        if (Case == TEXT("Direct.OwnedPayload")) { Q.Offer.RowName = TEXT("Absent"); Q.Seq = 99; F.Data.Row<FY01ShopOfferRow>(K::ShopOffer).Price = 999; }
        if (Case.StartsWith(TEXT("Direct.Reward")))
        { RewardGranted = F.P->TryAddMatchReward(7, 0); TestTrue(TEXT("回调合法奖励"), RewardGranted); }
        if (Case == TEXT("Direct.RewardRollback"))
        {
            CandidateHandle = Handle;
            TestTrue(TEXT("回调参数为新候选句柄"), Handle.IsValid() && !BeforeHandles.Contains(Handle));
            CandidateRemoved = F.ASC()->RemoveActiveGameplayEffect(Handle);
            TestTrue(TEXT("奖励之后真实移除候选GE"), CandidateRemoved);
        }
    });
    const auto Original = Q; const auto R = Economy->TryPurchase(F.PC, Q); Q = Original;
    F.ASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(Callback); F.ASC()->GameplayEffectApplicationQueries.Reset();
    const bool Failure = Case == TEXT("Direct.DerivedGEFailure") || Case == TEXT("Direct.Reentry") || Case.EndsWith(TEXT("Apply"))
        || Case == TEXT("Direct.RewardRollback") || Case.EndsWith(TEXT("Overflow")) || Case.EndsWith(TEXT("Limit"))
        || Case == TEXT("Direct.Waiting") || Case == TEXT("Direct.Dead") || Case == TEXT("Direct.Ended") || Case == TEXT("Direct.SourceCollision");
    if (!TestEqual(TEXT("真实购买结果"), R.IsOk(), !Failure)) { AddInfo(R.Message); return false; }
    const auto AfterSeeds = FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(Economy));
    TestEqual(TEXT("合成流独立"), AfterSeeds[1], Seeds[1]); TestEqual(TEXT("掉落流独立"), AfterSeeds[2], Seeds[2]);
    TestEqual(TEXT("购物副本成功才提交"), AfterSeeds[0], Direct || Failure ? Seeds[0] : Expected.GetCurrentSeed());
    const int32 Gold = 500 + (OldIncome ? 1 : 0) + (Case.StartsWith(TEXT("Direct.Reward")) ? 7 : 0) - (Failure ? 0 : Offer.Price);
    TestEqual(TEXT("实时余额且保留旧收入与回调奖励"), F.P->GetGold(), Gold);
    if (Case != TEXT("Direct.SpendingLimit")) { TestEqual(TEXT("支出原子提交"), F.P->GetIncome()->Observe().State.TotalSpending, int64(Failure ? 0 : Offer.Price)); }
    const auto V = F.View(); TestEqual(TEXT("包变化只来自随机商品"), V.Items.Num(), Direct ? 0 : Offer.Quantity);
    for (const auto& Id : V.EquipmentSlots) { TestFalse(TEXT("不自动装配"), Id.IsValid()); }
    for (const auto& S : V.Skills) { TestFalse(TEXT("不自动学习"), S.LearnedId.IsValid()); }
    TSet<FGuid> Ids;
    for (int32 I = 0; I < V.Items.Num(); ++I)
    { Ids.Add(V.Items[I].ItemId); TestEqual(TEXT("逐份先品质再定义"), V.Items[I].Definition.RowName, ExpectedItems[I]); TestEqual(TEXT("稳定效果快照"), V.Items[I].Effects[0].Magnitude, 10.); }
    TestEqual(TEXT("每份独立身份"), Ids.Num(), V.Items.Num());
    if (Direct)
    {
        TestEqual(TEXT("真实攻击而非伪来源"), F.Attack(), BeforeSources.CurrentAttack + (!Failure && Case != TEXT("Direct.GrowthOnly") && Case != TEXT("Direct.GPSOnly") ? 17.f : 0.f));
        TestEqual(TEXT("收入速率共同提交"), F.P->GetIncome()->Observe().State.Rate, Before.IncomeRate + (!Failure && Case != TEXT("Direct.GrowthOnly") ? 4. : 0.));
        TestEqual(TEXT("不触发击杀成长"), F.ASC()->GetUnitStats().Strength, 0.f);
        if (Failure)
        {
            FY01SourceSnapshot AfterSources; Sources->Observe(AfterSources);
            TestEqual(TEXT("失败来源版本不变"), AfterSources.Revision, BeforeSources.Revision);
            TestEqual(TEXT("失败来源数量不变"), AfterSources.Effects.Num(), BeforeSources.Effects.Num());
            TestEqual(TEXT("失败无残留GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), BeforeHandles.Num());
            for (const auto Handle : BeforeHandles) { TestNotNull(TEXT("失败原GE仍存在"), F.ASC()->GetActiveGameplayEffect(Handle)); }
            if (Case == TEXT("Direct.DerivedGEFailure") || Case == TEXT("Direct.RewardRollback"))
            {
                TestEqual(TEXT("唯一派生GE只查询一次"), Applied, 1);
                TestTrue(TEXT("失败精确保留原句柄集合"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()) == BeforeHandles);
            }
            if (Case == TEXT("Direct.DerivedGEFailure")) { TestFalse(TEXT("DerivedGEFailure拒绝唯一候选不执行OnApplied"), Once); }
            if (Case == TEXT("Direct.RewardRollback"))
            {
                TestEqual(TEXT("奖励后候选GE丢失拒绝暂存"), R.Code, E::EffectRejected);
                TestTrue(TEXT("RewardRollback真实OnApplied已奖励"), Once && RewardGranted);
                TestTrue(TEXT("RewardRollback确实移除新候选句柄"), CandidateRemoved && CandidateHandle.IsValid());
                TestNull(TEXT("候选句柄不残留"), F.ASC()->GetActiveGameplayEffect(CandidateHandle));
                TestEqual(TEXT("RewardRollback有真实原汇总GE可恢复"), BeforeHandles.Num(), 1);
            }
        }
    }
    FY01CommittedView Full; F.L()->ReadCommittedView(Full);
    if (Case != TEXT("Direct.ViewLimit"))
    {
        TestEqual(TEXT("完整视图余额"), Full.Gold, Gold); TestEqual(TEXT("完整视图原回执"), Full.Response.Code, R.Code);
        TestEqual(TEXT("限购与同次视图"), ShopPurchased(Full, Ref.RowName), Direct && !Failure ? 1 : 0);
        if (Direct && Case != TEXT("Direct.GPSOnly"))
        {
            const auto& State = Full.ShopOffers[0]; TestEqual(TEXT("自有规则快照数量"), State.Growth.Num(), 1);
            if (State.Growth.Num() == 1) { TestEqual(TEXT("规则静态快照"), State.Growth[0].Delta, 3.); TestEqual(TEXT("尚未触发规则"), State.Growth[0].AppliedCount, uint64(0)); }
        }
        if (Case.StartsWith(TEXT("Direct.Wire")))
        {
            auto Bad = Full; auto& State = Bad.ShopOffers[0];
            if (Case == TEXT("Direct.WireDuplicate")) { const auto Duplicate = State; Bad.ShopOffers.Add(Duplicate); }
            if (Case == TEXT("Direct.WireKind")) { State.ShopKind = static_cast<EY01ShopKind>(255); }
            if (Case == TEXT("Direct.WireSource")) { State.SourceId.Reset(); }
            if (Case == TEXT("Direct.WireGrowth")) { State.Growth[0].Target = EY01GrowthTarget::EquipmentInstance; }
            if (Case == TEXT("Direct.WirePrice")) { State.Price = -1; }
            if (Case == TEXT("Direct.WireRandomPurchased")) { State.ShopKind = EY01ShopKind::RandomTier; }
            if (Case == TEXT("Direct.WireForeignSession")) { State.SourceId = TEXT("R36/Foreign/P1/Shop/default"); }
            if (Case == TEXT("Direct.WireNaN")) { State.Effects[0].Magnitude = std::numeric_limits<double>::quiet_NaN(); }
            FY01ViewSender BadSender; FY01ViewWire BadWire; FString BadError;
            TestFalse(TEXT("非法业务商店头不能进入生产编码"), BadSender.Offer(Bad, BadWire, BadError));
        }
        FY01ViewSender Sender; FY01ViewReceiver Receiver; FY01ViewWire Wire; FString Error;
        for (const auto* View : {&Before, &Full})
        {
            if (!TestTrue(TEXT("真实视图编码"), Sender.Offer(*View, Wire, Error))) { AddInfo(Error); return false; }
            int32 Windows = 0; while (!Wire.Items.IsEmpty() && ++Windows < 100)
            { FY01ViewAck Ack; if (!TestTrue(TEXT("完整窗口接收"), Receiver.Receive(Wire, Ack, Error))) { return false; } Sender.Acknowledge(Ack, Wire, Error); }
            TestTrue(TEXT("客户端收到同份购买状态而非拼接"), FY01CommittedView::StaticStruct()->CompareScriptStruct(View, &Receiver.Get(), 0));
        }
    }
    const auto Replay = Economy->TryPurchase(F.PC, Q); TestTrue(TEXT("原结果重放"), FY01ItemResult::StaticStruct()->CompareScriptStruct(&R, &Replay, 0));
    TestEqual(TEXT("重放不再扣款"), F.P->GetGold(), Gold); TestTrue(TEXT("重放不再抽样"), FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(Economy)) == AfterSeeds);
    if (Failure && (Case == TEXT("Direct.DerivedGEFailure") || Case == TEXT("Direct.Reentry") || Case == TEXT("Direct.RewardRollback")))
    {
        Q.Seq = F.View().ExpectedSeq; TestTrue(TEXT("技术失败未占用限购名额，新序列可重试"), Economy->TryPurchase(F.PC, Q).IsOk());
        F.L()->ReadCommittedView(Full); TestEqual(TEXT("重试成功同次限购"), ShopPurchased(Full, Ref.RowName), 1);
    }
    if (!Direct && !Failure && Gold >= Offer.Price)
    {
        Q.Seq = F.View().ExpectedSeq;
        const auto Again = Economy->TryPurchase(F.PC, Q);
        TestTrue(TEXT("幸运箱不限购，新序号可再次购买"), Again.IsOk());
        TestEqual(TEXT("再次购买再扣一次价格"), F.P->GetGold(), Gold - Offer.Price);
        TestEqual(TEXT("再次购买再入包同数量"), F.View().Items.Num(), Offer.Quantity * 2);
        FY01CommittedView AgainView; F.L()->ReadCommittedView(AgainView);
        TestEqual(TEXT("随机商品不写已购"), ShopPurchased(AgainView, Ref.RowName), 0);
    }
    if (!Direct || Failure) { return true; }
    if (Case == TEXT("Direct.Reinitialize")) { TestTrue(TEXT("重复初始化"), Economy->Initialize(F.Match, 1, 1, Catalog, {}).IsOk()); }
    if (Case == TEXT("Direct.Ring64"))
    { for (uint64 Seq = 2; Seq <= 65; ++Seq) { auto Bad = Q; Bad.Seq = Seq; Bad.Offer.RowName = TEXT("Absent"); Economy->TryPurchase(F.PC, Bad); } }
    Q.Seq = F.View().ExpectedSeq; Q.Offer.RowName = TEXT("DEFAULT");
    TestEqual(TEXT("换序号或大小写不能绕过每局一次"), Economy->TryPurchase(F.PC, Q).Code, E::PurchaseLimit); TestEqual(TEXT("限购不扣钱"), F.P->GetGold(), Gold);
    if (Case == TEXT("Direct.OtherOffer")) { Q.Seq = F.View().ExpectedSeq; Q.Offer.RowName = TEXT("OtherOffer"); TestTrue(TEXT("不同商品同效果可分别购买"), Economy->TryPurchase(F.PC, Q).IsOk()); TestEqual(TEXT("效果分别叠加"), F.Attack(), 64.f); }
    if (Case == TEXT("Direct.OtherPlayer"))
    {
        auto* PC = F.Player(); auto* P = PC->GetPlayerState<AY01PlayerState>(); P->SetOwner(PC); FString Error;
        TestTrue(TEXT("第二玩家角色"), P->TryInitializeCharacter(Catalog, FY01MatchFixture::Ref(K::Character), Error));
        FY01R36EconomyTestAccess::Clock(P->GetIncome(), Now); TestTrue(TEXT("第二玩家会话"), P->GetLoadout()->Initialize(F.Match, 2).IsOk());
        TestTrue(TEXT("第二玩家商品"), P->GetEconomy()->Initialize(F.Match, 2, 1, Catalog, {Ref}).IsOk()); P->TryAddMatchReward(500, 0);
        Q.JoinSequence = 2; Q.Seq = 1; TestTrue(TEXT("限购按玩家隔离"), P->GetEconomy()->TryPurchase(PC, Q).IsOk()); TestEqual(TEXT("独立钱包"), P->GetGold(), 450);
    }
    if (Case == TEXT("Direct.NoRevoke"))
    {
        FY01SourceSnapshot S; Sources->Observe(S); const auto* Found = S.Effects.FindByPredicate([](const auto& X) { return X.Source.Kind == EY01RuntimeSourceKind::Shop; });
        if (TestNotNull(TEXT("真实Shop来源"), Found))
        {
            TestTrue(TEXT("来源绑定本局本玩家商品身份"), Found->Source.Id.Contains(F.Match.ToString(EGuidFormats::DigitsWithHyphens)) && Found->Source.Id.Contains(TEXT("/P1/Shop/")));
            TestFalse(TEXT("已购来源不能单独撤销"), Sources->RemoveSource(Found->Source.Kind, Found->Source.LocalKey).IsOk());
            FY01PreparedMutation Plan; TestFalse(TEXT("批次也不能撤销已购来源"), Sources->PrepareMutation({}, {Found->Source}, Plan).IsOk());
            TestEqual(TEXT("没有退款或失效"), F.Attack(), 47.f);
        }
    }
    if (Case == TEXT("Direct.RateSwitch")) { Now = 1.5; TestTrue(TEXT("新速率继续结算"), F.P->GetIncome()->Settle().IsOk()); TestEqual(TEXT("旧余量与新速率不被覆盖"), F.P->GetGold(), 452); }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36EconomyAtomicTest, "Y01.Framework.R36.Economy.Atomic",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36EconomyAtomicTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : {TEXT("DerivedGEFailure"), TEXT("Reentry"), TEXT("Reward"), TEXT("RewardRollback"),
        TEXT("RateSwitch"), TEXT("RetireDead"), TEXT("RetireEnded"), TEXT("SecondDefinition"), TEXT("SecondIdentity"),
        TEXT("OwnedInputs"), TEXT("AuditChanged")}) { Names.Add(Name); Commands.Add(Name); }
}
bool FY01R36EconomyAtomicTest::RunTest(const FString& Case)
{
    using E = EY01ItemError; FLoadoutWorld F; F.P->SetOwner(F.PC); double Now = 0;
    FY01R36EconomyTestAccess::Clock(F.P->GetIncome(), Now); TestTrue(TEXT("初始化库存"), F.Init().IsOk());
    auto* Economy = F.P->GetEconomy(); const FY01DataCatalog Catalog(F.Data.Tables);
    TestTrue(TEXT("初始化经济"), Economy->Initialize(F.Match, 1, 1, Catalog, {}).IsOk());
    FY01R36EconomyTestAccess::Seeds(Economy); F.Mode->TryStartNextWave(); F.P->TryAddMatchReward(500, 0);
    auto A = F.Grant(), B = F.Grant(), C = F.Grant(), D = F.Grant(); F.Add({A, B});
    FY01R36EconomyTestAccess::Grow(F.L(), A.ItemId);
    auto* Sources = F.P->GetSourceEffects(); FY01PreparedMutation Plan; FY01SourceTransaction Tx;
    TestTrue(TEXT("原真实来源预备"), Sources->PrepareMutation({{EY01RuntimeSourceKind::Base, TEXT("Old"),
        {{TEXT("Attack"), EY01SourceEffectKind::AttackFlat, 5}, {TEXT("Rate"), EY01SourceEffectKind::GoldPerSecond, 1}}}}, {}, Plan).IsOk());
    TestTrue(TEXT("原真实GE暂存"), Sources->StageMutation(Plan, Tx).IsOk()); TestTrue(TEXT("原真实GE提交"), Sources->CommitMutation(Tx).IsOk());
    TestTrue(TEXT("启动旧收入"), F.P->GetIncome()->Start().IsOk()); Now = 1.25;
    FY01SourceSnapshot BeforeSources; Sources->Observe(BeforeSources);
    const auto Handles = F.ASC()->GetActiveEffects(FGameplayEffectQuery()); const auto Before = F.View();
    const auto Seeds = FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(Economy));
    FY01ItemRequest Q; Q.MatchId = F.Match; Q.JoinSequence = 1; Q.Seq = 1; Q.ConfigRevision = 1;
    Q.Operation = EY01ItemOperation::Purchase; Q.Materials = {{A.ItemId, 1}, {B.ItemId, 1}};
    const auto OriginalRequest = Q;
    TArray<FY01ItemGrant> Grants{C, D};
    TArray<FY01ResolvedSource> Added{{EY01RuntimeSourceKind::Base, TEXT("New"),
        {{TEXT("One"), EY01SourceEffectKind::AttackFlat, 7}, {TEXT("Two"), EY01SourceEffectKind::AttackFlat, 9},
         {TEXT("Rate"), EY01SourceEffectKind::GoldPerSecond, 4}}}};
    TArray<FY01RuntimeSourceId> Removed{BeforeSources.Effects[0].Source};
    if (Case == TEXT("SecondDefinition")) { Grants[1].Definition.RowName = TEXT("Absent"); }
    if (Case == TEXT("SecondIdentity")) { Grants[1].ItemId = Grants[0].ItemId; }
    int32 Applied = 0, Retired = 0; bool Once = false, RewardGranted = false, CandidateRemoved = false;
    FActiveGameplayEffectHandle CandidateHandle;
    F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda(
        [&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec& Spec)
        {
            if (!Spec.Def || !Spec.Def->IsA(UY01EquipmentAttackEffect::StaticClass())) { return true; }
            ++Applied; return !(Case == TEXT("DerivedGEFailure") && Applied == 1);
        }));
    const auto ApplyHandle = F.ASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
        [&](UAbilitySystemComponent*, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle)
        {
            if (Once || !Spec.Def || !Spec.Def->IsA(UY01EquipmentAttackEffect::StaticClass())) { return; } Once = true;
            FY01LoadoutSnapshot Hidden; TestEqual(TEXT("暂存不发布材料或货物"), F.L()->Observe(Hidden).Code, E::Busy);
            TestEqual(TEXT("暂存余额尚未扣款"), F.P->GetGold(), 501);
            if (Case == TEXT("Reentry")) { TestEqual(TEXT("真实购买重入Busy"), Economy->TryPurchase(F.PC, OriginalRequest).Code, E::Busy); }
            if (Case == TEXT("Reward") || Case == TEXT("RewardRollback"))
            { RewardGranted = F.P->TryAddMatchReward(7, 0); TestTrue(TEXT("合法奖励仍可入账"), RewardGranted); }
            if (Case == TEXT("RewardRollback"))
            {
                CandidateHandle = Handle;
                TestTrue(TEXT("回调参数为新候选句柄"), Handle.IsValid() && !Handles.Contains(Handle));
                CandidateRemoved = F.ASC()->RemoveActiveGameplayEffect(Handle);
                TestTrue(TEXT("奖励之后真实移除候选GE"), CandidateRemoved);
            }
            if (Case == TEXT("AuditChanged")) { FY01R36EconomyTestAccess::Spending(F.P->GetIncome(), true); }
            if (Case == TEXT("OwnedInputs"))
            { Q.Materials.Reset(); Q.Seq = 44; Grants.Reset(); Added.Reset(); Removed.Reset(); F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, 999); }
        });
    const auto RemoveHandle = F.ASC()->OnAnyGameplayEffectRemovedDelegate().AddLambda([&](const FActiveGameplayEffect&)
    {
        if (++Retired != 1) { return; }
        if (Case == TEXT("RetireDead") || Case == TEXT("RetireEnded"))
        {
            TestEqual(TEXT("退休通知已扣款"), F.P->GetGold(), 401);
            if (Case == TEXT("RetireDead")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
            else { F.Mode->TryEndMatch(false); Economy->EndPlay(EEndPlayReason::Quit); }
        }
    });
    const auto Result = FY01R36EconomyTestAccess::Atomic(F.L(), F.PC, Catalog, Q, Grants, Added, Removed, Now);
    F.ASC()->GameplayEffectApplicationQueries.Reset(); F.ASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(ApplyHandle);
    F.ASC()->OnAnyGameplayEffectRemovedDelegate().Remove(RemoveHandle);
    const bool Failure = Case == TEXT("DerivedGEFailure") || Case == TEXT("Reentry") || Case == TEXT("RewardRollback")
        || Case == TEXT("SecondDefinition") || Case == TEXT("SecondIdentity") || Case == TEXT("AuditChanged");
    TestEqual(TEXT("共同事务结果"), Result.IsOk(), !Failure);
    const auto After = F.View();
    TestEqual(TEXT("单个原请求账本"), After.ExpectedSeq, uint64(2));
    TestEqual(TEXT("旧收入合法保留及按实际余额扣款"), F.P->GetGold(), (Failure ? 501 : 401) + (Case.StartsWith(TEXT("Reward")) ? 7 : 0));
    const auto AfterSeeds = FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(Economy));
    TestEqual(TEXT("合成流未动"), AfterSeeds[1], Seeds[1]); TestEqual(TEXT("掉落流未动"), AfterSeeds[2], Seeds[2]);
    if (Failure)
    {
        TestEqual(TEXT("库存版本精确保持"), After.Revision, Before.Revision);
        TestEqual(TEXT("材料数量保持"), After.Items.Num(), Before.Items.Num());
        for (int32 Index = 0; Index < Before.Items.Num() && Index < After.Items.Num(); ++Index)
        { TestTrue(TEXT("材料身份位置成长完整保持"), FY01ItemInstance::StaticStruct()->CompareScriptStruct(&Before.Items[Index], &After.Items[Index], 0)); }
        TestEqual(TEXT("失败购物流原样"), AfterSeeds[0], Seeds[0]);
        TestEqual(TEXT("不记支出"), F.P->GetIncome()->Observe().State.TotalSpending, int64(0));
        FY01SourceSnapshot AfterSources; Sources->Observe(AfterSources);
        TestEqual(TEXT("来源版本保持"), AfterSources.Revision, BeforeSources.Revision);
        TestEqual(TEXT("攻击精确回滚"), AfterSources.CurrentAttack, BeforeSources.CurrentAttack);
        TestEqual(TEXT("base保持"), AfterSources.BaseAttack, BeforeSources.BaseAttack);
        TestEqual(TEXT("无残留GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), Handles.Num());
        for (auto Handle : Handles) { TestNotNull(TEXT("保留原GE身份"), F.ASC()->GetActiveGameplayEffect(Handle)); }
        if (Case == TEXT("DerivedGEFailure") || Case == TEXT("RewardRollback"))
        {
            TestEqual(TEXT("唯一派生GE只查询一次"), Applied, 1);
            TestTrue(TEXT("失败精确保留原句柄集合"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()) == Handles);
        }
        if (Case == TEXT("DerivedGEFailure")) { TestFalse(TEXT("DerivedGEFailure拒绝唯一候选不执行OnApplied"), Once); }
        if (Case == TEXT("RewardRollback"))
        {
            TestEqual(TEXT("奖励后候选GE丢失拒绝暂存"), Result.Code, E::EffectRejected);
            TestTrue(TEXT("RewardRollback真实OnApplied已奖励"), Once && RewardGranted);
            TestTrue(TEXT("RewardRollback确实移除新候选句柄"), CandidateRemoved && CandidateHandle.IsValid());
            TestNull(TEXT("候选句柄不残留"), F.ASC()->GetActiveGameplayEffect(CandidateHandle));
            TestEqual(TEXT("RewardRollback有真实原汇总GE可恢复"), Handles.Num(), 1);
        }
    }
    else
    {
        TestEqual(TEXT("批次只推进一次版本"), After.Revision, Before.Revision + 1);
        TestEqual(TEXT("一批两材料两产物"), Result.Changes.Num(), 4);
        TestFalse(TEXT("消费材料不残留"), After.Items.ContainsByPredicate([&](const auto& Item) { return Item.ItemId == A.ItemId || Item.ItemId == B.ItemId; }));
        TestEqual(TEXT("产物成长从零"), F.Item(C.ItemId).Growth[0].Accumulated, 0.);
        FRandomStream Expected(123); Expected.GetFraction(); TestEqual(TEXT("成功推进购物副本"), AfterSeeds[0], Expected.GetCurrentSeed());
        TestEqual(TEXT("支出只记一次"), F.P->GetIncome()->Observe().State.TotalSpending, int64(100));
        if (Case == TEXT("RateSwitch"))
        {
            const auto I = F.P->GetIncome()->Observe(); TestEqual(TEXT("新速率不被支出快照覆盖"), I.State.Rate, 4.);
            TestEqual(TEXT("新段起点保留"), I.State.StartTime, Now); TestEqual(TEXT("旧余量保留"), I.State.Remainder, .25);
            TestEqual(TEXT("支出审计新速率"), I.State.Recent.Last().Rate, 4.);
            Now = 1.5; TestTrue(TEXT("新段继续收入"), F.P->GetIncome()->Settle().IsOk()); TestEqual(TEXT("新段到账"), F.P->GetGold(), 402);
        }
    }
    const auto Replay = Economy->TryPurchase(F.PC, OriginalRequest);
    TestTrue(TEXT("原共同结果精确重放"), FY01ItemResult::StaticStruct()->CompareScriptStruct(&Result, &Replay, 0));
    auto Conflict = OriginalRequest; ++Conflict.Materials[0].ExpectedEntityRevision;
    TestEqual(TEXT("同号材料版本意图不同"), Economy->TryPurchase(F.PC, Conflict).Code, E::PayloadConflict);
    Conflict = OriginalRequest; Conflict.Materials[0].ItemId = FGuid::NewGuid();
    TestEqual(TEXT("同号材料身份意图不同"), Economy->TryPurchase(F.PC, Conflict).Code, E::PayloadConflict);
    return true;
}

namespace
{
// 缺少新入口时仍编译并调用旧生产入口，红必须是业务断言而不是编译失败。
template<class T> FY01ItemResult InitCraftTalent(T* Economy, const FGuid& Match, const FY01DataCatalog& Catalog,
    const TArray<FY01DataRef>& Recipes, const FY01DataRef& Choice)
{
    if constexpr (requires { Economy->Initialize(Match, 1, 1, Catalog, TArray<FY01DataRef>(), Recipes, Choice); })
    { return Economy->Initialize(Match, 1, 1, Catalog, {}, Recipes, Choice); }
    else { return Economy->Initialize(Match, 1, 1, Catalog, {}); }
}
void IntentRef(FY01ItemRequest& Q, const TCHAR* Field, const FY01DataRef& Value)
{
    const auto* P = FindFProperty<FStructProperty>(FY01ItemRequest::StaticStruct(), Field);
    if (P && P->Struct == FY01DataRef::StaticStruct()) { *P->ContainerPtrToValuePtr<FY01DataRef>(&Q) = Value; }
}
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36CraftTalentEntryTest, "Y01.Framework.R36.Economy.CraftTalent.Entry",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36CraftTalentEntryTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("FreeSynthesis"), TEXT("StableTalentCandidates"), TEXT("IntentContract")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36CraftTalentEntryTest::RunTest(const FString& Case)
{
    FLoadoutWorld F; F.P->SetOwner(F.PC); double Now = 0;
    FY01R36EconomyTestAccess::Clock(F.P->GetIncome(), Now);
    if (!TestTrue(TEXT("真实库存会话"), F.Init().IsOk())) { return false; }
    if (Case == TEXT("IntentContract"))
    {
        for (const TCHAR* Field : {TEXT("Recipe"), TEXT("Talent"), TEXT("TalentChoiceId")})
        { TestNotNull(TEXT("请求扩展只有服务器公开身份意图"), FindFProperty<FStructProperty>(FY01ItemRequest::StaticStruct(), Field)); }
        TestEqual(TEXT("保留旧枚举并追加合成"), StaticEnum<EY01ItemOperation>()->GetValueByNameString(TEXT("Synthesize")), int64(7));
        TestEqual(TEXT("追加天赋选择"), StaticEnum<EY01ItemOperation>()->GetValueByNameString(TEXT("ChooseTalent")), int64(8));
        auto Fields = [](UScriptStruct* Type) { TSet<FName> Values; for (TFieldIterator<FProperty> It(Type); It; ++It) { Values.Add(It->GetFName()); } return Values; };
        const auto RecipeFields = Fields(FY01SynthesisRow::StaticStruct()); TestEqual(TEXT("配方含入口字段且保留四原字段"), RecipeFields.Num(), 6);
        for (const TCHAR* Name : {TEXT("InputType"), TEXT("InputQuality"), TEXT("InputCount"), TEXT("OutputPool")}) { TestTrue(TEXT("原免费配方契约"), RecipeFields.Contains(Name)); }
        TestTrue(TEXT("启用开关字段"), RecipeFields.Contains(TEXT("Enabled"))); TestTrue(TEXT("排序字段"), RecipeFields.Contains(TEXT("SortOrder")));
        TestEqual(TEXT("候选只有显示身份效果成长，无GA"), Fields(FY01TalentCandidate::StaticStruct()).Num(), 5);
        TestEqual(TEXT("机会状态无刷新收费字段"), Fields(FY01TalentChoiceState::StaticStruct()).Num(), 6);
        return true;
    }
    const auto Recipe = FY01MatchFixture::Ref(K::Synthesis), Choice = FY01MatchFixture::Ref(K::TalentChoice);
    auto& Output = F.Data.Row<FY01EquipmentRow>(K::Equipment, TEXT("Blue"));
    Output.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten"))}; Output.DisplayName = FText::FromString(TEXT("蓝产物"));
    auto& ChoiceRow = F.Data.Row<FY01TalentChoiceRow>(K::TalentChoice); ChoiceRow.Candidates.Reset();
    for (const TCHAR* Name : {TEXT("Attack"), TEXT("Income"), TEXT("Strength")})
    {
        FY01TalentRow Row; Row.DisplayName = FText::FromString(Name); Row.EffectIds = Output.EffectIds;
        F.Data.Add(K::Talent, Row, Name); ChoiceRow.Candidates.Add(FY01MatchFixture::Ref(K::Talent, Name));
    }
    TestTrue(TEXT("可信配置初始化"), InitCraftTalent(F.P->GetEconomy(), F.Match, FY01DataCatalog(F.Data.Tables), {Recipe}, Choice).IsOk());
    if (Case == TEXT("StableTalentCandidates"))
    {
        FY01CommittedView View; TestTrue(TEXT("实际生产完整视图"), F.L()->ReadCommittedView(View));
        const auto* State = FindFProperty<FStructProperty>(FY01CommittedView::StaticStruct(), TEXT("TalentChoice"));
        if (!TestNotNull(TEXT("完整业务视图必须包含稳定三候选状态"), State)) { return false; }
        const auto* Candidates = FindFProperty<FArrayProperty>(State->Struct, TEXT("Candidates"));
        if (!TestNotNull(TEXT("三候选数组"), Candidates)) { return false; }
        FScriptArrayHelper Values(Candidates, Candidates->ContainerPtrToValuePtr<void>(State->ContainerPtrToValuePtr<void>(&View)));
        TestEqual(TEXT("服务器已经生成三个候选"), Values.Num(), 3);
        TestEqual(TEXT("未选不授予"), F.Attack(), 30.f); return true;
    }
    F.Mode->TryStartNextWave(); TArray<FY01ItemGrant> Grants;
    for (int32 I = 0; I < 5; ++I) { Grants.Add(F.Grant()); }
    TestTrue(TEXT("真实五材料入包"), F.Add(Grants).IsOk());
    FY01ItemRequest Q; Q.MatchId = F.Match; Q.JoinSequence = 1; Q.Seq = 1; Q.ConfigRevision = 1;
    Q.Operation = static_cast<EY01ItemOperation>(7); IntentRef(Q, TEXT("Recipe"), Recipe);
    for (const auto& Grant : Grants) { Q.Materials.Add({Grant.ItemId, 1}); }
    TestEqual(TEXT("零金币实际五合一成功"), F.L()->SubmitRequest(F.PC, Q).Code, EY01ItemError::None);
    TestEqual(TEXT("背包五进一净减四"), F.View().Items.Num(), 1);
    TestEqual(TEXT("永久免费"), F.P->GetGold(), 0);
    return true;
}

namespace
{
TArray<FY01DataRef> CraftTalentData(FLoadoutWorld& F)
{
    TArray<FY01DataRef> Recipes;
    FY01GrowthRuleRow Growth; Growth.Target = EY01GrowthTarget::Player; Growth.Attribute = EY01GrowthAttribute::Strength; Growth.Delta = 3;
    F.Data.Add(K::GrowthRule, Growth, TEXT("Permanent")); F.Effect(TEXT("Income"), EY01SourceEffectKind::GoldPerSecond, 2);
    for (K Type : {K::Equipment, K::Skill})
    {
        for (int32 Quality = 0; Quality < 5; ++Quality)
        {
            FY01LocalItemPoolRow Pool; Pool.ItemType = Type; Pool.Quality = static_cast<EY01Quality>(Quality);
            for (int32 I = 0; I < 5; ++I)
            {
                const FName Id(*FString::Printf(TEXT("Item_%d_%d_%d"), int32(Type), Quality, I));
                if (Type == K::Equipment)
                { F.Equipment(Id, {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten"))}); auto& Row = F.Data.Row<FY01EquipmentRow>(Type, Id);
                    Row.Quality = Pool.Quality; Row.DisplayName = FText::FromName(Id); Row.GrowthRuleIds = {FY01MatchFixture::Ref(K::GrowthRule)}; }
                else
                { F.Book(Id); auto& Row = F.Data.Row<FY01SkillRow>(Type, Id); Row.Quality = Pool.Quality; Row.DisplayName = FText::FromName(Id);
                    Row.GrowthRuleIds = {FY01MatchFixture::Ref(K::GrowthRule, TEXT("Permanent"))}; }
                FY01WeightedEntry Entry; Entry.Item = FY01MatchFixture::Ref(Type, Id); Entry.Weight = I == 4 ? 0 : 1; Pool.Entries.Add(Entry);
            }
            const FName PoolId(*FString::Printf(TEXT("Pool_%d_%d"), int32(Type), Quality)); F.Data.Add(K::LocalItemPool, Pool, PoolId);
            if (Quality > 0)
            {
                FY01SynthesisRow Row; Row.InputType = Type; Row.InputQuality = static_cast<EY01Quality>(Quality - 1);
                Row.OutputPool = FY01MatchFixture::Ref(K::LocalItemPool, PoolId);
                const FName Id(*FString::Printf(TEXT("Recipe_%d_%d"), int32(Type), Quality - 1)); F.Data.Add(K::Synthesis, Row, Id);
                Recipes.Add(FY01MatchFixture::Ref(K::Synthesis, Id));
            }
        }
    }
    FY01TalentChoiceRow Choice;
    for (int32 I = 0; I < 3; ++I)
    {
        const FName Id(*FString::Printf(TEXT("Talent_%d"), I)); FY01TalentRow Row; Row.DisplayName = FText::FromName(Id);
        if (I == 0) { Row.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten")), FY01MatchFixture::Ref(K::SourceEffect, TEXT("Other")), FY01MatchFixture::Ref(K::SourceEffect, TEXT("Income"))}; }
        if (I == 1) { Row.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Income"))}; }
        Row.GrowthRuleIds = {FY01MatchFixture::Ref(K::GrowthRule, TEXT("Permanent"))};
        F.Data.Add(K::Talent, Row, Id); Choice.Candidates.Add(FY01MatchFixture::Ref(K::Talent, Id));
    }
    F.Data.Add(K::TalentChoice, Choice); return Recipes;
}
FY01CommittedView CraftFull(FLoadoutWorld& F) { FY01CommittedView V; F.L()->ReadCommittedView(V); return V; }
bool CraftTransfer(FAutomationTestBase& Test, const FY01CommittedView& Before, const FY01CommittedView& After)
{
    FY01ViewSender Sender; FY01ViewReceiver Receiver; FY01ViewWire Wire; FString Error;
    for (const auto* V : {&Before, &After})
    {
        if (!Test.TestTrue(TEXT("生产完整编码"), Sender.Offer(*V, Wire, Error))) { Test.AddInfo(Error); return false; }
        int32 Windows = 0;
        while (!Wire.Items.IsEmpty() && ++Windows < 1000)
        {
            const auto Previous = Receiver.Get().ViewRevision; FY01ViewAck Ack;
            if (!Test.TestTrue(TEXT("生产完整接收"), Receiver.Receive(Wire, Ack, Error))) { Test.AddInfo(Error); return false; }
            Test.TestTrue(TEXT("不会暴露半版"), Receiver.Get().ViewRevision == Previous || Receiver.Get().ViewRevision == V->ViewRevision);
            if (!Sender.Acknowledge(Ack, Wire, Error)) { return false; }
        }
        if (!Test.TestTrue(TEXT("解码与服务端同一完整快照"), FY01CommittedView::StaticStruct()->CompareScriptStruct(V, &Receiver.Get(), 0))) { return false; }
    }
    return true;
}
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36SynthesisBusinessTest, "Y01.Framework.R36.Economy.CraftTalent.Synthesis",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36SynthesisBusinessTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Type : {TEXT("Equipment"), TEXT("Skill")}) { for (int32 I = 0; I < 4; ++I)
        { const auto N = FString::Printf(TEXT("%s.%d"), Type, I); Names.Add(N); Commands.Add(N); } }
    for (const TCHAR* N : {TEXT("Duplicate"), TEXT("Four"), TEXT("Six"), TEXT("MixedType"), TEXT("MixedQuality"), TEXT("Foreign"),
        TEXT("Stale"), TEXT("MissingRecipe"), TEXT("WrongRecipeType"), TEXT("Equipped"), TEXT("Unequipped"), TEXT("Learned"),
        TEXT("RevisionLimit"), TEXT("AcquisitionLimit"), TEXT("EntityLimit"), TEXT("ViewLimit"), TEXT("SpendingLimit"), TEXT("AuditLimit"),
        TEXT("ConfigChanged"), TEXT("Ended"), TEXT("Waiting"), TEXT("Dead"), TEXT("Battle"), TEXT("Snapshot"), TEXT("BusyClock"),
        TEXT("ClockEnded"), TEXT("ClockConfig"), TEXT("OldIncome"), TEXT("BusySource"), TEXT("LostGE"), TEXT("OtherPlayer"), TEXT("NoCapacity500")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36SynthesisBusinessTest::RunTest(const FString& Case)
{
    using E = EY01ItemError; FLoadoutWorld F; F.P->SetOwner(F.PC); double Now = 0;
    FY01R36EconomyTestAccess::Clock(F.P->GetIncome(), Now); TestTrue(TEXT("初始化库存"), F.Init().IsOk());
    const auto Recipes = CraftTalentData(F); const FY01DataCatalog Catalog(F.Data.Tables); auto* Economy = F.P->GetEconomy();
    const K Type = Case.StartsWith(TEXT("Skill")) || Case == TEXT("Learned") ? K::Skill : K::Equipment;
    const int32 Quality = Case.Contains(TEXT(".")) ? FCString::Atoi(*Case.Right(1)) : 0;
    if (!TestTrue(TEXT("真实配方快照"), Economy->Initialize(F.Match, 1, 1, Catalog, {}, Recipes).IsOk())) { return false; }
    FY01R36EconomyTestAccess::Seeds(Economy);
    if (Case != TEXT("Waiting")) { F.Mode->TryStartNextWave(); }
    if (Case == TEXT("Battle")) { for (int32 I = 0; I < 23; ++I) { ++GFrameCounter; F.World->Tick(LEVELTICK_All, .05f); } }
    TArray<FY01ItemGrant> Grants;
    for (int32 I = 0; I < 5; ++I)
    { Grants.Add(F.Grant(FName(*FString::Printf(TEXT("Item_%d_%d_%d"), int32(Type), Quality, I)), Type)); }
    if (Case == TEXT("MixedType")) { Grants[4] = F.Grant(TEXT("Item_5_0_4"), K::Skill); }
    if (Case == TEXT("MixedQuality")) { Grants[4] = F.Grant(TEXT("Item_7_1_4")); }
    TestTrue(TEXT("不同名称五件入包"), F.Add(Grants).IsOk());
    FY01ItemRequest Q; Q.MatchId = F.Match; Q.JoinSequence = 1; Q.ConfigRevision = 1; Q.Seq = 1;
    Q.Operation = EY01ItemOperation::Synthesize; Q.Recipe = FY01MatchFixture::Ref(K::Synthesis, FName(*FString::Printf(TEXT("Recipe_%d_%d"), int32(Type), Quality)));
    for (const auto& G : Grants) { Q.Materials.Add({G.ItemId, 1}); }
    if (Case == TEXT("Equipped") || Case == TEXT("Unequipped"))
    {
        TestTrue(TEXT("先穿装备"), F.Send(F.Request(Grants[0].ItemId, 0)).IsOk());
        if (Case == TEXT("Unequipped")) { TestTrue(TEXT("先卸才能合成"), F.Send(F.Request(Grants[0].ItemId, 0, EY01ItemOperation::Unequip)).IsOk()); }
        Q.Materials[0].ExpectedEntityRevision = F.Item(Grants[0].ItemId).EntityRevision;
    }
    if (Case == TEXT("Learned"))
    { TestTrue(TEXT("先真实耗书学习"), F.L()->PrepareLearn(F.PC, F.LearnRequest(Grants[0].ItemId, 0)).IsOk()); Q.Materials[0].ItemId = F.View().Skills[0].LearnedId; }
    if (Case == TEXT("NoCapacity500"))
    { TArray<FY01ItemGrant> Extra; for (int32 I = 0; I < 501; ++I) { Extra.Add(F.Grant()); } TestTrue(TEXT("不设500硬容量"), F.Add(Extra).IsOk()); }
    if (Type == K::Equipment) { FY01R36EconomyTestAccess::Grow(F.L(), Grants[1].ItemId); }
    // 通过公开来源事务建立材料之外的力量，不能直接改派生快照或访问私有成长账本。
    FY01PreparedMutation StrengthPlan; FY01SourceTransaction StrengthTx;
    auto* StrengthSources = F.P->GetSourceEffects();
    const FY01ResolvedSource StrengthSource{EY01RuntimeSourceKind::Base, TEXT("RetainedStrength"),
        {{TEXT("RetainedStrength"), EY01SourceEffectKind::AttributeFlat, 0, EY01Attribute::None, EY01Attribute::Strength, 25}}};
    if (!TestTrue(TEXT("材料外力量真实来源准备"), StrengthSources->PrepareMutation({StrengthSource}, {}, StrengthPlan).IsOk())) { return false; }
    if (!TestTrue(TEXT("材料外力量真实来源暂存"), StrengthSources->StageMutation(StrengthPlan, StrengthTx).IsOk())) { return false; }
    if (!TestTrue(TEXT("材料外力量真实来源提交"), StrengthSources->CommitMutation(StrengthTx).IsOk())) { return false; }
    TestEqual(TEXT("合成前已有材料外力量"), F.ASC()->GetUnitStats().Strength, 25.f);
    E Expected = E::None;
    if (Case == TEXT("Duplicate")) { Q.Materials[4] = Q.Materials[0]; Expected = E::DuplicateItem; }
    if (Case == TEXT("Four")) { Q.Materials.Pop(); Expected = E::InvalidDefinition; }
    if (Case == TEXT("Six")) { const auto Copy = Q.Materials[0]; Q.Materials.Add(Copy); Expected = E::InvalidDefinition; }
    if (Case == TEXT("MixedType") || Case == TEXT("MixedQuality")) { Expected = E::InvalidDefinition; }
    if (Case == TEXT("Stale")) { ++Q.Materials[0].ExpectedEntityRevision; Expected = E::StaleEntity; }
    if (Case == TEXT("MissingRecipe")) { Q.Recipe.RowName = TEXT("Missing"); Expected = E::InvalidDefinition; }
    if (Case == TEXT("WrongRecipeType")) { Q.Recipe.Type = K::ShopOffer; Expected = E::InvalidDefinition; }
    if (Case == TEXT("Equipped")) { Expected = E::InvalidLocation; }
    if (Case == TEXT("Learned")) { Expected = E::MissingItem; }
    if (Case == TEXT("ConfigChanged")) { F.L()->SetConfigRevision(2); Expected = E::StaleConfig; }
    if (Case == TEXT("Ended")) { F.Mode->TryEndMatch(false); Expected = E::Phase; }
    if (Case == TEXT("Waiting")) { Expected = E::Phase; }
    if (Case == TEXT("Dead")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); Expected = E::Dead; }
    if (Case == TEXT("RevisionLimit") || Case == TEXT("AcquisitionLimit") || Case == TEXT("EntityLimit") || Case == TEXT("ViewLimit"))
    { FY01R36EconomyTestAccess::Limit(F.L(), Case == TEXT("RevisionLimit") ? 0 : Case == TEXT("AcquisitionLimit") ? 1 : Case == TEXT("EntityLimit") ? 2 : 3);
        if (Case == TEXT("EntityLimit")) { for (auto& M : Q.Materials) { M.ExpectedEntityRevision = MAX_uint64; } } Expected = E::Range; }
    if (Case == TEXT("SpendingLimit") || Case == TEXT("AuditLimit")) { FY01R36EconomyTestAccess::Spending(F.P->GetIncome(), Case == TEXT("AuditLimit")); }
    if (Case == TEXT("Snapshot"))
    { F.Data.Row<FY01SynthesisRow>(K::Synthesis, Q.Recipe.RowName).InputCount = 99; F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, 999); }
    auto* Sources = F.P->GetSourceEffects();
    if (Case == TEXT("OldIncome") || Case == TEXT("LostGE"))
    {
        FY01PreparedMutation Plan; FY01SourceTransaction Tx;
        TestTrue(TEXT("基础来源准备"), Sources->PrepareMutation({{EY01RuntimeSourceKind::Base, TEXT("Base"), {{TEXT("BaseAttack"), EY01SourceEffectKind::AttackFlat, 5}, {TEXT("Rate"), EY01SourceEffectKind::GoldPerSecond, 1}}}}, {}, Plan).IsOk());
        TestTrue(TEXT("基础来源暂存"), Sources->StageMutation(Plan, Tx).IsOk()); TestTrue(TEXT("基础来源提交"), Sources->CommitMutation(Tx).IsOk());
        if (Case == TEXT("OldIncome")) { F.P->GetIncome()->Start(); Now = 1.25; }
        else { F.ASC()->RemoveActiveGameplayEffect(F.ASC()->GetActiveEffects(FGameplayEffectQuery())[0]); Expected = E::HandleLost; }
    }
    if (Case == TEXT("Foreign") || Case == TEXT("OtherPlayer"))
    {
        auto* PC = F.Player(); auto* P = PC->GetPlayerState<AY01PlayerState>(); P->SetOwner(PC); FString Error;
        TestTrue(TEXT("另一玩家角色"), P->TryInitializeCharacter(Catalog, FY01MatchFixture::Ref(K::Character), Error));
        TestTrue(TEXT("另一玩家会话"), P->GetLoadout()->Initialize(F.Match, 2).IsOk());
        auto Foreign = F.Grant(); TestTrue(TEXT("另一玩家背包"), P->GetLoadout()->AddItems(Catalog, {Foreign}).IsOk());
        if (Case == TEXT("Foreign")) { Q.Materials[0].ItemId = Foreign.ItemId; Expected = E::MissingItem; }
        TestEqual(TEXT("不能借他人Controller合成"), Economy->TrySynthesize(PC, Q).Code, E::Authority);
    }
    Q.Seq = F.View().ExpectedSeq; const auto Original = Q;
    const auto Before = F.View(); F.L()->PublishCommittedView(); const auto FullBefore = CraftFull(F);
    const auto Seeds = FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(Economy));
    const auto Spending = F.P->GetIncome()->Observe().State.TotalSpending;
    const auto Handles = F.ASC()->GetActiveEffects(FGameplayEffectQuery());
    if (Case == TEXT("BusySource"))
    {
        FY01PreparedMutation Plan; FY01SourceTransaction Tx; Sources->PrepareMutation({}, {}, Plan); Sources->StageMutation(Plan, Tx);
        TestEqual(TEXT("来源忙不耗材料"), Economy->TrySynthesize(F.PC, Q).Code, E::Busy); Sources->RollbackMutation(Tx);
        TestEqual(TEXT("忙不耗序号"), F.View().ExpectedSeq, Q.Seq);
    }
    bool Once = false;
    if (Case.StartsWith(TEXT("Clock")) || Case == TEXT("BusyClock"))
    {
        FY01R36EconomyTestAccess::ReentryClock(F.P->GetIncome(), [&]
        { if (!Once) { Once = true;
            if (Case == TEXT("BusyClock")) { TestEqual(TEXT("共享锁重入"), Economy->TrySynthesize(F.PC, Q).Code, E::Busy); }
            if (Case == TEXT("ClockEnded")) { F.Mode->TryEndMatch(false); }
            if (Case == TEXT("ClockConfig")) { FY01R36EconomyTestAccess::Config(F.L()); }
        } return Now; });
        Expected = Case == TEXT("ClockConfig") ? E::StaleConfig : Case == TEXT("ClockEnded") ? E::Phase : E::Interrupted;
    }
    const auto R = Economy->TrySynthesize(F.PC, Q); FY01R36EconomyTestAccess::Clock(F.P->GetIncome(), Now);
    TestEqual(TEXT("真实合成业务结果"), R.Code, Expected); if (!R.IsOk()) { AddInfo(R.Message); }
    const auto After = F.View(); const auto AfterSeeds = FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(Economy));
    TestEqual(TEXT("购物流不动"), AfterSeeds[0], Seeds[0]); TestEqual(TEXT("掉落流不动"), AfterSeeds[2], Seeds[2]);
    TestEqual(TEXT("支出完全不动"), F.P->GetIncome()->Observe().State.TotalSpending, Spending);
    TestEqual(TEXT("金币仅计合法旧段"), F.P->GetGold(), Case == TEXT("OldIncome") ? 1 : 0);
    TestEqual(TEXT("材料外力量不追回"), F.ASC()->GetUnitStats().Strength, 25.f);
    if (Expected != E::None)
    {
        TestEqual(TEXT("失败合成流不动"), AfterSeeds[1], Seeds[1]); TestEqual(TEXT("失败材料数量不变"), After.Items.Num(), Before.Items.Num());
        for (int32 I = 0; I < Before.Items.Num() && I < After.Items.Num(); ++I)
        { TestTrue(TEXT("失败材料身份版本成长精确恢复"), FY01ItemInstance::StaticStruct()->CompareScriptStruct(&Before.Items[I], &After.Items[I], 0)); }
    }
    else if (R.IsOk())
    {
        TestEqual(TEXT("包净减4"), After.Items.Num(), Before.Items.Num() - 4); TestEqual(TEXT("一次提交一个版本"), After.Revision, Before.Revision + 1);
        TestEqual(TEXT("五删除加一新增"), R.Changes.Num(), 6); TestTrue(TEXT("服务器新身份"), R.ProductItemId.IsValid());
        const auto Product = F.Item(R.ProductItemId); TestEqual(TEXT("同类高一级"), int32(Product.Quality), Quality + 1);
        TestEqual(TEXT("不自动装配"), Product.Location, EY01ItemLocation::Backpack); TestEqual(TEXT("产物效果快照"), Product.Effects[0].Magnitude, 10.);
        for (const auto& G : Product.Growth) { TestEqual(TEXT("成长次数归零"), G.AppliedCount, uint64(0)); TestEqual(TEXT("成长累计归零"), G.Accumulated, 0.); }
        FRandomStream Random(456); Random.GetFraction(); TestEqual(TEXT("仅成功推进合成一次"), AfterSeeds[1], Random.GetCurrentSeed());
        auto Stale = Original; Stale.Seq = After.ExpectedSeq;
        TestEqual(TEXT("另号重复使用已消耗材料拒绝"), Economy->TrySynthesize(F.PC, Stale).Code, E::MissingItem);
    }
    for (auto Handle : Handles) { TestNotNull(TEXT("原GE身份保留"), F.ASC()->GetActiveGameplayEffect(Handle)); }
    const auto Replay = Economy->TrySynthesize(F.PC, Original); TestTrue(TEXT("原回执精确重放"), FY01ItemResult::StaticStruct()->CompareScriptStruct(&R, &Replay, 0));
    if (Case != TEXT("LostGE") && Case != TEXT("ViewLimit")) { CraftTransfer(*this, FullBefore, CraftFull(F)); }
    auto Conflict = Original; Conflict.Recipe.RowName = TEXT("Other"); TestEqual(TEXT("配方参与同号防重"), Economy->TrySynthesize(F.PC, Conflict).Code, E::PayloadConflict);
    Conflict = Original; Conflict.Recipe.Type = Original.Recipe.Type == K::ShopOffer ? K::Synthesis : K::ShopOffer;
    TestEqual(TEXT("配方类型也参与同号防重"), Economy->TrySynthesize(F.PC, Conflict).Code, E::PayloadConflict);
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36TalentBusinessTest, "Y01.Framework.R36.Economy.CraftTalent.Talent",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36TalentBusinessTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Choose"), TEXT("Income"), TEXT("Growth"), TEXT("Snapshot"), TEXT("Reinitialize"), TEXT("NoRevoke"), TEXT("OtherPlayer"),
        TEXT("ForeignChoice"), TEXT("NotCandidate"), TEXT("WrongType"), TEXT("Materials"), TEXT("Recipe"), TEXT("Item"), TEXT("SpendingLimit"), TEXT("AuditLimit"),
        TEXT("DerivedGEFailure"), TEXT("Reentry"), TEXT("DeadApply"), TEXT("EndedApply"), TEXT("ConfigApply"), TEXT("Reward"), TEXT("RewardRollback"),
        TEXT("Waiting"), TEXT("Battle"), TEXT("Dead"), TEXT("Ended"), TEXT("Config"), TEXT("ViewLimit"), TEXT("RevisionLimit"), TEXT("SourceCollision"), TEXT("Ring64")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36TalentBusinessTest::RunTest(const FString& Case)
{
    using E = EY01ItemError; FLoadoutWorld F; F.P->SetOwner(F.PC); double Now = 0;
    FY01R36EconomyTestAccess::Clock(F.P->GetIncome(), Now); TestTrue(TEXT("真实库存初始化"), F.Init().IsOk());
    const auto Recipes = CraftTalentData(F); const FY01DataCatalog Catalog(F.Data.Tables); auto* Economy = F.P->GetEconomy(); auto* Sources = F.P->GetSourceEffects();
    TestTrue(TEXT("显式启用三选"), Economy->Initialize(F.Match, 1, 1, Catalog, {}, Recipes, FY01MatchFixture::Ref(K::TalentChoice)).IsOk());
    const auto Initial = CraftFull(F); if (!TestEqual(TEXT("三个合法候选"), Initial.TalentChoice.Candidates.Num(), 3)) { return false; }
    TSet<FName> Ids; for (const auto& C : Initial.TalentChoice.Candidates) { Ids.Add(C.TalentId); }
    TestEqual(TEXT("三个不重复"), Ids.Num(), 3); TestTrue(TEXT("未选择"), Initial.TalentChoice.SelectedTalent.IsNone()); TestEqual(TEXT("未选无效果"), F.Attack(), 30.f);
    FY01ItemRequest Q; Q.MatchId = F.Match; Q.JoinSequence = 1; Q.Seq = 1; Q.ConfigRevision = 1; Q.Operation = EY01ItemOperation::ChooseTalent;
    Q.TalentChoiceId = Initial.TalentChoice.ChoiceId; Q.Talent = FY01MatchFixture::Ref(K::Talent, Case == TEXT("Growth") ? TEXT("Talent_2") : Case == TEXT("Income") ? TEXT("Talent_1") : TEXT("Talent_0"));
    if (Case != TEXT("Waiting")) { F.Mode->TryStartNextWave(); }
    if (Case == TEXT("Battle")) { for (int32 I = 0; I < 23; ++I) { ++GFrameCounter; F.World->Tick(LEVELTICK_All, .05f); } }
    if (Case == TEXT("Snapshot")) { F.Data.Row<FY01TalentChoiceRow>(K::TalentChoice).Candidates.Reset(); F.Effect(TEXT("Ten"), EY01SourceEffectKind::AttackFlat, 999); }
    if (Case == TEXT("Reinitialize")) { TestTrue(TEXT("同会话重复init空参数"), Economy->Initialize(F.Match, 1, 1, Catalog, {}).IsOk()); }
    if (Case == TEXT("SpendingLimit") || Case == TEXT("AuditLimit")) { FY01R36EconomyTestAccess::Spending(F.P->GetIncome(), Case == TEXT("AuditLimit")); }
    bool Failure = false;
    if (Case == TEXT("NotCandidate")) { Q.Talent.RowName = TEXT("Default"); Failure = true; }
    if (Case == TEXT("WrongType")) { Q.Talent.Type = K::Equipment; Failure = true; }
    if (Case == TEXT("Materials")) { Q.Materials.Add({FGuid::NewGuid(), 1}); Failure = true; }
    if (Case == TEXT("Recipe")) { Q.Recipe = Recipes[0]; Failure = true; }
    if (Case == TEXT("Item")) { Q.ItemId = FGuid::NewGuid(); Failure = true; }
    if (Case == TEXT("Waiting")) { Failure = true; }
    if (Case == TEXT("Dead")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); Failure = true; }
    if (Case == TEXT("Ended")) { F.Mode->TryEndMatch(false); Failure = true; }
    if (Case == TEXT("Config")) { F.L()->SetConfigRevision(2); Failure = true; }
    if (Case == TEXT("ViewLimit") || Case == TEXT("RevisionLimit")) { FY01R36EconomyTestAccess::Limit(F.L(), Case == TEXT("ViewLimit") ? 3 : 0); Failure = true; }
    if (Case == TEXT("ForeignChoice") || Case == TEXT("OtherPlayer"))
    {
        auto* PC = F.Player(); auto* P = PC->GetPlayerState<AY01PlayerState>(); P->SetOwner(PC); FString Error;
        TestTrue(TEXT("第二玩家初始化角色"), P->TryInitializeCharacter(Catalog, FY01MatchFixture::Ref(K::Character), Error));
        TestTrue(TEXT("第二玩家独立会话"), P->GetLoadout()->Initialize(F.Match, 2).IsOk());
        TestTrue(TEXT("第二玩家三选"), P->GetEconomy()->Initialize(F.Match, 2, 1, Catalog, {}, {}, FY01MatchFixture::Ref(K::TalentChoice)).IsOk());
        FY01CommittedView Other; P->GetLoadout()->ReadCommittedView(Other);
        TestNotEqual(TEXT("机会绑定玩家而非行身份"), Q.TalentChoiceId, Other.TalentChoice.ChoiceId);
        if (Case == TEXT("ForeignChoice")) { Q.TalentChoiceId = Other.TalentChoice.ChoiceId; Failure = true; }
        else { auto Request = Q; Request.JoinSequence = 2; Request.TalentChoiceId = Other.TalentChoice.ChoiceId;
            TestTrue(TEXT("另一玩家可独立选择"), P->GetLoadout()->SubmitRequest(PC, Request).IsOk()); TestEqual(TEXT("不影响本玩家"), F.Attack(), 30.f); }
    }
    if (Case == TEXT("SourceCollision"))
    {
        FY01PreparedMutation Plan; FY01SourceTransaction Tx; FY01ResolvedSource Source;
        Source.Kind = EY01RuntimeSourceKind::Talent; Source.LocalKey = Initial.TalentChoice.SourceKey;
        for (const auto& Efx : Initial.TalentChoice.Candidates[0].Effects) { Source.Effects.Add({Efx.EffectId, Efx.Kind, Efx.Magnitude}); }
        Sources->PrepareMutation({Source}, {}, Plan); Sources->StageMutation(Plan, Tx); Sources->CommitMutation(Tx); Failure = true;
    }
    if (Case == TEXT("DerivedGEFailure") || Case == TEXT("RewardRollback") || Case == TEXT("Reentry"))
    {
        FY01PreparedMutation Plan; FY01SourceTransaction Tx;
        TestTrue(TEXT("故障前已有精确GE"), Sources->PrepareMutation({{EY01RuntimeSourceKind::Base, TEXT("Existing"), {{TEXT("Existing"), EY01SourceEffectKind::AttackFlat, 5}}}}, {}, Plan).IsOk());
        TestTrue(TEXT("已有GE暂存"), Sources->StageMutation(Plan, Tx).IsOk()); TestTrue(TEXT("已有GE提交"), Sources->CommitMutation(Tx).IsOk());
    }
    F.L()->PublishCommittedView();
    const auto Before = CraftFull(F); const float Attack = F.Attack(); const auto Handles = F.ASC()->GetActiveEffects(FGameplayEffectQuery());
    const auto Seeds = FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(Economy));
    const auto Spending = F.P->GetIncome()->Observe().State.TotalSpending;
    int32 Applied = 0; bool Once = false, RewardGranted = false, CandidateRemoved = false;
    FActiveGameplayEffectHandle CandidateHandle;
    F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda([&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec& Spec)
    {
        if (!Spec.Def || !Spec.Def->IsA(UY01EquipmentAttackEffect::StaticClass())) { return true; }
        ++Applied; return !(Case == TEXT("DerivedGEFailure") && Applied == 1);
    }));
    const auto Hook = F.ASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda([&](UAbilitySystemComponent*, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle)
    {
        if (Once || !Spec.Def || !Spec.Def->IsA(UY01EquipmentAttackEffect::StaticClass())) { return; } Once = true;
        TestTrue(TEXT("暂存仍只可见未选"), CraftFull(F).TalentChoice.SelectedTalent.IsNone());
        if (Case == TEXT("Reentry")) { TestEqual(TEXT("选择重入复用唯一锁"), F.L()->SubmitRequest(F.PC, Q).Code, E::Busy); }
        if (Case == TEXT("DeadApply")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        if (Case == TEXT("EndedApply")) { F.Mode->TryEndMatch(false); }
        if (Case == TEXT("ConfigApply")) { FY01R36EconomyTestAccess::Config(F.L()); }
        if (Case == TEXT("Reward") || Case == TEXT("RewardRollback"))
        { RewardGranted = F.P->TryAddMatchReward(7, 0); TestTrue(TEXT("GE回调合法奖励"), RewardGranted); }
        if (Case == TEXT("RewardRollback"))
        {
            CandidateHandle = Handle;
            TestTrue(TEXT("回调参数为新候选句柄"), Handle.IsValid() && !Handles.Contains(Handle));
            CandidateRemoved = F.ASC()->RemoveActiveGameplayEffect(Handle);
            TestTrue(TEXT("奖励之后真实移除候选GE"), CandidateRemoved);
        }
    });
    Failure |= Case == TEXT("DerivedGEFailure") || Case == TEXT("RewardRollback") || Case == TEXT("Reentry") || Case.EndsWith(TEXT("Apply"));
    const auto R = Economy->TryChooseTalent(F.PC, Q); F.ASC()->GameplayEffectApplicationQueries.Reset(); F.ASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(Hook);
    TestEqual(TEXT("一次选择真实业务"), R.IsOk(), !Failure); if (!R.IsOk()) { AddInfo(R.Message); }
    const auto After = CraftFull(F); const auto Gold = Case.StartsWith(TEXT("Reward")) ? 7 : 0;
    TestEqual(TEXT("选择不收费只保留奖励"), F.P->GetGold(), Gold); TestEqual(TEXT("不走支出"), F.P->GetIncome()->Observe().State.TotalSpending, Spending);
    TestTrue(TEXT("三经济流完全不动"), FY01R36EconomyTestAccess::Seeds(static_cast<const UY01EconomyComponent*>(Economy)) == Seeds);
    TestEqual(TEXT("候选身份稳定"), After.TalentChoice.ChoiceId, Initial.TalentChoice.ChoiceId);
    for (int32 I = 0; I < 3; ++I) { TestTrue(TEXT("候选快照从未刷新替换"), FY01TalentCandidate::StaticStruct()->CompareScriptStruct(&After.TalentChoice.Candidates[I], &Initial.TalentChoice.Candidates[I], 0)); }
    if (Failure)
    {
        TestTrue(TEXT("失败不耗永久机会"), After.TalentChoice.SelectedTalent.IsNone()); TestEqual(TEXT("失败攻击精确恢复"), F.Attack(), Attack);
        TestEqual(TEXT("失败GE数精确恢复"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), Handles.Num());
        for (auto H : Handles) { TestNotNull(TEXT("原GE句柄保持"), F.ASC()->GetActiveGameplayEffect(H)); }
        if (Case == TEXT("DerivedGEFailure") || Case == TEXT("RewardRollback"))
        {
            TestEqual(TEXT("唯一派生GE只查询一次"), Applied, 1);
            TestTrue(TEXT("失败精确保留原句柄集合"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()) == Handles);
            TestEqual(TEXT("失败来源版本保持"), After.SourceRevision, Before.SourceRevision);
        }
        if (Case == TEXT("DerivedGEFailure")) { TestFalse(TEXT("DerivedGEFailure拒绝唯一候选不执行OnApplied"), Once); }
        if (Case == TEXT("RewardRollback"))
        {
            TestEqual(TEXT("奖励后候选GE丢失拒绝暂存"), R.Code, E::EffectRejected);
            TestTrue(TEXT("RewardRollback真实OnApplied已奖励"), Once && RewardGranted);
            TestTrue(TEXT("RewardRollback确实移除新候选句柄"), CandidateRemoved && CandidateHandle.IsValid());
            TestNull(TEXT("候选句柄不残留"), F.ASC()->GetActiveGameplayEffect(CandidateHandle));
            TestEqual(TEXT("RewardRollback有真实原汇总GE可恢复"), Handles.Num(), 1);
        }
    }
    else if (R.IsOk())
    {
        TestEqual(TEXT("选中才生效"), F.Attack(), Case == TEXT("Income") || Case == TEXT("Growth") ? 30.f : 47.f);
        TestEqual(TEXT("选择与来源同版推进"), After.Revision, Before.Revision + 1);
        TestEqual(TEXT("回执选中身份"), R.SelectedTalent, Q.Talent.RowName); TestEqual(TEXT("回执机会"), R.TalentChoiceId, Q.TalentChoiceId);
        TestEqual(TEXT("同版选择"), After.TalentChoice.SelectedTalent, Q.Talent.RowName);
        TestEqual(TEXT("同版来源速率"), After.IncomeRate, Case == TEXT("Growth") ? 0. : 2.);
        TestTrue(TEXT("已选后重复init不重置"), Economy->Initialize(F.Match, 1, 1, Catalog, {}).IsOk());
        auto Next = Q; Next.Seq = F.View().ExpectedSeq; Next.Talent.RowName = TEXT("Talent_2");
        TestFalse(TEXT("换序号换候选不能再次选"), F.L()->SubmitRequest(F.PC, Next).IsOk());
        if (Case == TEXT("NoRevoke"))
        {
            TestFalse(TEXT("禁止单独撤销已选天赋"), Sources->RemoveSource(EY01RuntimeSourceKind::Talent, After.TalentChoice.SourceKey).IsOk());
            FY01RuntimeSourceId Id{EY01RuntimeSourceKind::Talent, After.TalentChoice.SourceKey, After.TalentChoice.SourceId}; FY01PreparedMutation Plan;
            TestFalse(TEXT("禁止批量替换已选天赋"), Sources->PrepareMutation({}, {Id}, Plan).IsOk());
            TestTrue(TEXT("选后收入开始"), F.P->GetIncome()->Start().IsOk()); Now = 1; TestTrue(TEXT("天赋持续收入"), F.P->GetIncome()->Settle().IsOk());
            Sources->EndPlay(EEndPlayReason::Quit); TestEqual(TEXT("Ended仍清理效果"), F.Attack(), 30.f);
            const auto EndGold = F.P->GetGold(); Now = 3; F.P->GetIncome()->Settle(); TestEqual(TEXT("Ended停止天赋收入"), F.P->GetGold(), EndGold);
        }
    }
    const auto Replay = F.L()->SubmitRequest(F.PC, Q); TestTrue(TEXT("天赋原回执重放"), FY01ItemResult::StaticStruct()->CompareScriptStruct(&R, &Replay, 0));
    if (Case != TEXT("ViewLimit") && Case != TEXT("NoRevoke")) { CraftTransfer(*this, Before, CraftFull(F)); }
    for (int32 I = 0; I < 3; ++I)
    { auto Conflict = Q; if (I == 0) { Conflict.Talent.RowName = TEXT("Other"); } if (I == 1) { Conflict.Talent.Type = Q.Talent.Type == K::Talent ? K::Equipment : K::Talent; } if (I == 2) { Conflict.TalentChoiceId = FGuid::NewGuid(); }
        TestEqual(TEXT("天赋完整载荷防重"), F.L()->SubmitRequest(F.PC, Conflict).Code, E::PayloadConflict); }
    if (Case == TEXT("DerivedGEFailure") || Case == TEXT("RewardRollback") || Case == TEXT("Reentry"))
    { Q.Seq = F.View().ExpectedSeq; TestTrue(TEXT("技术失败后的新请求仍能选"), F.L()->SubmitRequest(F.PC, Q).IsOk()); }
    if (Case == TEXT("Ring64"))
    { for (int32 I = 0; I < 65; ++I) { auto Next = Q; Next.Seq = F.View().ExpectedSeq; F.L()->SubmitRequest(F.PC, Next); }
        TestEqual(TEXT("旧号淘汰不重新授予"), F.L()->SubmitRequest(F.PC, Q).Code, E::ExpiredSequence); }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36CraftConfigTest, "Y01.Framework.R36.Economy.CraftTalent.Config",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36CraftConfigTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("RecipeDuplicate"), TEXT("RecipeIdentity"), TEXT("RecipeMissing"), TEXT("RecipeType"), TEXT("RecipeCount"), TEXT("RecipeRed"),
        TEXT("OutputType"), TEXT("OutputQuality"), TEXT("OutputMissing"), TEXT("ZeroWeight"), TEXT("NaNWeight"), TEXT("Precision"), TEXT("Resolution"),
        TEXT("Display"), TEXT("FakeSkill"), TEXT("ZeroFakeSkill"), TEXT("ChoiceMissing"), TEXT("ChoiceType"), TEXT("ChoiceTwo"), TEXT("ChoiceFour"), TEXT("ChoiceDuplicate"),
        TEXT("TalentMissing"), TEXT("TalentType"), TEXT("TalentQuality"), TEXT("TalentDisplay"), TEXT("TalentAbility"), TEXT("TalentEffect"), TEXT("TalentEmpty"),
        TEXT("EffectNaN"), TEXT("EffectMissing"), TEXT("EffectDuplicate"), TEXT("EffectKind"), TEXT("EffectOverflow"), TEXT("GrowthNaN"), TEXT("GrowthTarget"), TEXT("GrowthDuplicate"),
        TEXT("InitViewLimit"), TEXT("InitBusySource"), TEXT("InitLostGE"), TEXT("RefreshIgnored"), TEXT("DefaultDisabled")}) { Names.Add(N); Commands.Add(N); }
}
bool FY01R36CraftConfigTest::RunTest(const FString& Case)
{
    FLoadoutWorld F; F.P->SetOwner(F.PC); TestTrue(TEXT("库存初始化"), F.Init().IsOk()); auto Recipes = CraftTalentData(F);
    auto Choice = FY01MatchFixture::Ref(K::TalentChoice); auto& ChoiceRow = F.Data.Row<FY01TalentChoiceRow>(K::TalentChoice);
    auto& Recipe = F.Data.Row<FY01SynthesisRow>(K::Synthesis, Recipes[0].RowName);
    auto& Pool = F.Data.Row<FY01LocalItemPoolRow>(K::LocalItemPool, Recipe.OutputPool.RowName);
    auto& Talent = F.Data.Row<FY01TalentRow>(K::Talent, TEXT("Talent_0"));
    if (Case == TEXT("RecipeDuplicate")) { F.Data.Add(K::Synthesis, Recipe, TEXT("Alias")); Recipes.Add(FY01MatchFixture::Ref(K::Synthesis, TEXT("Alias"))); }
    if (Case == TEXT("RecipeIdentity")) { const auto Copy = Recipes[0]; Recipes.Add(Copy); }
    if (Case == TEXT("RecipeMissing")) { Recipes[0].RowName = TEXT("Absent"); }
    if (Case == TEXT("RecipeType")) { Recipe.InputType = K::Talent; }
    if (Case == TEXT("RecipeCount")) { Recipe.InputCount = 4; }
    if (Case == TEXT("RecipeRed")) { Recipe.InputQuality = EY01Quality::Red; }
    if (Case == TEXT("OutputType")) { Pool.ItemType = K::Skill; }
    if (Case == TEXT("OutputQuality")) { Pool.Quality = EY01Quality::Purple; }
    if (Case == TEXT("OutputMissing")) { Recipe.OutputPool.RowName = TEXT("Absent"); }
    if (Case == TEXT("ZeroWeight")) { for (auto& E : Pool.Entries) { E.Weight = 0; } }
    if (Case == TEXT("NaNWeight")) { Pool.Entries[0].Weight = std::numeric_limits<float>::quiet_NaN(); }
    if (Case == TEXT("Precision") || Case == TEXT("Resolution")) { Pool.Entries[0].Weight = Case == TEXT("Precision") ? 1.e30f : 1; Pool.Entries[1].Weight = 1.e-30f; }
    if (Case == TEXT("Display")) { F.Data.Row<FY01EquipmentRow>(K::Equipment, Pool.Entries[0].Item.RowName).DisplayName = FText(); }
    if (Case.Contains(TEXT("FakeSkill")))
    { auto& Row = F.Data.Row<FY01SkillRow>(K::Skill, Case == TEXT("ZeroFakeSkill") ? TEXT("Item_5_1_4") : TEXT("Item_5_1_0")); Row.Ability = FSoftObjectPath(TEXT("/Script/Y01.Y01PlayerState")); }
    if (Case == TEXT("ChoiceMissing")) { Choice.RowName = TEXT("Absent"); }
    if (Case == TEXT("ChoiceType")) { Choice.Type = K::Talent; }
    if (Case == TEXT("ChoiceTwo")) { ChoiceRow.Candidates.Pop(); }
    if (Case == TEXT("ChoiceFour")) { const auto Copy = ChoiceRow.Candidates[0]; ChoiceRow.Candidates.Add(Copy); }
    if (Case == TEXT("ChoiceDuplicate")) { ChoiceRow.Candidates[2] = ChoiceRow.Candidates[0]; }
    if (Case == TEXT("TalentMissing")) { ChoiceRow.Candidates[0].RowName = TEXT("Absent"); }
    if (Case == TEXT("TalentType")) { ChoiceRow.Candidates[0].Type = K::Equipment; }
    if (Case == TEXT("TalentQuality")) { Talent.Quality = static_cast<EY01Quality>(255); }
    if (Case == TEXT("TalentDisplay")) { Talent.DisplayName = FText(); }
    if (Case == TEXT("TalentAbility")) { Talent.Ability = UY01BasicAttackAbility::StaticClass(); }
    if (Case == TEXT("TalentEffect")) { Talent.Effect = UY01EquipmentAttackEffect::StaticClass(); }
    if (Case == TEXT("TalentEmpty")) { Talent.EffectIds.Reset(); Talent.GrowthRuleIds.Reset(); }
    if (Case == TEXT("EffectNaN")) { F.Effect(TEXT("Other"), EY01SourceEffectKind::AttackFlat, std::numeric_limits<double>::quiet_NaN()); }
    if (Case == TEXT("EffectMissing")) { Talent.EffectIds[0].RowName = TEXT("Absent"); }
    if (Case == TEXT("EffectDuplicate")) { const auto Copy = Talent.EffectIds[0]; Talent.EffectIds.Add(Copy); }
    if (Case == TEXT("EffectKind")) { F.Effect(TEXT("Other"), static_cast<EY01SourceEffectKind>(255), 5); }
    if (Case == TEXT("EffectOverflow")) { F.Effect(TEXT("Other"), EY01SourceEffectKind::AttackFlat, 1.e300); }
    if (Case == TEXT("GrowthNaN")) { F.Data.Row<FY01GrowthRuleRow>(K::GrowthRule, TEXT("Permanent")).Delta = std::numeric_limits<double>::quiet_NaN(); }
    if (Case == TEXT("GrowthTarget")) { F.Data.Row<FY01GrowthRuleRow>(K::GrowthRule, TEXT("Permanent")).Target = EY01GrowthTarget::EquipmentInstance; }
    if (Case == TEXT("GrowthDuplicate")) { const auto Copy = Talent.GrowthRuleIds[0]; Talent.GrowthRuleIds.Add(Copy); }
    if (Case == TEXT("RefreshIgnored")) { ChoiceRow.RefreshesPerCandidate = 999; }
    if (Case == TEXT("DefaultDisabled")) { Recipes.Reset(); Choice = {}; }
    if (Case == TEXT("InitViewLimit")) { FY01R36EconomyTestAccess::Limit(F.L(), 3); }
    FY01SourceTransaction Pending; auto* Sources = F.P->GetSourceEffects();
    if (Case == TEXT("InitBusySource") || Case == TEXT("InitLostGE"))
    {
        FY01PreparedMutation Plan;
        TestTrue(TEXT("初始化前来源"), Sources->PrepareMutation({{EY01RuntimeSourceKind::Base, TEXT("Initial"), {{TEXT("Initial"), EY01SourceEffectKind::AttackFlat, 5}}}}, {}, Plan).IsOk());
        TestTrue(TEXT("初始化前来源暂存"), Sources->StageMutation(Plan, Pending).IsOk());
        if (Case == TEXT("InitLostGE")) { TestTrue(TEXT("来源提交"), Sources->CommitMutation(Pending).IsOk()); F.L()->PublishCommittedView();
            F.ASC()->RemoveActiveGameplayEffect(F.ASC()->GetActiveEffects(FGameplayEffectQuery())[0]); }
    }
    auto* Economy = F.P->GetEconomy(); const auto Before = CraftFull(F);
    const auto R = Economy->Initialize(F.Match, 1, 1, FY01DataCatalog(F.Data.Tables), {}, Recipes, Choice);
    const bool Valid = Case == TEXT("RefreshIgnored") || Case == TEXT("DefaultDisabled");
    TestEqual(TEXT("运行消费者严格配置校验"), R.IsOk(), Valid); TestEqual(TEXT("技术失败不部分启用"), Economy->IsInitialized(), Valid);
    const auto After = CraftFull(F);
    if (!Valid) { TestTrue(TEXT("失败完整视图不部分发布"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Before, &After, 0)); }
    else { TestEqual(TEXT("默认空不启用候选"), After.TalentChoice.Candidates.Num(), Case == TEXT("DefaultDisabled") ? 0 : 3); }
    if (Case == TEXT("InitBusySource")) { Sources->RollbackMutation(Pending); }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36CraftWireTest, "Y01.Framework.R36.Economy.CraftTalent.Wire",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36CraftWireTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("RecipeDuplicate"), TEXT("RecipeKindDuplicate"), TEXT("RecipeCount"), TEXT("RecipeRed"), TEXT("RecipeType"), TEXT("RecipePool"),
        TEXT("ChoiceMissing"), TEXT("ChoiceDefinition"), TEXT("ChoiceTwo"), TEXT("ChoiceDuplicate"), TEXT("ChoiceForeign"), TEXT("ChoiceSelected"),
        TEXT("TalentIdentity"), TEXT("TalentQuality"), TEXT("TalentDisplay"), TEXT("TalentNaN"), TEXT("TalentGrowth"), TEXT("DisabledDirty")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36CraftWireTest::RunTest(const FString& Case)
{
    FLoadoutWorld F; F.P->SetOwner(F.PC); TestTrue(TEXT("初始化"), F.Init().IsOk()); const auto Recipes = CraftTalentData(F);
    TestTrue(TEXT("公开完整配置"), F.P->GetEconomy()->Initialize(F.Match, 1, 1, FY01DataCatalog(F.Data.Tables), {}, Recipes, FY01MatchFixture::Ref(K::TalentChoice)).IsOk());
    const auto Good = CraftFull(F); auto Bad = Good;
    if (Case == TEXT("RecipeDuplicate")) { const auto Value = Bad.SynthesisRecipes[0]; Bad.SynthesisRecipes.Add(Value); }
    if (Case == TEXT("RecipeKindDuplicate")) { Bad.SynthesisRecipes[1].Recipe = Bad.SynthesisRecipes[0].Recipe; }
    if (Case == TEXT("RecipeCount")) { Bad.SynthesisRecipes[0].Recipe.InputCount = 4; }
    if (Case == TEXT("RecipeRed")) { Bad.SynthesisRecipes[0].Recipe.InputQuality = EY01Quality::Red; }
    if (Case == TEXT("RecipeType")) { Bad.SynthesisRecipes[0].Recipe.InputType = K::Talent; }
    if (Case == TEXT("RecipePool")) { Bad.SynthesisRecipes[0].Recipe.OutputPool.Type = K::Talent; }
    auto& Choice = Bad.TalentChoice;
    if (Case == TEXT("ChoiceMissing")) { Choice.ChoiceId.Invalidate(); }
    if (Case == TEXT("ChoiceDefinition")) { Choice.Definition.Type = K::Talent; }
    if (Case == TEXT("ChoiceTwo")) { Choice.Candidates.Pop(); }
    if (Case == TEXT("ChoiceDuplicate")) { Choice.Candidates[2] = Choice.Candidates[0]; }
    if (Case == TEXT("ChoiceForeign")) { Choice.SourceId = TEXT("Foreign"); }
    if (Case == TEXT("ChoiceSelected")) { Choice.SelectedTalent = TEXT("Unlisted"); }
    if (Case == TEXT("TalentIdentity")) { Choice.Candidates[0].TalentId = NAME_None; }
    if (Case == TEXT("TalentQuality")) { Choice.Candidates[0].Quality = static_cast<EY01Quality>(255); }
    if (Case == TEXT("TalentDisplay")) { Choice.Candidates[0].DisplayName = FText(); }
    if (Case == TEXT("TalentNaN")) { Choice.Candidates[0].Effects[0].Magnitude = std::numeric_limits<double>::quiet_NaN(); }
    if (Case == TEXT("TalentGrowth")) { Choice.Candidates[0].Growth[0].AppliedCount = 9; }
    if (Case == TEXT("DisabledDirty")) { Choice = {}; Choice.SelectedTalent = TEXT("Talent_0"); }
    FY01ViewSender Sender; FY01ViewWire Wire; FString Error;
    TestFalse(TEXT("非法经济完整视图不得编码发布"), Sender.Offer(Bad, Wire, Error));
    FY01ViewSender Trusted; FY01ViewReceiver Receiver; FY01ViewAck Ack;
    TestTrue(TEXT("先收到合法业务头"), Trusted.Offer(Good, Wire, Error));
    while (!Wire.Items.IsEmpty())
    { if (!Receiver.Receive(Wire, Ack, Error) || !Trusted.Acknowledge(Ack, Wire, Error)) { AddError(Error); return false; } }
    // 绕过发送器，生成校验和及全量封套正确但业务身份非法的真实二进制提交。
    FY01ViewCommit Commit; Commit.Header = Bad; ++Commit.Header.ViewRevision;
    Commit.Header.Items.Reset(); Commit.ChangedItems = Bad.Items;
    for (const auto& I : Bad.Items)
    { auto& M = Commit.Manifest.AddDefaulted_GetRef(); M.ItemId = I.ItemId; M.EntityRevision = I.EntityRevision; M.LastChangedRevision = I.LastChangedRevision; }
    TArray<uint8> Bytes; FMemoryWriter Writer(Bytes, true); FObjectAndNameAsStringProxyArchive Ar(Writer, false);
    Ar.SetWantBinaryPropertySerialization(true); uint32 Version = 0x59303107; Ar << Version;
    FY01ViewCommit::StaticStruct()->SerializeBin(Ar, &Commit);
    const auto Transfer = FGuid::NewGuid(); bool Accepted = true; int32 Window = 0;
    for (int32 Offset = 0; Offset < Bytes.Num(); Offset += FY01ViewWire::ChunkBytes * FY01ViewWire::WindowChunks)
    {
        Wire.Items.Reset(); ++Window;
        for (int32 I = 0; I < FY01ViewWire::WindowChunks && Offset + I * FY01ViewWire::ChunkBytes < Bytes.Num(); ++I)
        {
            auto& C = Wire.Items.AddDefaulted_GetRef(); C.Scope.MatchId = Good.MatchId; C.Scope.JoinSequence = Good.JoinSequence;
            C.Scope.ConnectionId = Good.ConnectionId; C.Scope.TransferId = Transfer; C.Scope.ViewRevision = Commit.Header.ViewRevision;
            C.Scope.WindowSequence = Window; C.Offset = Offset + I * FY01ViewWire::ChunkBytes; C.TotalBytes = Bytes.Num();
            C.Digest = FCrc::MemCrc32(Bytes.GetData(), Bytes.Num()); C.Bytes.Append(Bytes.GetData() + C.Offset, FMath::Min(FY01ViewWire::ChunkBytes, Bytes.Num() - C.Offset));
            C.ChunkDigest = FCrc::MemCrc32(C.Bytes.GetData(), C.Bytes.Num());
        }
        Accepted = Receiver.Receive(Wire, Ack, Error); if (!Accepted) { break; }
    }
    TestFalse(TEXT("真实接收器拒绝校验和正确的恶意业务头"), Accepted);
    TestEqual(TEXT("通过当前编码版本后由业务校验拒绝"), Error, FString(TEXT("清单、期望版本或槽位不完整，需要重同步")));
    TestTrue(TEXT("恶意完整头不覆盖上一完整版本"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Good, &Receiver.Get(), 0));
    CraftTransfer(*this, Good, Good); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R36EconomyOrderTest, "Y01.Framework.R36.RuntimeIntegration.EconomyOrder",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R36EconomyOrderTest::RunTest(const FString&)
{
    FLoadoutWorld W; TestTrue(TEXT("库存初始化"), W.Init().IsOk());
    for (int32 I = 0; I < 3; ++I)
    {
        const FName Name(*FString::Printf(TEXT("Ordered%d"), I));
        auto E = W.Data.Row<FY01EquipmentRow>(K::Equipment, TEXT("Sword"));
        E.DisplayName = FText::FromString(TEXT("顺序测试装备")); E.Quality = static_cast<EY01Quality>(I);
        W.Data.Add(K::Equipment, E, Name);
        FY01LocalItemPoolRow Pool; Pool.Quality = E.Quality; FY01WeightedEntry Entry;
        Entry.Item = W.Data.Ref(K::Equipment, Name); Pool.Entries.Add(Entry); W.Data.Add(K::LocalItemPool, Pool, Name);
    }
    TArray<FY01DataRef> Offers, Recipes;
    for (int32 I = 0; I < 2; ++I)
    {
        const FName Name(I == 0 ? TEXT("Zulu") : TEXT("Alpha"));
        FY01ShopOfferRow O; O.DisplayName = FText::FromString(TEXT("顺序商品")); O.Pool = W.Data.Ref(K::LocalItemPool, TEXT("Ordered0"));
        W.Data.Add(K::ShopOffer, O, Name); Offers.Add(W.Data.Ref(K::ShopOffer, Name));
        FY01SynthesisRow R; R.InputQuality = static_cast<EY01Quality>(I);
        R.OutputPool = W.Data.Ref(K::LocalItemPool, FName(*FString::Printf(TEXT("Ordered%d"), I + 1)));
        W.Data.Add(K::Synthesis, R, Name); Recipes.Add(W.Data.Ref(K::Synthesis, Name));
    }
    auto* Economy = W.P->GetEconomy();
    TestTrue(TEXT("按显式引用初始化"), Economy->Initialize(W.Match, 1, W.View().ConfigRevision, FY01DataCatalog(W.Data.Tables), Offers, Recipes).IsOk());
    for (int32 Pass = 0; Pass < 2; ++Pass)
    {
        FY01CommittedView V; TestTrue(TEXT("完整视图可读"), W.L()->ReadCommittedView(V));
        if (!TestEqual(TEXT("商品数量"), V.ShopOffers.Num(), 2) || !TestEqual(TEXT("配方数量"), V.SynthesisRecipes.Num(), 2)) { return false; }
        TestEqual(TEXT("商品不按ID重排"), V.ShopOffers[0].OfferId, Offers[0].RowName);
        TestEqual(TEXT("配方不按ID重排"), V.SynthesisRecipes[0].RecipeId, Recipes[0].RowName);
        Offers.Swap(0, 1); Recipes.Swap(0, 1);
        TestTrue(TEXT("重复初始化不得改变已提交顺序"), Economy->Initialize(W.Match, 1, W.View().ConfigRevision, FY01DataCatalog(W.Data.Tables), Offers, Recipes).IsOk());
        Offers.Swap(0, 1); Recipes.Swap(0, 1); W.L()->PublishCommittedView();
    }
    return true;
}

struct FY01DamageNextTestAccess
{
    static void Pump(UY01DamageEffectsComponent* C, double Now) { C->Update(Now); }
    static int32 Products(const UY01DamageEffectsComponent* C) { return C->Products.Num(); }
    static TArray<double> Deadlines(const UY01DamageEffectsComponent* C, FName Effect)
    {
        TArray<uint64> Ids; C->Products.GenerateKeyArray(Ids); Ids.Sort(); TArray<double> Out;
        for (uint64 Id : Ids) { const auto& P=C->Products.FindChecked(Id); if(P.Effect.EffectId==Effect){Out.Add(P.Expires);} } return Out;
    }
};
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DamageNextTest, "Y01.Framework.R36.DamageNext",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01DamageNextTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("FormulaSharedActive"), TEXT("DisplayOverkill"), TEXT("TargetingBranches"), TEXT("Ready347"),
        TEXT("ProjectileSnapshotDotCurrent"), TEXT("RefreshStackCap"), TEXT("AreaReentryExpiry"),
        TEXT("UnequipOwnershipTransaction"), TEXT("SessionAndReentry"), TEXT("KillQueue"), TEXT("CatalogWire"), TEXT("ActiveAreaIndicator")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01DamageNextTest::RunTest(const FString& Case)
{
    FLoadoutWorld W; W.P->SetOwner(W.PC);
    if (!TestTrue(TEXT("初始化真实库存"), W.Init().IsOk()) || !TestTrue(TEXT("准备阶段"), W.Mode->TryStartNextWave())) { return false; }
    auto* D = W.P->GetDamageEffects();
    auto Pump = [&](double Now) { FY01DamageNextTestAccess::Pump(D, Now); };
    auto HP = [](AY01Monster* M) { return M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()); };
    auto Spawn = [&](double X, double Y = 0.)
    {
        FY01MonsterRow R; R.Health = 10000; R.Armor = 0; R.Skills.Reset(); R.DropReward = W.Data.Ref(K::RewardPackage);
        W.Data.Add(K::Monster, R, TEXT("DamageTarget"));
        auto* M = W.World->SpawnActor<AY01Monster>(FVector(X,Y,100), FRotator::ZeroRotator);
        FString Error; TestTrue(TEXT("真实目标ASC"), M->TryInitializeMonster(FY01DataCatalog(W.Data.Tables), W.Data.Ref(K::Monster, TEXT("DamageTarget")), Error));
        return M;
    };
    auto Equip = [&](FName Name, const FY01DamageEffectRow& Effect, int32 Slot = 0, bool Attribute = false)
    {
        W.Data.Add(K::DamageEffect, Effect, Name); FY01EquipmentRow R; R.DisplayName = FText::FromName(Name);
        R.DamageEffectIds = {W.Data.Ref(K::DamageEffect, Name)};
        if (Attribute) { R.EffectIds = {W.Data.Ref(K::SourceEffect, TEXT("Ten"))}; }
        W.Data.Add(K::Equipment, R, Name); auto G = W.Grant(Name);
        TestTrue(TEXT("伤害装备入包"), W.Add({G}).IsOk()); TestTrue(TEXT("伤害装备穿戴"), W.Send(W.Request(G.ItemId, Slot)).IsOk()); return G.ItemId;
    };
    FY01DamageEffectRow R; R.FixedDamage = 10; R.PeriodSeconds = 3; R.SearchRadius = 1000; R.Radius = 150;
    if (Case == TEXT("FormulaSharedActive"))
    {
        FY01UnitCombatStats S; S.Strength = 10; S.Agility = 20; S.Wisdom = 30;
        R.FixedDamage = 100; R.Terms = {{EY01Attribute::Strength,.5},{EY01Attribute::Agility,.3},{EY01Attribute::Wisdom,1}};
        TestEqual(TEXT("公式141"), Y01Damage::Evaluate(R, S), 141.f); S.Strength = 20;
        TestEqual(TEXT("公式146"), Y01Damage::Evaluate(R, S), 146.f);
        auto* M = Spawn(600); R.Terms = {{EY01Attribute::AttackDamage,.5}};
        auto* Events = W.World->GetSubsystem<UY01DamageEvents>(); TArray<float> Actual;
        const auto Hook = Events->OnDamageCommitted.AddLambda([&](const FY01DamageEvent& Event) { Actual.Add(Event.ActualDamage); });
        ON_SCOPE_EXIT { Events->OnDamageCommitted.Remove(Hook); };
        auto ForgedContext = W.ASC()->MakeEffectContext(); ForgedContext.AddSourceObject(D);
        auto Forged = W.ASC()->MakeOutgoingSpec(UY01TemplateDamageEffect::StaticClass(),1,ForgedContext);
        Forged.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage,9999);
        W.ASC()->ApplyGameplayEffectSpecToTarget(*Forged.Data,M->GetCombatASC());
        TestEqual(TEXT("无服务器执行令牌不能伪造GE伤害"),HP(M),10000.f);
        Equip(TEXT("Formula"), R); Pump(3); TestEqual(TEXT("装备共享公式且不受普攻射程限制"), HP(M), 9885.f);
        R.Trigger = EY01DamageTrigger::Active; W.Data.Add(K::DamageEffect, R, TEXT("Active"));
        FY01SkillRow B; B.DisplayName = FText::FromString(TEXT("公式技能")); B.Ability = UY01DamageAbility::StaticClass(); B.CooldownSeconds = 8;
        B.DamageEffectIds = {W.Data.Ref(K::DamageEffect, TEXT("Active"))}; W.Data.Add(K::Skill, B, TEXT("DamageBook"));
        const auto G = W.Grant(TEXT("DamageBook"), K::Skill); TestTrue(TEXT("技能书入包"), W.Add({G}).IsOk());
        TestTrue(TEXT("原生GA学习"), W.Learn(W.LearnRequest(G.ItemId,0)).IsOk());
        TestTrue(TEXT("用户主动释放"), W.P->TryActivateSkillSlot(0)); TestEqual(TEXT("技能同公式"), HP(M), 9770.f);
        TestFalse(TEXT("主动CD不能重放"), W.P->TryActivateSkillSlot(0)); TestEqual(TEXT("普攻CD未修改"), W.ASC()->GetNextAutoAttackTime(), 0.);
        TestEqual(TEXT("真实伤害进入既有数字事件链"), Actual.Num(), 2);
        for (float Value : Actual) { TestEqual(TEXT("事件是实际扣血非客户端数值"), Value, 115.f); }
    }
    else if (Case == TEXT("ActiveAreaIndicator"))
    {
        auto* Near = Spawn(100); auto* Far = Spawn(400);
        auto Visuals = [&]
        {
            TArray<AY01DamageVisual*> Out;
            for (TActorIterator<AY01DamageVisual> It(W.World); It; ++It) { if (It->GetOwner() == W.P && !It->IsActorBeingDestroyed()) { Out.Add(*It); } }
            return Out;
        };
        auto Learn = [&](FName Name, const FY01DamageEffectRow& Effect, int32 Slot)
        {
            W.Data.Add(K::DamageEffect, Effect, Name);
            FY01SkillRow B; B.DisplayName = FText::FromName(Name); B.Ability = UY01DamageAbility::StaticClass(); B.CooldownSeconds = 8;
            B.DamageEffectIds = {W.Data.Ref(K::DamageEffect, Name)}; W.Data.Add(K::Skill, B, Name);
            const auto G = W.Grant(Name, K::Skill); TestTrue(TEXT("区域技能书入包"), W.Add({G}).IsOk());
            TestTrue(TEXT("区域技能学习"), W.Learn(W.LearnRequest(G.ItemId, Slot)).IsOk());
        };
        R.Trigger = EY01DamageTrigger::Active; R.Targeting = EY01DamageTargeting::SelfCircle; R.Radius = 250;
        Learn(TEXT("SelfFlash"), R, 0);
        auto Remote = R; Remote.Targeting = EY01DamageTargeting::RemoteCircle; Remote.Center = EY01DamageCenter::NearestEnemy;
        Remote.SearchRadius = 1000; Remote.Radius = 120; Learn(TEXT("RemoteFlash"), Remote, 1);
        TestEqual(TEXT("施放前无范围圈"), Visuals().Num(), 0);
        const FVector Origin = W.ASC()->GetAvatarActor()->GetActorLocation();
        TestTrue(TEXT("自身圆主动释放"), W.P->TryActivateSkillSlot(0));
        TestEqual(TEXT("圈内照常受伤"), HP(Near), 9990.f); TestEqual(TEXT("圈外不受伤"), HP(Far), 10000.f);
        auto Shown = Visuals();
        if (TestEqual(TEXT("即时区域显示一个范围圈"), Shown.Num(), 1))
        {
            TestTrue(TEXT("自身圆圈心为施放者"), FVector::DistSquared2D(Shown[0]->GetActorLocation(), Origin) < 1.);
            TestEqual(TEXT("圈半径等于真实判定半径"), Shown[0]->GetRadius(), 250.f);
            TestEqual(TEXT("指示圈类型"), Shown[0]->GetProduct(), EY01DamageProduct::Instant);
            TestTrue(TEXT("指示圈短暂存在"), Shown[0]->GetLifeSpan() > 0 && Shown[0]->GetLifeSpan() <= .5f);
            Shown[0]->Destroy();
        }
        TestEqual(TEXT("指示圈不进产物账本"), FY01DamageNextTestAccess::Products(D), 0);
        TestFalse(TEXT("指示圈不改主动CD"), W.P->TryActivateSkillSlot(0));
        TestTrue(TEXT("远处圆主动释放"), W.P->TryActivateSkillSlot(1));
        TestEqual(TEXT("远处圆以最近敌为心"), HP(Near), 9980.f); TestEqual(TEXT("远处圆不及远怪"), HP(Far), 10000.f);
        Shown = Visuals();
        if (TestEqual(TEXT("远处圆显示一个范围圈"), Shown.Num(), 1))
        {
            TestTrue(TEXT("远处圆圈心为真实中心"), FVector::DistSquared2D(Shown[0]->GetActorLocation(), Near->GetActorLocation()) < 1.);
            TestEqual(TEXT("远处圆半径一致"), Shown[0]->GetRadius(), 120.f);
        }
    }
    else if (Case == TEXT("DisplayOverkill"))
    {
        auto* M = Spawn(600); auto* Target = M->GetCombatASC();
        Target->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 25); R.FixedDamage = 35;
        TArray<FY01DamageEvent> Events; auto* Service = W.World->GetSubsystem<UY01DamageEvents>();
        const auto Hook = Service->OnDamageCommitted.AddLambda([&](const FY01DamageEvent& Event) { Events.Add(Event); });
        Equip(TEXT("DisplayOverkill"), R); Pump(3);
        TestEqual(TEXT("模板执行器过量伤害只扣剩余生命"), HP(M), 0.f);
        if (TestEqual(TEXT("模板执行器只发布一个事件"), Events.Num(), 1))
        {
            TestEqual(TEXT("模板执行器Actual是真实血差"), Events[0].ActualDamage, 25.f);
            TestEqual(TEXT("模板执行器Display保留完整结算量"), Events[0].DisplayAmount, 35.f);
            TestEqual(TEXT("模板执行器事件类型为伤害"), Events[0].Type, EY01DamageEventType::Damage);
        }
        Service->OnDamageCommitted.Remove(Hook);
    }
    else if (Case == TEXT("TargetingBranches"))
    {
        auto* A = Spawn(100); auto* B = Spawn(200); auto* C = Spawn(900); TArray<AActor*> Candidates = {C,A,B,A};
        FRandomStream Random(17); FY01DamageSelection Selection; R.TargetCount = 2;
        TestTrue(TEXT("最近N发动"), Y01Damage::Select(W.ASC(), R, Candidates, Random, Selection));
        TestEqual(TEXT("同次去重"), Selection.Targets.Num(),2); TestTrue(TEXT("最近顺序"), Selection.Targets[0] == A && Selection.Targets[1] == B);
        R.Order = EY01DamageOrder::Random; R.TargetCount = 9;
        TestTrue(TEXT("随机不足N"), Y01Damage::Select(W.ASC(),R,Candidates,Random,Selection)); TestEqual(TEXT("随机去重"), Selection.Targets.Num(),3);
        R.Targeting = EY01DamageTargeting::SelfCircle; R.Radius = 250;
        TestTrue(TEXT("自身圆"), Y01Damage::Select(W.ASC(),R,Candidates,Random,Selection)); TestEqual(TEXT("自身圆独立半径"),Selection.Targets.Num(),2);
        R.Targeting = EY01DamageTargeting::RemoteCircle; R.Radius = 1;
        for (auto Center : {EY01DamageCenter::NearestEnemy,EY01DamageCenter::RandomEnemy,EY01DamageCenter::RandomGround})
        {
            R.Center = Center; TestTrue(TEXT("远处三中心均可发动"), Y01Damage::Select(W.ASC(),R,Candidates,Random,Selection));
            TestTrue(TEXT("中心在搜索圆"), FVector::DistSquared2D(Selection.Center,W.ASC()->GetAvatarActor()->GetActorLocation()) <= 1000000.);
            if (Center == EY01DamageCenter::RandomGround) { TestEqual(TEXT("随机地面可落空"),Selection.Targets.Num(),0); }
        }
        TestFalse(TEXT("随机地面无敌待命"),Y01Damage::Select(W.ASC(),R,{},Random,Selection));
    }
    else if (Case == TEXT("Ready347"))
    {
        Equip(TEXT("Ready"),R); Pump(3); auto* M = Spawn(100); Pump(4); TestEqual(TEXT("4秒立即发"),HP(M),9990.f);
        Pump(6.99); TestEqual(TEXT("不按6秒旧刻度"),HP(M),9990.f); Pump(7); TestEqual(TEXT("7秒再发"),HP(M),9980.f);
        Pump(7); TestEqual(TEXT("同刻不重复"),HP(M),9980.f); Pump(100); TestEqual(TEXT("不补欠账"),HP(M),9970.f);
        const auto Duplicate = W.Grant(TEXT("Ready"));
        TestTrue(TEXT("同表行第二实例入包"), W.Add({Duplicate}).IsOk());
        TestTrue(TEXT("同表行第二来源入槽"), W.Send(W.Request(Duplicate.ItemId, 1)).IsOk());
        FY01SkillRow Passive; Passive.Type = EY01SkillType::Passive; Passive.DisplayName = FText::FromString(TEXT("周期技能"));
        Passive.DamageEffectIds = {W.Data.Ref(K::DamageEffect, TEXT("Ready"))}; W.Data.Add(K::Skill, Passive, TEXT("PeriodicBook"));
        const auto Book = W.Grant(TEXT("PeriodicBook"), K::Skill);
        TestTrue(TEXT("周期技能书入包"), W.Add({Book}).IsOk());
        TestTrue(TEXT("周期技能入槽"), W.Learn(W.LearnRequest(Book.ItemId, 0)).IsOk());
        Pump(102.99); TestEqual(TEXT("新增来源各等完整间隔"), HP(M), 9970.f);
        Pump(103); TestEqual(TEXT("同效果两装备及技能三来源不按表行合并"), HP(M), 9940.f);
        TestFalse(TEXT("被动技能不开放主动入口"), W.P->TryActivateSkillSlot(0));
    }
    else if (Case == TEXT("ProjectileSnapshotDotCurrent"))
    {
        auto* M = Spawn(600); R.Product = EY01DamageProduct::Projectile; R.Speed = 100; R.Lifetime = 20;
        R.FixedDamage = 0; R.Terms = {{EY01Attribute::AttackDamage,1}};
        const auto Id = Equip(TEXT("Flight"),R,0,true); Pump(3); TestEqual(TEXT("飞行尚未命中"),HP(M),10000.f);
        TestTrue(TEXT("卸装"),W.Send(W.Request(Id,0,EY01ItemOperation::Unequip)).IsOk()); Pump(9);
        TestEqual(TEXT("释放快照40保留"),HP(M),9960.f); TestEqual(TEXT("卸装属性消失"),W.Attack(),30.f);
        R.Product = EY01DamageProduct::AttachedDot; R.TickSeconds = 1; R.Lifetime = 5;
        const auto Dot = Equip(TEXT("Dot"),R,0,true); Pump(12); const float Before = HP(M);
        TestTrue(TEXT("卸毒装备"),W.Send(W.Request(Dot,0,EY01ItemOperation::Unequip)).IsOk()); M->SetActorLocation(FVector(5000,0,100));
        Pump(13); TestEqual(TEXT("挂身离搜索范围继续并读卸装后30"),HP(M),Before-30);
        W.ASC()->InvalidateCombatSession(); M->SetActorLocation(FVector(600,0,100));
        R.Product = EY01DamageProduct::Projectile; R.Lifetime = 20; Equip(TEXT("RetireFlight"),R);
        Pump(16); const float BeforeRetire = HP(M); int32 Retired = 0;
        const auto Hook = W.World->AddOnActorDestroyedHandler(FOnActorDestroyed::FDelegate::CreateLambda([&](AActor* Actor)
        { if (Actor->IsA<AY01DamageVisual>() && Actor->GetOwner() == W.P) { ++Retired; W.ASC()->InvalidateCombatSession(); } }));
        Pump(22); W.World->RemoveOnActorDestroyedHandler(Hook);
        TestEqual(TEXT("命中前退休表现一次"), Retired, 1);
        TestEqual(TEXT("退休回调切区后不能补发旧会话命中"), HP(M), BeforeRetire);
    }
    else if (Case == TEXT("RefreshStackCap"))
    {
        auto* M = Spawn(100); R.Product = EY01DamageProduct::AttachedDot; R.PeriodSeconds = 3; R.Lifetime = 10; R.TickSeconds = 2;
        Equip(TEXT("Refresh"),R); Pump(3); Pump(5); Pump(6); TestEqual(TEXT("刷新只延寿不补首跳"),HP(M),9980.f);
        Pump(7); TestEqual(TEXT("原节拍7秒"),HP(M),9970.f);
        W.ASC()->InvalidateCombatSession(); Pump(10);
        R.Stacking = EY01DamageStacking::Independent; R.MaxStacks = 2; Equip(TEXT("Stack"),R,1);
        Pump(13); Pump(16); const int32 Count = FY01DamageNextTestAccess::Products(D); Pump(19);
        TestEqual(TEXT("满层仅刷新最早者不增份"),FY01DamageNextTestAccess::Products(D),Count);
        const auto Deadlines = FY01DamageNextTestAccess::Deadlines(D,TEXT("Stack"));
        TestEqual(TEXT("同来源两层"), Deadlines.Num(),2);
        if(Deadlines.Num()==2) { TestEqual(TEXT("最早创建份已刷新"),Deadlines[0],29.); TestEqual(TEXT("另一份未改"),Deadlines[1],26.); }
    }
    else if (Case == TEXT("AreaReentryExpiry"))
    {
        auto* M = Spawn(100); R.Product = EY01DamageProduct::IndependentArea; R.Targeting = EY01DamageTargeting::SelfCircle;
        R.PeriodSeconds = 100; R.TickSeconds = 2; R.Lifetime = 6;
        Equip(TEXT("Area"),R); Pump(100); TestEqual(TEXT("区域首入立即"),HP(M),9990.f);
        M->SetActorLocation(FVector(1000,0,100)); Pump(101); M->SetActorLocation(FVector(100,0,100)); Pump(103);
        TestEqual(TEXT("重入不补首跳及欠账"),HP(M),9990.f); Pump(104); TestEqual(TEXT("保留原节拍"),HP(M),9980.f);
        Pump(106); TestEqual(TEXT("到期优先不跳"),HP(M),9980.f);
    }
    else if (Case == TEXT("UnequipOwnershipTransaction"))
    {
        auto* M = Spawn(100); R.Trigger = EY01DamageTrigger::Bound; R.Product = EY01DamageProduct::BoundArea;
        R.Targeting = EY01DamageTargeting::SelfCircle; R.Position = EY01DamagePosition::FollowPlayer; R.TickSeconds = 1; R.Lifetime = 0;
        const auto Bound = Equip(TEXT("Bound"),R,0,true); TestEqual(TEXT("穿即建绑定"),FY01DamageNextTestAccess::Products(D),1);
        R.Trigger = EY01DamageTrigger::Periodic; R.Product = EY01DamageProduct::IndependentArea; R.Lifetime = 10;
        const auto Free = Equip(TEXT("Free"),R,1); Pump(3);
        TestTrue(TEXT("卸绑定"),W.Send(W.Request(Bound,0,EY01ItemOperation::Unequip)).IsOk());
        TestEqual(TEXT("只移绑定独立保留"),FY01DamageNextTestAccess::Products(D),1); TestEqual(TEXT("不保留装备攻击"),W.Attack(),30.f);
        TestTrue(TEXT("卸独立来源"),W.Send(W.Request(Free,1,EY01ItemOperation::Unequip)).IsOk());
        TestEqual(TEXT("独立仍存在"),FY01DamageNextTestAccess::Products(D),1);
        FY01PreparedSource Plan; FY01SourceTransaction Tx;
        TestTrue(TEXT("空属性来源可Prepare"),W.P->GetSourceEffects()->PrepareSource(FY01DataCatalog(W.Data.Tables),EY01RuntimeSourceKind::Equipment,TEXT("Empty"),{},Plan).IsOk());
        TestTrue(TEXT("暂存"),W.P->GetSourceEffects()->StageSource(Plan,Tx).IsOk()); const float Before=HP(M); Pump(4);
        TestEqual(TEXT("Stage禁止伤害"),HP(M),Before); W.P->GetSourceEffects()->RollbackSource(Tx);
        TestEqual(TEXT("回滚不添加产物"),FY01DamageNextTestAccess::Products(D),1);
    }
    else if (Case == TEXT("SessionAndReentry"))
    {
        auto* A=Spawn(100); auto* B=Spawn(200); R.TargetCount=2; R.FixedDamage=10000; Equip(TEXT("Reentry"),R);
        A->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda([&](const FOnAttributeChangeData& Change)
        { if(Change.NewValue==0) { Pump(3); W.ASC()->InvalidateCombatSession(); } });
        Pump(3); TestEqual(TEXT("首目标一次致死"),HP(A),0.f); TestEqual(TEXT("死亡回调切区后立即停止余目标"),HP(B),10000.f);
        TestEqual(TEXT("切区清本人产物"),FY01DamageNextTestAccess::Products(D),0);
        auto* Other=W.Player()->GetPlayerState<AY01PlayerState>();
        TestEqual(TEXT("另一个玩家管理器独立"),FY01DamageNextTestAccess::Products(Other->GetDamageEffects()),0);
        Pump(4); TestEqual(TEXT("新会话首次重等"),HP(B),10000.f);
        W.P->GetSourceEffects()->EndSession(); Pump(100); TestEqual(TEXT("结束不再发动"),HP(B),10000.f);
    }
    else if (Case == TEXT("KillQueue"))
    {
        if (!TestTrue(TEXT("启用真实成长队列"), W.P->GetGrowth()->Initialize())) { return false; }
        auto* Kills = W.World->GetSubsystem<UY01KillRewards>();
        auto* A = Spawn(100); auto* B = Spawn(200);
        TestTrue(TEXT("登记两名真实目标"), Kills->RegisterMonster(A).IsValid() && Kills->RegisterMonster(B).IsValid());
        const auto Growth = W.Grant();
        TestTrue(TEXT("成长装备入包"), W.Add({Growth}).IsOk());
        TestTrue(TEXT("成长装备有效"), W.Send(W.Request(Growth.ItemId, 1)).IsOk());
        R.TargetCount = 2; R.FixedDamage = 10000; Equip(TEXT("KillQueue"), R);
        TArray<FY01KillEvent> Events;
        const auto Hook = Kills->OnEffectiveKill.AddLambda([&](const FY01KillEvent& Event)
        { TestFalse(TEXT("释放共同锁后消费奖励"), W.L()->IsBusy()); Events.Add(Event); Pump(3); });
        ON_SCOPE_EXIT { Kills->OnEffectiveKill.Remove(Hook); };
        Pump(3);
        TestEqual(TEXT("第一目标真实致死"), HP(A), 0.f);
        TestEqual(TEXT("待消费成长不阻断同次第二目标"), HP(B), 0.f);
        TestEqual(TEXT("两次合法击杀各发一次"), Events.Num(), 2);
        TestEqual(TEXT("成长沿原队列各累计3"), W.Attack(), 46.f);
        TestFalse(TEXT("释放后队列已消费"), Kills->HasPending(W.P));
        Pump(3); Pump(6); Kills->Drain();
        TestEqual(TEXT("重入及尸体不重复奖励"), Events.Num(), 2);
        TestEqual(TEXT("重入不重复成长"), W.Attack(), 46.f);
    }
    else if (Case == TEXT("CatalogWire"))
    {
        Equip(TEXT("WireDamage"),R); FY01CommittedView V; TestTrue(TEXT("完整视图"),W.L()->ReadCommittedView(V));
        if (!TestFalse(TEXT("视图含真实物品"), V.Items.IsEmpty())) { return false; }
        if (!TestEqual(TEXT("伤害快照进入编码视图"),V.Items.Last().DamageEffects.Num(),1)) { return false; }
        FY01ViewSender Sender; FY01ViewWire Wire; FY01ViewReceiver Receiver; FY01ViewAck Ack; FString Error;
        TestTrue(TEXT("新版本编码"),Sender.Offer(V,Wire,Error));
        while(!Wire.Items.IsEmpty()) { if(!Receiver.Receive(Wire,Ack,Error)||!Sender.Acknowledge(Ack,Wire,Error)){AddError(Error);return false;} }
        TestEqual(TEXT("接收伤害身份"),Receiver.Get().Items.Last().DamageEffects[0].EffectId,FName(TEXT("WireDamage")));
        V.Items.Last().DamageEffects[0].Config.TickSeconds=-1; FY01ViewSender Bad;
        TestFalse(TEXT("非法伤害快照不能编码"),Bad.Offer(V,Wire,Error));
        auto* Profile = NewObject<UY01R36Profile>();
        Profile->Character = W.Data.Ref(K::Character); Profile->OrderedWaves = W.Data.Waves();
        Profile->ContentEquipment = {W.Data.Ref(K::Equipment, TEXT("WireDamage"))};
        FY01SkillRow Skill; Skill.DisplayName = FText::FromString(TEXT("原生伤害书")); Skill.Ability = UY01DamageAbility::StaticClass();
        auto Active = R; Active.Trigger = EY01DamageTrigger::Active; W.Data.Add(K::DamageEffect, Active, TEXT("ProfileActive"));
        Skill.DamageEffectIds = {W.Data.Ref(K::DamageEffect, TEXT("ProfileActive"))}; W.Data.Add(K::Skill, Skill, TEXT("ProfileBook"));
        Profile->ContentSkills = {W.Data.Ref(K::Skill, TEXT("ProfileBook"))};
        for (const auto& Pair : W.Data.Tables) { Profile->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
        const auto ProfileResult = Profile->Validate();
        for (const auto& E : ProfileResult.Errors) { AddInfo(E.Field + TEXT(": ") + E.Message); }
        TestTrue(TEXT("Profile真实入口接受伤害only装备与原生主动书"), ProfileResult.IsValid());
        R.PeriodSeconds=0; W.Data.Add(K::DamageEffect,R,TEXT("WireDamage"));
        const auto Validation=FY01DataCatalog(W.Data.Tables).Validate();
        TestTrue(TEXT("目录定位非法伤害行"),Validation.Errors.ContainsByPredicate([](const auto& E){return E.Source.Type==K::DamageEffect;}));
    }
    return true;
}

#if Y01_LOADOUT_PRESENT
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36DevourTest, "Y01.Framework.R36.Loadout.Devour",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36DevourTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Success"), TEXT("NotRed"), TEXT("EmptySlot"), TEXT("StalePill"), TEXT("StaleSlot"), TEXT("Dead")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36DevourTest::RunTest(const FString& Case)
{
    using E = EY01ItemError;
    FLoadoutWorld F; F.P->SetOwner(F.PC);
    FY01ConsumableRow PillRow; PillRow.DisplayName = FText::FromString(TEXT("吞噬丹")); F.Data.Add(K::Consumable, PillRow, TEXT("Pill"));
    for (const TCHAR* Name : {TEXT("Red"), TEXT("Red2")})
    { F.Equipment(Name, {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Ten"))}); F.Data.Row<FY01EquipmentRow>(K::Equipment, Name).Quality = EY01Quality::Red; }
    if (!TestTrue(TEXT("真实库存会话"), F.Init().IsOk())) { return false; }
    TestTrue(TEXT("真实Mode进入Preparing"), F.Mode->TryStartNextWave());
    const auto A = F.Grant(TEXT("Red")), B = F.Grant(TEXT("Red")), C = F.Grant(TEXT("Sword")), D = F.Grant(TEXT("Red2"));
    if (!TestTrue(TEXT("丹药与装备入包"), F.Add({F.Grant(TEXT("Pill"), K::Consumable), F.Grant(TEXT("Pill"), K::Consumable), A, B, C, D}).IsOk())) { return false; }
    auto Pill = [&]() { for (const auto& I : F.View().Items) { if (I.Definition.Type == K::Consumable) { return I; } } return FY01ItemInstance(); };
    TestEqual(TEXT("丹药叠为一格两颗"), Pill().Count, 2);
    auto Devour = [&](int32 Slot) { return F.L()->SubmitRequest(F.PC, F.Request(Pill().ItemId, Slot, EY01ItemOperation::Devour)); };
    auto Unchanged = [&](const FY01LoadoutSnapshot& Before, float Attack)
    {
        const auto After = F.View();
        TestEqual(TEXT("拒绝不推进库存版本"), After.Revision, Before.Revision);
        TestEqual(TEXT("拒绝不消耗丹药"), Pill().Count, 2);
        TestEqual(TEXT("拒绝不产生已吞噬"), After.Devoured.Num(), 0);
        TestTrue(TEXT("拒绝不改变装备栏"), After.EquipmentSlots == Before.EquipmentSlots);
        TestEqual(TEXT("拒绝不改变攻击"), F.Attack(), Attack);
    };
    if (!TestTrue(TEXT("红装入槽"), F.Send(F.Request(A.ItemId, 0)).IsOk())) { return false; }
    TestEqual(TEXT("红装生效"), F.Attack(), 40.f);
    if (Case == TEXT("NotRed"))
    {
        TestTrue(TEXT("绿装入槽"), F.Send(F.Request(C.ItemId, 1)).IsOk()); const auto Before = F.View();
        TestEqual(TEXT("非红装拒绝"), Devour(1).Code, E::InvalidDefinition); Unchanged(Before, 50.f); return true;
    }
    if (Case == TEXT("EmptySlot"))
    { const auto Before = F.View(); TestEqual(TEXT("空槽拒绝"), Devour(2).Code, E::StaleSlot); Unchanged(Before, 40.f); return true; }
    if (Case == TEXT("StalePill"))
    {
        const auto Before = F.View(); auto R = F.Request(Pill().ItemId, 0, EY01ItemOperation::Devour); ++R.ExpectedEntityRevision;
        TestEqual(TEXT("过期丹药拒绝"), F.L()->SubmitRequest(F.PC, R).Code, E::StaleEntity); Unchanged(Before, 40.f); return true;
    }
    if (Case == TEXT("StaleSlot"))
    {
        const auto Before = F.View(); auto R = F.Request(Pill().ItemId, 0, EY01ItemOperation::Devour); ++R.ExpectedSlotEntityRevision;
        TestEqual(TEXT("过期目标拒绝"), F.L()->SubmitRequest(F.PC, R).Code, E::StaleSlot); Unchanged(Before, 40.f); return true;
    }
    if (Case == TEXT("Dead"))
    {
        F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); const auto Before = F.View();
        TestFalse(TEXT("死亡拒绝吞噬"), Devour(0).IsOk()); Unchanged(Before, 40.f); return true;
    }
    if (!TestTrue(TEXT("吞噬成功"), Devour(0).IsOk())) { return false; }
    auto V = F.View();
    TestFalse(TEXT("目标槽清空"), V.EquipmentSlots[0].IsValid());
    TestFalse(TEXT("目标装备移出注册表"), V.Items.ContainsByPredicate([&](const auto& I) { return I.ItemId == A.ItemId; }));
    TestEqual(TEXT("只消耗一颗丹药"), Pill().Count, 1);
    if (TestEqual(TEXT("已吞噬一件"), V.Devoured.Num(), 1)) { TestEqual(TEXT("已吞噬保留定义"), V.Devoured[0].Definition.RowName, FName(TEXT("Red"))); }
    TestEqual(TEXT("吞噬后效果常驻"), F.Attack(), 40.f);
    TestTrue(TEXT("同定义另一件可再装备"), F.Send(F.Request(B.ItemId, 0)).IsOk());
    TestEqual(TEXT("再装备独立叠加"), F.Attack(), 50.f);
    TestEqual(TEXT("同定义只能吞噬一次"), Devour(0).Code, E::DuplicateItem);
    TestEqual(TEXT("重复拒绝不消耗"), Pill().Count, 1);
    TestTrue(TEXT("卸下再装备件"), F.Send(F.Request(B.ItemId, 0, EY01ItemOperation::Unequip)).IsOk());
    TestEqual(TEXT("卸下不影响已吞噬来源"), F.Attack(), 40.f);
    TestTrue(TEXT("另一红装入槽"), F.Send(F.Request(D.ItemId, 1)).IsOk());
    TestTrue(TEXT("最后一颗丹药吞噬另一定义"), Devour(1).IsOk());
    V = F.View();
    TestFalse(TEXT("最后一颗用尽后移除格子"), V.Items.ContainsByPredicate([](const auto& I) { return I.Definition.Type == K::Consumable; }));
    TestEqual(TEXT("已吞噬两件"), V.Devoured.Num(), 2);
    TestEqual(TEXT("两件已吞噬同时生效"), F.Attack(), 50.f);
    return true;
}
#endif
#endif
