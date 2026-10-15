#include "Misc/AutomationTest.h"
#include "Y01FrameworkTestTypes.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01PlaygroundHUD.h"
#include "Playground/Y01PlaygroundSeed.h"
#include "Playground/Y01PlaygroundPassiveDamageEffect.h"
#include "Playground/Y01TestSkill.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/Canvas.h"
#include "CanvasTypes.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/UnrealType.h"
#include "Net/UnrealNetwork.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
FY01DataRef EQRef(FName Name = TEXT("EQ_0001"))
{ FY01DataRef R; R.Type = EY01TableKind::Equipment; R.RowName = Name; return R; }
void EQBonus(FY01EquipmentRow& Row, float Value)
{ if (auto* P = FindFProperty<FFloatProperty>(FY01EquipmentRow::StaticStruct(), TEXT("AttackBonus"))) { P->SetPropertyValue_InContainer(&Row, Value); } }
// 红阶段也能编译运行；缺少真实入口时返回失败，不制造成功结果。
template <typename T>
auto EQSet(T* PS, const FY01DataCatalog& Catalog, AController* Requester, int32 Slot, const FY01DataRef& Ref, FString& Error, int)
    -> decltype(PS->TrySetEquipmentSlot(Catalog, Requester, Slot, Ref, Error))
{ return PS->TrySetEquipmentSlot(Catalog, Requester, Slot, Ref, Error); }
template <typename T>
bool EQSet(T*, const FY01DataCatalog&, AController*, int32, const FY01DataRef&, FString& Error, ...)
{ Error = TEXT("装备入口尚未实现"); return false; }
struct FEQLog : FOutputDevice
{
    TArray<FString> Lines;
    FEQLog() { GLog->AddOutputDevice(this); }
    ~FEQLog() { GLog->RemoveOutputDevice(this); }
    bool CanBeUsedOnMultipleThreads() const override { return true; }
    void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName&) override
    { if (IsInGameThread() && FString(Text).StartsWith(TEXT("Y01R34 EQ_"))) { Lines.Add(Text); } }
};
struct FEQWorld
{
    TStrongObjectPtr<UY01PlaygroundProfile> Profile{Y01PlaygroundEditor::CreateSeed(GetTransientPackage())};
    UWorld* World;
    AY01PlaygroundGameMode* Mode;
    AY01PlaygroundController* A;
    AY01PlaygroundController* B;
    FEQWorld()
    {
        Profile->bTrainingEnabled = true;
        C()->InitialEquipment.Reset(); C()->InitialSkills.Reset(); C()->AttackDamage = 20; C()->Health = 1000; C()->MoveSpeed = 0;
        C()->R33TestGrowth.bEnabled = false;
        auto* Stage = Profile->Tables[EY01TableKind::TrainingStage]->FindRow<FY01TrainingStageRow>(TEXT("Default"), TEXT("EQ"));
        Stage->Spawns[0].DelaySeconds = 0; Stage->SpawnInterval = 100;
        FY01EquipmentRow Row; EQBonus(Row, 13); ETable()->AddRow(TEXT("EQ_0001"), Row);
        EQBonus(Row, 29); ETable()->AddRow(TEXT("EQ_0002"), Row);
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01PlaygroundGameMode::StaticClass();
        World->GetWorldSettings()->MaxUndilatedFrameTime = 100; World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01PlaygroundGameMode>(); World->SpawnActor<AY01PlaygroundConfig>()->Profile = Profile.Get();
        A = Player(); B = Player(); B->GetPawn()->SetActorLocation(FVector(5000,5000,100));
    }
    ~FEQWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    FY01CharacterRow* C() const { return Profile->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("EQ")); }
    UDataTable* ETable() const { return Profile->Tables[EY01TableKind::Equipment]; }
    FY01EquipmentRow* E(FName Name = TEXT("EQ_0001")) const { return ETable()->FindRow<FY01EquipmentRow>(Name, TEXT("EQ")); }
    AY01PlaygroundController* Player()
    {
        auto* PC = World->SpawnActor<AY01PlaygroundController>(); auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01PlaygroundCharacter>(FVector(0,0,100), FRotator::ZeroRotator);
        PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(PS); return PC;
    }
    AY01PlayerState* PS() const { return A->GetPlayerState<AY01PlayerState>(); }
    UY01CombatAbilitySystem* ASC() const { return PS()->GetCombatASC(); }
    float Base() const { return ASC()->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute()); }
    float Current() const { return ASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()); }
    void Start() { Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay(); }
    void Step(float Dt) { ++GFrameCounter; World->Tick(LEVELTICK_All, Dt); }
    bool Set(FY01DataRef Ref, FString& Error, int32 Slot = 0, AController* Requester = nullptr)
    { return EQSet(PS(), Profile->BuildCatalog(), Requester ? Requester : A, Slot, Ref, Error, 0); }
    int32 Grants() const
    { return ASC()->GetGrantedSources().FilterByPredicate([](const auto& G) { return G.Source.Type == EY01TableKind::Equipment; }).Num(); }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R34EquipmentFields, "Y01.R34.Equipment.Fields", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R34EquipmentFields::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Schema"),TEXT("DefaultZero"),TEXT("Negative"),TEXT("NaN"),TEXT("Infinity"),TEXT("QualityInvalid"),TEXT("QualityMax"),
        TEXT("QualityGreen"),TEXT("QualityBlue"),TEXT("QualityPurple"),TEXT("QualityGold"),TEXT("QualityRed"),TEXT("Zero"),TEXT("Fraction"),TEXT("Initial"),TEXT("InitialInvalidAtomic"),TEXT("InitialOverflow"),TEXT("InitialGERejected"),TEXT("InitialSecondGERejected"),TEXT("ASCThenPlayerState")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R34EquipmentFields::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FEQWorld F;
    if (Case == TEXT("Schema") || Case == TEXT("DefaultZero"))
    {
        auto* P = FindFProperty<FFloatProperty>(FY01EquipmentRow::StaticStruct(), TEXT("AttackBonus"));
        if (TestNotNull(TEXT("装备必须有可配置浮点AttackBonus"), P))
        { FY01EquipmentRow Row; TestEqual(TEXT("旧表兼容默认零"), P->GetPropertyValue_InContainer(&Row), 0.f); }
        TestNull(TEXT("不引入固定装备部位"), FY01EquipmentRow::StaticStruct()->FindPropertyByName(TEXT("Slot"))); return true;
    }
    if (Case == TEXT("ASCThenPlayerState"))
    {
        F.C()->InitialEquipment.Add(EQRef()); FString Error;
        TestTrue(TEXT("直接ASC初始化既有路径"),F.ASC()->TryInitializeFromData(F.Profile->BuildCatalog(),F.Profile->Character,Error));
        TestEqual(TEXT("直接ASC已授予真实攻击"),F.Current(),33.f);
        TestTrue(TEXT("随后PS初始化同步同身份"),F.PS()->TryInitializeCharacter(F.Profile->BuildCatalog(),F.Profile->Character,Error));
        TestEqual(TEXT("PS必须同步已授予实际槽"),F.PS()->GetEquipmentSlots()[0].RowName,FName(TEXT("EQ_0001")));
        TestEqual(TEXT("同步不能再授予"),F.Current(),33.f); return true;
    }
    float Value = Case == TEXT("Negative") ? -1.f : Case == TEXT("NaN") ? std::numeric_limits<float>::quiet_NaN()
        : Case == TEXT("Infinity") ? std::numeric_limits<float>::infinity() : Case == TEXT("Zero") ? 0.f : Case == TEXT("Fraction") ? 2.25f : 13.f;
    const bool BadQuality = Case == TEXT("QualityInvalid") || Case == TEXT("QualityMax");
    if (BadQuality) { F.E()->Quality = static_cast<EY01Quality>(Case == TEXT("QualityMax") ? 5 : 255); }
    int32 Quality = 0;
    for (const TCHAR* N : {TEXT("Green"),TEXT("Blue"),TEXT("Purple"),TEXT("Gold"),TEXT("Red")})
    { if (Case == FString(TEXT("Quality")) + N) { F.E()->Quality = static_cast<EY01Quality>(Quality); } ++Quality; }
    if (Case == TEXT("InitialInvalidAtomic")) { Value = -2; F.C()->InitialEquipment.Add(EQRef(TEXT("EQ_0002"))); }
    if (Case == TEXT("InitialOverflow")) { Value = MAX_flt; F.C()->AttackDamage = MAX_flt; }
    int32 ApplicationCount = 0;
    if (Case == TEXT("InitialSecondGERejected")) { F.C()->InitialEquipment.Add(EQRef(TEXT("EQ_0002"))); }
    if (Case == TEXT("InitialGERejected") || Case == TEXT("InitialSecondGERejected"))
    { F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda([&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { return ++ApplicationCount < (Case == TEXT("InitialGERejected") ? 1 : 2); })); }
    EQBonus(*F.E(), Value); F.C()->InitialEquipment.Add(EQRef());
    const bool Invalid = BadQuality || !FMath::IsFinite(Value) || Value < 0;
    const auto Validation = F.Profile->Validate();
    TestEqual(TEXT("目录验证匹配合法性"), Validation.IsValid(), !Invalid);
    if (Invalid)
    { TestTrue(TEXT("精确装备字段错误"), Validation.Errors.ContainsByPredicate([&](const auto& E)
        { return E.Source.Type == EY01TableKind::Equipment && E.Source.RowName == TEXT("EQ_0001") && E.Field == (BadQuality ? TEXT("Quality") : TEXT("AttackBonus")); })); }
    FString Error;
    const bool Initialized = F.PS()->TryInitializeCharacter(F.Profile->BuildCatalog(), F.Profile->Character, Error);
    const bool InitRejected = Invalid || Case == TEXT("InitialOverflow") || Case == TEXT("InitialGERejected") || Case == TEXT("InitialSecondGERejected");
    TestEqual(TEXT("直接初始化不能绕开数值和品质验证"), Initialized, !InitRejected);
    if (InitRejected)
    { TestEqual(TEXT("无部分属性"), F.Base(), 0.f); TestTrue(TEXT("无部分GE/GA"), F.ASC()->GetGrantedSources().IsEmpty()); TestEqual(TEXT("所有槽未提交"), F.Grants(), 0); TestEqual(TEXT("没有残留GE current"),F.Current(),0.f); }
    else
    { TestEqual(TEXT("装备不改base"), F.Base(), 20.f); TestEqual(TEXT("真实加N不是固定5或品质倍率"), F.Current(), 20.f + Value); TestEqual(TEXT("初始槽身份"), F.PS()->GetEquipmentSlots()[0].RowName, FName(TEXT("EQ_0001"))); }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R34EquipmentRuntime, "Y01.R34.Equipment.Runtime", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R34EquipmentRuntime::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Equip"),TEXT("Unequip"),TEXT("RepeatEquip"),TEXT("RepeatUnequip"),TEXT("Replace"),TEXT("OtherSlots"),TEXT("InitialUnequip"),TEXT("InitialIdempotent"),TEXT("Reinitialize"),
        TEXT("Growth"),TEXT("Snapshot"),TEXT("LegacyInstant"),TEXT("LegacyInfinite"),TEXT("NewEffectRejected"),TEXT("Missing"),TEXT("WrongType"),TEXT("MalformedEmpty"),
        TEXT("InvalidNumber"),TEXT("InvalidQuality"),TEXT("Overflow"),TEXT("Client"),TEXT("OtherPlayer"),TEXT("NullRequester"),TEXT("InvalidSlotNegative"),TEXT("InvalidSlotSix"),
        TEXT("Dead"),TEXT("Ended"),TEXT("Uninitialized"),TEXT("Reentry"),TEXT("RPC"),TEXT("RPCUnequip"),TEXT("RPCClient"),TEXT("RPCDead"),TEXT("RPCEnded"),TEXT("RPCMissing"),TEXT("RPCForeignPawn"),TEXT("GERejected"),TEXT("GrantSnapshot"),TEXT("InvalidEffectPath"),TEXT("DetachedHandle")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R34EquipmentRuntime::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FEQWorld F; FString Error;
    const bool Legacy = Case.StartsWith(TEXT("Legacy"));
    if (Legacy) { F.E()->Effect = Case == TEXT("LegacyInstant") ? UY01TestInstantEffect::StaticClass() : UY01PlaygroundPassiveDamageEffect::StaticClass(); }
    if (Case.StartsWith(TEXT("Initial")) || Legacy) { F.C()->InitialEquipment.Add(EQRef()); }
    if (Case == TEXT("OtherSlots")) { F.C()->InitialEquipment = {EQRef(), EQRef(TEXT("EQ_0002"))}; }
    if (Case == TEXT("Growth")) { F.C()->R33TestGrowth.bEnabled = true; F.C()->R33TestGrowth.AttackPerEffectiveSecond = 3; }
    if (Case == TEXT("Snapshot"))
    { F.Profile->bR34AutoAttackEnabled = true; F.C()->AttackType = EY01AttackType::Ranged; F.C()->Range = 800; F.C()->ProjectileSpeed = 400; F.C()->AttackInterval = 10; }
    if (Case == TEXT("Uninitialized"))
    { TestFalse(TEXT("未初始化拒绝"), F.Set(EQRef(), Error)); TestEqual(TEXT("未初始化无属性写入"), F.Current(), 0.f); return true; }
    F.Start(); if (!TestTrue(TEXT("有效夹具真实初始化"), F.PS()->IsCharacterInitialized())) { return false; }
    if (Legacy)
    {
        const float Expected = Case == TEXT("LegacyInfinite") ? 38.f : 36.f;
        TestEqual(TEXT("旧瞬时GE仍真实写base+3，持续GE不写base"),F.Base(),Case == TEXT("LegacyInstant") ? 23.f : 20.f);
        TestEqual(TEXT("旧Effect与新bonus初始化各一次"), F.Current(), Expected);
        const int32 Before = F.Grants();
        TestFalse(TEXT("含旧Effect槽安全拒绝卸下，防瞬时重复利用"), F.Set(FY01DataRef(), Error));
        TestFalse(TEXT("含旧Effect槽安全拒绝替换"), F.Set(EQRef(TEXT("EQ_0002")), Error));
        TestTrue(TEXT("同身份幂等不重放旧Effect"), F.Set(EQRef(), Error));
        TestEqual(TEXT("旧效果及bonus完整保留"), F.Current(), Expected); TestEqual(TEXT("授予记录不变"), F.Grants(), Before);
        TestEqual(TEXT("重复调试不能利用瞬时效果重复加base"),F.Base(),Case == TEXT("LegacyInstant") ? 23.f : 20.f); return true;
    }
    if (Case.StartsWith(TEXT("RPC")))
    {
        if (Case == TEXT("RPCMissing")) { F.ETable()->RemoveRow(TEXT("EQ_0001")); }
        if (Case == TEXT("RPCDead")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        if (Case == TEXT("RPCEnded")) { TestTrue(TEXT("离开Waiting"), F.Mode->TryStartNextWave()); TestTrue(TEXT("真实结束"), F.Mode->TryEndMatch(false)); }
        if (Case == TEXT("RPCClient")) { F.A->SetRole(ROLE_SimulatedProxy); }
        APawn* SavedPawn = F.A->GetPawn();
        if (Case == TEXT("RPCForeignPawn")) { F.A->SetPawn(F.B->GetPawn()); }
        F.A->ServerPlaygroundAction_Implementation(8);
        const bool Allowed = Case == TEXT("RPC") || Case == TEXT("RPCUnequip");
        TestEqual(TEXT("Action8仅合法拥有者装第1槽"), F.Current(), Allowed ? 33.f : 20.f);
        TestEqual(TEXT("他人无装备变动"), F.B->GetPlayerState<AY01PlayerState>()->GetEquipmentSlots()[0].RowName, NAME_None);
        if (Case == TEXT("RPCUnequip")) { F.A->ServerPlaygroundAction_Implementation(9); TestEqual(TEXT("Action9卸下"), F.Current(), 20.f); }
        F.A->SetRole(ROLE_Authority); if (Case == TEXT("RPCForeignPawn")) { F.A->SetPawn(SavedPawn); }
        return true;
    }
    if (!TestTrue(TEXT("真实装上EQ_0001入口"), F.Set(EQRef(), Error))) { AddInfo(Error); return false; }
    TestEqual(TEXT("初装base不变"), F.Base(), 20.f); TestEqual(TEXT("初装真实加13"), F.Current(), Case == TEXT("OtherSlots") ? 62.f : 33.f);
    if (Case == TEXT("Equip")) { return true; }
    if (Case == TEXT("RepeatEquip") || Case == TEXT("InitialIdempotent"))
    { const int32 N = F.Grants(); for (int I = 0; I < 3; ++I) { TestTrue(TEXT("重复装备成功无副作用"), F.Set(EQRef(), Error)); } TestEqual(TEXT("不叠加"), F.Current(), 33.f); TestEqual(TEXT("不新增记录"), F.Grants(), N); return true; }
    if (Case == TEXT("Reinitialize"))
    { TestTrue(TEXT("先替换"), F.Set(EQRef(TEXT("EQ_0002")), Error)); TestTrue(TEXT("同角色重入初始化"), F.PS()->TryInitializeCharacter(F.Profile->BuildCatalog(), F.Profile->Character, Error)); TestEqual(TEXT("重入不回写初始槽"), F.PS()->GetEquipmentSlots()[0].RowName, FName(TEXT("EQ_0002"))); TestEqual(TEXT("不重新授予"), F.Current(), 49.f); return true; }
    if (Case == TEXT("Replace"))
    { TestTrue(TEXT("替换纯bonus装备"), F.Set(EQRef(TEXT("EQ_0002")), Error)); TestEqual(TEXT("旧GE移除新GE加29"), F.Current(), 49.f); TestEqual(TEXT("只有当前装备记录"), F.Grants(), 1); TestEqual(TEXT("base不变"), F.Base(), 20.f); return true; }
    if (Case == TEXT("Growth")) { F.Step(1.05f); TestEqual(TEXT("成长真实写base"), F.Base(), 23.f); TestEqual(TEXT("装备叠在成长base上"), F.Current(), 36.f); }
    AY01TestAttack* Attack = nullptr; AY01Monster* Target = nullptr;
    if (Case == TEXT("Snapshot"))
    {
        TestTrue(TEXT("进入真实训练房"), F.Mode->GetTraining()->Request(F.A, true)); auto Ms = F.Mode->GetTraining()->Monsters(F.PS());
        if (!TestTrue(TEXT("训练敌人存在"), !Ms.IsEmpty())) { return false; }
        Target = Ms[0]; for (auto* M : Ms) { M->GetCharacterMovement()->DisableMovement(); M->SetActorLocation(F.A->GetPawn()->GetActorLocation()+FVector(2000,0,0)); }
        Target->SetActorLocation(F.A->GetPawn()->GetActorLocation()+FVector(400,0,0)); Target->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(),100); Target->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),100);
        TestTrue(TEXT("真实在途普攻"), AY01TestAttack::TryAutoAttack(F.ASC()));
        for (TActorIterator<AY01TestAttack> It(F.World); It; ++It) { if (It->GetState().Caster == F.A->GetPawn()) { Attack = *It; } }
        if (!TestNotNull(TEXT("真实飞行Actor"), Attack)) { return false; } TestEqual(TEXT("装备后提交快照33"), Attack->GetState().DamageSnapshot, 33.f);
    }
    if (Case == TEXT("Unequip") || Case == TEXT("RepeatUnequip") || Case == TEXT("InitialUnequip") || Case == TEXT("OtherSlots") || Case == TEXT("Growth") || Case == TEXT("Snapshot"))
    {
        TestTrue(TEXT("真实卸下"), F.Set(FY01DataRef(), Error));
        TestEqual(TEXT("只移除第1槽GE不抹成长/他槽"), F.Current(), Case == TEXT("Growth") ? 23.f : Case == TEXT("OtherSlots") ? 49.f : 20.f);
        TestEqual(TEXT("实际第1槽清空"), F.PS()->GetEquipmentSlots()[0].RowName, NAME_None);
        if (Case == TEXT("RepeatUnequip")) { TestTrue(TEXT("重复卸幂等"), F.Set(FY01DataRef(), Error)); TestEqual(TEXT("重复卸不减base"), F.Base(), 20.f); }
        if (Attack) { TestEqual(TEXT("卸下不改已提交快照"), Attack->GetState().DamageSnapshot, 33.f); F.Step(1.01f); TestEqual(TEXT("真实命中仍是旧快照"), Y01Playground::Health(Target->GetCombatASC()), 67.f); }
        return true;
    }
    if (Case == TEXT("GrantSnapshot"))
    { EQBonus(*F.E(),99); TestTrue(TEXT("同ID不热重配"),F.Set(EQRef(),Error)); TestEqual(TEXT("保持原授予值"),F.Current(),33.f); TestTrue(TEXT("卸下原handle不减新表值"),F.Set(FY01DataRef(),Error)); TestEqual(TEXT("精确恢复base"),F.Current(),20.f); return true; }
    if (Case == TEXT("GERejected"))
    { F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda([](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { return false; })); }
    if (Case == TEXT("DetachedHandle"))
    { F.ASC()->RemoveActiveEffects(FGameplayEffectQuery()); }
    FY01DataRef Ref = EQRef(TEXT("EQ_0002")); int32 Slot = 0; AController* Requester = F.A;
    if (Case == TEXT("Missing")) { Ref.RowName = TEXT("Missing"); }
    if (Case == TEXT("WrongType")) { Ref.Type = EY01TableKind::Skill; Ref.RowName = TEXT("Default"); }
    if (Case == TEXT("MalformedEmpty")) { Ref = EQRef(NAME_None); }
    if (Case == TEXT("InvalidNumber")) { EQBonus(*F.E(TEXT("EQ_0002")), std::numeric_limits<float>::quiet_NaN()); }
    if (Case == TEXT("InvalidQuality")) { F.E(TEXT("EQ_0002"))->Quality = static_cast<EY01Quality>(255); }
    if (Case == TEXT("NewEffectRejected")) { F.E(TEXT("EQ_0002"))->Effect = UY01PlaygroundPassiveDamageEffect::StaticClass(); }
    if (Case == TEXT("InvalidEffectPath")) { F.E(TEXT("EQ_0002"))->Effect = TSoftClassPtr<UGameplayEffect>(FSoftObjectPath(TEXT("/Game/Missing.Missing_C"))); }
    if (Case == TEXT("Overflow")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), MAX_flt); EQBonus(*F.E(TEXT("EQ_0002")), MAX_flt); }
    if (Case == TEXT("Client")) { F.PS()->SetRole(ROLE_SimulatedProxy); }
    if (Case == TEXT("OtherPlayer")) { Requester = F.B; }
    if (Case == TEXT("NullRequester")) { Requester = nullptr; }
    if (Case == TEXT("InvalidSlotNegative")) { Slot = -1; }
    if (Case == TEXT("InvalidSlotSix")) { Slot = 6; }
    if (Case == TEXT("Dead")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),0); }
    if (Case == TEXT("Ended")) { TestTrue(TEXT("离开Waiting"), F.Mode->TryStartNextWave()); TestTrue(TEXT("真实Ended"), F.Mode->TryEndMatch(false)); }
    if (Case == TEXT("Reentry"))
    {
        bool Nested = true; int Calls = 0;
        auto H = F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).AddLambda([&](const FOnAttributeChangeData&)
        { ++Calls; FString E; Nested = F.Set(FY01DataRef(), E); });
        TestTrue(TEXT("外层替换成功"), F.Set(Ref, Error)); F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).Remove(H);
        TestTrue(TEXT("真实GE触发属性回调"), Calls > 0); TestFalse(TEXT("重入不允许半提交槽操作"), Nested); TestEqual(TEXT("外层最终槽及属性一致"), F.Current(),49.f); return true;
    }
    const float Base = F.Base(), Current = F.Current(); const int32 Grants = F.Grants();
    TestFalse(TEXT("非法请求必须失败"), EQSet(F.PS(), F.Profile->BuildCatalog(), Requester, Slot, Ref, Error, 0));
    TestFalse(TEXT("失败原因明确"), Error.IsEmpty()); TestEqual(TEXT("失败base原子"), F.Base(), Base); TestEqual(TEXT("失败current原子"), F.Current(), Current);
    TestEqual(TEXT("失败保留原槽"), F.PS()->GetEquipmentSlots()[0].RowName,FName(TEXT("EQ_0001"))); TestEqual(TEXT("失败授予记录原子"),F.Grants(),Grants);
    F.PS()->SetRole(ROLE_Authority); return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R34EquipmentView, "Y01.R34.Equipment.View", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R34EquipmentView::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{ for (const TCHAR* N : {TEXT("Observer"),TEXT("ReadOnly"),TEXT("Replication"),TEXT("HUD"),TEXT("HUDClick"),TEXT("Feedback"),TEXT("Logs")}) { Names.Add(N); Commands.Add(N); } }
bool FY01R34EquipmentView::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter,GFrameCounter); FEQLog Log; FEQWorld F; F.C()->InitialEquipment.Add(EQRef(TEXT("EQ_0002"))); F.Start();
    F.A->ServerPlaygroundAction_Implementation(8); F.A->CaptureObserver(F.Mode); const auto& O = F.A->GetObserver();
    if (Case == TEXT("Logs"))
    {
        F.A->ServerPlaygroundAction_Implementation(9);
        for (const TCHAR* Kind : {TEXT("EQ_CONFIG"),TEXT("EQ_EQUIP"),TEXT("EQ_UNEQUIP")})
        { const auto* L=Log.Lines.FindByPredicate([&](const FString& S){return S.Contains(Kind);});
            if (TestNotNull(FString(TEXT("真实业务日志："))+Kind,L)) { for (const TCHAR* Key : {TEXT("Source="),TEXT("DS="),TEXT("Slot="),TEXT("Row="),TEXT("Quality="),TEXT("AttackBonus="),TEXT("Base="),TEXT("Current=")}) { TestTrue(Key,L->Contains(Key)); } } }
        return true;
    }
    auto* P = FindFProperty<FArrayProperty>(FY01PlaygroundObserver::StaticStruct(), TEXT("Equipment"));
    if (!TestNotNull(TEXT("OwnerOnly快照含实际装备数组"),P)) { return false; }
    auto* Inner = CastField<FStructProperty>(P->Inner); if (!TestNotNull(TEXT("装备观测结构"),Inner)) { return false; }
    FScriptArrayHelper Items(P,P->ContainerPtrToValuePtr<void>(&O)); if (!TestEqual(TEXT("六通用槽实际观测"),Items.Num(),6)) { return false; }
    auto* Row = FindFProperty<FNameProperty>(Inner->Struct,TEXT("Row")); auto* Bonus=FindFProperty<FFloatProperty>(Inner->Struct,TEXT("AttackBonus"));
    auto* Quality=FindFProperty<FEnumProperty>(Inner->Struct,TEXT("Quality"));
    if (!TestNotNull(TEXT("装备ID"),Row) || !TestNotNull(TEXT("装备+N"),Bonus) || !TestNotNull(TEXT("五色品质"),Quality)) { return false; }
    TestEqual(TEXT("第1槽来自实际数组"),Row->GetPropertyValue_InContainer(Items.GetRawPtr(0)),FName(TEXT("EQ_0001")));
    TestEqual(TEXT("观测真实已授予bonus"),Bonus->GetPropertyValue_InContainer(Items.GetRawPtr(0)),13.f);
    TestEqual(TEXT("base观测"),O.AttackBase,20.f); TestEqual(TEXT("current观测"),O.AttackCurrent,33.f);
    if (Case==TEXT("ReadOnly"))
    {
        F.A->SetRole(ROLE_SimulatedProxy); EQBonus(*F.E(),99); F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(),30); F.A->CaptureObserver(F.Mode);
        TestEqual(TEXT("客户端不读取目录重算bonus"),Bonus->GetPropertyValue_InContainer(Items.GetRawPtr(0)),13.f); TestEqual(TEXT("客户端不重算current"),O.AttackCurrent,33.f); F.A->SetRole(ROLE_Authority);
    }
    if (Case==TEXT("Replication"))
    {
        F.A->GetClass()->SetUpRuntimeReplicationData(); TArray<FLifetimeProperty> Props; F.A->GetLifetimeReplicatedProps(Props);
        auto* Observer = FindFProperty<FStructProperty>(F.A->GetClass(),TEXT("Observer"));
        TestTrue(TEXT("只向拥有者复制"),Observer && Props.ContainsByPredicate([&](const auto& R){return R.RepIndex==Observer->RepIndex && R.Condition==COND_OwnerOnly;}));
        TestTrue(TEXT("现有六槽继续复制"),FindFProperty<FProperty>(F.PS()->GetClass(),TEXT("EquipmentSlots"))->HasAnyPropertyFlags(CPF_Net));
    }
    if (Case==TEXT("Feedback"))
    {
        auto* Result=FindFProperty<FStrProperty>(FY01PlaygroundObserver::StaticStruct(),TEXT("EquipmentResult"));
        if (TestNotNull(TEXT("独立装备操作反馈"),Result))
        { TestTrue(TEXT("成功反馈"),Result->GetPropertyValue_InContainer(&O).Contains(TEXT("成功"))); F.ETable()->RemoveRow(TEXT("EQ_0001")); F.A->ServerPlaygroundAction_Implementation(9); F.A->ServerPlaygroundAction_Implementation(8); TestTrue(TEXT("错误引用反馈"),Result->GetPropertyValue_InContainer(&O).Contains(TEXT("失败"))); }
    }
    if (Case.StartsWith(TEXT("HUD")))
    {
        auto* HUD=F.World->SpawnActor<AY01PlaygroundHUD>(); HUD->PlayerOwner=F.A;
        if (Case==TEXT("HUDClick"))
        { HUD->NotifyHitBoxClick(TEXT("EquipmentUnequip")); TestEqual(TEXT("真实Canvas卸装入口"),F.Current(),20.f); HUD->NotifyHitBoxClick(TEXT("EquipmentEquip")); TestEqual(TEXT("真实Canvas装上入口"),F.Current(),33.f); }
        for (float Scale : {1.f,.75f})
        {
            TArray<FString> Texts;
            HUD->ObserveDrawForTest=[&](const FBox2D& B,const FString& Text)
            { Texts.Add(Text); if (B.Min.Y>=205*Scale && B.Min.Y<342*Scale && B.Min.X<458*Scale) { TestTrue(TEXT("装备UI不进入中央/训练面板：")+Text,B.Max.X<=456*Scale && B.Max.Y<=342*Scale); } };
            FCanvas Batch(nullptr,nullptr,F.World,ERHIFeatureLevel::SM5); TStrongObjectPtr<UCanvas> Canvas(NewObject<UCanvas>());
            Canvas->Init(1280*Scale,800*Scale,nullptr,&Batch); Canvas->Update(); HUD->SetCanvas(Canvas.Get(),nullptr); HUD->DrawHUD(); HUD->SetCanvas(nullptr,nullptr);
            const FString All=FString::Join(Texts,TEXT("\n"));
            for (const TCHAR* Token : {TEXT("EQ_0001"),TEXT("装上"),TEXT("卸下"),TEXT("base"),TEXT("current"),TEXT("[6]")}) { TestTrue(Token,All.Contains(Token)); }
            const auto* Equip=HUD->GetHitBoxAtCoordinates(FVector2D(129,286)*Scale,false);
            const auto* Unequip=HUD->GetHitBoxAtCoordinates(FVector2D(339,286)*Scale,false);
            TestTrue(TEXT("装按钮缩放坐标正确"),Equip && Equip->GetName()==TEXT("EquipmentEquip")); TestTrue(TEXT("卸按钮缩放坐标正确"),Unequip && Unequip->GetName()==TEXT("EquipmentUnequip"));
        }
        HUD->ObserveDrawForTest=nullptr;
    }
    return true;
}
#endif
