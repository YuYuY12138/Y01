#include "Misc/AutomationTest.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01PlaygroundSeed.h"
#include "Playground/Y01TestSkill.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/DataTable.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameplayEffectExtension.h"
#include "Misc/CommandLine.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include <limits>
#include "Components/WidgetComponent.h"
#include "Components/TextBlock.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "Materials/Material.h"
#include "Camera/PlayerCameraManager.h"
#include "Widgets/SWidget.h"
#include "PrimitiveSceneProxyDesc.h"
#include "SceneViewOwner.h"
#include "Net/RepLayout.h"
#include "Engine/NetConnection.h"
#include "Engine/DemoNetDriver.h"
#include "UObject/GarbageCollection.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
// 先通过真实GE与诊断输出观察行为，功能不存在时仍能编译和运行。
struct FDamageLog : FOutputDevice
{
    FString Previous = FCommandLine::Get();
    TArray<FString> Commits, Sent, Received, Batches, Chunks;
    FDamageLog() { FCommandLine::Set(*(Previous + TEXT(" -Y01DamageTrace"))); GLog->AddOutputDevice(this); }
    ~FDamageLog() { GLog->RemoveOutputDevice(this); FCommandLine::Set(*Previous); }
    bool CanBeUsedOnMultipleThreads() const override { return true; }
    void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName&) override
    {
        if (!IsInGameThread()) { return; }
        const FString Line(Text);
        if (Line.StartsWith(TEXT("Y01Damage COMMIT "))) { Commits.Add(Line); }
        if (Line.StartsWith(TEXT("Y01Damage SEND "))) { Sent.Add(Line); }
        if (Line.StartsWith(TEXT("Y01Damage RECEIVE "))) { Received.Add(Line); }
        if (Line.StartsWith(TEXT("Y01Damage BATCH "))) { Batches.Add(Line); }
        if (Line.StartsWith(TEXT("Y01Damage CHUNK "))) { Chunks.Add(Line); }
    }
    static FString Field(const FString& Line, const TCHAR* Key)
    {
        const FString Token = FString(TEXT(" ")) + Key + TEXT("="); const int32 At = Line.Find(Token);
        if (At == INDEX_NONE) { return {}; }
        FString Value = Line.Mid(At + Token.Len()); int32 End;
        if (Value.FindChar(' ', End)) { Value.LeftInline(End); } return Value;
    }
    static double Number(const FString& Line, const TCHAR* Key)
    { const FString Value = Field(Line, Key); return Value.IsEmpty() ? -1.e30 : FCString::Atod(*Value); }
};
struct FDamageWorld
{
    TStrongObjectPtr<UY01PlaygroundProfile> Profile{Y01PlaygroundEditor::CreateSeed(GetTransientPackage())};
    UWorld* World;
    AY01PlaygroundGameMode* Mode;
    AY01PlaygroundController* A;
    AY01PlaygroundController* B;
    FDamageWorld()
    {
        Profile->bTrainingEnabled = true;
        auto* C = Profile->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("Damage"));
        C->Health = 1000; C->AttackDamage = 20; C->Range = 800; C->MoveSpeed = 0;
        C->InitialEquipment.Reset(); C->R33TestGrowth.bEnabled = false;
        C->InitialSkills.SetNum(2);
        for (int32 I = 0; I < 2; ++I)
        {
            C->InitialSkills[I].Type = EY01TableKind::Skill; C->InitialSkills[I].RowName = I ? TEXT("HeavyAttack") : TEXT("BasicAttack");
            auto* R = Profile->Tables[EY01TableKind::Skill]->FindRow<FY01SkillRow>(C->InitialSkills[I].RowName, TEXT("Damage"));
            R->Ability = UY01TestSkillAbility::StaticClass(); R->CooldownSeconds = 0;
            R->TestSkill.Mode = I ? EY01TestSkillMode::Projectile : EY01TestSkillMode::Area;
            R->TestSkill.DamageCoefficient = 1; R->TestSkill.Radius = I ? 12 : 250;
            R->TestSkill.HalfHeight = 120; R->TestSkill.ActiveSeconds = .35f;
            R->TestSkill.TargetRange = 800; R->TestSkill.Speed = 400; R->TestSkill.Lifetime = 2.5f; R->TestSkill.HitFeedbackSeconds = .1f;
        }
        for (const TCHAR* Name : {TEXT("Default"), TEXT("Boss")})
        {
            auto* M = Profile->Tables[EY01TableKind::Monster]->FindRow<FY01MonsterRow>(Name, TEXT("Damage"));
            M->Health = 1000; M->AttackDamage = 0; M->MoveSpeed = 0; M->Range = 800; M->Skills.Reset();
        }
        auto* Stage = Profile->Tables[EY01TableKind::TrainingStage]->FindRow<FY01TrainingStageRow>(TEXT("Default"), TEXT("Damage"));
        Stage->SpawnInterval = 100; Stage->MaxAlive = 2; Stage->Spawns[0].Count = 2; Stage->Spawns[0].DelaySeconds = 0;
        for (const auto& Ref : Profile->OrderedWaves)
        {
            auto* Wave = Profile->Tables[EY01TableKind::Wave]->FindRow<FY01WaveRow>(Ref.RowName, TEXT("Damage"));
            Wave->PreparationSeconds = Wave->Type == EY01WaveType::Boss ? 1 : 0;
            for (auto& Spawn : Wave->Spawns) { Spawn.DelaySeconds = 0; }
        }
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01PlaygroundGameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01PlaygroundGameMode>(); World->SpawnActor<AY01PlaygroundConfig>()->Profile = Profile.Get();
        A = Player(); B = Player(); B->GetPawn()->SetActorLocation(FVector(5000,5000,100));
    }
    ~FDamageWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    AY01PlaygroundController* Player()
    {
        auto* PC = World->SpawnActor<AY01PlaygroundController>(); auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01PlaygroundCharacter>(FVector(0,0,100), FRotator::ZeroRotator);
        PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(PS); return PC;
    }
    AY01PlayerState* PS(AY01PlaygroundController* PC) const { return PC->GetPlayerState<AY01PlayerState>(); }
    UY01CombatAbilitySystem* ASC() const { return PS(A)->GetCombatASC(); }
    void Start() { Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay(); }
    void Tick(float Seconds) { for (int32 I=0; I<FMath::RoundToInt(Seconds * 100); ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .01f); } }
    TArray<AY01Monster*> Training(AY01PlaygroundController* PC)
    {
        Mode->GetTraining()->Request(PC, true); auto Monsters = Mode->GetTraining()->Monsters(PS(PC));
        for (int32 I=0; I<Monsters.Num(); ++I)
        { Monsters[I]->GetCharacterMovement()->DisableMovement(); Monsters[I]->SetActorLocation(PC->GetPawn()->GetActorLocation() + FVector(I ? 700 : 150, 0, 0)); }
        return Monsters;
    }
    static float HP(UY01CombatAbilitySystem* ASC) { return Y01Playground::Health(ASC); }
    static void Damage(UY01CombatAbilitySystem* Source, UY01CombatAbilitySystem* Target, float Amount)
    {
        auto Spec = Source->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, Source->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, Amount); Source->ApplyGameplayEffectSpecToTarget(*Spec.Data, Target);
    }
    static void Untagged(UY01CombatAbilitySystem* Source, UY01CombatAbilitySystem* Target, float Amount, bool bTagged = false)
    {
        auto* GE = NewObject<UGameplayEffect>(); GE->DurationPolicy = EGameplayEffectDurationType::Instant;
        auto& Mod = GE->Modifiers.AddDefaulted_GetRef(); Mod.Attribute = UY01AttributeSet::GetHealthAttribute();
        Mod.ModifierOp = EGameplayModOp::Additive; Mod.ModifierMagnitude = FScalableFloat(Amount);
        FGameplayEffectSpec Spec(GE, Source->MakeEffectContext(), 1);
        const auto Tag = FGameplayTag::RequestGameplayTag(TEXT("Effect.Y01.Damage"), false);
        if (bTagged && Tag.IsValid()) { Spec.AddDynamicAssetTag(Tag); }
        Source->ApplyGameplayEffectSpecToTarget(Spec, Target);
    }
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01DamagePresentationAsset, "Y01.Playground.Damage.AssetPresentation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FY01DamagePresentationAsset::RunTest(const FString&)
{
    const auto* Table = LoadObject<UDataTable>(nullptr, TEXT("/Game/Y01Playground/Data/DT_MatchRules.DT_MatchRules"));
    if (!TestNotNull(TEXT("只读加载正式DT_MatchRules"), Table)) { return false; }
    const auto* Rules = Table->FindRow<FY01MatchRulesRow>(TEXT("Default"), TEXT("Step4AssetPresentation"));
    if (!TestNotNull(TEXT("正式DT_MatchRules存在Default行"), Rules)) { return false; }
    TestEqual(TEXT("正式伤害颜色为红"), Rules->Presentation.DamageColor, FLinearColor::Red);
    TestEqual(TEXT("正式治疗颜色为绿"), Rules->Presentation.HealingColor, FLinearColor::Green);
    TestEqual(TEXT("正式自然回血合并窗为1秒"), Rules->Presentation.RegenMergeSeconds, 1.0);
    AddInfo(FString::Printf(TEXT("DT_MatchRules.Default.Presentation Damage=(%g,%g,%g,%g) Healing=(%g,%g,%g,%g) RegenMergeSeconds=%g"),
        Rules->Presentation.DamageColor.R, Rules->Presentation.DamageColor.G, Rules->Presentation.DamageColor.B, Rules->Presentation.DamageColor.A,
        Rules->Presentation.HealingColor.R, Rules->Presentation.HealingColor.G, Rules->Presentation.HealingColor.B, Rules->Presentation.HealingColor.A,
        Rules->Presentation.RegenMergeSeconds));
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DamageActual, "Y01.Playground.Damage.Actual", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01DamageActual::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Legacy.Armor"), TEXT("Legacy.Overkill"), TEXT("Legacy.Zero"), TEXT("Legacy.Negative"), TEXT("Legacy.NaN"), TEXT("Legacy.Infinity"),
        TEXT("Manual.Area"), TEXT("Manual.Ball"), TEXT("Auto.Sector"), TEXT("Auto.Ball"), TEXT("MonsterToPlayer"),
        TEXT("SameFrameHeal"), TEXT("Nested.DamageHeal"), TEXT("Nested.DirectSet"), TEXT("Death.Destroy"), TEXT("Death.SessionDestroy"),
        TEXT("Filter.Direct"), TEXT("Filter.Cost"), TEXT("Filter.PreRejected"), TEXT("Filter.ForgedSkill"), TEXT("Filter.Initialization")})
    { Names.Add(N); Commands.Add(N); }
    for (const TCHAR* N : {TEXT("Display.Overkill"), TEXT("Healing.ActualClamp"), TEXT("Healing.FullNoEvent")}) { Names.Add(N); Commands.Add(N); }
}
bool FY01DamageActual::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FDamageWorld F; FDamageLog Log;
    const bool Auto = Case.StartsWith(TEXT("Auto."));
    if (Auto)
    {
        F.Profile->bR34AutoAttackEnabled = true;
        auto* C = F.Profile->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("Damage"));
        C->AttackType = Case.EndsWith(TEXT("Ball")) ? EY01AttackType::Ranged : EY01AttackType::Melee;
        C->Range = 250; C->AttackInterval = 10;
    }
    F.Start(); if (!TestTrue(TEXT("真实配置与初始化"), F.Mode->IsMatchConfigured() && F.ASC()->IsCombatInitialized())) { return false; }
    auto Monsters = F.Training(F.A); if (!TestEqual(TEXT("真实训练两只"), Monsters.Num(), 2)) { return false; }
    auto* M = Monsters[0]; auto* T = M->GetCombatASC(); auto* S = F.ASC();
    TestEqual(TEXT("初始化不产生伤害事件"), Log.Commits.Num(), 0);
    if (Case == TEXT("Display.Overkill") || Case.StartsWith(TEXT("Healing.")))
    {
        TArray<FY01DamageEvent> Events;
        auto* Service = F.World->GetSubsystem<UY01DamageEvents>();
        const auto Handle = Service->OnDamageCommitted.AddLambda([&](const FY01DamageEvent& E) { Events.Add(E); });
        if (Case == TEXT("Display.Overkill"))
        {
            T->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 25);
            FDamageWorld::Damage(S, T, 35);
            TestEqual(TEXT("过量结算只实际扣除剩余25"), FDamageWorld::HP(T), 0.f);
            if (TestEqual(TEXT("过量伤害只发布一个数字事件"), Events.Num(), 1))
            {
                TestEqual(TEXT("真实血差保持25"), Events[0].ActualDamage, 25.f);
                TestEqual(TEXT("展示保留扣甲后HP截断前35"), Events[0].DisplayAmount, 35.f);
                TestEqual(TEXT("事件明确为伤害"), Events[0].Type, EY01DamageEventType::Damage);
            }
        }
        else
        {
            T->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), Case == TEXT("Healing.ActualClamp") ? 975 : 1000);
            S->ApplyHealingTo(T, 35);
            TestEqual(TEXT("治疗按上限落实"), FDamageWorld::HP(T), 1000.f);
            const int32 Expected = Case == TEXT("Healing.ActualClamp") ? 1 : 0;
            if (TestEqual(TEXT("只有实际恢复才发布治疗数字"), Events.Num(), Expected) && Expected == 1)
            {
                TestEqual(TEXT("过量治疗只显示实际恢复25"), Events[0].DisplayAmount, 25.f);
                TestEqual(TEXT("治疗事件不冒充实际伤害"), Events[0].ActualDamage, 0.f);
                TestEqual(TEXT("事件明确为治疗"), Events[0].Type, EY01DamageEventType::Healing);
            }
        }
        Service->OnDamageCommitted.Remove(Handle);
        return true;
    }
    const FVector Position = Case == TEXT("MonsterToPlayer") ? F.A->GetPawn()->GetActorLocation() : M->GetActorLocation();
    float Expected = 20;
    if (Case == TEXT("Legacy.Armor") || Case.StartsWith(TEXT("Manual.")) || Auto)
    { T->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), 7); Expected = 13; }
    if (Case == TEXT("Legacy.Overkill") || Case.StartsWith(TEXT("Death.")))
    { T->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 8); Expected = 8; }
    if (Case == TEXT("Death.Destroy"))
    {
        T->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda([&](const FOnAttributeChangeData& Data)
        { if (Data.NewValue <= 0) { M->Destroy(); F.A->GetPawn()->Destroy(); } });
    }
    if (Case == TEXT("Death.SessionDestroy"))
    { T->CombatSessionInvalidated.AddLambda([&]() { M->Destroy(); }); }
    if (Case.StartsWith(TEXT("Nested.")))
    {
        bool Entered = false;
        auto Handle = T->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda([&](const FOnAttributeChangeData& Data)
        {
            if (Entered || Data.NewValue >= Data.OldValue) { return; } Entered = true;
            if (Case.EndsWith(TEXT("DirectSet"))) { T->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 995); }
            else { FDamageWorld::Untagged(S, T, 5); }
            FDamageWorld::Damage(S, T, 7);
        });
        FDamageWorld::Damage(S, T, 10);
        T->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).Remove(Handle);
        TestEqual(TEXT("嵌套伤害及回血真实落实"), FDamageWorld::HP(T), 988.f);
        if (TestEqual(TEXT("两个独立实际伤害事件"), Log.Commits.Num(), 2))
        {
            TestEqual(TEXT("内层先发布实际7"), FDamageLog::Number(Log.Commits[0], TEXT("Actual")), 7.0);
            TestEqual(TEXT("外层冻结实际10不受回血影响"), FDamageLog::Number(Log.Commits[1], TEXT("Actual")), 10.0);
            TestNotEqual(TEXT("嵌套身份不覆盖"), FDamageLog::Field(Log.Commits[0], TEXT("Id")), FDamageLog::Field(Log.Commits[1], TEXT("Id")));
        }
        return true;
    }
    if (Case == TEXT("SameFrameHeal"))
    {
        FDamageWorld::Damage(S, T, 10); FDamageWorld::Untagged(S, T, 5); FDamageWorld::Damage(S, T, 20);
        TestEqual(TEXT("同帧真实扣血与回血"), FDamageWorld::HP(T), 975.f);
        if (TestEqual(TEXT("不按OnRep合并，两击两事件"), Log.Commits.Num(), 2))
        {
            TestEqual(TEXT("第一击10"), FDamageLog::Number(Log.Commits[0], TEXT("Actual")), 10.0);
            TestEqual(TEXT("第二击20"), FDamageLog::Number(Log.Commits[1], TEXT("Actual")), 20.0);
            TestTrue(TEXT("顺序身份单调"), FDamageLog::Number(Log.Commits[1], TEXT("Id")) > FDamageLog::Number(Log.Commits[0], TEXT("Id")));
        }
        return true;
    }
    if (Case.StartsWith(TEXT("Filter.")) || Case == TEXT("Legacy.Zero") || Case == TEXT("Legacy.Negative") || Case == TEXT("Legacy.NaN") || Case == TEXT("Legacy.Infinity"))
    {
        float Health = 1000;
        if (Case == TEXT("Filter.Direct")) { T->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 990); Health = 990; }
        if (Case == TEXT("Filter.Cost")) { FDamageWorld::Untagged(S, T, -5); Health = 995; }
        if (Case == TEXT("Filter.PreRejected"))
        {
            T->TargetPermission = [](const UY01CombatAbilitySystem*, const UY01CombatAbilitySystem*) { return false; };
            FDamageWorld::Untagged(S, T, -5, true); F.Mode->GetTraining()->BindCombat(T);
        }
        if (Case == TEXT("Filter.ForgedSkill"))
        { auto Spec = S->MakeOutgoingSpec(UY01TestSkillDamageEffect::StaticClass(), 1, S->MakeEffectContext()); S->ApplyGameplayEffectSpecToTarget(*Spec.Data, T); }
        if (Case.StartsWith(TEXT("Legacy.")))
        {
            const float Value = Case.EndsWith(TEXT("NaN")) ? std::numeric_limits<float>::quiet_NaN() : Case.EndsWith(TEXT("Infinity")) ? std::numeric_limits<float>::infinity()
                : Case.EndsWith(TEXT("Negative")) ? -1 : 0;
            FDamageWorld::Damage(S, T, Value);
            if (Case.EndsWith(TEXT("Zero"))) { T->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), 25); FDamageWorld::Damage(S, T, 20); T->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), 0); }
        }
        TestEqual(TEXT("过滤路径的实际生命"), FDamageWorld::HP(T), Health);
        TestEqual(TEXT("直接设置、消耗、拒绝、非正伤害均不显示"), Log.Commits.Num(), 0);
        // 正向对照防止“从不产生事件”的假绿。
        FDamageWorld::Damage(S, T, 20);
    }
    else if (Case.StartsWith(TEXT("Manual.")))
    { TestTrue(TEXT("真实手动技能提交"), S->TryActivateSkillSlot(Case.EndsWith(TEXT("Ball")) ? 1 : 0)); F.Tick(.5f); }
    else if (Auto) { TestTrue(TEXT("真实自动普攻提交"), AY01TestAttack::TryAutoAttack(S)); F.Tick(.5f); }
    else if (Case == TEXT("MonsterToPlayer")) { FDamageWorld::Damage(T, S, 20); }
    else { FDamageWorld::Damage(S, T, 20); }
    if (TestEqual(TEXT("成功落实的一击只产生一次事件"), Log.Commits.Num(), 1))
    {
        const FString& E = Log.Commits[0];
        TestEqual(TEXT("实际值取扣甲后且受剩余生命限制"), FDamageLog::Number(E, TEXT("Actual")), static_cast<double>(Expected));
        TestEqual(TEXT("目标Avatar位置X"), FDamageLog::Number(E, TEXT("X")), Position.X);
        TestEqual(TEXT("目标Avatar位置Y"), FDamageLog::Number(E, TEXT("Y")), Position.Y);
        TestEqual(TEXT("目标Avatar位置Z"), FDamageLog::Number(E, TEXT("Z")), Position.Z);
        TestTrue(TEXT("有单调身份"), FDamageLog::Number(E, TEXT("Id")) > 0);
        TestFalse(TEXT("有World身份"), FDamageLog::Field(E, TEXT("Epoch")).IsEmpty());
    }
    if (Case.StartsWith(TEXT("Death."))) { TestTrue(TEXT("目标死亡立即Destroy"), M->IsActorBeingDestroyed()); }
    if (Case.StartsWith(TEXT("Manual.")) || Auto || Case == TEXT("Legacy.Armor"))
    { TestEqual(TEXT("真实GE扣甲后生命"), FDamageWorld::HP(T), 987.f); }
    if (Case == TEXT("MonsterToPlayer")) { TestEqual(TEXT("玩家真实生命减少"), FDamageWorld::HP(S), 980.f); }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DamageRoute, "Y01.Playground.Damage.Route", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01DamageRoute::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("MainBoth"), TEXT("TrainingBoth"), TEXT("MainExcludesTraining"), TEXT("Unknown"), TEXT("CrossRegion"),
        TEXT("SelfHealing.Main"), TEXT("SelfHealing.Training"), TEXT("DeathSnapshot"), TEXT("DeathSessionSnapshot"),
        TEXT("SwitchPending"), TEXT("SwitchDuringDamage"), TEXT("LogoutPending"), TEXT("LogoutDuringDamage"),
        TEXT("QueueOldest"), TEXT("RateLimit"), TEXT("Receive.Duplicate"), TEXT("Receive.OutOfOrder"), TEXT("Receive.OldRegion"), TEXT("Receive.OldWorld"),
        TEXT("Receive.OldControl"), TEXT("Receive.Bounds"), TEXT("Receive.Oversized"), TEXT("Receive.Malformed"), TEXT("Receive.Disabled"), TEXT("Receive.EndPlay"), TEXT("RPC.Contract")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01DamageRoute::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FDamageWorld F; FDamageLog Log; F.Start();
    F.A->Player = NewObject<ULocalPlayer>(GEngine);
    F.B->Player = NewObject<ULocalPlayer>(GEngine);
    if (!TestTrue(TEXT("路由夹具初始化"), F.Mode->IsMatchConfigured())) { return false; }
    if (Case.StartsWith(TEXT("SelfHealing.")))
    {
        if (Case.EndsWith(TEXT("Training")) && !TestEqual(TEXT("自疗夹具真实进入训练房"), F.Training(F.A).Num(), 2)) { return false; }
        TArray<FY01DamageEvent> AEvents, BEvents;
        const auto HA = F.A->OnDamageReceived.AddLambda([&](const FY01DamageEvent& E) { AEvents.Add(E); });
        const auto HB = F.B->OnDamageReceived.AddLambda([&](const FY01DamageEvent& E) { BEvents.Add(E); });
        auto* Self = F.ASC(); Self->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 900);
        TestTrue(TEXT("使用明确治疗入口自疗"), Self->ApplyHealingTo(Self, 20)); F.Tick(.1f);
        TestEqual(TEXT("自疗者收到本区域治疗数字"), AEvents.Num(), 1);
        TestEqual(TEXT("主线自疗共享、训练自疗不外泄"), BEvents.Num(), Case.EndsWith(TEXT("Main")) ? 1 : 0);
        if (!AEvents.IsEmpty())
        {
            TestEqual(TEXT("自疗事件为Healing"), AEvents[0].Type, EY01DamageEventType::Healing);
            TestEqual(TEXT("自疗显示有效恢复量"), AEvents[0].DisplayAmount, 20.f);
            TestEqual(TEXT("自疗事件区域跟随治疗者"), AEvents[0].RegionId, F.A->GetDamageAudience().RegionId);
        }
        F.A->OnDamageReceived.Remove(HA); F.B->OnDamageReceived.Remove(HB);
        return true;
    }
    if (Case == TEXT("RPC.Contract") || Case.StartsWith(TEXT("Receive.")))
    {
        auto* ScopeRPC = AY01PlayerController::StaticClass()->FindFunctionByName(TEXT("ClientSetDamageAudience"));
        auto* BatchRPC = AY01PlayerController::StaticClass()->FindFunctionByName(TEXT("ClientDamageBatch"));
        if (!TestNotNull(TEXT("稳定基础Controller接收区域"), ScopeRPC) || !TestNotNull(TEXT("稳定基础Controller接收有界批次"), BatchRPC)) { return false; }
        if (Case == TEXT("RPC.Contract"))
        {
            TestTrue(TEXT("批次仅OwningClient"), BatchRPC->HasAnyFunctionFlags(FUNC_NetClient));
            TestFalse(TEXT("cosmetic不占Reliable洪泛"), BatchRPC->HasAnyFunctionFlags(FUNC_NetReliable | FUNC_NetServer | FUNC_NetMulticast));
            TestTrue(TEXT("区域控制可靠"), ScopeRPC->HasAllFunctionFlags(FUNC_NetClient | FUNC_NetReliable)); return true;
        }
        AY01PlayerController* PC = Case == TEXT("Receive.Disabled") ? F.World->SpawnActor<AY01PlayerController>() : F.A;
        FY01DamageAudience Scope; Scope.WorldEpoch = F.World->GetSubsystem<UY01DamageEvents>()->GetWorldEpoch(); Scope.RegionId = 0; Scope.Generation = 100;
        auto SetScope = [&](const FY01DamageAudience& Value) { auto Params = Value; PC->ProcessEvent(ScopeRPC, &Params); };
        auto Receive = [&](const FY01DamageAudience& Audience, TArray<FY01DamageEvent> Events)
        { struct FParams { FY01DamageAudience Audience; TArray<FY01DamageEvent> Events; } Params{Audience, MoveTemp(Events)}; PC->ProcessEvent(BatchRPC, &Params); };
        auto Event = [&](uint64 Id) { FY01DamageEvent E; E.EventId = Id; E.WorldEpoch = Scope.WorldEpoch; E.RegionId = Scope.RegionId; E.ActualDamage = 7; E.DisplayAmount = 7; return E; };
        SetScope(Scope);
        if (Case == TEXT("Receive.Disabled")) { Receive(Scope, {Event(1)}); TestEqual(TEXT("基础Controller默认关闭"), Log.Received.Num(), 0); return true; }
        if (Case == TEXT("Receive.EndPlay"))
        { PC->RouteEndPlay(EEndPlayReason::LevelTransition); SetScope(Scope); Receive(Scope, {Event(1)}); TestEqual(TEXT("EndPlay清理且不再播放"), Log.Received.Num(), 0); return true; }
        if (Case == TEXT("Receive.Oversized"))
        {
            for (int32 Count : {9, 128, 129})
            {
                TArray<FY01DamageEvent> Many; for (int32 Id=1; Id<=Count; ++Id) { Many.Add(Event(Id)); }
                Receive(Scope, Many); TestEqual(TEXT("单RPC超8整包拒绝，周期容量仍为128"), Log.Received.Num(), 0);
            }
            TArray<FY01DamageEvent> Eight; for (uint64 Id=2; Id<=9; ++Id) { Eight.Add(Event(Id)); }
            Receive(Scope, Eight); TestEqual(TEXT("单RPC边界8全部接收"), Log.Received.Num(), 8);
            Log.Received.Reset();
        }
        if (Case == TEXT("Receive.Malformed"))
        {
            TArray<FY01DamageEvent> Bad;
            for (float Damage : {0.f, -1.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()})
            { auto E = Event(1); E.ActualDamage = Damage; Bad.Add(E); }
            auto E = Event(0); Bad.Add(E); E = Event(1); E.WorldPosition.X = std::numeric_limits<double>::quiet_NaN(); Bad.Add(E);
            E = Event(1); E.WorldEpoch = FGuid::NewGuid(); Bad.Add(E); E = Event(1); E.RegionId = 88; Bad.Add(E);
            Receive(Scope, Bad); TestEqual(TEXT("无效载荷不播放不占去重身份"), Log.Received.Num(), 0);
        }
        if (Case == TEXT("Receive.Bounds"))
        {
            for (uint64 Id=1; Id<=640; ++Id) { Receive(Scope, {Event(Id)}); }
            TestEqual(TEXT("持续合法新事件不丢"), Log.Received.Num(), 640);
            Receive(Scope, {Event(1), Event(640)}); TestEqual(TEXT("有界窗口外旧身份仍不可重播"), Log.Received.Num(), 640); return true;
        }
        if (Case == TEXT("Receive.OutOfOrder"))
        { Receive(Scope, {Event(3), Event(1), Event(2), Event(3)}); TestEqual(TEXT("窗口内乱序各一次"), Log.Received.Num(), 3); return true; }
        Receive(Scope, {Event(1)}); TestEqual(TEXT("合法首事件实际接收"), Log.Received.Num(), 1);
        if (Case == TEXT("Receive.Duplicate")) { Receive(Scope, {Event(1), Event(2), Event(2)}); TestEqual(TEXT("两身份各播放一次"), Log.Received.Num(), 2); }
        if (Case == TEXT("Receive.OldRegion") || Case == TEXT("Receive.OldWorld") || Case == TEXT("Receive.OldControl"))
        {
            const auto OldScope = Scope; const auto OldEvent = Event(2);
            ++Scope.Generation;
            if (Case == TEXT("Receive.OldWorld")) { Scope.WorldEpoch = FGuid::NewGuid(); } else { Scope.RegionId = 9; }
            SetScope(Scope); SetScope(OldScope); Receive(OldScope, {OldEvent});
            TestEqual(TEXT("旧图/旧区/旧控制不恢复播放"), Log.Received.Num(), 1);
            Receive(Scope, {Event(1)}); TestEqual(TEXT("新区域代次可接收"), Log.Received.Num(), 2);
        }
        return true;
    }
    const bool Main = Case == TEXT("MainBoth") || Case == TEXT("MainExcludesTraining");
    TArray<AY01Monster*> Monsters;
    if (Main)
    {
        TestTrue(TEXT("真实主线开波"), F.Mode->TryStartNextWave()); F.Tick(.05f);
        for (AY01Monster* M : F.Mode->GetWaveMonsters()) { Monsters.Add(M); M->GetCharacterMovement()->DisableMovement(); }
        if (!TestTrue(TEXT("主线有怪"), !Monsters.IsEmpty())) { return false; }
        F.A->GetPawn()->SetActorLocation(Monsters[0]->GetActorLocation() + FVector(100,0,0));
        F.B->GetPawn()->SetActorLocation(Monsters[0]->GetActorLocation() + FVector(-100,0,0));
        if (Case == TEXT("MainExcludesTraining")) { F.Training(F.B); }
    }
    else { Monsters = F.Training(F.A); }
    if (!TestTrue(TEXT("真实目标存在"), !Monsters.IsEmpty())) { return false; }
    auto* M = Monsters[0]; auto* T = M->GetCombatASC(); auto* S = F.ASC();
    auto SentTo = [&](AY01PlayerController* PC)
    { return Log.Sent.FilterByPredicate([&](const FString& Line) { return FDamageLog::Field(Line, TEXT("Controller")) == PC->GetName(); }).Num(); };
    if (Case == TEXT("TrainingBoth") || Case == TEXT("CrossRegion"))
    {
        auto Other = F.Training(F.B); if (!TestEqual(TEXT("B房独立怪"), Other.Num(), 2)) { return false; }
        if (Case == TEXT("CrossRegion"))
        {
            S->TargetPermission = nullptr; Other[0]->GetCombatASC()->TargetPermission = nullptr;
            FDamageWorld::Untagged(S, Other[0]->GetCombatASC(), -1, true);
            F.Tick(.1f); TestEqual(TEXT("跨房身份不得回退广播"), Log.Sent.Num(), 0); F.Mode->GetTraining()->BindCombat(S);
        }
        else
        {
            FDamageWorld::Damage(F.PS(F.B)->GetCombatASC(), Other[0]->GetCombatASC(), 20);
            FDamageWorld::Damage(Other[0]->GetCombatASC(), F.PS(F.B)->GetCombatASC(), 20);
        }
    }
    if (Case == TEXT("Unknown"))
    {
        auto* Unknown = F.World->SpawnActor<AY01Monster>(); FString Error;
        FY01DataRef Ref; Ref.Type = EY01TableKind::Monster; Ref.RowName = TEXT("Default");
        TestTrue(TEXT("合法ASC但不属于已注册区域"), Unknown->TryInitializeMonster(F.Profile->BuildCatalog(), Ref, Error));
        S->TargetPermission = nullptr; FDamageWorld::Untagged(S, Unknown->GetCombatASC(), -1, true);
        F.Tick(.1f); TestEqual(TEXT("未知怪身份不广播"), Log.Sent.Num(), 0); F.Mode->GetTraining()->BindCombat(S);
    }
    if (Case == TEXT("DeathSnapshot") || Case == TEXT("DeathSessionSnapshot"))
    {
        T->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 8);
        if (Case == TEXT("DeathSessionSnapshot")) { T->CombatSessionInvalidated.AddLambda([&]() { M->Destroy(); }); }
        else { T->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda([&](const FOnAttributeChangeData& Data)
            { if (Data.NewValue <= 0) { M->Destroy(); F.A->GetPawn()->Destroy(); } }); }
    }
    if (Case.StartsWith(TEXT("Switch")) || Case.StartsWith(TEXT("Logout")))
    {
        FDamageWorld::Damage(S, T, 1); F.Tick(.1f);
        TestEqual(TEXT("清空用例也必须有可达的正向路由"), SentTo(F.A), 1);
        Log.Sent.Reset(); Log.Received.Reset(); Log.Batches.Reset();
    }
    if (Case == TEXT("SwitchDuringDamage") || Case == TEXT("LogoutDuringDamage"))
    {
        T->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda([&](const FOnAttributeChangeData& Data)
        { if (Data.NewValue < Data.OldValue) { if (Case == TEXT("SwitchDuringDamage")) { F.Mode->GetTraining()->Request(F.A, false); } else { F.Mode->Logout(F.A); } } });
    }
    const int32 Hits = Case == TEXT("QueueOldest") ? 160 : Case == TEXT("RateLimit") ? 100 : 1;
    for (int32 I=0; I<Hits; ++I) { FDamageWorld::Damage(S, T, Hits > 1 ? 1 : 20); if (Case == TEXT("RateLimit")) { F.Tick(.01f); } }
    if (Main || Case == TEXT("TrainingBoth")) { FDamageWorld::Damage(T, S, 20); }
    if (Case == TEXT("SwitchPending")) { F.Mode->GetTraining()->Request(F.A, false); }
    if (Case == TEXT("LogoutPending")) { F.Mode->Logout(F.A); }
    F.Tick(.2f);
    const bool Cleared = Case.StartsWith(TEXT("Switch")) || Case.StartsWith(TEXT("Logout"));
    const int32 ExpectedA = Cleared ? 0 : Case == TEXT("QueueOldest") ? 128 : Case == TEXT("RateLimit") ? 100 : Main || Case == TEXT("TrainingBoth") ? 2 : 1;
    TestEqual(TEXT("A只接收当时合法区域事件"), SentTo(F.A), ExpectedA);
    TestEqual(TEXT("B主线共享、训练只属自己"), SentTo(F.B), Case == TEXT("MainBoth") || Case == TEXT("TrainingBoth") ? 2 : 0);
    TestEqual(TEXT("本地实际消费一次不漏重"), Log.Received.Num(), Log.Sent.Num());
    if (Case == TEXT("QueueOldest") && TestEqual(TEXT("限容128"), Log.Sent.Num(), 128))
    { TestEqual(TEXT("超限丢最旧32条"), FDamageLog::Number(Log.Sent[0], TEXT("Id")), 33.0); }
    if (Case == TEXT("RateLimit"))
    {
        TestTrue(TEXT("1.2秒不超过24批"), Log.Batches.Num() <= 24);
        for (int32 I=1; I<Log.Batches.Num(); ++I)
        { TestTrue(TEXT("相邻批间隔至少50ms"), FDamageLog::Number(Log.Batches[I], TEXT("Time")) - FDamageLog::Number(Log.Batches[I-1], TEXT("Time")) >= .049999); }
    }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DamageLifecycle, "Y01.Playground.Damage.Lifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01DamageLifecycle::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("TravelClient"), TEXT("TravelQueue"), TEXT("TravelSeamless"), TEXT("WorldEnd"), TEXT("AutomaticReturn")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01DamageLifecycle::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FDamageWorld F; FDamageLog Log; F.Start();
    F.A->Player = NewObject<ULocalPlayer>(GEngine); F.B->Player = NewObject<ULocalPlayer>(GEngine);
    auto Monsters = F.Training(F.A); if (!TestEqual(TEXT("生命周期夹具"), Monsters.Num(), 2)) { return false; }
    auto* S = F.ASC(); auto* T = Monsters[0]->GetCombatASC();
    FDamageWorld::Damage(S, T, 1); F.Tick(.1f);
    TestEqual(TEXT("生命周期正向对照"), Log.Received.Num(), 1);
    const auto Old = F.A->GetDamageAudience();
    FY01DamageEvent E; E.WorldEpoch = Old.WorldEpoch; E.EventId = 999; E.RegionId = Old.RegionId; E.ActualDamage = 1; E.DisplayAmount = 1;
    if (Case == TEXT("WorldEnd"))
    {
        auto* Service = F.World->GetSubsystem<UY01DamageEvents>(); auto Pending = Service->Capture(S, T);
        Pending.Event.ActualDamage = 1; Pending.Event.DisplayAmount = 1;
        F.World->EndPlay(EEndPlayReason::LevelTransition);
        const int32 Before = Log.Commits.Num(); Service->Commit(Pending);
        TestFalse(TEXT("World结束立刻使Epoch失效"), Service->GetWorldEpoch().IsValid());
        TestEqual(TEXT("世界结束不再发布迟到GE"), Log.Commits.Num(), Before); return true;
    }
    if (Case == TEXT("AutomaticReturn"))
    {
        F.Mode->GetTraining()->StopAll();
        TestFalse(TEXT("真实训练自动返回"), F.Mode->GetTraining()->Observe(F.PS(F.A)).bInside);
        TestEqual(TEXT("自动返回也更新接收区域"), F.A->GetDamageAudience().RegionId, 0);
        TestTrue(TEXT("自动返回代次递增"), F.A->GetDamageAudience().Generation > Old.Generation);
    }
    else
    {
        FDamageWorld::Damage(S, T, 1);
        int32 Resets = 0; auto Handle = F.A->OnDamageDisplayReset.AddLambda([&]() { ++Resets; });
        if (Case == TEXT("TravelSeamless")) { F.A->PostSeamlessTravel(); }
        else { F.A->PreClientTravel(TEXT("/Game/Y01Playground/Maps/L_Y01Playground"), TRAVEL_Relative, true); }
        TestEqual(TEXT("Travel通知未来池清理"), Resets, 1);
        F.A->OnDamageDisplayReset.Remove(Handle);
        if (Case == TEXT("TravelQueue") || Case == TEXT("TravelSeamless")) { F.Tick(.1f); TestEqual(TEXT("Travel清待发队列"), Log.Sent.Num(), 1); }
    }
    auto Scope = Old; ++Scope.Generation;
    F.A->ProcessEvent(F.A->FindFunctionChecked(TEXT("ClientSetDamageAudience")), &Scope);
    struct FParams { FY01DamageAudience Audience; TArray<FY01DamageEvent> Events; } Params{Scope, {E}};
    F.A->ProcessEvent(F.A->FindFunctionChecked(TEXT("ClientDamageBatch")), &Params);
    TestEqual(TEXT("切图或已返回不接受旧包"), Log.Received.Num(), 1);
    if (Case == TEXT("TravelSeamless"))
    {
        F.A->SetDamageRegion(0);
        TestFalse(TEXT("旧World不能重新启用服务器区域"), F.A->GetDamageAudience().WorldEpoch.IsValid());
        Scope.WorldEpoch = FGuid::NewGuid(); Scope.Generation = Old.Generation + 2;
        F.A->ProcessEvent(F.A->FindFunctionChecked(TEXT("ClientSetDamageAudience")), &Scope);
        E.WorldEpoch = Scope.WorldEpoch; E.EventId = 1; Params = {Scope, {E}};
        F.A->ProcessEvent(F.A->FindFunctionChecked(TEXT("ClientDamageBatch")), &Params);
        TestEqual(TEXT("seamless收尾后允许新Epoch重新消费"), Log.Received.Num(), 2);
    }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DamageFatalReturn, "Y01.Playground.Damage.FatalReturn", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01DamageFatalReturn::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("BeforeCommit"), TEXT("Pending"), TEXT("AlreadyReceived")}) { Names.Add(N); Commands.Add(N); }
}
bool FY01DamageFatalReturn::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FDamageWorld F; FDamageLog Log; F.Start();
    F.A->Player = NewObject<ULocalPlayer>(GEngine); F.B->Player = NewObject<ULocalPlayer>(GEngine);
    const FVector ReturnPosition = F.A->GetPawn()->GetActorLocation();
    auto Monsters = F.Training(F.A); F.Training(F.B);
    if (!TestEqual(TEXT("致命一击真实训练夹具"), Monsters.Num(), 2)) { return false; }
    auto* PlayerASC = F.ASC(); auto* MonsterASC = Monsters[0]->GetCombatASC();
    PlayerASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 8);
    PlayerASC->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), 7);
    const FVector HitPosition = F.A->GetPawn()->GetActorLocation();
    const auto OldAudience = F.A->GetDamageAudience();
    const int32 Gold = F.PS(F.A)->GetGold(), XP = F.PS(F.A)->GetExperience();
    TArray<FY01DamageEvent> Received; int32 OtherReceived = 0;
    auto H = F.A->OnDamageReceived.AddLambda([&](const FY01DamageEvent& E) { Received.Add(E); });
    auto HB = F.B->OnDamageReceived.AddLambda([&](const FY01DamageEvent&) { ++OtherReceived; });
    // 使用原自动返回入口，覆盖Health广播内重入和批次发送前后的实际顺序。
    auto HealthHandle = PlayerASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda(
        [&](const FOnAttributeChangeData& Data)
        { if (Case == TEXT("BeforeCommit") && Data.OldValue > 0 && Data.NewValue <= 0) { F.Mode->Tick(0); } });
    FDamageWorld::Damage(MonsterASC, PlayerASC, 20);
    PlayerASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).Remove(HealthHandle);
    if (Case == TEXT("AlreadyReceived")) { F.A->Tick(0); TestEqual(TEXT("切区前已接收致命事件"), Received.Num(), 1); }
    F.Mode->Tick(0);
    TestEqual(TEXT("护甲及过量伤害后生命归零"), FDamageWorld::HP(PlayerASC), 0.f);
    TestFalse(TEXT("仍由原规则自动返回主线"), F.Mode->GetTraining()->Observe(F.PS(F.A)).bInside);
    TestTrue(TEXT("仍传送至原返回位置"), F.A->GetPawn()->GetActorLocation().Equals(ReturnPosition));
    TestEqual(TEXT("显示区域立即切主线"), F.A->GetDamageAudience().RegionId, 0);
    TestTrue(TEXT("切区代次仍递增"), F.A->GetDamageAudience().Generation > OldAudience.Generation);
    TestEqual(TEXT("不改变金币"), F.PS(F.A)->GetGold(), Gold); TestEqual(TEXT("不改变经验"), F.PS(F.A)->GetExperience(), XP);
    F.Tick(.1f);
    if (TestEqual(TEXT("自动返回的最后有效伤害仍且只消费一次"), Received.Num(), 1))
    {
        TestEqual(TEXT("最后一击实际生命减少8"), Received[0].ActualDamage, 8.f);
        TestEqual(TEXT("最后一击显示扣甲后的13"), Received[0].DisplayAmount, 13.f);
        TestEqual(TEXT("最后一击仍为伤害类型"), Received[0].Type, EY01DamageEventType::Damage);
        TestEqual(TEXT("保留真实受击快照而非返回位置"), Received[0].WorldPosition, HitPosition);
        TestEqual(TEXT("快照仍属于受击训练房"), Received[0].RegionId, OldAudience.RegionId);
    }
    TestEqual(TEXT("另一训练房完全隔离"), OtherReceived, 0);
    FY01DamageEvent Late; Late.WorldEpoch = OldAudience.WorldEpoch; Late.RegionId = OldAudience.RegionId; Late.EventId = 999;
    Late.ActualDamage = 1; Late.DisplayAmount = 1;
    struct FParams { FY01DamageAudience Audience; TArray<FY01DamageEvent> Events; } Params{OldAudience, {Late}};
    F.A->ProcessEvent(F.A->FindFunctionChecked(TEXT("ClientDamageBatch")), &Params);
    Params.Audience = F.A->GetDamageAudience(); F.A->ProcessEvent(F.A->FindFunctionChecked(TEXT("ClientDamageBatch")), &Params);
    TestEqual(TEXT("不能借致命交接接收其他旧区事件"), Received.Num(), 1);
    F.A->OnDamageReceived.Remove(H); F.B->OnDamageReceived.Remove(HB);
    return true;
}
namespace
{
struct FNumberProbe
{
    static UActorComponent* Pool(AY01PlayerController* PC)
    {
        for (UActorComponent* C : PC->GetComponents())
        { if (C && C->GetClass()->GetName() == TEXT("Y01DamageNumberPoolComponent") && !C->IsBeingDestroyed()) { return C; } }
        return nullptr;
    }
    static TArray<UWidgetComponent*> Entries(UWorld* World, AY01PlayerController* PC, bool ActiveOnly = false)
    {
        TArray<UWidgetComponent*> Result;
        for (TActorIterator<AActor> It(World); It; ++It)
        {
            if (It->IsActorBeingDestroyed()) { continue; }
            if (auto* W = It->FindComponentByClass<UWidgetComponent>(); W && W->GetClass()->GetName() == TEXT("Y01DamageWorldWidgetComponent") && W->GetOwnerPlayer()
                && W->GetOwnerPlayer()->PlayerController == PC && (!ActiveOnly || (W->IsVisible() && !It->IsHidden()))) { Result.Add(W); }
        }
        return Result;
    }
    static UTextBlock* Text(UWidgetComponent* C)
    {
        auto* W = C ? C->GetUserWidgetObject() : nullptr;
        if (!W || !W->WidgetTree) { return nullptr; }
        UTextBlock* Result = nullptr;
        W->WidgetTree->ForEachWidget([&](UWidget* Child) { if (auto* T = Cast<UTextBlock>(Child)) { Result = T; } });
        return Result;
    }
    static void Send(AY01PlayerController* PC, uint64 Id, float Amount = 7, FVector Position = FVector(100,200,300),
        EY01DamageEventType Type = EY01DamageEventType::Damage)
    {
        const auto Scope = PC->GetDamageAudience();
        FY01DamageEvent E; E.WorldEpoch = Scope.WorldEpoch; E.RegionId = Scope.RegionId; E.EventId = Id;
        E.ActualDamage = Type == EY01DamageEventType::Damage ? Amount : 0; E.DisplayAmount = Amount; E.Type = Type; E.WorldPosition = Position;
        struct FParams { FY01DamageAudience Audience; TArray<FY01DamageEvent> Events; } Params{Scope, {E}};
        PC->ProcessEvent(PC->FindFunctionChecked(TEXT("ClientDamageBatch")), &Params);
    }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DamageNumberPoolTest, "Y01.Playground.Damage.Numbers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01DamageNumberPoolTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Prewarm32"), TEXT("Capacity129"), TEXT("ReuseIdentity"), TEXT("AnimationReset"), TEXT("Billboard"), TEXT("ViewVisibility"),
        TEXT("Format.Integer"), TEXT("Format.Decimal"), TEXT("Format.Tiny"), TEXT("Format.Rounding"), TEXT("ZeroInvalid"),
        TEXT("RegionReset"), TEXT("Travel"), TEXT("SeamlessWorld"), TEXT("WorldEnd"), TEXT("EndPlay"), TEXT("DedicatedServer"), TEXT("Remote"), TEXT("Disabled"),
        TEXT("TargetDestroyed"), TEXT("Fatal.BeforeCommit"), TEXT("Fatal.Pending"), TEXT("Fatal.AlreadyReceived")})
    { Names.Add(N); Commands.Add(N); }
    for (const TCHAR* N : {TEXT("ReuseColor"), TEXT("PresentationConfig"), TEXT("PresentationFallback")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01DamageNumberPoolTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FDamageWorld F;
    F.A->Player = NewObject<ULocalPlayer>(GEngine); F.A->GetLocalPlayer()->PlayerController = F.A;
    F.B->Player = NewObject<ULocalPlayer>(GEngine); F.B->GetLocalPlayer()->PlayerController = F.B;
    const FLinearColor ConfigDamage(.2f, .3f, .8f, 1), ConfigHealing(.9f, .6f, .1f, 1);
    if (Case == TEXT("PresentationConfig"))
    {
        auto* Rules = F.Profile->Tables[EY01TableKind::MatchRules]->FindRow<FY01MatchRulesRow>(TEXT("Default"), TEXT("Presentation"));
        Rules->Presentation.DamageColor = ConfigDamage; Rules->Presentation.HealingColor = ConfigHealing; Rules->Presentation.RegenMergeSeconds = .4;
    }
    if (Case == TEXT("DedicatedServer")) { F.World->WorldType = EWorldType::PIE; F.World->SetPlayInEditorInitialNetMode(NM_DedicatedServer); }
    if (Case == TEXT("Remote")) { F.A->Player = nullptr; }
    F.Start(); F.A->SetDamageRegion(0); F.B->SetDamageRegion(0);
    auto* PresentationProperty = FindFProperty<FStructProperty>(AY01GameState::StaticClass(), TEXT("Presentation"));
    if (Case == TEXT("PresentationConfig"))
    {
        const auto& Presentation = F.Mode->GetFlowState()->GetPresentation();
        TestTrue(TEXT("GameState配置带正版本"), Presentation.Revision > 0);
        TestEqual(TEXT("GameState复制伤害色"), Presentation.Values.DamageColor, ConfigDamage);
        TestEqual(TEXT("GameState复制治疗色"), Presentation.Values.HealingColor, ConfigHealing);
        TestEqual(TEXT("GameState复制回血合并秒数"), Presentation.Values.RegenMergeSeconds, .4);
        TestTrue(TEXT("Presentation是网络复制属性"), PresentationProperty && PresentationProperty->HasAnyPropertyFlags(CPF_Net));
    }
    if (Case == TEXT("PresentationFallback") && PresentationProperty)
    {
        auto* Presentation = PresentationProperty->ContainerPtrToValuePtr<FY01ReplicatedPresentation>(F.Mode->GetFlowState());
        Presentation->Values.DamageColor = ConfigDamage; Presentation->Values.HealingColor = ConfigHealing; Presentation->Revision = 0;
    }
    // 按运行时反射查找，类尚未实现时也必须编译成功，并真实执行出红。
    if (!TestNotNull(TEXT("程序自有可运行的数字池类型"), FindObject<UClass>(nullptr, TEXT("/Script/Y01.Y01DamageNumberPoolComponent")))) { return false; }
    if (Case == TEXT("DedicatedServer") || Case == TEXT("Remote") || Case == TEXT("Disabled"))
    {
        AY01PlayerController* PC = F.A;
        if (Case == TEXT("DedicatedServer")) { TestEqual(TEXT("真实World报告DS模式"), F.World->GetNetMode(), NM_DedicatedServer); }
        if (Case == TEXT("Disabled")) { PC = F.World->SpawnActor<AY01PlayerController>(); PC->Player = NewObject<ULocalPlayer>(GEngine); }
        PC->Tick(0); FNumberProbe::Send(PC, 1);
        TestNull(TEXT("DS/非本地/未启用Controller根本没有池实例"), FNumberProbe::Pool(PC));
        TestEqual(TEXT("DS/非本地/未启用Controller没有显示Actor或Widget"), FNumberProbe::Entries(F.World, PC).Num(), 0);
        if (Case == TEXT("DedicatedServer"))
        {
            int32 Committed = 0; auto* Service = F.World->GetSubsystem<UY01DamageEvents>();
            auto Handle = Service->OnDamageCommitted.AddLambda([&](const FY01DamageEvent&) { ++Committed; });
            auto Monsters = F.Training(F.A);
            if (TestEqual(TEXT("DS实际训练目标"), Monsters.Num(), 2)) { FDamageWorld::Damage(F.ASC(), Monsters[0]->GetCombatASC(), 7); }
            TestEqual(TEXT("DS仍发布实际伤害事件"), Committed, 1); Service->OnDamageCommitted.Remove(Handle);
        }
        return true;
    }
    auto* Pool = FNumberProbe::Pool(F.A);
    if (!TestNotNull(TEXT("BeginPlay即建立本地池而非首击临时创建"), Pool)) { return false; }
    auto Entries = FNumberProbe::Entries(F.World, F.A);
    if (!TestEqual(TEXT("默认预热32个真实条目"), Entries.Num(), 32)) { return false; }
    TestFalse(TEXT("池不复制"), Pool->GetIsReplicated());
    auto Idle = [&](UWidgetComponent* W)
    {
        TestTrue(TEXT("闲置Actor隐藏"), W->GetOwner()->IsHidden()); TestFalse(TEXT("闲置组件不可见"), W->IsVisible());
        TestFalse(TEXT("条目Actor不单独Tick"), W->GetOwner()->PrimaryActorTick.bCanEverTick);
        TestFalse(TEXT("条目WidgetComponent不单独Tick"), W->PrimaryComponentTick.bCanEverTick);
        TestTrue(TEXT("位置归零"), W->GetComponentLocation().IsNearlyZero()); TestTrue(TEXT("旋转归零"), W->GetComponentRotation().IsNearlyZero());
        if (auto* T = FNumberProbe::Text(W))
        {
            TestTrue(TEXT("归还文字为空"), T->GetText().IsEmpty());
            TestEqual(TEXT("归还颜色重置为中性白"), T->GetColorAndOpacity().GetSpecifiedColor(), FLinearColor::White);
        }
        else { AddError(TEXT("必须有真实TextBlock")); }
        if (auto* U = W->GetUserWidgetObject())
        {
            TestEqual(TEXT("归还透明度归零"), U->GetRenderOpacity(), 0.f);
            TestEqual(TEXT("归还Widget隐藏"), U->GetVisibility(), ESlateVisibility::Hidden);
            const auto Transform = U->GetRenderTransform();
            TestTrue(TEXT("归还动画变换完整归零"), Transform.Translation.IsNearlyZero() && Transform.Shear.IsNearlyZero()
                && Transform.Scale.Equals(FVector2D(1,1)) && FMath::IsNearlyZero(Transform.Angle));
        }
        else { AddError(TEXT("预热就必须有真实UUserWidget")); }
    };
    for (auto* W : Entries)
    {
        TestEqual(TEXT("只能World Space"), W->GetWidgetSpace(), EWidgetSpace::World);
        TestFalse(TEXT("条目Actor不复制"), W->GetOwner()->GetIsReplicated());
        TestEqual(TEXT("显示所属本地玩家"), W->GetOwnerPlayer(), F.A->GetLocalPlayer());
        TestTrue(TEXT("分屏世界可见性只属于本地拥有者"), W->bOnlyOwnerSee);
        TestEqual(TEXT("数字不干扰碰撞"), W->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
        auto* Material = W->GetMaterial(0);
        if (TestNotNull(TEXT("使用引擎世界Widget材质"), Material)) { TestFalse(TEXT("保留正常世界深度遮挡"), Material->GetMaterial()->bDisableDepthTest); }
        Idle(W);
    }
    if (Case == TEXT("Prewarm32"))
    {
        const auto Other = FNumberProbe::Entries(F.World, F.B); TestEqual(TEXT("每本地Controller独立预热"), Other.Num(), 32);
        for (auto* W : Entries) { TestFalse(TEXT("两个池不共享条目"), Other.Contains(W)); } return true;
    }
    if (Case.StartsWith(TEXT("Fatal.")))
    {
        auto Monsters = F.Training(F.A); if (!TestEqual(TEXT("致命数字真实目标"), Monsters.Num(), 2)) { return false; }
        auto* PlayerASC = F.ASC(); PlayerASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 8);
        PlayerASC->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), 7);
        const FVector HitPosition = F.A->GetPawn()->GetActorLocation();
        auto H = PlayerASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda([&](const FOnAttributeChangeData& D)
        { if (Case.EndsWith(TEXT("BeforeCommit")) && D.NewValue <= 0) { F.Mode->Tick(0); } });
        FDamageWorld::Damage(Monsters[0]->GetCombatASC(), PlayerASC, 20);
        PlayerASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).Remove(H);
        UWidgetComponent* Before = nullptr;
        const float Played = Case.EndsWith(TEXT("AlreadyReceived")) ? .3f : 0.f;
        if (Played > 0)
        {
            F.A->Tick(0); auto Active = FNumberProbe::Entries(F.World, F.A, true);
            if (TestEqual(TEXT("切区前致命数字已出现"), Active.Num(), 1))
            {
                Before = Active[0];
                TestTrue(TEXT("已接收致命数字从快照Z加100开始"), Before->GetComponentLocation().Equals(HitPosition + FVector(0,0,100)));
                Pool->TickComponent(Played, LEVELTICK_All, &Pool->PrimaryComponentTick);
            }
        }
        F.Mode->Tick(0); F.A->Tick(0);
        auto Active = FNumberProbe::Entries(F.World, F.A, true);
        if (!TestEqual(TEXT("自动返回后仍显示最后一击"), Active.Num(), 1)) { return false; }
        auto* W = Active[0]; if (Before) { TestEqual(TEXT("切区不能重新借出或重置寿命"), W, Before); }
        TestEqual(TEXT("致命过量按扣甲后展示量显示"), FNumberProbe::Text(W)->GetText().ToString(), FString(TEXT("13")));
        TestTrue(TEXT("保持受击快照及已播放上浮距离而非追踪Pawn"), W->GetComponentLocation().Equals(HitPosition + FVector(0,0,100 + 60 * Played / .8f), .01));
        TestTrue(TEXT("切区不恢复已经淡出的透明度"), FMath::IsNearlyEqual(W->GetUserWidgetObject()->GetRenderOpacity(), 1.f - Played / .8f, .01f));
        F.Tick(.4f - Played); TestTrue(TEXT("致命数字到半寿命仍存在"), W->IsVisible());
        TestTrue(TEXT("致命数字正常淡出"), FMath::IsNearlyEqual(W->GetUserWidgetObject()->GetRenderOpacity(), .5f, .02f));
        F.Tick(.41f); Idle(W); return true;
    }
    if (Case == TEXT("ZeroInvalid"))
    {
        for (float Value : {0.f, -1.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) { FNumberProbe::Send(F.A, 1, Value); }
        TestEqual(TEXT("零及非法伤害不借条目"), FNumberProbe::Entries(F.World, F.A, true).Num(), 0);
    }
    const float Value = Case == TEXT("Format.Decimal") ? 12.34f : Case == TEXT("Format.Tiny") ? .03f : Case == TEXT("Format.Rounding") ? 12.96f : 7.f;
    FNumberProbe::Send(F.A, 1, Value);
    auto Active = FNumberProbe::Entries(F.World, F.A, true);
    if (!TestEqual(TEXT("合法事件激活一个真实条目"), Active.Num(), 1)) { return false; }
    auto* First = Active[0]; auto* Text = FNumberProbe::Text(First); auto* Widget = First->GetUserWidgetObject();
    if (!TestNotNull(TEXT("实际原生TextBlock"), Text) || !TestNotNull(TEXT("实际原生UUserWidget"), Widget)) { return false; }
    TestEqual(TEXT("伤害数字使用默认或复制配置色"), Text->GetColorAndOpacity().GetSpecifiedColor(),
        Case == TEXT("PresentationConfig") ? ConfigDamage : FLinearColor::Red);
    TestTrue(TEXT("使用引擎字体不增加资产"), Text->GetFont().FontObject && Text->GetFont().FontObject->GetPathName().StartsWith(TEXT("/Engine/")));
    TestFalse(TEXT("UUserWidget无独立Slate Tick"), Widget->TakeWidget()->GetCanTick());
    TestEqual(TEXT("其他本地玩家不串池"), FNumberProbe::Entries(F.World, F.B, true).Num(), 0);
    if (Case.StartsWith(TEXT("Format.")))
    {
        const FString Expected = Case == TEXT("Format.Decimal") ? TEXT("12") : Case == TEXT("Format.Tiny") ? TEXT("<1") : Case == TEXT("Format.Rounding") ? TEXT("12") : TEXT("7");
        TestEqual(TEXT("向零截断去小数不四舍五入，不足1的正值显示<1"), Text->GetText().ToString(), Expected); return true;
    }
    if (Case == TEXT("PresentationConfig") || Case == TEXT("PresentationFallback"))
    {
        F.Tick(.81f); Idle(First);
        FNumberProbe::Send(F.A, 2, 9, FVector(10,20,30), EY01DamageEventType::Healing);
        auto Healing = FNumberProbe::Entries(F.World, F.A, true);
        if (!TestEqual(TEXT("治疗配置验证激活一个条目"), Healing.Num(), 1)) { return false; }
        TestEqual(TEXT("治疗数字使用默认或复制配置色"), FNumberProbe::Text(Healing[0])->GetColorAndOpacity().GetSpecifiedColor(),
            Case == TEXT("PresentationConfig") ? ConfigHealing : FLinearColor::Green);
        return true;
    }
    if (Case == TEXT("ReuseColor"))
    {
        F.Tick(.81f); Idle(First);
        FNumberProbe::Send(F.A, 2, 9, FVector(10,20,30), EY01DamageEventType::Healing);
        auto Healing = FNumberProbe::Entries(F.World, F.A, true);
        if (!TestEqual(TEXT("治疗复用同一空闲条目"), Healing.Num(), 1)) { return false; }
        TestEqual(TEXT("治疗复用不串伤害红色"), FNumberProbe::Text(Healing[0])->GetColorAndOpacity().GetSpecifiedColor(), FLinearColor::Green);
        F.Tick(.81f); Idle(Healing[0]);
        FNumberProbe::Send(F.A, 3, 11, FVector(10,20,30), EY01DamageEventType::Damage);
        auto Damage = FNumberProbe::Entries(F.World, F.A, true);
        if (!TestEqual(TEXT("再次伤害复用同一空闲条目"), Damage.Num(), 1)) { return false; }
        TestEqual(TEXT("红绿红复用不串治疗绿色"), FNumberProbe::Text(Damage[0])->GetColorAndOpacity().GetSpecifiedColor(), FLinearColor::Red);
        return true;
    }
    if (Case == TEXT("Capacity129"))
    {
        for (uint64 Id = 2; Id <= 128; ++Id) { FNumberProbe::Send(F.A, Id, static_cast<float>(Id)); }
        auto Full = FNumberProbe::Entries(F.World, F.A); TestEqual(TEXT("128个真实条目硬上限"), Full.Num(), 128);
        TestEqual(TEXT("128条全在使用"), FNumberProbe::Entries(F.World, F.A, true).Num(), 128);
        FNumberProbe::Send(F.A, 129, 129);
        TestEqual(TEXT("第129不新建"), FNumberProbe::Entries(F.World, F.A).Num(), 128);
        TestEqual(TEXT("第129精确复用最旧条目"), FNumberProbe::Text(First)->GetText().ToString(), FString(TEXT("129")));
        for (auto* W : Full) { TestTrue(TEXT("达到上限后对象身份稳定"), FNumberProbe::Entries(F.World, F.A).Contains(W)); } return true;
    }
    if (Case == TEXT("RegionReset"))
    {
        F.A->SetDamageRegion(4); TestEqual(TEXT("普通切区立即归还旧数字"), FNumberProbe::Entries(F.World, F.A, true).Num(), 0);
        Idle(First); TestEqual(TEXT("普通切区不销毁重建预热条目"), FNumberProbe::Entries(F.World, F.A).Num(), 32); return true;
    }
    if (Case == TEXT("Travel") || Case == TEXT("SeamlessWorld") || Case == TEXT("WorldEnd") || Case == TEXT("EndPlay"))
    {
        if (Case == TEXT("EndPlay")) { F.A->RouteEndPlay(EEndPlayReason::LevelTransition); F.A->RouteEndPlay(EEndPlayReason::LevelTransition); }
        else if (Case == TEXT("WorldEnd")) { F.World->EndPlay(EEndPlayReason::LevelTransition); }
        else { F.A->PreClientTravel(TEXT("/Game/Y01Playground/Maps/L_Y01Playground"), TRAVEL_Relative, true); F.A->PreClientTravel(TEXT("/Game/Y01Playground/Maps/L_Y01Playground"), TRAVEL_Relative, true); }
        for (auto* W : Entries) { TestTrue(TEXT("地图或结束释放所有条目Actor"), W->GetOwner()->IsActorBeingDestroyed()); TestNull(TEXT("释放Widget资源"), W->GetUserWidgetObject()); }
        F.A->Tick(0); TestEqual(TEXT("旧地图不能重新预热"), FNumberProbe::Entries(F.World, F.A).Num(), 0);
        if (Case == TEXT("SeamlessWorld"))
        {
            FDamageWorld Next; Next.Start(); auto* Kept = F.A; Kept->UnPossess(); Kept->PlayerState = nullptr;
            F.World->RemoveController(Kept); Kept->Rename(nullptr, Next.World->PersistentLevel); Next.World->AddController(Kept);
            Kept->PostSeamlessTravel(); Kept->Tick(0); Kept->SetDamageRegion(0);
            TestEqual(TEXT("真实迁移后Controller身份仍相同"), Kept, F.A); TestEqual(TEXT("Controller已属于新World"), Kept->GetWorld(), Next.World);
            TestEqual(TEXT("seamless保留Controller在新World重新预热"), FNumberProbe::Entries(Next.World, Kept).Num(), 32);
            FNumberProbe::Send(Kept, 1); TestEqual(TEXT("新地图正常播放"), FNumberProbe::Entries(Next.World, Kept, true).Num(), 1);
        }
        return true;
    }
    if (Case == TEXT("TargetDestroyed"))
    {
        F.A->SetDamageRegion(0); F.A->OnDamageDisplayReset.Broadcast();
        auto Monsters = F.Training(F.A); auto* M = Monsters[0]; const FVector Hit = M->GetActorLocation();
        M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 8);
        FDamageWorld::Damage(F.ASC(), M->GetCombatASC(), 20); M->Destroy(); F.A->Tick(0);
        auto Live = FNumberProbe::Entries(F.World, F.A, true);
        if (TestEqual(TEXT("怪物销毁后数字照常出现"), Live.Num(), 1))
        { TestTrue(TEXT("数字不附着受击Actor"), Live[0]->GetComponentLocation().Equals(Hit + FVector(0,0,100))); F.Tick(.4f); TestTrue(TEXT("销毁不提前终止数字"), Live[0]->IsVisible()); }
        return true;
    }
    if (Case == TEXT("ViewVisibility"))
    {
        auto CheckView = [&]()
        {
            const FPrimitiveSceneProxyDesc Desc(First);
            TestTrue(TEXT("拥有者当前实际视角不会被OnlyOwnerSee剔除"), FSceneViewOwner(F.A->GetViewTarget()).IsPartOf(Desc.ActorOwners));
            TestFalse(TEXT("其他本地玩家视角不能看到本池数字"), FSceneViewOwner(F.B->GetViewTarget()).IsPartOf(Desc.ActorOwners));
        };
        CheckView();
        auto* Camera = F.World->SpawnActor<AActor>(FVector(500,200,800), FRotator::ZeroRotator);
        F.A->SetViewTarget(Camera); F.Tick(.01f); CheckView();
        TestTrue(TEXT("相机切换不重建条目"), FNumberProbe::Entries(F.World, F.A, true).Contains(First));
        Camera->Destroy(); F.A->SetViewTarget(F.A->GetPawn()); F.Tick(.01f); CheckView();
        TestTrue(TEXT("视角Actor销毁不销毁数字"), First->IsVisible() && !First->GetOwner()->IsActorBeingDestroyed());
        return true;
    }
    if (Case == TEXT("Billboard"))
    {
        for (const FVector Location : {FVector(1000,-600,1500), FVector(-600,1000,900)})
        {
            auto* Camera = F.World->SpawnActor<AActor>(Location, FRotator(0,25,0)); F.A->SetViewTarget(Camera);
            F.A->PlayerCameraManager->UpdateCamera(0); F.Tick(.01f);
            FVector Eye; FRotator Rotation; F.A->GetPlayerViewPoint(Eye, Rotation);
            const FVector Toward = (Eye - First->GetComponentLocation()).GetSafeNormal();
            TestTrue(TEXT("每帧朝向拥有者本地相机"), FVector::DotProduct(First->GetForwardVector(), Toward) > .999);
        }
        return true;
    }
    TestTrue(TEXT("借出位置为快照Z加100cm"), First->GetComponentLocation().Equals(FVector(100,200,400)));
    F.Tick(.4f);
    TestTrue(TEXT("半寿命上移30cm"), First->GetComponentLocation().Equals(FVector(100,200,430), .01));
    TestTrue(TEXT("半寿命透明度一半"), FMath::IsNearlyEqual(Widget->GetRenderOpacity(), .5f, .01f));
    F.Tick(.41f); Idle(First);
    if (Case == TEXT("ReuseIdentity") || Case == TEXT("AnimationReset"))
    {
        for (uint64 Cycle = 2; Cycle <= 5; ++Cycle)
        {
            FNumberProbe::Send(F.A, Cycle, 9, FVector(10,20,30)); auto Now = FNumberProbe::Entries(F.World, F.A, true);
            if (!TestEqual(TEXT("反复借出仍只有一条"), Now.Num(), 1)) { return false; }
            TestEqual(TEXT("Actor/组件身份反复借还不变"), Now[0], First); TestEqual(TEXT("UUserWidget对象身份不变"), Now[0]->GetUserWidgetObject(), Widget);
            TestEqual(TEXT("TextBlock对象身份不变"), FNumberProbe::Text(Now[0]), Text); TestEqual(TEXT("新文字替换旧文字"), Text->GetText().ToString(), FString(TEXT("9")));
            TestTrue(TEXT("借出重置位置"), First->GetComponentLocation().Equals(FVector(10,20,130))); TestEqual(TEXT("借出透明度恢复"), Widget->GetRenderOpacity(), 1.f);
            F.Tick(.79f); TestTrue(TEXT("重置计时后不到0.8秒不归还"), First->IsVisible()); F.Tick(.02f); Idle(First);
        }
        TestEqual(TEXT("反复借还不额外创建销毁"), FNumberProbe::Entries(F.World, F.A).Num(), 32);
    }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DamageChunks, "Y01.Playground.Damage.Chunks", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01DamageChunks::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Full128"), TEXT("Duplicate"), TEXT("ReenterQueue"), TEXT("Switch"), TEXT("Destroy"), TEXT("Travel"), TEXT("WorldEnd"), TEXT("FatalHandoff")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01DamageChunks::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FDamageWorld F; FDamageLog Log;
    F.World->WorldType = EWorldType::PIE; F.World->SetPlayInEditorInitialNetMode(NM_ListenServer);
    F.A->Player = NewObject<ULocalPlayer>(GEngine); F.B->Player = NewObject<ULocalPlayer>(GEngine); F.Start();
    TestEqual(TEXT("本地listen server同步消费路径"), F.World->GetNetMode(), NM_ListenServer);
    auto Monsters = F.Training(F.A); if (!TestEqual(TEXT("满载真实训练目标"), Monsters.Num(), 2)) { return false; }
    auto* S = F.ASC(); auto* T = Monsters[0]->GetCombatASC(); const auto OriginalAudience = F.A->GetDamageAudience();
    TArray<FY01DamageEvent> Received;
    bool Entered = false;
    const auto Handle = F.A->OnDamageReceived.AddLambda([&](const FY01DamageEvent& E)
    {
        Received.Add(E);
        if (Entered) { return; } Entered = true;
        if (Case == TEXT("ReenterQueue"))
        {
            FDamageWorld::Damage(S, T, 1);
            // 模拟本地消费回调跨过发送截止时间，不能嵌套冲走下一周期队列。
            TGuardValue<double> Clock(F.World->RealTimeSeconds, F.World->RealTimeSeconds + .1);
            F.A->AY01PlayerController::Tick(0);
        }
        if (Case == TEXT("Switch"))
        {
            F.Mode->GetTraining()->Request(F.A, false);
            auto Next = F.Training(F.A);
            if (TestEqual(TEXT("回调中重新进入新训练房"), Next.Num(), 2)) { FDamageWorld::Damage(S, Next[0]->GetCombatASC(), 1); }
            F.A->AY01PlayerController::Tick(0);
        }
        if (Case == TEXT("Destroy")) { F.A->Destroy(); }
        if (Case == TEXT("Travel")) { F.A->PreClientTravel(TEXT("/Game/Y01Playground/Maps/L_Y01Playground"), TRAVEL_Relative, true); }
        if (Case == TEXT("WorldEnd")) { F.World->EndPlay(EEndPlayReason::LevelTransition); }
        if (Case == TEXT("FatalHandoff")) { FDamageWorld::Damage(T, S, 2000); F.Mode->Tick(0); }
    });
    for (int32 I=0; I<128; ++I) { FDamageWorld::Damage(S, T, 1); }
    TestEqual(TEXT("满载128个合格GE已实际扣血"), FDamageWorld::HP(T), 872.f);
    TestEqual(TEXT("满载128个独立提交"), Log.Commits.Num(), 128);
    F.A->AY01PlayerController::Tick(0);
    const bool Interrupted = Case == TEXT("Switch") || Case == TEXT("Destroy") || Case == TEXT("Travel") || Case == TEXT("WorldEnd") || Case == TEXT("FatalHandoff");
    TestEqual(TEXT("整个128仍是同一20Hz周期"), Log.Batches.Num(), 1);
    TestEqual(TEXT("未中断128全送，中断只交付首包"), Log.Sent.Num(), Interrupted ? 8 : 128);
    TestEqual(TEXT("未中断16个小RPC，中断即停止余包"), Log.Chunks.Num(), Interrupted ? 1 : 16);
    TestEqual(TEXT("回调切区或结束后不再消费旧事件"), Received.Num(), Interrupted ? 1 : 128);
    for (const auto& Chunk : Log.Chunks)
    {
        TestTrue(TEXT("单RPC最多8条"), FDamageLog::Number(Chunk, TEXT("Count")) > 0 && FDamageLog::Number(Chunk, TEXT("Count")) <= 8);
        TestTrue(TEXT("预算含Audience和RPC头且严格小于768"), FDamageLog::Number(Chunk, TEXT("BytesBudget")) > 0 && FDamageLog::Number(Chunk, TEXT("BytesBudget")) < 768);
    }
    if (Case == TEXT("Full128") && !Received.IsEmpty())
    {
        // 使用Generic的真实属性编码，不用sizeof推断网络长度；完整RPC头另由联网引擎日志核对。
        auto Scope = OriginalAudience; auto E = Received[0];
        FNetBitWriter EventWire(nullptr, 0), AudienceWire(nullptr, 0); bool Unmapped = false;
        FRepLayout::CreateFromStruct(FY01DamageEvent::StaticStruct())->SerializePropertiesForStruct(FY01DamageEvent::StaticStruct(), EventWire, nullptr, &E, Unmapped);
        FRepLayout::CreateFromStruct(FY01DamageAudience::StaticStruct())->SerializePropertiesForStruct(FY01DamageAudience::StaticStruct(), AudienceWire, nullptr, &Scope, Unmapped);
        TestFalse(TEXT("纯值编码不依赖对象映射"), Unmapped || EventWire.IsError() || AudienceWire.IsError());
        TestEqual(TEXT("UE5.7含展示量与类型的实际事件编码为61字节存储"), EventWire.GetNumBits(), static_cast<int64>(482));
        TestEqual(TEXT("Audience含final权限实际编码40字节"), AudienceWire.GetNumBits(), static_cast<int64>(40 * 8));
        for (const auto& Chunk : Log.Chunks)
        {
            const int64 Count = static_cast<int64>(FDamageLog::Number(Chunk, TEXT("Count")));
            TestTrue(TEXT("实际值编码加96字节RPC/数组/内容头余量不超声明预算"),
                (EventWire.GetNumBytes() * Count + AudienceWire.GetNumBytes() + 96) <= FDamageLog::Number(Chunk, TEXT("BytesBudget")));
        }
        AddInfo(FString::Printf(TEXT("Generic wire: Event=%lld bytes Audience=%lld bytes; 8-event values=%lld bytes"),
            EventWire.GetNumBytes(), AudienceWire.GetNumBytes(), EventWire.GetNumBytes() * 8 + AudienceWire.GetNumBytes()));
    }
    for (int32 I=0; I<Received.Num(); ++I)
    {
        TestEqual(TEXT("满载按原事件顺序不漏不重"), Received[I].EventId, static_cast<uint64>(I + 1));
        TestTrue(TEXT("每包保留World身份和区域"), Received[I].WorldEpoch == OriginalAudience.WorldEpoch && Received[I].RegionId == OriginalAudience.RegionId);
    }
    if (Case == TEXT("Duplicate"))
    {
        for (const auto& E : Received) { F.A->QueueDamageEvent(OriginalAudience, E); }
        F.Tick(.1f);
        TestEqual(TEXT("跨16包重传仍全部去重"), Received.Num(), 128);
        TestEqual(TEXT("重复包真实经过发送入口"), Log.Sent.Num(), 256);
    }
    if (Case == TEXT("ReenterQueue") || Case == TEXT("Switch") || Case == TEXT("FatalHandoff"))
    {
        F.Tick(.1f);
        TestEqual(TEXT("重入新增队列留给下一周期，不被旧数组覆盖"), Received.Num(), Interrupted ? 2 : 129);
        if (!Received.IsEmpty()) { TestEqual(TEXT("重入新事件身份129"), Received.Last().EventId, static_cast<uint64>(129)); }
        TestEqual(TEXT("重入不能突破20Hz周期限额"), Log.Batches.Num(), 2);
        if (Log.Batches.Num() == 2)
        { TestTrue(TEXT("重入后的周期间隔仍至少50ms"), FDamageLog::Number(Log.Batches[1], TEXT("Time")) - FDamageLog::Number(Log.Batches[0], TEXT("Time")) >= .049999); }
        if (Case == TEXT("FatalHandoff"))
        {
            const auto Scope = F.A->GetDamageAudience();
            TestEqual(TEXT("死亡自动回主线"), Scope.RegionId, 0);
            TestTrue(TEXT("新包仅准许最后致命事件的旧区身份"), !Received.IsEmpty() && Scope.IsFinalEvent(Received.Last()));
            struct FParams { FY01DamageAudience Audience; TArray<FY01DamageEvent> Events; } Params{Scope, {Received[0], Received.Last()}};
            F.A->ProcessEvent(F.A->FindFunctionChecked(TEXT("ClientDamageBatch")), &Params);
            TestEqual(TEXT("最后事件去重且不能借交接重放其他旧区身份"), Received.Num(), 2);
        }
    }
    F.A->OnDamageReceived.Remove(Handle);
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DamageBackpressure, "Y01.Playground.Damage.Backpressure", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01DamageBackpressure::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("SaturatedStart"), TEXT("SaturatedTail"), TEXT("QueueOldest"), TEXT("SwitchPending"), TEXT("EndPlayPending")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01DamageBackpressure::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FDamageWorld F; FDamageLog Log; F.Start();
    F.A->Player = NewObject<ULocalPlayer>(GEngine);
    auto Monsters = F.Training(F.A); if (!TestEqual(TEXT("背压真实训练目标"), Monsters.Num(), 2)) { return false; }
    auto* S = F.ASC(); auto* T = Monsters[0]->GetCombatASC();
    // 复用引擎真实带宽就绪判定；Standalone仍同步消费RPC，便于在包间精确制造饱和。
    TStrongObjectPtr<UDemoNetDriver> Driver(NewObject<UDemoNetDriver>());
    TStrongObjectPtr<USimulatedClientNetConnection> Connection(NewObject<USimulatedClientNetConnection>());
    Connection->InitConnection(Driver.Get(), USOCK_Open, FURL());
    Driver->ClientConnections.Add(Connection.Get());
    Connection->QueuedBits = 0; F.A->NetConnection = Connection.Get();
    TestTrue(TEXT("真实连接初始可发送"), Connection->IsNetReady());
    TArray<uint64> Received; bool Entered = false;
    const auto Handle = F.A->OnDamageReceived.AddLambda([&](const FY01DamageEvent& E)
    {
        Received.Add(E.EventId);
        if (Entered || Case == TEXT("SaturatedStart")) { return; } Entered = true;
        Connection->QueuedBits = 1;
        TestFalse(TEXT("真实连接已超过本帧发送额度"), Connection->IsNetReady());
        if (Case == TEXT("QueueOldest"))
        { for (int32 I=0; I<40; ++I) { FDamageWorld::Damage(S, T, 1); } }
    });
    for (int32 I=0; I<128; ++I) { FDamageWorld::Damage(S, T, 1); }
    if (Case == TEXT("SaturatedStart")) { Connection->QueuedBits = 1; }
    F.A->AY01PlayerController::Tick(0);
    const int32 FirstCount = Case == TEXT("SaturatedStart") ? 0 : 8;
    TestEqual(TEXT("饱和后不再把未发送RPC交给引擎丢弃"), Log.Sent.Num(), FirstCount);
    TestEqual(TEXT("饱和只影响后续独立包，不截断已发包"), Received.Num(), FirstCount);
    TestEqual(TEXT("只发送就绪的小包"), Log.Chunks.Num(), FirstCount / 8);
    F.A->NetConnection = nullptr;
    if (Case == TEXT("SwitchPending"))
    {
        F.Mode->GetTraining()->Request(F.A, false); auto Next = F.Training(F.A);
        if (TestEqual(TEXT("背压后切区可重新训练"), Next.Num(), 2)) { FDamageWorld::Damage(S, Next[0]->GetCombatASC(), 1); }
    }
    if (Case == TEXT("EndPlayPending")) { F.A->RouteEndPlay(EEndPlayReason::LevelTransition); }
    F.Tick(.1f);
    const int32 Expected = Case == TEXT("QueueOldest") ? 136 : Case == TEXT("SwitchPending") ? 9 : Case == TEXT("EndPlayPending") ? 8 : 128;
    TestEqual(TEXT("带宽恢复交付未发送尾部，切区/结束丢弃旧尾部"), Received.Num(), Expected);
    TestEqual(TEXT("没有发送后重试或隐式可靠重传"), Log.Sent.Num(), Expected);
    for (int32 I=0; I<Received.Num(); ++I)
    {
        const uint64 Id = Case == TEXT("QueueOldest") && I >= 8 ? I + 33 : Case == TEXT("SwitchPending") && I == 8 ? 129 : I + 1;
        TestEqual(TEXT("尾部排在重入新增前，超128仍精确丢最旧"), Received[I], Id);
    }
    for (int32 I=1; I<Log.Batches.Num(); ++I)
    { TestTrue(TEXT("背压不绕过20Hz"), FDamageLog::Number(Log.Batches[I], TEXT("Time")) - FDamageLog::Number(Log.Batches[I-1], TEXT("Time")) >= .049999); }
    F.Tick(.1f); TestEqual(TEXT("已交付的尾部不重发"), Received.Num(), Expected);
    F.A->OnDamageReceived.Remove(Handle);
    const TWeakObjectPtr<USimulatedClientNetConnection> ReleasedConnection = Connection.Get();
    // 驱动必须存活到连接完成清理；随后强制GC，避免夹具把悬空清理延迟到其他测试。
    Connection->CleanUp();
    Connection.Reset(); Driver.Reset(); CollectGarbage(RF_NoFlags, true);
    TestFalse(TEXT("背压夹具连接完整回收，不把清理崩溃留给后续测试"), ReleasedConnection.IsValid());
    return true;
}
#endif
