#include "Misc/AutomationTest.h"
#include "Playground/Y01R36Profile.h"
#include "Y01MatchFixture.h"
#include "UObject/UnrealType.h"
#include "GameFramework/Pawn.h"
#include <limits>
#if __has_include("Combat/Y01Core.h")
#include "Combat/Y01Core.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Components/CapsuleComponent.h"
#include "Components/WidgetComponent.h"
#include "Playground/Y01R36Runtime.h"
#include "Playground/Y01R36UI.h"
#include "Playground/Y01TestSkill.h"
#include "Player/Y01PlayerState.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextBlock.h"
#include "Blueprint/WidgetTree.h"
#include "GameFramework/WorldSettings.h"
#include "Components/CanvasPanelSlot.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#define Y01_CORE_AVAILABLE 1
#endif

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01CoreConfigurationTest, "Y01.Core.Step1.Configuration",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01CoreConfigurationTest::RunTest(const FString&)
{
    using K = EY01TableKind;
    FY01MatchFixture F;
    auto* P = NewObject<UY01R36Profile>();
    auto& Character = F.Row<FY01CharacterRow>(K::Character);
    Character.InitialEquipment.Reset(); Character.InitialSkills.Reset();
    P->Character = F.Ref(K::Character); P->OrderedWaves = F.Waves();
    for (const auto& Pair : F.Tables) { P->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
    auto* Enabled = FindFProperty<FBoolProperty>(P->GetClass(), TEXT("bCoreDefenseEnabled"));
    auto* Rules = FindFProperty<FStructProperty>(P->GetClass(), TEXT("CoreRules"));
    auto* Core = FindFProperty<FStructProperty>(FY01MatchRulesRow::StaticStruct(), TEXT("Core"));
    if (!TestTrue(TEXT("正式Core配置和显式启用开关必须存在"), Enabled && Rules && Core)) { return false; }
    TestFalse(TEXT("旧Profile默认关闭"), Enabled->GetPropertyValue_InContainer(P));
    TestTrue(TEXT("默认19表仍合法"), P->Validate().IsValid());
    auto* Ref = Rules->ContainerPtrToValuePtr<FY01DataRef>(P);
    TestTrue(TEXT("默认引用MatchRules.Default"), Ref->Type == K::MatchRules && Ref->RowName == TEXT("Default"));
    Enabled->SetPropertyValue_InContainer(P, true);
    TestTrue(TEXT("完整启用配置合法"), P->Validate().IsValid());
    Ref->RowName = TEXT("Missing"); TestFalse(TEXT("启用不能忽略缺失Rules"), P->Validate().IsValid());
    Enabled->SetPropertyValue_InContainer(P, false); TestTrue(TEXT("关闭忽略未使用引用"), P->Validate().IsValid());
    *Ref = F.Ref(K::Character); Enabled->SetPropertyValue_InContainer(P, true);
    TestFalse(TEXT("拒绝跨表伪装Rules"), P->Validate().IsValid()); *Ref = F.Ref(K::MatchRules);
    void* Settings = Core->ContainerPtrToValuePtr<void>(&F.Row<FY01MatchRulesRow>(K::MatchRules));
    const TCHAR* Names[] = {TEXT("MaxHealth"), TEXT("HealthRegen"), TEXT("HitAlertInterval"), TEXT("CriticalHealthRatio")};
    const float Defaults[] = {1000, 5, 3, .3f};
    for (int32 I = 0; I < 4; ++I)
    {
        auto* Field = FindFProperty<FFloatProperty>(Core->Struct, Names[I]);
        if (!TestNotNull(Names[I], Field)) { continue; }
        TestEqual(TEXT("兼容默认值"), Field->GetPropertyValue_InContainer(Settings), Defaults[I]);
        Field->SetPropertyValue_InContainer(Settings, -1);
        TestFalse(TEXT("目录和Profile拒绝负配置"), P->Validate().IsValid());
        Field->SetPropertyValue_InContainer(Settings, Defaults[I]);
    }
    TestNull(TEXT("不造InitialHP"), FindFProperty<FProperty>(Core->Struct, TEXT("InitialHP")));
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01CoreRespawnConfigTest, "Y01.Core.Step1.RespawnConfiguration",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01CoreRespawnConfigTest::RunTest(const FString&)
{
    FY01MatchFixture F;
    auto* Field = FindFProperty<FFloatProperty>(FY01WaveRow::StaticStruct(), TEXT("RespawnSeconds"));
    if (!TestNotNull(TEXT("波次复活等待正式字段"), Field)) { return false; }
    auto& Row = F.Row<FY01WaveRow>(EY01TableKind::Wave);
    TestEqual(TEXT("兼容默认10秒"), Field->GetPropertyValue_InContainer(&Row), 10.f);
    Field->SetPropertyValue_InContainer(&Row, 0);
    TestFalse(TEXT("零等待非法"), FY01DataCatalog(F.Tables).Validate().IsValid());
    Field->SetPropertyValue_InContainer(&Row, std::numeric_limits<float>::infinity());
    TestFalse(TEXT("无限等待非法"), FY01DataCatalog(F.Tables).Validate().IsValid());
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01CorePawnContractTest, "Y01.Core.Step1.PawnContract",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01CorePawnContractTest::RunTest(const FString&)
{
    auto* Class = FindObject<UClass>(nullptr, TEXT("/Script/Y01.Y01Core"));
    if (!TestNotNull(TEXT("正式核心类存在"), Class)) { return false; }
    TestTrue(TEXT("核心是独立Pawn，不复用Monster"), Class->GetSuperClass() == APawn::StaticClass());
    TestTrue(TEXT("核心全队常相关复制"), Class->GetDefaultObject<APawn>()->GetIsReplicated() && Class->GetDefaultObject<APawn>()->bAlwaysRelevant);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01CoreLifeTest, "Y01.Core.Step1.LifeAndPresentation",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01CoreLifeTest::RunTest(const FString&)
{
#ifdef Y01_CORE_AVAILABLE
    FY01MatchFixture F; FString Error;
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL());
    auto* Core = World->SpawnActor<AY01Core>();
    auto* Monster = World->SpawnActor<AY01Monster>(FVector(200,0,0), FRotator::ZeroRotator);
    TestTrue(TEXT("核心独立初始化"), Core->TryInitializeCore(FY01DataCatalog(F.Tables), F.Ref(EY01TableKind::MatchRules), Error));
    TestTrue(TEXT("攻击者初始化"), Monster->TryInitializeMonster(FY01DataCatalog(F.Tables), F.Ref(EY01TableKind::Monster), Error));
    auto* ASC = Core->GetCombatASC(); auto* Source = Monster->GetCombatASC();
    Core->DispatchBeginPlay();
    TestEqual(TEXT("核心满血且不取怪物HP"), Core->GetSnapshot().Health, 1000.f);
    TestTrue(TEXT("无攻击授予"), ASC->GetGrantedSources().IsEmpty());
    TestFalse(TEXT("未注册默认拒绝"), Source->IsLegalCombatTarget(Core));
    ASC->TargetPermission = [Source, ASC](const auto* S, const auto* T) { return S == Source && T == ASC; };
    int32 Fallen = 0; Core->OnFallen.AddLambda([&] { ++Fallen; });
    auto Hit = [&](float Damage)
    {
        auto Spec = Source->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, Source->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, Damage);
        Source->ApplyGameplayEffectSpecToTarget(*Spec.Data, ASC);
    };
    ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 900);
    TestEqual(TEXT("属性调整不是受击"), Core->GetSnapshot().HitSequence, uint64(0));
    Hit(25); Hit(25);
    TestEqual(TEXT("真实伤害不受提示CD限制"), Core->GetSnapshot().Health, 850.f);
    TestEqual(TEXT("两次真实受击"), Core->GetSnapshot().HitSequence, uint64(2));
    TestEqual(TEXT("全队警报只发布一次"), Core->GetSnapshot().AlertSequence, uint64(1));
    TestTrue(TEXT("服务器样本闪红"), Core->GetSnapshot().IsHitFlash());
    TestFalse(TEXT("跨波不能重置核心"), Core->TryInitializeCore(FY01DataCatalog(F.Tables), F.Ref(EY01TableKind::MatchRules), Error));
    for (int32 I = 0; I < 40; ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .025f); }
    TestTrue(TEXT("受击后持续回复"), Core->GetSnapshot().Health > 853 && Core->GetSnapshot().Health <= 856);
    TestFalse(TEXT("闪红按服务器样本结束"), Core->GetSnapshot().IsHitFlash());
    ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 290);
    TestTrue(TEXT("低于比例持续危急"), Core->GetSnapshot().IsCritical());
    ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 999);
    for (int32 I = 0; I < 40; ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .025f); }
    TestEqual(TEXT("回复上限"), Core->GetSnapshot().Health, 1000.f);
    TestFalse(TEXT("回复解除危急"), Core->GetSnapshot().IsCritical());
    auto* Capsule = Core->FindComponentByClass<UCapsuleComponent>();
    TestTrue(TEXT("查询开启且不阻挡Pawn移动"), Capsule && Capsule->IsQueryCollisionEnabled() && Capsule->GetCollisionResponseToChannel(ECC_Pawn) != ECR_Block);
    TArray<FHitResult> Hits; FCollisionQueryParams Params; Params.AddIgnoredActor(Monster);
    World->SweepMultiByObjectType(Hits, FVector(-200,0,0), FVector(200,0,0), FQuat::Identity,
        FCollisionObjectQueryParams(ECC_Pawn), FCollisionShape::MakeSphere(12), Params);
    TestTrue(TEXT("投射物对象查询可命中核心"), Hits.ContainsByPredicate([Core](const auto& H) { return H.GetActor() == Core; }));
    TestNotNull(TEXT("独立世界血条组件"), Core->FindComponentByClass<UWidgetComponent>());
    Hit(2000); Hit(25);
    TestEqual(TEXT("只失守一次"), Fallen, 1);
    TestTrue(TEXT("归零锁存"), Core->GetSnapshot().bFallen);
    ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 1000);
    for (int32 I = 0; I < 40; ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .025f); }
    TestEqual(TEXT("失守后任何回血都不能复活"), Core->GetSnapshot().Health, 0.f);
    TestEqual(TEXT("重复执行不多发失守"), Fallen, 1);
    World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    return true;
#else
    AddError(TEXT("真实核心生命、受击和场景表现接口尚未实现")); return false;
#endif
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01CoreRouteTest, "Y01.Core.Step1.MainlineRoute",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01CoreRouteTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("ZeroDetection"), TEXT("PlayerPriority"), TEXT("Projectile"), TEXT("Missing"), TEXT("Duplicate"), TEXT("LateDuplicate"), TEXT("Removed")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01CoreRouteTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    return [this, &Case]<typename T>()
    {
        if constexpr (!requires(T* M) { M->GetMatchCore(); M->IsCoreDefenseEnabled(); M->IsGameplayFrozen(); })
        { AddError(TEXT("主线核心登记、进军和冻结接口尚未实现")); return false; }
        else
        {
            FY01MatchFixture F;
            auto* Profile = NewObject<UY01R36Profile>(); Profile->bCoreDefenseEnabled = true; Profile->bTrainingEnabled = true;
            Profile->Character = F.Ref(EY01TableKind::Character); Profile->OrderedWaves = {F.Ref(EY01TableKind::Wave)};
            auto& C = F.Row<FY01CharacterRow>(EY01TableKind::Character); C.InitialEquipment.Reset(); C.InitialSkills.Reset(); C.AttackDamage = 0;
            auto& M = F.Row<FY01MonsterRow>(EY01TableKind::Monster); M.DetectionRadius = Case == TEXT("PlayerPriority") ? 200 : 0; M.Range = 100; M.MoveSpeed = 300;
            if (Case == TEXT("Projectile")) { M.AttackType = EY01AttackType::Ranged; M.ProjectileSpeed = 500; }
            auto& W = F.Row<FY01WaveRow>(EY01TableKind::Wave); W.PreparationSeconds = .1f; W.Spawns[0].Count = 1; W.Spawns[0].DelaySeconds = 0;
            auto Late = W.Spawns[0]; Late.DelaySeconds = 20; W.Spawns.Add(Late);
            for (const auto& P : F.Tables) { Profile->Tables.Add(P.Key, const_cast<UDataTable*>(P.Value)); }
            const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
                .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
            auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
            GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
            World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
            World->GetWorldSettings()->DefaultGameMode = T::StaticClass(); World->SetGameMode(FURL()); auto* Mode = World->GetAuthGameMode<T>();
            World->SpawnActor<AY01R36Config>()->Content = Profile;
            auto* Core = Case == TEXT("Missing") ? nullptr : World->SpawnActor<AY01Core>(FVector(0,0,90), FRotator::ZeroRotator);
            if (Case == TEXT("Duplicate")) { World->SpawnActor<AY01Core>(FVector(1000,0,90), FRotator::ZeroRotator); }
            if (Case == TEXT("Missing") || Case == TEXT("Duplicate"))
            { AddExpectedError(TEXT("CoreDefense需要唯一地图核心"), EAutomationExpectedErrorFlags::Contains, 1); }
            auto* PC = World->SpawnActor<AY01PlaygroundController>(); auto* PS = World->SpawnActor<AY01PlayerState>();
            auto* Pawn = World->SpawnActor<AY01PlaygroundCharacter>(FVector(-500,0,90), FRotator::ZeroRotator);
            PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn); Mode->GetFlowState()->AddPlayerState(PS);
            auto* Floor = World->SpawnActor<AStaticMeshActor>(FVector(0,0,-50), FRotator::ZeroRotator);
            Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
            Floor->SetActorScale3D(FVector(50,50,1)); Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
            Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay();
            auto Tick = [&](double Time) { for (int32 I = 0; I < FMath::CeilToInt(Time / .025); ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .025f); } };
            if (!Core || Case == TEXT("Duplicate"))
            {
                TestFalse(TEXT("缺失/重复拒绝配置"), Mode->IsMatchConfigured());
                TestTrue(TEXT("启用策略不回退且冻结"), Mode->IsCoreDefenseEnabled() && Mode->IsGameplayFrozen());
                TestNull(TEXT("不登记不唯一核心"), Mode->GetMatchCore());
                World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); return true;
            }
            TestTrue(TEXT("唯一地图核心已注册"), Mode->GetMatchCore() == Core && Core->GetSnapshot().bInitialized);
            TestTrue(TEXT("GameState全队引用"), Mode->GetFlowState()->GetMatchCore() == Core);
            Tick(.3);
            if (TestTrue(TEXT("首批真实刷怪"), Mode->GetWaveMonsters().Num() == 1))
            {
                auto* Monster = Mode->GetWaveMonsters()[0].Get(); auto* ASC = Monster->GetCombatASC();
                TestTrue(TEXT("零发现仍追核心"), ASC->GetCombatTarget() == Core);
                TestFalse(TEXT("进军不绕过攻击距离"), AY01TestAttack::TryAutoAttack(ASC));
                TestFalse(TEXT("核心不能反击"), Core->GetCombatASC()->IsCombatPairAllowed(ASC));
                TestFalse(TEXT("玩家不能攻击核心"), PS->GetCombatASC()->IsCombatPairAllowed(Core->GetCombatASC()));
                auto* Private = World->SpawnActor<AY01Monster>(FVector(1000,0,90), FRotator::ZeroRotator); FString Error;
                Private->TryInitializeMonster(FY01DataCatalog(F.Tables), F.Ref(EY01TableKind::Monster), Error);
                TestFalse(TEXT("未登记私人怪拒绝核心"), Private->GetCombatASC()->IsLegalCombatTarget(Core));
                if (Case == TEXT("PlayerPriority"))
                {
                    const FVector Origin = Monster->GetActorLocation();
                    Pawn->SetActorLocation(Origin + FVector(150,0,0));
                    auto* PC2 = World->SpawnActor<AY01PlaygroundController>(); auto* PS2 = World->SpawnActor<AY01PlayerState>();
                    auto* Pawn2 = World->SpawnActor<AY01PlaygroundCharacter>(Origin + FVector(0,180,0), FRotator::ZeroRotator);
                    PC2->PlayerState = PS2; PC2->Possess(Pawn2); PS2->GetCombatASC()->InitAbilityActorInfo(PS2, Pawn2);
                    PS2->TryInitializeCharacter(Profile->BuildCatalog(), Profile->Character, Error); Mode->GetFlowState()->AddPlayerState(PS2);
                    Mode->GetTraining()->BindCombat(PS2->GetCombatASC());
                    TArray<AActor*> Candidates{Pawn2, Pawn};
                    TestTrue(TEXT("范围内最近玩家优于核心"), Monster->RefreshDetectionTarget(Candidates, Core) == Pawn);
                    Pawn->SetActorLocation(Origin + FVector(1000,0,0));
                    TestTrue(TEXT("越界立即改选合法玩家"), Monster->RefreshDetectionTarget(Candidates, Core) == Pawn2);
                    TestTrue(TEXT("实际进入练功房"), Mode->GetTraining()->Request(PC2, true));
                    TestTrue(TEXT("离区玩家不合法，回退核心"), Monster->RefreshDetectionTarget(Candidates, Core) == Core);
                    Tick(.6);
                    for (auto* RoomMonster : Mode->GetTraining()->Monsters(PS2))
                    { TestFalse(TEXT("登记私人房怪不能打核心"), RoomMonster->GetCombatASC()->IsLegalCombatTarget(Core)); }
                    TestTrue(TEXT("练功房仍共享同一核心警报快照"), Mode->GetFlowState()->GetMatchCore() == Core);
                }
                if (Case == TEXT("LateDuplicate") || Case == TEXT("Removed"))
                {
                    AddExpectedError(Case == TEXT("Removed") ? TEXT("Y01Core MISSING") : TEXT("Y01Core DUPLICATE"), EAutomationExpectedErrorFlags::Contains, 1);
                    if (Case == TEXT("Removed")) { Core->Destroy(); }
                    else { World->SpawnActor<AY01Core>(FVector(2000,0,90), FRotator::ZeroRotator); }
                    TestTrue(TEXT("运行期缺失/重复立即冻结"), Mode->IsGameplayFrozen());
                    TestFalse(TEXT("运行期异常不提前Ended"), Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended);
                    World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); return true;
                }
                PS->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0);
                Tick(2);
                TestTrue(TEXT("全死不结束且无人仍真实攻击核心"), Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Battle && Core->GetSnapshot().HitSequence > 0);
                const auto Count = Mode->GetWaveMonsters().Num();
                const auto HitsBefore = Core->GetSnapshot().HitSequence;
                TestFalse(TEXT("普攻真实间隔不能连续绕过"), AY01TestAttack::TryAutoAttack(ASC) && AY01TestAttack::TryAutoAttack(ASC));
                if (Case == TEXT("Projectile")) { TestTrue(TEXT("远程真实查询扣血"), HitsBefore > 0 && Core->GetSnapshot().Health < 1000); }
                Core->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0);
                TestTrue(TEXT("失守同步冻结"), Mode->IsGameplayFrozen()); Tick(21);
                TestEqual(TEXT("冻结阻断迟到批次"), Mode->GetWaveMonsters().Num(), Count);
                TestFalse(TEXT("冻结阻断开波和结算"), Mode->TryStartNextWave() || Mode->TryCompleteWave());
                TestEqual(TEXT("第2段结清后失败终局"), Mode->GetFlowState()->GetWaveSnapshot().Result, EY01MatchResult::Failure);
            }
            World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); return true;
        }
    }.template operator()<AY01R36GameMode>();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01CoreAlertTest, "Y01.Core.Step1.TeamAlert",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FY01CoreAlertTest::RunTest(const FString&)
{
    auto* Field = FindFProperty<FObjectPropertyBase>(UY01R36Panel::StaticClass(), TEXT("CoreAlert"));
    TestNotNull(TEXT("R36顶部独立单条警报接线"), Field);
    FY01CoreSnapshot S; S.bInitialized = true; S.MaxHealth = 1000; S.Health = 250;
    S.AlertSequence = 1; S.LastAlertTime = 5; S.ServerSampleTime = 5;
    TestTrue(TEXT("低血量攻击提示"), S.AlertText().Contains(TEXT("生命危急")));
    S.ServerSampleTime = 9; TestTrue(TEXT("提示仅消费服务器样本到期"), S.AlertText().IsEmpty());
    S.bFallen = true; TestEqual(TEXT("失守优先且持续"), S.AlertText(), FString(TEXT("大本营已失守")));
    TArray<uint8> Bytes; FMemoryWriter Writer(Bytes); bool Saved = false; S.NetSerialize(Writer, nullptr, Saved);
    FY01CoreSnapshot Received; FMemoryReader Reader(Bytes); bool Loaded = false; Received.NetSerialize(Reader, nullptr, Loaded);
    TestTrue(TEXT("复制同包保留服务器序号时间和优先提示"), Saved && Loaded && Received.AlertText() == S.AlertText()
        && Received.AlertSequence == S.AlertSequence && Received.ServerSampleTime == S.ServerSampleTime);
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(false)
        .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    auto* Panel = CreateWidget<UY01R36Panel>(World); TSharedPtr<SWidget> Slate = Panel->TakeWidget();
    Panel->PresentCoreAlert(Received);
    auto* Alert = Cast<UTextBlock>(Panel->WidgetTree->FindWidget(TEXT("CoreAlert")));
    TestTrue(TEXT("真实单条Widget失守优先"), Alert && Alert->GetText().ToString() == TEXT("大本营已失守") && Alert->IsVisible());
    if (Alert)
    {
        const auto* AlertSlot = Cast<UCanvasPanelSlot>(Alert->Slot);
        const auto* Wave = Panel->WidgetTree->FindWidget(TEXT("WaveCountdown")); const auto* WaveSlot = Cast<UCanvasPanelSlot>(Wave->Slot);
        TestTrue(TEXT("顶部警报不重叠波次也不碰Y96面板"), AlertSlot->GetPosition().Y >= WaveSlot->GetPosition().Y + WaveSlot->GetSize().Y
            && AlertSlot->GetPosition().Y + AlertSlot->GetSize().Y <= 96);
        Panel->PresentCoreAlert({}); TestFalse(TEXT("关闭无固定核心HUD"), Alert->IsVisible());
        TestTrue(TEXT("重复投影不新增警报"), Panel->WidgetTree->FindWidget(TEXT("CoreAlert")) == Alert);
    }
    Slate.Reset(); World->DestroyWorld(false); GEngine->DestroyWorldContext(World);
    return true;
}
#endif
