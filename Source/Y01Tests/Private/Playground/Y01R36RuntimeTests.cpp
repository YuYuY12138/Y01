#include "Misc/AutomationTest.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01PlayerState.h"
#include "Y01MatchFixture.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "UObject/UnrealType.h"
#include <limits>
#include <cmath>
#include <type_traits>
#include "GameplayEffectAggregator.h"
#include "Playground/Y01R36Verification.h"
#include "Playground/Y01R36Assets.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01EconomyComponent.h"
#include "Combat/Y01Monster.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"

#if WITH_DEV_AUTOMATION_TESTS
// 仅开发夹具访问模块私有时钟；正式入口不接受客户端时间或任意owner。
struct FY01R36RuntimeTestAccess
{
    static void Clock(UY01IncomeComponent* C, double* Time) { C->TestClock = [Time] { return *Time; }; }
    static void Audit(UY01IncomeComponent* C, int64 Total) { C->State.TotalIncome = Total; }
    static void Sequence(UY01IncomeComponent* C, int64 Total) { C->State.Sequence = Total; }
};
namespace
{
using ESource = EY01RuntimeSourceKind;
using EEffect = EY01SourceEffectKind;
using EError = EY01RuntimeError;
struct FRuntimeWorld
{
    FY01MatchFixture Data;
    UWorld* World;
    AY01PlayerState* P;
    double Now = 0;
    FGuid Match = FGuid::NewGuid();
    FRuntimeWorld()
    {
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
        P = Player();
        Effect(TEXT("A"), EEffect::AttackFlat, 5);
        Effect(TEXT("B"), EEffect::AttackFlat, 7);
        Effect(TEXT("R"), EEffect::GoldPerSecond, 1);
    }
    ~FRuntimeWorld()
    {
        World->EndPlay(EEndPlayReason::Quit);
        World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    }
    AY01PlayerState* Player()
    {
        auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* ASC = PS->GetCombatASC(); ASC->InitAbilityActorInfo(PS, PS);
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 30);
        FY01R36RuntimeTestAccess::Clock(PS->GetIncome(), &Now);
        return PS;
    }
    UY01SourceEffectsComponent* S() const { return P->GetSourceEffects(); }
    UY01IncomeComponent* I() const { return P->GetIncome(); }
    UY01CombatAbilitySystem* ASC() const { return P->GetCombatASC(); }
    float Attack() const { return ASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()); }
    FY01IncomeSnapshot Income() const { return I()->Observe(); }
    void Effect(FName Id, EEffect Kind, double Value)
    { FY01SourceEffectRow R; R.Kind = Kind; R.Magnitude = Value; Data.Add(EY01TableKind::SourceEffect, R, Id); }
    TArray<FY01DataRef> Refs(std::initializer_list<const TCHAR*> Ids) const
    { TArray<FY01DataRef> R; for (const TCHAR* Id : Ids) { R.Add(FY01MatchFixture::Ref(EY01TableKind::SourceEffect, Id)); } return R; }
    FY01RuntimeResult Init(double Remainder = 0) { return S()->Initialize(Match, 1, Remainder); }
    FY01RuntimeResult Prepare(FY01PreparedSource& Plan, FString Key, TArray<FY01DataRef> Ids, ESource Kind = ESource::Equipment)
    { return S()->PrepareSource(FY01DataCatalog(Data.Tables), Kind, Key, Ids, Plan); }
    FY01RuntimeResult Add(FString Key, TArray<FY01DataRef> Ids, ESource Kind = ESource::Equipment)
    {
        FY01PreparedSource Plan; auto R = Prepare(Plan, Key, Ids, Kind); if (!R.IsOk()) { return R; }
        FY01SourceTransaction Tx; R = S()->StageSource(Plan, Tx); return R.IsOk() ? S()->CommitSource(Tx) : R;
    }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36EffectsTest, "Y01.Framework.R36.Effects",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36EffectsTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("DefaultDisabled"), TEXT("SharedIndependent"), TEXT("RepeatPayload"), TEXT("ChangedPayload"),
        TEXT("MultipleEffects"), TEXT("SecondGEFailure"), TEXT("StageRollback"), TEXT("StageCommit"), TEXT("StalePlan"),
        TEXT("ForeignPlan"), TEXT("ForeignTransaction"), TEXT("SortAndIdentity"), TEXT("SnapshotDuringStage"),
        TEXT("Reentry.Apply"), TEXT("Reentry.Attribute"), TEXT("Reentry.Remove"), TEXT("Reentry.Stop"), TEXT("Reentry.Initialize"),
        TEXT("DetachedHandle"), TEXT("ClientInitialize"), TEXT("ClientMutation"), TEXT("SessionIdentity"), TEXT("MultiPlayer"),
        TEXT("Income.Fraction"), TEXT("Income.RateChange"), TEXT("Income.Partitions"), TEXT("Income.DecimalPartitions"),
        TEXT("Income.RemoveEarned"), TEXT("Income.FiveToFour"), TEXT("Income.FailureKeepsEarned"), TEXT("Income.Waiting"),
        TEXT("Income.StartStop"), TEXT("Income.StopWaiting"), TEXT("Income.EndPlay"), TEXT("Income.DestroyOwner"),
        TEXT("Income.NoPawnDeath"), TEXT("Income.Ring32"), TEXT("Income.GoldMax"), TEXT("Income.GoldOverflow"),
        TEXT("Income.AuditMax"), TEXT("Income.AuditOverflow"), TEXT("Income.WholeInt64Boundary"), TEXT("Income.WholeBelowInt64"),
        TEXT("Income.TimeBackwards"), TEXT("Income.TimeNaN"), TEXT("Income.ElapsedOverflow"), TEXT("Income.GrossOverflow"),
        TEXT("Income.Underflow"), TEXT("Income.RemainderSwallow"), TEXT("Income.FaultPermanent"), TEXT("Income.InvalidRemainder"),
        TEXT("Legacy.Equipment"), TEXT("Legacy.Skill"), TEXT("Tx.ClientCommit"), TEXT("Tx.ClientRollback"),
        TEXT("Tx.RepeatedCommit"), TEXT("Tx.StopCapturedTime"), TEXT("Tx.EndPlayDuringApply"),
        TEXT("Tx.ActorInfoChanged"), TEXT("Tx.RemoveNotification"), TEXT("Income.Zero"), TEXT("Income.Subunit"),
        TEXT("Income.SequenceOverflow"), TEXT("Identity.InvalidMatch"), TEXT("Identity.InvalidJoin"),
        TEXT("Regression.EndPlayRollback"), TEXT("Regression.EndPlayRemove"), TEXT("Regression.EndPlayCapturedTime"),
        TEXT("Regression.EndPlayRepeated"), TEXT("Regression.RemovePrecisionRollback"), TEXT("Regression.SourceOrder") }) { Names.Add(N); Commands.Add(N); }
    for (const TCHAR* Kind : { TEXT("Attack"), TEXT("Rate") })
    {
        for (const TCHAR* N : { TEXT("Zero"), TEXT("Fraction"), TEXT("Negative"), TEXT("NaN"), TEXT("Infinity"),
            TEXT("Enum"), TEXT("SumOverflow"), TEXT("Swallow"), TEXT("ReverseSwallow") })
        { const FString Name = FString(TEXT("Numeric.")) + Kind + TEXT(".") + N; Names.Add(Name); Commands.Add(Name); }
    }
    for (const TCHAR* N : { TEXT("Underflow"), TEXT("FloatOverflow"), TEXT("BaseSwallow"), TEXT("NormalRounding") })
    { const FString Name = FString(TEXT("Numeric.Attack.")) + N; Names.Add(Name); Commands.Add(Name); }
    for (const TCHAR* N : { TEXT("WrongType"), TEXT("Empty"), TEXT("MissingRow"), TEXT("MissingTable"), TEXT("WrongSchema"),
        TEXT("Duplicate"), TEXT("CaseDuplicate"), TEXT("BadKey"), TEXT("BadKind") })
    { const FString Name = FString(TEXT("Invalid.")) + N; Names.Add(Name); Commands.Add(Name); }
}
bool FY01R36EffectsTest::RunTest(const FString& Case)
{
    FRuntimeWorld F;
    auto Ok = [&](const TCHAR* Message, const FY01RuntimeResult& R)
    { if (!R.IsOk()) { AddInfo(R.Message); } return TestTrue(Message, R.IsOk()); };
    auto Bad = [&](const TCHAR* Message, const FY01RuntimeResult& R)
    { TestFalse(Message, R.IsOk()); TestFalse(TEXT("错误有诊断"), R.Message.IsEmpty()); };
    if (Case == TEXT("DefaultDisabled"))
    {
        TestNotNull(TEXT("生产来源组件挂PlayerState"), F.S()); TestNotNull(TEXT("生产收入组件挂PlayerState"), F.I());
        TestFalse(TEXT("默认来源不激活"), F.S()->IsActive()); TestFalse(TEXT("默认收入不激活"), F.I()->IsActive());
        TestFalse(TEXT("来源不Tick"), F.S()->PrimaryComponentTick.bCanEverTick); TestFalse(TEXT("收入不Tick"), F.I()->PrimaryComponentTick.bCanEverTick);
        F.Now = 10; Bad(TEXT("未初始化不能开始"), F.I()->Start());
        TestEqual(TEXT("默认无金币副作用"), F.P->GetGold(), 0); TestEqual(TEXT("默认无GAS副作用"), F.Attack(), 30.f); return true;
    }
    if (Case == TEXT("ClientInitialize"))
    { F.P->SetRole(ROLE_SimulatedProxy); Bad(TEXT("客户端初始化拒绝"), F.Init()); F.P->SetRole(ROLE_Authority); return true; }
    if (Case.StartsWith(TEXT("Identity.")))
    {
        Bad(TEXT("服务器会话身份也必须验证"), F.S()->Initialize(Case.EndsWith(TEXT("InvalidMatch")) ? FGuid() : F.Match, Case.EndsWith(TEXT("InvalidJoin")) ? 0 : 1));
        Ok(TEXT("非法身份不消耗初始化机会"), F.Init()); return true;
    }
    if (Case == TEXT("Income.InvalidRemainder"))
    {
        for (double V : {-1., 1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        { Bad(TEXT("初始余量必须在[0,1)"), F.Init(V)); }
        Ok(TEXT("拒绝后仍可合法初始化"), F.Init()); return true;
    }
    const double InitialRemainder = Case == TEXT("Income.Fraction") || Case == TEXT("Income.RemainderSwallow") ? .25 : 0;
    if (Case == TEXT("SharedIndependent"))
    {
        Ok(TEXT("显式服务器初始化"), F.Init());
        Ok(TEXT("第一独立来源"), F.Add(TEXT("E01"), F.Refs({TEXT("A")})));
        Ok(TEXT("两独立source共享effect"), F.Add(TEXT("E02"), F.Refs({TEXT("A")}))); TestEqual(TEXT("30+5+5=40"), F.Attack(), 40.f);
        Ok(TEXT("撤销一份"), F.S()->RemoveSource(ESource::Equipment, TEXT("E01"))); TestEqual(TEXT("保留另一份35"), F.Attack(), 35.f);
        Ok(TEXT("重复移除幂等"), F.S()->RemoveSource(ESource::Equipment, TEXT("E01"))); TestEqual(TEXT("base始终30"), F.ASC()->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute()), 30.f); return true;
    }
    if (Case == TEXT("Income.Fraction"))
    {
        Ok(TEXT("显式余量初始化"), F.Init(.25)); F.Effect(TEXT("R"), EEffect::GoldPerSecond, 1.5);
        Ok(TEXT("基础收入来源"), F.Add(TEXT("Income"), F.Refs({TEXT("R")}), ESource::Base)); Ok(TEXT("正式启动"), F.I()->Start());
        F.Now = .5; Ok(TEXT("分段公式"), F.I()->Settle()); TestEqual(TEXT("0.25+0.5*1.5入账1"), F.P->GetGold(), 1); TestEqual(TEXT("余量0"), F.Income().State.Remainder, 0.); return true;
    }
    if (!Ok(TEXT("显式服务器初始化"), F.Init(InitialRemainder))) { return false; }
    if (Case.StartsWith(TEXT("Regression.")))
    {
        if (Case == TEXT("Regression.RemovePrecisionRollback") || Case == TEXT("Regression.SourceOrder"))
        {
            F.Effect(TEXT("Small"), EEffect::AttackFlat, 3);
            F.Effect(TEXT("Large"), EEffect::AttackFlat, 16777216);
            const bool Reverse = Case == TEXT("Regression.SourceOrder");
            if (Reverse) { Ok(TEXT("先注册大值来源"), F.Add(TEXT("E03"), F.Refs({TEXT("Large")}))); }
            Ok(TEXT("小值来源一"), F.Add(TEXT("E01"), F.Refs({TEXT("Small")})));
            Ok(TEXT("小值来源二"), F.Add(TEXT("E02"), F.Refs({TEXT("Small")})));
            if (!Reverse) { Ok(TEXT("后注册大值来源"), F.Add(TEXT("E03"), F.Refs({TEXT("Large")}))); }
            TestEqual(TEXT("真实ASC按稳定来源顺序求和"), F.Attack(), 16777252.f);
            const float Before = F.Attack();
            const auto Handles = F.ASC()->GetActiveEffects(FGameplayEffectQuery());
            FY01SourceSnapshot BeforeView; Ok(TEXT("移除前快照"), F.S()->Observe(BeforeView));
            int32 Calls = 0;
            auto H = F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).AddLambda(
                [&](const FOnAttributeChangeData&) { if (++Calls == 1) { Bad(TEXT("移除委托重入拒绝"), F.I()->Settle()); } });
            Bad(TEXT("委托重入拒绝移除"), F.S()->RemoveSource(ESource::Equipment, TEXT("E01")));
            F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).Remove(H);
            TestTrue(TEXT("真实属性委托已触发"), Calls > 0);
            TestEqual(TEXT("失败操作精确恢复真实攻击"), F.Attack(), Before);
            TestEqual(TEXT("原GE数量保持"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), Handles.Num());
            for (const auto Handle : Handles) { TestNotNull(TEXT("原精确handle保持"), F.ASC()->GetActiveGameplayEffect(Handle)); }
            FY01SourceSnapshot AfterView; Ok(TEXT("移除后快照"), F.S()->Observe(AfterView));
            TestEqual(TEXT("失败不改来源版本"), AfterView.Revision, BeforeView.Revision);
            TestEqual(TEXT("失败不改来源明细"), AfterView.Effects.Num(), BeforeView.Effects.Num());
            return true;
        }
        Ok(TEXT("旧合法来源"), F.Add(TEXT("Old"), F.Refs({TEXT("A"), TEXT("R")})));
        Ok(TEXT("开始旧收入"), F.I()->Start());
        FY01SourceTransaction Tx;
        const bool Pending = Case == TEXT("Regression.EndPlayRollback") || Case == TEXT("Regression.EndPlayCapturedTime");
        if (Pending)
        {
            FY01PreparedSource Plan; Ok(TEXT("暂存预检"), F.Prepare(Plan, TEXT("New"), F.Refs({TEXT("B")})));
            F.Now = 1; Ok(TEXT("等待提交的真实GE"), F.S()->StageSource(Plan, Tx));
        }
        F.Now = 2;
        int32 Calls = 0;
        if (Case == TEXT("Regression.EndPlayRemove"))
        {
            auto H = F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).AddLambda(
                [&](const FOnAttributeChangeData&) { if (++Calls == 1) { F.S()->EndPlay(EEndPlayReason::Quit); F.Now = 3; } });
            Bad(TEXT("结束打断移除事务"), F.S()->RemoveSource(ESource::Equipment, TEXT("Old")));
            F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).Remove(H);
            TestTrue(TEXT("结束发生在真实移除属性委托"), Calls > 0);
        }
        else if (Pending)
        {
            auto H = F.ASC()->OnAnyGameplayEffectRemovedDelegate().AddLambda([&](const FActiveGameplayEffect&)
            {
                if (++Calls != 1) { return; }
                if (Case == TEXT("Regression.EndPlayRollback")) { F.S()->EndPlay(EEndPlayReason::Quit); }
                F.Now = 3;
            });
            if (Case == TEXT("Regression.EndPlayRollback")) { Ok(TEXT("回滚中的结束收尾"), F.S()->RollbackSource(Tx)); }
            else { F.S()->EndPlay(EEndPlayReason::Quit); }
            F.ASC()->OnAnyGameplayEffectRemovedDelegate().Remove(H);
            TestTrue(TEXT("真实GE移除委托推进时间"), Calls > 0);
        }
        else { F.S()->EndPlay(EEndPlayReason::Quit); }
        TestEqual(TEXT("结束不残留旧GE或暂存GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 0);
        TestEqual(TEXT("结束恢复基础攻击"), F.Attack(), 30.f);
        TestEqual(TEXT("仅结算结束入口时刻"), F.P->GetGold(), 2);
        TestEqual(TEXT("收入已停止"), F.Income().State.Phase, EY01IncomePhase::Stopped);
        FY01SourceSnapshot View; Ok(TEXT("结束后可读取最终来源快照"), F.S()->Observe(View));
        TestEqual(TEXT("结束清空已提交来源"), View.Effects.Num(), 0);
        const int64 Sequence = F.Income().State.Sequence;
        F.Now = 10; F.S()->EndPlay(EEndPlayReason::Quit); F.I()->EndPlay(EEndPlayReason::Quit);
        TestEqual(TEXT("重复结束不重复收入"), F.P->GetGold(), 2);
        TestEqual(TEXT("重复结束不追加流水"), F.Income().State.Sequence, Sequence);
        return true;
    }
    if (Case.StartsWith(TEXT("Tx.")))
    {
        Ok(TEXT("旧合法来源"), F.Add(TEXT("Old"), F.Refs({TEXT("A"), TEXT("R")}))); Ok(TEXT("旧收入运行"), F.I()->Start());
        const auto OldHandles = F.ASC()->GetActiveEffects(FGameplayEffectQuery());
        if (Case == TEXT("Tx.RemoveNotification"))
        {
            FY01RuntimeResult Nested; int Calls = 0;
            auto H = F.ASC()->OnAnyGameplayEffectRemovedDelegate().AddLambda([&](const FActiveGameplayEffect&)
            { ++Calls; Nested = F.Add(TEXT("Nested"), F.Refs({TEXT("B")})); FY01SourceSnapshot V; Bad(TEXT("移除通知不读半事务快照"), F.S()->Observe(V)); });
            Ok(TEXT("通知是不可逆提交之后"), F.S()->RemoveSource(ESource::Equipment, TEXT("Old")));
            F.ASC()->OnAnyGameplayEffectRemovedDelegate().Remove(H);
            TestEqual(TEXT("真实移除通知一次"), Calls, 1); Bad(TEXT("通知中的业务重入拒绝"), Nested);
            TestEqual(TEXT("移除完整提交"), F.Attack(), 30.f); TestEqual(TEXT("没有半份速率"), F.Income().State.Rate, 0.); return true;
        }
        FY01PreparedSource Plan; Ok(TEXT("新来源预检"), F.Prepare(Plan, TEXT("New"), F.Refs({TEXT("B"), TEXT("R")})));
        if (Case == TEXT("Tx.EndPlayDuringApply") || Case == TEXT("Tx.ActorInfoChanged"))
        {
            auto H = F.ASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda([&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle)
            { if (Case.EndsWith(TEXT("EndPlayDuringApply"))) { F.S()->EndPlay(EEndPlayReason::Quit); } else { F.ASC()->InitAbilityActorInfo(F.World->SpawnActor<AActor>(), F.P); } });
            FY01SourceTransaction Tx; Bad(TEXT("应用后生命周期/owner失效必须回滚"), F.S()->StageSource(Plan, Tx));
            F.ASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(H); F.ASC()->InitAbilityActorInfo(F.P, F.P);
            if (Case == TEXT("Tx.EndPlayDuringApply"))
            {
                TestEqual(TEXT("EndPlay清理全部来源攻击"), F.Attack(), 30.f);
                TestNull(TEXT("EndPlay清理旧handle"), F.ASC()->GetActiveGameplayEffect(OldHandles[0]));
                TestEqual(TEXT("EndPlay没有残留GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 0);
                FY01SourceSnapshot View; Ok(TEXT("结束后来源快照可读"), F.S()->Observe(View));
                TestEqual(TEXT("结束后来源为空"), View.Effects.Num(), 0);
            }
            else
            {
                TestEqual(TEXT("owner改变不留半份新GE"), F.Attack(), 35.f);
                TestNotNull(TEXT("owner改变保留旧handle"), F.ASC()->GetActiveGameplayEffect(OldHandles[0]));
            }
            return true;
        }
        F.Now = 1; FY01SourceTransaction Tx; Ok(TEXT("真实暂存"), F.S()->StageSource(Plan, Tx));
        if (Case.StartsWith(TEXT("Tx.Client")))
        {
            F.P->SetRole(ROLE_SimulatedProxy);
            Bad(TEXT("客户端即使拿到令牌也无权提交/回滚"), Case.EndsWith(TEXT("ClientCommit")) ? F.S()->CommitSource(Tx) : F.S()->RollbackSource(Tx));
            F.P->SetRole(ROLE_Authority);
            Ok(TEXT("权限拒绝不能销毁服务器事务与回滚句柄"), F.S()->RollbackSource(Tx)); TestEqual(TEXT("服务器恢复旧精确状态"), F.Attack(), 35.f); return true;
        }
        if (Case == TEXT("Tx.StopCapturedTime"))
        {
            F.Now = 2; Bad(TEXT("事务期间Stop拒绝且挂起停止"), F.I()->Stop()); F.Now = 3;
            Ok(TEXT("业务回滚"), F.S()->RollbackSource(Tx)); TestEqual(TEXT("只结算Stop请求时刻而非回滚时刻"), F.P->GetGold(), 2);
            Bad(TEXT("挂起Stop永久生效"), F.I()->Start()); TestEqual(TEXT("旧攻击保留"), F.Attack(), 35.f); return true;
        }
        Ok(TEXT("首次提交"), F.S()->CommitSource(Tx)); Bad(TEXT("已消费令牌不能二次提交"), F.S()->CommitSource(Tx));
        Bad(TEXT("已消费令牌不能回滚已提交来源"), F.S()->RollbackSource(Tx)); TestEqual(TEXT("只授予一次"), F.Attack(), 42.f); return true;
    }
    if (Case.StartsWith(TEXT("Legacy.")))
    {
        auto& C = F.Data.Row<FY01CharacterRow>(EY01TableKind::Character); C.InitialEquipment.Reset(); C.InitialSkills.Reset();
        if (Case.EndsWith(TEXT("Equipment")))
        { F.Data.Row<FY01EquipmentRow>(EY01TableKind::Equipment).EffectIds = F.Refs({TEXT("A")}); C.InitialEquipment.Add(FY01MatchFixture::Ref(EY01TableKind::Equipment)); }
        else { F.Data.Row<FY01SkillRow>(EY01TableKind::Skill).EffectIds = F.Refs({TEXT("A")}); C.InitialSkills.Add(FY01MatchFixture::Ref(EY01TableKind::Skill)); }
        FString Error; TestFalse(TEXT("旧消费者不静默忽略SourceEffect"), F.P->TryInitializeCharacter(FY01DataCatalog(F.Data.Tables), FY01MatchFixture::Ref(EY01TableKind::Character), Error));
        TestEqual(TEXT("旧消费者拒绝无半份攻击"), F.Attack(), 30.f); return true;
    }
    if (Case.StartsWith(TEXT("Numeric.")))
    {
        const bool Attack = Case.Contains(TEXT(".Attack.")); const EEffect Kind = Attack ? EEffect::AttackFlat : EEffect::GoldPerSecond;
        double V = Case.EndsWith(TEXT("Zero")) ? 0 : Case.EndsWith(TEXT("Fraction")) ? .25 : Case.EndsWith(TEXT("NormalRounding")) ? .1 : 5;
        if (Case.EndsWith(TEXT("Negative"))) { V = -1; }
        if (Case.EndsWith(TEXT("NaN"))) { V = std::numeric_limits<double>::quiet_NaN(); }
        if (Case.EndsWith(TEXT("Infinity"))) { V = std::numeric_limits<double>::infinity(); }
        if (Case.EndsWith(TEXT("Underflow"))) { V = 1e-100; }
        if (Case.EndsWith(TEXT("FloatOverflow"))) { V = double(MAX_flt) * 2; }
        if (Case.EndsWith(TEXT("BaseSwallow"))) { V = 1; F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 16777216.f); }
        if (Case.EndsWith(TEXT("SumOverflow")) || Case.EndsWith(TEXT("Swallow")))
        {
            F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 0);
            const bool Overflow = Case.EndsWith(TEXT("SumOverflow"));
            const double Large = Overflow ? (Attack ? double(MAX_flt) : std::numeric_limits<double>::max()) : (Attack ? 16777216. : 9007199254740992.);
            const bool Reverse = Case.EndsWith(TEXT("ReverseSwallow"));
            F.Effect(TEXT("Old"), Kind, Reverse ? 1 : Large);
            if (!Ok(TEXT("旧合法来源"), F.Add(TEXT("A01"), F.Refs({TEXT("Old")})))) { return false; }
            V = Reverse || Overflow ? Large : 1;
        }
        F.Effect(TEXT("N"), Case.EndsWith(TEXT("Enum")) ? static_cast<EEffect>(255) : Kind, V);
        const float Before = F.Attack(); const double BeforeRate = F.Income().State.Rate;
        const auto R = F.Add(TEXT("Z01"), F.Refs({TEXT("N")}));
        const bool Valid = Case.EndsWith(TEXT("Zero")) || Case.EndsWith(TEXT("Fraction")) || Case.EndsWith(TEXT("NormalRounding"));
        if (Valid) { Ok(TEXT("零、小数、通常舍入合法"), R); if (Attack && V > 0) { TestTrue(TEXT("真实GAS有正加值"), F.Attack() > Before); } }
        else { Bad(TEXT("非法数值必须拒绝"), R); TestEqual(TEXT("失败攻击原子"), F.Attack(), Before); TestEqual(TEXT("失败速率原子"), F.Income().State.Rate, BeforeRate); }
        return true;
    }
    if (Case.StartsWith(TEXT("Invalid.")))
    {
        auto Refs = F.Refs({TEXT("A"), TEXT("R")}); FString Key = TEXT("E01"); ESource Kind = ESource::Equipment;
        if (Case.EndsWith(TEXT("WrongType"))) { Refs[1].Type = EY01TableKind::Equipment; }
        if (Case.EndsWith(TEXT("Empty"))) { Refs[1] = FY01DataRef(); }
        if (Case.EndsWith(TEXT("MissingRow"))) { Refs[1].RowName = TEXT("Absent"); }
        if (Case.EndsWith(TEXT("MissingTable"))) { F.Data.Tables.Remove(EY01TableKind::SourceEffect); }
        if (Case.EndsWith(TEXT("WrongSchema"))) { F.Data.Tables[EY01TableKind::SourceEffect] = F.Data.Tables[EY01TableKind::Skill]; }
        if (Case.EndsWith(TEXT("Duplicate"))) { Refs[1] = Refs[0]; }
        if (Case.EndsWith(TEXT("CaseDuplicate"))) { Refs[1].RowName = TEXT("a"); }
        if (Case.EndsWith(TEXT("BadKey"))) { Key = TEXT("../P2/E01"); }
        if (Case.EndsWith(TEXT("BadKind"))) { Kind = static_cast<ESource>(255); }
        Bad(TEXT("预检全部引用后才允许GE"), F.Add(Key, Refs, Kind));
        TestEqual(TEXT("引用错误没有半份攻击"), F.Attack(), 30.f); TestEqual(TEXT("没有半份收入"), F.Income().State.Rate, 0.); return true;
    }
    if (Case.StartsWith(TEXT("Income.")))
    {
        double Rate = Case == TEXT("Income.Zero") ? 0 : Case == TEXT("Income.Subunit") ? .25 : Case == TEXT("Income.Fraction") ? 1.5 : Case == TEXT("Income.GrossOverflow") ? std::numeric_limits<double>::max()
            : Case == TEXT("Income.Underflow") ? std::numeric_limits<double>::min() : Case == TEXT("Income.RemainderSwallow") ? 1e-20
            : Case == TEXT("Income.DecimalPartitions") ? .1 : 1;
        F.Effect(TEXT("R"), EEffect::GoldPerSecond, Rate);
        if (!Ok(TEXT("基础收入来源"), F.Add(TEXT("Income"), F.Refs({TEXT("R")}), ESource::Base))) { return false; }
        if (Case == TEXT("Income.Waiting") || Case == TEXT("Income.StopWaiting"))
        {
            F.Now = 10; Ok(TEXT("Waiting结算幂等"), F.I()->Settle()); TestEqual(TEXT("Waiting不入账"), F.P->GetGold(), 0);
            if (Case.EndsWith(TEXT("StopWaiting"))) { Ok(TEXT("Waiting也可永久停止"), F.I()->Stop()); Bad(TEXT("停止不能启动"), F.I()->Start()); }
            else { Ok(TEXT("Waiting后首次启动"), F.I()->Start()); F.Now = 11; Ok(TEXT("只赚启动之后"), F.I()->Settle()); TestEqual(TEXT("不补Waiting"), F.P->GetGold(), 1); }
            return true;
        }
        if (!Ok(TEXT("服务器Start"), F.I()->Start())) { return false; }
        if (Case == TEXT("Income.Zero") || Case == TEXT("Income.Subunit"))
        {
            F.Now = 3; Ok(TEXT("零/小数正式结算"), F.I()->Settle()); TestEqual(TEXT("不提前凑整"), F.P->GetGold(), 0);
            TestEqual(TEXT("小数余额分开"), F.Income().State.Remainder, Case.EndsWith(TEXT("Subunit")) ? .75 : 0.);
            F.Now = 4; Ok(TEXT("下一终点"), F.I()->Settle()); TestEqual(TEXT("按公式整数到账"), F.P->GetGold(), Case.EndsWith(TEXT("Subunit")) ? 1 : 0); return true;
        }
        if (Case == TEXT("Income.Partitions") || Case == TEXT("Income.DecimalPartitions"))
        {
            auto* Other = F.Player(); Ok(TEXT("第二玩家初始化"), Other->GetSourceEffects()->Initialize(F.Match, 2));
            FY01PreparedSource Plan; Ok(TEXT("第二玩家预检"), Other->GetSourceEffects()->PrepareSource(FY01DataCatalog(F.Data.Tables), ESource::Base, TEXT("Income"), F.Refs({TEXT("R")}), Plan));
            FY01SourceTransaction Tx; Ok(TEXT("第二玩家暂存"), Other->GetSourceEffects()->StageSource(Plan, Tx)); Ok(TEXT("第二玩家提交"), Other->GetSourceEffects()->CommitSource(Tx)); Ok(TEXT("第二玩家启动"), Other->GetIncome()->Start());
            for (int32 K = 1; K <= 80; ++K) { F.Now = K * .125; Ok(TEXT("小分组"), F.I()->Settle()); }
            Ok(TEXT("相同终点大分组"), Other->GetIncome()->Settle());
            TestEqual(TEXT("余额分组严格一致"), F.P->GetGold(), Other->GetGold());
            TestEqual(TEXT("余量分组严格一致"), F.Income().State.Remainder, Other->GetIncome()->Observe().State.Remainder, 0.); return true;
        }
        if (Case == TEXT("Income.RateChange") || Case == TEXT("Income.RemoveEarned") || Case == TEXT("Income.FiveToFour"))
        {
            if (Case.EndsWith(TEXT("FiveToFour")))
            {
                F.Effect(TEXT("Extra"), EEffect::GoldPerSecond, 4); Ok(TEXT("底层来源叠加4而非购买"), F.Add(TEXT("E01"), F.Refs({TEXT("Extra")})));
                TestEqual(TEXT("速率合计5"), F.Income().State.Rate, 5.); F.Now = .25;
                Ok(TEXT("移除基础1"), F.S()->RemoveSource(ESource::Base, TEXT("Income"))); TestEqual(TEXT("剩余速率4"), F.Income().State.Rate, 4.);
            }
            else
            {
                F.Now = 1.25;
                if (Case.EndsWith(TEXT("RemoveEarned"))) { Ok(TEXT("卸下"), F.S()->RemoveSource(ESource::Base, TEXT("Income"))); }
                else { Ok(TEXT("追加速率1"), F.Add(TEXT("E01"), F.Refs({TEXT("R")}))); }
            }
            TestEqual(TEXT("变速前已赚1"), F.P->GetGold(), 1); TestEqual(TEXT("变速继承0.25"), F.Income().State.Remainder, .25);
            F.Now += .5; Ok(TEXT("新段"), F.I()->Settle());
            TestEqual(TEXT("卸下不扣回已赚"), F.P->GetGold(), Case.EndsWith(TEXT("RemoveEarned")) ? 1 : Case.EndsWith(TEXT("FiveToFour")) ? 3 : 2);
            TestEqual(TEXT("小数不丢"), F.Income().State.Remainder, .25); return true;
        }
        if (Case == TEXT("Income.FailureKeepsEarned"))
        {
            F.Now = 1.25; F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda([](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { return false; }));
            Bad(TEXT("请求后续GE失败"), F.Add(TEXT("E01"), F.Refs({TEXT("A"), TEXT("R")})));
            TestEqual(TEXT("合法旧段已赚不回滚"), F.P->GetGold(), 1); TestEqual(TEXT("合法余量保留"), F.Income().State.Remainder, .25); TestEqual(TEXT("失败不加新速率"), F.Income().State.Rate, 1.); return true;
        }
        if (Case == TEXT("Income.StartStop") || Case == TEXT("Income.EndPlay") || Case == TEXT("Income.DestroyOwner"))
        {
            F.Now = 1.25; Ok(TEXT("重复Start不重置"), F.I()->Start());
            if (Case.EndsWith(TEXT("EndPlay"))) { F.I()->EndPlay(EEndPlayReason::Quit); }
            else if (Case.EndsWith(TEXT("DestroyOwner"))) { F.P->DispatchBeginPlay(); F.P->Destroy(); }
            else { Ok(TEXT("最终结算"), F.I()->Stop()); }
            TestEqual(TEXT("最终余额"), F.P->GetGold(), 1); TestEqual(TEXT("最终余量"), F.Income().State.Remainder, .25);
            F.Now = 10; F.I()->Stop(); F.I()->EndPlay(EEndPlayReason::Quit); Bad(TEXT("退出不能重启"), F.I()->Start());
            TestEqual(TEXT("重复停止不多发"), F.P->GetGold(), 1); return true;
        }
        if (Case == TEXT("Income.NoPawnDeath"))
        { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); TestNull(TEXT("没有Pawn"), F.P->GetPawn()); F.Now = 2; Ok(TEXT("死亡无Pawn仍结算"), F.I()->Settle()); TestEqual(TEXT("收入不依赖Pawn"), F.P->GetGold(), 2); return true; }
        if (Case == TEXT("Income.Ring32"))
        {
            for (int K = 1; K <= 40; ++K) { F.Now = K; Ok(TEXT("流水"), F.I()->Settle()); }
            const auto V = F.Income(); TestEqual(TEXT("最近32条"), V.State.Recent.Num(), 32); TestEqual(TEXT("审计累计40"), V.State.TotalIncome, int64(40));
            if (V.State.Recent.Num() == 32) { const auto& L = V.State.Recent.Last(); TestEqual(TEXT("流水序列40"), L.Sequence, int64(40)); TestEqual(TEXT("流水余额"), L.Balance, 40); TestEqual(TEXT("流水速率"), L.Rate, 1.); TestEqual(TEXT("流水时间"), L.Time, 40.); TestEqual(TEXT("自然收入类型"), L.Kind, EY01IncomeEntryKind::Natural); }
            return true;
        }
        F.Now = 1;
        if (Case == TEXT("Income.GoldMax")) { F.P->TryAddMatchReward(MAX_int32 - 1, 0); }
        if (Case == TEXT("Income.GoldOverflow") || Case == TEXT("Income.FaultPermanent")) { F.P->TryAddMatchReward(MAX_int32, 0); }
        if (Case == TEXT("Income.AuditMax")) { FY01R36RuntimeTestAccess::Audit(F.I(), MAX_int64 - 1); }
        if (Case == TEXT("Income.AuditOverflow")) { FY01R36RuntimeTestAccess::Audit(F.I(), MAX_int64); }
        if (Case == TEXT("Income.SequenceOverflow")) { FY01R36RuntimeTestAccess::Sequence(F.I(), MAX_int64); }
        if (Case == TEXT("Income.WholeInt64Boundary")) { F.Now = 9223372036854775808.; }
        if (Case == TEXT("Income.WholeBelowInt64")) { F.Now = std::nextafter(9223372036854775808., 0.); }
        if (Case == TEXT("Income.TimeBackwards")) { F.Now = -.1; }
        if (Case == TEXT("Income.TimeNaN")) { F.Now = std::numeric_limits<double>::quiet_NaN(); }
        if (Case == TEXT("Income.ElapsedOverflow")) { F.Now = std::numeric_limits<double>::infinity(); }
        if (Case == TEXT("Income.GrossOverflow")) { F.Now = 2; }
        if (Case == TEXT("Income.Underflow")) { F.Now = std::numeric_limits<double>::min(); }
        const auto Before = F.Income(); const auto R = F.I()->Settle();
        if (Case == TEXT("Income.GoldMax") || Case == TEXT("Income.AuditMax"))
        { Ok(TEXT("精确边界仍可入账"), R); TestEqual(TEXT("边界整额"), F.P->GetGold(), Before.Balance + 1); }
        else
        {
            Bad(TEXT("自然结算整次拒绝"), R); TestEqual(TEXT("不半入账"), F.P->GetGold(), Before.Balance); TestEqual(TEXT("保留最后有效余量"), F.Income().State.Remainder, Before.State.Remainder);
            TestEqual(TEXT("审计不虚增"), F.Income().State.TotalIncome, Before.State.TotalIncome); TestEqual(TEXT("停止并IncomeFault"), F.Income().State.Phase, EY01IncomePhase::Fault);
            if (Case == TEXT("Income.WholeInt64Boundary")) { TestEqual(TEXT("2^63在cast前拒绝"), R.Code, EError::IntegerRange); }
            if (Case == TEXT("Income.WholeBelowInt64")) { TestEqual(TEXT("2^63下方能安全cast随后拒绝金币容量"), R.Code, EError::BalanceRange); }
            if (Case == TEXT("Income.FaultPermanent"))
            { FindFProperty<FIntProperty>(AY01PlayerState::StaticClass(), TEXT("Gold"))->SetPropertyValue_InContainer(F.P, 0); F.Now = 2; Bad(TEXT("Fault不自动补发"), F.I()->Settle()); Bad(TEXT("Fault不能重启"), F.I()->Start()); TestEqual(TEXT("腾出钱包也不补发"), F.P->GetGold(), 0); }
        }
        return true;
    }
    if (Case == TEXT("SessionIdentity"))
    { Ok(TEXT("相同会话初始化幂等"), F.Init()); Bad(TEXT("不可换Match"), F.S()->Initialize(FGuid::NewGuid(), 1)); Bad(TEXT("不可换JoinSeq"), F.S()->Initialize(F.Match, 2)); return true; }
    if (Case == TEXT("ClientMutation"))
    { F.P->SetRole(ROLE_SimulatedProxy); Bad(TEXT("客户端来源拒绝"), F.Add(TEXT("E01"), F.Refs({TEXT("A")}))); Bad(TEXT("客户端收入拒绝"), F.I()->Start()); F.P->SetRole(ROLE_Authority); return true; }
    if (Case == TEXT("SortAndIdentity"))
    {
        for (ESource Kind : {ESource::Shop, ESource::Talent, ESource::Skill, ESource::Equipment, ESource::Base})
        { Ok(TEXT("逆序注册"), F.Add(TEXT("Z"), F.Refs({TEXT("R"), TEXT("B"), TEXT("A")}), Kind)); }
        Ok(TEXT("LocalKey排序"), F.Add(TEXT("A"), F.Refs({TEXT("A")})));
        FY01SourceSnapshot V; Ok(TEXT("正式快照"), F.S()->Observe(V)); TestEqual(TEXT("每份效果独立明细"), V.Effects.Num(), 16);
        if (V.Effects.Num() == 16)
        { TestEqual(TEXT("基础优先"), V.Effects[0].Source.Kind, ESource::Base); TestEqual(TEXT("EffectId字典序"), V.Effects[0].EffectId, FName(TEXT("A"))); TestEqual(TEXT("装备LocalKey字典序"), V.Effects[3].Source.LocalKey, FString(TEXT("A"))); TestEqual(TEXT("最后Shop"), V.Effects.Last().Source.Kind, ESource::Shop);
            TestEqual(TEXT("服务器身份格式"), V.Effects[0].Source.Id, FString::Printf(TEXT("R36/%s/P1/Base/Z"), *F.Match.ToString(EGuidFormats::DigitsWithHyphens))); }
        TestEqual(TEXT("基础攻击分开"), V.BaseAttack, 30.f); TestEqual(TEXT("最终取真实ASC"), V.CurrentAttack, F.Attack()); return true;
    }
    FY01PreparedSource Plan; if (!Ok(TEXT("来源预检"), F.Prepare(Plan, TEXT("E01"), F.Refs({TEXT("A"), TEXT("R")})))) { return false; }
    if (Case == TEXT("ForeignPlan") || Case == TEXT("MultiPlayer"))
    {
        auto* Other = F.Player(); Ok(TEXT("另一玩家会话"), Other->GetSourceEffects()->Initialize(F.Match, 2)); FY01SourceTransaction Tx;
        Bad(TEXT("计划不可跨owner"), Other->GetSourceEffects()->StageSource(Plan, Tx));
        Ok(TEXT("自己的来源"), F.Add(TEXT("E01"), F.Refs({TEXT("A"), TEXT("R")}))); F.I()->Start(); Other->GetIncome()->Start(); F.Now = 2; F.I()->Settle(); Other->GetIncome()->Settle();
        TestEqual(TEXT("自己收入"), F.P->GetGold(), 2); TestEqual(TEXT("他人不串账"), Other->GetGold(), 0); TestEqual(TEXT("他人攻击未变"), Other->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), 30.f); return true;
    }
    if (Case == TEXT("StalePlan"))
    { Ok(TEXT("改变版本"), F.Add(TEXT("E02"), F.Refs({TEXT("B")}))); FY01SourceTransaction Tx; Bad(TEXT("旧预检不能提交"), F.S()->StageSource(Plan, Tx)); TestEqual(TEXT("旧来源未动"), F.Attack(), 37.f); return true; }
    if (Case.StartsWith(TEXT("Stage")) || Case == TEXT("SnapshotDuringStage") || Case == TEXT("ForeignTransaction"))
    {
        FY01SourceTransaction Tx; if (!Ok(TEXT("正式暂存"), F.S()->StageSource(Plan, Tx))) { return false; }
        TestEqual(TEXT("暂存使用真实GE"), F.Attack(), 35.f); TestEqual(TEXT("暂存不生效收入"), F.Income().State.Rate, 0.);
        FY01SourceSnapshot View; Bad(TEXT("不发布半事务快照"), F.S()->Observe(View));
        if (Case == TEXT("ForeignTransaction")) { auto* Other = F.Player(); Other->GetSourceEffects()->Initialize(F.Match, 2); Bad(TEXT("外来事务拒绝"), Other->GetSourceEffects()->CommitSource(Tx)); }
        if (Case == TEXT("StageCommit")) { Ok(TEXT("最终提交"), F.S()->CommitSource(Tx)); TestEqual(TEXT("提交收入"), F.Income().State.Rate, 1.); }
        else { Ok(TEXT("后续GA/余额步骤失败可回滚"), F.S()->RollbackSource(Tx)); TestEqual(TEXT("精确恢复真实攻击"), F.Attack(), 30.f); TestEqual(TEXT("无残留GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 0); }
        return true;
    }
    if (!Ok(TEXT("旧来源注册"), F.Add(TEXT("E01"), F.Refs({TEXT("A"), TEXT("R")})))) { return false; }
    const auto Handles = F.ASC()->GetActiveEffects(FGameplayEffectQuery());
    if (Case == TEXT("ChangedPayload")) { F.Effect(TEXT("A"), EEffect::AttackFlat, 6); Bad(TEXT("同source不同payload拒绝"), F.Add(TEXT("E01"), F.Refs({TEXT("A"), TEXT("R")}))); TestEqual(TEXT("旧值保留"), F.Attack(), 35.f); return true; }
    if (Case == TEXT("RepeatPayload"))
    { Ok(TEXT("重复source幂等且输入顺序无关"), F.Add(TEXT("E01"), F.Refs({TEXT("R"), TEXT("A")}))); TestEqual(TEXT("重复不多加"), F.Attack(), 35.f); TestEqual(TEXT("重复不多GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 1); return true; }
    if (Case == TEXT("DetachedHandle"))
    { F.ASC()->RemoveActiveGameplayEffect(Handles[0]); Bad(TEXT("失效句柄明确拒绝"), F.S()->RemoveSource(ESource::Equipment, TEXT("E01"))); return true; }
    if (Case == TEXT("SecondGEFailure") || Case.StartsWith(TEXT("Reentry.")))
    {
        int Calls = 0; FDelegateHandle H; FY01RuntimeResult Nested;
        if (Case == TEXT("SecondGEFailure"))
        { F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda([&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { return ++Calls != 2; })); }
        auto Reenter = [&]()
        {
            if (++Calls != 1) { return; }
            if (Case.EndsWith(TEXT("Stop"))) { Nested = F.I()->Stop(); }
            else if (Case.EndsWith(TEXT("Initialize"))) { Nested = F.Init(); }
            else { Nested = F.S()->RemoveSource(ESource::Equipment, TEXT("E01")); }
        };
        const bool Attribute = Case == TEXT("Reentry.Attribute") || Case == TEXT("Reentry.Remove");
        if (Case.StartsWith(TEXT("Reentry.")))
        {
            if (Attribute) { H = F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).AddLambda([&](const FOnAttributeChangeData&) { Reenter(); }); }
            else { H = F.ASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda([&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle) { Reenter(); }); }
        }
        const auto R = Case == TEXT("Reentry.Remove") ? F.S()->RemoveSource(ESource::Equipment, TEXT("E01")) : F.Add(TEXT("E02"), F.Refs({TEXT("A"), TEXT("B"), TEXT("R")}));
        if (Attribute) { F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).Remove(H); }
        else { F.ASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(H); }
        F.ASC()->GameplayEffectApplicationQueries.Reset();
        Bad(TEXT("失败或回调重入整次回滚"), R); TestTrue(TEXT("真实GAS回调确实执行"), Calls > 0);
        if (Case == TEXT("SecondGEFailure")) { TestEqual(TEXT("真实第二次GE应用查询拒绝"), Calls, 2); }
        TestEqual(TEXT("旧来源真实攻击保持"), F.Attack(), 35.f); TestEqual(TEXT("旧来源速率保持"), F.Income().State.Rate, 1.);
        TestNotNull(TEXT("精确旧handle保留"), F.ASC()->GetActiveGameplayEffect(Handles[0]));
        TestEqual(TEXT("没有残留半份GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 1); return true;
    }
    if (Case == TEXT("MultipleEffects"))
    { Ok(TEXT("一source多攻击与收入"), F.Add(TEXT("E02"), F.Refs({TEXT("A"), TEXT("B"), TEXT("R")}))); TestEqual(TEXT("真实最终47"), F.Attack(), 47.f); TestEqual(TEXT("收入合计2"), F.Income().State.Rate, 2.); return true; }
    AddError(TEXT("未覆盖的R36运行时测试分支：") + Case); return false;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36MutationTest, "Y01.Framework.R36.Effects.Mutation",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36MutationTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("ExchangeTwo"), TEXT("ReplaceSameKey"), TEXT("FinalNotTransientOverflow"),
        TEXT("RollbackExact"), TEXT("SecondQueryRejected"), TEXT("LostOldBefore"), TEXT("LostOldStaged"),
        TEXT("LostFirstNew"), TEXT("LostSecondNew"), TEXT("DuplicateRemove"), TEXT("DuplicateAdd"),
        TEXT("DuplicateEffect"), TEXT("InvalidIdentity"), TEXT("ForeignRemoval"), TEXT("ForeignPlan"),
        TEXT("ForeignTransaction"), TEXT("Session"), TEXT("StaleRevision"), TEXT("Replay"), TEXT("SnapshotOwned"),
        TEXT("ScopeRollback"), TEXT("ScopeCommit"), TEXT("ScopeWeakOwner"), TEXT("StageBusy"),
        TEXT("Reentry.Inhibit"), TEXT("Reentry.Apply"), TEXT("Reentry.CommitDelete"), TEXT("Reentry.RollbackDelete"),
        TEXT("Reentry.Restore"), TEXT("End.Inhibit"), TEXT("End.Apply"), TEXT("End.CommitDelete"),
        TEXT("End.RollbackDelete"), TEXT("End.Restore"), TEXT("Stop.Stage"), TEXT("Stop.Inhibit"),
        TEXT("Stop.Apply"), TEXT("Stop.Rollback"), TEXT("StableOrder"), TEXT("ExternalModifier"),
        TEXT("QualificationSnapshot"), TEXT("ExternalState") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36MutationTest::RunTest(const FString& Case)
{
    static_assert(!std::is_copy_constructible_v<FY01ScopedSourceMutation>);
    static_assert(!std::is_move_constructible_v<FY01ScopedSourceMutation>);
    FRuntimeWorld F;
    auto Ok = [&](const TCHAR* M, const FY01RuntimeResult& R)
    { if (!R.IsOk()) { AddInfo(R.Message); } return TestTrue(M, R.IsOk()); };
    auto Code = [&](const TCHAR* M, const FY01RuntimeResult& R, EError E) { TestEqual(M, R.Code, E); };
    if (!Ok(TEXT("初始化真实World和ASC"), F.Init())) { return false; }
    const bool Overflow = Case == TEXT("FinalNotTransientOverflow");
    const bool Ordered = Case == TEXT("StableOrder");
    if (Overflow)
    {
        F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 0);
        F.Effect(TEXT("A"), EEffect::AttackFlat, double(MAX_flt) * .375);
        F.Effect(TEXT("B"), EEffect::AttackFlat, double(MAX_flt) * .375);
    }
    if (Ordered)
    {
        F.Effect(TEXT("A"), EEffect::AttackFlat, 3);
        F.Effect(TEXT("B"), EEffect::AttackFlat, 3);
        F.Effect(TEXT("Large"), EEffect::AttackFlat, 16777216);
        if (!Ok(TEXT("逆序加入大值"), F.Add(TEXT("Z"), F.Refs({TEXT("Large")})))) { return false; }
    }
    if (!Ok(TEXT("旧来源一"), F.Add(TEXT("Old1"), F.Refs({TEXT("A"), TEXT("R")})))
        || !Ok(TEXT("旧来源二"), F.Add(TEXT("Old2"), F.Refs({TEXT("B"), TEXT("R")})))) { return false; }
    if (Ordered) { TestEqual(TEXT("稳定排序基线16777252"), F.Attack(), 16777252.f); }
    FActiveGameplayEffectHandle External;
    if (Case == TEXT("ExternalModifier"))
    {
        auto Spec = F.ASC()->MakeOutgoingSpec(UY01EquipmentAttackEffect::StaticClass(), 1, F.ASC()->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_EquipmentAttack, 11);
        External = F.ASC()->ApplyGameplayEffectSpecToSelf(*Spec.Data);
        TestTrue(TEXT("真实外部GE"), External.IsValid());
    }
    if (Case == TEXT("ExternalState"))
    {
        auto* GE = NewObject<UGameplayEffect>(); GE->DurationPolicy = EGameplayEffectDurationType::Infinite;
        auto* Stacking = FindFProperty<FEnumProperty>(UGameplayEffect::StaticClass(), TEXT("StackingType"));
        if (!TestNotNull(TEXT("真实GE叠层配置字段"), Stacking)) { return false; }
        Stacking->GetUnderlyingProperty()->SetIntPropertyValue(Stacking->ContainerPtrToValuePtr<void>(GE),
            static_cast<int64>(EGameplayEffectStackingType::AggregateByTarget));
        GE->StackLimitCount = 3;
        auto& Mod = GE->Modifiers.AddDefaulted_GetRef(); Mod.Attribute = UY01AttributeSet::GetAttackDamageAttribute();
        Mod.ModifierOp = EGameplayModOp::Additive; Mod.ModifierMagnitude = FScalableFloat(11);
        Mod.SourceTags.RequireTags.AddTag(Y01Tags::Effect_Damage);
        FGameplayEffectSpec Spec(GE, F.ASC()->MakeEffectContext(), 1); Spec.SetStackCount(3);
        External = F.ASC()->ApplyGameplayEffectSpecToSelf(Spec);
        TestTrue(TEXT("带标签资格与叠层的真实外部GE"), External.IsValid());
    }
    auto Mods = [&]()
    {
        FGameplayEffectAttributeCaptureSpec Capture(FGameplayEffectAttributeCaptureDefinition(
            UY01AttributeSet::GetAttackDamageAttribute(), EGameplayEffectAttributeCaptureSource::Target, false));
        F.ASC()->CaptureAttributeForGameplayEffect(Capture);
        FAggregator Snapshot; TestTrue(TEXT("只读复制真实聚合器不求值"), Capture.AttemptGetAttributeAggregatorSnapshot(Snapshot));
        TArray<FAggregatorMod> Result;
        Snapshot.ForEachMod([&](const FAggregatorModInfo& Info) { Result.Add(*Info.Mod); });
        return Result;
    };
    const auto BeforeMods = Mods();
    auto UnchangedMods = [&]()
    {
        const auto AfterMods = Mods(); TestEqual(TEXT("modifier数量保持"), AfterMods.Num(), BeforeMods.Num());
        for (int32 I = 0; I < FMath::Min(AfterMods.Num(), BeforeMods.Num()); ++I)
        {
            const auto& A = AfterMods[I]; const auto& B = BeforeMods[I];
            TestEqual(TEXT("modifier原handle与相对顺序"), A.ActiveHandle, B.ActiveHandle);
            TestEqual(TEXT("预检/回滚不污染原modifier资格"), A.Qualifies(), B.Qualifies());
            TestEqual(TEXT("modifier幅度"), A.EvaluatedMagnitude, B.EvaluatedMagnitude);
            TestEqual(TEXT("modifier叠层字段"), A.StackCount, B.StackCount);
            TestEqual(TEXT("modifier预测标志"), A.IsPredicted, B.IsPredicted);
            TestTrue(TEXT("modifier原标签要求指针"), A.SourceTagReqs == B.SourceTagReqs && A.TargetTagReqs == B.TargetTagReqs);
        }
    };
    Ok(TEXT("旧收入启动"), F.I()->Start());
    FY01SourceSnapshot Before; Ok(TEXT("已提交快照"), F.S()->Observe(Before));
    auto Removal = [&](const TCHAR* Key)
    { return Before.Effects.FindByPredicate([&](const auto& E) { return E.Source.LocalKey == Key; })->Source; };
    TArray<FY01RuntimeSourceId> Removals{Removal(TEXT("Old1")), Removal(TEXT("Old2"))};
    TArray<FY01ResolvedSource> Adds{
        {ESource::Equipment, Case == TEXT("ReplaceSameKey") ? TEXT("Old1") : TEXT("New1"),
            {{TEXT("Attack"), EEffect::AttackFlat, Overflow ? double(MAX_flt) * .375 : 9}, {TEXT("Rate"), EEffect::GoldPerSecond, 3}}},
        {ESource::Skill, TEXT("New2"), {{TEXT("Attack"), EEffect::AttackFlat, Overflow ? double(MAX_flt) * .375 : 11}, {TEXT("Rate"), EEffect::GoldPerSecond, 4}}}};
    const auto OldHandles = F.ASC()->GetActiveEffects(FGameplayEffectQuery());
    const float OldCurrent = F.Attack();
    FY01PreparedMutation Plan;
    if (!Ok(TEXT("批量最终集合预检应成功"), F.S()->PrepareMutation(Adds, Removals, Plan))) { return false; }
    if (Case == TEXT("QualificationSnapshot")) { UnchangedMods(); return true; }
    if (Case == TEXT("ExternalState")) { UnchangedMods(); }
    auto Restored = [&]()
    {
        FY01SourceSnapshot After; Ok(TEXT("回滚后公开快照"), F.S()->Observe(After));
        TestEqual(TEXT("精确base不改"), After.BaseAttack, Before.BaseAttack);
        TestEqual(TEXT("精确current不补差"), After.CurrentAttack, OldCurrent);
        TestEqual(TEXT("失败版本不动"), After.Revision, Before.Revision);
        TestEqual(TEXT("来源明细完整"), After.Effects.Num(), Before.Effects.Num());
        TestEqual(TEXT("旧速率完整"), F.Income().State.Rate, 2.);
        TestEqual(TEXT("没有半份新GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), OldHandles.Num());
        for (auto H : OldHandles)
        {
            const auto* GE = F.ASC()->GetActiveGameplayEffect(H);
            if (TestNotNull(TEXT("原始handle原样恢复而非重建"), GE))
            { TestFalse(TEXT("原资格恢复"), GE->bIsInhibited); TestEqual(TEXT("原叠层"), GE->Spec.GetStackCount(), Case == TEXT("ExternalState") && H == External ? 3 : 1); }
        }
        if (Case == TEXT("ExternalState")) { UnchangedMods(); }
    };
    if (Case == TEXT("DuplicateRemove") || Case == TEXT("DuplicateAdd") || Case == TEXT("DuplicateEffect")
        || Case == TEXT("InvalidIdentity") || Case == TEXT("ForeignRemoval"))
    {
        EError Expected = EError::InvalidIdentity;
        if (Case == TEXT("DuplicateRemove")) { const auto Copy = Removals[0]; Removals.Add(Copy); }
        if (Case == TEXT("DuplicateAdd")) { const auto Copy = Adds[0]; Adds.Add(Copy); }
        if (Case == TEXT("DuplicateEffect")) { const auto Copy = Adds[1].Effects[0]; Adds[1].Effects.Add(Copy); Expected = EError::DuplicateEffect; }
        if (Case == TEXT("InvalidIdentity")) { Adds[1].LocalKey = TEXT("../invalid"); }
        if (Case == TEXT("ForeignRemoval")) { Removals[1].Id.ReplaceInline(TEXT("/P1/"), TEXT("/P2/")); }
        FY01PreparedMutation Invalid;
        Code(TEXT("非法批量输入明确拒绝"), F.S()->PrepareMutation(Adds, Removals, Invalid), Expected);
        Restored(); return true;
    }
    FY01SourceTransaction Tx;
    if (Case == TEXT("ForeignPlan") || Case == TEXT("Session"))
    {
        auto* Other = F.Player(); Ok(TEXT("不同owner即使相同session"), Other->GetSourceEffects()->Initialize(F.Match, 1));
        Code(TEXT("计划不能跨owner"), Other->GetSourceEffects()->StageMutation(Plan, Tx), EError::Stale);
        Code(TEXT("已初始化session不可覆盖"), F.S()->Initialize(FGuid::NewGuid(), 2), EError::Session);
        Restored(); return true;
    }
    if (Case == TEXT("StaleRevision"))
    {
        Ok(TEXT("另一次事务改变revision"), F.Add(TEXT("Extra"), F.Refs({TEXT("R")})));
        Code(TEXT("批量计划版本失效"), F.S()->StageMutation(Plan, Tx), EError::Stale); return true;
    }
    if (Case == TEXT("LostOldBefore"))
    {
        F.ASC()->RemoveActiveGameplayEffect(OldHandles.Last());
        Code(TEXT("任一旧handle丢失在stage拒绝"), F.S()->StageMutation(Plan, Tx), EError::HandleLost); return true;
    }
    if (Case == TEXT("SnapshotOwned")) { Adds[0].Effects[0].Magnitude = 999; Adds.Reset(); Removals.Reset(); }
    int32 Calls = 0;
    FDelegateHandle ApplyHook, RemoveHook;
    TArray<TPair<FActiveGameplayEffectHandle, FDelegateHandle>> InhibitHooks;
    auto Callback = [&]()
    {
        if (++Calls != 1) { return; }
        FY01SourceSnapshot Hidden; Code(TEXT("委托不能观察半事务"), F.S()->Observe(Hidden), EError::Busy);
        if (Case.StartsWith(TEXT("End."))) { F.Now = 2; F.S()->EndPlay(EEndPlayReason::Quit); F.Now = 9; }
        else if (Case.StartsWith(TEXT("Stop.")))
        { F.Now = 2; Code(TEXT("首次Stop忙且冻结"), F.I()->Stop(), EError::Busy); F.Now = 3; F.I()->Stop(); F.Now = 9; }
        else { Code(TEXT("真实委托业务重入Busy"), F.I()->Settle(), EError::Busy); }
    };
    if (Case.EndsWith(TEXT("Inhibit")) || Case.EndsWith(TEXT("Restore")))
    {
        for (auto H : OldHandles)
        {
            auto* Event = F.ASC()->OnGameplayEffectInhibitionChangedDelegate(H);
            const auto Hook = Event->AddLambda([&](FActiveGameplayEffectHandle, bool Inhibited)
            { if (Inhibited != Case.EndsWith(TEXT("Restore"))) { Callback(); } });
            InhibitHooks.Add({H, Hook});
        }
    }
    if (Case.EndsWith(TEXT("Apply")))
    { ApplyHook = F.ASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda([&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle) { Callback(); }); }
    if (Case.Contains(TEXT("Delete")) || Case == TEXT("Stop.Rollback"))
    { RemoveHook = F.ASC()->OnAnyGameplayEffectRemovedDelegate().AddLambda([&](const FActiveGameplayEffect&) { Callback(); }); }
    if (Case == TEXT("SecondQueryRejected"))
    { F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda([&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { return ++Calls != 2; })); }
    F.Now = 1.25;
    const auto Stage = F.S()->StageMutation(Plan, Tx);
    const bool StageFails = Case.EndsWith(TEXT("Inhibit")) || Case.EndsWith(TEXT("Apply")) || Case == TEXT("SecondQueryRejected");
    if (StageFails)
    {
        Code(TEXT("真实GAS批量中断"), Stage, Case == TEXT("SecondQueryRejected") ? EError::EffectRejected : EError::Interrupted);
        TestTrue(TEXT("真实GAS委托/query已经执行"), Calls > 0);
        if (Case == TEXT("SecondQueryRejected")) { TestEqual(TEXT("第二个来源GE真实拒绝"), Calls, 2); }
    }
    else if (!Ok(TEXT("多来源真实暂存"), Stage)) { return false; }
    else
    {
        FY01SourceSnapshot Hidden; Code(TEXT("stage不公开半事务"), F.S()->Observe(Hidden), EError::Busy);
        TestEqual(TEXT("stage收入仍旧速率"), F.Income().State.Rate, 2.);
        for (auto H : OldHandles)
        {
            const auto* GE = F.ASC()->GetActiveGameplayEffect(H);
            if (TestNotNull(TEXT("提交前旧handle仍存在"), GE))
            { TestEqual(TEXT("只抑制待移除来源"), GE->bIsInhibited, H != External && (!Ordered || GE->Spec.GetSetByCallerMagnitude(Y01Tags::Data_EquipmentAttack) != 16777216.f)); }
        }
        if (Case == TEXT("LostOldStaged") || Case == TEXT("LostFirstNew") || Case == TEXT("LostSecondNew"))
        {
            auto Live = F.ASC()->GetActiveEffects(FGameplayEffectQuery());
            Live.RemoveAll([&](auto H) { return OldHandles.Contains(H); });
            F.ASC()->RemoveActiveGameplayEffect(Case == TEXT("LostOldStaged") ? OldHandles.Last() : Live[Case == TEXT("LostFirstNew") ? 0 : 1]);
            Code(TEXT("提交检查每一个handle"), F.S()->CommitMutation(Tx), EError::HandleLost);
            if (Case != TEXT("LostOldStaged")) { Restored(); }
            return true;
        }
        if (Case == TEXT("ForeignTransaction"))
        {
            auto* Other = F.Player(); Other->GetSourceEffects()->Initialize(F.Match, 2);
            Code(TEXT("外来commit拒绝"), Other->GetSourceEffects()->CommitMutation(Tx), EError::Stale);
            Code(TEXT("外来rollback拒绝"), Other->GetSourceEffects()->RollbackMutation(Tx), EError::Stale);
        }
        if (Case.StartsWith(TEXT("Scope")))
        {
            TWeakObjectPtr<UY01SourceEffectsComponent> Weak = F.S();
            {
                FY01ScopedSourceMutation Guard(MoveTemp(Tx));
                if (Case == TEXT("ScopeCommit")) { Ok(TEXT("作用域提交"), Guard.Commit()); }
                if (Case == TEXT("ScopeWeakOwner")) { F.S()->EndPlay(EEndPlayReason::Quit); F.S()->MarkAsGarbage(); }
            }
            if (Case == TEXT("ScopeWeakOwner"))
            { TestFalse(TEXT("守卫不持有强owner"), Weak.IsValid()); TestEqual(TEXT("终止清除所有真实GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 0); return true; }
        }
        else if (Case == TEXT("StageBusy") || Case == TEXT("Stop.Stage"))
        {
            if (Case == TEXT("StageBusy"))
            { FY01PreparedMutation Nested; Code(TEXT("stage与commit之间重入"), F.S()->PrepareMutation({}, {}, Nested), EError::Busy); }
            else { F.Now = 2; Code(TEXT("外层暂存时Stop"), F.I()->Stop(), EError::Busy); F.Now = 9; }
            Code(TEXT("中断事务不可提交"), F.S()->CommitMutation(Tx), EError::Interrupted);
        }
        else if (Case == TEXT("RollbackExact") || Ordered || Case == TEXT("ExternalModifier") || Case == TEXT("ExternalState") || Case == TEXT("ForeignTransaction")
            || Case.EndsWith(TEXT("RollbackDelete")) || Case.EndsWith(TEXT("Restore")) || Case == TEXT("Stop.Rollback"))
        { Ok(TEXT("显式逆序回滚"), F.S()->RollbackMutation(Tx)); }
        else
        {
            F.Now = 2;
            Ok(TEXT("批量原子提交"), F.S()->CommitMutation(Tx));
            if (Case == TEXT("Replay"))
            {
                Code(TEXT("提交令牌不重放"), F.S()->CommitMutation(Tx), EError::Stale);
                Code(TEXT("提交后不回滚"), F.S()->RollbackMutation(Tx), EError::Stale);
                Code(TEXT("旧计划不重放"), F.S()->StageMutation(Plan, Tx), EError::Stale);
            }
        }
    }
    F.ASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(ApplyHook);
    F.ASC()->OnAnyGameplayEffectRemovedDelegate().Remove(RemoveHook);
    F.ASC()->GameplayEffectApplicationQueries.Reset();
    for (const auto& Pair : InhibitHooks)
    { if (auto* E = F.ASC()->OnGameplayEffectInhibitionChangedDelegate(Pair.Key)) { E->Remove(Pair.Value); } }
    if (Case.StartsWith(TEXT("End.")))
    {
        TestTrue(TEXT("结束来自真实GAS委托"), Calls > 0);
        TestEqual(TEXT("批量结束全部清理"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 0);
        TestEqual(TEXT("结束回到base"), F.Attack(), Before.BaseAttack);
        TestEqual(TEXT("结束在首次请求时停止"), F.Income().State.LastTime, 2.);
        const auto Ended = F.Income(); F.Now = 20; F.S()->EndPlay(EEndPlayReason::Quit);
        TestEqual(TEXT("重复EndPlay无重复流水"), F.Income().State.Sequence, Ended.State.Sequence); return true;
    }
    if (Case.StartsWith(TEXT("Stop.")))
    {
        TestEqual(TEXT("批量Stop只赚到2秒"), F.P->GetGold(), 4);
        TestEqual(TEXT("批量Stop边界"), F.Income().State.LastTime, 2.);
        Code(TEXT("Stop后普通Remove拒绝"), F.S()->RemoveSource(ESource::Equipment, TEXT("Old1")), EError::Stopped);
    }
    const bool Committed = Case == TEXT("ExchangeTwo") || Case == TEXT("ReplaceSameKey") || Overflow || Case == TEXT("Replay")
        || Case == TEXT("SnapshotOwned") || Case == TEXT("ScopeCommit") || Case == TEXT("Reentry.CommitDelete");
    if (Committed)
    {
        FY01SourceSnapshot After; Ok(TEXT("最终公开快照"), F.S()->Observe(After));
        TestEqual(TEXT("只递增一个revision"), After.Revision, Before.Revision + 1);
        TestEqual(TEXT("提交最终rate"), F.Income().State.Rate, 7.);
        TestEqual(TEXT("两新GE无旧GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 2);
        for (auto H : OldHandles) { TestNull(TEXT("只有commit永久删除旧handle"), F.ASC()->GetActiveGameplayEffect(H)); }
        TestEqual(TEXT("最终真实攻击"), F.Attack(), Overflow ? OldCurrent : 50.f);
        F.Now = 2.25; Ok(TEXT("新段从stage捕获时刻计费"), F.I()->Settle()); TestEqual(TEXT("合法旧2.5与新7"), F.P->GetGold(), 9);
    }
    else { Restored(); }
    if (!Case.StartsWith(TEXT("Stop.")) && !Committed)
    { TestEqual(TEXT("旧合法收入绝不追回"), F.P->GetGold(), 2); TestEqual(TEXT("旧合法余量保留"), F.Income().State.Remainder, .5); }
    if (Case.StartsWith(TEXT("Reentry."))) { TestTrue(TEXT("真实重入已发生"), Calls > 0); }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36LifecycleIntegrationTest, "Y01.Framework.R36.RuntimeIntegration.Lifecycle",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36LifecycleIntegrationTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Waiting"), TEXT("Preparing"), TEXT("BattlePublish"), TEXT("DuplicateInitialization"),
        TEXT("Death"), TEXT("Region"), TEXT("Ended"), TEXT("Logout"), TEXT("LateJoin"), TEXT("FourSources"),
                TEXT("EndedSources"), TEXT("EndedReentry"), TEXT("Rejoin"), TEXT("ExplicitFinish"),
                TEXT("GateWaiting"), TEXT("GateDead"), TEXT("GateEnded"), TEXT("Ledger"),
                                TEXT("ControlledDeath"), TEXT("ControlledDisabled"), TEXT("ControlledWrongToken"),
                                TEXT("ControlledNatural"), TEXT("ControlledWaiting"), TEXT("ControlledEnded"), TEXT("LifecycleTrace"),
                                        TEXT("ControlledS3Death"), TEXT("ControlledS3Disabled"), TEXT("ControlledS3WrongToken"),
                                        TEXT("ControlledS3Natural"), TEXT("ControlledS3Waiting"), TEXT("ControlledS3Ended"),
                                                TEXT("S4.ContentDeathOn"), TEXT("S4.ContentDeathOff"), TEXT("S4.EvidenceDeathOn"), TEXT("S4.EvidenceDeathOff"),
                                                TEXT("S4.ContentFinalOn"), TEXT("S4.ContentFinalOff"), TEXT("S4.EvidenceFinalOn"), TEXT("S4.EvidenceFinalOff"),
                                                TEXT("S4.PendingPublish"), TEXT("AutomaticStart")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36LifecycleIntegrationTest::RunTest(const FString& Case)
{
    using K = EY01TableKind; using Op = EY01ItemOperation;
    const FString OldCommand = FCommandLine::Get(); FCommandLine::Set(TEXT(""));
    ON_SCOPE_EXIT { FCommandLine::Set(*OldCommand); };
    FY01MatchFixture F; auto* Profile = NewObject<UY01R36Profile>(); double Now = 0;
    auto& C = F.Row<FY01CharacterRow>(K::Character); C.InitialEquipment.Reset(); C.InitialSkills.Reset(); C.AttackDamage = 30; C.Health = 300;
    FY01SourceEffectRow Effect; Effect.Magnitude = 5; F.Add(K::SourceEffect, Effect, TEXT("Attack"));
    Effect.Kind = EY01SourceEffectKind::GoldPerSecond; Effect.Magnitude = 1; F.Add(K::SourceEffect, Effect, TEXT("Income"));
    const TArray<FY01DataRef> Effects = {F.Ref(K::SourceEffect, TEXT("Attack")), F.Ref(K::SourceEffect, TEXT("Income"))};
    for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Blue"))})
    { auto& E = F.Row<FY01EquipmentRow>(K::Equipment, Name); E.EffectIds = Effects; E.DisplayName = FText::FromString(TEXT("经济装备")); }
    const auto MonsterSkill = F.Row<FY01SkillRow>(K::Skill); F.Add(K::Skill, MonsterSkill, TEXT("MonsterBasic"));
    F.Row<FY01MonsterRow>(K::Monster).Skills = {F.Ref(K::Skill, TEXT("MonsterBasic"))};
    auto& Book = F.Row<FY01SkillRow>(K::Skill); Book.Type = EY01SkillType::Passive; Book.Ability.Reset(); Book.EffectIds = Effects;
    Book.DisplayName = FText::FromString(TEXT("经济被动书"));
    FY01LocalItemPoolRow BookPool; BookPool.ItemType = K::Skill; FY01WeightedEntry Entry; Entry.Item = F.Ref(K::Skill); BookPool.Entries.Add(Entry);
    F.Add(K::LocalItemPool, BookPool, TEXT("Books"));
    auto& Offer = F.Row<FY01ShopOfferRow>(K::ShopOffer); Offer.Price = 10; Offer.DisplayName = FText::FromString(TEXT("购买装备"));
    auto BookOffer = Offer; BookOffer.Pool = F.Ref(K::LocalItemPool, TEXT("Books")); BookOffer.DisplayName = FText::FromString(TEXT("购买书"));
    F.Add(K::ShopOffer, BookOffer, TEXT("Book"));
    FY01ShopOfferRow Direct; Direct.Price = 50; Direct.ShopKind = EY01ShopKind::DirectEffect; Direct.EffectIds = Effects;
    Direct.DisplayName = FText::FromString(TEXT("直接增益")); F.Add(K::ShopOffer, Direct, TEXT("Direct"));
    FY01TalentChoiceRow Choice;
    for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Two")), FName(TEXT("Three"))})
    {
        FY01TalentRow Talent; Talent.DisplayName = FText::FromString(TEXT("一次天赋")); Talent.EffectIds = Effects;
        F.Add(K::Talent, Talent, Name); Choice.Candidates.Add(F.Ref(K::Talent, Name));
    }
    F.Add(K::TalentChoice, Choice);
    F.Row<FY01RewardPackageRow>(K::RewardPackage).Gold = 0; F.Row<FY01RewardPackageRow>(K::RewardPackage).Experience = 0;
    F.Row<FY01MonsterRow>(K::Monster).Health = 1000000; F.Row<FY01MonsterRow>(K::Monster).AttackDamage = 0;
    Profile->bEconomyEnabled = true; Profile->InitialGold = 200; Profile->BaseGoldPerSecond = 1; Profile->bTrainingEnabled = true;
    Profile->ContentShopOffers = {F.Ref(K::ShopOffer), F.Ref(K::ShopOffer, TEXT("Book")), F.Ref(K::ShopOffer, TEXT("Direct"))};
    Profile->ContentRecipes = {F.Ref(K::Synthesis)}; Profile->TalentChoice = F.Ref(K::TalentChoice);
    Profile->Character = F.Ref(K::Character); Profile->OrderedWaves = F.Waves();
    if (Case == TEXT("ExplicitFinish") || Case.StartsWith(TEXT("S4."))) { Profile->OrderedWaves.SetNum(1); }
    if (Case.StartsWith(TEXT("S4."))) { FCommandLine::Set(TEXT("-R36Scenario=controlled")); }
    const bool S4 = Case.StartsWith(TEXT("S4.")) || Case == TEXT("AutomaticStart");
    auto* Evidence = FindFProperty<FBoolProperty>(Profile->GetClass(), TEXT("bEvidenceMode"));
    if (Evidence) { Evidence->SetPropertyValue_InContainer(Profile, !S4 || Case.Contains(TEXT("Evidence"))); }
    if (S4 && Case.EndsWith(TEXT("Off"))) { Profile->bEconomyEnabled = false; }
    if (S4)
    {
        auto& Wave = F.Row<FY01WaveRow>(K::Wave); Wave.PreparationSeconds = .1f;
        Wave.Spawns = {{F.Ref(K::Monster), 1, 0}, {F.Ref(K::Monster), 1, 2}};
    }
    for (const auto& Pair : F.Tables) { Profile->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); };
    World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
    World->GetWorldSettings()->DefaultGameMode = AY01R36RuntimeProbe::StaticClass(); World->SetGameMode(FURL());
    auto* Mode = World->GetAuthGameMode<AY01R36RuntimeProbe>(); World->SpawnActor<AY01R36Config>()->Content = Profile;
    auto AddPlayer = [&]()
    {
        auto* PC = World->SpawnActor<AY01R36Controller>(); auto* PS = PC->GetPlayerState<AY01PlayerState>();
        if (!PS) { PS = World->SpawnActor<AY01PlayerState>(); PC->PlayerState = PS; }
        PS->SetOwner(PC); Mode->GetFlowState()->AddPlayerState(PS);
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(0, 0, 100), FRotator::ZeroRotator); PC->Possess(Pawn);
        PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn); Pawn->GetCharacterMovement()->DisableMovement();
        FY01R36RuntimeTestAccess::Clock(PS->GetIncome(), &Now); return PC;
    };
    auto* PC = AddPlayer(); auto* PS = PC->GetPlayerState<AY01PlayerState>(); auto* L = PS->GetLoadout(); auto* Income = PS->GetIncome();
    FString Error; if (!TestTrue(TEXT("真实配置加载"), Mode->LoadMatchConfiguration(Error))) { AddError(Error); return false; }
    World->BeginPlay();
    auto View = [&]() { L->PublishCommittedView(); FY01CommittedView V; L->ReadCommittedView(V); return V; };
    auto Request = [&](Op Operation)
    {
        const auto V = View(); FY01ItemRequest R; R.MatchId = V.MatchId; R.JoinSequence = V.JoinSequence; R.Seq = V.ExpectedSeq;
        R.ConfigRevision = V.ConfigRevision; R.Operation = Operation; return R;
    };
    auto Buy = [&](FName Id) { auto R = Request(Op::Purchase); R.Offer = F.Ref(K::ShopOffer, Id); return L->SubmitRequest(PC, R); };
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    auto Tick = [&]() { ++GFrameCounter; World->Tick(LEVELTICK_All, .05f); };
    if (Case == TEXT("AutomaticStart"))
    {
        TestEqual(TEXT("初始化hook不递归启动"), Mode->GetFlowState()->GetWaveSnapshot().Phase, EY01MatchPhase::Waiting);
        TestTrue(TEXT("完整经济初始化成功"), PS->GetEconomy()->IsInitialized() && Mode->GetLoadoutError().IsEmpty());
        Tick();
        TestEqual(TEXT("无手动输入，权威Tick自动进入首波准备"), Mode->GetFlowState()->GetWaveSnapshot().Phase, EY01MatchPhase::Preparing);
        TestFalse(TEXT("自动策略拒绝手动重复开始"), Mode->TryStartNextWave());
        return true;
    }
    if (S4)
    {
        TestNotNull(TEXT("证据模式必须有独立公开配置"), Evidence);
        TestTrue(TEXT("明确手动开始"), Mode->TryStartNextWave());
        const bool IsEvidence = Case.Contains(TEXT("Evidence"));
        if (Case.Contains(TEXT("Death")))
        {
            PS->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0);
            Tick(); Tick();
            TestEqual(TEXT("死亡结束仅由证据模式决定，与经济开关无关"), Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended, !IsEvidence);
            if (IsEvidence) { TestTrue(TEXT("证据场仍允许显式结束"), Mode->TryEndMatch(false)); }
            return true;
        }
        for (int32 I = 0; I < 5; ++I) { Tick(); }
        TestEqual(TEXT("首批一只"), Mode->GetWaveMonsters().Num(), 1);
        for (AY01Monster* Monster : Mode->GetWaveMonsters()) { Monster->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        Tick();
        TestEqual(TEXT("延迟批次仍待生成"), Mode->GetFlowState()->GetWaveSnapshot().PendingSpawns, 1);
        TestFalse(TEXT("不能提前完成波次"), Mode->TryCompleteWave());
        if (Case == TEXT("S4.PendingPublish"))
        {
            const auto Start = Income->Observe().State.StartTime;
            Now = 1.25; for (int32 I = 0; I < 20; ++I) { View(); Tick(); }
            TestEqual(TEXT("重复发布不能重启收入"), Income->Observe().State.StartTime, Start);
            TestEqual(TEXT("真实收入仍累计"), Income->Observe().State.TotalIncome, int64(1));
            return true;
        }
        for (int32 I = 0; I < 45; ++I) { Tick(); }
        TestEqual(TEXT("第二批按时出现"), Mode->GetWaveMonsters().Num(), 2);
        for (AY01Monster* Monster : Mode->GetWaveMonsters()) { Monster->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        Tick(); Tick();
        TestEqual(TEXT("最后波自动结束仅由证据模式决定"), Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended, !IsEvidence);
        if (IsEvidence) { TestTrue(TEXT("证据场末波仍显式结束"), Mode->TryEndMatch(false)); }
        else { TestEqual(TEXT("自然成功结束"), Mode->GetFlowState()->GetWaveSnapshot().Result, EY01MatchResult::Success); }
        return true;
    }
    TestTrue(TEXT("真实初始化hook启用经济"), PS->GetEconomy()->IsInitialized());
    TestEqual(TEXT("仅配置初始200金币"), View().Gold, 200); TestEqual(TEXT("初始基础GPS1"), View().IncomeRate, 1.);
    TestEqual(TEXT("空包"), View().Items.Num(), 0);
    if (Case.StartsWith(TEXT("Gate")))
    {
        if (Case != TEXT("GateWaiting")) { TestTrue(TEXT("操作矩阵进入真实准备阶段"), Mode->TryStartNextWave()); }
        if (Case == TEXT("GateDead")) { PS->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        if (Case == TEXT("GateEnded")) { TestTrue(TEXT("操作矩阵真实结束"), Mode->TryEndMatch(false)); }
        for (uint8 I = 0; I <= static_cast<uint8>(Op::ChooseTalent); ++I)
        {
            auto R = Request(static_cast<Op>(I));
            TestEqual(TEXT("所有九种库存经济意图统一阶段/存活拒绝"), L->SubmitRequest(PC, R).Code,
                Case == TEXT("GateDead") ? EY01ItemError::Dead : EY01ItemError::Phase);
        }
        return true;
    }
    if (Case == TEXT("Waiting"))
    {
        Now = 5; Tick(); TestEqual(TEXT("Waiting无收入"), PS->GetGold(), 200);
        TestEqual(TEXT("Waiting收入未启动"), Income->Observe().State.Phase, EY01IncomePhase::Waiting);
        TestEqual(TEXT("Waiting拒绝实际购买"), Buy(TEXT("Default")).Code, EY01ItemError::Phase); return true;
    }
    if (Case.StartsWith(TEXT("Controlled")))
    {
        const FString Branch = Case.Replace(TEXT("ControlledS3"), TEXT("Controlled"));
        const FString Label = FString(Case.StartsWith(TEXT("ControlledS3")) ? TEXT("R36-S3-Visible-LifeFixture-") : TEXT("R36-S2-Runtime-LifeFixture-"))
            + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
        const FString Directory = FPaths::ProjectSavedDir() / Label; IFileManager::Get().MakeDirectory(*Directory, true);
        const FString FileToken = Branch == TEXT("ControlledWrongToken") ? FGuid::NewGuid().ToString(EGuidFormats::Digits) : Token;
        const FString FileName = Branch == TEXT("ControlledEnded") ? TEXT("ended.request") : TEXT("death.request");
        TestTrue(TEXT("只创建新隔离生命周期令牌"), FFileHelper::SaveStringToFile(FileToken, *(Directory / FileName), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
        FString Params = FString::Printf(TEXT("-R36Fixture=controlled -R36Lifecycle=controlled -R36Run=%s -R36StopToken=%s"), *Label, *Token);
        if (Branch == TEXT("ControlledDisabled")) { Params.ReplaceInline(TEXT("-R36Lifecycle=controlled"), TEXT("")); }
        if (Branch == TEXT("ControlledNatural")) { Params.ReplaceInline(TEXT("-R36Fixture=controlled"), TEXT("-R36Scenario=natural")); }
        if (Branch != TEXT("ControlledWaiting")) { TestTrue(TEXT("受控生命周期实际准备"), Mode->TryStartNextWave()); }
        FCommandLine::Set(*Params); Now = 2.25;
        for (int32 I = 0; I < 12; ++I) { Tick(); }
        const bool Dead = Branch == TEXT("ControlledDeath"), Ended = Branch == TEXT("ControlledEnded");
        TestEqual(TEXT("只有显式授权死亡夹具改变HP"), View().Health, Dead ? 0.f : 300.f);
        TestEqual(TEXT("只有显式授权结束夹具结束对局"), Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended, Ended);
        if (Dead) { TestEqual(TEXT("受控死亡后真实购买拒绝"), Buy(TEXT("Default")).Code, EY01ItemError::Dead); }
        Now = 4.5; for (int32 I = 0; I < 12; ++I) { Tick(); }
        TestEqual(TEXT("死亡收入继续而结束停止"), PS->GetGold(), Branch == TEXT("ControlledWaiting") ? 200 : Ended ? 202 : 204);
        TestTrue(TEXT("保留生命周期请求夹具"), IFileManager::Get().FileExists(*(Directory / FileName)));
        return true;
    }
    TestTrue(TEXT("真实Preparing阶段"), Mode->TryStartNextWave());
    TestEqual(TEXT("Preparing首次启动"), Income->Observe().State.Phase, EY01IncomePhase::Running);
    if (Case == TEXT("LifecycleTrace"))
    {
        TestTrue(TEXT("观测夹具实际进入个人房"), Mode->GetTraining()->Request(PC, true)); PC->CaptureObserver(Mode);
        bool Found = false;
        for (const auto& Text : Y01R36Trace::Capture(PC, true))
        {
            TSharedPtr<FJsonObject> J; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J);
            FString Kind; if (!J || !J->TryGetStringField(TEXT("kind"), Kind) || Kind != TEXT("SCOPE")) { continue; }
            double Phase = -1, Room = 0; bool Inside = false;
            Found = J->TryGetNumberField(TEXT("phase"), Phase) && J->TryGetBoolField(TEXT("inside"), Inside) && J->TryGetNumberField(TEXT("room"), Room);
            TestTrue(TEXT("只读Trace反映真实准备与个人房"), Found && Phase == static_cast<int32>(EY01MatchPhase::Preparing) && Inside && Room > 0);
        }
        TestTrue(TEXT("生命周期观测字段存在"), Found);
        APawn* Pawn = PC->GetPawn(); TestNotNull(TEXT("真实可移动Pawn"), Pawn);
        Mode->RefreshTarget(PS);
        const auto* Target = PS->GetCombatASC()->GetCombatTarget();
        for (const auto& Text : Y01R36Trace::Capture(PC, true))
        {
            TSharedPtr<FJsonObject> J; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J);
            FString Kind; if (!J || !J->TryGetStringField(TEXT("kind"), Kind) || Kind != TEXT("SCOPE")) { continue; }
            double X = 0, Y = 0, Distance = -1; bool Valid = false;
            TestTrue(TEXT("只读观测含出生位置与目标有效性"), J->TryGetNumberField(TEXT("pawnX"), X)
                && J->TryGetNumberField(TEXT("pawnY"), Y) && J->TryGetNumberField(TEXT("targetDistance"), Distance)
                && J->TryGetBoolField(TEXT("targetValid"), Valid));
            if (Pawn) { TestEqual(TEXT("观测真实X"), X, Pawn->GetActorLocation().X); TestEqual(TEXT("观测真实Y"), Y, Pawn->GetActorLocation().Y); }
            TestEqual(TEXT("目标合法性来自权威ASC"), Valid, PS->GetCombatASC()->IsValidCombatTarget(Target));
            TestEqual(TEXT("目标距离不伪造"), Distance, Pawn && Target ? FVector::Distance(Pawn->GetActorLocation(), Target->GetActorLocation()) : -1.);
        }
        return true;
    }
    if (Case == TEXT("ExplicitFinish"))
    {
        for (int32 I = 0; I < 40; ++I) { Tick(); }
        TestTrue(TEXT("真实阶段已刷怪"), Mode->GetWaveMonsters().Num() > 0);
        for (AY01Monster* Monster : Mode->GetWaveMonsters()) { Monster->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        Tick(); Tick();
        TestTrue(TEXT("末波完成等待显式结束"), Mode->GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Ended);
        TestTrue(TEXT("用户显式结束仍可执行"), Mode->TryEndMatch(false)); return true;
    }
    if (Case == TEXT("EndedReentry"))
    {
        Now = 2.25; int32 Calls = 0;
        const auto Hook = PS->GetCombatASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
            [&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle)
            { ++Calls; Mode->TryEndMatch(false); });
        const auto R = Buy(TEXT("Direct")); PS->GetCombatASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(Hook);
        TestTrue(TEXT("真实来源GE回调重入Ended"), Calls > 0); TestFalse(TEXT("结束期间购买不可提交"), R.IsOk());
        FY01SourceSnapshot Sources; PS->GetSourceEffects()->Observe(Sources);
        TestEqual(TEXT("退栈后无残留来源"), Sources.Effects.Num(), 0);
        TestEqual(TEXT("Ended回到基础攻击"), View().Stats.AttackDamage, 30.f);
        TestEqual(TEXT("合法收入只结算到结束边界"), Income->Observe().State.TotalIncome, int64(2));
        TestEqual(TEXT("拒绝不扣款"), Income->Observe().State.TotalSpending, int64(0)); return true;
    }
    if (Case == TEXT("Rejoin"))
    {
        Now = 2.25; Mode->Logout(PC); Now = 4.5;
        auto* Joined = AddPlayer(); Joined->Player = NewObject<ULocalPlayer>(GEngine); Mode->PostLogin(Joined);
        auto* B = Joined->GetPlayerState<AY01PlayerState>(); FY01CommittedView NewView; B->GetLoadout()->ReadCommittedView(NewView);
        TestNotEqual(TEXT("重入使用新连接身份"), NewView.JoinSequence, View().JoinSequence);
        Now = 5.5; Tick(); TestEqual(TEXT("退出者不获得离线收入"), PS->GetGold(), 202);
        TestEqual(TEXT("重入仅当前在线一秒"), B->GetGold(), 201); return true;
    }
    if (Case == TEXT("FourSources") || Case == TEXT("EndedSources") || Case == TEXT("Ledger"))
    {
        auto Expect = [&](float Attack, double Rate) { const auto V = View(); TestEqual(TEXT("完整视图攻击"), V.Stats.AttackDamage, Attack); TestEqual(TEXT("完整视图GPS"), V.IncomeRate, Rate); };
        Expect(30, 1);
        if (!TestTrue(TEXT("实际购买装备"), Buy(TEXT("Default")).IsOk()) || !TestTrue(TEXT("实际购买书"), Buy(TEXT("Book")).IsOk())) { return false; }
        Expect(30, 1); auto V = View(); const auto Equipment = V.Items[0]; const auto Skill = V.Items[1];
        auto Equip = Request(Op::Equip); Equip.ItemId = Equipment.ItemId; Equip.ExpectedEntityRevision = Equipment.EntityRevision; Equip.Slot = 0;
        TestTrue(TEXT("装备实际入槽"), L->SubmitRequest(PC, Equip).IsOk()); Expect(35, 2);
        auto Learn = Request(Op::PrepareLearn); Learn.ItemId = Skill.ItemId; Learn.ExpectedEntityRevision = Skill.EntityRevision; Learn.Slot = 0;
        TestTrue(TEXT("书实际学习"), L->SubmitRequest(PC, Learn).IsOk()); Expect(40, 3);
        auto Talent = Request(Op::ChooseTalent); Talent.TalentChoiceId = View().TalentChoice.ChoiceId; Talent.Talent = F.Ref(K::Talent);
        TestTrue(TEXT("固定天赋一次选择"), L->SubmitRequest(PC, Talent).IsOk()); Expect(45, 4);
        TestTrue(TEXT("直接商品实际购买"), Buy(TEXT("Direct")).IsOk()); Expect(50, 5);
        Now = 2.25; Tick(); TestEqual(TEXT("5速率2.25秒到账11"), Income->Observe().State.TotalIncome, int64(11));
        TestEqual(TEXT("小数保留0.25"), Income->Observe().State.Remainder, .25);
        TestFalse(TEXT("重复限购拒绝"), Buy(TEXT("Direct")).IsOk()); TestEqual(TEXT("拒绝不追回11收入"), Income->Observe().State.TotalIncome, int64(11));
        V = View(); const auto* Item = V.Items.FindByPredicate([&](const auto& I) { return I.ItemId == Equipment.ItemId; });
        auto Unequip = Request(Op::Unequip); Unequip.ItemId = Item->ItemId; Unequip.ExpectedEntityRevision = Item->EntityRevision; Unequip.Slot = 0;
        Unequip.ExpectedSlotItemId = Item->ItemId; Unequip.ExpectedSlotEntityRevision = Item->EntityRevision;
        TestTrue(TEXT("实际卸装"), L->SubmitRequest(PC, Unequip).IsOk()); Expect(45, 4);
        Now = 2.5; Tick(); TestEqual(TEXT("累计合法收入12"), Income->Observe().State.TotalIncome, int64(12));
        TestEqual(TEXT("累计合法支出70"), Income->Observe().State.TotalSpending, int64(70));
        TestEqual(TEXT("最终小数0.25"), Income->Observe().State.Remainder, .25); TestEqual(TEXT("唯一钱包142"), View().Gold, 142);
        if (Case == TEXT("Ledger"))
        {
            const auto Before = Income->Observe(); int32 Found = 0;
            auto CheckLedger = [&](const TSharedPtr<FJsonObject>& J)
            {
                if (!TestTrue(TEXT("真实账本JSON存在"), J.IsValid())) { return; }
                FString Total, Spending, Connection; double Balance = -1, Remainder = -1, Rate = -1;
                TestTrue(TEXT("收入累计精确十进制字符串"), J->TryGetStringField(TEXT("totalIncome"), Total));
                TestTrue(TEXT("支出累计精确十进制字符串"), J->TryGetStringField(TEXT("totalSpending"), Spending));
                TestEqual(TEXT("实际自然累计12"), Total, FString(TEXT("12")));
                TestEqual(TEXT("实际独立支出70"), Spending, FString(TEXT("70")));
                J->TryGetNumberField(TEXT("balance"), Balance); J->TryGetNumberField(TEXT("remainder"), Remainder);
                J->TryGetNumberField(TEXT("rate"), Rate); J->TryGetStringField(TEXT("connection"), Connection);
                TestEqual(TEXT("唯一钱包142"), Balance, 142.); TestEqual(TEXT("余量0.25"), Remainder, .25);
                TestEqual(TEXT("当前速率4"), Rate, 4.); TestEqual(TEXT("当前Owner身份"), Connection, View().ConnectionId.ToString());
            };
            for (const auto& Text : Y01R36Trace::Capture(PC, true))
            {
                TSharedPtr<FJsonObject> J; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J);
                FString Kind; if (J && J->TryGetStringField(TEXT("kind"), Kind) && Kind == TEXT("ECONOMY")) { ++Found; CheckLedger(J); }
            }
            TestEqual(TEXT("真实Capture输出一份本人账本"), Found, 1);
            TSharedPtr<FJsonObject> RequestJson; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Y01R36Trace::RequestRecord(PC)), RequestJson);
            const TSharedPtr<FJsonObject>* Ledger = nullptr;
            if (TestTrue(TEXT("逐请求记录附带同Owner账本"), RequestJson && RequestJson->TryGetObjectField(TEXT("economy"), Ledger))) { CheckLedger(*Ledger); }
            TestEqual(TEXT("只读Trace不推进收入序列"), Income->Observe().State.Sequence, Before.State.Sequence);
            TestEqual(TEXT("只读Trace不改变钱包"), PS->GetGold(), Before.Balance);
        }
        if (Case == TEXT("EndedSources"))
        {
            TestTrue(TEXT("四来源链后真实Ended"), Mode->TryEndMatch(false));
            FY01SourceSnapshot Sources; PS->GetSourceEffects()->Observe(Sources);
            TestEqual(TEXT("Ended全部来源清理"), Sources.Effects.Num(), 0);
            TestEqual(TEXT("Ended回到基础攻击而非丢失GE半视图"), View().Stats.AttackDamage, 30.f);
            Now = 8; Tick(); TestEqual(TEXT("结束后余额不变"), View().Gold, 142);
            TestFalse(TEXT("已终止来源不能重新初始化"), PS->GetSourceEffects()->Initialize(View().MatchId, View().JoinSequence).IsOk());
        }
        return true;
    }
    Now = 2.25;
    if (Case == TEXT("Death")) { PS->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    if (Case == TEXT("Region")) { TestTrue(TEXT("真实进入个人房"), Mode->GetTraining()->Request(PC, true)); }
    if (Case == TEXT("Ended")) { TestTrue(TEXT("真实Ended"), Mode->TryEndMatch(false)); }
    if (Case == TEXT("Logout")) { Mode->Logout(PC); }
    if (Case == TEXT("DuplicateInitialization")) { Mode->OnPlayerInitialized(PS); Mode->OnPlayerInitialized(PS); }
    if (Case == TEXT("LateJoin"))
    {
        auto* Other = AddPlayer(); Other->Player = NewObject<ULocalPlayer>(GEngine);
        Mode->PostLogin(Other); auto* B = Other->GetPlayerState<AY01PlayerState>();
        TestTrue(TEXT("LateJoin实际初始化经济"), B->GetEconomy()->IsInitialized());
        TestEqual(TEXT("LateJoin不追发离线收入"), B->GetGold(), 200);
        TestEqual(TEXT("LateJoin从当前Preparing开始"), B->GetIncome()->Observe().State.StartTime, Now);
    }
    Tick(); TestEqual(TEXT("在线或终止边界结算2金币"), PS->GetGold(), 202);
    if (Case == TEXT("BattlePublish"))
    {
        for (int32 I = 0; I < 40; ++I) { Tick(); }
        TestEqual(TEXT("真实计时进入Battle"), Mode->GetFlowState()->GetWaveSnapshot().Phase, EY01MatchPhase::Battle);
        TestEqual(TEXT("Battle刷怪Publish不重启收入"), Income->Observe().State.StartTime, 0.);
    }
    if (Case == TEXT("Death"))
    {
        TestTrue(TEXT("死亡不自动Ended"), Mode->GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Ended);
        TestEqual(TEXT("死亡实际购买拒绝"), Buy(TEXT("Default")).Code, EY01ItemError::Dead);
    }
    if (Case == TEXT("Region")) { TestTrue(TEXT("真实返回主线"), Mode->GetTraining()->Request(PC, false)); }
    Now = 4.5; Tick();
    const bool Stopped = Case == TEXT("Ended") || Case == TEXT("Logout");
    TestEqual(TEXT("死亡切区持续，Ended或Logout停止"), PS->GetGold(), Stopped ? 202 : 204);
    TestEqual(TEXT("分段小数保留"), Income->Observe().State.Remainder, Stopped ? .25 : .5);
    if (Stopped)
    {
        TestEqual(TEXT("终止收入状态"), Income->Observe().State.Phase, EY01IncomePhase::Stopped);
        Mode->OnPlayerInitialized(PS); TestFalse(TEXT("停止后不能重启收入"), Income->Start().IsOk());
    }
    else { TestEqual(TEXT("多次Tick或Publish不重启起点"), Income->Observe().State.StartTime, 0.); }
    return true;
}
#if WITH_EDITOR
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36S4RuntimeTest, "Y01.Framework.R36.S4.Runtime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R36S4RuntimeTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{ for (const TCHAR* N : {TEXT("NaturalThree"), TEXT("CompareSources")}) { Names.Add(N); Commands.Add(N); } }
bool FY01R36S4RuntimeTest::RunTest(const FString& Case)
{
    using K = EY01TableKind; using Op = EY01ItemOperation;
    const FString OldCommand = FCommandLine::Get();
    FString Root;
    const bool bReadOnly = FParse::Value(*OldCommand, TEXT("R36RuntimeFixtureRoot="), Root);
    if (bReadOnly && Root.IsEmpty()) { AddError(TEXT("只读S4夹具必须给出已有Root")); return false; }
    FCommandLine::Set(TEXT("-R36Profile=profile"));
    ON_SCOPE_EXIT { FCommandLine::Set(*OldCommand); };
    if (!bReadOnly)
    {
        Root = TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/S4_Runtime_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        AddInfo(TEXT("保留S4运行夹具：") + Root);
        if (!TestEqual(TEXT("S4数据入口"), NewObject<UY01R36AssetsCommandlet>()->Main(TEXT("-ContentS4 -Root=") + Root), 0)) { return false; }
    }
    else { AddInfo(TEXT("只读复用已有S4运行夹具：") + Root); }
    Y01R36Editor::FGeneration Generated;
    if (!TestTrue(TEXT("真实保存配置只读"), Y01R36Editor::Generate(Root, Generated, true))) { AddError(Generated.Error); return false; }
    const bool Natural = Case == TEXT("NaturalThree");
    auto* Profile = Generated.Profiles[Natural ? 1 : 2].Get();
    if (!TestTrue(TEXT("S4配置不是旧S1入口"), Profile->bEconomyEnabled && Profile->OrderedWaves.Num() == (Natural ? 3 : 2))) { return false; }
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); };
    World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
    World->GetWorldSettings()->DefaultGameMode = AY01R36RuntimeProbe::StaticClass(); World->SetGameMode(FURL());
    auto* Mode = World->GetAuthGameMode<AY01R36RuntimeProbe>(); World->SpawnActor<AY01R36Config>()->Content = Profile;
    double Now = 0;
    auto Player = [&]()
    {
        auto* PC = World->SpawnActor<AY01R36Controller>(); auto* PS = PC->GetPlayerState<AY01PlayerState>();
        if (!PS) { PS = World->SpawnActor<AY01PlayerState>(); PC->PlayerState = PS; }
        PS->SetOwner(PC); Mode->GetFlowState()->AddPlayerState(PS);
        FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(10000,0,100), FRotator::ZeroRotator, Params);
        PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn); Pawn->GetCharacterMovement()->DisableMovement();
        FY01R36RuntimeTestAccess::Clock(PS->GetIncome(), &Now); return PC;
    };
    auto* PC = Player(); auto* PS = PC->GetPlayerState<AY01PlayerState>(); auto* Other = Player()->GetPlayerState<AY01PlayerState>();
    FString Error; if (!TestTrue(TEXT("实际配置加载"), Mode->LoadMatchConfiguration(Error))) { AddError(Error); return false; }
    World->BeginPlay();
    auto* Loadout = PS->GetLoadout();
    auto View = [&]() { Loadout->PublishCommittedView(); FY01CommittedView V; Loadout->ReadCommittedView(V); return V; };
    auto Intent = [&](Op Operation)
    {
        const auto V = View(); FY01ItemRequest R; R.MatchId=V.MatchId; R.JoinSequence=V.JoinSequence;
        R.ConfigRevision=V.ConfigRevision; R.Seq=V.ExpectedSeq; R.Operation=Operation; return R;
    };
    TGuardValue<uint64> Frame(GFrameCounter, GFrameCounter);
    auto Tick = [&](int32 Count) { for (int32 I=0; I<Count; ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All,.05f); } };
    if (Natural)
    {
        TestEqual(TEXT("真实空包"), View().Items.Num(),0); TestEqual(TEXT("真实初始Gold0"),PS->GetGold(),0);
        const int32 First[] = {2,3,2}, Last[] = {1,1,2}, Delay[] = {2,3,2};
        for (int32 Wave=0; Wave<3; ++Wave)
        {
            TestFalse(TEXT("日常自动策略拒绝手动推进"),Mode->RequestAdvance(PC));
            if (Wave == 0) { Tick(1); }
            const double Deadline = Mode->GetFlowState()->GetWaveSnapshot().NextWaveDeadline;
            while (World->GetTimeSeconds() + .1 < Deadline) { Tick(1); }
            TestEqual(TEXT("截止前没有提前出下一波"),Mode->GetFlowState()->GetWaveSnapshot().WaveIndex,Wave-1);
            TestEqual(TEXT("仅首波Preparing，后续并行倒计时保持Battle"),Mode->GetFlowState()->GetWaveSnapshot().Phase,
                Wave == 0 ? EY01MatchPhase::Preparing : EY01MatchPhase::Battle);
            Tick(3);
            const int32 Previous = Wave == 0 ? 0 : Wave == 1 ? 3 : 7;
            TestEqual(TEXT("首批真实生成且保留旧波记录"),Mode->GetWaveMonsters().Num(),Previous+First[Wave]);
            for (AY01Monster* M:Mode->GetWaveMonsters()) { M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),0); }
            Tick(1); TestFalse(TEXT("首批死光不能早清波"),Mode->TryCompleteWave());
            TestEqual(TEXT("待生成末批"),Mode->GetFlowState()->GetWaveSnapshot().PendingSpawns,Last[Wave]);
            Tick(Delay[Wave]*20+1); TestEqual(TEXT("延迟批次完整出现"),Mode->GetWaveMonsters().Num(),Previous+First[Wave]+Last[Wave]);
            for (AY01Monster* M:Mode->GetWaveMonsters()) { M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),0); }
            Tick(2); TestEqual(TEXT("完成计数"),Mode->GetFlowState()->GetWaveSnapshot().CompletedWaves,Wave+1);
            const int32 Reward = FMath::Min(Wave+1,2)*300;
            TestEqual(TEXT("每人原清波链金币"),PS->GetGold(),Reward); TestEqual(TEXT("另一人独立收到全额"),Other->GetGold(),Reward);
            TestEqual(TEXT("清波无XP"),PS->GetExperience(),0); TestFalse(TEXT("奖励幂等"),Mode->TryCompleteWave());
            TestEqual(TEXT("只有最终波自然Ended"),Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended,Wave==2);
        }
        TestEqual(TEXT("自然胜利"),Mode->GetFlowState()->GetWaveSnapshot().Result,EY01MatchResult::Success);
        TestFalse(TEXT("结束后不能再推进"),Mode->RequestAdvance(PC)); return true;
    }
    TestTrue(TEXT("证据手动开始"),Mode->RequestAdvance(PC));
    auto Buy = [&](FName Name) { auto R=Intent(Op::Purchase); R.Offer=FY01MatchFixture::Ref(K::ShopOffer,Name); return Loadout->SubmitRequest(PC,R); };
    TestTrue(TEXT("实际购装备"),Buy(TEXT("EvidenceEquipment")).IsOk()); TestTrue(TEXT("实际购被动书"),Buy(TEXT("EvidenceBook")).IsOk());
    const auto Items=View().Items;
    if (!TestEqual(TEXT("实际两件入包"),Items.Num(),2)) { return false; }
    auto Equip=Intent(Op::Equip); Equip.ItemId=Items[0].ItemId; Equip.ExpectedEntityRevision=Items[0].EntityRevision; Equip.Slot=0;
    TestTrue(TEXT("实际装备"),Loadout->SubmitRequest(PC,Equip).IsOk());
    auto Learn=Intent(Op::PrepareLearn); Learn.ItemId=Items[1].ItemId; Learn.ExpectedEntityRevision=Items[1].EntityRevision; Learn.Slot=0;
    TestTrue(TEXT("实际学习"),Loadout->SubmitRequest(PC,Learn).IsOk());
    auto Talent=Intent(Op::ChooseTalent); Talent.TalentChoiceId=View().TalentChoice.ChoiceId;
    FY01DataValidation Q; Talent.Talent=Profile->BuildCatalog().FindRow<FY01TalentChoiceRow>(Profile->TalentChoice,Q)->Candidates[0];
    TestTrue(TEXT("实际一次天赋"),Loadout->SubmitRequest(PC,Talent).IsOk()); TestTrue(TEXT("实际直接商品"),Buy(TEXT("EvidenceBlessing")).IsOk());
    TestEqual(TEXT("Compare真实总ATK52"),View().Stats.AttackDamage,52.f); TestEqual(TEXT("Compare真实总GPS6"),View().IncomeRate,6.);
    TestEqual(TEXT("四来源支出70"),PS->GetGold(),130);
    const auto V=View(); const auto* Item=V.Items.FindByPredicate([&](const auto& I){return I.ItemId==Equip.ItemId;});
    auto Unequip=Intent(Op::Unequip); Unequip.ItemId=Item->ItemId; Unequip.ExpectedEntityRevision=Item->EntityRevision; Unequip.Slot=0;
    Unequip.ExpectedSlotItemId=Item->ItemId; Unequip.ExpectedSlotEntityRevision=Item->EntityRevision;
    TestTrue(TEXT("实际卸装"),Loadout->SubmitRequest(PC,Unequip).IsOk());
    TestEqual(TEXT("卸装ATK45"),View().Stats.AttackDamage,45.f); TestEqual(TEXT("卸装GPS4"),View().IncomeRate,4.);
    TestEqual(TEXT("另一玩家仍裸攻"),Other->GetCombatASC()->GetUnitStats().AttackDamage,30.f);
    return true;
}
#endif

namespace
{
struct FAutomaticWaveWorld
{
    FY01MatchFixture Data;
    UWorld* World = nullptr;
    AY01R36RuntimeProbe* Mode = nullptr;
    AY01R36Controller* PC = nullptr;
    AY01PlayerState* Player = nullptr;
    FString Error;
    explicit FAutomaticWaveWorld(const FString& Case)
    {
        using K = EY01TableKind;
        auto& C = Data.Row<FY01CharacterRow>(K::Character); C.InitialEquipment.Reset(); C.InitialSkills.Reset(); C.Health = 10000;
        auto& M = Data.Row<FY01MonsterRow>(K::Monster); M.Health = 1000; M.AttackDamage = 0;
        if (Case == TEXT("OldCombat")) { M.AttackDamage = 5; M.Range = 100; M.MoveSpeed = 120; C.AttackDamage = 0; C.Range = 100; }
        auto& A = Data.Row<FY01WaveRow>(K::Wave); A.PreparationSeconds = .2f;
        A.Spawns = {{Data.Ref(K::Monster), 1, 0}, {Data.Ref(K::Monster), 1, 2}};
        auto& B = Data.Row<FY01WaveRow>(K::Wave, TEXT("Boss")); B.PreparationSeconds = .3f;
        B.Spawns = {{Data.Ref(K::Monster), 1, 0}};
        auto Last = B; Last.PreparationSeconds = .4f; Last.Spawns[0].DelaySeconds = .1f;
        auto Reward = Data.Row<FY01RewardPackageRow>(K::RewardPackage); Reward.Gold = 20;
        Data.Add(K::RewardPackage, Reward, TEXT("Second")); B.Reward = Data.Ref(K::RewardPackage, TEXT("Second"));
        Reward.Gold = 30; Data.Add(K::RewardPackage, Reward, TEXT("Last")); Last.Reward = Data.Ref(K::RewardPackage, TEXT("Last"));
        Data.Add(K::Wave, Last, TEXT("Last"));
        auto* Profile = NewObject<UY01R36Profile>(); Profile->Character = Data.Ref(K::Character);
        Profile->OrderedWaves = {Data.Ref(K::Wave), Data.Ref(K::Wave, TEXT("Boss")), Data.Ref(K::Wave, TEXT("Last"))};
        Profile->bTrainingEnabled = true; Profile->bR34AutoAttackEnabled = Case == TEXT("OldCombat");
        Profile->bEvidenceMode = Case == TEXT("Evidence");
        if (Case == TEXT("ZeroWait"))
        {
            A.PreparationSeconds = B.PreparationSeconds = 0; Last.PreparationSeconds = 0;
            B.Type = Last.Type = EY01WaveType::Normal;
            Data.Add(K::Wave, Last, TEXT("Last"));
        }
        if (Case == TEXT("DuplicateRefs")) { Profile->OrderedWaves = {Data.Ref(K::Wave), Data.Ref(K::Wave)}; }
        for (const auto& Pair : Data.Tables) { Profile->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01R36RuntimeProbe::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01R36RuntimeProbe>();
        auto* Config = World->SpawnActor<AY01R36Config>(); Config->Content = Config->Baseline = Config->Compare = Profile;
        if (Case == TEXT("OldCombat"))
        {
            auto* Floor = World->SpawnActor<AStaticMeshActor>(FVector(10000, 0, -50), FRotator::ZeroRotator);
            Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
            Floor->SetActorScale3D(FVector(30, 30, 1)); Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
        }
        if (Case != TEXT("NoPlayers")) { PC = AddPlayer(false); Player = PC->GetPlayerState<AY01PlayerState>(); }
        Mode->LoadMatchConfiguration(Error); World->BeginPlay();
    }
    ~FAutomaticWaveWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    AY01R36Controller* AddPlayer(bool bLogin = true)
    {
        auto* Controller = World->SpawnActor<AY01R36Controller>(); auto* P = Controller->GetPlayerState<AY01PlayerState>();
        if (!P) { P = World->SpawnActor<AY01PlayerState>(); Controller->PlayerState = P; }
        P->SetOwner(Controller); Mode->GetFlowState()->AddPlayerState(P);
        FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(10000, 0, 100), FRotator::ZeroRotator, Params);
        Controller->Possess(Pawn); P->GetCombatASC()->InitAbilityActorInfo(P, Pawn); Pawn->GetCharacterMovement()->DisableMovement();
        if (bLogin) { Controller->Player = NewObject<ULocalPlayer>(GEngine); Mode->PostLogin(Controller); }
        return Controller;
    }
    void Tick(int32 Count = 1)
    { for (int32 I = 0; I < Count; ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .05f); } }
    const FY01WaveSnapshot& S() const { return Mode->GetFlowState()->GetWaveSnapshot(); }
    void Kill(int32 Index) { Mode->GetWaveMonsters()[Index]->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36AutomaticWavesTest, "Y01.Framework.R36.AutomaticWaves",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36AutomaticWavesTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Overlap"), TEXT("OutOfOrder"), TEXT("EndCancels"), TEXT("DeathWins"), TEXT("PreparingDeath"),
        TEXT("Training"), TEXT("ZeroWait"), TEXT("DuplicateRefs"), TEXT("NoPlayers"), TEXT("LateJoin"), TEXT("NoPawn"),
        TEXT("ClientTick"), TEXT("Observer"), TEXT("Evidence"), TEXT("Compare"), TEXT("Controlled"), TEXT("DeadlineOrigin"),
        TEXT("OldCombat"), TEXT("TrainingDeath"), TEXT("ObserverBoundary")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36AutomaticWavesTest::RunTest(const FString& Case)
{
    const FString Command = FCommandLine::Get(); FCommandLine::Set(TEXT(""));
    ON_SCOPE_EXIT { FCommandLine::Set(*Command); };
    TGuardValue<uint64> Frame(GFrameCounter, GFrameCounter);
    if (Case == TEXT("Compare")) { FCommandLine::Set(TEXT("-R36Profile=compare")); }
    if (Case == TEXT("Controlled")) { FCommandLine::Set(TEXT("-R36Scenario=controlled")); }
    FAutomaticWaveWorld F(Case);
    if (!TestTrue(TEXT("真实配置成功"), F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { AddError(F.Error); return false; }
    if (Case == TEXT("Evidence") || Case == TEXT("Compare") || Case == TEXT("Controlled"))
    {
        TestFalse(TEXT("隔离策略保持手动"), F.S().bAutomaticWaves); F.Tick(30);
        TestEqual(TEXT("手动场等待用户"), F.S().Phase, EY01MatchPhase::Waiting);
        TestTrue(TEXT("旧手动入口保留"), F.Mode->RequestAdvance(F.PC)); return true;
    }
    TestTrue(TEXT("R36日常策略无需新Run"), F.S().bAutomaticWaves);
    TestEqual(TEXT("配置后仍等权威Tick"), F.S().Phase, EY01MatchPhase::Waiting);
    if (Case == TEXT("NoPlayers"))
    {
        F.Tick(80); TestEqual(TEXT("无人服务器不倒计时"), F.S().Phase, EY01MatchPhase::Waiting);
        F.PC = F.AddPlayer(); F.Player = F.PC->GetPlayerState<AY01PlayerState>();
        TestTrue(TEXT("晚到玩家完整初始化"), F.Player->GetLoadout()->IsInitialized());
    }
    if (Case == TEXT("NoPawn"))
    {
        APawn* Pawn = F.PC->GetPawn(); F.PC->UnPossess(); F.Tick(20);
        TestEqual(TEXT("无有效Pawn不启动"), F.S().Phase, EY01MatchPhase::Waiting);
        F.PC->Possess(Pawn); F.Player->GetCombatASC()->InitAbilityActorInfo(F.Player, Pawn);
    }
    if (Case == TEXT("ClientTick"))
    {
        F.Mode->SetRole(ROLE_SimulatedProxy); F.Tick(20);
        TestEqual(TEXT("非权威不能调度"), F.S().Phase, EY01MatchPhase::Waiting);
        F.Mode->SetRole(ROLE_Authority);
    }
    F.Tick(); TestEqual(TEXT("无需Start进入首准备"), F.S().Phase, EY01MatchPhase::Preparing);
    const double Preparation = Case == TEXT("ZeroWait") ? 0 : .2f;
    TestTrue(TEXT("首deadline从就绪Tick起算"), FMath::IsNearlyEqual(F.S().NextWaveDeadline, F.World->GetTimeSeconds() + Preparation, 1.e-6));
    TestEqual(TEXT("尚未生成无最近出波"), F.S().WaveIndex, INDEX_NONE);
    TestFalse(TEXT("直接开始拒绝"), F.Mode->TryStartNextWave());
    TestFalse(TEXT("Enter请求拒绝"), F.Mode->RequestAdvance(F.PC));
    const double Deadline = F.S().NextWaveDeadline; F.PC->ServerPlaygroundAction_Implementation(4);
    TestEqual(TEXT("Action4不重置deadline"), F.S().NextWaveDeadline, Deadline);
    if (Case == TEXT("PreparingDeath"))
    {
        F.Player->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); F.Tick(80);
        TestEqual(TEXT("准备死亡失败"), F.S().Result, EY01MatchResult::Failure);
        TestEqual(TEXT("死亡取消首批"), F.Mode->GetWaveMonsters().Num(), 0); return true;
    }
    if (Case == TEXT("ZeroWait"))
    {
        F.Tick(); TestEqual(TEXT("零等待有限次迭代出齐三个波"), F.S().WaveIndex, 2);
        TestEqual(TEXT("首准备后Battle"), F.S().Phase, EY01MatchPhase::Battle); return true;
    }
    F.Tick(6);
    if (!TestEqual(TEXT("首波已实际生成"), F.Mode->GetWaveMonsters().Num(), 1)) { return false; }
    auto* Old = F.Mode->GetWaveMonsters()[0].Get(); const auto Session = Old->GetCombatASC()->GetCombatSession();
    const double Start = F.S().WaveStates[0].StartTime;
    if (Case != TEXT("DuplicateRefs"))
    { TestTrue(TEXT("下一波deadline基于上波实际开始+本波Preparation"), FMath::IsNearlyEqual(F.S().NextWaveDeadline, Start + double(.3f), 1.e-6)); }
    if (Case == TEXT("LateJoin"))
    {
        const double Next = F.S().NextWaveDeadline; auto* Other = F.AddPlayer(); F.Tick();
        TestTrue(TEXT("晚加入完整Loadout"), Other->GetPlayerState<AY01PlayerState>()->GetLoadout()->IsInitialized());
        TestEqual(TEXT("晚加入不重置上一波"), F.S().WaveStates[0].StartTime, Start);
        TestEqual(TEXT("晚加入不重置下一截止"), F.S().NextWaveDeadline, Next); return true;
    }
    if (Case == TEXT("EndCancels"))
    {
        TestTrue(TEXT("权威结束"), F.Mode->TryEndMatch(false)); F.Tick(100);
        TestEqual(TEXT("结束不再生成下一波和旧延迟批"), F.Mode->GetWaveMonsters().Num(), 1);
        TestEqual(TEXT("取消汇总pending"), F.S().TotalPendingSpawns, 0);
        TestEqual(TEXT("没有下一截止"), F.S().NextWaveIndex, INDEX_NONE); return true;
    }
    if (Case == TEXT("Training") || Case == TEXT("TrainingDeath"))
    {
        TestTrue(TEXT("进入训练"), F.Mode->GetTraining()->Request(F.PC, true));
        TestFalse(TEXT("主线怪不能跨区伤玩家"), Old->GetCombatASC()->IsCombatPairAllowed(F.Player->GetCombatASC()));
        F.Tick(18); TestEqual(TEXT("训练期间主线仍自动到第三波"), F.S().WaveIndex, 2);
        if (Case == TEXT("TrainingDeath"))
        {
            F.Player->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); F.Tick(100);
            TestEqual(TEXT("训练死亡结束整场而不是胜利"), F.S().Result, EY01MatchResult::Failure);
            TestEqual(TEXT("训练死亡取消旧波跨波批次"), F.Mode->GetWaveMonsters().Num(), 3);
            TestFalse(TEXT("训练会话一并清理"), F.Mode->GetTraining()->Observe(F.Player).bInside);
            TestEqual(TEXT("所有主线pending取消"), F.S().TotalPendingSpawns, 0); return true;
        }
        TestTrue(TEXT("返回主线"), F.Mode->GetTraining()->Request(F.PC, false));
        TestTrue(TEXT("旧怪返回后仍授权"), F.Player->GetCombatASC()->IsCombatPairAllowed(Old->GetCombatASC()));
    }
    else { F.Tick(18); }
    TestTrue(TEXT("下一波不销毁旧怪"), IsValid(Old) && !Old->IsActorBeingDestroyed());
    TestEqual(TEXT("普通换波不失效旧攻击会话"), Old->GetCombatASC()->GetCombatSession(), Session);
    TestEqual(TEXT("后续准备保持Battle"), F.S().Phase, EY01MatchPhase::Battle);
    if (Case == TEXT("DuplicateRefs"))
    {
        F.Tick(50); TestEqual(TEXT("同ref两波四只"), F.Mode->GetWaveMonsters().Num(), 4);
        for (int32 I = 0; I < 4; ++I) { F.Kill(I); } F.Tick(2);
        TestEqual(TEXT("幂等key是顺序号不是ref"), F.S().CompletedWaves, 2);
        TestEqual(TEXT("重复ref分别奖励"), F.Player->GetGold(), 20); return true;
    }
    if (!TestEqual(TEXT("旧批次未到但三波已开始"), F.Mode->GetWaveMonsters().Num(), 3)) { return false; }
    TestEqual(TEXT("最新波pending为零"), F.S().PendingSpawns, 0);
    TestEqual(TEXT("汇总保留第一波pending"), F.S().TotalPendingSpawns, 1);
    if (Case == TEXT("ObserverBoundary"))
    {
        F.Kill(0); F.PC->CaptureObserver(F.Mode);
        const auto BeforeTick = F.PC->GetObserver();
        TestEqual(TEXT("调度前观测Alive与逐波聚合来自同一快照"), BeforeTick.Alive, BeforeTick.Waves.TotalAliveMonsters);
        F.Tick(); F.PC->CaptureObserver(F.Mode);
        const auto AfterTick = F.PC->GetObserver();
        TestEqual(TEXT("下次权威Tick发布击杀后的全场存活数"), AfterTick.Alive, 2);
        TestEqual(TEXT("调度后观测与聚合同样本"), AfterTick.Alive, AfterTick.Waves.TotalAliveMonsters);
        TestEqual(TEXT("观测总量仍含旧波未出批次"), AfterTick.Total, 4);
        return true;
    }
    if (Case == TEXT("Observer"))
    {
        F.PC->CaptureObserver(F.Mode); const auto& O = F.PC->GetObserver();
        TestEqual(TEXT("Alive全场三只"), O.Alive, 3); TestEqual(TEXT("Total同范围含旧延迟四只"), O.Total, 4);
        TestEqual(TEXT("同样本包含汇总pending"), O.Waves.TotalPendingSpawns, 1);
        TestEqual(TEXT("逐波复制三条"), O.Waves.WaveStates.Num(), 3);
        bool Found = false;
        for (const auto& Text : Y01R36Trace::Capture(F.PC, true))
        {
            TSharedPtr<FJsonObject> J; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J);
            if (J && J->GetStringField(TEXT("kind")) == TEXT("WAVE"))
            { Found = J->GetBoolField(TEXT("automaticWaves")) && J->GetNumberField(TEXT("totalPendingSpawns")) == 1; }
        }
        TestTrue(TEXT("Trace输出新增聚合字段"), Found);
    }
    F.Kill(2); F.Tick();
    TestTrue(TEXT("末波先清独立结算"), F.S().WaveStates[2].bRewarded);
    TestEqual(TEXT("先收到末波30金币"), F.Player->GetGold(), 30);
    TestEqual(TEXT("旧字段仅代表最新波"), F.S().bWaveCompleted, true);
    TestTrue(TEXT("最新波完成不停止旧怪战斗"), F.Mode->IsCombatRunning());
    TestFalse(TEXT("末波先清不能胜利"), F.Mode->TryEndMatch(true));
    if (Case == TEXT("OldCombat"))
    {
        Old->SetActorLocation(F.PC->GetPawn()->GetActorLocation() + FVector(400, 0, 0));
        Old->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        const FVector Before = Old->GetActorLocation(); F.Tick(6);
        const double Travel = FVector::Dist2D(Before, Old->GetActorLocation());
        TestTrue(TEXT("末波已结算后旧怪仍实际移动"), Travel > 1 && Travel <= 45);
        TestTrue(TEXT("旧怪仍向唯一合法玩家靠近"), FVector::Dist2D(Old->GetActorLocation(), F.PC->GetPawn()->GetActorLocation()) < 400);
        Old->SetActorLocation(F.PC->GetPawn()->GetActorLocation() + FVector(50, 0, 0));
        const float BeforeHP = Y01Playground::Health(F.Player->GetCombatASC()); F.Tick(8);
        TestTrue(TEXT("进入攻击范围后锁定同一玩家"), Old->GetCombatASC()->GetCombatTarget() == F.PC->GetPawn());
        TestTrue(TEXT("末波已结算后旧怪仍造成真实攻击伤害"), Y01Playground::Health(F.Player->GetCombatASC()) < BeforeHP);
        return true;
    }
    F.Kill(1); F.Tick(); TestEqual(TEXT("中间波独立追加20"), F.Player->GetGold(), 50);
    F.Kill(0); F.Tick(); TestEqual(TEXT("旧波有延迟不结算"), F.S().CompletedWaves, 2);
    TestFalse(TEXT("独立奖励不能重复提交"), F.Mode->TryCompleteWave());
    while (F.World->GetTimeSeconds() < Start + 2.15) { F.Tick(); }
    if (!TestEqual(TEXT("跨两次换波的延迟批次没有取消"), F.Mode->GetWaveMonsters().Num(), 4)) { return false; }
    TestEqual(TEXT("旧波尚有一个存活"), F.S().WaveStates[0].AliveMonsters, 1);
    F.Kill(3); F.Tick(); TestEqual(TEXT("全部逐波清理各结算一次"), F.S().CompletedWaves, 3);
    TestEqual(TEXT("三波奖励总和60"), F.Player->GetGold(), 60);
    if (Case == TEXT("DeathWins")) { F.Player->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    F.Tick(); TestEqual(TEXT("结束结果死亡优先"), F.S().Result, Case == TEXT("DeathWins") ? EY01MatchResult::Failure : EY01MatchResult::Success);
    F.Tick(50); TestEqual(TEXT("结束无额外生成"), F.Mode->GetWaveMonsters().Num(), 4);
    return true;
}
#endif
