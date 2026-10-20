// R37第三期定向测试：时间与普攻成长触发（CombatTime/BasicAttack/EffectiveKill × 玩家四属性与实例攻击）。
// 夹具沿用 Y01R37CritShieldTests 的真实权威 AY01GameMode 骨架（纯内存19表 + TryConfigureMatch +
// LoadoutComponent/GrowthComponent Initialize + TryStartNextWave + 0.01秒步进Tick + SpawnMonster/HitPlayer），
// 差异：
// 1. 普通波准备0.02秒即真实进入Battle（时间成长只在Battle阶段计时；普攻成长Ended前任何阶段都结算），
//    Boss行同步放大为长挂起满足DataCatalog跨行BossPreparation校验；
// 2. 波次默认怪物行弱化为零攻击/零射程/零移速/无技能木桩，杜绝环境伤害干扰成长数值；
// 3. 显式启用玩家成长会话（UY01GrowthComponent::Initialize）；击杀目标由测试显式登记进KillRewards。
// 实现语义披露（以真实行为为准）：
// a. 击杀批只结算 EffectiveKill 规则（Apply 薄壳按触发器过滤；用例1尾部有穿时间成长件击杀的隔离断言），
//    时间/普攻成长仅由 SettleTick 按批结算；用例4的击杀段另换 KillGear 仅为账本断言清晰；
// b. 玩家侧成长写入账本后随会话保留（移除来源不追扣），卸下装备只暂停计时不清退已得——任务草案中
//    “卸下时间成长装备后上限回落30”与该实现语义不符，测试按真实行为断言保留，并另行通过
//    静态AttributeFlat→MaxHealth来源的真实移除链验证“上限回落→当前血钳到新上限”的降钳制。
// c. 生命上限变动同步当前生命（R37四期后新规则，用户拍板“所有来源都同步加”）：上限升且当前血>0时
//    加同差值并封顶新上限（含装卸装备，装卸=先加后钳回），上限降仍钳到新上限，死亡态(0血)升上限不复活。
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01PlayerController.h"
#include "Player/Y01Character.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01GrowthComponent.h"
#include "Player/Y01AttributeResolver.h"
#include "Combat/Y01Combat.h"
#include "Combat/Y01Monster.h"
#include "Combat/Y01DamageEvents.h"
#include "Match/Y01GameFlow.h"
#include "Match/Y01KillRewards.h"
#include "Y01MatchFixture.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
using K = EY01TableKind;

// 单局测试世界：真实GameMode、真实PlayerState初始化链、真实组件节拍；析构即完整清理。
struct FR37GrowthWorld
{
    FY01MatchFixture Data;
    UWorld* World = nullptr;
    AY01GameMode* Mode = nullptr;
    AY01PlayerController* PC = nullptr;
    AY01PlayerState* P = nullptr;
    AY01Monster* Monster = nullptr;
    FGuid Match = FGuid::NewGuid();
    FString Error;
    FString SpawnError;
    bool bConfigured = false;
    FY01ItemResult InitResult;
    bool bGrowthEnabled = false;
    bool bWaveStarted = false;

    FR37GrowthWorld()
    {
        auto& C = Data.Row<FY01CharacterRow>(K::Character);
        C.InitialEquipment.Reset(); C.InitialSkills.Reset();
        C.Health = 100; C.AttackDamage = 20; C.Range = 1000; C.Armor = 0; C.HealthRegen = 0;
        C.Strength = 0; C.Agility = 0; C.Wisdom = 0;
        // 普通波准备0.02秒：波次开启后数个0.01步内真实进入Battle；Boss行须严格长于普通行（跨行校验）。
        auto& W = Data.Row<FY01WaveRow>(K::Wave);
        W.PreparationSeconds = 0.02f; W.Spawns[0].DelaySeconds = 0;
        Data.Row<FY01WaveRow>(K::Wave, TEXT("Boss")).PreparationSeconds = 1000001.f;
        // 波次默认怪物弱化为无害木桩：EnterBattle会按波次生成2只，零射程零移速无技能，绝不干扰成长数值。
        auto& M = Data.Row<FY01MonsterRow>(K::Monster);
        M.AttackDamage = 0; M.Range = 0; M.MoveSpeed = 0; M.DetectionRadius = 0; M.Skills.Reset();
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01GameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01GameMode>();
        PC = SpawnPlayer(); P = PC->GetPlayerState<AY01PlayerState>(); P->SetOwner(PC);
        // MatchRules/Character经TryConfigureMatch真实路径注入：三维基础值与生命在InitializePlayer中初始化。
        bConfigured = Mode->TryConfigureMatch(FY01DataCatalog(Data.Tables), Data.Waves(), FY01MatchFixture::Ref(K::Character), Error);
        InitResult = P->GetLoadout()->Initialize(Match, 1);
        bGrowthEnabled = P->GetGrowth()->Initialize();
        bWaveStarted = Mode->TryStartNextWave();
        Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay();
    }
    ~FR37GrowthWorld()
    { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }

    AY01PlayerController* SpawnPlayer()
    {
        auto* C = World->SpawnActor<AY01PlayerController>(); auto* S = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(0, 0, 100), FRotator::ZeroRotator);
        C->PlayerState = S; C->Possess(Pawn); S->GetCombatASC()->InitAbilityActorInfo(S, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(S); return C;
    }
    UY01CombatAbilitySystem* ASC() const { return P->GetCombatASC(); }
    UY01GrowthComponent* Growth() const { return P->GetGrowth(); }
    const FY01EffectiveKillGrowthLedger& Ledger() const { return P->GetGrowth()->GetLedger(); }
    float PlayerHealth() const { return ASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()); }
    float PlayerMaxHealthBase() const { return ASC()->GetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute()); }
    float PlayerAttack() const { return ASC()->GetUnitStats().AttackDamage; }
    float MonsterHealth() const
    { return Monster ? Monster->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) : 0.f; }
    EY01MatchPhase Phase() const { return Mode->GetFlowState()->GetWaveSnapshot().Phase; }
    bool InBattle() const { return Phase() == EY01MatchPhase::Battle; }
    // 按0.01秒推进世界：驱动TimerManager（准备期计时器）与全部组件Tick（成长组件1秒间隔结算）。
    void Tick(float Seconds)
    { const int32 Steps = FMath::RoundToInt(Seconds * 100); for (int32 I = 0; I < Steps; ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .01f); } }
    bool TickUntilBattle()
    {
        for (int32 I = 0; I < 40 && !InBattle(); ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .01f); }
        return InBattle();
    }

    FY01ItemGrant Grant(FName Name, K Type = K::Equipment) const
    { FY01ItemGrant G; G.ItemId = FGuid::NewGuid(); G.Definition = FY01MatchFixture::Ref(Type, Name); return G; }
    FY01ItemResult Add(const TArray<FY01ItemGrant>& Grants) { return P->GetLoadout()->AddItems(FY01DataCatalog(Data.Tables), Grants); }
    FY01LoadoutSnapshot View() const { FY01LoadoutSnapshot V; P->GetLoadout()->Observe(V); return V; }
    FY01ItemInstance Item(FGuid Id) const { FY01ItemInstance I; P->GetLoadout()->FindItem(Id, I); return I; }
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
    FY01ItemResult Send(const FY01ItemRequest& R)
    { return R.Operation == EY01ItemOperation::Unequip ? P->GetLoadout()->TryUnequip(PC, R) : P->GetLoadout()->TryEquip(PC, R); }
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
    FY01ItemResult Learn(const FY01ItemRequest& R) { return P->GetLoadout()->PrepareLearn(PC, R); }
    // 注入成长规则行（Trigger/Target/Attribute/Delta），行名即实例账本RuleId。
    void AddGrowthRule(FName Name, EY01GrowthTrigger Trigger, EY01GrowthTarget Target, EY01GrowthAttribute Attribute, double Delta)
    {
        FY01GrowthRuleRow G; G.Trigger = Trigger; G.Target = Target; G.Attribute = Attribute; G.Delta = Delta;
        Data.Add(K::GrowthRule, G, Name);
    }
    void AddFlatSourceEffect(FName Name, EY01Attribute Target, double Amount)
    {
        FY01SourceEffectRow E; E.Kind = EY01SourceEffectKind::AttributeFlat; E.TargetAttribute = Target; E.Amount = Amount;
        Data.Add(K::SourceEffect, E, Name);
    }
    // 注入仅携带成长规则的装备并入包穿戴到指定槽（实例成长只支持攻击力，玩家侧支持四属性）。
    FY01ItemResult EquipGrowth(FName Name, const TArray<FY01DataRef>& GrowthRefs, FGuid& OutItemId, int32 Slot = 0)
    {
        FY01EquipmentRow E; E.DisplayName = FText::FromName(Name); E.GrowthRuleIds = GrowthRefs;
        Data.Add(K::Equipment, E, Name);
        const auto G = Grant(Name);
        const auto Added = Add({G});
        if (!Added.IsOk()) { return Added; }
        OutItemId = G.ItemId;
        return Send(Request(G.ItemId, Slot));
    }
    // 注入仅携带来源效果的装备（用于静态上限回落→降钳制链）。
    FY01ItemResult EquipStatic(FName Name, const TArray<FY01DataRef>& EffectRefs, FGuid& OutItemId, int32 Slot = 0)
    {
        FY01EquipmentRow E; E.DisplayName = FText::FromName(Name); E.EffectIds = EffectRefs;
        Data.Add(K::Equipment, E, Name);
        const auto G = Grant(Name);
        const auto Added = Add({G});
        if (!Added.IsOk()) { return Added; }
        OutItemId = G.ItemId;
        return Send(Request(G.ItemId, Slot));
    }
    // 注入被动成长技能书（无Ability、无来源效果、仅玩家侧成长规则）并入包学习到槽0。
    FY01ItemResult LearnGrowthBook(FName Name, const TArray<FY01DataRef>& GrowthRefs)
    {
        FY01SkillRow S; S.DisplayName = FText::FromName(Name);
        S.Type = EY01SkillType::Passive; S.GrowthRuleIds = GrowthRefs;
        Data.Add(K::Skill, S, Name);
        const auto G = Grant(Name, K::Skill);
        const auto Added = Add({G});
        if (!Added.IsOk()) { return Added; }
        return Learn(LearnRequest(G.ItemId, 0));
    }
    // 生成并初始化一只安全木桩：血1000、零攻击、大射程（供反向打玩家）、不移动、不回血、无技能、不掉落。
    AY01Monster* SpawnMonster(FVector Location)
    {
        FY01MonsterRow R; R.Health = 1000; R.AttackDamage = 0; R.Range = 5000; R.MoveSpeed = 0;
        R.Armor = 0; R.HealthRegen = 0; R.Skills.Reset(); R.DropReward = Data.Ref(K::RewardPackage);
        Data.Add(K::Monster, R, TEXT("R37Target"));
        auto* M = World->SpawnActor<AY01Monster>(Location, FRotator::ZeroRotator);
        Monster = M;
        if (M) { M->TryInitializeMonster(FY01DataCatalog(Data.Tables), Data.Ref(K::Monster, TEXT("R37Target")), SpawnError); }
        if (auto* Move = M ? M->GetCharacterMovement() : nullptr) { Move->DisableMovement(); }
        return M;
    }
    // 玩家以真实伤害GE打目标怪物（走共享伤害管线：减甲→扣血→死亡/击杀事件）。
    void Damage(AY01Monster* Target, float Amount)
    {
        auto Spec = ASC()->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, ASC()->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, Amount);
        ASC()->ApplyGameplayEffectSpecToTarget(*Spec.Data, Target->GetCombatASC());
    }
    // 登记击杀目标并致死：返回致死一击是否使目标归零（真实Health执行生成KillEvent并由成长组件消费）。
    bool KillTarget(AY01Monster* Target)
    {
        auto* Kills = World->GetSubsystem<UY01KillRewards>();
        if (!Kills || !Kills->RegisterMonster(Target).IsValid()) { return false; }
        Damage(Target, 99999.f);
        return Target->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) <= 0.f;
    }
    // 怪物以真实伤害GE打玩家（走共享伤害管线：减甲→扣血）。
    void HitPlayer(float Damage)
    {
        auto* Source = Monster->GetCombatASC();
        auto Spec = Source->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, Source->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, Damage);
        Source->ApplyGameplayEffectSpecToTarget(*Spec.Data, ASC());
    }
};
}

// ── 用例1：CombatTime Target=Player 力量时间成长 ────────────────────────────────
// Delta=2/秒：Battle阶段组件按1秒间隔结算（1.01/2.01/3.01s各Elapsed≈1秒）→ 步进3.2秒力量+6；
// 卸下暂停该件计时且保留已得；重新装备继续累计；离开Battle（完成本波→长准备Boss波回Preparing）后不计时。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GrowthCombatTimeTest, "Y01.R37.Growth.CombatTime",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GrowthCombatTimeTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FR37GrowthWorld F;
    if (!TestTrue(FString::Printf(TEXT("真实权威Mode目录配置成功 Error=%s"), *F.Error),
        F.bConfigured && F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    if (!TestTrue(TEXT("库存初始化成功"), F.InitResult.IsOk())) { return false; }
    if (!TestTrue(TEXT("玩家成长会话启用"), F.bGrowthEnabled)) { return false; }
    if (!TestTrue(TEXT("波次进入准备阶段"), F.bWaveStarted)) { return false; }

    // 经内存表注入装备行GrowthRuleIds+成长规则行：CombatTime/Player/Strength/Delta=2。
    F.AddGrowthRule(TEXT("TimeStrength"), EY01GrowthTrigger::CombatTime, EY01GrowthTarget::Player, EY01GrowthAttribute::Strength, 2);
    FGuid ItemId;
    if (!TestTrue(TEXT("时间成长装备入包并穿戴"),
        F.EquipGrowth(TEXT("TimeGear"), {F.Data.Ref(K::GrowthRule, TEXT("TimeStrength"))}, ItemId).IsOk())) { return false; }

    if (!TestTrue(TEXT("真实进入Battle阶段"), F.TickUntilBattle())) { return false; }
    TestEqual(TEXT("进战前力量成长为零"), F.Ledger().Strength, 0.);
    // 步进3.2秒：三次结算各Elapsed≈1秒×Delta2 → 账本与ASC三维各+6（保留小数累计语义，容忍秒内节拍抖动）。
    F.Tick(3.2f);
    TestTrue(TEXT("账本力量累计≈6"), FMath::IsNearlyEqual(F.Ledger().Strength, 6., 0.5));
    TestTrue(TEXT("ASC力量增长≈6"), FMath::IsNearlyEqual(F.ASC()->GetUnitStats().Strength, 6.f, 0.5f));

    // 卸下装备：来源移出Records，该件计时暂停且已得保留。
    if (!TestTrue(TEXT("卸下成功"), F.Send(F.Request(ItemId, 0, EY01ItemOperation::Unequip)).IsOk())) { return false; }
    F.Tick(2.05f);
    TestTrue(TEXT("卸下后步进不再累计"), FMath::IsNearlyEqual(F.Ledger().Strength, 6., 0.5));
    TestTrue(TEXT("卸下后ASC三维保持已得"), FMath::IsNearlyEqual(F.ASC()->GetUnitStats().Strength, 6.f, 0.5f));

    // 重新装备：已得保留并继续累计（再两次结算+4→10）。
    if (!TestTrue(TEXT("重新装备成功"), F.Send(F.Request(ItemId, 0, EY01ItemOperation::Equip)).IsOk())) { return false; }
    F.Tick(2.05f);
    const double AfterReequip = F.Ledger().Strength;
    TestTrue(TEXT("重新装备后继续累计≈10"), FMath::IsNearlyEqual(AfterReequip, 10., 0.5));

    // 击杀隔离：穿时间成长件时击杀只走 EffectiveKill 批，时间规则不因击杀被整额结算（无Tick介入前严格不变）。
    auto* Victim = F.SpawnMonster(FVector(400, 0, 100));
    if (!TestTrue(TEXT("隔离击杀目标初始化成功"), Victim != nullptr && F.SpawnError.IsEmpty())) { return false; }
    if (!TestTrue(TEXT("隔离击杀完成"), F.KillTarget(Victim))) { return false; }
    TestTrue(TEXT("击杀不结算时间成长"), FMath::IsNearlyEqual(F.Ledger().Strength, AfterReequip, 1.e-6));

    // 离开Battle：完成本波（PendingSpawns已为0）→开启长准备Boss波回Preparing；非Battle阶段不计时。
    if (!TestTrue(TEXT("完成波次"), F.Mode->TryCompleteWave())) { return false; }
    if (!TestTrue(TEXT("开启Boss波进入Preparing"), F.Mode->TryStartNextWave())) { return false; }
    if (!TestEqual(TEXT("当前阶段为Preparing"), F.Phase(), EY01MatchPhase::Preparing)) { return false; }
    F.Tick(3.05f);
    TestTrue(TEXT("离战后数值保持不变"), F.Ledger().Strength == AfterReequip);
    return true;
}

// ── 用例2：BasicAttack Target=EquipmentInstance 实例攻击成长 ────────────────────
// Delta=1：3次真实普攻发动（各20伤害）→ NotifyBasicAttack×3 → 一个Tick周期结算一批：
// 批次增量=3次×Delta1 全额计入实例Accumulated≈3；AppliedCount为批次应用计次（一次结算应用一批计1）；
// 实例攻击成长经来源GE生效（当前攻击20→23）；玩家死亡后实例数值保留。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GrowthBasicAttackTest, "Y01.R37.Growth.BasicAttack",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GrowthBasicAttackTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FR37GrowthWorld F;
    if (!TestTrue(FString::Printf(TEXT("真实权威Mode目录配置成功 Error=%s"), *F.Error),
        F.bConfigured && F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    if (!TestTrue(TEXT("库存初始化成功"), F.InitResult.IsOk())) { return false; }
    if (!TestTrue(TEXT("玩家成长会话启用"), F.bGrowthEnabled)) { return false; }
    if (!TestTrue(TEXT("波次进入准备阶段"), F.bWaveStarted)) { return false; }

    F.AddGrowthRule(TEXT("HitAttack"), EY01GrowthTrigger::BasicAttack, EY01GrowthTarget::EquipmentInstance, EY01GrowthAttribute::AttackDamage, 1);
    FGuid ItemId;
    if (!TestTrue(TEXT("普攻成长装备入包并穿戴"),
        F.EquipGrowth(TEXT("HitGear"), {F.Data.Ref(K::GrowthRule, TEXT("HitAttack"))}, ItemId).IsOk())) { return false; }
    if (!TestTrue(TEXT("真实进入Battle阶段"), F.TickUntilBattle())) { return false; }

    auto* M = F.SpawnMonster(FVector(400, 0, 100));
    if (!TestTrue(TEXT("怪物初始化成功"), M != nullptr && F.SpawnError.IsEmpty())) { return false; }
    TestEqual(TEXT("怪物初始生命1000"), F.MonsterHealth(), 1000.f);

    // 真实普攻3次：每次发动同步造成20伤害并NotifyBasicAttack计数（攻击力20、怪1000血不致死）。
    TestTrue(TEXT("普攻第1次激活"), F.ASC()->SetCombatTarget(M) && F.ASC()->TryActivateBasicAttack());
    TestTrue(TEXT("普攻第2次激活"), F.ASC()->TryActivateBasicAttack());
    TestTrue(TEXT("普攻第3次激活"), F.ASC()->TryActivateBasicAttack());
    TestEqual(TEXT("三次普攻扣怪60"), F.MonsterHealth(), 940.f);

    // 等待至多一个成长Tick周期（结算点1.01s）：批3次×Delta1 → 实例累计3。
    F.Tick(1.2f);
    const auto Inst = F.Item(ItemId);
    const auto* Entry = Inst.Growth.FindByPredicate([](const auto& V) { return V.RuleId == FName(TEXT("HitAttack")); });
    if (!TestNotNull(TEXT("实例携带成长账本"), Entry)) { return false; }
    TestTrue(TEXT("实例Accumulated≈3"), FMath::IsNearlyEqual(Entry->Accumulated, 3., 1e-6));
    TestEqual(TEXT("实例单批结算计次=1"), Entry->AppliedCount, uint64(1));
    // 实例攻击成长经来源GE生效：当前攻击=20基础+3成长。
    TestTrue(TEXT("ASC攻击增长3"), FMath::IsNearlyEqual(F.PlayerAttack(), 23.f, 1e-4f));

    // 死亡后数值保留：致死后继续步进一个结算周期，实例账本不清空不扣减。
    F.HitPlayer(500.f);
    TestEqual(TEXT("玩家死亡生命为0"), F.PlayerHealth(), 0.f);
    F.Tick(1.2f);
    const auto After = F.Item(ItemId);
    const auto* Kept = After.Growth.FindByPredicate([](const auto& V) { return V.RuleId == FName(TEXT("HitAttack")); });
    if (!TestNotNull(TEXT("死亡后实例账本仍在"), Kept)) { return false; }
    TestTrue(TEXT("死亡不清空实例累计"), FMath::IsNearlyEqual(Kept->Accumulated, 3., 1e-6));
    TestEqual(TEXT("死亡不扣实例计次"), Kept->AppliedCount, uint64(1));
    TestTrue(TEXT("死亡不扣实例攻击"), FMath::IsNearlyEqual(F.PlayerAttack(), 23.f, 1e-4f));
    return true;
}

// 普攻成长开局即结算：Preparing（开波前/练功房）普攻当秒入账；时间成长仍只在Battle计时。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GrowthBasicAttackPreparingTest, "Y01.R37.Growth.BasicAttackPreparing",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GrowthBasicAttackPreparingTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FR37GrowthWorld F;
    if (!TestTrue(FString::Printf(TEXT("真实权威Mode目录配置成功 Error=%s"), *F.Error),
        F.bConfigured && F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    if (!TestTrue(TEXT("库存初始化成功"), F.InitResult.IsOk())) { return false; }
    if (!TestTrue(TEXT("玩家成长会话启用"), F.bGrowthEnabled)) { return false; }
    if (!TestTrue(TEXT("真实进入Battle阶段"), F.TickUntilBattle())) { return false; }
    if (!TestTrue(TEXT("完成波次"), F.Mode->TryCompleteWave())) { return false; }
    if (!TestTrue(TEXT("开启Boss波进入长Preparing"), F.Mode->TryStartNextWave())) { return false; }
    if (!TestEqual(TEXT("当前阶段为Preparing"), F.Phase(), EY01MatchPhase::Preparing)) { return false; }

    F.AddGrowthRule(TEXT("HitAgi"), EY01GrowthTrigger::BasicAttack, EY01GrowthTarget::Player, EY01GrowthAttribute::Agility, .04);
    F.AddGrowthRule(TEXT("TimeStr"), EY01GrowthTrigger::CombatTime, EY01GrowthTarget::Player, EY01GrowthAttribute::Strength, 1);
    FGuid ItemId;
    if (!TestTrue(TEXT("普攻敏捷+时间力量装备入包并穿戴"), F.EquipGrowth(TEXT("PrepGear"),
        {F.Data.Ref(K::GrowthRule, TEXT("HitAgi")), F.Data.Ref(K::GrowthRule, TEXT("TimeStr"))}, ItemId).IsOk())) { return false; }
    auto* M = F.SpawnMonster(FVector(400, 0, 100));
    if (!TestTrue(TEXT("怪物初始化成功"), M != nullptr && F.SpawnError.IsEmpty())) { return false; }
    TestTrue(TEXT("普攻第1次激活"), F.ASC()->SetCombatTarget(M) && F.ASC()->TryActivateBasicAttack());
    TestTrue(TEXT("普攻第2次激活"), F.ASC()->TryActivateBasicAttack());
    TestTrue(TEXT("普攻第3次激活"), F.ASC()->TryActivateBasicAttack());
    F.Tick(1.2f);
    TestEqual(TEXT("仍处Preparing"), F.Phase(), EY01MatchPhase::Preparing);
    TestTrue(TEXT("Preparing普攻敏捷当秒入账≈0.12"), FMath::IsNearlyEqual(F.ASC()->GetUnitStats().Agility, .12f, 1e-4f));
    TestTrue(TEXT("账本敏捷≈0.12"), FMath::IsNearlyEqual(F.Ledger().Agility, .12, 1e-6));
    TestEqual(TEXT("Preparing不计时间成长"), F.ASC()->GetUnitStats().Strength, 0.f);
    return true;
}

// 槽内成长推进实体版本后，按成长前视图发出的卸下与替换仍代表同一意图；早于穿戴的版本仍拒绝。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GrowthEquippedIntentTest, "Y01.R37.Growth.EquippedGrowthKeepsIntent",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GrowthEquippedIntentTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FR37GrowthWorld F;
    if (!TestTrue(FString::Printf(TEXT("真实权威Mode目录配置成功 Error=%s"), *F.Error),
        F.bConfigured && F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    if (!TestTrue(TEXT("库存初始化成功"), F.InitResult.IsOk() && F.bGrowthEnabled && F.bWaveStarted)) { return false; }
    F.AddGrowthRule(TEXT("TimeAttack"), EY01GrowthTrigger::CombatTime, EY01GrowthTarget::EquipmentInstance, EY01GrowthAttribute::AttackDamage, 1);
    FGuid Blade;
    if (!TestTrue(TEXT("时间成长装备穿戴"), F.EquipGrowth(TEXT("TimeBlade"), {F.Data.Ref(K::GrowthRule, TEXT("TimeAttack"))}, Blade).IsOk())) { return false; }
    F.AddFlatSourceEffect(TEXT("PlainAttack"), EY01Attribute::AttackDamage, 1);
    FY01EquipmentRow Plain; Plain.DisplayName = FText::FromString(TEXT("Plain")); Plain.EffectIds = {F.Data.Ref(K::SourceEffect, TEXT("PlainAttack"))};
    F.Data.Add(K::Equipment, Plain, TEXT("Plain"));
    const auto Other = F.Grant(TEXT("Plain")); const auto Added = F.Add({Other});
    if (!TestTrue(FString::Printf(TEXT("替换件入包 %s"), *Added.Message), Added.IsOk())) { return false; }
    if (!TestTrue(TEXT("真实进入Battle阶段"), F.TickUntilBattle())) { return false; }
    auto Unequip = F.Request(Blade, 0, EY01ItemOperation::Unequip); const uint64 Seen = F.Item(Blade).EntityRevision;
    F.Tick(1.2f);
    if (!TestTrue(TEXT("槽内成长推进实体版本"), F.Item(Blade).EntityRevision > Seen)) { return false; }
    TestTrue(TEXT("成长前视图卸下成功"), F.Send(Unequip).IsOk());
    TestEqual(TEXT("已回背包"), F.Item(Blade).Location, EY01ItemLocation::Backpack);
    if (!TestTrue(TEXT("重新穿戴"), F.Send(F.Request(Blade, 0)).IsOk())) { return false; }
    auto Replace = F.Request(Other.ItemId, 0);
    F.Tick(1.2f);
    TestTrue(TEXT("成长前视图替换成功"), F.Send(Replace).IsOk());
    TestEqual(TEXT("替换件在槽"), F.Item(Other.ItemId).Location, EY01ItemLocation::EquipmentSlot);
    TestEqual(TEXT("成长件回包"), F.Item(Blade).Location, EY01ItemLocation::Backpack);
    auto Future = F.Request(Other.ItemId, 0, EY01ItemOperation::Unequip); ++Future.ExpectedEntityRevision;
    TestEqual(TEXT("未来版本仍拒绝"), F.Send(Future).Code, EY01ItemError::StaleEntity);
    return true;
}

// ── 用例3：CombatTime 玩家侧技能来源（练功房形态等价验证） ──────────────────────
// 时间成长只按 GameState.Phase==Battle 门控——练功房与主线共用Battle阶段，本用例经Skills来源枚举路径
// （被动成长技能书）验证Battle阶段步进3秒力量增长≈6>0，即“练功房不暂停计时”的最小可信等价断言。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GrowthCombatTimeTrainingRoomTest, "Y01.R37.Growth.CombatTimeTrainingRoom",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GrowthCombatTimeTrainingRoomTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FR37GrowthWorld F;
    if (!TestTrue(FString::Printf(TEXT("真实权威Mode目录配置成功 Error=%s"), *F.Error),
        F.bConfigured && F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    if (!TestTrue(TEXT("库存初始化成功"), F.InitResult.IsOk())) { return false; }
    if (!TestTrue(TEXT("玩家成长会话启用"), F.bGrowthEnabled)) { return false; }
    if (!TestTrue(TEXT("波次进入准备阶段"), F.bWaveStarted)) { return false; }

    F.AddGrowthRule(TEXT("TrainTime"), EY01GrowthTrigger::CombatTime, EY01GrowthTarget::Player, EY01GrowthAttribute::Strength, 2);
    if (!TestTrue(TEXT("被动成长技能书入包并学习到槽0"), F.LearnGrowthBook(TEXT("TrainBook"), {F.Data.Ref(K::GrowthRule, TEXT("TrainTime"))}).IsOk())) { return false; }
    if (!TestTrue(TEXT("真实进入Battle阶段"), F.TickUntilBattle())) { return false; }

    // 步进3.2秒：技能来源经Skills枚举入批，三次结算各Elapsed≈1秒×Delta2 → 力量+6（>0即时间未暂停）。
    const double Before = F.Ledger().Strength;
    F.Tick(3.2f);
    const double Growth = F.Ledger().Strength - Before;
    TestTrue(TEXT("Battle(练功房形态)步进3秒力量增长>0"), Growth > 0);
    TestTrue(TEXT("技能来源时间累计≈6"), FMath::IsNearlyEqual(Growth, 6., 0.5));
    TestTrue(TEXT("ASC力量同步增长≈6"), FMath::IsNearlyEqual(F.ASC()->GetUnitStats().Strength, 6.f, 0.5f));
    return true;
}

// ── 用例4：CombatTime四属性玩家成长 + 升同步加/降钳制 + EffectiveKill触发器通用 + 尸体关碰撞销毁 ──
// 同件挂四条CombatTime规则（Str1/Agi2/Wis3/Max10）：先受伤至70/100再进Battle步进3.2秒→
// 三维+3/+6/+9、上限130、当前血同步+30到100（升同步加）。卸下该件：玩家成长按真实语义保留（上限不回落，
// 见文件头披露b）。降钳制另经静态+100上限装备的真实移除链验证：装上上限230→当前血同步+100到200→
// 卸下上限回落130→当前血钳到130。最后换仅含EffectiveKill MaxHealth 5规则的装备击杀：上限再+5
// （当前血同步+5）；被击杀怪物尸体立即关碰撞并设3秒后销毁（步进3.5秒后actor已销毁）。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GrowthPlayerAttributesTest, "Y01.R37.Growth.PlayerAttributes",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GrowthPlayerAttributesTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FR37GrowthWorld F;
    if (!TestTrue(FString::Printf(TEXT("真实权威Mode目录配置成功 Error=%s"), *F.Error),
        F.bConfigured && F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    if (!TestTrue(TEXT("库存初始化成功"), F.InitResult.IsOk())) { return false; }
    if (!TestTrue(TEXT("玩家成长会话启用"), F.bGrowthEnabled)) { return false; }
    if (!TestTrue(TEXT("波次进入准备阶段"), F.bWaveStarted)) { return false; }

    F.AddGrowthRule(TEXT("QuadStr"), EY01GrowthTrigger::CombatTime, EY01GrowthTarget::Player, EY01GrowthAttribute::Strength, 1);
    F.AddGrowthRule(TEXT("QuadAgi"), EY01GrowthTrigger::CombatTime, EY01GrowthTarget::Player, EY01GrowthAttribute::Agility, 2);
    F.AddGrowthRule(TEXT("QuadWis"), EY01GrowthTrigger::CombatTime, EY01GrowthTarget::Player, EY01GrowthAttribute::Wisdom, 3);
    F.AddGrowthRule(TEXT("QuadMax"), EY01GrowthTrigger::CombatTime, EY01GrowthTarget::Player, EY01GrowthAttribute::MaxHealth, 10);
    F.AddGrowthRule(TEXT("KillMax"), EY01GrowthTrigger::EffectiveKill, EY01GrowthTarget::Player, EY01GrowthAttribute::MaxHealth, 5);
    FGuid ItemA;
    if (!TestTrue(TEXT("四属性时间成长装备入包并穿戴"), F.EquipGrowth(TEXT("QuadGear"),
        {F.Data.Ref(K::GrowthRule, TEXT("QuadStr")), F.Data.Ref(K::GrowthRule, TEXT("QuadAgi")),
         F.Data.Ref(K::GrowthRule, TEXT("QuadWis")), F.Data.Ref(K::GrowthRule, TEXT("QuadMax"))}, ItemA).IsOk())) { return false; }

    // 先受伤：70/100（当前血<上限），为“升不治疗”提供观察基线。
    auto* Dummy = F.SpawnMonster(FVector(400, 0, 100));
    if (!TestTrue(TEXT("伤害载体怪物初始化成功"), Dummy != nullptr && F.SpawnError.IsEmpty())) { return false; }
    F.HitPlayer(30.f);
    TestEqual(TEXT("受伤后当前血70"), F.PlayerHealth(), 70.f);

    if (!TestTrue(TEXT("真实进入Battle阶段"), F.TickUntilBattle())) { return false; }
    // 步进3.2秒：三次结算各Elapsed≈1秒 → Str+3/Agi+6/Wis+9/Max+30。
    F.Tick(3.2f);
    TestTrue(TEXT("力量增长≈3"), FMath::IsNearlyEqual(F.ASC()->GetUnitStats().Strength, 3.f, 0.5f));
    TestTrue(TEXT("敏捷增长≈6"), FMath::IsNearlyEqual(F.ASC()->GetUnitStats().Agility, 6.f, 0.5f));
    TestTrue(TEXT("智慧增长≈9"), FMath::IsNearlyEqual(F.ASC()->GetUnitStats().Wisdom, 9.f, 0.5f));
    TestTrue(TEXT("上限增长≈30"), FMath::IsNearlyEqual(F.PlayerMaxHealthBase(), 130.f, 0.5f));
    TestEqual(TEXT("升上限同步加：当前血70+30=100"), F.PlayerHealth(), 100.f);
    TestTrue(TEXT("账本上限累计≈30"), FMath::IsNearlyEqual(F.Ledger().MaxHealth, 30., 0.5));

    // 卸下时间成长件：按真实语义玩家成长保留（上限不回落），仅暂停计时。
    if (!TestTrue(TEXT("卸下时间成长件"), F.Send(F.Request(ItemA, 0, EY01ItemOperation::Unequip)).IsOk())) { return false; }
    F.Tick(1.05f);
    TestTrue(TEXT("卸下后上限按已得保留≈130"), FMath::IsNearlyEqual(F.PlayerMaxHealthBase(), 130.f, 0.5f));
    TestEqual(TEXT("卸下后当前血不变"), F.PlayerHealth(), 100.f);

    // 降钳制（真实移除链）：装上静态+100上限→上限230且当前血同步+100到200；卸下→上限回落130→当前血钳到130。
    F.AddFlatSourceEffect(TEXT("MaxHundred"), EY01Attribute::MaxHealth, 100);
    FGuid ItemB;
    if (!TestTrue(TEXT("静态上限装备入包并穿戴"),
        F.EquipStatic(TEXT("MaxGear"), {F.Data.Ref(K::SourceEffect, TEXT("MaxHundred"))}, ItemB).IsOk())) { return false; }
    TestTrue(TEXT("静态上限装配后上限≈230"), FMath::IsNearlyEqual(F.PlayerMaxHealthBase(), 230.f, 0.5f));
    TestEqual(TEXT("升上限同步加：当前血100+100=200"), F.PlayerHealth(), 200.f);
    if (!TestTrue(TEXT("卸下静态上限装备"), F.Send(F.Request(ItemB, 0, EY01ItemOperation::Unequip)).IsOk())) { return false; }
    TestTrue(TEXT("上限回落≈130"), FMath::IsNearlyEqual(F.PlayerMaxHealthBase(), 130.f, 0.5f));
    TestEqual(TEXT("降钳制：当前血钳到新上限130"), F.PlayerHealth(), 130.f);

    // EffectiveKill触发器通用：换上仅含击杀规则的装备，登记并击杀 → 上限再+5（当前血同步+5）。
    FGuid ItemC;
    if (!TestTrue(TEXT("击杀成长装备入包并穿戴"),
        F.EquipGrowth(TEXT("KillGear"), {F.Data.Ref(K::GrowthRule, TEXT("KillMax"))}, ItemC).IsOk())) { return false; }
    auto* Victim = F.SpawnMonster(FVector(-400, 0, 100));
    if (!TestTrue(TEXT("击杀目标初始化成功"), Victim != nullptr && F.SpawnError.IsEmpty())) { return false; }
    if (!TestTrue(TEXT("登记并击杀目标"), F.KillTarget(Victim))) { return false; }
    TestTrue(TEXT("击杀后上限≈135"), FMath::IsNearlyEqual(F.PlayerMaxHealthBase(), 135.f, 0.5f));
    TestEqual(TEXT("击杀升上限同步加：当前血135"), F.PlayerHealth(), 135.f);
    TestFalse(TEXT("尸体立即关闭碰撞"), Victim->GetActorEnableCollision());
    TestTrue(TEXT("尸体设3秒销毁"), Victim->GetLifeSpan() > 0.f && Victim->GetLifeSpan() <= 3.f);
    TestEqual(TEXT("账本击杀计次1"), F.Ledger().AppliedKills, uint64(1));
    TestTrue(TEXT("账本配对LastKillEventId有效"), F.Ledger().LastKillEventId.IsValid());
    TestTrue(TEXT("账本击杀上限累计≈35"), FMath::IsNearlyEqual(F.Ledger().MaxHealth, 35., 0.5));
    F.Tick(3.5f);
    TestFalse(TEXT("尸体3秒后已销毁"), IsValid(Victim));
    return true;
}

// ── 用例5：纯resolver——三维成长等价、上限成长先于衍生百分比、非法增量拒绝 ─────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GrowthPlayerAttributesRejectTest, "Y01.R37.Growth.PlayerAttributesReject",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GrowthPlayerAttributesRejectTest::RunTest(const FString&)
{
    using A = EY01Attribute;
    using C = EY01ContributionKind;
    FY01UnitCombatStats Base; Base.Strength = 10; Base.Agility = 4; Base.Wisdom = 2; Base.AttackDamage = 5;
    Base.Health = 300; Base.AttackInterval = 1; Base.Armor = 0; Base.HealthRegen = 0;
    FY01ResolvedAttributes Out;

    // 单参隐式构造等价力量成长：Totals(2.0) → Strength=10+2=12，其余维度不变。
    if (!TestTrue(TEXT("单参Totals纯解析成功"), FY01AttributeResolver::Resolve(Base, FY01PlayerGrowthTotals(2.0), {}, Out).IsOk())) { return false; }
    TestEqual(TEXT("力量等价成长进三维值"), Out.Stats.Strength, 12.f);
    TestEqual(TEXT("敏捷不受影响"), Out.Stats.Agility, 4.f);
    TestEqual(TEXT("智慧不受影响"), Out.Stats.Wisdom, 2.f);
    TestEqual(TEXT("上限不受影响"), Out.Stats.Health, 300.f);
    const auto* GrowthRow = Out.Breakdowns[static_cast<int32>(A::Strength) - 1].Contributions.FindByPredicate(
        [](const auto& X) { return X.Kind == C::Growth; });
    if (!TestNotNull(TEXT("力量成长明细单列"), GrowthRow)) { return false; }
    TestEqual(TEXT("明细来源为玩家成长"), GrowthRow->SourceKey, FString(TEXT("玩家成长")));
    TestEqual(TEXT("明细贡献=2"), GrowthRow->Contribution, 2.);

    // 上限成长进小计、先于衍生百分比：Totals{0,0,0,50}+AttributePercent→MaxHealth 10 → (300+50)×1.1=385。
    FY01SourceEffectSnapshot Percent; Percent.Source.Kind = EY01RuntimeSourceKind::Equipment; Percent.Source.LocalKey = TEXT("ItemA");
    Percent.EffectId = FName(TEXT("HealthPercent")); Percent.Kind = EY01SourceEffectKind::AttributePercent;
    Percent.TargetAttribute = A::MaxHealth; Percent.Amount = 10;
    if (!TestTrue(TEXT("上限成长+百分比解析成功"),
        FY01AttributeResolver::Resolve(Base, FY01PlayerGrowthTotals{0, 0, 0, 50}, {Percent}, Out).IsOk())) { return false; }
    TestTrue(TEXT("成长小计先于衍生百分比：Health=350×1.1"), FMath::IsNearlyEqual(Out.Stats.Health, 385.f, 1e-4f));
    const auto* HealthRow = Out.Breakdowns[static_cast<int32>(A::MaxHealth) - 1].Contributions.FindByPredicate(
        [](const auto& X) { return X.Kind == C::Growth; });
    if (!TestNotNull(TEXT("上限成长明细单列"), HealthRow)) { return false; }
    TestEqual(TEXT("上限明细贡献=50"), HealthRow->Contribution, 50.);

    // 非法增量拒绝：负值与NaN均整批拒绝（失败不泄漏Stats）。
    TestFalse(TEXT("负上限成长拒绝"), FY01AttributeResolver::Resolve(Base, FY01PlayerGrowthTotals{0, 0, 0, -1}, {}, Out).IsOk());
    TestFalse(TEXT("NaN上限成长拒绝"),
        FY01AttributeResolver::Resolve(Base, FY01PlayerGrowthTotals{0, 0, 0, std::numeric_limits<double>::quiet_NaN()}, {}, Out).IsOk());
    TestFalse(TEXT("负力量成长拒绝"), FY01AttributeResolver::Resolve(Base, FY01PlayerGrowthTotals(-1.0), {}, Out).IsOk());
    const FY01UnitCombatStats Empty;
    TestTrue(TEXT("拒绝后Stats恢复默认"),
        FY01UnitCombatStats::StaticStruct()->CompareScriptStruct(&Out.Stats, &Empty, 0));
    return true;
}
#endif
