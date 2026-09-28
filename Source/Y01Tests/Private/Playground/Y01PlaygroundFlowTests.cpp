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
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "GameFramework/WorldSettings.h"
#include "EnhancedPlayerInput.h"

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
        TEXT("NoPreparationTarget"), TEXT("TestOnlyClasses") })
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
    TestTrue(TEXT("Enter开始波次"), F.Mode->TryStartNextWave());
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
#endif
