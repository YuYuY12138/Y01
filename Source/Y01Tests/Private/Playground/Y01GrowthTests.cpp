#include "Misc/AutomationTest.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01PlaygroundSeed.h"
#include "Playground/Y01PlaygroundHUD.h"
#include "Playground/Y01TestSkill.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/Canvas.h"
#include "CanvasTypes.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"
#include "EngineUtils.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
// 功能不存在时仍编译；通过反射观测真实配置/服务器快照，失败必须来自行为断言。
void GrowthConfig(FY01CharacterRow& Row, bool Enabled, double Seconds, double Kills, double Gold)
{
    auto* P = FindFProperty<FStructProperty>(FY01CharacterRow::StaticStruct(), TEXT("R33TestGrowth"));
    if (!P) { return; }
    void* Value = P->ContainerPtrToValuePtr<void>(&Row);
    if (auto* B = FindFProperty<FBoolProperty>(P->Struct, TEXT("bEnabled"))) { B->SetPropertyValue_InContainer(Value, Enabled); }
    const TCHAR* Names[] = { TEXT("AttackPerEffectiveSecond"), TEXT("AttackPerEffectiveKill"), TEXT("GoldPerEffectiveSecond") };
    const double Rates[] = { Seconds, Kills, Gold };
    for (int32 I = 0; I < 3; ++I)
    { if (auto* N = FindFProperty<FDoubleProperty>(P->Struct, Names[I])) { N->SetPropertyValue_InContainer(Value, Rates[I]); } }
}
double GrowthNumber(AY01PlaygroundController* PC, const TCHAR* Name)
{
    auto* G = FindFProperty<FStructProperty>(FY01PlaygroundObserver::StaticStruct(), TEXT("Growth"));
    if (!G) { return -999; }
    const void* V = G->ContainerPtrToValuePtr<void>(&PC->GetObserver());
    if (auto* P = FindFProperty<FNumericProperty>(G->Struct, Name))
    { const void* N = P->ContainerPtrToValuePtr<void>(V); return P->IsFloatingPoint() ? P->GetFloatingPointPropertyValue(N) : static_cast<double>(P->GetSignedIntPropertyValue(N)); }
    return -999;
}
struct FGrowthWorld
{
    TStrongObjectPtr<UY01PlaygroundProfile> Profile{Y01PlaygroundEditor::CreateSeed(GetTransientPackage())};
    UWorld* World;
    AY01PlaygroundGameMode* Mode;
    AY01PlaygroundController* A;
    AY01PlaygroundController* B;
    FGrowthWorld()
    {
        Profile->bTrainingEnabled = true;
        GrowthConfig(*Row(), true, 1, 2, 1);
        auto* Stage = Profile->Tables[EY01TableKind::TrainingStage]->FindRow<FY01TrainingStageRow>(TEXT("Default"), TEXT("Growth"));
        Stage->SpawnInterval = 100; Stage->MaxAlive = 6; Stage->Spawns[0].Count = 2; Stage->Spawns[0].DelaySeconds = 0;
        for (auto Name : { TEXT("Default"), TEXT("Boss") })
        { auto* M = Profile->Tables[EY01TableKind::Monster]->FindRow<FY01MonsterRow>(Name, TEXT("Growth")); M->AttackDamage = 0; M->MoveSpeed = 0; }
        auto* Ball = Profile->Tables[EY01TableKind::Skill]->FindRow<FY01SkillRow>(TEXT("HeavyAttack"), TEXT("Growth"));
        Ball->Ability = UY01TestSkillAbility::StaticClass(); Ball->TestSkill.Mode = EY01TestSkillMode::Projectile;
        Ball->TestSkill.Radius = 12; Ball->TestSkill.TargetRange = 800; Ball->TestSkill.Speed = 200;
        Ball->TestSkill.Lifetime = 4; Ball->TestSkill.HitFeedbackSeconds = .1f;
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01PlaygroundGameMode::StaticClass();
        World->GetWorldSettings()->MaxUndilatedFrameTime = 10; World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01PlaygroundGameMode>(); World->SpawnActor<AY01PlaygroundConfig>()->Profile = Profile.Get();
        A = Player(); B = Player();
    }
    ~FGrowthWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    FY01CharacterRow* Row() const { return Profile->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("Growth")); }
    AY01PlaygroundController* Player()
    {
        auto* PC = World->SpawnActor<AY01PlaygroundController>(); auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01PlaygroundCharacter>(FVector(0,0,100), FRotator::ZeroRotator);
        PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(PS); return PC;
    }
    AY01PlayerState* PS(AY01PlaygroundController* PC) const { return PC->GetPlayerState<AY01PlayerState>(); }
    UY01CombatAbilitySystem* ASC() const { return PS(A)->GetCombatASC(); }
    float Base() const { return ASC()->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute()); }
    float Current() const { return ASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()); }
    void Start() { Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay(); }
    void Step(float Dt) { ++GFrameCounter; World->Tick(LEVELTICK_All, Dt); }
    void Tick(float Seconds) { for (int32 I = 0; I < FMath::RoundToInt(Seconds * 8); ++I) { Step(.125f); } }
    void Capture() { A->CaptureObserver(Mode); B->CaptureObserver(Mode); }
    AY01Monster* Training(AY01PlaygroundController* PC)
    {
        Mode->GetTraining()->Request(PC, true);
        for (auto* M : Mode->GetTraining()->Monsters(PS(PC)))
        { M->GetCharacterMovement()->DisableMovement(); M->SetActorLocation(PC->GetPawn()->GetActorLocation() + FVector(400,0,0)); }
        auto Ms = Mode->GetTraining()->Monsters(PS(PC)); return Ms.IsEmpty() ? nullptr : Ms[0];
    }
    bool Hit(AY01PlaygroundController* PC, AY01Monster* M)
    { auto* C = PS(PC)->GetCombatASC(); C->SetCombatTarget(M); return PS(PC)->TryActivateSkillSlot(0); }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01GrowthTests, "Y01.Playground.Growth.Runtime",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01GrowthTests::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Defaults"), TEXT("Disabled"), TEXT("Zero"), TEXT("Time.Small"), TEXT("Time.Large"),
        TEXT("Time.Remainder"), TEXT("Time.GoldFraction"), TEXT("Time.WaitingPreparingBattle"), TEXT("Time.DeathBoundary"),
        TEXT("Time.Ended"), TEXT("Time.Logout"), TEXT("Time.PawnLost"), TEXT("Time.Restart"), TEXT("Time.LatePlayer"),
        TEXT("Kill.Training"), TEXT("Kill.Main"), TEXT("Kill.Boss"), TEXT("Kill.RewardOverflow"), TEXT("Kill.DirectZero"),
        TEXT("Kill.Destroy"), TEXT("Kill.CrossRoom"), TEXT("Kill.Stale"), TEXT("Attack.Passive"), TEXT("Attack.Snapshot"),
        TEXT("Overflow.Attack"), TEXT("Overflow.Gold"), TEXT("Overflow.Unrepresentable"), TEXT("HUD.OwnerOnly"), TEXT("HUD.Toggle"), TEXT("HUD.Snapshot"),
        TEXT("Boundary.FractionalAttackTicks"), TEXT("Boundary.UnrepresentableTicks"), TEXT("Boundary.GoldTicks"), TEXT("Boundary.NoGoldDebt"),
        TEXT("Boundary.RealDeath"), TEXT("Boundary.EndPlay"), TEXT("Boundary.KillSnapshot"), TEXT("Boundary.HudBounds"), TEXT("Boundary.DuplicateRegistration"),
        TEXT("Boundary.DecimalGoldTicks"), TEXT("Boundary.DecimalGoldNoDebt"), TEXT("Boundary.DecimalGoldCapacity") })
    { Names.Add(N); Commands.Add(N); }
    for (const TCHAR* Field : { TEXT("Seconds"), TEXT("Kills"), TEXT("Gold") })
    { for (const TCHAR* Bad : { TEXT("Negative"), TEXT("NaN"), TEXT("Infinity") })
        { const FString N = FString(TEXT("Invalid.")) + Field + TEXT(".") + Bad; Names.Add(N); Commands.Add(N); } }
}
bool FY01GrowthTests::RunTest(const FString& Case)
{
    TGuardValue<uint64> FrameGuard(GFrameCounter, GFrameCounter); FGrowthWorld F;
    if (Case == TEXT("Defaults"))
    {
        FY01CharacterRow Default;
        auto* P = FindFProperty<FStructProperty>(FY01CharacterRow::StaticStruct(), TEXT("R33TestGrowth"));
        if (!TestNotNull(TEXT("角色表单一R33测试成长组"), P)) { return false; }
        const void* V = P->ContainerPtrToValuePtr<void>(&Default);
        TestFalse(TEXT("通用默认关闭"), FindFProperty<FBoolProperty>(P->Struct, TEXT("bEnabled"))->GetPropertyValue_InContainer(V));
        for (const TCHAR* N : { TEXT("AttackPerEffectiveSecond"), TEXT("AttackPerEffectiveKill"), TEXT("GoldPerEffectiveSecond") })
        { auto* Rate = FindFProperty<FDoubleProperty>(P->Struct, N); if (TestNotNull(TEXT("可配双精度rate"), Rate)) { TestEqual(TEXT("默认零"), Rate->GetPropertyValue_InContainer(V), 0.); } }
        return true;
    }
    if (Case.StartsWith(TEXT("Invalid.")))
    {
        const double Bad = Case.EndsWith(TEXT("NaN")) ? std::numeric_limits<double>::quiet_NaN() : Case.EndsWith(TEXT("Infinity")) ? std::numeric_limits<double>::infinity() : -1;
        GrowthConfig(*F.Row(), false, Case.Contains(TEXT("Seconds")) ? Bad : 1, Case.Contains(TEXT("Kills")) ? Bad : 2, Case.Contains(TEXT("Gold")) ? Bad : 1);
        TestFalse(TEXT("关闭也拒绝非法配置"), F.Profile->Validate().IsValid()); FString Error;
        TestFalse(TEXT("直接初始化不绕过校验"), F.ASC()->TryInitializeFromData(F.Profile->BuildCatalog(), F.Profile->Character, Error));
        TestTrue(TEXT("拒绝无半套GE"), F.ASC()->GetGrantedSources().IsEmpty()); return true;
    }
    if (Case == TEXT("Disabled")) { GrowthConfig(*F.Row(), false, 1, 2, 1); }
    if (Case == TEXT("Zero")) { GrowthConfig(*F.Row(), true, 0, 0, 0); }
    if (Case == TEXT("Time.GoldFraction")) { GrowthConfig(*F.Row(), true, 0, 0, .25); }
    if (Case == TEXT("Overflow.Attack")) { GrowthConfig(*F.Row(), true, std::numeric_limits<double>::max(), 2, 1); }
    if (Case == TEXT("Overflow.Gold")) { GrowthConfig(*F.Row(), true, 1, 2, static_cast<double>(MAX_int32) + 1.25); }
    F.Start(); if (!TestTrue(TEXT("真实世界配置成功"), F.Mode->IsMatchConfigured())) { return false; }
    if (Case == TEXT("Disabled") || Case == TEXT("Zero"))
    { F.Tick(2); TestEqual(TEXT("无攻击授予"), F.Base(), 20.f); TestEqual(TEXT("无金币授予"), F.PS(F.A)->GetGold(), 0); return true; }
    if (Case.StartsWith(TEXT("Boundary.")))
    {
        if (Case == TEXT("Boundary.FractionalAttackTicks") || Case == TEXT("Boundary.UnrepresentableTicks") || Case == TEXT("Boundary.GoldTicks"))
        {
            FGrowthWorld Small, Large;
            const double Rate = Case == TEXT("Boundary.FractionalAttackTicks") ? .1 : 1;
            GrowthConfig(*Small.Row(), true, Rate, 0, 1.5); GrowthConfig(*Large.Row(), true, Rate, 0, 1.5);
            Small.Start(); Large.Start();
            if (Case == TEXT("Boundary.UnrepresentableTicks"))
            { Small.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(),16777216.f); Large.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(),16777216.f); }
            Small.PS(Small.A)->TryAddMatchReward(MAX_int32-2,0); Large.PS(Large.A)->TryAddMatchReward(MAX_int32-2,0);
            Small.Tick(3); Large.Step(3); Small.Capture(); Large.Capture();
            TestEqual(TEXT("大小tick攻击base严格等价"),Large.Base(),Small.Base(),0.f);
            TestEqual(TEXT("大小tick金币逐秒失败语义等价"),Large.PS(Large.A)->GetGold(),Small.PS(Small.A)->GetGold());
            TestEqual(TEXT("大小tick实际攻击账本等价"),GrowthNumber(Large.A,TEXT("TimeAttackGranted")),GrowthNumber(Small.A,TEXT("TimeAttackGranted")),0.);
            TestEqual(TEXT("金币小数保留且后续可成功"),GrowthNumber(Large.A,TEXT("GoldRemainder")),.5); return true;
        }
        if (Case.StartsWith(TEXT("Boundary.DecimalGold")))
        {
            FGrowthWorld Small, Large;
            const bool bNoDebt = Case.EndsWith(TEXT("NoDebt")), bCapacity = Case.EndsWith(TEXT("Capacity"));
            const double Rate = bCapacity ? 1.1 : .1;
            GrowthConfig(*Small.Row(), true, 0, 0, Rate); GrowthConfig(*Large.Row(), true, 0, 0, Rate);
            Small.Start(); Large.Start();
            const int32 Initial = bNoDebt ? MAX_int32 : bCapacity ? MAX_int32 - 10 : 0;
            Small.PS(Small.A)->TryAddMatchReward(Initial, 0); Large.PS(Large.A)->TryAddMatchReward(Initial, 0);
            Small.Tick(10); Large.Step(10); Small.Capture(); Large.Capture();
            TestEqual(TEXT("小tick真实累计十个有效秒"), GrowthNumber(Small.A,TEXT("EffectiveSeconds")), 10., 0.);
            TestEqual(TEXT("大tick真实累计十个有效秒"), GrowthNumber(Large.A,TEXT("EffectiveSeconds")), 10., 0.);
            const int32 Expected = bNoDebt ? 0 : bCapacity ? 9 : 1;
            TestEqual(TEXT("十进制小数逐秒实际整额"), Small.PS(Small.A)->GetGold() - Initial, Expected);
            TestEqual(TEXT("十进制小数大tick实际整额"), Large.PS(Large.A)->GetGold() - Initial, Expected);
            TestEqual(TEXT("十进制小数实际账本等价"), GrowthNumber(Large.A,TEXT("TimeGoldGranted")), GrowthNumber(Small.A,TEXT("TimeGoldGranted")), 0.);
            TestEqual(TEXT("十进制小数余数严格等价"), GrowthNumber(Large.A,TEXT("GoldRemainder")), GrowthNumber(Small.A,TEXT("GoldRemainder")), 0.);
            if (bNoDebt)
            {
                auto* Gold = FindFProperty<FIntProperty>(AY01PlayerState::StaticClass(),TEXT("Gold"));
                if (!TestNotNull(TEXT("仅夹具模拟消费释放容量"), Gold)) { return false; }
                Gold->SetPropertyValue_InContainer(Small.PS(Small.A),0); Gold->SetPropertyValue_InContainer(Large.PS(Large.A),0);
                Small.Tick(1); Large.Step(1); Small.Capture(); Large.Capture();
                TestEqual(TEXT("小tick失败整额不延迟补发"), Small.PS(Small.A)->GetGold(), 0);
                TestEqual(TEXT("大tick失败整额不延迟补发"), Large.PS(Large.A)->GetGold(), 0);
                TestEqual(TEXT("消费后小数账本仍等价"), GrowthNumber(Large.A,TEXT("GoldRemainder")), GrowthNumber(Small.A,TEXT("GoldRemainder")), 0.);
            }
            return true;
        }
        if (Case == TEXT("Boundary.NoGoldDebt"))
        {
            FGrowthWorld W; GrowthConfig(*W.Row(),true,1,0,.75); W.Start(); W.PS(W.A)->TryAddMatchReward(MAX_int32,0); W.Tick(2);
            auto* Gold = FindFProperty<FIntProperty>(AY01PlayerState::StaticClass(),TEXT("Gold"));
            if (!TestNotNull(TEXT("仅夹具腾出容量模拟之后消费"),Gold)) { return false; }
            Gold->SetPropertyValue_InContainer(W.PS(W.A),0); W.Tick(1); W.Capture();
            TestEqual(TEXT("失败整额不补发但保留合法小数"),W.PS(W.A)->GetGold(),1);
            TestEqual(TEXT("实际累计不虚增"),GrowthNumber(W.A,TEXT("TimeGoldGranted")),1.);
            TestEqual(TEXT("小数可回算"),GrowthNumber(W.A,TEXT("GoldRemainder")),.25); return true;
        }
        if (Case == TEXT("Boundary.RealDeath") || Case == TEXT("Boundary.EndPlay"))
        {
            auto* M = F.Training(F.A); F.Tick(.75f);
            if (Case == TEXT("Boundary.RealDeath"))
            {
                F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),1);
                M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(),1000);
                M->SetActorLocation(F.A->GetPawn()->GetActorLocation()+FVector(50,0,0));
                TestTrue(TEXT("真实怪物目标在攻击距离内"),M->GetCombatASC()->SetCombatTarget(F.A->GetPawn()));
                TestTrue(TEXT("真实怪物GA致死"),M->GetCombatASC()->TryActivateBasicAttack());
                TestEqual(TEXT("真实死亡而非直接设零"),Y01Playground::Health(F.ASC()),0.f);
            }
            else { F.Mode->EndPlay(EEndPlayReason::Quit); }
            F.Step(2); TestEqual(TEXT("死亡/EndPlay边界不加整帧"),F.Base(),20.f); return true;
        }
        if (Case == TEXT("Boundary.DuplicateRegistration"))
        {
            auto* M = F.Training(F.A); F.Mode->GetGrowth()->RegisterMonster(M,F.PS(F.A)); F.Mode->GetGrowth()->RegisterMonster(M,F.PS(F.B));
            M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),1); TestTrue(TEXT("真实致死"),F.Hit(F.A,M)); F.Capture();
            TestEqual(TEXT("重复登记不改归属且不重复计杀"),GrowthNumber(F.A,TEXT("GrowthKills")),1.); return true;
        }
        if (Case == TEXT("Boundary.KillSnapshot"))
        {
            auto* M = F.Training(F.A); auto Ms = F.Mode->GetTraining()->Monsters(F.PS(F.A)); auto* N = Ms[1];
            N->SetActorLocation(F.A->GetPawn()->GetActorLocation()+FVector(0,450,0)); N->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),1);
            F.ASC()->SetCombatTarget(M); TestTrue(TEXT("真实提交球"),F.PS(F.A)->TryActivateSkillSlot(1));
            for (TActorIterator<AY01TestAttack> It(F.World); It; ++It)
            { TestEqual(TEXT("球真实朝向较近的存活目标"),It->GetState().Direction,FVector(1,0,0)); }
            TestTrue(TEXT("在途期间另一真实击杀"),F.Hit(F.A,N)); TestEqual(TEXT("击杀立即加base"),F.Base(),22.f);
            F.Tick(2); TestEqual(TEXT("击杀成长不追改在途伤害"),Y01Playground::Health(M->GetCombatASC()),35.f); return true;
        }
    }
    if (Case.StartsWith(TEXT("HUD.")) || Case == TEXT("Boundary.HudBounds"))
    {
        F.Tick(1); F.Capture();
        if (Case == TEXT("HUD.OwnerOnly"))
        {
            F.A->GetClass()->SetUpRuntimeReplicationData(); TArray<FLifetimeProperty> Props; F.A->GetLifetimeReplicatedProps(Props);
            auto* P = FindFProperty<FStructProperty>(F.A->GetClass(), TEXT("Observer"));
            TestTrue(TEXT("成长只随OwnerOnly完整快照复制"), P && Props.ContainsByPredicate([&](auto& R) { return R.RepIndex == P->RepIndex && R.Condition == COND_OwnerOnly; }));
            TestEqual(TEXT("服务器观测已结算秒"), GrowthNumber(F.A, TEXT("EffectiveSeconds")), 1.); return true;
        }
        auto* HUD = F.World->SpawnActor<AY01PlaygroundHUD>(); HUD->PlayerOwner = F.A;
        TArray<FString> Draws; TArray<FBox2D> Bounds;
        HUD->ObserveDrawForTest = [&](const FBox2D& B, const FString& Text) { Draws.Add(Text); Bounds.Add(B); };
        auto Render = [&] { Draws.Reset(); Bounds.Reset(); FCanvas Batch(nullptr, nullptr, F.World, ERHIFeatureLevel::SM5);
            TStrongObjectPtr<UCanvas> C(NewObject<UCanvas>()); C->Init(1280,800,nullptr,&Batch); C->Update(); HUD->SetCanvas(C.Get(),nullptr); HUD->DrawHUD(); HUD->SetCanvas(nullptr,nullptr); };
        auto Has = [&](const TCHAR* Text) { return Draws.ContainsByPredicate([&](const FString& D) { return D.Contains(Text); }); };
        Render(); TestFalse(TEXT("属性默认收起"), Has(TEXT("[基础]")));
        HUD->NotifyHitBoxClick(TEXT("Attributes")); Render();
        TestTrue(TEXT("四组实际Canvas绘制"), Has(TEXT("[基础]")) && Has(TEXT("[经济]")) && Has(TEXT("[成长]")) && Has(TEXT("[特殊战斗]")));
        TestTrue(TEXT("未接入不冒充攻速"), Has(TEXT("攻速/护甲")) && Has(TEXT("未接")));
        TestTrue(TEXT("非正式测试与非实时提示"), Has(TEXT("非正式")) && Has(TEXT("非实时")));
        if (Case == TEXT("Boundary.HudBounds"))
        {
            for (int32 I=0; I<Bounds.Num(); ++I) { if (Bounds[I].Min.X == 932 && Bounds[I].Min.Y >= 208)
                { TestTrue(TEXT("属性实际文本不越出右栏：")+Draws[I], Bounds[I].Max.X <= 1264 && Bounds[I].Max.Y <= 558); } }
            return true;
        }
        if (Case == TEXT("HUD.Toggle"))
        {
            HUD->NotifyHitBoxClick(TEXT("SkillDetails")); Render(); TestFalse(TEXT("说明展开关闭属性"), Has(TEXT("[基础]")));
            HUD->NotifyHitBoxClick(TEXT("Attributes")); Render(); TestTrue(TEXT("属性重新展开"), Has(TEXT("[基础]")));
            TestFalse(TEXT("属性展开关闭原技能说明长文"), Has(TEXT("离区 / 死亡 / Ended")));
            TestTrue(TEXT("原技能按钮坐标未动"), Bounds.ContainsByPredicate([](const FBox2D& B) { return B.Min == FVector2D(920,166) && B.Max == FVector2D(1264,200); }));
        }
        else
        {
            const auto Before = Draws; F.A->SetActorTickEnabled(false); F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 333); F.PS(F.A)->TryAddMatchReward(111,0);
            Render(); TestTrue(TEXT("HUD仅读同一次快照，不混实时ASC"), Before == Draws);
            F.Capture(); Render(); TestFalse(TEXT("新快照刷新"), Before == Draws);
        }
        return true;
    }
    if (Case.StartsWith(TEXT("Kill.")))
    {
        AY01Monster* M = nullptr;
        if (Case == TEXT("Kill.Main") || Case == TEXT("Kill.Boss"))
        {
            TestTrue(TEXT("真实开始主线"), F.Mode->TryStartNextWave()); F.Tick(6.f);
            AddInfo(FString::Printf(TEXT("主线 World=%g Phase=%d Pending=%d Monsters=%d"), F.World->GetTimeSeconds(), static_cast<int32>(F.Mode->GetFlowState()->GetWaveSnapshot().Phase), F.Mode->GetFlowState()->GetWaveSnapshot().PendingSpawns, F.Mode->GetWaveMonsters().Num()));
            if (Case == TEXT("Kill.Boss"))
            { for (AY01Monster* Old : F.Mode->GetWaveMonsters()) { Old->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),0); }
                TestTrue(TEXT("普通波已清可结算"), F.Mode->TryCompleteWave()); TestTrue(TEXT("真实开始Boss波"), F.Mode->TryStartNextWave()); F.Tick(9.f); }
            if (!TestTrue(TEXT("真实主线生成登记"), !F.Mode->GetWaveMonsters().IsEmpty())) { return false; }
            M = F.Mode->GetWaveMonsters()[0]; M->SetActorLocation(F.A->GetPawn()->GetActorLocation() + FVector(200,0,0));
            if (Case == TEXT("Kill.Boss")) { TestEqual(TEXT("Boss真实定义而非普通怪"), M->GetCombatASC()->GetDefinition().RowName, FName(TEXT("Boss"))); }
        }
        else { M = F.Training(F.A); }
        if (!TestNotNull(TEXT("真实注册怪物"), M)) { return false; }
        const float Base = F.Base(); F.Capture(); const double BeforeKills = GrowthNumber(F.A, TEXT("GrowthKills"));
        M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),1);
        if (Case == TEXT("Kill.RewardOverflow")) { F.PS(F.A)->TryAddMatchReward(MAX_int32 - F.PS(F.A)->GetGold(),0); }
        bool Effective = true;
        if (Case == TEXT("Kill.DirectZero")) { M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),0); Effective = false; }
        else if (Case == TEXT("Kill.Destroy")) { M->Destroy(); Effective = false; }
        else if (Case == TEXT("Kill.CrossRoom"))
        { F.Training(F.B); F.B->GetPawn()->SetActorLocation(F.A->GetPawn()->GetActorLocation()); TestFalse(TEXT("跨房真实攻击拒绝"), F.Hit(F.B,M)); Effective = false; }
        else if (Case == TEXT("Kill.Stale"))
        { F.Mode->RefreshTarget(F.PS(F.A)); TestTrue(TEXT("真实在途球"), F.PS(F.A)->TryActivateSkillSlot(1)); F.Mode->GetTraining()->Request(F.A,false); Effective = false; }
        else { TestTrue(TEXT("真实权威GA致死"), F.Hit(F.A,M)); TestEqual(TEXT("真实HP正到零"), Y01Playground::Health(M->GetCombatASC()),0.f); }
        F.Capture(); TestEqual(TEXT("实际成长攻击仅合法击杀"), F.Base(), Base + (Effective ? 2 : 0));
        TestEqual(TEXT("有效击杀与奖励解耦"), GrowthNumber(F.A,TEXT("GrowthKills")), BeforeKills + (Effective ? 1 : 0));
        TestEqual(TEXT("无助攻共享"), GrowthNumber(F.B,TEXT("GrowthKills")),0.);
        if (Case == TEXT("Kill.RewardOverflow")) { TestEqual(TEXT("奖励失败不虚增旧成功Kills"), F.Mode->GetTraining()->Observe(F.PS(F.A)).Kills,0); }
        if (Effective) { TestFalse(TEXT("死亡后重复攻击拒绝"), F.Hit(F.A,M)); F.Capture(); TestEqual(TEXT("一怪一次"), GrowthNumber(F.A,TEXT("GrowthKills")),BeforeKills+1); }
        return true;
    }
    if (Case == TEXT("Attack.Snapshot"))
    {
        auto* M = F.Training(F.A); TestNotNull(TEXT("真实训练怪"),M); F.Mode->RefreshTarget(F.PS(F.A));
        TestTrue(TEXT("真实提交球"), F.PS(F.A)->TryActivateSkillSlot(1)); AY01TestAttack* Attack = nullptr;
        for (TActorIterator<AY01TestAttack> It(F.World); It; ++It) { Attack = *It; }
        if (!TestNotNull(TEXT("真实在途实例"),Attack)) { return false; }
        TestEqual(TEXT("初始快照含一次被动"), Attack->GetState().DamageSnapshot,25.f);
        F.Tick(1); TestEqual(TEXT("在途成长改变base"), F.Base(),21.f); TestEqual(TEXT("不改在途快照"),Attack->GetState().DamageSnapshot,25.f);
        F.Tick(1); TestEqual(TEXT("真实旧球扣25"),Y01Playground::Health(M->GetCombatASC()),35.f);
        TestTrue(TEXT("后续真实即时释放"),F.Hit(F.A,M)); TestEqual(TEXT("新释放使用成长后27"),Y01Playground::Health(M->GetCombatASC()),8.f); return true;
    }
    if (Case == TEXT("Time.Restart"))
    { F.Tick(2); { FGrowthWorld New; New.Start(); TestEqual(TEXT("停止重开属性清零"),New.Base(),20.f); TestEqual(TEXT("停止重开金币清零"),New.PS(New.A)->GetGold(),0); } return true; }
    if (Case == TEXT("Time.LatePlayer"))
    { F.Tick(2); auto* PC = F.Player(); FString Error; TestTrue(TEXT("晚加入真实初始化"), F.PS(PC)->TryInitializeCharacter(F.Profile->BuildCatalog(), F.Profile->Character, Error)); F.Step(.125f); const auto Base = F.PS(PC)->GetCombatASC()->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute());
        TestEqual(TEXT("晚进入不补离线"),Base,20.f); return true; }
    if (Case == TEXT("Time.GoldFraction"))
    { F.Tick(3); TestEqual(TEXT("金币小数不提前授予"),F.PS(F.A)->GetGold(),0); F.Training(F.A); F.Tick(1); TestEqual(TEXT("切区保留金币小数"),F.PS(F.A)->GetGold(),1); return true; }
    if (Case == TEXT("Time.Remainder"))
    { F.Tick(.75f); F.Training(F.A); F.Tick(.5f); F.Mode->GetTraining()->Request(F.A,false); F.Capture();
        TestEqual(TEXT("跨区累计一秒"),F.Base(),21.f); TestEqual(TEXT("跨区余数"),GrowthNumber(F.A,TEXT("SecondRemainder")),.25); return true; }
    if (Case == TEXT("Time.WaitingPreparingBattle"))
    { F.Tick(1); F.Mode->TryStartNextWave(); F.Tick(5.25f); TestEqual(TEXT("Waiting Preparing Battle均累计"),F.Base(),26.f); return true; }
    if (Case == TEXT("Time.DeathBoundary") || Case == TEXT("Time.Ended") || Case == TEXT("Time.Logout") || Case == TEXT("Time.PawnLost"))
    {
        F.Tick(.75f);
        if (Case == TEXT("Time.DeathBoundary")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),0); }
        if (Case == TEXT("Time.Ended")) { F.Mode->TryStartNextWave(); F.Mode->TryEndMatch(false); }
        if (Case == TEXT("Time.Logout")) { F.Mode->Logout(F.A); }
        if (Case == TEXT("Time.PawnLost")) { F.A->GetPawn()->Destroy(); }
        F.Step(2); F.Capture(); TestEqual(TEXT("停止不多算下一整帧"),F.Base(),20.f);
        if (Case != TEXT("Time.Logout")) { TestEqual(TEXT("停止保留合法余数"),GrowthNumber(F.A,TEXT("SecondRemainder")),.75); } return true;
    }
    if (Case == TEXT("Overflow.Unrepresentable")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(),16777216.f); }
    if (Case == TEXT("Time.Large")) { F.Step(3.25f); } else { F.Tick(3.25f); }
    F.Capture();
    if (Case.StartsWith(TEXT("Overflow.")))
    {
        TestEqual(TEXT("攻击失败与金币独立"),F.Base(),Case == TEXT("Overflow.Attack") ? 20.f : Case == TEXT("Overflow.Unrepresentable") ? 16777216.f : 23.f);
        TestEqual(TEXT("金币失败不影响攻击"),F.PS(F.A)->GetGold(),Case == TEXT("Overflow.Gold") ? 0 : 3);
        TestEqual(TEXT("异常实际攻击累计不虚增"),GrowthNumber(F.A,TEXT("TimeAttackGranted")),Case == TEXT("Overflow.Gold") ? 3. : 0.); return true;
    }
    TestEqual(TEXT("每满有效秒授予base"),F.Base(),23.f); TestEqual(TEXT("被动+5不能反写重复"),F.Current(),28.f);
    TestEqual(TEXT("整额金币"),F.PS(F.A)->GetGold(),3); TestEqual(TEXT("有效秒可回算"),GrowthNumber(F.A,TEXT("EffectiveSeconds")),3.);
    TestEqual(TEXT("余数保留"),GrowthNumber(F.A,TEXT("SecondRemainder")),.25);
    TestEqual(TEXT("实际授予累计"),GrowthNumber(F.A,TEXT("TimeAttackGranted")),3.);
    if (Case == TEXT("Attack.Passive"))
    { for (const auto& G : F.ASC()->GetGrantedSources()) { if (G.Source.RowName == TEXT("Fortitude")) { TestNotNull(TEXT("保留真实被动GE"),F.ASC()->GetActiveGameplayEffect(G.Effect)); } } }
    return true;
}
#endif
