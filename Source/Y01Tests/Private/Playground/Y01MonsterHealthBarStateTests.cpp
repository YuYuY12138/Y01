#include "Misc/AutomationTest.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01PlaygroundSeed.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/Level.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameplayEffectExtension.h"
#include "UObject/UnrealType.h"
#include "Net/UnrealNetwork.h"
#include "Engine/NetConnection.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
// 通过反射观察持久事实：功能尚未实现时也能编译并产生行为断言失败。
struct FStateProbe
{
    FStructProperty* Property = nullptr;
    void* Data = nullptr;
    FStateProbe(UObject* Object, const TCHAR* Name)
    {
        Property = FindFProperty<FStructProperty>(Object->GetClass(), Name);
        if (Property) { Data = Property->ContainerPtrToValuePtr<void>(Object); }
    }
    bool Flag(const TCHAR* Name) const
    {
        const auto* P = Property ? FindFProperty<FBoolProperty>(Property->Struct, Name) : nullptr;
        return P && P->GetPropertyValue_InContainer(Data);
    }
    int64 Number(const TCHAR* Name) const
    {
        const auto* P = Property ? FindFProperty<FProperty>(Property->Struct, Name) : nullptr;
        if (const auto* E = CastField<FEnumProperty>(P)) { return E->GetUnderlyingProperty()->GetSignedIntPropertyValue(E->ContainerPtrToValuePtr<void>(Data)); }
        const auto* N = CastField<FNumericProperty>(P);
        return N ? N->GetSignedIntPropertyValue(N->ContainerPtrToValuePtr<void>(Data)) : -1;
    }
    FGuid Epoch() const
    {
        const auto* P = Property ? FindFProperty<FStructProperty>(Property->Struct, TEXT("WorldEpoch")) : nullptr;
        return P ? *P->ContainerPtrToValuePtr<FGuid>(Data) : FGuid();
    }
    static bool Register(AY01Monster* M, int32 Region = 0, int32 Session = 0)
    {
        auto* Function = M->FindFunction(TEXT("RegisterHealthBarPresentation"));
        if (!Function) { return false; }
        struct { int32 RegionId; int32 RegionSession; bool ReturnValue; } Params{Region, Session, false};
        M->ProcessEvent(Function, &Params); return Params.ReturnValue;
    }
};
struct FHealthBarWorld
{
    TStrongObjectPtr<UY01PlaygroundProfile> Profile{Y01PlaygroundEditor::CreateSeed(GetTransientPackage())};
    UWorld* World;
    AY01PlaygroundGameMode* Mode;
    AY01PlaygroundController* A;
    AY01PlaygroundController* B;
    FHealthBarWorld()
    {
        Profile->bTrainingEnabled = true;
        auto* C = Profile->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("R27"));
        C->Health = 1000; C->Range = 1000; C->MoveSpeed = 0; C->InitialEquipment.Reset(); C->R33TestGrowth.bEnabled = false;
        auto* Monsters = Profile->Tables[EY01TableKind::Monster].Get();
        for (const TCHAR* Name : {TEXT("Default"), TEXT("Boss")})
        {
            auto* M = Monsters->FindRow<FY01MonsterRow>(Name, TEXT("R27"));
            M->Health = 100; M->AttackDamage = 0; M->MoveSpeed = 0; M->Skills.Reset();
        }
        const auto Normal = *Monsters->FindRow<FY01MonsterRow>(TEXT("Default"), TEXT("R27"));
        Monsters->AddRow(TEXT("TrainingDummy"), Normal); Monsters->AddRow(TEXT("Wave1Minion"), Normal);
        auto* Stage = Profile->Tables[EY01TableKind::TrainingStage]->FindRow<FY01TrainingStageRow>(TEXT("Default"), TEXT("R27"));
        Stage->SpawnInterval = 100; Stage->MaxAlive = 2; Stage->Spawns[0].Count = 2; Stage->Spawns[0].DelaySeconds = 0;
        Stage->Spawns[0].Monster.RowName = TEXT("TrainingDummy");
        for (const auto& Ref : Profile->OrderedWaves)
        {
            auto* W = Profile->Tables[EY01TableKind::Wave]->FindRow<FY01WaveRow>(Ref.RowName, TEXT("R27"));
            W->PreparationSeconds = W->Type == EY01WaveType::Boss ? 1 : 0;
            for (auto& S : W->Spawns) { S.DelaySeconds = 0; }
        }
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01PlaygroundGameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01PlaygroundGameMode>(); World->SpawnActor<AY01PlaygroundConfig>()->Profile = Profile.Get();
        A = Player(); B = Player();
    }
    ~FHealthBarWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    AY01PlaygroundController* Player()
    {
        auto* PC = World->SpawnActor<AY01PlaygroundController>(); auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01PlaygroundCharacter>(FVector(0,0,100), FRotator::ZeroRotator);
        PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(PS); return PC;
    }
    AY01PlayerState* PS(AY01PlaygroundController* PC) const { return PC->GetPlayerState<AY01PlayerState>(); }
    UY01CombatAbilitySystem* Source() const { return PS(A)->GetCombatASC(); }
    void Start() { Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay(); }
    void Tick(float Seconds = .02f) { for (int32 I = 0; I < FMath::CeilToInt(Seconds * 100); ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .01f); } }
    AY01Monster* Loose(FName Name = TEXT("Default"))
    {
        FActorSpawnParameters P; P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* M = World->SpawnActor<AY01Monster>(FVector(100,0,100), FRotator::ZeroRotator, P);
        FY01DataRef Ref; Ref.Type = EY01TableKind::Monster; Ref.RowName = Name; FString Error;
        M->TryInitializeMonster(Profile->BuildCatalog(), Ref, Error); return M;
    }
    void Damage(AY01Monster* M, float Amount)
    {
        auto Spec = Source()->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, Source()->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, Amount); Source()->ApplyGameplayEffectSpecToTarget(*Spec.Data, M->GetCombatASC());
    }
    void Modify(AY01Monster* M, float Amount, bool Tagged = false)
    {
        auto* GE = NewObject<UGameplayEffect>(); GE->DurationPolicy = EGameplayEffectDurationType::Instant;
        auto& Mod = GE->Modifiers.AddDefaulted_GetRef(); Mod.Attribute = UY01AttributeSet::GetHealthAttribute();
        Mod.ModifierOp = EGameplayModOp::Additive; Mod.ModifierMagnitude = FScalableFloat(Amount);
        FGameplayEffectSpec Spec(GE, Source()->MakeEffectContext(), 1);
        if (Tagged) { Spec.AddDynamicAssetTag(Y01Tags::Effect_Damage); }
        Source()->ApplyGameplayEffectSpecToTarget(Spec, M->GetCombatASC());
    }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01HealthBarFact, "Y01.Playground.R27.State.Fact", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01HealthBarFact::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Actual"), TEXT("Armor"), TEXT("Overkill"), TEXT("Zero"), TEXT("BlockedByArmor"), TEXT("NaN"), TEXT("Infinity"),
        TEXT("DirectSet"), TEXT("Cost"), TEXT("MaxClamp"), TEXT("Rejected"), TEXT("SameFrameFullHeal"), TEXT("NoAudience"),
        TEXT("NestedDamageHeal"), TEXT("NestedCostDamage"), TEXT("DeathDestroy"), TEXT("DeathSessionDestroy"), TEXT("DirectDeath"), TEXT("RegisterAfterDamage")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01HealthBarFact::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FHealthBarWorld F; F.Start();
    if (!TestTrue(TEXT("真实服务器配置成功"), F.Mode->IsMatchConfigured())) { return false; }
    auto* M = F.Loose(); auto* T = M->GetCombatASC(); FStateProbe P(M, TEXT("PresentationState"));
    TestTrue(TEXT("怪物持有持久展示事实结构"), P.Property != nullptr);
    TestFalse(TEXT("初始化不创造受伤资格"), P.Flag(TEXT("bEverActuallyDamaged")));
    bool Expected = true;
    if (Case == TEXT("NoAudience")) { F.World->GetSubsystem<UY01DamageEvents>()->AudiencePolicy.Unbind(); }
    if (Case == TEXT("Armor")) { T->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), 7); }
    if (Case == TEXT("BlockedByArmor")) { T->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), 50); Expected = false; }
    if (Case == TEXT("Zero") || Case == TEXT("NaN") || Case == TEXT("Infinity")) { Expected = false; }
    if (Case == TEXT("DirectSet")) { T->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 90); Expected = false; }
    else if (Case == TEXT("Cost")) { F.Modify(M, -10); Expected = false; }
    else if (Case == TEXT("MaxClamp")) { T->SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(), 70); Expected = false; }
    else if (Case == TEXT("Rejected"))
    { T->TargetPermission = [](const UY01CombatAbilitySystem*, const UY01CombatAbilitySystem*) { return false; }; F.Modify(M, -10, true); Expected = false; }
    else if (Case == TEXT("DirectDeath")) { T->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); Expected = false; }
    else if (Case.StartsWith(TEXT("Nested")))
    {
        bool Entered = false;
        auto H = T->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda([&](const FOnAttributeChangeData& D)
        {
            if (Entered || D.NewValue >= D.OldValue) { return; } Entered = true;
            F.Modify(M, 5); F.Damage(M, 7);
        });
        if (Case == TEXT("NestedCostDamage")) { F.Modify(M, -10); } else { F.Damage(M, 10); }
        T->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).Remove(H);
        TestEqual(TEXT("嵌套实际生命"), Y01Playground::Health(T), 88.f);
    }
    else
    {
        if (Case == TEXT("DeathDestroy"))
        {
            T->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda([&](const FOnAttributeChangeData& D)
            { if (D.NewValue <= 0) { TestTrue(TEXT("死亡委托前已锁存"), P.Flag(TEXT("bEverActuallyDamaged"))); TestTrue(TEXT("死亡委托前已有终态"), P.Flag(TEXT("bDead"))); M->Destroy(); } });
        }
        if (Case == TEXT("DeathSessionDestroy"))
        { T->CombatSessionInvalidated.AddLambda([&]() { TestTrue(TEXT("会话撤销前已锁存"), P.Flag(TEXT("bEverActuallyDamaged"))); M->Destroy(); }); }
        const float Amount = Case == TEXT("NaN") ? std::numeric_limits<float>::quiet_NaN() : Case == TEXT("Infinity") ? std::numeric_limits<float>::infinity()
            : Case == TEXT("Zero") ? 0 : Case == TEXT("Overkill") || Case.StartsWith(TEXT("Death")) ? 1000 : 20;
        F.Damage(M, Amount);
    }
    if (Case == TEXT("SameFrameFullHeal")) { F.Modify(M, 100); TestEqual(TEXT("同帧回满"), Y01Playground::Health(T), 100.f); }
    TestEqual(TEXT("仅获准带伤害标签的实际有限正扣血锁存"), P.Flag(TEXT("bEverActuallyDamaged")), Expected);
    if (Case == TEXT("Overkill") || Case.StartsWith(TEXT("Death")) || Case == TEXT("DirectDeath")) { TestTrue(TEXT("死亡终态独立于伤害资格"), P.Flag(TEXT("bDead"))); }
    if (Case == TEXT("RegisterAfterDamage"))
    { TestTrue(TEXT("受伤后可登记"), FStateProbe::Register(M)); TestTrue(TEXT("登记不抹除受伤事实"), P.Flag(TEXT("bEverActuallyDamaged"))); }
    if (!Expected && Case != TEXT("DirectDeath"))
    {
        T->TargetPermission = {}; T->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), 0);
        F.Damage(M, 1); TestTrue(TEXT("正向对照避免永不锁存的假绿"), P.Flag(TEXT("bEverActuallyDamaged")));
    }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01HealthBarScope, "Y01.Playground.R27.State.Scope", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01HealthBarScope::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("InitialAuthorization"), TEXT("TrainingSessions"), TEXT("MainlineShared"), TEXT("LateJoin"), TEXT("FailedEntry"),
        TEXT("Ended"), TEXT("Logout"), TEXT("Travel"), TEXT("Identity.Default"), TEXT("Identity.TrainingDummy"), TEXT("Identity.Wave1Minion"), TEXT("Identity.Boss"), TEXT("Identity.Unknown"), TEXT("ReplicationContract")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01HealthBarScope::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FHealthBarWorld F;
    if (Case == TEXT("FailedEntry")) { F.Profile->bTrainingEnabled = false; }
    F.Start(); if (!TestTrue(TEXT("真实配置成功"), F.Mode->IsMatchConfigured())) { return false; }
    FStateProbe A(F.A, TEXT("HealthBarAuthorization")), B(F.B, TEXT("HealthBarAuthorization"));
    TestTrue(TEXT("首次伤害之前已主动授权"), A.Flag(TEXT("bEnabled")));
    TestEqual(TEXT("主线区域"), A.Number(TEXT("RegionId")), int64(0));
    TestEqual(TEXT("主线会话为零"), A.Number(TEXT("RegionSession")), int64(0));
    TestTrue(TEXT("复用World服务Epoch"), A.Epoch() == F.World->GetSubsystem<UY01DamageEvents>()->GetWorldEpoch());
    if (Case.StartsWith(TEXT("Identity.")))
    {
        const auto Name = Case.RightChop(9); auto* M = F.Loose(Name == TEXT("Unknown") ? FName(TEXT("Default")) : FName(*Name));
        FStateProbe P(M, TEXT("PresentationState"));
        TestFalse(TEXT("未进入真实容器之前为Unknown"), P.Flag(TEXT("bRegistered")));
        if (Name == TEXT("Unknown")) { TestEqual(TEXT("未登记身份Unknown"), P.Number(TEXT("Kind")), int64(0)); return true; }
        TestTrue(TEXT("合法初始化怪物显式登记"), FStateProbe::Register(M));
        TestTrue(TEXT("登记有效"), P.Flag(TEXT("bRegistered")));
        TestEqual(TEXT("按合法定义而不是波类型判定身份"), P.Number(TEXT("Kind")), int64(Name == TEXT("Boss") ? 2 : 1));
        return true;
    }
    if (Case == TEXT("FailedEntry"))
    { const auto G = A.Number(TEXT("Generation")); TestFalse(TEXT("失败入房"), F.Mode->GetTraining()->Request(F.A, true)); TestEqual(TEXT("失败不改授权代次"), A.Number(TEXT("Generation")), G); return true; }
    if (Case == TEXT("Travel"))
    { F.A->PreClientTravel(TEXT("/Game/Next"), TRAVEL_Relative, true); TestFalse(TEXT("旅行立即撤权"), A.Flag(TEXT("bEnabled"))); F.A->Tick(0); TestFalse(TEXT("旧世界就绪补偿不能重新授权"), A.Flag(TEXT("bEnabled"))); return true; }
    if (Case == TEXT("Logout"))
    { F.Mode->Logout(F.A); TestFalse(TEXT("登出撤权"), A.Flag(TEXT("bEnabled"))); return true; }
    if (Case == TEXT("ReplicationContract"))
    {
        auto* Base = F.World->SpawnActor<AY01PlayerController>(); FStateProbe BaseScope(Base, TEXT("HealthBarAuthorization"));
        TestFalse(TEXT("基础Controller默认不接入"), BaseScope.Flag(TEXT("bEnabled")));
        auto Check = [&](UObject* Object, FStateProbe& P, ELifetimeCondition Condition)
        {
            if (!TestNotNull(TEXT("复制结构存在"), P.Property)) { return; }
            TestTrue(TEXT("复制且统一RepNotify"), P.Property->HasAllPropertyFlags(CPF_Net | CPF_RepNotify));
            TestTrue(TEXT("完整结构自定义网络序列化"), P.Property->Struct->GetCppStructOps() && P.Property->Struct->GetCppStructOps()->HasNetSerializer());
            Object->GetClass()->SetUpRuntimeReplicationData();
            TArray<FLifetimeProperty> Props; Object->GetLifetimeReplicatedProps(Props);
            const auto* Found = Props.FindByPredicate([&](const auto& V) { return V.RepIndex == P.Property->RepIndex; });
            TestTrue(TEXT("准确复制条件"), Found && Found->Condition == Condition);
        };
        Check(F.A, A, COND_OwnerOnly); auto* M = F.Loose(); FStateProbe P(M, TEXT("PresentationState")); Check(M, P, COND_None); return true;
    }
    if (Case == TEXT("TrainingSessions"))
    {
        TestTrue(TEXT("A成功入房"), F.Mode->GetTraining()->Request(F.A, true));
        const auto View = F.Mode->GetTraining()->Observe(F.PS(F.A));
        TestEqual(TEXT("刷怪时已有房间授权"), A.Number(TEXT("RegionId")), int64(View.Room));
        TestEqual(TEXT("授权使用真实session"), A.Number(TEXT("RegionSession")), int64(View.Session));
        for (auto* M : F.Mode->GetTraining()->Monsters(F.PS(F.A)))
        { FStateProbe P(M, TEXT("PresentationState")); TestTrue(TEXT("训练容器登记"), P.Flag(TEXT("bRegistered"))); TestEqual(TEXT("训练怪同会话"), P.Number(TEXT("RegionSession")), int64(View.Session)); TestEqual(TEXT("TrainingDummy是Normal"), P.Number(TEXT("Kind")), int64(1)); }
        F.Mode->GetTraining()->Request(F.B, true); TestNotEqual(TEXT("独立私房"), A.Number(TEXT("RegionId")), B.Number(TEXT("RegionId")));
        F.Mode->GetTraining()->Request(F.A, false); TestEqual(TEXT("离房恢复主线"), A.Number(TEXT("RegionId")), int64(0));
        F.Mode->GetTraining()->Request(F.A, true); TestTrue(TEXT("同房重入session更新"), A.Number(TEXT("RegionSession")) > View.Session); return true;
    }
    if (Case == TEXT("InitialAuthorization")) { TestTrue(TEXT("另一主线玩家同步授权"), B.Flag(TEXT("bEnabled"))); return true; }
    TestTrue(TEXT("启动真实主线"), F.Mode->RequestAdvance(F.A)); F.Tick();
    if (!TestTrue(TEXT("真实波怪存在"), F.Mode->GetWaveMonsters().Num() > 0)) { return false; }
    auto* M = F.Mode->GetWaveMonsters()[0].Get(); FStateProbe P(M, TEXT("PresentationState"));
    TestTrue(TEXT("主线容器登记"), P.Flag(TEXT("bRegistered"))); TestEqual(TEXT("主线怪区域"), P.Number(TEXT("RegionId")), int64(0));
    F.Damage(M, 10); F.Modify(M, 100);
    TestTrue(TEXT("存活主线回满仍有事实"), P.Flag(TEXT("bEverActuallyDamaged")));
    if (Case == TEXT("LateJoin"))
    {
        auto* Late = F.Player(); Late->Player = NewObject<ULocalPlayer>(GEngine);
        F.Mode->PostLogin(Late); FStateProbe L(Late, TEXT("HealthBarAuthorization"));
        TestTrue(TEXT("晚加入不依赖第一击授权"), L.Flag(TEXT("bEnabled"))); TestTrue(TEXT("晚加入可读服务器持久受伤事实"), P.Flag(TEXT("bEverActuallyDamaged")));
    }
    if (Case == TEXT("MainlineShared"))
    {
        TestTrue(TEXT("两个主线玩家使用同Epoch"), A.Epoch() == B.Epoch());
        F.Mode->GetTraining()->Request(F.A, true); F.Mode->GetTraining()->Request(F.A, false);
        TestTrue(TEXT("离区重入存活怪资格保留"), P.Flag(TEXT("bEverActuallyDamaged"))); TestEqual(TEXT("重入主线授权"), A.Number(TEXT("RegionId")), int64(0));
    }
    if (Case == TEXT("Ended")) { TestTrue(TEXT("对局结束"), F.Mode->TryEndMatch(false)); TestFalse(TEXT("Ended撤销A"), A.Flag(TEXT("bEnabled"))); TestFalse(TEXT("Ended撤销B"), B.Flag(TEXT("bEnabled"))); }
    return true;
}
namespace
{
template<typename T> bool DeliverR27(UObject* Object, const TCHAR* PropertyName, const TCHAR* NotifyName, T Value)
{
    auto* P = FindFProperty<FStructProperty>(Object->GetClass(), PropertyName); auto* Notify = Object->FindFunction(NotifyName);
    if (!P || !Notify) { return false; }
    FNetBitWriter Wire(nullptr, 0); bool Success = false; Value.NetSerialize(Wire, nullptr, Success);
    if (!Success || Wire.IsError()) { return false; }
    FNetBitReader Reader(nullptr, Wire.GetData(), Wire.GetNumBits());
    auto* Destination = P->ContainerPtrToValuePtr<T>(Object); Destination->NetSerialize(Reader, nullptr, Success);
    if (!Success || Reader.IsError()) { return false; }
    Object->ProcessEvent(Notify, nullptr); return true;
}
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01HealthBarEdges, "Y01.Playground.R27.State.Edges", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01HealthBarEdges::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Wire.Monster"), TEXT("Wire.Authorization"), TEXT("Delivery.LateFullHeal"), TEXT("Delivery.Order"), TEXT("Delivery.OldEpoch"),
        TEXT("Delivery.ClientAuthority"), TEXT("Delivery.ScopeMismatch"), TEXT("Lifecycle.TravelServer"), TEXT("Lifecycle.TravelServerNoPre"), TEXT("Lifecycle.TravelClient"),
        TEXT("Lifecycle.MonsterWorld"), TEXT("Lifecycle.ModeEndPlay"), TEXT("Lifecycle.WorldEndPlay"), TEXT("Lifecycle.DeathTerminal"),
        TEXT("Registration.Invalid"), TEXT("Registration.Notifications"), TEXT("Registration.BossWaveMinion"), TEXT("Readiness.Compensation")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01HealthBarEdges::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FHealthBarWorld F;
    if (Case == TEXT("Registration.BossWaveMinion"))
    {
        auto* W = F.Profile->Tables[EY01TableKind::Wave]->FindRow<FY01WaveRow>(TEXT("Boss"), TEXT("R27"));
        auto Entry = W->Spawns[0]; Entry.Monster.RowName = TEXT("Wave1Minion"); W->Spawns.Add(Entry);
    }
    F.Start(); if (!TestTrue(TEXT("边界测试真实配置"), F.Mode->IsMatchConfigured())) { return false; }
    auto* M = F.Loose(); TestTrue(TEXT("登记"), M->RegisterHealthBarPresentation(0, 0)); F.Damage(M, 20); F.Modify(M, 100);
    const auto State = M->GetHealthBarPresentationState(); const auto Scope = F.A->GetHealthBarAuthorization();
    TestTrue(TEXT("实际扣血后同帧回满仍符合资格"), State.HasLiveHealthBarEligibility());
    if (Case.StartsWith(TEXT("Wire.")))
    {
        auto Roundtrip = [&](auto Original)
        {
            using T = decltype(Original); FNetBitWriter Wire(nullptr, 0); bool Success = false;
            Original.NetSerialize(Wire, nullptr, Success); TestTrue(TEXT("真实网络位编码成功"), Success && !Wire.IsError());
            T Received; FNetBitReader Reader(nullptr, Wire.GetData(), Wire.GetNumBits()); Received.NetSerialize(Reader, nullptr, Success);
            TestTrue(TEXT("完整结构解码成功"), Success && !Reader.IsError());
            TestTrue(TEXT("所有字段原子往返相同"), T::StaticStruct()->CompareScriptStruct(&Original, &Received, 0));
            auto Invalid = Original; Invalid.RegionId = -8; FNetBitWriter BadWire(nullptr, 0); Invalid.NetSerialize(BadWire, nullptr, Success);
            TestFalse(TEXT("非法scope不被编码器认可"), Success);
            FNetBitReader BadReader(nullptr, BadWire.GetData(), BadWire.GetNumBits()); Received.NetSerialize(BadReader, nullptr, Success);
            TestFalse(TEXT("非法scope不被解码器认可"), Success);
            TestTrue(TEXT("失败不部分覆盖旧状态"), T::StaticStruct()->CompareScriptStruct(&Original, &Received, 0));
        };
        if (Case.EndsWith(TEXT("Monster"))) { Roundtrip(State); } else { Roundtrip(Scope); }
        return true;
    }
    if (Case == TEXT("Registration.Invalid"))
    {
        auto* Unknown = F.World->SpawnActor<AY01Monster>();
        TestFalse(TEXT("未初始化不能登记"), Unknown->RegisterHealthBarPresentation(0, 0));
        for (auto Pair : {FIntPoint(-1,0), FIntPoint(0,1), FIntPoint(1,0), FIntPoint(1,-1)})
        { TestFalse(TEXT("非法Region/Session拒绝"), M->RegisterHealthBarPresentation(Pair.X, Pair.Y)); }
        TestEqual(TEXT("失败登记不改revision"), M->GetHealthBarPresentationState().Revision, State.Revision); return true;
    }
    if (Case == TEXT("Registration.Notifications"))
    {
        int32 Notices = 0; M->OnHealthBarPresentationChanged.AddLambda([&]() { ++Notices; });
        M->RegisterHealthBarPresentation(0, 0); TestEqual(TEXT("重复登记幂等"), Notices, 0);
        M->UnregisterHealthBarPresentation(); TestEqual(TEXT("离区通知一次"), Notices, 1);
        TestFalse(TEXT("撤销后立即无资格"), M->GetHealthBarPresentationState().HasLiveHealthBarEligibility());
        M->RegisterHealthBarPresentation(0, 0); TestTrue(TEXT("重新登记仍保存事实"), M->GetHealthBarPresentationState().HasLiveHealthBarEligibility());
        const int32 Before = Notices; F.Damage(M, 1); F.Modify(M, 100); TestEqual(TEXT("后续伤害回血不重复资格通知"), Notices, Before);
        M->OnHealthBarPresentationChanged.Clear(); return true;
    }
    if (Case == TEXT("Lifecycle.DeathTerminal"))
    {
        auto* Fresh = F.Loose(); Fresh->RegisterHealthBarPresentation(0, 0);
        Fresh->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0);
        TestTrue(TEXT("非伤害死亡终态"), Fresh->GetHealthBarPresentationState().bDead);
        TestFalse(TEXT("非伤害死亡不创造资格"), Fresh->GetHealthBarPresentationState().bEverActuallyDamaged);
        Fresh->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 100); F.Damage(Fresh, 1);
        TestFalse(TEXT("死亡后不能复活资格"), Fresh->GetHealthBarPresentationState().bEverActuallyDamaged);
        TestFalse(TEXT("死亡后不能重新登记"), Fresh->RegisterHealthBarPresentation(0,0)); return true;
    }
    if (Case == TEXT("Lifecycle.ModeEndPlay") || Case == TEXT("Lifecycle.WorldEndPlay"))
    {
        F.Mode->GetTraining()->Request(F.A, true);
        if (Case == TEXT("Lifecycle.ModeEndPlay")) { F.Mode->EndPlay(EEndPlayReason::LevelTransition); }
        else { F.World->EndPlay(EEndPlayReason::LevelTransition); }
        TestFalse(TEXT("清理训练房不能重开授权"), F.A->GetHealthBarAuthorization().bEnabled);
        F.Mode->RefreshHealthBarAuthorization(F.A); TestFalse(TEXT("旧就绪补偿不能重开"), F.A->GetHealthBarAuthorization().bEnabled); return true;
    }
    if (Case == TEXT("Lifecycle.TravelServer") || Case == TEXT("Lifecycle.TravelServerNoPre"))
    {
        FHealthBarWorld Next; Next.Start();
        if (Case == TEXT("Lifecycle.TravelServer")) { F.A->PreClientTravel(TEXT("/Game/Next"), TRAVEL_Relative, true); }
        F.A->UnPossess(); F.A->PlayerState = nullptr; F.World->RemoveController(F.A);
        F.A->Rename(nullptr, Next.World->PersistentLevel); Next.World->AddController(F.A); F.A->PostSeamlessTravel();
        TestFalse(TEXT("新世界未配置前关闭"), F.A->GetHealthBarAuthorization().bEnabled);
        F.A->SetHealthBarRegion(0,0); const auto NewScope = F.A->GetHealthBarAuthorization();
        TestTrue(TEXT("同Actor迁移后可显式授权新World"), NewScope.bEnabled);
        TestTrue(TEXT("新World复用新Epoch"), NewScope.WorldEpoch == Next.World->GetSubsystem<UY01DamageEvents>()->GetWorldEpoch());
        TestTrue(TEXT("同Actor授权代次不回退"), NewScope.Generation > Scope.Generation); return true;
    }
    if (Case == TEXT("Lifecycle.MonsterWorld"))
    {
        FHealthBarWorld Next; Next.Start(); M->Rename(nullptr, Next.World->PersistentLevel);
        TestFalse(TEXT("跨World立即失效"), M->GetHealthBarPresentationState().bRegistered);
        TestTrue(TEXT("新World可重新登记"), M->RegisterHealthBarPresentation(0,0));
        TestFalse(TEXT("同Actor旧世界受伤事实不污染新世界"), M->GetHealthBarPresentationState().bEverActuallyDamaged);
        TestTrue(TEXT("同Actorrevision继续前进"), M->GetHealthBarPresentationState().Revision > State.Revision); return true;
    }
    if (Case == TEXT("Registration.BossWaveMinion"))
    {
        F.Mode->RequestAdvance(F.A); F.Tick();
        for (AY01Monster* W : F.Mode->GetWaveMonsters()) { W->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        TestTrue(TEXT("主线第一波完成"), F.Mode->TryCompleteWave()); F.Mode->RequestAdvance(F.A); F.Tick(1.1f);
        int32 Bosses = 0, Minions = 0;
        for (AY01Monster* W : F.Mode->GetWaveMonsters())
        {
            const auto P = W->GetHealthBarPresentationState();
            if (W->GetCombatASC()->GetDefinition().RowName == TEXT("Boss")) { ++Bosses; TestTrue(TEXT("Boss展示身份"), P.Kind == EY01MonsterPresentationKind::Boss); }
            else { ++Minions; TestTrue(TEXT("Boss波杂兵仍Normal"), P.Kind == EY01MonsterPresentationKind::Normal); }
        }
        TestTrue(TEXT("确有Boss和杂兵"), Bosses > 0 && Minions > 0); return true;
    }
    if (Case == TEXT("Readiness.Compensation"))
    {
        F.A->RevokeHealthBarAuthorization(); F.A->CaptureObserver(F.Mode);
        TestTrue(TEXT("无伤害就绪补偿主动授权"), F.A->GetHealthBarAuthorization().bEnabled);
        auto* Base = F.World->SpawnActor<AY01PlayerController>(); Base->SetHealthBarRegion(0,0);
        TestFalse(TEXT("基础Controller不能被配置意外启用"), Base->GetHealthBarAuthorization().bEnabled); return true;
    }
    // 这里模拟完整网络结构解码及RepNotify投递，不冒充真实socket/远程客户端验收。
    auto* Peer = F.World->SpawnActor<AY01PlaygroundController>(); Peer->SetRole(ROLE_SimulatedProxy);
    auto* Remote = F.World->SpawnActor<AY01Monster>(); Remote->SetRole(ROLE_SimulatedProxy);
    auto SendScope = [&](const FY01HealthBarAuthorization& S) { TestTrue(TEXT("授权完整投递"), DeliverR27(Peer, TEXT("HealthBarAuthorization"), TEXT("OnRep_HealthBarAuthorization"), S)); };
    auto SendState = [&](const FY01MonsterPresentationState& S) { TestTrue(TEXT("展示状态完整投递"), DeliverR27(Remote, TEXT("PresentationState"), TEXT("OnRep_PresentationState"), S)); };
    SendScope(Scope); SendState(State);
    TestTrue(TEXT("晚加入仅快照可恢复满血存活怪资格"), Remote->GetHealthBarPresentationState().HasLiveHealthBarEligibility());
    TestTrue(TEXT("晚加入主线授权匹配"), Peer->GetHealthBarAuthorization().Matches(Remote->GetHealthBarPresentationState()));
    TestEqual(TEXT("展示结构不重复复制生命"), Y01Playground::Health(Remote->GetCombatASC()), 0.f);
    if (Case == TEXT("Delivery.Order") || Case == TEXT("Delivery.OldEpoch"))
    {
        auto NewScope = Scope; NewScope.Generation += 2; NewScope.RegionId = 4; NewScope.RegionSession = 9;
        auto NewState = State; NewState.Revision += 2; NewState.RegionId = 4; NewState.RegionSession = 9;
        SendScope(NewScope); SendState(NewState);
        auto OldScope = Scope; auto OldState = State;
        if (Case == TEXT("Delivery.OldEpoch")) { OldScope.WorldEpoch = OldState.WorldEpoch = FGuid::NewGuid(); OldScope.Generation += 100; OldState.Revision += 100; }
        SendScope(OldScope); SendState(OldState);
        TestEqual(TEXT("旧代次/Epoch不能覆盖授权"), Peer->GetHealthBarAuthorization().Generation, NewScope.Generation);
        TestEqual(TEXT("旧revision/Epoch不能覆盖展示"), Remote->GetHealthBarPresentationState().Revision, NewState.Revision);
    }
    if (Case == TEXT("Delivery.ClientAuthority"))
    {
        Peer->SetHealthBarRegion(7,8); Peer->RevokeHealthBarAuthorization();
        TestEqual(TEXT("客户端不能写授权"), Peer->GetHealthBarAuthorization().Generation, Scope.Generation);
        TestFalse(TEXT("客户端不能登记"), Remote->RegisterHealthBarPresentation(7,8)); Remote->UnregisterHealthBarPresentation();
        TestEqual(TEXT("客户端不能撤销服务器登记"), Remote->GetHealthBarPresentationState().Revision, State.Revision);
    }
    if (Case == TEXT("Delivery.ScopeMismatch"))
    {
        auto Other = State; Other.RegionId = 4; Other.RegionSession = 1;
        TestFalse(TEXT("不同区域隐藏"), Scope.Matches(Other));
        auto Private = Scope; Private.RegionId = 4; Private.RegionSession = 2;
        TestFalse(TEXT("同房旧session隐藏"), Private.Matches(Other));
        Other = State; Other.Kind = EY01MonsterPresentationKind::Unknown; TestFalse(TEXT("Unknown隐藏"), Scope.Matches(Other));
        Other = State; Other.WorldEpoch = FGuid::NewGuid(); TestFalse(TEXT("旧世界隐藏"), Scope.Matches(Other));
    }
    if (Case == TEXT("Lifecycle.TravelClient"))
    {
        FHealthBarWorld Next; Next.Start(); Peer->PreClientTravel(TEXT("/Game/Next"), TRAVEL_Relative, true);
        F.World->RemoveController(Peer); Peer->Rename(nullptr, Next.World->PersistentLevel); Next.World->AddController(Peer);
        SendScope(Scope); TestFalse(TEXT("迁移后旧世界包仍隐藏"), Peer->GetHealthBarAuthorization().bEnabled);
        auto NewScope = Scope; NewScope.WorldEpoch = Next.World->GetSubsystem<UY01DamageEvents>()->GetWorldEpoch(); NewScope.Generation += 10;
        SendScope(NewScope); TestTrue(TEXT("新世界可靠快照恢复"), Peer->GetHealthBarAuthorization().bEnabled);
        auto OldScope = Scope; OldScope.Generation += 100; SendScope(OldScope);
        TestTrue(TEXT("已退休Epoch即使代次大也不能回退"), Peer->GetHealthBarAuthorization().WorldEpoch == NewScope.WorldEpoch);
    }
    return true;
}
#endif
