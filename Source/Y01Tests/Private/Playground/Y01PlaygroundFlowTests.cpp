#include "Misc/AutomationTest.h"
#include "Playground/Y01PlaygroundProfile.h"
#include "Playground/Y01PlaygroundSeed.h"
#include "Match/Y01GameFlow.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01Character.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "UObject/UnrealType.h"
#include "Playground/Y01PlaygroundController.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/WorldSettings.h"
#include "EnhancedPlayerInput.h"
#include "Playground/Y01R36Runtime.h"
#include "Combat/Y01Targeting.h"
#include "Match/Y01SpawnPoint.h"
#include "Engine/GameInstance.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
UClass* PlaygroundClass(const TCHAR* Name)
{
    return FindObject<UClass>(nullptr, *(FString(TEXT("/Script/Y01Tests.")) + Name));
}
struct FPlaygroundWorld
{
    UWorld* World;
    AY01GameMode* Mode;
    AY01PlayerState* Player;
    AY01Character* Pawn;
    TStrongObjectPtr<UY01PlaygroundProfile> Profile;
    FPlaygroundWorld() : Profile(Y01PlaygroundEditor::CreateSeed(GetTransientPackage()))
    {
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
        UClass* Class = PlaygroundClass(TEXT("Y01PlaygroundGameMode"));
        Mode = World->SpawnActor<AY01GameMode>(Class ? Class : AY01GameMode::StaticClass());
        Player = World->SpawnActor<AY01PlayerState>();
        Pawn = World->SpawnActor<AY01Character>();
        Player->GetCombatASC()->InitAbilityActorInfo(Player, Pawn);
        Mode->GetFlowState()->AddPlayerState(Player);
    }
    ~FPlaygroundWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    void Tick(float Seconds)
    {
        for (int32 I = 0; I < FMath::CeilToInt(Seconds / 0.05f); ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, 0.05f); }
    }
    bool AddConfig(UY01PlaygroundProfile* Value)
    {
        UClass* Class = PlaygroundClass(TEXT("Y01PlaygroundConfig"));
        if (!Class) { return false; }
        AActor* Config = World->SpawnActor<AActor>(Class);
        auto* Property = FindFProperty<FObjectProperty>(Class, TEXT("Profile"));
        if (!Property) { return false; }
        Property->SetObjectPropertyValue_InContainer(Config, Value);
        return true;
    }
    const FY01WaveSnapshot& Snapshot() const { return Mode->GetFlowState()->GetWaveSnapshot(); }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01PlaygroundFlowTest, "Y01.Playground.Flow",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01PlaygroundFlowTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("SavedProfile"), TEXT("MissingProfile"), TEXT("InvalidTable"), TEXT("DuplicateConfig"),
        TEXT("LivingMonsterBlocksReward"), TEXT("EnterAndReward"), TEXT("FinalWaveRetainsEnded"), TEXT("FailureOnZeroHealth"),
        TEXT("NoPreparationTarget"), TEXT("TestOnlyClasses"), TEXT("ManualWaits"), TEXT("DeathBeforeFinalSuccess") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01PlaygroundFlowTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> FrameGuard(GFrameCounter, GFrameCounter);
    FPlaygroundWorld F;
    if (Case == TEXT("TestOnlyClasses"))
    {
        for (const TCHAR* Name : { TEXT("Y01PlaygroundGameMode"), TEXT("Y01PlaygroundConfig"), TEXT("Y01PlaygroundController"),
            TEXT("Y01PlaygroundCharacter"), TEXT("Y01PlaygroundHUD"), TEXT("Y01PlaygroundVisual") })
        { TestNotNull(Name, PlaygroundClass(Name)); }
        return true;
    }
    if (Case == TEXT("LivingMonsterBlocksReward"))
    {
        FString Error;
        TestTrue(TEXT("基础配置真实目录"), F.Mode->TryConfigureMatch(F.Profile->BuildCatalog(), F.Profile->OrderedWaves, F.Profile->Character, Error));
        TestTrue(TEXT("开始"), F.Mode->TryStartNextWave()); F.Tick(6);
        TestEqual(TEXT("真实刷出三怪"), F.Mode->GetWaveMonsters().Num(), 3);
        TestFalse(TEXT("活怪存在不许提前领奖"), F.Mode->TryCompleteWave());
        TestEqual(TEXT("金币未变"), F.Player->GetGold(), 0);
        return true;
    }
    if (Case == TEXT("SavedProfile"))
    {
        F.Profile.Reset(LoadObject<UY01PlaygroundProfile>(nullptr, TEXT("/Game/Y01Playground/DA_PlaygroundProfile.DA_PlaygroundProfile")));
        if (!TestNotNull(TEXT("读取磁盘profile"), F.Profile.Get())) { return false; }
    }
    if (Case == TEXT("InvalidTable"))
    { F.Profile->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("FlowTest"))->Health = -1; }
    const bool bBad = Case == TEXT("MissingProfile") || Case == TEXT("InvalidTable") || Case == TEXT("DuplicateConfig");
    TestTrue(TEXT("场景配置Actor及强引用可接线"), F.AddConfig(Case == TEXT("MissingProfile") ? nullptr : F.Profile.Get()));
    if (Case == TEXT("DuplicateConfig")) { TestTrue(TEXT("第二配置Actor"), F.AddConfig(F.Profile.Get())); }
    if (bBad || !PlaygroundClass(TEXT("Y01PlaygroundGameMode")))
    { AddExpectedError(TEXT("MissingConfiguration"), EAutomationExpectedErrorFlags::Contains, 1); }
    F.Mode->DispatchBeginPlay();
    if (bBad)
    {
        TestFalse(TEXT("错误配置不得启动"), F.Mode->IsMatchConfigured());
        TestEqual(TEXT("保留Waiting"), F.Snapshot().Phase, EY01MatchPhase::Waiting);
        TestFalse(TEXT("具体错误可见"), F.Mode->GetConfigurationError().IsEmpty());
        if (Case == TEXT("InvalidTable")) { TestTrue(TEXT("错误指明行字段"), F.Mode->GetConfigurationError().Contains(TEXT("Health"))); }
        return true;
    }
    if (!TestTrue(TEXT("从场景profile而非全局Settings初始化"), F.Mode->IsMatchConfigured())) { return false; }
    if (Case == TEXT("SavedProfile"))
    {
        FY01DataValidation Query;
        const auto* Row = F.Profile->BuildCatalog().FindRow<FY01CharacterRow>(F.Profile->Character, Query);
        TestEqual(TEXT("实际HP来源于保存表"), F.Player->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), Row->Health);
        TestEqual(TEXT("等待明确操作"), F.Snapshot().Phase, EY01MatchPhase::Waiting); return true;
    }
    if (Case == TEXT("ManualWaits"))
    {
        F.Tick(20);
        TestFalse(TEXT("旧Playground自动策略默认关闭"), F.Snapshot().bAutomaticWaves);
        TestEqual(TEXT("旧入口长期等待手动开始"), F.Snapshot().Phase, EY01MatchPhase::Waiting);
        TestEqual(TEXT("旧入口未自动生成怪物"), F.Mode->GetWaveMonsters().Num(), 0);
    }
    TestTrue(TEXT("Enter开始波次"), F.Mode->TryStartNextWave());
    TestFalse(TEXT("准备期间重复Enter拒绝"), F.Mode->TryStartNextWave());
    if (Case == TEXT("NoPreparationTarget"))
    {
        TestNull(TEXT("准备期没有攻击目标"), F.Player->GetCombatASC()->GetCombatTarget());
        TestFalse(TEXT("准备期按键无伤害"), F.Player->TryActivateSkillSlot(0)); return true;
    }
    F.Tick(6);
    if (Case == TEXT("FailureOnZeroHealth"))
    {
        F.Player->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0);
        F.Tick(0.15f);
        TestEqual(TEXT("测试失败Ended"), F.Snapshot().Phase, EY01MatchPhase::Ended);
        TestEqual(TEXT("失败结果"), F.Snapshot().Result, EY01MatchResult::Failure); return true;
    }
    auto KillWave = [&]()
    {
        for (AY01Monster* Monster : F.Mode->GetWaveMonsters())
        { Monster->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        F.Tick(0.2f);
    };
    KillWave();
    TestTrue(TEXT("全死服务器自动结算"), F.Snapshot().bWaveCompleted);
    TestEqual(TEXT("真实金币"), F.Player->GetGold(), 10);
    TestEqual(TEXT("真实经验"), F.Player->GetExperience(), 5);
    TestFalse(TEXT("领奖幂等"), F.Mode->TryCompleteWave());
    TestNull(TEXT("领奖后清除目标"), F.Player->GetCombatASC()->GetCombatTarget());
    if (Case == TEXT("ManualWaits"))
    {
        const auto First = F.Mode->GetWaveMonsters()[0]; F.Tick(20);
        TestEqual(TEXT("旧入口清波后仍等待手动Next"), F.Snapshot().WaveIndex, 0);
        TestTrue(TEXT("旧手动下一波仍可接受"), F.Mode->TryStartNextWave());
        TestTrue(TEXT("旧手动路径保留旧怪清理"), !IsValid(First) || First->IsActorBeingDestroyed()); return true;
    }
    if (Case == TEXT("DeathBeforeFinalSuccess"))
    {
        TestTrue(TEXT("手动末波开始"), F.Mode->TryStartNextWave()); F.Tick(9);
        for (AY01Monster* Monster : F.Mode->GetWaveMonsters())
        { Monster->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        TestTrue(TEXT("末波奖励提交产生下一Tick胜利候选"), F.Mode->TryCompleteWave());
        F.Player->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); F.Tick(.05f);
        TestEqual(TEXT("同帧死亡不能被既有胜利候选覆盖"), F.Snapshot().Result, EY01MatchResult::Failure);
        TestFalse(TEXT("死亡结束不可再启动"), F.Mode->TryStartNextWave()); return true;
    }
    if (Case == TEXT("FinalWaveRetainsEnded"))
    {
        TestTrue(TEXT("下一波明确开始"), F.Mode->TryStartNextWave()); F.Tick(9); KillWave(); F.Tick(0.2f);
        TestEqual(TEXT("最终保留Ended"), F.Snapshot().Phase, EY01MatchPhase::Ended);
        TestEqual(TEXT("成功"), F.Snapshot().Result, EY01MatchResult::Success);
        TestEqual(TEXT("两波实际奖励"), F.Player->GetGold(), 35);
        TestFalse(TEXT("Ended不能继续"), F.Mode->TryStartNextWave());
    }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01PlaygroundHUDTest, "Y01.Playground.HUD",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01PlaygroundHUDTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("AuthoritativeSlots"), TEXT("CooldownDeadline"), TEXT("ErrorVisible"), TEXT("OwnerOnly"),
        TEXT("ContinuousMovementAndDamage"), TEXT("ExplicitRestartNotice"), TEXT("PersistedMonsterSkill"), TEXT("EditedMonsterSkill") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01PlaygroundHUDTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> FrameGuard(GFrameCounter, GFrameCounter);
    FPlaygroundWorld F;
    auto* Mode = Cast<AY01PlaygroundGameMode>(F.Mode);
    auto* PC = F.World->SpawnActor<AY01PlaygroundController>();
    PC->PlayerState = F.Player; PC->Possess(F.Pawn);
    auto* Floor = F.World->SpawnActor<AStaticMeshActor>(FVector(0, 0, -50), FRotator::ZeroRotator);
    Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
    Floor->SetActorScale3D(FVector(30, 30, 1));
    Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
    F.Pawn->SetActorLocation(FVector(0, 0, 90));
    if (Case == TEXT("PersistedMonsterSkill") || Case == TEXT("EditedMonsterSkill"))
    {
        auto* Saved = Case == TEXT("PersistedMonsterSkill")
            ? LoadObject<UY01PlaygroundProfile>(nullptr, TEXT("/Game/Y01Playground/DA_PlaygroundProfile.DA_PlaygroundProfile")) : F.Profile.Get();
        if (!TestNotNull(TEXT("持久配置或隔离编辑配置"), Saved)) { return false; }
        if (Case == TEXT("EditedMonsterSkill"))
        {
            for (const FName Name : { FName(TEXT("Default")), FName(TEXT("Boss")) })
            { Saved->Tables[EY01TableKind::Monster]->FindRow<FY01MonsterRow>(Name, TEXT("HUDTest"))->Skills[0].RowName = TEXT("HeavyAttack"); }
            Saved->Tables[EY01TableKind::Skill]->FindRow<FY01SkillRow>(TEXT("HeavyAttack"), TEXT("HUDTest"))->CooldownSeconds = 2.75f;
            if (!TestTrue(TEXT("自定义技能引用及CD是合法用户编辑"), Saved->Validate().IsValid())) { return false; }
        }
        if (!TestTrue(TEXT("用户配置全部引用合法"), Saved->Validate().IsValid())) { return false; }
        const FY01DataCatalog Catalog = Saved->BuildCatalog();
        FY01DataValidation Query;
        FString Error;
        if (Case == TEXT("EditedMonsterSkill") && !TestTrue(TEXT("初始化真实攻击目标"),
            F.Player->GetCombatASC()->TryInitializeFromData(Catalog, Saved->Character, Error))) { return false; }
        const auto* Table = Saved->Tables.FindRef(EY01TableKind::Monster).Get();
        if (!TestNotNull(TEXT("可加载怪物表"), Table)) { return false; }
        for (const FName Name : Table->GetRowNames())
        {
            FY01DataRef Definition; Definition.Type = EY01TableKind::Monster; Definition.RowName = Name;
            const auto* Row = Catalog.FindRow<FY01MonsterRow>(Definition, Query);
            if (!TestNotNull(TEXT("怪物行可解析"), Row)) { return false; }
            auto* Monster = F.World->SpawnActor<AY01Monster>();
            if (!TestNotNull(TEXT("真实怪物实例"), Monster)) { return false; }
            if (!TestTrue(TEXT("运行时按当前表初始化：") + Error, Monster->TryInitializeMonster(Catalog, Definition, Error))) { return false; }
            auto* ASC = Monster->GetCombatASC();
            TestEqual(TEXT("怪物HP取当前表而非种子常量"), Y01Playground::Health(ASC), Row->Health);
            TestEqual(TEXT("运行时技能数量与当前表一致"), ASC->GetInitialSkills().Num(), Row->Skills.Num());
            for (int32 Slot = 0; Slot < Row->Skills.Num(); ++Slot)
            {
                const auto& Ref = Row->Skills[Slot];
                TestTrue(TEXT("引用技能表"), Ref.Type == EY01TableKind::Skill);
                const auto* Skill = Catalog.FindRow<FY01SkillRow>(Ref, Query);
                if (!TestNotNull(TEXT("实际引用技能有效"), Skill)) { return false; }
                TestTrue(TEXT("实际技能CD有限且非负"), FMath::IsFinite(Skill->CooldownSeconds) && Skill->CooldownSeconds >= 0);
                const auto* Grant = ASC->GetGrantedSources().FindByPredicate([Slot](const FY01GrantedSource& Item)
                    { return Item.Source.Type == EY01TableKind::Skill && Item.Slot == Slot; });
                if (!TestNotNull(TEXT("实际槽位授予"), Grant)) { return false; }
                TestEqual(TEXT("运行时采用用户选定ref"), Grant->Source.RowName, Ref.RowName);
                TestEqual(TEXT("运行时采用该ref的CD"), Grant->CooldownSeconds, Skill->CooldownSeconds);
                if (Case == TEXT("EditedMonsterSkill"))
                {
                    Monster->SetActorLocation(F.Pawn->GetActorLocation() + FVector(100, 0, 0));
                    TestTrue(TEXT("设置真实敌对目标"), ASC->SetCombatTarget(F.Pawn));
                    TestTrue(TEXT("用户选择的合法技能可激活"), ASC->TryActivateSkillSlot(Slot));
                    TestTrue(TEXT("实际截止时间取自编辑后的CD"), FMath::IsNearlyEqual(
                        Grant->NextActivationTime - F.World->GetTimeSeconds(), static_cast<double>(Skill->CooldownSeconds), 0.001));
                    TestFalse(TEXT("自定义CD内禁止重复激活"), ASC->TryActivateSkillSlot(Slot));
                }
                AddInfo(FString::Printf(TEXT("Monster=%s HP=%g Slot=%d Ref=%s CD=%g"),
                    *Name.ToString(), Row->Health, Slot, *Ref.RowName.ToString(), Skill->CooldownSeconds));
            }
            Monster->Destroy();
        }
        TestTrue(TEXT("引用解析无错误"), Query.IsValid());
        return true;
    }
    if (Case == TEXT("OwnerOnly"))
    {
        PC->GetClass()->SetUpRuntimeReplicationData();
        auto* Prop = FindFProperty<FStructProperty>(PC->GetClass(), TEXT("Observer"));
        TArray<FLifetimeProperty> Props; PC->GetLifetimeReplicatedProps(Props);
        TestTrue(TEXT("注册OwnerOnly观测复制"), Prop && Props.ContainsByPredicate([&](const auto& P)
            { return P.RepIndex == Prop->RepIndex && P.Condition == COND_OwnerOnly; })); return true;
    }
    F.AddConfig(Case == TEXT("ErrorVisible") ? nullptr : F.Profile.Get());
    if (Case == TEXT("ErrorVisible")) { AddExpectedError(TEXT("MissingConfiguration"), EAutomationExpectedErrorFlags::Contains, 1); }
    F.Mode->DispatchBeginPlay(); PC->CaptureObserver(Mode);
    if (Case == TEXT("ErrorVisible"))
    { TestTrue(TEXT("远端错误模型包含具体配置错误"), PC->GetObserver().Error.Contains(TEXT("MissingConfiguration"))); return true; }
    if (Case == TEXT("ExplicitRestartNotice"))
    {
        const auto IsF5 = [](const FKeyBind& Bind) { return Bind.Key == EKeys::F5; };
        const auto* Defaults = GetDefault<UEnhancedPlayerInput>();
        const int32 DefaultCount = Defaults->DebugExecBindings.Num();
        TestTrue(TEXT("引擎默认存在F5调试绑定，真实复现冲突"), Defaults->DebugExecBindings.ContainsByPredicate(IsF5));
        PC->PlayerInput = NewObject<UEnhancedPlayerInput>(PC);
        const int32 OtherBindings = PC->PlayerInput->DebugExecBindings.FilterByPredicate(
            [](const FKeyBind& Bind) { return Bind.Key != EKeys::F5; }).Num();
        PC->InitInputSystem();
        TestFalse(TEXT("Playground F5不得同时切换调试渲染视图"), PC->PlayerInput->DebugExecBindings.ContainsByPredicate(IsF5));
        TestEqual(TEXT("保留其他实例调试键"), PC->PlayerInput->DebugExecBindings.Num(), OtherBindings);
        TestEqual(TEXT("不改全局默认绑定"), Defaults->DebugExecBindings.Num(), DefaultCount);
        TestTrue(TEXT("全局F5仍保留"), Defaults->DebugExecBindings.ContainsByPredicate(IsF5));
        TestTrue(TEXT("F5响应服务器操作"), Mode->RequestRestart(PC));
        TestTrue(TEXT("告知停止重开以读取磁盘"), PC->GetObserver().Notice.Contains(TEXT("Ctrl+C"))); return true;
    }
    if (Case == TEXT("AuthoritativeSlots"))
    {
        const auto& O = PC->GetObserver();
        TestTrue(TEXT("观测已就绪"), O.bReady);
        if (!TestEqual(TEXT("四槽包含空槽"), O.Slots.Num(), 4)) { return false; }
        TestEqual(TEXT("第一个实际技能"), O.Slots[0].Row, FName(TEXT("BasicAttack")));
        TestTrue(TEXT("主动分类"), O.Slots[0].bActive);
        TestTrue(TEXT("实际被动GE已生效"), O.Slots[2].bEffectApplied);
        TestTrue(TEXT("第四槽为空"), O.Slots[3].Row.IsNone());
        TestEqual(TEXT("预期奖励读取表"), O.RewardGold, 10); return true;
    }
    F.World->GetWorldSettings()->NotifyBeginPlay();
    F.World->BeginPlay();
    TestTrue(TEXT("Enter经过测试Mode"), Mode->RequestAdvance(PC)); F.Tick(6);
    auto* Monster = Mode->GetWaveMonsters()[0].Get();
    if (Case == TEXT("CooldownDeadline"))
    {
        Mode->RefreshTarget(F.Player); TestTrue(TEXT("真实GA激活"), F.Player->TryActivateSkillSlot(0));
        PC->CaptureObserver(Mode);
        const auto& O = PC->GetObserver();
        if (!TestEqual(TEXT("四槽"), O.Slots.Num(), 4)) { return false; }
        TestTrue(TEXT("CD必须是服务器未来截止时间"), O.Slots[0].Deadline > F.World->GetTimeSeconds() + 0.6);
        TestTrue(TEXT("选择真实怪物"), O.Target != nullptr);
        TestEqual(TEXT("观测攻击范围来自表"), O.TargetRange, 140.f); return true;
    }
    const FVector Before = Monster->GetActorLocation(); F.Tick(0.3f);
    const double Travel = FVector::Dist2D(Before, Monster->GetActorLocation());
    TestTrue(TEXT("持续移动不是停在无controller"), Travel > 1);
    TestTrue(TEXT("移动遵守表速度非瞬移"), Travel <= 40);
    TestTrue(TEXT("怪物没穿地"), Monster->GetActorLocation().Z > 80);
    F.Tick(4);
    TestTrue(TEXT("真实怪物技能伤害"), Y01Playground::Health(F.Player->GetCombatASC()) < 200);
    TestTrue(TEXT("玩家没穿地"), F.Pawn->GetActorLocation().Z > 80);
    return true;
}
namespace
{
struct FSpawnBatchWorld
{
    TStrongObjectPtr<UY01PlaygroundProfile> Seed{Y01PlaygroundEditor::CreateSeed(GetTransientPackage())};
    TStrongObjectPtr<UY01R36Profile> Profile{NewObject<UY01R36Profile>()};
    UWorld* World;
    AY01R36GameMode* Mode;
    AY01PlaygroundController* PC;
    AStaticMeshActor* Floor;
    FSpawnBatchWorld()
    {
        Profile->Tables = Seed->Tables; Profile->Character = Seed->Character; Profile->bTrainingEnabled = true;
        Profile->bR34AutoAttackEnabled = true;
        auto* Character = Profile->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("SpawnTest"));
        Character->InitialEquipment.Reset(); Character->InitialSkills.Reset(); Character->Health = 20000;
        Profile->Tables[EY01TableKind::Wave]->EmptyTable();
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01R36GameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01R36GameMode>();
        World->SpawnActor<AY01R36Config>()->Content = Profile.Get(); PC = AddPlayer();
        Floor = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, -50), FRotator::ZeroRotator);
        Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
        Floor->SetActorScale3D(FVector(100, 100, 1)); Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
    }
    ~FSpawnBatchWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    AY01PlaygroundController* AddPlayer()
    {
        auto* Controller = World->SpawnActor<AY01PlaygroundController>(); auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01PlaygroundCharacter>(FVector(-500, 0, 90), FRotator::ZeroRotator);
        Controller->PlayerState = PS; Controller->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn);
        Mode->GetFlowState()->AddPlayerState(PS); return Controller;
    }
    static FY01DataRef Ref(EY01TableKind Kind, FName Name = TEXT("Default")) { FY01DataRef R; R.Type = Kind; R.RowName = Name; return R; }
    static FY01SpawnBatch Batch(FName Id, double Offset, FName Tag, int32 Count = 1, FName Monster = TEXT("Default"))
    {
        FY01SpawnBatch B; B.BatchId = Id; B.OffsetSeconds = Offset; B.PointTag = Tag;
        B.PointKind = Tag == TEXT("Boss") ? EY01SpawnPointKind::Boss : EY01SpawnPointKind::Normal;
        FY01BatchMonster M; M.Monster = Ref(EY01TableKind::Monster, Monster); M.Count = Count; B.Monsters.Add(M); return B;
    }
    void Wave(FName Name, double Prepare, TArray<FY01SpawnBatch> Batches)
    {
        FY01WaveRow W; W.PreparationSeconds = Prepare; W.Schedule = EY01WaveSchedule::Batches; W.Batches = MoveTemp(Batches);
        W.Reward = Ref(EY01TableKind::RewardPackage); Profile->Tables[EY01TableKind::Wave]->AddRow(Name, W);
        Profile->OrderedWaves.Add(Ref(EY01TableKind::Wave, Name));
    }
    AY01SpawnPoint* Point(FName Tag, FVector Location = FVector(600, 0, 50))
    {
        auto* P = World->SpawnActor<AY01SpawnPoint>(Location, FRotator::ZeroRotator);
        P->Tags = {Tag}; P->PointKind = Tag == TEXT("Boss") ? EY01SpawnPointKind::Boss : EY01SpawnPointKind::Normal; return P;
    }
    void Start() { Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay(); }
    void Tick(double Seconds) { for (int32 I = 0; I < FMath::CeilToInt(Seconds / .025); ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .025f); } }
    const FY01WaveSnapshot& S() const { return Mode->GetFlowState()->GetWaveSnapshot(); }
    FY01MonsterRow* Monster(FName Name = TEXT("Default")) { return Profile->Tables[EY01TableKind::Monster]->FindRow<FY01MonsterRow>(Name, TEXT("SpawnTest")); }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01SpawnNextTest, "Y01.Framework.R36.SpawnNext",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01SpawnNextTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Schema"), TEXT("PointValidation"), TEXT("CommonOriginMixedFixedCount"), TEXT("CrossWaveLateBatch"),
        TEXT("SpawnFailureEnds"), TEXT("DetectionXY"), TEXT("MainlineDetection"), TEXT("TrainingDetection")}) { Names.Add(N); Commands.Add(N); }
}
bool FY01SpawnNextTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> FrameGuard(GFrameCounter, GFrameCounter);
    FSpawnBatchWorld F;
    F.Wave(TEXT("First"), .1, {F.Batch(TEXT("Early"), .2, TEXT("1"), 2), F.Batch(TEXT("Late"), .6, TEXT("1"))});
    F.Point(TEXT("1"));
    if (Case == TEXT("Schema"))
    {
        auto* W = F.Profile->Tables[EY01TableKind::Wave]->FindRow<FY01WaveRow>(TEXT("First"), TEXT("SpawnTest"));
        TestTrue(TEXT("合法批次Profile预检"), F.Profile->Validate().IsValid());
        FY01SpawnEntry E; E.Monster = F.Ref(EY01TableKind::Monster); W->Spawns.Add(E);
        TestFalse(TEXT("拒绝混填"), F.Profile->Validate().IsValid()); W->Spawns.Reset();
        W->Batches[1].BatchId = W->Batches[0].BatchId; TestFalse(TEXT("批次ID唯一"), F.Profile->Validate().IsValid()); W->Batches[1].BatchId = TEXT("Late");
        W->Batches[0].PointTag = TEXT("01"); TestFalse(TEXT("普通点不接受非规范数字"), F.Profile->Validate().IsValid()); W->Batches[0].PointTag = TEXT("1");
        W->Batches[0].Monsters[0].Count = 0; TestFalse(TEXT("Count正整数"), F.Profile->Validate().IsValid()); W->Batches[0].Monsters[0].Count = 2;
        W->Batches[0].Monsters[0].Monster.RowName = TEXT("Missing"); TestFalse(TEXT("怪物严格引用"), F.Profile->Validate().IsValid()); W->Batches[0].Monsters[0].Monster.RowName = TEXT("Default");
        for (float V : {-1.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
        { F.Monster()->DetectionRadius = V; TestFalse(TEXT("发现半径有限非负"), F.Profile->Validate().IsValid()); }
        F.Monster()->DetectionRadius = 0; TestTrue(TEXT("零合法"), F.Profile->Validate().IsValid()); return true;
    }
    if (Case == TEXT("PointValidation"))
    {
        TMap<FName, TWeakObjectPtr<AY01SpawnPoint>> Points; FString Error;
        TestTrue(TEXT("仅产品点注册"), AY01SpawnPoint::BuildRegistry(F.World, Points, Error));
        auto* Duplicate = F.Point(TEXT("1")); TestFalse(TEXT("重复明确拒绝"), AY01SpawnPoint::BuildRegistry(F.World, Points, Error));
        TestTrue(TEXT("重复含对象名"), Error.Contains(Duplicate->GetName())); Duplicate->Tags = {TEXT("0")};
        TestFalse(TEXT("非法点名拒绝"), AY01SpawnPoint::BuildRegistry(F.World, Points, Error));
        Duplicate->Tags = {TEXT("Boss")}; TestFalse(TEXT("类型不匹配拒绝"), AY01SpawnPoint::BuildRegistry(F.World, Points, Error));
        Duplicate->Destroy();
        auto* W = F.Profile->Tables[EY01TableKind::Wave]->FindRow<FY01WaveRow>(TEXT("First"), TEXT("SpawnTest")); W->Batches[0].PointTag = TEXT("9");
        AddExpectedError(TEXT("MissingConfiguration"), EAutomationExpectedErrorFlags::Contains, 1); F.Start();
        TestFalse(TEXT("地图缺点禁止启动"), F.Mode->IsMatchConfigured());
        TestTrue(TEXT("错误报告波和批次"), F.Mode->GetConfigurationError().Contains(TEXT("First")) && F.Mode->GetConfigurationError().Contains(TEXT("Early"))); return true;
    }
    if (Case == TEXT("CommonOriginMixedFixedCount"))
    {
        F.AddPlayer(); auto* W = F.Profile->Tables[EY01TableKind::Wave]->FindRow<FY01WaveRow>(TEXT("First"), TEXT("SpawnTest"));
        FY01BatchMonster Boss; Boss.Monster = F.Ref(EY01TableKind::Monster, TEXT("Boss")); Boss.Count = 1; W->Batches[0].Monsters.Add(Boss);
        W->Batches.Add(F.Batch(TEXT("SameTime"), .2, TEXT("Boss"), 1, TEXT("Boss"))); F.Point(TEXT("Boss"), FVector(-1200, 0, 50));
        F.Monster()->DetectionRadius = 0; F.Monster(TEXT("Boss"))->DetectionRadius = 0; F.Start(); F.Tick(.375);
        TestEqual(TEXT("两玩家不乘人数；同刻混阵"), F.S().TotalSpawnedMonsters, 4);
        const auto& M = F.Mode->GetWaveMonsters(); if (!TestEqual(TEXT("真实对象数"), M.Num(), 4)) { return false; }
        TestEqual(TEXT("同刻配置顺序"), M[2]->GetCombatASC()->GetDefinition().RowName, FName(TEXT("Boss")));
        TestNull(TEXT("主线零半径不发现"), M[0]->GetCombatASC()->GetCombatTarget());
        TestTrue(TEXT("Boss点坐标"), M[3]->GetActorLocation().X < -1000);
        TestTrue(TEXT("胶囊不重叠"), FVector::Dist2D(M[0]->GetActorLocation(), M[1]->GetActorLocation()) >= 68);
        F.Tick(.425); TestEqual(TEXT("晚批以共同起点非累计offset"), F.S().TotalSpawnedMonsters, 5); return true;
    }
    if (Case == TEXT("CrossWaveLateBatch"))
    {
        F.Wave(TEXT("Second"), .3, {F.Batch(TEXT("Now"), 0, TEXT("1"))}); F.Monster()->DetectionRadius = 0;
        F.Start(); F.Tick(.55); TestEqual(TEXT("下一波按时开始"), F.S().WaveIndex, 1);
        TestEqual(TEXT("旧波仍pending"), F.S().WaveStates[0].PendingSpawns, 1);
        for (AY01Monster* M : F.Mode->GetWaveMonsters()) { M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        F.Tick(.05); TestFalse(TEXT("待生成不清波"), F.S().WaveStates[0].bRewarded);
        F.Tick(.3); TestEqual(TEXT("旧波晚批仍生成"), F.S().WaveStates[0].SpawnedMonsters, 3);
        TestEqual(TEXT("第二波独立计数"), F.S().WaveStates[1].SpawnedMonsters, 1);
        TestTrue(TEXT("真实结束"), F.Mode->TryEndMatch(false)); F.Tick(.3); TestEqual(TEXT("Ended取消pending"), F.S().TotalPendingSpawns, 0); return true;
    }
    if (Case == TEXT("SpawnFailureEnds"))
    {
        F.Floor->Destroy(); AddExpectedError(TEXT("SpawnFailure"), EAutomationExpectedErrorFlags::Contains, 2);
        F.Start(); F.Tick(.5); TestEqual(TEXT("无地面不是静默漏怪"), F.S().Result, EY01MatchResult::Failure);
        TestEqual(TEXT("进入Ended链"), F.S().Phase, EY01MatchPhase::Ended);
        TestFalse(TEXT("错误可观测"), F.Mode->GetConfigurationError().IsEmpty()); TestFalse(TEXT("不得成功领奖"), F.Mode->TryCompleteWave());
        FSpawnBatchWorld BadInit; BadInit.Wave(TEXT("InitFailure"), .1, {BadInit.Batch(TEXT("Init"), .2, TEXT("1"))}); BadInit.Point(TEXT("1"));
        BadInit.Start(); BadInit.Monster()->DetectionRadius = -1;
        BadInit.Tick(.5);
        TestEqual(TEXT("运行时初始化失败也必须终止"), BadInit.S().Result, EY01MatchResult::Failure); return true;
    }
    if (Case == TEXT("DetectionXY"))
    {
        F.Monster()->DetectionRadius = 100; F.Monster()->Range = 10; F.Start();
        FString Error; auto* M = F.World->SpawnActor<AY01Monster>(FVector(0, 0, 90), FRotator::ZeroRotator);
        TestTrue(TEXT("初始化"), M->TryInitializeMonster(F.Profile->BuildCatalog(), F.Ref(EY01TableKind::Monster), Error));
        APawn* A = F.PC->GetPawn(); A->SetActorLocation(FVector(100, 0, 500));
        auto* BPC = F.AddPlayer(); APawn* B = BPC->GetPawn(); B->SetActorLocation(FVector(-100, 0, 90));
        TestTrue(TEXT("第二玩家初始化"), BPC->GetPlayerState<AY01PlayerState>()->TryInitializeCharacter(F.Profile->BuildCatalog(), F.Profile->Character, Error));
        TArray<AActor*> Candidates{B, A}; auto* First = Y01Targeting::NearestXY(M->GetCombatASC(), Candidates, 100);
        Candidates = {A, B}; TestEqual(TEXT("同距与数组顺序无关"), Y01Targeting::NearestXY(M->GetCombatASC(), Candidates, 100), First);
        TestNotNull(TEXT("XY闭圆忽略高度"), First); TestNull(TEXT("零不发现"), Y01Targeting::NearestXY(M->GetCombatASC(), Candidates, 0));
        TestTrue(TEXT("发现目标允许在攻击圈外"), M->GetCombatASC()->SetDiscoveredTarget(A));
        TestFalse(TEXT("普攻范围独立"), M->GetCombatASC()->TryAutoAttack(A, [](float) { return true; })); return true;
    }
    if (Case == TEXT("MainlineDetection"))
    {
        F.Monster()->DetectionRadius = 1400; F.Monster()->Range = 50; F.Start(); F.Tick(.4);
        if (!TestTrue(TEXT("主线怪已生成"), !F.Mode->GetWaveMonsters().IsEmpty())) { return false; }
        auto* M = F.Mode->GetWaveMonsters()[0].Get(); TestEqual(TEXT("自身行半径"), M->GetDetectionRadius(), 1400.f);
        TestEqual(TEXT("攻击圈外仍发现并追击"), M->GetCombatASC()->GetCombatTarget(), static_cast<AActor*>(F.PC->GetPawn()));
        F.PC->GetPawn()->SetActorLocation(M->GetActorLocation() + FVector(-1500, 0, 0)); F.Tick(.025);
        TestNull(TEXT("越界立即失效无需等刷新"), M->GetCombatASC()->GetCombatTarget());
        TestTrue(TEXT("停止追击"), M->GetCharacterMovement()->Velocity.IsNearlyZero());
        F.PC->GetPawn()->SetActorLocation(M->GetActorLocation() + FVector(-1000, 0, 0)); F.Tick(.125);
        TestEqual(TEXT("重新进入发现圈后重搜"), M->GetCombatASC()->GetCombatTarget(), static_cast<AActor*>(F.PC->GetPawn())); return true;
    }
    F.Monster()->DetectionRadius = 0; F.Monster(TEXT("Boss"))->DetectionRadius = 500; F.Monster(TEXT("Boss"))->Range = 20;
    auto* Training = F.Profile->Tables[EY01TableKind::TrainingStage].Get(); Training->EmptyTable(); FY01TrainingStageRow Stage;
    FY01SpawnEntry E; E.Monster = F.Ref(EY01TableKind::Monster); Stage.Spawns.Add(E); E.Monster.RowName = TEXT("Boss"); Stage.Spawns.Add(E);
    Stage.Reward = F.Ref(EY01TableKind::RewardPackage); Training->AddRow(TEXT("Default"), Stage);
    F.Start(); auto* PS = F.PC->GetPlayerState<AY01PlayerState>(); TestTrue(TEXT("进入私房"), F.Mode->GetTraining()->Request(F.PC, true));
    F.Mode->GetTraining()->Update(); const auto M = F.Mode->GetTraining()->Monsters(PS);
    if (!TestEqual(TEXT("既有训练补位生成"), M.Num(), 2)) { return false; }
    TestNull(TEXT("训练零半径不锁房主"), M[0]->GetCombatASC()->GetCombatTarget());
    TestEqual(TEXT("训练Boss自身半径发现"), M[1]->GetCombatASC()->GetCombatTarget(), static_cast<AActor*>(F.PC->GetPawn()));
    TestFalse(TEXT("训练攻击距离独立"), M[1]->GetCombatASC()->IsAutoAttackInRange(F.PC->GetPawn()));
    F.PC->GetPawn()->SetActorLocation(M[1]->GetActorLocation() + FVector(501, 0, 0)); F.Mode->GetTraining()->Update();
    TestNull(TEXT("训练越界立即取消"), M[1]->GetCombatASC()->GetCombatTarget()); return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01MonsterMovementPolicyTest, "Y01.Framework.R36.MonsterMovement",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01MonsterMovementPolicyTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("PeerIgnoreScopeCleanup"), TEXT("SeparationDeterministicBounded"),
        TEXT("AttackDeathStop"), TEXT("VisualMeshSmoothing")}) { Names.Add(N); Commands.Add(N); }
}
bool FY01MonsterMovementPolicyTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> FrameGuard(GFrameCounter, GFrameCounter);
    FSpawnBatchWorld F;
    F.Wave(TEXT("Movement"), .1, {F.Batch(TEXT("Normals"), 0, TEXT("1"), 2),
        F.Batch(TEXT("Boss"), 0, TEXT("Boss"), 1, TEXT("Boss"))});
    F.Point(TEXT("1")); F.Point(TEXT("Boss"), FVector(900, 0, 50));
    F.Start(); F.Tick(.2);
    TArray<AY01Monster*> Normals; AY01Monster* Boss = nullptr;
    for (AY01Monster* M : F.Mode->GetWaveMonsters())
    {
        if (M->GetCombatASC()->GetDefinition().RowName == TEXT("Boss")) { Boss = M; }
        else { Normals.Add(M); }
    }
    if (!TestEqual(TEXT("两只普通怪"), Normals.Num(), 2) || !TestNotNull(TEXT("一只Boss"), Boss)) { return false; }
    auto* A = Normals[0]; auto* B = Normals[1];
    TArray<AY01Monster*> Peers{A, B, Boss};
    if (Case == TEXT("PeerIgnoreScopeCleanup"))
    {
        A->RefreshMovementPeers(Peers); B->RefreshMovementPeers(Peers); Boss->RefreshMovementPeers(Peers);
        const auto& AIgnores = A->GetCapsuleComponent()->GetMoveIgnoreActors();
        const auto& BIgnores = B->GetCapsuleComponent()->GetMoveIgnoreActors();
        TestTrue(TEXT("同区普通怪双向忽略移动碰撞"), AIgnores.Contains(B) && BIgnores.Contains(A));
        TestFalse(TEXT("普通怪不忽略Boss"), AIgnores.Contains(Boss));
        TestFalse(TEXT("Boss不忽略普通怪"), Boss->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(A));
        TestFalse(TEXT("普通怪不忽略玩家"), AIgnores.Contains(F.PC->GetPawn()));
        TestFalse(TEXT("普通怪不忽略墙地"), AIgnores.Contains(F.Floor));

        FString Error;
        auto SpawnRoomMonster = [&](int32 Session)
        {
            auto* M = F.World->SpawnActor<AY01Monster>(FVector(300, 300, 90), FRotator::ZeroRotator);
            TestTrue(TEXT("区域怪初始化"), M->TryInitializeMonster(F.Profile->BuildCatalog(), F.Ref(EY01TableKind::Monster), Error));
            TestTrue(TEXT("区域怪登记"), M->RegisterHealthBarPresentation(7, Session)); return M;
        };
        auto* RoomA = SpawnRoomMonster(1); auto* RoomB = SpawnRoomMonster(1);
        RoomA->RefreshMovementPeers({RoomB}); RoomB->RefreshMovementPeers({RoomA});
        TestTrue(TEXT("同区域同会话普通怪双向忽略"), RoomA->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(RoomB)
            && RoomB->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(RoomA));
        RoomB->UnregisterHealthBarPresentation(); TestTrue(TEXT("切换会话登记"), RoomB->RegisterHealthBarPresentation(7, 2));
        RoomA->RefreshMovementPeers({RoomB}); RoomB->RefreshMovementPeers({RoomA});
        TestFalse(TEXT("不同会话清除双方忽略"), RoomA->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(RoomB)
            || RoomB->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(RoomA));
        RoomB->Destroy(); RoomA->RefreshMovementPeers({RoomB});
        TestFalse(TEXT("销毁后无陈旧忽略"), RoomA->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(RoomB));
        RoomA->UnregisterHealthBarPresentation(); RoomA->RefreshMovementPeers({A});
        TestFalse(TEXT("区域注销清除忽略"), RoomA->GetCapsuleComponent()->GetMoveIgnoreActors().Contains(A));
        return true;
    }
    if (Case == TEXT("SeparationDeterministicBounded"))
    {
        const FVector Desired(80, 0, 0);
        TestTrue(TEXT("无邻居不改期望速度"), A->ResolveMovementVelocity({}, Desired).Equals(Desired));
        A->SetActorLocation(FVector(0, 0, 90)); B->SetActorLocation(A->GetActorLocation());
        const FVector VA = A->ResolveMovementVelocity({B}, Desired);
        const FVector VB = B->ResolveMovementVelocity({A}, Desired);
        TestTrue(TEXT("完全重合仍确定性向相反方向分开"), FVector::DotProduct(VA - Desired, VB - Desired) < 0);
        TestTrue(TEXT("分离后A不超表速度"), VA.Size2D() <= A->GetCharacterMovement()->MaxWalkSpeed + KINDA_SMALL_NUMBER);
        TestTrue(TEXT("分离后B不超表速度"), VB.Size2D() <= B->GetCharacterMovement()->MaxWalkSpeed + KINDA_SMALL_NUMBER);
        return true;
    }
    if (Case == TEXT("AttackDeathStop"))
    {
        F.PC->GetPawn()->SetActorLocation(A->GetActorLocation() + FVector(10, 0, 0)); F.Tick(.05);
        TestTrue(TEXT("攻击范围内停止移动"), A->GetCharacterMovement()->Velocity.IsNearlyZero());
        A->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); F.Tick(.05);
        TestTrue(TEXT("死亡停止移动"), A->GetCharacterMovement()->Velocity.IsNearlyZero());
        TestEqual(TEXT("死亡禁用移动模式"), A->GetCharacterMovement()->MovementMode, MOVE_None); return true;
    }
    AY01PlaygroundVisual* Visual = nullptr;
    for (TActorIterator<AY01PlaygroundVisual> It(F.World); It; ++It) { if (It->Target == A) { Visual = *It; break; } }
    if (!TestNotNull(TEXT("怪物Visual存在"), Visual)) { return false; }
    Visual->Tick(1.f / 60.f);
    const FVector VisualBefore = Visual->GetActorLocation();
    const FVector MeshBefore = A->GetMesh()->GetComponentLocation();
    const FVector PreservedOffset = VisualBefore - MeshBefore;
    A->SetActorLocation(A->GetActorLocation() + FVector(600, 0, 0));
    A->GetMesh()->SetWorldLocation(MeshBefore + FVector(120, 0, 0)); Visual->Tick(1.f / 60.f);
    const FVector Expected = A->GetMesh()->GetComponentLocation() + PreservedOffset;
    TestTrue(TEXT("Visual跟随Character Mesh平滑位置并补回模型偏移"), Visual->GetActorLocation().Equals(Expected));
    TestTrue(TEXT("客户端可见外观不瞬移到权威胶囊"), Visual->GetActorLocation().X < A->GetActorLocation().X);
    return true;
}
#endif
