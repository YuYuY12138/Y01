#include "Misc/AutomationTest.h"
#include "Playground/Y01R36Verification.h"
#include "Y01MatchFixture.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01EconomyComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Combat/Y01Core.h"
#include "Match/Y01SpawnPoint.h"
#include "Misc/CommandLine.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Playground/Y01R36UI.h"
#include "UObject/UnrealType.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Blueprint/WidgetTree.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36FormalEntryTest, "Y01.Framework.R36.Runtime.FormalEntry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R36FormalEntryTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("NoConfig"), TEXT("Content"), TEXT("LoadedContent"), TEXT("DuplicateConfig"),
        TEXT("ExplicitProfile"), TEXT("Evidence"), TEXT("Compare"), TEXT("ExplicitDuplicate"), TEXT("Unknown"), TEXT("SoftReferences"), TEXT("MissingProfiles")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36FormalEntryTest::RunTest(const FString& Case)
{
    const FString OldCommand = FCommandLine::Get(); FCommandLine::Set(TEXT(""));
    ON_SCOPE_EXIT { FCommandLine::Set(*OldCommand); };
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); };
    auto* Mode = World->SpawnActor<AY01R36RuntimeProbe>();
    UY01R36Profile* DiskProfile = nullptr;
    if (Case != TEXT("NoConfig"))
    {
        auto* Config = World->SpawnActor<AY01R36Config>();
        if (Case == TEXT("MissingProfiles"))
        {
            const TSoftObjectPtr<UY01R36Profile> Missing(FSoftObjectPath(TEXT("/Game/NoSuchEarly13Profile.NoSuchEarly13Profile")));
            Config->Content = Missing; Config->Baseline = Missing; Config->Compare = Missing;
        }
        else
        {
            DiskProfile = LoadObject<UY01R36Profile>(nullptr, TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/S4_AddedRows_20261001_01/Content/DA_R36Profile"));
            if (!TestNotNull(TEXT("保留原Content磁盘Profile"), DiskProfile)) { return false; }
            Config->Content = DiskProfile; Config->Baseline = DiskProfile; Config->Compare = DiskProfile;
            if (Case.Contains(TEXT("Duplicate"))) { auto* Other = World->SpawnActor<AY01R36Config>(); Other->Content = DiskProfile; }
        }
    }
    if (Case == TEXT("Content")) { FCommandLine::Set(TEXT("-R36Profile=content")); }
    if (Case == TEXT("ExplicitProfile") || Case == TEXT("ExplicitDuplicate")) { FCommandLine::Set(TEXT("-R36Profile=profile")); }
    if (Case == TEXT("Evidence")) { FCommandLine::Set(TEXT("-R36Profile=evidence")); }
    if (Case == TEXT("Compare")) { FCommandLine::Set(TEXT("-R36Profile=compare")); }
    if (Case == TEXT("Unknown")) { FCommandLine::Set(TEXT("-R36Profile=typo")); }
    if (Case == TEXT("SoftReferences"))
    {
        for (const TCHAR* N : {TEXT("Content"), TEXT("Baseline"), TEXT("Compare")})
        { TestNotNull(TEXT("三引用均为软引用"), FindFProperty<FSoftObjectProperty>(AY01R36Config::StaticClass(), N)); }
        return true;
    }
    FString Error; auto* Selected = Cast<UY01R36Profile>(Mode->SelectProfile(Error));
    if (Case == TEXT("ExplicitDuplicate") || Case == TEXT("Unknown"))
    { TestNull(TEXT("显式歧义或拼写错误拒绝"), Selected); TestFalse(TEXT("拒绝原因可诊断"), Error.IsEmpty()); return true; }
    if (Case == TEXT("ExplicitProfile") || Case == TEXT("Evidence") || Case == TEXT("Compare"))
    { TestTrue(TEXT("显式入口保留原Profile身份和名单"), Selected == DiskProfile); return true; }
    if (!TestNotNull(TEXT("默认不依赖Config/Profile"), Selected)) { AddError(Error); return false; }
    TestTrue(TEXT("已加载磁盘Content不能接管默认入口"), Selected != DiskProfile);
    TestEqual(TEXT("默认固定24表"), Selected->Tables.Num(), 24);
    for (const auto& Pair : Selected->Tables)
    { TestTrue(TEXT("所有表来自正式Data"), Pair.Value->GetPathName().StartsWith(TEXT("/Game/Y01Playground/Data/DT_"))); }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36FlowHooksTest, "Y01.Framework.R36.FlowHooks",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36FlowHooksTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Initialized"), TEXT("FailedInitialization"), TEXT("PhaseChangesOnly"), TEXT("LegacyDisabled") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36FlowHooksTest::RunTest(const FString& Case)
{
    FY01MatchFixture Data;
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    auto* Mode = World->SpawnActor<AY01R36FlowProbe>();
    auto* PS = World->SpawnActor<AY01PlayerState>(); PS->GetCombatASC()->InitAbilityActorInfo(PS, PS);
    Mode->GetFlowState()->AddPlayerState(PS);
    FString Error;
    if (Case == TEXT("FailedInitialization"))
    { Data.Row<FY01CharacterRow>(EY01TableKind::Character).Health = -1; }
    const bool Configured = Mode->TryConfigureMatch(FY01DataCatalog(Data.Tables), Data.Waves(),
        FY01MatchFixture::Ref(EY01TableKind::Character), Error);
    if (Case == TEXT("FailedInitialization"))
    { TestFalse(TEXT("坏配置拒绝"), Configured); TestEqual(TEXT("失败不调用成功hook"), Mode->InitializedCalls, 0); }
    else
    {
        TestTrue(TEXT("真实角色配置成功"), Configured);
        if (Case == TEXT("Initialized"))
        {
            TestEqual(TEXT("成功初始化实际调用一次hook"), Mode->InitializedCalls, 1);
            TestTrue(TEXT("hook时角色已完成初始化"), Mode->bSawInitializedCharacter);
        }
        if (Case == TEXT("LegacyDisabled"))
        { TestFalse(TEXT("默认空hook不启用旧Mode的Loadout"), PS->GetLoadout()->IsInitialized()); }
        if (Case == TEXT("PhaseChangesOnly"))
        {
            TestTrue(TEXT("真实Preparing"), Mode->TryStartNextWave());
            TestEqual(TEXT("Preparing只通知一次"), Mode->Phases.Num(), 1);
            TGuardValue<uint64> Frame(GFrameCounter, GFrameCounter);
            for (int32 I = 0; I < 35; ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .05f); }
            TestEqual(TEXT("刷怪Publish不重复通知Battle"), Mode->Phases.Num(), 2);
            TestTrue(TEXT("真实奖励Publish"), Mode->TryCompleteWave());
            TestEqual(TEXT("奖励Publish也不重复阶段"), Mode->Phases.Num(), 2);
            TestTrue(TEXT("真实Ended"), Mode->TryEndMatch(true));
            TestEqual(TEXT("Ended通知一次"), Mode->Phases.Num(), 3);
            TestFalse(TEXT("重复Ended被拒绝"), Mode->TryEndMatch(true));
            TestEqual(TEXT("重复Ended不通知"), Mode->Phases.Num(), 3);
            if (Mode->Phases.Num() == 3)
            {
                TestEqual(TEXT("首次从Waiting进入"), Mode->PreviousPhases[0], EY01MatchPhase::Waiting);
                TestEqual(TEXT("首阶段Preparing"), Mode->Phases[0], EY01MatchPhase::Preparing);
                TestEqual(TEXT("再从Preparing进入Battle"), Mode->PreviousPhases[1], EY01MatchPhase::Preparing);
                TestEqual(TEXT("最后Ended"), Mode->Phases[2], EY01MatchPhase::Ended);
            }
        }
    }
    World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36FormalSessionTest, "Y01.Framework.R36.Runtime.FormalSession",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R36FormalSessionTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Default"), TEXT("Content")}) { Names.Add(N); Commands.Add(N); }
}
bool FY01R36FormalSessionTest::RunTest(const FString& Case)
{
    const FString OldCommand = FCommandLine::Get();
    FCommandLine::Set(Case == TEXT("Content") ? TEXT("-R36Profile=content") : TEXT(""));
    ON_SCOPE_EXIT { FCommandLine::Set(*OldCommand); };
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); };
    World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
    World->GetWorldSettings()->DefaultGameMode = AY01R36RuntimeProbe::StaticClass(); World->SetGameMode(FURL());
    auto* Mode = World->GetAuthGameMode<AY01R36RuntimeProbe>();
    auto* PC = World->SpawnActor<AY01R36Controller>(); auto* PS = World->SpawnActor<AY01PlayerState>();
    auto* Pawn = World->SpawnActor<AY01Character>(FVector(10000, 0, 100), FRotator::ZeroRotator);
    PS->SetOwner(PC); PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn);
    Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(PS);
    // 临时运行世界提供与正式地图相同的唯一核心；不生成地图、不添加Config、不修改正式表行。
    auto* Core = World->SpawnActor<AY01Core>();
    // 刷怪点同样是地图对象：按正式DT_Wave实际批次的点名与类型提供，不硬编码、不放宽生产门禁。
    const auto* WaveTable = LoadObject<UDataTable>(nullptr, TEXT("/Game/Y01Playground/Data/DT_Wave"));
    if (!TestNotNull(TEXT("正式DT_Wave可读"), WaveTable)) { return false; }
    TMap<FName, EY01SpawnPointKind> PointKinds;
    for (const auto& Pair : WaveTable->GetRowMap())
    { for (const auto& Batch : reinterpret_cast<const FY01WaveRow*>(Pair.Value)->Batches) { PointKinds.Add(Batch.PointTag, Batch.PointKind); } }
    int32 PointIndex = 0;
    for (const auto& Pair : PointKinds)
    {
        auto* Point = World->SpawnActor<AY01SpawnPoint>(FVector(-3000 - 600 * PointIndex++, 0, 50), FRotator::ZeroRotator);
        Point->Tags = {Pair.Key}; Point->PointKind = Pair.Value;
    }
    FString Error;
    if (!TestTrue(TEXT("无Config正式表完成完整加载链"), Mode->LoadMatchConfiguration(Error))) { AddError(Error); return false; }
    World->BeginPlay();
    const auto* Profile = Cast<UY01R36Profile>(Mode->GetProfile());
    if (!TestNotNull(TEXT("正式入口持有瞬态R36配置"), Profile)) { return false; }
    TestEqual(TEXT("正式24表"), Profile->Tables.Num(), 24);
    TestTrue(TEXT("初始化无来源/经济错误"), Mode->GetLoadoutError().IsEmpty());
    FY01DataValidation Query;
    const auto* Rules = Profile->BuildCatalog().FindRow<FY01MatchRulesRow>({EY01TableKind::MatchRules, TEXT("Default")}, Query);
    if (!TestNotNull(TEXT("同一MatchRules.Default提供Startup和Core"), Rules)) { return false; }
    TestTrue(TEXT("核心开关沿Startup"), Mode->IsCoreDefenseEnabled() == Rules->Startup.bCoreDefenseEnabled);
    if (Rules->Startup.bCoreDefenseEnabled) { TestTrue(TEXT("完整核心链实际绑定地图唯一核心"), Mode->GetMatchCore() == Core); }
    TestTrue(TEXT("经济开关沿Startup"), PS->GetEconomy()->IsInitialized() == Rules->Startup.bEconomyEnabled);
    FY01CommittedView View;
    if (!TestTrue(TEXT("真实库存会话已发布"), PS->GetLoadout()->ReadCommittedView(View))) { return false; }
    TestEqual(TEXT("目录发现不自动发放装备或技能书"), View.Items.Num(), 0);
    TestEqual(TEXT("初始金币沿Startup"), PS->GetGold(), Rules->Startup.bEconomyEnabled ? Rules->Startup.InitialGold : 0);
    TestTrue(TEXT("正式日常入口自动波策略"), Mode->IsAutomaticWaves());
    TGuardValue<uint64> Frame(GFrameCounter, GFrameCounter); ++GFrameCounter; World->Tick(LEVELTICK_All, .05f);
    TestEqual(TEXT("完整权威Tick自动开始首波准备"), Mode->GetFlowState()->GetWaveSnapshot().Phase, EY01MatchPhase::Preparing);
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36BootstrapTest, "Y01.Framework.R36.Bootstrap",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36BootstrapTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("DefaultContent"), TEXT("ProfileSelection"), TEXT("MissingConfig"), TEXT("DuplicateConfig"),
        TEXT("InvalidProfile"), TEXT("InitializedOnly"), TEXT("FailedCharacter"), TEXT("LateJoin"), TEXT("JoinIdentity"),
        TEXT("NoEconomy"), TEXT("LegacyBypass"), TEXT("ControllerClass"), TEXT("InitializationFault"), TEXT("S4WaveTrace"), TEXT("S4TableTrace") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36BootstrapTest::RunTest(const FString& Case)
{
    const FString OldCommand = FCommandLine::Get();
    FCommandLine::Set(TEXT("-R36Profile=profile"));
    ON_SCOPE_EXIT { FCommandLine::Set(*OldCommand); };
    using K = EY01TableKind;
    FY01MatchFixture Data;
    auto& Character = Data.Row<FY01CharacterRow>(K::Character);
    Character.InitialEquipment.Reset(); Character.InitialSkills.Reset();
    Data.Row<FY01SkillRow>(K::Skill).DisplayName = FText::FromString(TEXT("程序技能"));
    auto* Profile = NewObject<UY01R36Profile>();
    Profile->Character = FY01MatchFixture::Ref(K::Character); Profile->OrderedWaves = Data.Waves();
    Profile->ContentSkills.Add(FY01MatchFixture::Ref(K::Skill));
    for (const auto& Pair : Data.Tables) { Profile->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
    World->GetWorldSettings()->DefaultGameMode = AY01R36RuntimeProbe::StaticClass(); World->SetGameMode(FURL());
    auto* Mode = World->GetAuthGameMode<AY01R36RuntimeProbe>();
    auto AddPlayer = [&]()
    {
        auto* PC = World->SpawnActor<AY01R36Controller>(); auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(0, 0, 100), FRotator::ZeroRotator);
        PS->SetOwner(PC); PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(PS); return PC;
    };
    auto* PC = AddPlayer(); auto* PS = PC->GetPlayerState<AY01PlayerState>();
    TestFalse(TEXT("配置前Loadout默认关闭"), PS->GetLoadout()->IsInitialized());
    auto* Config = World->SpawnActor<AY01R36Config>(); Config->Content = Profile;
    Config->Baseline = NewObject<UY01R36Profile>(); Config->Compare = NewObject<UY01R36Profile>();
    if (Case == TEXT("ProfileSelection"))
    {
        TestTrue(TEXT("空选择走Content"), Config->Resolve(TEXT("")) == Profile);
        TestTrue(TEXT("content选择Config"), Config->Resolve(TEXT("content")) == Profile);
        TestTrue(TEXT("profile选择Content"), Config->Resolve(TEXT("profile")) == Profile);
        TestTrue(TEXT("evidence选择Baseline"), Config->Resolve(TEXT("evidence")) == Config->Baseline.LoadSynchronous());
        TestTrue(TEXT("compare选择"), Config->Resolve(TEXT("compare")) == Config->Compare.LoadSynchronous());
        TestNull(TEXT("未知配置不静默回退"), Config->Resolve(TEXT("invalid")));
    }
    if (Case == TEXT("MissingConfig")) { Config->Destroy(); }
    if (Case == TEXT("DuplicateConfig")) { World->SpawnActor<AY01R36Config>()->Content = Profile; }
    if (Case == TEXT("InvalidProfile")) { Data.Row<FY01SkillRow>(K::Skill).DisplayName = FText(); }
    if (Case == TEXT("FailedCharacter")) { Character.Health = -1; }
    if (Case == TEXT("InitializationFault"))
    {
        FString InitError; TestTrue(TEXT("夹具角色先初始化"), PS->TryInitializeCharacter(Profile->BuildCatalog(), Profile->Character, InitError));
        TestTrue(TEXT("夹具旧来源先绑定其他会话"), PS->GetSourceEffects()->Initialize(FGuid::NewGuid(), 99).IsOk());
    }
    FString Error; const bool Loaded = Mode->LoadMatchConfiguration(Error);
    const bool Bad = Case == TEXT("MissingConfig") || Case == TEXT("DuplicateConfig") || Case == TEXT("InvalidProfile") || Case == TEXT("FailedCharacter");
    if (Bad)
    {
        TestFalse(TEXT("非法入口拒绝"), Loaded); TestFalse(TEXT("初始化失败不启用Loadout"), PS->GetLoadout()->IsInitialized());
    }
    else
    {
        TestTrue(TEXT("独立Config通过原加载链"), Loaded);
        TestTrue(TEXT("持有选中的R36Profile"), Mode->GetProfile() == Profile);
        if (Case == TEXT("InitializationFault"))
        {
            TestFalse(TEXT("来源会话冲突不启用Loadout"), PS->GetLoadout()->IsInitialized());
            TestFalse(TEXT("初始化故障阻断开始"), Mode->TryStartNextWave());
            TestFalse(TEXT("故障可诊断"), Mode->GetLoadoutError().IsEmpty());
            Mode->Tick(.05f);
            TestEqual(TEXT("初始化故障也阻断权威自动准备"), Mode->GetFlowState()->GetWaveSnapshot().Phase, EY01MatchPhase::Waiting);
        }
        else
        {
            TestTrue(TEXT("成功角色初始化后才启用"), PS->GetLoadout()->IsInitialized());
            FY01CommittedView View; TestTrue(TEXT("空包完整视图已发布"), PS->GetLoadout()->ReadCommittedView(View));
            TestEqual(TEXT("没有伪购买预置"), View.Items.Num(), 0);
            if (Case == TEXT("LateJoin") || Case == TEXT("JoinIdentity"))
            {
                auto* Second = AddPlayer(); Second->Player = NewObject<ULocalPlayer>(GEngine); Mode->PostLogin(Second);
                FY01CommittedView Other; TestTrue(TEXT("真实PostLogin初始化晚加入者"), Second->GetPlayerState<AY01PlayerState>()->GetLoadout()->ReadCommittedView(Other));
                TestTrue(TEXT("同局身份"), View.MatchId.IsValid() && View.MatchId == Other.MatchId);
                TestTrue(TEXT("连接Join独立"), View.JoinSequence > 0 && Other.JoinSequence > View.JoinSequence);
            }
            if (Case == TEXT("LegacyBypass"))
            {
                FString Reject; TestFalse(TEXT("旧角色初始化不能重置"), PS->TryInitializeCharacter(Profile->BuildCatalog(), Profile->Character, Reject));
                TestFalse(TEXT("旧定义换装禁止"), PS->TrySetEquipmentSlot(Profile->BuildCatalog(), PC, 0, FY01MatchFixture::Ref(K::Equipment), Reject));
            }
            if (Case == TEXT("NoEconomy"))
            {
                Mode->Tick(.05f);
                TestEqual(TEXT("权威Tick实际自动进入Preparing"), Mode->GetFlowState()->GetWaveSnapshot().Phase, EY01MatchPhase::Preparing);
                TestEqual(TEXT("S1不启动收入"), PS->GetIncome()->Observe().State.Phase, EY01IncomePhase::Waiting);
                TestEqual(TEXT("S1不伪造收入"), PS->GetGold(), 0);
            }
        }
        if (Case == TEXT("ControllerClass")) { TestTrue(TEXT("独立Controller作为默认入口"), Mode->PlayerControllerClass == AY01R36Controller::StaticClass()); }
        if (Case == TEXT("S4WaveTrace") || Case == TEXT("S4TableTrace"))
        {
            Mode->Tick(.05f);
            TestEqual(TEXT("自动进入准备以观测真实波次"), Mode->GetFlowState()->GetWaveSnapshot().Phase, EY01MatchPhase::Preparing);
            const auto Before = Mode->GetFlowState()->GetWaveSnapshot();
            TSharedPtr<FJsonObject> Wave;
            for (const auto& Row : Y01R36Trace::Capture(PC, true))
            {
                TSharedPtr<FJsonObject> J;
                if (FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Row), J) && J->GetStringField(TEXT("kind")) == TEXT("WAVE")) { Wave = J; }
            }
            TestTrue(TEXT("通用只读波次记录存在"), Wave.IsValid());
            if (Wave.IsValid() && Case == TEXT("S4WaveTrace"))
            {
                TestEqual(TEXT("观测真实待生成数"), Wave->GetNumberField(TEXT("pending")), double(Before.PendingSpawns));
                TestEqual(TEXT("观测真实波次索引"), Wave->GetNumberField(TEXT("index")), double(Before.WaveIndex));
                TestEqual(TEXT("观测真实完成数"), Wave->GetNumberField(TEXT("completed")), double(Before.CompletedWaves));
                TestEqual(TEXT("不是推测清波"), Wave->GetBoolField(TEXT("cleared")), Before.bWaveCompleted);
                TestEqual(TEXT("时间来自世界"), Wave->GetNumberField(TEXT("time")), double(World->GetTimeSeconds()));
            }
            else if (Wave.IsValid())
            {
                TestEqual(TEXT("所选Profile真实路径"), Wave->GetStringField(TEXT("profile")), Profile->GetPathName());
                TestEqual(TEXT("全部实际表路径"), Wave->GetObjectField(TEXT("tables"))->Values.Num(), Profile->Tables.Num());
                TestEqual(TEXT("有序波次来自Profile数组"), Wave->GetArrayField(TEXT("orderedWaves")).Num(), Profile->OrderedWaves.Num());
            }
            const auto After = Mode->GetFlowState()->GetWaveSnapshot();
            TestTrue(TEXT("遥测没有修改波次"), FY01WaveSnapshot::StaticStruct()->CompareScriptStruct(&Before, &After, 0));
        }
    }
    World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36ControlledTest, "Y01.Framework.R36.Controlled",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36ControlledTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("DefaultEmpty"), TEXT("Controlled"), TEXT("Repeat"), TEXT("LateJoin"), TEXT("Client"),
        TEXT("InvalidFlag"), TEXT("InvalidCount"), TEXT("FailedCharacter"), TEXT("S3Empty"), TEXT("S1Empty"), TEXT("S4Empty") }) { Names.Add(N); Commands.Add(N); }
}
bool FY01R36ControlledTest::RunTest(const FString& Case)
{
    using K = EY01TableKind;
    const FString Previous = FCommandLine::Get(); ON_SCOPE_EXIT { FCommandLine::Set(*Previous); };
    FString Params = TEXT("-R36Fixture=controlled -R36Items=17");
    if (Case == TEXT("DefaultEmpty")) { Params = TEXT("-R36Items=17"); }
    if (Case == TEXT("InvalidFlag")) { Params = TEXT("-R36Fixture=natural"); }
    if (Case == TEXT("InvalidCount")) { Params = TEXT("-R36Fixture=controlled -R36Items=17garbage"); }
    if (Case == TEXT("S3Empty")) { Params = TEXT("-R36Fixture=controlled -R36Items=0 -R36Run=R36-S3-Visible-Empty"); }
    if (Case == TEXT("S1Empty")) { Params = TEXT("-R36Fixture=controlled -R36Items=0 -R36Run=R36-S1-UI-Empty"); }
    if (Case == TEXT("S4Empty")) { Params = TEXT("-R36Fixture=controlled -R36Items=0 -R36Run=R36-S4-Custom-Empty"); }
    FCommandLine::Set(*Params);
    FY01MatchFixture Data; auto& Character = Data.Row<FY01CharacterRow>(K::Character);
    Character.InitialEquipment.Reset(); Character.InitialSkills.Reset(); Character.AttackDamage = 30;
    if (Case == TEXT("FailedCharacter")) { Character.Health = -1; }
    FY01SourceEffectRow Effect; Effect.Magnitude = 10; Data.Add(K::SourceEffect, Effect);
    auto& Equip = Data.Row<FY01EquipmentRow>(K::Equipment); Equip.DisplayName = FText::FromString(TEXT("受控装备")); Equip.EffectIds = {Data.Ref(K::SourceEffect)};
    Data.Row<FY01SkillRow>(K::Skill).DisplayName = FText::FromString(TEXT("受控技能"));
    auto* Profile = NewObject<UY01R36Profile>(); Profile->Character = Data.Ref(K::Character); Profile->OrderedWaves = Data.Waves();
    Profile->ContentEquipment = {Data.Ref(K::Equipment)}; Profile->ContentSkills = {Data.Ref(K::Skill)};
    for (const auto& Pair : Data.Tables) { Profile->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); };
    World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
    World->GetWorldSettings()->DefaultGameMode = AY01R36RuntimeProbe::StaticClass(); World->SetGameMode(FURL());
    auto* Mode = World->GetAuthGameMode<AY01R36RuntimeProbe>();
    auto Player = [&]()
    {
        auto* PC = World->SpawnActor<AY01R36Controller>(); auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(0, 0, 100), FRotator::ZeroRotator);
        PS->SetOwner(PC); PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(PS); return PC;
    };
    auto* PC = Player(); auto* PS = PC->GetPlayerState<AY01PlayerState>();
    auto* Config = World->SpawnActor<AY01R36Config>(); Config->Content = Profile;
    if (Case == TEXT("Client")) { Mode->SetRole(ROLE_SimulatedProxy); }
    FString Error; const bool Loaded = Mode->LoadMatchConfiguration(Error);
    if (Case == TEXT("FailedCharacter"))
    { TestFalse(TEXT("坏角色不启动"), Loaded); TestFalse(TEXT("失败不初始化实例系统"), PS->GetLoadout()->IsInitialized()); return true; }
    Mode->SetRole(ROLE_Authority);
    FY01CommittedView View; const bool HasView = PS->GetLoadout()->ReadCommittedView(View);
    if (Case == TEXT("Client")) { TestFalse(TEXT("客户端不能从成功hook预置"), HasView); return true; }
    TestTrue(TEXT("真实配置链"), Loaded); TestTrue(TEXT("权威完整视图"), HasView);
    if (Case == TEXT("S3Empty") || Case == TEXT("S4Empty"))
    {
        TestEqual(TEXT("S3空包不注入物品"), View.Items.Num(), 0);
        TestTrue(TEXT("S3显式空包允许正常启动"), Mode->GetLoadoutError().IsEmpty() && Mode->TryStartNextWave());
        return true;
    }
    const bool Rejected = Case == TEXT("InvalidFlag") || Case == TEXT("InvalidCount") || Case == TEXT("S1Empty");
    if (Case == TEXT("DefaultEmpty") || Rejected)
    {
        TestEqual(TEXT("没有controlled不发物品"), View.Items.Num(), 0);
        if (Rejected) { TestFalse(TEXT("非法受控参数有明确诊断"), Mode->GetLoadoutError().IsEmpty()); }
        return true;
    }
    TestEqual(TEXT("显式服务器预置全部17件"), View.Items.Num(), 17);
    int32 Equipment = 0, Books = 0; TSet<FGuid> Ids;
    for (const auto& Item : View.Items)
    { Ids.Add(Item.ItemId); Equipment += Item.Definition.Type == K::Equipment; Books += Item.Definition.Type == K::Skill; TestEqual(TEXT("只入包不装配"), Item.Location, EY01ItemLocation::Backpack); }
    TestEqual(TEXT("独立实例身份"), Ids.Num(), 17); TestTrue(TEXT("公开内容数组发现装备和书"), Equipment > 0 && Books > 0);
    TestEqual(TEXT("没有假购买扣款或收入"), View.Gold, 0); TestEqual(TEXT("预置不自动穿戴"), View.Stats.AttackDamage, 30.f);
    if (Case == TEXT("Repeat"))
    { Mode->OnPlayerInitialized(PS); FY01CommittedView After; PS->GetLoadout()->ReadCommittedView(After); TestEqual(TEXT("重复hook不再发"), After.Items.Num(), 17); TestEqual(TEXT("重复不改版本"), After.ViewRevision, View.ViewRevision); }
    if (Case == TEXT("LateJoin"))
    {
        auto* Second = Player(); Second->Player = NewObject<ULocalPlayer>(GEngine); Mode->PostLogin(Second);
        FY01CommittedView Other; TestTrue(TEXT("晚加入完整视图"), Second->GetPlayerState<AY01PlayerState>()->GetLoadout()->ReadCommittedView(Other));
        TestEqual(TEXT("每连接独立预置"), Other.Items.Num(), 17);
        for (const auto& Item : Other.Items) { TestFalse(TEXT("跨玩家实例身份不复用"), Ids.Contains(Item.ItemId)); }
    }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36TraceTest, "Y01.Framework.R36.Trace",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36TraceTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Scope"), TEXT("Digest"), TEXT("Receipt"), TEXT("ReadOnlyCapture"), TEXT("NoController"),
            TEXT("WireProgress"), TEXT("UITree"), TEXT("TickEnabled"), TEXT("TickDisabled"), TEXT("Slots"), TEXT("Interaction"), TEXT("Economy"), TEXT("EconomyUI") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36TraceTest::RunTest(const FString& Case)
{
    if (Case == TEXT("NoController"))
    { TestTrue(TEXT("没有Controller不伪造观测"), Y01R36Trace::Capture(nullptr, true).IsEmpty()); return true; }
    FY01CommittedView V; V.MatchId = FGuid::NewGuid(); V.ConnectionId = FGuid::NewGuid(); V.JoinSequence = 2;
    V.ViewRevision = 7; V.ExpectedSeq = 4; V.ResponseSerial = 3; V.Response.Request.Seq = 3;
    V.Response.Code = EY01ItemError::Busy;
    auto& Item = V.Items.AddDefaulted_GetRef(); Item.ItemId = FGuid::NewGuid(); Item.EntityRevision = 1;
    Item.Display.Name = FText::FromString(TEXT("包含引号\"和换行\n的显示名"));
    auto Parse = [](const FString& Text)
    {
        TSharedPtr<FJsonObject> Object; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Object);
        return Object;
    };
    const auto Record = Y01R36Trace::ViewRecord(V); const auto Json = Parse(Record);
    if (!TestTrue(TEXT("Trace是完整JSON对象"), Json.IsValid())) { return true; }
    if (Case == TEXT("Economy"))
    {
        V.Gold = 142; V.IncomeRate = 4; V.Stats.Strength = 0;
        auto& Offer = V.ShopOffers.AddDefaulted_GetRef(); Offer.OfferId = TEXT("Offer"); Offer.Price = 50; Offer.bPurchased = true;
        V.TalentChoice.ChoiceId = FGuid::NewGuid(); V.TalentChoice.SelectedTalent = TEXT("Talent");
        Offer.Effects.Add({TEXT("CustomAttack"), EY01SourceEffectKind::AttackFlat, 2});
        Offer.Effects.Add({TEXT("CustomIncome"), EY01SourceEffectKind::GoldPerSecond, 3});
        V.Response.ProductItemId = Item.ItemId;
        const auto E = Parse(Y01R36Trace::ViewRecord(V)); double Gold = -1, Rate = -1; FString Talent, Product;
        TestTrue(TEXT("金币来自完整视图"), E->TryGetNumberField(TEXT("gold"), Gold)); TestEqual(TEXT("金币142"), Gold, 142.);
        TestTrue(TEXT("收入来自完整视图"), E->TryGetNumberField(TEXT("incomeRate"), Rate)); TestEqual(TEXT("GPS4"), Rate, 4.);
        E->TryGetStringField(TEXT("selectedTalent"), Talent); TestEqual(TEXT("已选权威天赋"), Talent, FString(TEXT("Talent")));
        E->TryGetStringField(TEXT("productItem"), Product); TestEqual(TEXT("真实产物回执"), Product, Item.ItemId.ToString());
        const TArray<TSharedPtr<FJsonValue>>* Offers = nullptr;
        TestTrue(TEXT("数据驱动商品观测"), E->TryGetArrayField(TEXT("offers"), Offers) && Offers->Num() == 1);
        if (Offers && Offers->Num() == 1)
        {
            double Attack = -1, Income = -1;
            TestTrue(TEXT("商品实际攻击效果可观测"), (*Offers)[0]->AsObject()->TryGetNumberField(TEXT("attackEffect"), Attack));
            TestTrue(TEXT("商品实际收入效果可观测"), (*Offers)[0]->AsObject()->TryGetNumberField(TEXT("incomeEffect"), Income));
            TestEqual(TEXT("不是硬编码五攻击"), Attack, 2.); TestEqual(TEXT("不是硬编码一收入"), Income, 3.);
        }
    }
    if (Case == TEXT("Scope"))
    {
        FString Match, Connection; double Join = 0, Revision = 0, Items = 0;
        Json->TryGetStringField(TEXT("match"), Match); Json->TryGetStringField(TEXT("connection"), Connection);
        Json->TryGetNumberField(TEXT("join"), Join); Json->TryGetNumberField(TEXT("revision"), Revision); Json->TryGetNumberField(TEXT("items"), Items);
        TestEqual(TEXT("真实局身份"), Match, V.MatchId.ToString()); TestEqual(TEXT("真实连接身份"), Connection, V.ConnectionId.ToString());
        TestEqual(TEXT("Join"), Join, 2.); TestEqual(TEXT("独立视图版本"), Revision, 7.); TestEqual(TEXT("完整实例数量"), Items, 1.);
    }
    if (Case == TEXT("Digest"))
    {
        FString Digest; Json->TryGetStringField(TEXT("digest"), Digest); TestFalse(TEXT("摘要不可为空"), Digest.IsEmpty());
        TestEqual(TEXT("同视图编码确定"), Y01R36Trace::ViewRecord(V), Record);
        V.Items[0].EntityRevision++; TestNotEqual(TEXT("实体变化必须改变摘要"), Y01R36Trace::ViewRecord(V), Record);
    }
    if (Case == TEXT("Receipt"))
    {
        double Seq = 0, Response = 0, Code = 0; Json->TryGetNumberField(TEXT("seq"), Seq);
        Json->TryGetNumberField(TEXT("response"), Response); Json->TryGetNumberField(TEXT("code"), Code);
        TestEqual(TEXT("失败回执序号"), Seq, 3.); TestEqual(TEXT("响应序列"), Response, 3.);
        TestEqual(TEXT("失败也记录权威结果"), Code, static_cast<double>(EY01ItemError::Busy));
    }
    if (Case == TEXT("Slots"))
    {
        V.EquipmentSlots.SetNum(6); V.EquipmentSlots[0] = Item.ItemId; V.Items[0].Location = EY01ItemLocation::EquipmentSlot;
        V.Skills.SetNum(4); V.Skills[0].LearnedId = FGuid::NewGuid(); V.Skills[0].Deadline = 12;
        const auto Slots = Parse(Y01R36Trace::ViewRecord(V));
        const TArray<TSharedPtr<FJsonValue>>* Equipment = nullptr; const TArray<TSharedPtr<FJsonValue>>* Skills = nullptr;
        Slots->TryGetArrayField(TEXT("equipment"), Equipment); Slots->TryGetArrayField(TEXT("skills"), Skills);
        TestTrue(TEXT("六装备身份槽可核对"), Equipment && Equipment->Num() == 6);
        TestTrue(TEXT("四技能身份和CD可核对"), Skills && Skills->Num() == 4);
        double Bag = -1; Slots->TryGetNumberField(TEXT("bag"), Bag); TestEqual(TEXT("包数量不含身上装备"), Bag, 0.);
    }
    if (Case == TEXT("ReadOnlyCapture") || Case == TEXT("WireProgress") || Case == TEXT("UITree") || Case == TEXT("Interaction") || Case == TEXT("EconomyUI") || Case.StartsWith(TEXT("Tick")))
    {
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World); World->InitializeActorsForPlay(FURL());
        ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); };
        auto* PC = World->SpawnActor<AY01R36Controller>(); auto* PS = World->SpawnActor<AY01PlayerState>();
        PC->PlayerState = PS; PS->SetOwner(PC);
        if (Case.StartsWith(TEXT("Tick")))
        {
            const FString Previous = FCommandLine::Get(); ON_SCOPE_EXIT { FCommandLine::Set(*Previous); };
            FCommandLine::Set(Case == TEXT("TickEnabled") ? TEXT("-R36Trace") : TEXT(""));
            PC->Tick(.3f);
            TestEqual(TEXT("真实Tick只在显式参数下记录"), !PC->GetTrace().IsEmpty(), Case == TEXT("TickEnabled"));
            return true;
        }
        if (Case == TEXT("Interaction"))
        {
            auto* Panel = CreateWidget<UY01R36Panel>(World); const auto PanelSlate = Panel->TakeWidget();
            V.EquipmentSlots.SetNum(6); V.Skills.SetNum(4); V.Health = 100;
            for (const auto Phase : { EY01MatchPhase::Waiting, EY01MatchPhase::Preparing, EY01MatchPhase::Battle, EY01MatchPhase::Ended })
            {
                Panel->Present(V, Phase, 10);
                bool CanDrag = false; const auto UI = Parse(Y01R36Trace::PanelRecord(Panel));
                TestTrue(TEXT("真实输入前必须有明确权限观测"), UI->TryGetBoolField(TEXT("canDrag"), CanDrag));
                TestEqual(TEXT("观测与面板实际权限一致"), CanDrag, Panel->CanDrag());
                TestEqual(TEXT("常驻槽权限按阶段而非默认面板显隐"), CanDrag, Phase == EY01MatchPhase::Preparing || Phase == EY01MatchPhase::Battle);
            }
            TestEqual(TEXT("权限观测不推进视图"), Panel->GetModel().View().ViewRevision, V.ViewRevision);
            return true;
        }
        if (Case == TEXT("EconomyUI"))
        {
            auto* Panel = CreateWidget<UY01R36Panel>(World); const auto PanelSlate = Panel->TakeWidget();
            V.EquipmentSlots.SetNum(6); V.Skills.SetNum(4); V.Health = 100; V.Gold = 200;
            V.ShopOffers.AddDefaulted_GetRef().OfferId = TEXT("Offer"); V.TalentChoice.ChoiceId = FGuid::NewGuid();
            V.TalentChoice.Candidates.AddDefaulted_GetRef().TalentId = TEXT("Talent"); V.SynthesisRecipes.AddDefaulted();
            Panel->Present(V, EY01MatchPhase::Preparing, 10);
            const auto UI = Parse(Y01R36Trace::PanelRecord(Panel));
            for (const auto* Name : {TEXT("Purchase_0"), TEXT("Talent_0"), TEXT("MaterialMode"), TEXT("Synthesize"), TEXT("Training")})
            {
                const TSharedPtr<FJsonObject>* G = nullptr;
                if (!TestTrue(TEXT("真实控件几何可观测"), UI->TryGetObjectField(Name, G))) { continue; }
                bool LaidOut = true; (*G)->TryGetBoolField(TEXT("laidOut"), LaidOut); TestFalse(TEXT("无Viewport不伪造坐标"), LaidOut);
            }
            TestTrue(TEXT("材料夹具完整视图已接收"), Panel->GetModel().View().ViewRevision == V.ViewRevision);
            TestTrue(TEXT("材料夹具允许交互"), Panel->GetModel().CanInteract(EY01MatchPhase::Preparing));
            TestEqual(TEXT("材料夹具存在配方"), Panel->GetModel().View().SynthesisRecipes.Num(), 1);
            TestFalse(TEXT("材料夹具没有确认"), Panel->GetModel().NeedsConfirmation());
            Panel->ToggleBackpack();
            TestTrue(TEXT("材料夹具只展开背包"), Panel->IsBackpackExpanded());
            TestFalse(TEXT("不连带打开人物或商店"), Panel->IsCharacterExpanded() || Panel->IsShopExpanded());
            Panel->MaterialMode();
            TestTrue(TEXT("材料模式实际面板状态"), Panel->IsSelectingMaterials());
            const auto Selected = Parse(Y01R36Trace::PanelRecord(Panel)); bool Mode = false;
            TestTrue(TEXT("材料模式真实状态"), Selected->TryGetBoolField(TEXT("selectingMaterials"), Mode) && Mode);
            return true;
        }
        if (Case == TEXT("UITree"))
        {
            auto* Panel = CreateWidget<UY01R36Panel>(World); const auto PanelSlate = Panel->TakeWidget();
            V.EquipmentSlots.SetNum(6); V.Skills.SetNum(4); V.Health = 100;
            Panel->Present(V, EY01MatchPhase::Preparing, 10);
            const auto UI = Parse(Y01R36Trace::PanelRecord(Panel)); double Total = 0; UI->TryGetNumberField(TEXT("total"), Total);
            TestEqual(TEXT("真实TileView列表数量"), Total, 1.);
            const TArray<TSharedPtr<FJsonValue>>* Cells = nullptr; UI->TryGetArrayField(TEXT("cells"), Cells);
            TestTrue(TEXT("未布局时仍观测六四槽，不伪造屏幕位置"), Cells && Cells->Num() == 10);
            TestFalse(TEXT("默认背包收起"), UI->GetBoolField(TEXT("backpackExpanded")));
            TestFalse(TEXT("默认人物收起"), UI->GetBoolField(TEXT("characterExpanded")));
            TestFalse(TEXT("默认商店收起"), UI->GetBoolField(TEXT("shopExpanded")));
            TestEqual(TEXT("旧字段明确仅兼容背包含义"), UI->GetStringField(TEXT("panelsExpandedMeaning")), FString(TEXT("backpack-only-compatibility")));
            TestFalse(TEXT("未知时钟不冒充保持样本"), UI->GetBoolField(TEXT("waveTimeHeld")));
            TestEqual(TEXT("无GS时钟未知"), UI->GetStringField(TEXT("waveClockSource")), FString(TEXT("Unknown")));
            for (const auto& Cell : *Cells)
            { TestTrue(TEXT("底槽始终可见"), Cell->AsObject()->GetBoolField(TEXT("visible"))); }
            for (const auto* Name : {TEXT("ToggleCharacter"), TEXT("ToggleBackpack"), TEXT("ToggleShop"), TEXT("TopVitals"), TEXT("WaveCountdown"), TEXT("StatValue_IncomeRate")})
            { TestTrue(TEXT("新控件契约存在"), UI->HasField(Name)); }
            TestFalse(TEXT("无旧手动下一波控件"), UI->HasField(TEXT("Advance")));
            auto* GS = World->SpawnActor<AY01GameState>(); World->SetGameState(GS);
            auto& Waves = *FindFProperty<FStructProperty>(GS->GetClass(), TEXT("Snapshot"))->ContainerPtrToValuePtr<FY01WaveSnapshot>(GS);
            Waves.bAutomaticWaves = true; Waves.TotalWaves = 5; Waves.NextWaveIndex = 2; Waves.NextWaveDeadline = 25;
            Waves.TotalAliveMonsters = 7; Waves.TotalPendingSpawns = 2; Waves.Revision = 19;
            V.Skills[0].LearnedId = FGuid::NewGuid(); V.Skills[0].Display.SkillType = EY01SkillType::Active; V.Skills[0].Deadline = 30;
            ++V.ViewRevision; Panel->Present(V, EY01MatchPhase::Battle, 21);
            const auto Timed = Parse(Y01R36Trace::PanelRecord(Panel));
            TestEqual(TEXT("Trace明确权威样本来源"), Timed->GetStringField(TEXT("waveClockSource")), FString(TEXT("AuthoritySample")));
            TestEqual(TEXT("Trace服务器时间"), Timed->GetNumberField(TEXT("waveServerTime")), 21.);
            TestEqual(TEXT("Trace下一波0基索引"), Timed->GetNumberField(TEXT("nextWaveIndex")), 2.);
            TestEqual(TEXT("Trace下一波deadline"), Timed->GetNumberField(TEXT("nextWaveDeadline")), 25.);
            TestEqual(TEXT("Trace全局存活"), Timed->GetNumberField(TEXT("totalAliveMonsters")), 7.);
            TestEqual(TEXT("Trace待生成"), Timed->GetNumberField(TEXT("totalPendingSpawns")), 2.);
            TestEqual(TEXT("Trace波次版本"), Timed->GetNumberField(TEXT("waveRevision")), 19.);
            TestEqual(TEXT("Trace顶部真实文字"), Timed->GetObjectField(TEXT("WaveCountdown"))->GetStringField(TEXT("text")), FString(TEXT("第 3 波还有 4 秒")));
            const auto Skill = Timed->GetArrayField(TEXT("cells"))[6]->AsObject();
            TestEqual(TEXT("Trace技能权威deadline"), Skill->GetNumberField(TEXT("deadline")), 30.);
            TestEqual(TEXT("Trace技能剩余CD"), Skill->GetNumberField(TEXT("remaining")), 9.);
            TestTrue(TEXT("Trace技能标签包含CD"), Skill->GetStringField(TEXT("label")).Contains(TEXT("9.0")));
            TestEqual(TEXT("观测不消耗物品"), Panel->GetModel().View().Items.Num(), 1);
            return true;
        }
        if (Case == TEXT("WireProgress"))
        {
            auto& Wire = *FindFProperty<FStructProperty>(PS->GetLoadout()->GetClass(), TEXT("ViewWire"))->ContainerPtrToValuePtr<FY01ViewWire>(PS->GetLoadout());
            auto& Chunk = Wire.Items.AddDefaulted_GetRef(); Chunk.Scope.MatchId = V.MatchId; Chunk.Scope.JoinSequence = 2;
            Chunk.Scope.ConnectionId = V.ConnectionId; Chunk.Scope.TransferId = FGuid::NewGuid();
            Chunk.Scope.WindowSequence = 3; Chunk.Offset = 8192; Chunk.TotalBytes = 20000; Chunk.Bytes.SetNum(1024);
            bool Found = false;
            for (const auto& Row : Y01R36Trace::Capture(PC, true))
            {
                const auto WireJson = Parse(Row); FString Kind; WireJson->TryGetStringField(TEXT("kind"), Kind);
                if (Kind == TEXT("WIRE"))
                {
                    double Window = 0, Total = 0, End = 0; WireJson->TryGetNumberField(TEXT("window"), Window);
                    WireJson->TryGetNumberField(TEXT("bytes"), Total); WireJson->TryGetNumberField(TEXT("end"), End);
                    Found = true; TestEqual(TEXT("实际窗口序号"), Window, 3.); TestEqual(TEXT("传输总字节"), Total, 20000.);
                    TestEqual(TEXT("当前进度"), End, 9216.);
                }
            }
            TestTrue(TEXT("即使完整视图未到也观测在途窗口"), Found);
            TestEqual(TEXT("观测不能推进窗口或ACK"), Wire.Items[0].Scope.WindowSequence, 3); return true;
        }
        const auto Rows = Y01R36Trace::Capture(PC, true);
        TestFalse(TEXT("真实Controller可观测到未初始化PS及零私有视图"), Rows.IsEmpty());
        TestFalse(TEXT("观测不能初始化库存"), PS->GetLoadout()->IsInitialized());
        FY01CommittedView Out; TestFalse(TEXT("未收完整提交不能伪造视图"), PS->GetLoadout()->ReadCommittedView(Out));
        if (!Rows.IsEmpty())
        {
            const auto Scope = Parse(Rows.Last()); double Count = -1;
            if (Scope) { Scope->TryGetNumberField(TEXT("privateViews"), Count); }
            TestEqual(TEXT("未初始化没有私有视图"), Count, 0.);
        }
    }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36StopTest, "Y01.Framework.R36.ControlledStop",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36StopTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Valid"), TEXT("Once"), TEXT("Disabled"), TEXT("WrongToken"), TEXT("Traversal"),
        TEXT("NoFixture"), TEXT("Client"), TEXT("Oversized"), TEXT("Missing"), TEXT("S2Controlled"), TEXT("S2Runtime"),
                TEXT("S2Natural"), TEXT("S2NoMarker"), TEXT("S1Natural"), TEXT("S2WrongToken"), TEXT("S2Traversal"),
        TEXT("S3Controlled"), TEXT("S3Natural"), TEXT("S3Presentation"), TEXT("S3NoMarker"), TEXT("S3WrongToken"), TEXT("S3Traversal"),
        TEXT("S4Controlled"), TEXT("S4Natural"), TEXT("S4NoMarker"), TEXT("S4WrongToken"), TEXT("S4Traversal"), TEXT("S4Mixed") }) { Names.Add(N); Commands.Add(N); }
}
bool FY01R36StopTest::RunTest(const FString& Case)
{
    const FString Previous = FCommandLine::Get(); ON_SCOPE_EXIT { FCommandLine::Set(*Previous); };
    const FString Prefix = Case.StartsWith(TEXT("S4")) ? TEXT("R36-S4-Custom-") : Case == TEXT("S3Presentation") ? TEXT("R36-S3-Presentation-") : Case.StartsWith(TEXT("S3")) ? TEXT("R36-S3-Visible-")
        : Case == TEXT("S2Runtime") ? TEXT("R36-S2-Runtime-") : Case.StartsWith(TEXT("S2")) ? TEXT("R36-S2-Visible-") : TEXT("R36-S1-UI-");
    const FString Label = Prefix + TEXT("StopFixture-") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Token = FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Directory = FPaths::ProjectSavedDir() / Label;
    IFileManager::Get().MakeDirectory(*Directory, true);
    FString FileToken = Case.EndsWith(TEXT("WrongToken")) ? FGuid::NewGuid().ToString(EGuidFormats::Digits) : Token;
    if (Case == TEXT("Oversized")) { FileToken += TEXT("extra"); }
    if (Case != TEXT("Missing")) { TestTrue(TEXT("隔离停止夹具可写"), FFileHelper::SaveStringToFile(FileToken,
        *(Directory / TEXT("stop.request")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)); }
    FString Params = FString::Printf(TEXT("-R36Fixture=controlled -R36Run=%s -R36StopToken=%s"), *Label, *Token);
    if (Case == TEXT("Disabled")) { Params = TEXT(""); }
    if (Case.EndsWith(TEXT("Traversal"))) { Params.ReplaceInline(*Label, *(TEXT("../Saved/") + Label)); }
    if (Case == TEXT("NoFixture") || Case.EndsWith(TEXT("NoMarker"))) { Params.ReplaceInline(TEXT("-R36Fixture=controlled"), TEXT("")); }
    if (Case.EndsWith(TEXT("Natural"))) { Params.ReplaceInline(TEXT("-R36Fixture=controlled"), TEXT("-R36Scenario=natural")); }
    if (Case == TEXT("S4Mixed")) { Params += TEXT(" -R36Scenario=natural"); }
    FCommandLine::Set(*Params);
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false)
        .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    ON_SCOPE_EXIT { World->DestroyWorld(false); GEngine->DestroyWorldContext(World); };
    auto* Mode = World->SpawnActor<AY01R36RuntimeProbe>();
    if (Case == TEXT("Client")) { Mode->SetRole(ROLE_SimulatedProxy); }
    Mode->Tick(.5f);
    if (Case == TEXT("Once")) { Mode->Tick(.5f); }
    TestEqual(TEXT("仅本次显式controlled有效令牌请求一次正常退出"), Mode->StopRequests,
        Case == TEXT("Valid") || Case == TEXT("Once") || Case == TEXT("S2Controlled") || Case == TEXT("S2Runtime") || Case == TEXT("S2Natural")
        || Case == TEXT("S3Controlled") || Case == TEXT("S3Natural") || Case == TEXT("S3Presentation")
                || Case == TEXT("S4Controlled") || Case == TEXT("S4Natural") ? 1 : 0);
    TestEqual(TEXT("停止后不删除夹具"), IFileManager::Get().FileExists(*(Directory / TEXT("stop.request"))), Case != TEXT("Missing"));
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36S3TraceTest, "Y01.Framework.R36.Trace.PresentationS3",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36S3TraceTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Growth"), TEXT("Visual"), TEXT("Sample"), TEXT("Training")}) { Names.Add(N); Commands.Add(N); }
}
bool FY01R36S3TraceTest::RunTest(const FString& Case)
{
    auto Parse = [](const FString& Text)
    { TSharedPtr<FJsonObject> J; FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J); return J; };
    FY01CommittedView V; V.MatchId = FGuid::NewGuid(); V.ConnectionId = FGuid::NewGuid(); V.JoinSequence = 1;
    V.ViewRevision = 1; V.Health = 100; V.EquipmentSlots.SetNum(6); V.Skills.SetNum(4);
    auto& Item = V.Items.AddDefaulted_GetRef(); Item.ItemId = FGuid::NewGuid(); Item.ArrivalDeliveryId = FGuid::NewGuid();
    if (Case == TEXT("Growth"))
    {
        auto& G = Item.Growth.AddDefaulted_GetRef(); G.RuleId = TEXT("Attack"); G.Delta = 1; G.AppliedCount = MAX_uint64; G.Accumulated = 3;
        auto& P = V.Growth.PlayerRules.AddDefaulted_GetRef(); P.SourceId = TEXT("Equipment:source"); P.Rule = G;
        P.Rule.RuleId = TEXT("Strength"); P.Rule.Target = EY01GrowthTarget::Player; P.Rule.Attribute = EY01GrowthAttribute::Strength;
        V.Growth.bEnabled = true; V.Growth.AppliedKills = MAX_uint64; V.Growth.Strength = 3;
        const auto J = Parse(Y01R36Trace::ViewRecord(V));
        const TSharedPtr<FJsonObject>* Ledger = nullptr; const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
        if (TestTrue(TEXT("完整来源账本可观测"), J->TryGetObjectField(TEXT("growth"), Ledger)))
        {
            FString Count; (*Ledger)->TryGetStringField(TEXT("appliedKills"), Count); TestEqual(TEXT("uint64不丢精度"), Count, LexToString(MAX_uint64));
            const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
            if (TestTrue(TEXT("逐来源玩家成长"), (*Ledger)->TryGetArrayField(TEXT("playerRules"), Rules) && Rules->Num() == 1))
            { TestEqual(TEXT("来源身份不替换为显示名"), (*Rules)[0]->AsObject()->GetStringField(TEXT("source")), P.SourceId); }
        }
        if (TestTrue(TEXT("全部实例成长明细"), J->TryGetArrayField(TEXT("itemDetails"), Items) && Items->Num() == 1))
        {
            const auto Detail = (*Items)[0]->AsObject(); const TArray<TSharedPtr<FJsonValue>>* Rules = nullptr;
            TestEqual(TEXT("实例身份"), Detail->GetStringField(TEXT("item")), Item.ItemId.ToString());
            if (TestTrue(TEXT("实例独立累计"), Detail->TryGetArrayField(TEXT("growth"), Rules) && Rules->Num() == 1))
            { TestEqual(TEXT("原始累计"), (*Rules)[0]->AsObject()->GetNumberField(TEXT("accumulated")), 3.);
              TestEqual(TEXT("逐规则次数精确"), (*Rules)[0]->AsObject()->GetStringField(TEXT("appliedCount")), LexToString(MAX_uint64)); }
        }
        TestEqual(TEXT("只读不改账本"), V.Growth.AppliedKills, MAX_uint64); return true;
    }
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World); World->InitializeActorsForPlay(FURL());
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); };
    auto* PC = World->SpawnActor<AY01R36Controller>();
    if (Case == TEXT("Sample"))
    {
        const FString Previous = FCommandLine::Get(); ON_SCOPE_EXIT { FCommandLine::Set(*Previous); };
        FCommandLine::Set(TEXT("-R36Trace -R36Run=R36-S3-Visible-Sample")); PC->Tick(.1f);
        const auto Before = PC->GetTrace(); FPlatformProcess::Sleep(.12f); PC->Tick(.1f);
        TestTrue(TEXT("S3真实时钟100ms采样覆盖0.6秒闪格"), Before != PC->GetTrace()); return true;
    }
    if (Case == TEXT("Training"))
    {
        const auto Rows = Y01R36Trace::Capture(PC, false); const auto J = Parse(Rows.Last());
        double Kills = -1, Alive = -1;
        TestTrue(TEXT("训练杀数直接读取观察器"), J->TryGetNumberField(TEXT("trainingKills"), Kills));
        TestTrue(TEXT("训练存活数"), J->TryGetNumberField(TEXT("trainingAlive"), Alive));
        TestEqual(TEXT("训练观测原值"), Kills, double(PC->GetObserver().Training.Kills)); return true;
    }
    auto* Panel = CreateWidget<UY01R36Panel>(World); Panel->Present(V, EY01MatchPhase::Preparing, 10);
    auto* Layer = Cast<UY01R36FlightLayer>(Panel->WidgetTree->FindWidget(TEXT("DeliveryFlightLayer")));
    if (!TestNotNull(TEXT("已有真实飞行图层"), Layer)) { return false; }
    FY01R36ProjectedFlight Flight; Flight.Flight.Alpha = .5; Flight.Origin = FVector2D(20,40); Flight.Target = FVector2D(220,240);
    const auto Slate = Panel->TakeWidget();
    Layer->Present({Flight}); const auto J = Parse(Y01R36Trace::PanelRecord(Panel));
    double Unseen = -1; bool Enabled = false; const TArray<TSharedPtr<FJsonValue>>* Glyphs = nullptr;
    TestTrue(TEXT("未显示提示只读观测"), J->TryGetNumberField(TEXT("unseenArrivals"), Unseen)); TestEqual(TEXT("未布局不消费"), Unseen, 1.);
    TestTrue(TEXT("本地飞行开关"), J->TryGetBoolField(TEXT("flightsEnabled"), Enabled) && Enabled);
    TestTrue(TEXT("真实五图元而非模型条数"), J->TryGetArrayField(TEXT("flightGlyphs"), Glyphs) && Glyphs->Num() == 5);
    const TArray<TSharedPtr<FJsonValue>>* Cells = nullptr;
    if (J->TryGetArrayField(TEXT("cells"), Cells))
    { for (const auto& Cell : *Cells) { double Flash = -1; TestTrue(TEXT("每格闪烁剩余可采样"), Cell->AsObject()->TryGetNumberField(TEXT("flashRemaining"), Flash)); TestEqual(TEXT("非包格不闪"), Flash, 0.); } }
    const TSharedPtr<FJsonObject>* Button = nullptr; TestTrue(TEXT("复用现有禁播按钮几何"), J->TryGetObjectField(TEXT("ToggleFlights"), Button));
    Panel->ToggleFlights(); const auto Off = Parse(Y01R36Trace::PanelRecord(Panel));
    TestTrue(TEXT("禁播后真实图元清空"), Off->TryGetArrayField(TEXT("flightGlyphs"), Glyphs) && Glyphs->IsEmpty());
    TestEqual(TEXT("观测与禁播无库存副作用"), Panel->GetModel().View().Items.Num(), 1); return true;
}
#endif
