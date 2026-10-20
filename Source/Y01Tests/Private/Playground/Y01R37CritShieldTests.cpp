// R37第二期定向测试：暴击、吸血、护盾（周期获得/施放获得）——只验证真实行为，不为绿灯修改语义。
// 夹具沿用 Y01R36LoadoutTests 的 FLoadoutWorld 骨架（真实 AY01GameMode + 纯内存19表目录 + 真实装备/学习链），
// 并补充 Start()/Tick()：世界 BeginPlay 后按0.01秒节拍推进，使护盾组件(0.1s)与伤害效果组件(0.05s)
// 按引擎真实组件节拍运行，而非测试内手动泵。每个测试构造独立世界，析构按既有路径完整销毁。
// 每条断言旁注释给出数值推导；当任务预期与实现不一致时，以真实行为为准并在测试报告中披露差异。
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
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01ShieldComponent.h"
#include "Combat/Y01Combat.h"
#include "Combat/Y01DamageEffects.h"
#include "Combat/Y01Monster.h"
#include "Combat/Y01DamageEvents.h"
#include "Match/Y01GameFlow.h"
#include "Y01MatchFixture.h"

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
using K = EY01TableKind;

// 单局测试世界：真实GameMode、真实PlayerState初始化链、真实组件节拍；析构即完整清理。
struct FR37CritShieldWorld
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
    bool bWaveStarted = false;

    FR37CritShieldWorld()
    {
        auto& C = Data.Row<FY01CharacterRow>(K::Character);
        C.InitialEquipment.Reset(); C.InitialSkills.Reset();
        C.Health = 100; C.AttackDamage = 20; C.Range = 1000; C.Armor = 0; C.HealthRegen = 0;
        Data.Row<FY01MatchRulesRow>(K::MatchRules).DefaultCritMultiplier = 2;
        // 准备阶段长挂起：波次永不进入战斗，怪物全部由测试显式生成，杜绝环境干扰。
        // Boss行必须保持长于普通行（DataCatalog跨行校验BossPreparation），两行同步放大。
        Data.Row<FY01WaveRow>(K::Wave).PreparationSeconds = 1000000.f;
        Data.Row<FY01WaveRow>(K::Wave, TEXT("Boss")).PreparationSeconds = 1000001.f;
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01GameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01GameMode>();
        PC = SpawnPlayer(); P = PC->GetPlayerState<AY01PlayerState>(); P->SetOwner(PC);
        // MatchRules通过TryConfigureMatch真实路径注入：Default行的暴击倍率在此被读取进GameMode。
        bConfigured = Mode->TryConfigureMatch(FY01DataCatalog(Data.Tables), Data.Waves(), FY01MatchFixture::Ref(K::Character), Error);
        InitResult = P->GetLoadout()->Initialize(Match, 1);
        bWaveStarted = Mode->TryStartNextWave();
        // 真实BeginPlay：护盾/伤害效果组件从下一个世界节拍开始按各自周期Tick。
        Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay();
    }
    ~FR37CritShieldWorld()
    { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }

    AY01PlayerController* SpawnPlayer()
    {
        auto* C = World->SpawnActor<AY01PlayerController>(); auto* S = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(0, 0, 100), FRotator::ZeroRotator);
        C->PlayerState = S; C->Possess(Pawn); S->GetCombatASC()->InitAbilityActorInfo(S, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(S); return C;
    }
    UY01CombatAbilitySystem* ASC() const { return P->GetCombatASC(); }
    UY01ShieldComponent* Shield() const { return P->GetShield(); }
    float PlayerHealth() const { return ASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()); }
    float MonsterHealth() const
    { return Monster ? Monster->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) : 0.f; }
    double TotalShield() const { return Shield()->GetTotal(); }
    // 按0.01秒推进世界：驱动TimerManager与全部组件Tick（护盾0.1s、伤害效果0.05s）。
    void Tick(float Seconds)
    { const int32 Steps = FMath::RoundToInt(Seconds * 100); for (int32 I = 0; I < Steps; ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .01f); } }

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
    FY01ItemResult Learn(const FY01ItemRequest& R)
    {
        switch (R.Operation)
        {
        case EY01ItemOperation::PrepareLearn: return P->GetLoadout()->PrepareLearn(PC, R);
        case EY01ItemOperation::ConfirmLearn: return P->GetLoadout()->ConfirmLearn(PC, R);
        case EY01ItemOperation::CancelLearn: return P->GetLoadout()->CancelLearn(PC, R);
        default: return P->GetLoadout()->RemoveSkill(PC, R);
        }
    }
    // 注入来源效果行（Amount + 可选盾参数）。
    void AddSourceEffect(FName Name, EY01SourceEffectKind Kind, double Amount, double DurationSeconds = 0, double LayerCap = 0, double IntervalSeconds = 0)
    {
        FY01SourceEffectRow E; E.Kind = Kind; E.Amount = Amount;
        E.DurationSeconds = DurationSeconds; E.LayerCap = LayerCap; E.IntervalSeconds = IntervalSeconds;
        Data.Add(K::SourceEffect, E, Name);
    }
    // 注入装备行（来源效果+伤害效果）并入包穿戴到指定槽。
    FY01ItemResult Equip(FName Name, const TArray<FY01DataRef>& EffectRefs, const TArray<FY01DataRef>& DamageRefs, FGuid& OutItemId, int32 Slot = 0)
    {
        FY01EquipmentRow E; E.DisplayName = FText::FromName(Name);
        E.EffectIds = EffectRefs; E.DamageEffectIds = DamageRefs;
        Data.Add(K::Equipment, E, Name);
        const auto G = Grant(Name);
        const auto Added = Add({G});
        if (!Added.IsOk()) { return Added; }
        OutItemId = G.ItemId;
        return Send(Request(G.ItemId, Slot));
    }
    // 注入主动技能书（UY01DamageAbility载体，携带来源效果与主动伤害效果）并入包学习到槽0。
    FY01ItemResult LearnSkillBook(FName Name, const TArray<FY01DataRef>& EffectRefs, const TArray<FY01DataRef>& DamageRefs)
    {
        FY01SkillRow S; S.DisplayName = FText::FromName(Name);
        S.Type = EY01SkillType::Active; S.Ability = UY01DamageAbility::StaticClass(); S.CooldownSeconds = 0;
        S.EffectIds = EffectRefs; S.DamageEffectIds = DamageRefs;
        Data.Add(K::Skill, S, Name);
        const auto G = Grant(Name, K::Skill);
        const auto Added = Add({G});
        if (!Added.IsOk()) { return Added; }
        return Learn(LearnRequest(G.ItemId, 0));
    }
    // 生成并初始化一只安全木桩：血1000、零攻击、大射程（供反向打玩家）、不移动、不回血、无技能。
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
    // 怪物以真实伤害GE打玩家（走共享伤害管线：减甲→护盾吸收→扣血）。
    void HitPlayer(float Damage)
    {
        auto* Source = Monster->GetCombatASC();
        auto Spec = Source->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, Source->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, Damage);
        Source->ApplyGameplayEffectSpecToTarget(*Spec.Data, ASC());
    }
};
}

// ── 用例1：暴击基础攻击 ─────────────────────────────────────────────────────────
// 暴击率100%装备 + MatchRules基础暴击倍率2 → 普攻必暴：怪损 2×攻击力(护甲0)。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37CritBasicAttackTest, "Y01.R37.Crit.BasicAttack",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37CritBasicAttackTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FR37CritShieldWorld F;
    if (!TestTrue(FString::Printf(TEXT("真实权威Mode目录配置成功 Error=%s"), *F.Error),
        F.bConfigured && F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    if (!TestTrue(TEXT("库存初始化成功"), F.InitResult.IsOk())) { return false; }
    if (!TestTrue(TEXT("波次进入准备阶段"), F.bWaveStarted)) { return false; }

    // 暴击率来源效果 Amount=100：CritChance属性钳制到[0,1] → 1.0（FRand()∈[0,1) → 必暴）。
    F.AddSourceEffect(TEXT("CritHundred"), EY01SourceEffectKind::CritChance, 100);
    FGuid ItemId;
    if (!TestTrue(TEXT("暴击装备入包并穿戴"), F.Equip(TEXT("CritGear"),
        {F.Data.Ref(K::SourceEffect, TEXT("CritHundred"))}, {}, ItemId).IsOk())) { return false; }
    TestTrue(TEXT("暴击率属性合计为100%"), FMath::IsNearlyEqual(F.ASC()->GetNumericAttribute(UY01AttributeSet::GetCritChanceAttribute()), 1.f));
    TestTrue(TEXT("MatchRules基础暴击倍率为2"), FMath::IsNearlyEqual(F.Mode->GetDefaultCritMultiplier(), 2.));
    TestTrue(TEXT("暴击倍率加成为0"), FMath::IsNearlyEqual(F.ASC()->GetNumericAttribute(UY01AttributeSet::GetCritMultiplierBonusAttribute()), 0.f));

    auto* M = F.SpawnMonster(FVector(400, 0, 100));
    if (!TestTrue(TEXT("怪物初始化成功"), M != nullptr && F.SpawnError.IsEmpty())) { return false; }
    TestEqual(TEXT("怪物初始生命1000"), F.MonsterHealth(), 1000.f);

    auto* Events = F.World->GetSubsystem<UY01DamageEvents>();
    TArray<FY01DamageEvent> Captured;
    const auto Hook = Events->OnDamageCommitted.AddLambda([&](const FY01DamageEvent& Event) { Captured.Add(Event); });
    ON_SCOPE_EXIT { Events->OnDamageCommitted.Remove(Hook); };

    TestTrue(TEXT("普攻激活成功"), F.ASC()->SetCombatTarget(M) && F.ASC()->TryActivateBasicAttack());
    // 推导：攻击力20，必暴 → 20×(基础倍率2+加成0)=40；怪物护甲0、无盾 → 扣血40。
    TestTrue(TEXT("怪物损失2倍攻击力(40)"), FMath::IsNearlyEqual(F.MonsterHealth(), 960.f));
    if (TestEqual(TEXT("普攻提交一个伤害事件"), Captured.Num(), 1))
    {
        TestTrue(TEXT("事件暴击标记为真"), Captured[0].bCritical);
        TestEqual(TEXT("事件实际损失为40"), Captured[0].ActualDamage, 40.f);
        TestEqual(TEXT("事件类型为伤害"), Captured[0].Type, EY01DamageEventType::Damage);
    }
    return true;
}

// ── 用例2：吸血基础攻击 ─────────────────────────────────────────────────────────
// 吸血50%装备：普攻实际损血20×50%=10回给玩家；同件装备携带 bLifesteal=false 的挂身DOT验证非普攻来源不吸血。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37LifestealBasicAttackTest, "Y01.R37.Lifesteal.BasicAttack",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37LifestealBasicAttackTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FR37CritShieldWorld F;
    if (!TestTrue(FString::Printf(TEXT("真实权威Mode目录配置成功 Error=%s"), *F.Error),
        F.bConfigured && F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    if (!TestTrue(TEXT("库存初始化成功"), F.InitResult.IsOk())) { return false; }
    if (!TestTrue(TEXT("波次进入准备阶段"), F.bWaveStarted)) { return false; }

    // 吸血50% + 挂身DOT（周期触发首次发动在装备1秒后，挂身后每0.5秒一跳、每跳3点、不吸血、寿命5秒）。
    F.AddSourceEffect(TEXT("Steal50"), EY01SourceEffectKind::Lifesteal, 50);
    FY01DamageEffectRow Dot; Dot.Product = EY01DamageProduct::AttachedDot;
    Dot.FixedDamage = 3; Dot.TickSeconds = 0.5; Dot.Lifetime = 5;
    Dot.PeriodSeconds = 1; Dot.SearchRadius = 1000; Dot.Radius = 150; Dot.bLifesteal = false;
    F.Data.Add(K::DamageEffect, Dot, TEXT("PoisonDot"));
    FGuid ItemId;
    if (!TestTrue(TEXT("吸血装备入包并穿戴"), F.Equip(TEXT("StealGear"),
        {F.Data.Ref(K::SourceEffect, TEXT("Steal50"))}, {F.Data.Ref(K::DamageEffect, TEXT("PoisonDot"))}, ItemId).IsOk())) { return false; }
    TestTrue(TEXT("吸血属性为50%"), FMath::IsNearlyEqual(F.ASC()->GetNumericAttribute(UY01AttributeSet::GetLifestealAttribute()), 0.5f));

    // 玩家降到半血50/100。
    F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 50.f);
    TestEqual(TEXT("玩家处于半血"), F.PlayerHealth(), 50.f);

    auto* M = F.SpawnMonster(FVector(400, 0, 100));
    if (!TestTrue(TEXT("怪物初始化成功"), M != nullptr && F.SpawnError.IsEmpty())) { return false; }

    // 推导：周期触发首次发动=装备后PeriodSeconds(1秒)→0.5秒内DOT尚未发动。
    F.Tick(0.5f);
    TestEqual(TEXT("DOT发动前怪物满血"), F.MonsterHealth(), 1000.f);
    TestEqual(TEXT("DOT发动前玩家保持半血"), F.PlayerHealth(), 50.f);
    // 首次发动(~1.05s)立即打第一跳3点；下一跳在+0.5s，所以1.2秒内恰好一跳。
    F.Tick(0.7f);
    TestTrue(TEXT("DOT第一跳扣怪3点"), FMath::IsNearlyEqual(F.MonsterHealth(), 997.f));
    TestEqual(TEXT("DOT不吸血时玩家保持半血"), F.PlayerHealth(), 50.f);

    // 普攻：玩家攻击力20 → 怪再损20；吸血=实际生命损失20×50%=10 → 玩家50→60。
    TestTrue(TEXT("普攻激活成功"), F.ASC()->SetCombatTarget(M) && F.ASC()->TryActivateBasicAttack());
    TestTrue(TEXT("普攻扣怪20点"), FMath::IsNearlyEqual(F.MonsterHealth(), 977.f));
    TestEqual(TEXT("吸血回复实际损失的一半(10)"), F.PlayerHealth(), 60.f);

    // 再推进1.2秒（世界时刻~2.4）：DOT在~1.55/~2.05继续跳怪，但bLifesteal=false路径不得回血。
    F.Tick(1.2f);
    TestTrue(TEXT("后续DOT跳继续扣怪"), F.MonsterHealth() < 977.f);
    TestEqual(TEXT("非普攻伤害严格不回血"), F.PlayerHealth(), 60.f);
    return true;
}

// ── 用例3：周期护盾 ─────────────────────────────────────────────────────────────
// ShieldPeriodic 30/上限60/时长2/间隔1：首刻+1秒共两层到上限60；随后逐击验证吸收与耗尽。
// 披露：任务草案“50伤→玩家损20、盾余30”与管线语义及自身算术矛盾（吸收+损血=50，不可能再余30）；
// 本用例按真实管线断言：Total=60时50伤→吸收50、损血0、余10；30伤→吸收10、损血20、余0；10伤→全额损血。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37ShieldPeriodicTest, "Y01.R37.Shield.Periodic",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37ShieldPeriodicTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FR37CritShieldWorld F;
    if (!TestTrue(FString::Printf(TEXT("真实权威Mode目录配置成功 Error=%s"), *F.Error),
        F.bConfigured && F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    if (!TestTrue(TEXT("库存初始化成功"), F.InitResult.IsOk())) { return false; }
    if (!TestTrue(TEXT("波次进入准备阶段"), F.bWaveStarted)) { return false; }

    // 周期护盾：单层30、层上限60、时长2秒、间隔1秒；装备于槽0。
    F.AddSourceEffect(TEXT("PeriodicShield"), EY01SourceEffectKind::ShieldPeriodic, 30, 2, 60, 1);
    FGuid ItemId;
    if (!TestTrue(TEXT("护盾装备入包并穿戴"), F.Equip(TEXT("ShieldGear"),
        {F.Data.Ref(K::SourceEffect, TEXT("PeriodicShield"))}, {}, ItemId).IsOk())) { return false; }

    // 首个护盾Tick(~0.1s)立即获得第一层30（Next=首刻+1秒）。
    F.Tick(0.35f);
    TestTrue(TEXT("首层护盾为30"), FMath::IsNearlyEqual(F.TotalShield(), 30.));
    // 第二层在~1.1s发放：同层累加 min(上限60, 30+30)=60 并刷新到期。
    F.Tick(1.2f);
    TestTrue(TEXT("两层层叠到上限60"), FMath::IsNearlyEqual(F.TotalShield(), 60.));

    auto* M = F.SpawnMonster(FVector(400, 0, 100));
    if (!TestTrue(TEXT("怪物初始化成功"), M != nullptr && F.SpawnError.IsEmpty())) { return false; }

    // 第一击50：护甲0 → 盾吸收min(60,50)=50 → 损血0，盾余10。
    F.HitPlayer(50.f);
    TestEqual(TEXT("全额吸收时玩家无损"), F.PlayerHealth(), 100.f);
    TestTrue(TEXT("吸收后盾余10"), FMath::IsNearlyEqual(F.TotalShield(), 10.));
    // 第二击30：盾吸收10 → 损血20，盾清空。
    F.HitPlayer(30.f);
    TestEqual(TEXT("部分吸收时玩家损20"), F.PlayerHealth(), 80.f);
    TestTrue(TEXT("盾耗尽为0"), FMath::IsNearlyEqual(F.TotalShield(), 0.));
    // 第三击10：无盾 → 全额损血10。
    F.HitPlayer(10.f);
    TestEqual(TEXT("无盾时全额损血"), F.PlayerHealth(), 70.f);
    TestTrue(TEXT("无盾不生成护盾"), FMath::IsNearlyEqual(F.TotalShield(), 0.));
    return true;
}

// ── 用例4：施放护盾 ─────────────────────────────────────────────────────────────
// 技能携带 ShieldOnCast 25/25/5 + 主动即时伤害2：施放成功→盾25；玩家死亡→盾清0。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37ShieldOnCastTest, "Y01.R37.Shield.OnCast",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37ShieldOnCastTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FR37CritShieldWorld F;
    if (!TestTrue(FString::Printf(TEXT("真实权威Mode目录配置成功 Error=%s"), *F.Error),
        F.bConfigured && F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    if (!TestTrue(TEXT("库存初始化成功"), F.InitResult.IsOk())) { return false; }
    if (!TestTrue(TEXT("波次进入准备阶段"), F.bWaveStarted)) { return false; }

    // 施放护盾来源效果：单层25、层上限25、时长5秒（仅技能来源合法）。
    F.AddSourceEffect(TEXT("OnCastShield"), EY01SourceEffectKind::ShieldOnCast, 25, 5, 25, 0);
    // 技能载体：主动即时伤害2（固定值），同一技能携带护盾效果——施放成功后NotifyCast发盾。
    FY01DamageEffectRow Strike; Strike.Trigger = EY01DamageTrigger::Active;
    Strike.Product = EY01DamageProduct::Instant; Strike.FixedDamage = 2; Strike.SearchRadius = 1000;
    F.Data.Add(K::DamageEffect, Strike, TEXT("CastStrike"));
    if (!TestTrue(TEXT("技能书入包并学习到槽0"), F.LearnSkillBook(TEXT("ShieldSkill"),
        {F.Data.Ref(K::SourceEffect, TEXT("OnCastShield"))}, {F.Data.Ref(K::DamageEffect, TEXT("CastStrike"))}).IsOk())) { return false; }
    auto* M = F.SpawnMonster(FVector(400, 0, 100));
    if (!TestTrue(TEXT("怪物初始化成功"), M != nullptr && F.SpawnError.IsEmpty())) { return false; }
    // 先走0.2秒：伤害效果组件同步已提交来源（护盾在施放前必须为空）。
    F.Tick(0.2f);
    TestTrue(TEXT("施放前无护盾"), FMath::IsNearlyEqual(F.TotalShield(), 0.));

    TestTrue(TEXT("技能施放成功"), F.P->TryActivateSkillSlot(0));
    // 推导：施放护盾=单层25（≤上限25）；主动即时伤害2 → 怪1000-2=998。
    TestTrue(TEXT("施放获得25点护盾"), FMath::IsNearlyEqual(F.TotalShield(), 25.));
    TestEqual(TEXT("技能伤害扣怪2点"), F.MonsterHealth(), 998.f);

    // 致命一击500：盾吸收25 → 触达生命475 → 扣血min(475,100)=100 → 死亡分支同步ResetLayers。
    F.HitPlayer(500.f);
    TestEqual(TEXT("致命伤后玩家生命为0"), F.PlayerHealth(), 0.f);
    TestTrue(TEXT("死亡即清空护盾"), FMath::IsNearlyEqual(F.TotalShield(), 0.));
    return true;
}
#endif
