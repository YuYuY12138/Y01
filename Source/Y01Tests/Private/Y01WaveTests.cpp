#include "Misc/AutomationTest.h"
#include "Data/Y01DataSettings.h"
#include "Player/Y01PlayerState.h"
#include "UObject/UnrealType.h"
#include "Misc/ConfigCacheIni.h"
#include "Match/Y01GameFlow.h"
#include "Y01MatchFixture.h"
#include "Y01Smoke.h"
#include "Y01FrameworkTestTypes.h"
#include "Misc/CommandLine.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01WaveContractTest, "Y01.Waves.Contract",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01WaveContractTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("GameMode"), TEXT("GameState"), TEXT("OrderedWaves"), TEXT("CharacterRef"), TEXT("RewardDryRun"), TEXT("DefaultMode") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01WaveContractTest::RunTest(const FString& Case)
{
    if (Case == TEXT("GameMode")) { TestNotNull(TEXT("正式权威GameMode存在"), FindObject<UClass>(nullptr, TEXT("/Script/Y01.Y01GameMode"))); }
    if (Case == TEXT("GameState")) { TestNotNull(TEXT("正式复制GameState存在"), FindObject<UClass>(nullptr, TEXT("/Script/Y01.Y01GameState"))); }
    if (Case == TEXT("OrderedWaves")) { TestNotNull(TEXT("Settings显式有序波次"), FindFProperty<FArrayProperty>(UY01DataSettings::StaticClass(), TEXT("OrderedWaves"))); }
    if (Case == TEXT("CharacterRef")) { TestNotNull(TEXT("Settings显式角色身份"), FindFProperty<FStructProperty>(UY01DataSettings::StaticClass(), TEXT("MatchCharacter"))); }
    if (Case == TEXT("RewardDryRun")) { TestNotNull(TEXT("奖励预校验可用"), AY01PlayerState::StaticClass()->FindFunctionByName(TEXT("CanAddMatchReward"))); }
    if (Case == TEXT("DefaultMode"))
    {
        FString Mode;
        GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GlobalDefaultGameMode"), Mode, GEngineIni);
        TestEqual(TEXT("正式入口不能静默退回引擎GameMode"), Mode, FString(TEXT("/Script/Y01.Y01GameMode")));
    }
    return true;
}
namespace
{
struct FWaveWorld : FY01MatchFixture
{
    UWorld* World;
    AY01GameMode* Mode;
    AY01PlayerState* A;
    AY01PlayerState* B;
    explicit FWaveWorld(TSubclassOf<AY01GameMode> ModeClass = AY01GameMode::StaticClass())
    {
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
        Mode = World->SpawnActor<AY01GameMode>(ModeClass);
        A = World->SpawnActor<AY01PlayerState>(); B = World->SpawnActor<AY01PlayerState>();
        Mode->GetFlowState()->AddPlayerState(A); Mode->GetFlowState()->AddPlayerState(B);
    }
    ~FWaveWorld() { World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    void Tick(float Seconds) { for (int32 I = 0; I < FMath::CeilToInt(Seconds / 0.05f); ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, 0.05f); } }
    bool Configure(FString& Error) { return Mode->TryConfigureMatch(FY01DataCatalog(Tables), Waves(), Ref(EY01TableKind::Character), Error); }
    const FY01WaveSnapshot& S() const { return Mode->GetFlowState()->GetWaveSnapshot(); }
};
void Role(AActor* Actor, ENetRole Value)
{ FindFieldChecked<FByteProperty>(AActor::StaticClass(), TEXT("Role"))->SetPropertyValue_InContainer(Actor, Value); }
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01WaveFlowTest, "Y01.Waves.Flow",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01WaveFlowTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("Configure"), TEXT("MissingConfiguration"), TEXT("InvalidOrder"), TEXT("InvalidReward"),
        TEXT("PreparationTimer"), TEXT("DelayedSpawns"), TEXT("ExplicitPolicy"), TEXT("IdempotentReward"), TEXT("GoldAtomic"),
        TEXT("ExperienceAtomic"), TEXT("FailureNoReward"), TEXT("FailureCancelsTimers"), TEXT("Authority"), TEXT("LegalTransitions"),
        TEXT("OrderedBoss"), TEXT("DryRun"), TEXT("Replication"), TEXT("ReconfigureRejected"), TEXT("RewardReentry"),
        TEXT("MixedSpawnDelays"), TEXT("EmptyOrder"), TEXT("ClientConfigure") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01WaveFlowTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> FrameGuard(GFrameCounter, GFrameCounter);
    FWaveWorld F(Case == TEXT("RewardReentry") ? AY01WaveReentryTestMode::StaticClass() : AY01GameMode::StaticClass()); FString Error;
    TestTrue(TEXT("真实19表验证"), FY01DataCatalog(F.Tables).Validate().IsValid());
    if (Case == TEXT("MissingConfiguration"))
    {
        AddExpectedError(TEXT("MissingConfiguration"), EAutomationExpectedErrorFlags::Contains, 1);
        F.Mode->DispatchBeginPlay();
        TestTrue(TEXT("缺配置错误可诊断"), F.Mode->GetConfigurationError().Contains(TEXT("MissingConfiguration")));
        TestFalse(TEXT("缺配置拒绝开始"), F.Mode->TryStartNextWave());
        TestEqual(TEXT("留Waiting"), F.S().Phase, EY01MatchPhase::Waiting); return true;
    }
    if (Case == TEXT("DryRun"))
    {
        TestTrue(TEXT("合法奖励可预校验"), F.A->CanAddMatchReward(10, 5));
        TestEqual(TEXT("预校验无副作用"), F.A->GetGold(), 0);
        TestFalse(TEXT("负值预校验拒绝"), F.A->CanAddMatchReward(-1, 5));
        F.A->TryAddMatchReward(MAX_int32, MAX_int32);
        TestFalse(TEXT("金币溢出拒绝"), F.A->CanAddMatchReward(1, 0));
        TestFalse(TEXT("经验溢出拒绝"), F.A->CanAddMatchReward(0, 1));
        Role(F.A, ROLE_SimulatedProxy); TestFalse(TEXT("非权威预校验拒绝"), F.A->CanAddMatchReward(0, 0)); Role(F.A, ROLE_Authority); return true;
    }
    if (Case == TEXT("Replication"))
    {
        AY01GameState::StaticClass()->SetUpRuntimeReplicationData();
        auto* Prop = FindFProperty<FStructProperty>(AY01GameState::StaticClass(), TEXT("Snapshot"));
        TArray<FLifetimeProperty> Props; F.Mode->GetFlowState()->GetLifetimeReplicatedProps(Props);
        TestTrue(TEXT("快照复制和通知"), Prop && Prop->HasAllPropertyFlags(CPF_Net | CPF_RepNotify));
        TestTrue(TEXT("实际注册复制"), Prop && Props.ContainsByPredicate([&](const auto& P) { return P.RepIndex == Prop->RepIndex; })); return true;
    }
    if (Case == TEXT("EmptyOrder"))
    {
        TestFalse(TEXT("禁止猜测第一行或TMap顺序"), F.Mode->TryConfigureMatch(FY01DataCatalog(F.Tables), {}, FY01MatchFixture::Ref(EY01TableKind::Character), Error));
        TestFalse(TEXT("未部分初始化"), F.A->IsCharacterInitialized()); return true;
    }
    if (Case == TEXT("ClientConfigure"))
    {
        Role(F.Mode, ROLE_SimulatedProxy); TestFalse(TEXT("客户端不得注入配置"), F.Configure(Error));
        Role(F.Mode, ROLE_Authority); TestFalse(TEXT("客户端未提交玩家属性"), F.A->IsCharacterInitialized());
    }
    if (Case == TEXT("MixedSpawnDelays"))
    {
        auto& Wave = F.Row<FY01WaveRow>(EY01TableKind::Wave);
        Wave.Spawns[0].Count = 1; Wave.Spawns[0].DelaySeconds = 0;
        FY01SpawnEntry Late = Wave.Spawns[0]; Late.DelaySeconds = 1; Wave.Spawns.Add(Late);
        TestTrue(TEXT("混合延时配置成功"), F.Configure(Error)); TestTrue(TEXT("混合波开始"), F.Mode->TryStartNextWave());
        F.Tick(1.15f); TestEqual(TEXT("零延时立即刷一个"), F.Mode->GetWaveMonsters().Num(), 1);
        TestEqual(TEXT("另一个仍待刷"), F.S().PendingSpawns, 1); TestFalse(TEXT("不能因已有怪物死亡提前完成"), F.Mode->TryCompleteWave());
        F.Tick(1.0f); TestEqual(TEXT("延时另一个刷出"), F.Mode->GetWaveMonsters().Num(), 2);
        TestTrue(TEXT("延时完成后明确完成可行"), F.Mode->TryCompleteWave()); return true;
    }
    if (!TestTrue(TEXT("合法配置必须能提交"), F.Configure(Error))) { AddInfo(Error); return false; }
    if (Case == TEXT("Configure"))
    {
        TestTrue(TEXT("配置就绪"), F.Mode->IsMatchConfigured()); TestTrue(TEXT("初始化全部玩家"), F.A->IsCharacterInitialized() && F.B->IsCharacterInitialized());
        TestEqual(TEXT("配置不会偷跑"), F.S().Phase, EY01MatchPhase::Waiting); return true;
    }
    if (Case == TEXT("ReconfigureRejected")) { TestFalse(TEXT("已配置不能覆盖目录"), F.Configure(Error)); return true; }
    if (Case == TEXT("InvalidOrder") || Case == TEXT("InvalidReward"))
    {
        FWaveWorld Bad;
        auto Refs = Bad.Waves(); if (Case == TEXT("InvalidOrder")) { Refs[1].RowName = TEXT("Missing"); }
        else { FY01RewardEntry Item; Item.Item = FY01MatchFixture::Ref(EY01TableKind::Equipment); Bad.Row<FY01RewardPackageRow>(EY01TableKind::RewardPackage).Items.Add(Item); }
        TestFalse(TEXT("错误或未支持的奖励不能部分配置"), Bad.Mode->TryConfigureMatch(FY01DataCatalog(Bad.Tables), Refs, FY01MatchFixture::Ref(EY01TableKind::Character), Error));
        TestFalse(TEXT("失败不初始化玩家"), Bad.A->IsCharacterInitialized()); return true;
    }
    if (Case == TEXT("Authority"))
    {
        Role(F.Mode, ROLE_SimulatedProxy); TestFalse(TEXT("客户端不能开始"), F.Mode->TryStartNextWave());
        TestFalse(TEXT("客户端不能结束"), F.Mode->TryEndMatch(false)); Role(F.Mode, ROLE_Authority);
    }
    TestFalse(TEXT("Waiting不能完成波次"), F.Mode->TryCompleteWave());
    TestFalse(TEXT("Waiting不能胜利"), F.Mode->TryEndMatch(true));
    TestTrue(TEXT("显式开始"), F.Mode->TryStartNextWave());
    TestEqual(TEXT("Preparing"), F.S().Phase, EY01MatchPhase::Preparing);
    TestFalse(TEXT("Preparing不可重复开始"), F.Mode->TryStartNextWave());
    TestFalse(TEXT("Preparing不可领奖"), F.Mode->TryCompleteWave());
    TestTrue(TEXT("使用服务器deadline"), F.S().PreparationDeadline >= F.S().ServerPhaseTime + 0.99);
    if (Case == TEXT("FailureCancelsTimers"))
    {
        TestTrue(TEXT("准备中可以明确失败"), F.Mode->TryEndMatch(false)); F.Tick(3);
        TestEqual(TEXT("失败后无刷怪"), F.Mode->GetWaveMonsters().Num(), 0); return true;
    }
    F.Tick(0.5f); TestEqual(TEXT("未超时保持准备"), F.S().Phase, EY01MatchPhase::Preparing);
    F.Tick(0.65f); TestEqual(TEXT("真实timer进入Battle"), F.S().Phase, EY01MatchPhase::Battle);
    TestEqual(TEXT("延时尚未刷怪"), F.Mode->GetWaveMonsters().Num(), 0);
    TestEqual(TEXT("待刷数量来自Count"), F.S().PendingSpawns, 2);
    TestFalse(TEXT("仍有待刷不得提前完成"), F.Mode->TryCompleteWave());
    F.Tick(0.6f); TestEqual(TEXT("延时后按Count刷怪"), F.Mode->GetWaveMonsters().Num(), 2);
    TestEqual(TEXT("待刷清零"), F.S().PendingSpawns, 0);
    if (Case == TEXT("PreparationTimer") || Case == TEXT("DelayedSpawns")) { return true; }
    if (Case == TEXT("Authority"))
    { Role(F.Mode, ROLE_SimulatedProxy); TestFalse(TEXT("客户端不得发奖"), F.Mode->TryCompleteWave()); Role(F.Mode, ROLE_Authority); return true; }
    if (Case == TEXT("FailureNoReward"))
    {
        TestTrue(TEXT("战斗失败"), F.Mode->TryEndMatch(false)); TestFalse(TEXT("失败不能补发成功奖"), F.Mode->TryCompleteWave());
        TestEqual(TEXT("失败金币零"), F.A->GetGold() + F.B->GetGold(), 0); TestEqual(TEXT("失败经验零"), F.A->GetExperience() + F.B->GetExperience(), 0); return true;
    }
    if (Case == TEXT("GoldAtomic") || Case == TEXT("ExperienceAtomic"))
    {
        F.B->TryAddMatchReward(Case == TEXT("GoldAtomic") ? MAX_int32 : 0, Case == TEXT("ExperienceAtomic") ? MAX_int32 : 0);
        TestFalse(TEXT("任何玩家溢出整批拒绝"), F.Mode->TryCompleteWave());
        TestEqual(TEXT("第一个玩家没有部分金币"), F.A->GetGold(), 0); TestEqual(TEXT("没有部分经验"), F.A->GetExperience(), 0);
        TestEqual(TEXT("未消费幂等key"), F.S().CompletedWaves, 0);
        F.Mode->GetFlowState()->RemovePlayerState(F.B);
        TestTrue(TEXT("移除溢出接收者后可重试同一key"), F.Mode->TryCompleteWave());
        TestEqual(TEXT("重试发奖一次"), F.A->GetGold(), 10); return true;
    }
    TestTrue(TEXT("正式生产不硬编码清怪政策，显式完成"), F.Mode->TryCompleteWave());
    TestFalse(TEXT("重复完成不重复发奖"), F.Mode->TryCompleteWave());
    TestEqual(TEXT("玩家A表金币"), F.A->GetGold(), 10); TestEqual(TEXT("玩家B表经验"), F.B->GetExperience(), 5);
    TestEqual(TEXT("完成数一次"), F.S().CompletedWaves, 1);
    if (Case == TEXT("RewardReentry"))
    {
        const auto* Reentry = CastChecked<AY01WaveReentryTestMode>(F.Mode);
        TestEqual(TEXT("真实发奖后回调一次"), Reentry->RewardCallbacks, 1);
        TestFalse(TEXT("回调重入不能再发奖、开波或结束"), Reentry->bNestedComplete || Reentry->bNestedStart || Reentry->bNestedEnd);
    }
    if (Case == TEXT("OrderedBoss"))
    {
        TestTrue(TEXT("显式下一波"), F.Mode->TryStartNextWave()); TestEqual(TEXT("顺序来自数组而非TMap"), F.S().Wave.RowName, FName(TEXT("Boss")));
        TestEqual(TEXT("boss准备更长"), F.S().PreparationDeadline - F.S().ServerPhaseTime, 2.0);
        F.Tick(2.7f); TestTrue(TEXT("第二波独立key"), F.Mode->TryCompleteWave()); TestEqual(TEXT("两波20金币"), F.A->GetGold(), 20);
        TestFalse(TEXT("越过已配置波次拒绝"), F.Mode->TryStartNextWave());
    }
    TestTrue(TEXT("明确结束，不自动接竞拍"), F.Mode->TryEndMatch(true));
    TestFalse(TEXT("Ended不可再次结束"), F.Mode->TryEndMatch(false)); TestFalse(TEXT("Ended不可开始"), F.Mode->TryStartNextWave());
    TestEqual(TEXT("复制胜利结果"), F.S().Result, EY01MatchResult::Success);
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01SmokeIsolationTest, "Y01.Waves.Smoke",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01SmokeIsolationTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("OptIn"), TEXT("DisabledURL"), TEXT("EnabledFixture"), TEXT("LocalObservation"), TEXT("ProbeReplication"), TEXT("FloorReplication") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01SmokeIsolationTest::RunTest(const FString& Case)
{
    if (Case == TEXT("OptIn"))
    {
        TestTrue(TEXT("显式参数启用"), AY01SmokeGameMode::IsOptedIn(TEXT("-Y01Smoke -Y01SmokeScenario=Success")));
        for (const TCHAR* Args : { TEXT(""), TEXT("?game=/Script/Y01Tests.Y01SmokeGameMode"), TEXT("-Y01SmokeScenario=Success"), TEXT("-NotY01Smoke"), TEXT("-Y01SmokeExtra") })
        { TestFalse(TEXT("类名或近似参数不能启用"), AY01SmokeGameMode::IsOptedIn(Args)); }
        return true;
    }
    FWaveWorld F;
    if (Case == TEXT("LocalObservation") || Case == TEXT("ProbeReplication"))
    {
        auto* Probe = F.World->SpawnActor<AY01SmokeProbe>();
        if (Case == TEXT("LocalObservation"))
        {
            const FString Before = AY01SmokeProbe::Observe(F.World, Probe);
            F.A->TryAddMatchReward(10, 5);
            const FString After = AY01SmokeProbe::Observe(F.World, Probe);
            TestFalse(TEXT("观测必须来自本地真实字段，字段变化改变观测"), Before == After);
            TestTrue(TEXT("包含金币和经验"), After.Contains(TEXT("Gold=10 XP=5"))); return true;
        }
        AY01SmokeProbe::StaticClass()->SetUpRuntimeReplicationData(); TArray<FLifetimeProperty> Props; Probe->GetLifetimeReplicatedProps(Props);
        for (const TCHAR* Name : { TEXT("Sequence"), TEXT("Core"), TEXT("Monsters") })
        {
            auto* Prop = FindFProperty<FProperty>(AY01SmokeProbe::StaticClass(), Name);
            TestTrue(TEXT("探针复制实物引用而非伪造生命"), Prop && Props.ContainsByPredicate([&](const auto& P) { return P.RepIndex == Prop->RepIndex; }));
        }
        return true;
    }
    const FString Original = FCommandLine::Get();
    const bool bUseFixture = Case != TEXT("DisabledURL");
    FCommandLine::Set(bUseFixture ? TEXT("-Y01Smoke -Y01SmokeScenario=Success") : TEXT("?game=/Script/Y01Tests.Y01SmokeGameMode"));
    auto* Mode = F.World->SpawnActor<AY01SmokeGameMode>();
    if (Case == TEXT("DisabledURL")) { AddExpectedError(TEXT("MissingConfiguration"), EAutomationExpectedErrorFlags::Contains, 1); }
    Mode->DispatchBeginPlay();
    FCommandLine::Set(*Original);
    TestEqual(TEXT("内存数据必须由明确参数控制"), Mode->HasSmokeFixture(), bUseFixture);
    if (Case == TEXT("FloorReplication"))
    {
        int32 Floors = 0;
        for (TActorIterator<AStaticMeshActor> It(F.World); It; ++It)
        {
            ++Floors;
            TestTrue(TEXT("地面Actor必须复制供客户端解析基座"), It->GetIsReplicated());
            TestTrue(TEXT("地面变换必须复制"), It->IsReplicatingMovement());
            TestTrue(TEXT("网格组件必须复制引擎cube与碰撞变换"), It->GetStaticMeshComponent()->GetIsReplicated());
        }
        TestEqual(TEXT("单一测试碰撞地面"), Floors, 1);
    }
    TestEqual(TEXT("配置不能擅自开局"), Mode->GetFlowState()->GetWaveSnapshot().Phase, EY01MatchPhase::Waiting);
    TestFalse(TEXT("没有两个真实连接及ACK时拒绝开局"), Mode->TryStartNextWave());
    return true;
}
#endif
