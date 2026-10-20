#include "Misc/AutomationTest.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01PlaygroundSeed.h"
#include "Playground/Y01TestSkill.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Y01MatchFixture.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "UObject/UnrealType.h"
#include "EngineUtils.h"
#include "Playground/Y01PlaygroundHUD.h"
#include "Engine/Canvas.h"
#include "CanvasTypes.h"
#include "Components/LineBatchComponent.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Net/UnrealNetwork.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
void R34Number(UStruct* Type, void* Row, const TCHAR* Name, double Value)
{
    if (auto* P = FindFProperty<FNumericProperty>(Type, Name))
    { void* V = P->ContainerPtrToValuePtr<void>(Row); if (P->IsFloatingPoint()) { P->SetFloatingPointPropertyValue(V, Value); } else { P->SetIntPropertyValue(V, static_cast<int64>(Value)); } }
    if (auto* P = FindFProperty<FEnumProperty>(Type, Name))
    { P->GetUnderlyingProperty()->SetIntPropertyValue(P->ContainerPtrToValuePtr<void>(Row), static_cast<int64>(Value)); }
}
void R34Enabled(UY01PlaygroundProfile* Profile, bool Enabled)
{ if (auto* P = FindFProperty<FBoolProperty>(Profile->GetClass(), TEXT("bR34AutoAttackEnabled"))) { P->SetPropertyValue_InContainer(Profile, Enabled); } }
struct FR34Log : FOutputDevice
{
    TArray<FString> Lines;
    FR34Log() { GLog->AddOutputDevice(this); }
    ~FR34Log() { GLog->RemoveOutputDevice(this); }
    bool CanBeUsedOnMultipleThreads() const override { return true; }
    void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName&) override
    { if (IsInGameThread() && FString(Text).StartsWith(TEXT("Y01R34 "))) { Lines.Add(Text); } }
    const FString* Find(const FString& Kind, const FString& Identity = TEXT("")) const
    { return Lines.FindByPredicate([&](const FString& L) { return L.StartsWith(TEXT("Y01R34 ") + Kind + TEXT(" ")) && L.Contains(Identity); }); }
};
struct FR34World
{
    TStrongObjectPtr<UY01PlaygroundProfile> Profile{Y01PlaygroundEditor::CreateSeed(GetTransientPackage())};
    UWorld* World;
    AY01PlaygroundGameMode* Mode;
    AY01PlaygroundController* A;
    AY01PlaygroundController* B;
    FR34World()
    {
        Profile->bTrainingEnabled = true; R34Enabled(Profile.Get(), true);
        C()->InitialSkills.Reset(); C()->InitialEquipment.Reset(); C()->Health = 1000; C()->AttackDamage = 20; C()->Range = 250; C()->MoveSpeed = 0;
        for (auto Name : { TEXT("Default"), TEXT("Boss") })
        { auto* R = M(Name); R->Health = 1000; R->AttackDamage = 0; R->MoveSpeed = 0; R->Range = 250; R->Skills.Reset(); }
        auto* Stage = Profile->Tables[EY01TableKind::TrainingStage]->FindRow<FY01TrainingStageRow>(TEXT("Default"), TEXT("R34"));
        Stage->SpawnInterval = 100; Stage->MaxAlive = 6; Stage->Spawns[0].Count = 2; Stage->Spawns[0].DelaySeconds = 0;
        for (const auto& Ref : Profile->OrderedWaves)
        { auto* Wave = Profile->Tables[EY01TableKind::Wave]->FindRow<FY01WaveRow>(Ref.RowName, TEXT("R34")); for (auto& Spawn : Wave->Spawns) { Spawn.DelaySeconds = 0; } }
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
    ~FR34World() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    FY01CharacterRow* C() const { return Profile->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("R34")); }
    FY01MonsterRow* M(FName Name = TEXT("Default")) const { return Profile->Tables[EY01TableKind::Monster]->FindRow<FY01MonsterRow>(Name, TEXT("R34")); }
    void CN(const TCHAR* Name, double V) { R34Number(FY01CharacterRow::StaticStruct(), C(), Name, V); }
    void MN(const TCHAR* Name, double V) { R34Number(FY01MonsterRow::StaticStruct(), M(), Name, V); }
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
    void Step(float Dt) { ++GFrameCounter; World->Tick(LEVELTICK_All, Dt); }
    void Tick(float Seconds) { for (int32 I=0; I<FMath::RoundToInt(Seconds * 100); ++I) { Step(.01f); } }
    TArray<AY01Monster*> Training(AY01PlaygroundController* PC)
    {
        Mode->GetTraining()->Request(PC, true); auto Monsters = Mode->GetTraining()->Monsters(PS(PC));
        for (auto* Monster : Monsters) { Monster->GetCharacterMovement()->DisableMovement(); Monster->SetActorLocation(PC->GetPawn()->GetActorLocation() + FVector(700,0,0)); }
        return Monsters;
    }
    void Place(AActor* Actor, FVector Delta) { Actor->SetActorLocation(A->GetPawn()->GetActorLocation() + Delta); }
    float HP(UY01CombatAbilitySystem* S) const { return Y01Playground::Health(S); }
    TArray<AY01TestAttack*> Attacks() const
    { TArray<AY01TestAttack*> R; for (TActorIterator<AY01TestAttack> It(World); It; ++It) { if (!It->IsActorBeingDestroyed()) { R.Add(*It); } } return R; }
    TArray<AY01TestAttack*> OwnAttacks() const
    { return Attacks().FilterByPredicate([&](auto* Attack) { return Attack->GetState().Caster == A->GetPawn(); }); }
    double Deadline(UY01CombatAbilitySystem* S = nullptr) const
    { auto* P = FindFProperty<FDoubleProperty>(UY01CombatAbilitySystem::StaticClass(), TEXT("NextAutoAttackTime")); return P ? P->GetPropertyValue_InContainer(S ? S : ASC()) : -1; }
    AStaticMeshActor* Wall(FVector Delta)
    {
        auto* W = World->SpawnActor<AStaticMeshActor>(); W->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
        W->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
        Place(W, Delta); W->SetActorScale3D(FVector(.1,3,3)); W->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll")); return W;
    }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36AutoGuard, "Y01.Framework.R36.AutoGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R36AutoGuard::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("SourceStage.Apply"), TEXT("SourceStage.Held"), TEXT("SourceOnlyStage.Apply"),
        TEXT("EquipStage.Apply"), TEXT("EquipStage.Inhibit"), TEXT("EquipStage.Remove"), TEXT("LearnStage.Apply"),
        TEXT("Commit.Equip"), TEXT("Commit.Unequip"), TEXT("Commit.Learn"), TEXT("Commit.Add"), TEXT("Commit.Config"),
        TEXT("Commit.Source"), TEXT("SourceOnlyCommit.Source"), TEXT("Commit.NestedGA"), TEXT("FailedStart"),
        TEXT("Normal"), TEXT("LegacyDisabled"), TEXT("Snapshot"), TEXT("SnapshotDuringStage"),
        TEXT("Death"), TEXT("Leave"), TEXT("Ended"), TEXT("StartDeath"), TEXT("StartEnded")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36AutoGuard::RunTest(const FString& Case)
{
    using K = EY01TableKind; using E = EY01ItemError; using O = EY01ItemOperation;
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FR34World F; FY01MatchFixture Data;
    const bool Legacy = Case == TEXT("LegacyDisabled"), SourceOnly = Case.StartsWith(TEXT("SourceOnly"));
    const bool Flight = Case.StartsWith(TEXT("Snapshot")) || Case == TEXT("Death") || Case == TEXT("Leave") || Case == TEXT("Ended");
    F.C()->AttackInterval = 10;
    if (Flight) { F.C()->AttackType = EY01AttackType::Ranged; F.C()->Range = 800; F.C()->ProjectileSpeed = 400; }
    F.Start(); auto* L = F.PS(F.A)->GetLoadout(); auto* S = F.PS(F.A)->GetSourceEffects(); const FGuid Match = FGuid::NewGuid();
    if (!TestTrue(TEXT("真实Playground配置"), F.Mode->IsMatchConfigured())) { return false; }
    if (!Legacy && !TestTrue(TEXT("显式R36会话"), SourceOnly ? S->Initialize(Match, 1).IsOk() : L->Initialize(Match, 1).IsOk())) { return false; }
    TestTrue(TEXT("进入允许装卸阶段"), F.Mode->TryStartNextWave());
    auto Monsters = F.Training(F.A);
    if (!TestTrue(TEXT("真实个人房合法目标"), !Monsters.IsEmpty())) { return false; }
    auto* Target = Monsters[0]; F.Place(Target, FVector(Flight ? 400 : 100, 0, 0));
    for (int32 I = 1; I < Monsters.Num(); ++I) { F.Place(Monsters[I], FVector(2000, 0, 0)); }
    TestTrue(TEXT("目标合法且在范围内"), F.ASC()->IsAutoAttackInRange(Target)); F.ASC()->SetCombatTarget(Target);
    FY01SourceEffectRow Effect; Effect.Magnitude = 10; Data.Add(K::SourceEffect, Effect);
    FY01EquipmentRow Equipment; Equipment.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect)}; Data.Add(K::Equipment, Equipment);
    FY01SkillRow Skill; Skill.Ability = UY01BasicAttackAbility::StaticClass(); Skill.EffectIds = Equipment.EffectIds; Data.Add(K::Skill, Skill);
    FY01ItemGrant A, B, Book; A.ItemId = FGuid::NewGuid(); A.Definition = FY01MatchFixture::Ref(K::Equipment);
    B = A; B.ItemId = FGuid::NewGuid(); Book.ItemId = FGuid::NewGuid(); Book.Definition = FY01MatchFixture::Ref(K::Skill);
    if (Case == TEXT("EquipStage.Inhibit") || Case == TEXT("EquipStage.Remove"))
    {
        // 等值替换不重建派生攻击GE；换入不同幅度才真实经历旧GE停用与移除回调。
        FY01SourceEffectRow Stronger = Effect; Stronger.Magnitude = 20; Data.Add(K::SourceEffect, Stronger, TEXT("Stronger"));
        FY01EquipmentRow Replacement; Replacement.EffectIds = {FY01MatchFixture::Ref(K::SourceEffect, TEXT("Stronger"))};
        Data.Add(K::Equipment, Replacement, TEXT("Stronger")); B.Definition = FY01MatchFixture::Ref(K::Equipment, TEXT("Stronger"));
    }
    auto View = [&] { FY01LoadoutSnapshot V; L->Observe(V); return V; };
    auto Request = [&](FGuid Id, O Op)
    {
        const auto V = View(); FY01ItemRequest R; R.MatchId = Match; R.JoinSequence = 1; R.Seq = V.ExpectedSeq;
        R.Operation = Op; R.ItemId = Id; R.Slot = 0; FY01ItemInstance Item; L->FindItem(Id, Item); R.ExpectedEntityRevision = Item.EntityRevision;
        if (Op == O::PrepareLearn) { R.ConfigRevision = V.ConfigRevision; }
        else if (!V.EquipmentSlots.IsEmpty())
        {
            R.ExpectedSlotItemId = V.EquipmentSlots[0]; FY01ItemInstance Old; L->FindItem(R.ExpectedSlotItemId, Old);
            R.ExpectedSlotEntityRevision = R.ExpectedSlotItemId.IsValid() ? Old.EntityRevision : 0;
        }
        return R;
    };
    if (!Legacy && !SourceOnly && !TestTrue(TEXT("装备和技能书入包"), L->AddItems(FY01DataCatalog(Data.Tables), {A, B, Book}).IsOk())) { return false; }
    const bool Equipped = Case == TEXT("EquipStage.Inhibit") || Case == TEXT("EquipStage.Remove") || Case == TEXT("Commit.Unequip") || Flight;
    if (Equipped && !TestTrue(TEXT("原装备完整提交"), L->TryEquip(F.A, Request(A.ItemId, O::Equip)).IsOk())) { return false; }
    const auto Equip = Request(B.ItemId, O::Equip), Unequip = Request(A.ItemId, O::Unequip), Learn = Request(Book.ItemId, O::PrepareLearn);
    const auto Before = View(); const double Deadline = F.Deadline(); int32 Calls = 0, Starts = 0;
    auto Blocked = [&]
    {
        ++Calls;
        TestFalse(TEXT("暂存回调不得发动非GA普攻"), F.ASC()->TryAutoAttack(Target, [&](float) { ++Starts; return true; }));
        TestEqual(TEXT("拒绝不调用StartAttack"), Starts, 0);
        TestFalse(TEXT("真实调用端同样拒绝半状态"), AY01TestAttack::TryAutoAttack(F.ASC()));
        TestTrue(TEXT("拒绝不生成攻击Actor"), F.OwnAttacks().IsEmpty()); TestEqual(TEXT("拒绝不推进CD"), F.Deadline(), Deadline);
    };
    auto Prepare = [&](FY01PreparedMutation& Plan)
    { return S->PrepareMutation({{EY01RuntimeSourceKind::Base, TEXT("AutoGuard"), {{TEXT("Flat"), EY01SourceEffectKind::AttackFlat, 7}}}}, {}, Plan); };
    if (Case.Contains(TEXT("Stage")) && !Flight)
    {
        FDelegateHandle H;
        if (Case == TEXT("EquipStage.Inhibit"))
        { H = F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).AddLambda([&](const FOnAttributeChangeData&) { Blocked(); }); }
        else if (Case == TEXT("EquipStage.Remove"))
        { H = F.ASC()->OnAnyGameplayEffectRemovedDelegate().AddLambda([&](const FActiveGameplayEffect&) { Blocked(); }); }
        else if (Case != TEXT("SourceStage.Held"))
        { H = F.ASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda([&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle) { Blocked(); }); }
        if (Case.StartsWith(TEXT("Source")))
        {
            FY01PreparedMutation Plan; FY01SourceTransaction Tx;
            TestTrue(TEXT("真实来源预检"), Prepare(Plan).IsOk()); TestTrue(TEXT("真实来源暂存"), S->StageMutation(Plan, Tx).IsOk());
            if (Case == TEXT("SourceStage.Held")) { Blocked(); }
            TestTrue(TEXT("拒绝攻击不打断原来源提交"), S->CommitMutation(Tx).IsOk());
        }
        else if (Case == TEXT("LearnStage.Apply")) { TestTrue(TEXT("学习完整提交"), L->PrepareLearn(F.A, Learn).IsOk()); }
        else { TestTrue(TEXT("装配完整提交"), L->TryEquip(F.A, Equip).IsOk()); }
        F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).Remove(H);
        F.ASC()->OnAnyGameplayEffectRemovedDelegate().Remove(H); F.ASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(H);
        TestTrue(TEXT("确实进入真实事务回调"), Calls > 0);
        TestTrue(TEXT("事务结束后真实普攻恢复"), AY01TestAttack::TryAutoAttack(F.ASC())); return true;
    }
    if (Case == TEXT("FailedStart"))
    {
        TestFalse(TEXT("StartAttack失败"), F.ASC()->TryAutoAttack(Target, [&](float)
        { ++Calls; TestTrue(TEXT("失败回调也在事务锁内"), L->IsBusy()); return false; }));
        TestEqual(TEXT("实际尝试一次"), Calls, 1); TestEqual(TEXT("失败不推进CD"), F.Deadline(), Deadline);
        TestFalse(TEXT("失败返回释放锁"), L->IsBusy()); TestTrue(TEXT("失败后仍可装配"), L->TryEquip(F.A, Equip).IsOk());
        TestTrue(TEXT("同帧仍能真实普攻"), AY01TestAttack::TryAutoAttack(F.ASC())); return true;
    }
    FY01PreparedMutation SourcePlan;
    if (Case.EndsWith(TEXT(".Source"))) { TestTrue(TEXT("回调前来源计划"), Prepare(SourcePlan).IsOk()); }
    FDelegateHandle SpawnHandle;
    if (Case.Contains(TEXT("Commit.")) || Case == TEXT("StartDeath") || Case == TEXT("StartEnded"))
    {
        SpawnHandle = F.World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor)
        {
            if (!Cast<AY01TestAttack>(Actor) || Actor->GetOwner() != F.PS(F.A)) { return; } ++Calls;
            if (Case == TEXT("StartDeath")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); return; }
            if (Case == TEXT("StartEnded")) { TestTrue(TEXT("发动中真实结束比赛"), F.Mode->TryEndMatch(false)); return; }
            if (!SourceOnly) { TestTrue(TEXT("真实StartAttack生成Actor期间持有Loadout锁"), L->IsBusy()); }
            if (Case == TEXT("Commit.NestedGA"))
            {
                TestFalse(TEXT("非GA提交期间GA也不能重入"), F.ASC()->TryActivateBasicAttack());
                TestEqual(TEXT("嵌套GA不能提前释放外层锁"), L->TryEquip(F.A, Equip).Code, E::Busy);
            }
            if (Case == TEXT("Commit.Equip")) { TestEqual(TEXT("发动中换装Busy"), L->TryEquip(F.A, Equip).Code, E::Busy); }
            if (Case == TEXT("Commit.Unequip")) { TestEqual(TEXT("发动中卸装Busy"), L->TryUnequip(F.A, Unequip).Code, E::Busy); }
            if (Case == TEXT("Commit.Learn")) { TestEqual(TEXT("发动中学习Busy"), L->PrepareLearn(F.A, Learn).Code, E::Busy); }
            if (Case == TEXT("Commit.Add")) { TestEqual(TEXT("发动中入包Busy"), L->AddItems(FY01DataCatalog(Data.Tables), {}).Code, E::Busy); }
            if (Case == TEXT("Commit.Config")) { TestEqual(TEXT("发动中发布配置Busy"), L->SetConfigRevision(2).Code, E::Busy); }
            if (Case.EndsWith(TEXT(".Source")))
            {
                FY01SourceTransaction Tx; const auto R = S->StageMutation(SourcePlan, Tx);
                TestEqual(TEXT("发动中直接来源暂存Busy"), R.Code, EY01RuntimeError::Busy);
                if (R.IsOk()) { TestTrue(TEXT("红阶段收束意外成功的真实事务"), S->CommitMutation(Tx).IsOk()); }
            }
        }));
    }
    const float Damage = F.ASC()->GetUnitStats().AttackDamage;
    TestTrue(TEXT("真实非GA普攻成功发出"), AY01TestAttack::TryAutoAttack(F.ASC())); F.World->RemoveOnActorSpawnedHandler(SpawnHandle);
    TestEqual(TEXT("普攻按独立间隔提交一次CD"), F.Deadline(), F.World->GetTimeSeconds() + 10.);
    TestFalse(TEXT("返回后不残留Loadout锁"), L->IsBusy());
    if (Case.Contains(TEXT("Commit.")))
    {
        TestEqual(TEXT("真实生成回调一次"), Calls, 1);
        if (!SourceOnly)
        {
            const auto After = View(); TestEqual(TEXT("Busy不耗请求序号"), After.ExpectedSeq, Before.ExpectedSeq);
            TestEqual(TEXT("Busy不改库存版本"), After.Revision, Before.Revision); TestEqual(TEXT("Busy不耗书"), After.Items.Num(), Before.Items.Num());
            TestEqual(TEXT("Busy不改配置纪元"), After.ConfigRevision, Before.ConfigRevision);
            TestTrue(TEXT("回调后同请求可装配"), L->TryEquip(F.A, Equip).IsOk());
        }
        TestEqual(TEXT("实际伤害仍使用发动前快照"), F.HP(Target->GetCombatASC()), 1000.f - Damage); return true;
    }
    if (Legacy) { TestFalse(TEXT("未启用R36不隐式初始化注册表"), L->IsInitialized()); }
    if (Case == TEXT("StartDeath") || Case == TEXT("StartEnded"))
    {
        TestEqual(TEXT("中断回调一次"), Calls, 1); F.Step(.01f);
        TestTrue(TEXT("发动中死亡或结束仍清理"), F.OwnAttacks().IsEmpty()); TestEqual(TEXT("中断不伤害目标"), F.HP(Target->GetCombatASC()), 1000.f); return true;
    }
    auto Own = F.OwnAttacks(); if (!TestEqual(TEXT("只产生一份真实普攻"), Own.Num(), 1)) { return false; }
    auto* Attack = Own[0]; TestEqual(TEXT("Actor保存原伤害快照"), Attack->GetState().DamageSnapshot, Damage);
    if (!Flight) { TestEqual(TEXT("近战真实扣血"), F.HP(Target->GetCombatASC()), 1000.f - Damage); return true; }
    if (Case.StartsWith(TEXT("Snapshot")))
    {
        if (Case == TEXT("SnapshotDuringStage"))
        {
            FY01PreparedMutation Plan; FY01SourceTransaction Tx; TestTrue(TEXT("飞行中来源预检"), Prepare(Plan).IsOk());
            TestTrue(TEXT("飞行中来源暂存"), S->StageMutation(Plan, Tx).IsOk()); F.Tick(1.1f);
            TestTrue(TEXT("飞行命中后完整提交来源"), S->CommitMutation(Tx).IsOk());
        }
        else { TestTrue(TEXT("发出后允许卸装"), L->TryUnequip(F.A, Unequip).IsOk()); F.Tick(1.1f); }
        TestEqual(TEXT("已发出攻击不读取后续或半状态攻击力"), F.HP(Target->GetCombatASC()), 1000.f - Damage); return true;
    }
    if (Case == TEXT("Death")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    if (Case == TEXT("Leave")) { TestTrue(TEXT("真实离开个人房"), F.Mode->GetTraining()->Request(F.A, false)); }
    if (Case == TEXT("Ended")) { TestTrue(TEXT("真实比赛结束"), F.Mode->TryEndMatch(false)); }
    F.Step(.01f); TestTrue(TEXT("死亡离区结束清理原攻击"), Attack->IsActorBeingDestroyed());
    TestEqual(TEXT("清理不退已提交CD"), F.Deadline(), 10.); TestTrue(TEXT("不留活动攻击"), F.OwnAttacks().IsEmpty()); return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R34Fields, "Y01.R34.Fields", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R34Fields::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Schema"), TEXT("Defaults"), TEXT("Initialize"), TEXT("DisplayOnly")}) { Names.Add(N); Commands.Add(N); }
    for (const TCHAR* Kind : { TEXT("Character"), TEXT("Monster") })
    {
        for (const TCHAR* Field : { TEXT("AttackInterval"), TEXT("MeleeAngle"), TEXT("ProjectileSpeed"), TEXT("Armor"), TEXT("HealthRegen") })
        { for (const TCHAR* Bad : { TEXT("Negative"), TEXT("NaN"), TEXT("Infinity") })
            { FString N = FString(Kind) + TEXT(".") + Field + TEXT(".") + Bad; Names.Add(N); Commands.Add(N); } }
        for (const TCHAR* Extra : { TEXT("AttackType.Invalid"), TEXT("AttackInterval.Zero"), TEXT("MeleeAngle.Zero"), TEXT("MeleeAngle.Over"), TEXT("ProjectileSpeed.Zero") })
        { FString N = FString(Kind) + TEXT(".") + Extra; Names.Add(N); Commands.Add(N); }
    }
    for (const TCHAR* Field : { TEXT("Strength"), TEXT("Agility"), TEXT("Wisdom") })
    { for (const TCHAR* Bad : { TEXT("Negative"), TEXT("NaN"), TEXT("Infinity") })
        { FString N = FString(TEXT("Character.")) + Field + TEXT(".") + Bad; Names.Add(N); Commands.Add(N); } }
}
bool FY01R34Fields::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FR34World F;
    if (Case == TEXT("Schema"))
    {
        for (auto* Type : { FY01CharacterRow::StaticStruct(), FY01MonsterRow::StaticStruct() })
        {
            for (const TCHAR* Name : { TEXT("Health"), TEXT("AttackDamage"), TEXT("Range"), TEXT("MoveSpeed"), TEXT("AttackInterval"), TEXT("MeleeAngle"), TEXT("ProjectileSpeed"), TEXT("Armor"), TEXT("HealthRegen") })
            { TestNotNull(FString(TEXT("统一数值字段 ")) + Type->GetName() + TEXT(".") + Name, FindFProperty<FFloatProperty>(Type, Name)); }
            auto* Enum = FindFProperty<FEnumProperty>(Type, TEXT("AttackType"));
            if (TestNotNull(TEXT("近远程显式枚举"), Enum))
            { TestEqual(TEXT("近战枚举身份"), Enum->GetEnum()->GetValueByNameString(TEXT("Melee")), int64(0)); TestEqual(TEXT("远程枚举身份"), Enum->GetEnum()->GetValueByNameString(TEXT("Ranged")), int64(1)); }
        }
        for (const TCHAR* N : { TEXT("Strength"), TEXT("Agility"), TEXT("Wisdom") }) { TestNotNull(N, FindFProperty<FFloatProperty>(FY01CharacterRow::StaticStruct(), N)); }
        return true;
    }
    if (Case == TEXT("Defaults"))
    {
        TStrongObjectPtr<UY01PlaygroundProfile> Old(Y01PlaygroundEditor::CreateSeed(GetTransientPackage()));
        auto* P = FindFProperty<FBoolProperty>(Old->GetClass(), TEXT("bR34AutoAttackEnabled"));
        if (TestNotNull(TEXT("必须有显式R34兼容开关"), P)) { TestFalse(TEXT("旧Profile默认禁用"), P->GetPropertyValue_InContainer(Old.Get())); }
        TestTrue(TEXT("旧内存目录新增字段默认合法"), Old->Validate().IsValid()); return true;
    }
    if (Case == TEXT("Initialize") || Case == TEXT("DisplayOnly"))
    {
        F.CN(TEXT("Armor"), 7); F.CN(TEXT("HealthRegen"), 3);
        F.CN(TEXT("Strength"), 900); F.CN(TEXT("Agility"), 800); F.CN(TEXT("Wisdom"), 700);
        F.Start(); TestTrue(TEXT("真实角色初始化"), F.ASC()->IsCombatInitialized());
        if (Case == TEXT("Initialize"))
        {
            for (const TCHAR* N : {TEXT("Armor"), TEXT("HealthRegen")})
            { auto* P = FindFProperty<FStructProperty>(UY01AttributeSet::StaticClass(), N);
                if (TestNotNull(TEXT("运行时GAS属性：") + FString(N), P)) { TestEqual(TEXT("真实表值初始化"), F.ASC()->GetNumericAttribute(FGameplayAttribute(P)), FString(N) == TEXT("Armor") ? 7.f : 3.f); } }
        }
        TestEqual(TEXT("三维不影响攻击"), F.ASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), 20.f);
        TestEqual(TEXT("三维不影响HP"), F.HP(F.ASC()), 1000.f); TestEqual(TEXT("三维不影响移速"), F.ASC()->GetInitialMoveSpeed(), 0.f); return true;
    }
    TArray<FString> Parts; Case.ParseIntoArray(Parts, TEXT("."));
    if (Parts.Num() != 3) { AddError(TEXT("未知测试身份")); return false; }
    const double Bad = Parts[2] == TEXT("NaN") ? std::numeric_limits<double>::quiet_NaN() : Parts[2] == TEXT("Infinity") ? std::numeric_limits<double>::infinity()
        : Parts[2] == TEXT("Zero") ? 0 : Parts[2] == TEXT("Over") ? 360.01 : Parts[2] == TEXT("Invalid") ? 99 : -1;
    const bool Character = Parts[0] == TEXT("Character");
    if (Character) { F.CN(*Parts[1], Bad); } else { F.MN(*Parts[1], Bad); }
    TestFalse(TEXT("非法统一字段必须被目录拒绝：") + Case, F.Profile->Validate().IsValid());
    auto* Monster = F.World->SpawnActor<AY01Monster>(); auto* ASC = Character ? F.ASC() : Monster->GetCombatASC();
    if (!Character) { ASC->InitAbilityActorInfo(Monster, Monster); }
    FY01DataRef Ref = F.Profile->Character; if (!Character) { Ref.Type = EY01TableKind::Monster; }
    FString Error; TestFalse(TEXT("直接ASC初始化不能绕过字段校验"), ASC->TryInitializeFromData(F.Profile->BuildCatalog(), Ref, Error));
    TestTrue(TEXT("拒绝无半套授予"), ASC->GetGrantedSources().IsEmpty()); TestEqual(TEXT("拒绝无HP写入"), F.HP(ASC), 0.f); return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R34Combat, "Y01.R34.Combat", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R34Combat::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Auto.Player"), TEXT("Auto.TrainingMonster"), TEXT("Auto.WaveMonster"), TEXT("Auto.Boss"),
        TEXT("Auto.Independent"), TEXT("Auto.NoTarget"), TEXT("Auto.Interval"), TEXT("Auto.LongTick"), TEXT("Auto.NoChase"),
        TEXT("Auto.Switch"), TEXT("Auto.Disabled"), TEXT("Auto.NearestXY"), TEXT("Auto.Tie"), TEXT("Auto.NoSlots"),
        TEXT("Sector.Radius"), TEXT("Sector.RadiusOutside"), TEXT("Sector.Angle"), TEXT("Sector.AngleOutside"),
        TEXT("Sector.Height"), TEXT("Sector.HeightOutside"), TEXT("Sector.Below"), TEXT("Sector.Multi"), TEXT("Sector.Once"),
        TEXT("Sector.WindowEntry"), TEXT("Sector.Fixed"), TEXT("Sector.Wall"), TEXT("Sector.ShortWindow"), TEXT("Sector.FullAngle"),
        TEXT("Ball.Specified"), TEXT("Ball.OtherUnit"), TEXT("Ball.Wall"), TEXT("Ball.MaxTravel"), TEXT("Ball.MovingMiss"),
        TEXT("Ball.Sweep"), TEXT("Ball.TargetDeath"), TEXT("Ball.TargetSession"), TEXT("Ball.SourceDeath"), TEXT("Ball.SourceSession"),
        TEXT("Ball.Ended"), TEXT("Ball.Logout"), TEXT("Ball.Snapshot"), TEXT("Ball.ArmorAtHit"), TEXT("Ball.Feedback"), TEXT("Ball.Forged"),
        TEXT("Armor.Subtract"), TEXT("Armor.ZeroFloor"), TEXT("Armor.LegacyRange"), TEXT("Armor.Clamp"),
        TEXT("Regen.Waiting"), TEXT("Regen.Preparing"), TEXT("Regen.Battle"), TEXT("Regen.Training"), TEXT("Regen.Monster"),
        TEXT("Regen.Clamp"), TEXT("Regen.Death"), TEXT("Regen.Ended"), TEXT("Regen.Logout"), TEXT("Regen.DamageContinuous"),
        TEXT("Regen.LongTick"), TEXT("Regen.NoOffline"), TEXT("Isolation.Room"), TEXT("Isolation.Main"), TEXT("Isolation.Faction"),
        TEXT("Kill.Training"), TEXT("Kill.Main"), TEXT("Kill.Boss"), TEXT("Skill.Independent") })
    { Names.Add(N); Commands.Add(N); }
    for (const TCHAR* N : {TEXT("Regen.DisplayMerge"), TEXT("Regen.DisplayFraction"), TEXT("Regen.DisplaySessionReset"),
        TEXT("Regen.DisplayRegionReset"), TEXT("Regen.DisplayDeathReset"), TEXT("Regen.DisplayDirectSet"),
        TEXT("Display.TestSkillOverkill")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R34Combat::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FR34World F;
    const bool Ball = Case.StartsWith(TEXT("Ball."));
    F.CN(TEXT("AttackInterval"), Ball ? 10 : 1); F.CN(TEXT("MeleeAngle"), 90);
    if (Ball) { F.CN(TEXT("AttackType"), 1); F.C()->Range = 800; F.CN(TEXT("ProjectileSpeed"), Case == TEXT("Ball.Sweep") ? 40000 : 400); }
    if (Case == TEXT("Sector.ShortWindow")) { F.CN(TEXT("AttackInterval"), .1); }
    if (Case == TEXT("Sector.FullAngle")) { F.CN(TEXT("MeleeAngle"), 360); }
    if (Case == TEXT("Display.TestSkillOverkill")) { F.C()->AttackDamage = 35; }
    const bool Enemy = Case == TEXT("Auto.TrainingMonster") || Case == TEXT("Auto.WaveMonster") || Case == TEXT("Auto.Boss");
    if (Enemy) { F.C()->AttackDamage = 0; F.C()->Range = 0; F.M()->AttackDamage = 7; F.M(TEXT("Boss"))->AttackDamage = 11; }
    if (Case == TEXT("Auto.Disabled")) { R34Enabled(F.Profile.Get(), false); }
    if (Case.StartsWith(TEXT("Regen.")))
    {
        F.CN(TEXT("HealthRegen"), Case == TEXT("Regen.DisplayFraction") ? .6 : 3);
        F.MN(TEXT("HealthRegen"), 4); F.C()->AttackDamage = 0;
    }
    if (Case.StartsWith(TEXT("Kill."))) { F.C()->R33TestGrowth.bEnabled = true; F.C()->R33TestGrowth.AttackPerEffectiveKill = 2; }
    if (Case == TEXT("Skill.Independent"))
    { FY01DataRef R; R.Type = EY01TableKind::Skill; R.RowName = TEXT("BasicAttack"); F.C()->InitialSkills = {R}; }
    F.Start(); if (!TestTrue(TEXT("真实配置与角色初始化"), F.Mode->IsMatchConfigured() && F.ASC()->IsCombatInitialized())) { return false; }
    if (Case.StartsWith(TEXT("Regen.")))
    {
        UY01CombatAbilitySystem* S = F.ASC();
        if (Case == TEXT("Regen.Preparing") || Case == TEXT("Regen.Ended")) { F.Mode->TryStartNextWave(); }
        if (Case == TEXT("Regen.Battle")) { F.Mode->TryStartNextWave(); F.Tick(5.1f); }
        if (Case == TEXT("Regen.Training") || Case == TEXT("Regen.Monster"))
        { auto Ms = F.Training(F.A); if (!TestEqual(TEXT("两只训练怪"), Ms.Num(), 2)) { return false; } if (Case == TEXT("Regen.Monster")) { S = Ms[0]->GetCombatASC(); } }
        S->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), Case == TEXT("Regen.Death") ? 0 : Case == TEXT("Regen.Clamp") ? 999 : 500);
        if (Case.StartsWith(TEXT("Regen.Display")))
        {
            TArray<FY01DamageEvent> Events;
            auto* Service = F.World->GetSubsystem<UY01DamageEvents>();
            const auto Handle = Service->OnDamageCommitted.AddLambda([&](const FY01DamageEvent& E) { Events.Add(E); });
            if (Case == TEXT("Regen.DisplayMerge"))
            {
                F.Tick(.5f); TestEqual(TEXT("自然回血合并窗内不提前出字"), Events.Num(), 0);
                F.Tick(.5f); TestTrue(TEXT("一秒内真实自然回血累计3"), FMath::IsNearlyEqual(F.HP(S), 503.f, .01f));
            }
            else if (Case == TEXT("Regen.DisplayFraction"))
            {
                F.Tick(1); TestEqual(TEXT("不足1的自然回血保留小数但不出字"), Events.Num(), 0);
                F.Tick(1); TestTrue(TEXT("小数跨合并窗真实累计"), FMath::IsNearlyEqual(F.HP(S), 501.2f, .01f));
            }
            else
            {
                F.Tick(.5f); TestEqual(TEXT("重置前半窗不提前出字"), Events.Num(), 0);
                if (Case == TEXT("Regen.DisplaySessionReset")) { S->InvalidateCombatSession(); }
                if (Case == TEXT("Regen.DisplayRegionReset")) { TestTrue(TEXT("真实切入训练区"), F.Mode->GetTraining()->Request(F.A, true)); }
                if (Case == TEXT("Regen.DisplayDeathReset"))
                {
                    S->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0);
                    S->UpdateHealthRegen(true);
                    S->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 500);
                    S->UpdateHealthRegen(true);
                }
                if (Case == TEXT("Regen.DisplayDirectSet"))
                {
                    S->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 600);
                    TestEqual(TEXT("管理或测试直接加血不产生治疗字"), Events.Num(), 0);
                    Service->OnDamageCommitted.Remove(Handle); return true;
                }
                F.Tick(.5f); TestEqual(TEXT("重置后不能串入前半窗"), Events.Num(), 0);
                F.Tick(.51f);
            }
            if (TestEqual(TEXT("有效自然回血按窗口合并为一个事件"), Events.Num(), 1))
            {
                TestEqual(TEXT("整数显示累积小数且不冒充实际伤害"), Events[0].ActualDamage, 0.f);
                TestEqual(TEXT("自然回血明确为治疗"), Events[0].Type, EY01DamageEventType::Healing);
                TestEqual(TEXT("合并显示量"), Events[0].DisplayAmount, Case == TEXT("Regen.DisplayFraction") ? 1.f : 3.f);
            }
            Service->OnDamageCommitted.Remove(Handle); return true;
        }
        if (Case == TEXT("Regen.Ended")) { F.Mode->TryEndMatch(false); }
        if (Case == TEXT("Regen.Logout")) { F.Mode->Logout(F.A); }
        if (Case == TEXT("Regen.NoOffline"))
        {
            APawn* Pawn = F.A->GetPawn(); F.A->UnPossess(); F.Tick(2); F.A->Possess(Pawn); F.Step(.01f);
            TestTrue(TEXT("离线区间绝不补发"), F.HP(S) <= 500.04f); F.Tick(1); TestTrue(TEXT("恢复有效在线后持续回血"), F.HP(S) >= 502.9f); return true;
        }
        if (Case == TEXT("Regen.LongTick")) { F.Step(10); TestEqual(TEXT("长Tick按有效DS时间回血"), F.HP(S), 530.f); return true; }
        F.Tick(.5f);
        if (Case == TEXT("Regen.DamageContinuous")) { S->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), F.HP(S)-10); }
        F.Tick(.5f);
        const float Expected = Case == TEXT("Regen.Death") ? 0 : Case == TEXT("Regen.Ended") || Case == TEXT("Regen.Logout") ? 500
            : Case == TEXT("Regen.Clamp") ? 1000 : Case == TEXT("Regen.Monster") ? 504 : Case == TEXT("Regen.DamageContinuous") ? 493 : 503;
        TestTrue(TEXT("生命变化符合阶段、死亡、上限与连续DS时间"), FMath::IsNearlyEqual(F.HP(S), Expected, .01f)); return true;
    }
    const bool Main = Case == TEXT("Auto.WaveMonster") || Case == TEXT("Auto.Boss") || Case == TEXT("Kill.Main") || Case == TEXT("Kill.Boss");
    TArray<AY01Monster*> Ms;
    if (Main)
    {
        F.Mode->TryStartNextWave(); F.Tick(5.1f);
        if (Case.EndsWith(TEXT("Boss")))
        {
            for (AY01Monster* M : F.Mode->GetWaveMonsters()) { M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
            F.Mode->TryCompleteWave(); F.Mode->TryStartNextWave(); F.Tick(8.1f);
        }
        for (AY01Monster* M : F.Mode->GetWaveMonsters()) { Ms.Add(M); M->GetCharacterMovement()->DisableMovement(); F.Place(M, FVector(700,0,0)); }
    }
    else { Ms = F.Training(F.A); }
    if (!TestTrue(TEXT("真实区域怪物"), Ms.Num() >= 1)) { return false; }
    auto* M = Ms[0]; auto* N = Ms.Num()>1 ? Ms[1] : M; auto* T = M->GetCombatASC();
    F.Place(M, FVector(Ball ? 400 : 50,0,0));
    if (Case == TEXT("Display.TestSkillOverkill"))
    {
        TArray<FY01DamageEvent> Events; auto* Service = F.World->GetSubsystem<UY01DamageEvents>();
        const auto Handle = Service->OnDamageCommitted.AddLambda([&](const FY01DamageEvent& E) { Events.Add(E); });
        T->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 25); F.Step(.01f);
        TestEqual(TEXT("测试技能执行器过量伤害只扣剩余生命"), F.HP(T), 0.f);
        if (TestEqual(TEXT("测试技能执行器只发布一个事件"), Events.Num(), 1))
        {
            TestEqual(TEXT("测试技能执行器Actual是真实血差"), Events[0].ActualDamage, 25.f);
            TestEqual(TEXT("测试技能执行器Display保留完整结算量"), Events[0].DisplayAmount, 35.f);
            TestEqual(TEXT("测试技能执行器事件类型为伤害"), Events[0].Type, EY01DamageEventType::Damage);
        }
        Service->OnDamageCommitted.Remove(Handle); return true;
    }
    if (Case == TEXT("Auto.Disabled")) { F.Tick(2); TestEqual(TEXT("旧Profile无自动攻击"), F.HP(T), 1000.f); TestTrue(TEXT("无自动Actor"), F.OwnAttacks().IsEmpty()); return true; }
    if (Case.StartsWith(TEXT("Armor.")))
    {
        T->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), Case == TEXT("Armor.ZeroFloor") ? 30 : 7);
        if (Case == TEXT("Armor.Clamp")) { T->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), -9); TestEqual(TEXT("护甲非负"), T->GetNumericAttribute(UY01AttributeSet::GetArmorAttribute()), 0.f); return true; }
        if (Case == TEXT("Armor.LegacyRange")) { F.Place(M, FVector(600,0,0)); }
        auto Spec = F.ASC()->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, F.ASC()->MakeEffectContext()); Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, 20);
        F.ASC()->ApplyGameplayEffectSpecToTarget(*Spec.Data, T);
        TestEqual(TEXT("真实GE减甲且保留旧距离保护"), F.HP(T), Case == TEXT("Armor.Subtract") ? 987.f : 1000.f); return true;
    }
    if (Enemy)
    {
        const float Before = F.HP(F.ASC()); F.Step(.01f);
        TestEqual(TEXT("怪无任何技能槽仍真实自动攻击玩家"), F.HP(F.ASC()), Before - (Case == TEXT("Auto.Boss") ? 11 : 7));
        TestTrue(TEXT("怪有独立普攻截止"), F.Deadline(T) > F.World->GetTimeSeconds()); return true;
    }
    if (Case == TEXT("Auto.NoTarget"))
    { F.Place(M, FVector(700,0,0)); F.Step(10); TestEqual(TEXT("空目标不消费间隔"), F.Deadline(), 0.); TestTrue(TEXT("空目标无Actor"), F.OwnAttacks().IsEmpty()); F.Place(M, FVector(50,0,0)); }
    if (Case == TEXT("Auto.NearestXY")) { F.Place(M, FVector(100,0,120)); F.Place(N, FVector(130,0,0)); }
    if (Case == TEXT("Auto.Tie")) { F.Place(M, FVector(100,0,0)); F.Place(N, FVector(-100,0,0)); }
    if (Case.StartsWith(TEXT("Isolation.")))
    {
        auto Other = F.Training(F.B); if (!TestEqual(TEXT("B独立房"), Other.Num(), 2)) { return false; }
        F.PS(F.B)->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 0);
        F.Place(Other[0], FVector(10,0,0)); F.Place(F.B->GetPawn(), FVector(15,0,0));
        if (Case == TEXT("Isolation.Main")) { F.Mode->TryStartNextWave(); F.Tick(5.1f); for (AY01Monster* W : F.Mode->GetWaveMonsters()) { F.Place(W, FVector(20,0,0)); } }
        const float H = F.HP(Other[0]->GetCombatASC()); F.Step(.01f);
        TestEqual(TEXT("同位置另一房敌人不受伤"), F.HP(Other[0]->GetCombatASC()), H);
        TestEqual(TEXT("同阵营玩家不能成为普攻受害者"), F.HP(F.PS(F.B)->GetCombatASC()), 1000.f);
        TestTrue(TEXT("本房合法怪仍自动受伤"), F.HP(T) < 1000); return true;
    }
    if (Case.StartsWith(TEXT("Sector.")))
    {
        FVector Offset(200,0,0);
        if (Case == TEXT("Sector.Radius")) { Offset.X = 250; }
        if (Case == TEXT("Sector.RadiusOutside")) { Offset.X = 250.01; }
        if (Case == TEXT("Sector.Angle")) { Offset = FVector(200,200,0).GetSafeNormal()*200; }
        if (Case == TEXT("Sector.AngleOutside")) { Offset = FVector(199,201,0).GetSafeNormal()*200; }
        if (Case == TEXT("Sector.Height")) { Offset.Z = 120; }
        if (Case == TEXT("Sector.HeightOutside")) { Offset.Z = 120.01; }
        if (Case == TEXT("Sector.Below")) { Offset.Z = -120.01; }
        if (Case == TEXT("Sector.WindowEntry") || Case == TEXT("Sector.Fixed")) { Offset.X = 400; }
        if (Case == TEXT("Sector.FullAngle")) { Offset.X = -200; }
        F.Place(N, Offset); if (Case == TEXT("Sector.Wall")) { F.Wall(FVector(150,0,0)); }
    }
    if (Case.StartsWith(TEXT("Kill."))) { T->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 20); }
    const int32 Gold = F.PS(F.A)->GetGold(); const int64 Kills = F.Mode->GetGrowth()->Observe(F.PS(F.A)).GrowthKills;
    const FVector Origin = F.A->GetPawn()->GetActorLocation(); F.Step(.01f);
    if (Case.StartsWith(TEXT("Kill.")))
    {
        TestEqual(TEXT("自动普攻真实击杀"), F.HP(T), 0.f); TestEqual(TEXT("合法GE成长只触发一次"), F.Mode->GetGrowth()->Observe(F.PS(F.A)).GrowthKills, Kills+1);
        TestEqual(TEXT("击杀攻击成长"), F.ASC()->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute()), 22.f);
        F.Tick(.1f); TestTrue(TEXT("训练或末怪波次产生奖励"), F.PS(F.A)->GetGold() > Gold || (Main && Ms.Num()>1)); return true;
    }
    auto Own = F.OwnAttacks(); if (!TestEqual(TEXT("无需技能键生成真实自动攻击Actor"), Own.Num(), 1)) { return false; }
    auto* Attack = Own[0]; const double Deadline = F.Deadline();
    TestTrue(TEXT("成功开启才消费本单位间隔"), FMath::IsNearlyEqual(Deadline, F.World->GetTimeSeconds() + (Ball ? 10 : Case == TEXT("Sector.ShortWindow") ? .1 : 1), .00001));
    TestEqual(TEXT("不消费槽0CD"), F.ASC()->GetSkillCooldownRemaining(0), 0.f);
    if (Case == TEXT("Auto.NoSlots")) { TestTrue(TEXT("无技能授予"), F.ASC()->GetInitialSkills().IsEmpty()); TestEqual(TEXT("真实HP减少"), F.HP(T), 980.f); }
    if (Case == TEXT("Auto.NearestXY")) { TestTrue(TEXT("最近按XY不被Z改变方向"), Attack->GetState().Direction.Equals(FVector(1,0,0), .00001)); }
    if (Case == TEXT("Auto.Tie")) { TestTrue(TEXT("平距稳定选择一个方向"), FMath::Abs(Attack->GetState().Direction.X) > .999); }
    if (Case == TEXT("Auto.Independent"))
    { F.Place(N, FVector(0,100,0)); F.Step(.2f); TestTrue(TEXT("怪的普攻时钟与玩家独立"), F.Deadline(N->GetCombatASC()) != Deadline); }
    if (Case == TEXT("Auto.Interval") || Case == TEXT("Auto.NoTarget") || Case == TEXT("Auto.NoChase"))
    { F.Tick(.8f); TestEqual(TEXT("间隔内每敌仅一次"), F.HP(T), 980.f); TestTrue(TEXT("玩家无追击位移"), F.A->GetPawn()->GetActorLocation().Equals(Origin)); F.Tick(.22f); TestEqual(TEXT("间隔到再次真实扣血"), F.HP(T), 960.f); }
    if (Case == TEXT("Auto.LongTick")) { F.Step(10); TestEqual(TEXT("长Tick只出一次不补积攒攻击"), F.HP(T), 960.f); TestTrue(TEXT("next基于当前DS时间"), FMath::IsNearlyEqual(F.Deadline(), F.World->GetTimeSeconds()+1,.00001)); }
    if (Case == TEXT("Auto.Switch"))
    { F.Mode->GetTraining()->Request(F.A,false); TestTrue(TEXT("切区立即取消"), Attack->IsActorBeingDestroyed()); TestEqual(TEXT("切区保留next"), F.Deadline(), Deadline); F.Training(F.A); F.Step(.01f); TestTrue(TEXT("切区不刷新普攻"), F.OwnAttacks().IsEmpty()); }
    if (Case == TEXT("Skill.Independent"))
    { F.ASC()->SetCombatTarget(M); TestTrue(TEXT("普攻期间仍可手动技能"), F.PS(F.A)->TryActivateSkillSlot(0)); TestEqual(TEXT("手动不改普攻next"), F.Deadline(), Deadline); TestEqual(TEXT("技能与普攻分别扣血"), F.HP(T), 960.f); }
    if (Case.StartsWith(TEXT("Sector.")))
    {
        bool Hit = !Case.Contains(TEXT("Outside")) && Case != TEXT("Sector.Below") && Case != TEXT("Sector.WindowEntry") && Case != TEXT("Sector.Fixed");
        TestEqual(TEXT("立即几何判定含边界、全额不分摊"), F.HP(N->GetCombatASC()), Hit ? 980.f : 1000.f);
        if (Case == TEXT("Sector.WindowEntry")) { F.Place(N,FVector(200,0,0)); Hit = true; }
        if (Case == TEXT("Sector.Fixed")) { F.A->GetPawn()->SetActorLocation(Origin+FVector(0,500,0)); N->SetActorLocation(Origin+FVector(200,0,0)); Hit = true; }
        if (Case == TEXT("Sector.ShortWindow"))
        { TestTrue(TEXT("窗口=min(.35,间隔)"), FMath::IsNearlyEqual(Attack->GetState().EndsAt-Attack->GetState().StartedAt,.1,.00001)); F.Place(M,FVector(700,0,0)); F.Place(N,FVector(700,200,0)); F.Tick(.11f); TestTrue(TEXT("短窗口到期消失"), Attack->IsActorBeingDestroyed()); return true; }
        F.Tick(.3f); TestEqual(TEXT("窗口进入有效且每敌一次"), F.HP(N->GetCombatASC()), Hit ? 980.f : 1000.f); F.Tick(.06f); TestTrue(TEXT(".35秒窗结束消失"), Attack->IsActorBeingDestroyed()); return true;
    }
    if (!Ball) { return true; }
    TestEqual(TEXT("远程出手无提前伤害"), F.HP(T), 1000.f); TestEqual(TEXT("球真实半径12"), Attack->GetState().Config.Radius, 12.f);
    TestTrue(TEXT("寿命严格Range/Speed"), FMath::IsNearlyEqual(Attack->GetState().EndsAt-Attack->GetState().StartedAt, 800./Attack->GetState().Config.Speed, .00001));
    if (Case == TEXT("Ball.Specified") || Case == TEXT("Ball.OtherUnit")) { F.Place(N,FVector(200,0,0)); }
    if (Case == TEXT("Ball.Wall")) { F.Wall(FVector(200,0,0)); }
    if (Case == TEXT("Ball.MovingMiss")) { F.Place(M,FVector(400,250,0)); }
    if (Case == TEXT("Ball.MaxTravel")) { F.Place(M,FVector(1000,0,0)); }
    if (Case == TEXT("Ball.Snapshot")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 200); }
    if (Case == TEXT("Ball.ArmorAtHit")) { T->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), 7); }
    if (Case == TEXT("Ball.Forged"))
    { auto C = F.ASC()->MakeEffectContext(); C.AddSourceObject(Attack); auto Spec = F.ASC()->MakeOutgoingSpec(UY01TestSkillDamageEffect::StaticClass(),1,C); F.ASC()->ApplyGameplayEffectSpecToTarget(*Spec.Data,T); TestEqual(TEXT("攻击Actor来源仍不能绕过最终几何授权"),F.HP(T),1000.f); }
    const bool Cancel = Case == TEXT("Ball.TargetDeath") || Case == TEXT("Ball.TargetSession") || Case == TEXT("Ball.SourceDeath") || Case == TEXT("Ball.SourceSession") || Case == TEXT("Ball.Ended") || Case == TEXT("Ball.Logout");
    if (Case == TEXT("Ball.TargetDeath")) { T->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),0); }
    if (Case == TEXT("Ball.TargetSession")) { T->InvalidateCombatSession(); }
    if (Case == TEXT("Ball.SourceDeath")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),0); }
    if (Case == TEXT("Ball.SourceSession")) { F.ASC()->InvalidateCombatSession(); }
    if (Case == TEXT("Ball.Ended")) { F.Mode->TryStartNextWave(); F.Mode->TryEndMatch(false); }
    if (Case == TEXT("Ball.Logout")) { F.Mode->Logout(F.A); }
    if (Cancel)
    { F.Step(.01f); TestTrue(TEXT("失效取消在途球"),Attack->IsActorBeingDestroyed()); TestEqual(TEXT("取消保留next"),F.Deadline(),Deadline); return true; }
    if (Case == TEXT("Ball.Sweep")) { F.Step(.02f); TestEqual(TEXT("高速连续Sweep不穿隧"), F.HP(T), 980.f); return true; }
    if (Case == TEXT("Ball.Feedback"))
    { F.Tick(.9f); TestEqual(TEXT("命中后绿色反馈阶段"), Attack->GetState().Phase,EY01TestAttackPhase::HitFeedback); const auto P=Attack->GetActorLocation(); F.Tick(.03f); TestTrue(TEXT("反馈停球"),Attack->GetActorLocation().Equals(P)); F.Tick(.1f); TestTrue(TEXT("反馈.1秒消失"),Attack->IsActorBeingDestroyed()); return true; }
    F.Tick(2.1f);
    const bool Miss=Case==TEXT("Ball.Wall")||Case==TEXT("Ball.MovingMiss")||Case==TEXT("Ball.MaxTravel");
    TestEqual(TEXT("仅发射选定目标可受伤、移动可miss、伤害快照命中减甲"),F.HP(T),Miss?1000.f:Case==TEXT("Ball.ArmorAtHit")?987.f:980.f);
    TestEqual(TEXT("其他单位不截获不受伤"),F.HP(N->GetCombatASC()),1000.f); TestTrue(TEXT("最大距离后消失"),Attack->IsActorBeingDestroyed()); return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R34Acceptance, "Y01.R34.Acceptance", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R34Acceptance::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Observer.Stats"), TEXT("Observer.TargetXY"), TEXT("Observer.Snapshot"), TEXT("Observer.Replication"),
        TEXT("HUD.Unified"), TEXT("HUD.Bounds"), TEXT("Log.Config"), TEXT("Log.Attack"), TEXT("Log.Hit"), TEXT("Log.Cancel"), TEXT("Log.Regen"),
        TEXT("Boundary.ZeroXY"), TEXT("Boundary.NoTraining"), TEXT("Boundary.Client"), TEXT("Boundary.FailedStart"), TEXT("Boundary.Reentry"),
        TEXT("Boundary.WaveReward"), TEXT("Boundary.TargetSwitch"), TEXT("Boundary.TieStable"), TEXT("Boundary.TrainingRanged"),
        TEXT("Boundary.WaveRanged"), TEXT("Boundary.BossRanged"), TEXT("Boundary.SectorDraw"), TEXT("Boundary.NoRegenDebt"),
                TEXT("Boundary.BallLifetimePrecision"), TEXT("Boundary.BallTravelPrecision") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R34Acceptance::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FR34World F; FR34Log Log;
    F.CN(TEXT("Armor"), 7); F.CN(TEXT("HealthRegen"), 3); F.CN(TEXT("Strength"), 9); F.CN(TEXT("Agility"), 8); F.CN(TEXT("Wisdom"), 6);
    const bool RangedEnemy = Case.EndsWith(TEXT("Ranged"));
    const bool BallPrecision = Case.StartsWith(TEXT("Boundary.Ball"));
    const bool Main = Case == TEXT("Boundary.NoTraining") || Case == TEXT("Boundary.WaveReward") || Case == TEXT("Boundary.WaveRanged") || Case == TEXT("Boundary.BossRanged");
    if (Case == TEXT("Boundary.NoTraining")) { F.Profile->bTrainingEnabled = false; }
    if (Case == TEXT("Boundary.WaveReward")) { F.C()->R33TestGrowth.bEnabled = true; F.C()->R33TestGrowth.AttackPerEffectiveKill = 2; }
    if (Case == TEXT("Boundary.TargetSwitch") || BallPrecision) { F.CN(TEXT("AttackType"), 1); F.C()->Range = 800; F.CN(TEXT("AttackInterval"), 10); }
    if (BallPrecision) { F.CN(TEXT("ProjectileSpeed"), 700); }
    if (RangedEnemy)
    {
        F.C()->AttackDamage = 0;
        for (auto Name : {TEXT("Default"), TEXT("Boss")}) { F.M(Name)->AttackType = EY01AttackType::Ranged; F.M(Name)->Range = 800; F.M(Name)->AttackDamage = 20; F.M(Name)->AttackInterval = 10; }
        F.A->GetPawn()->SetActorLocation(FVector(3000,0,100));
    }
    F.Start();
    if (!TestTrue(TEXT("验收场配置成功"), F.Mode->IsMatchConfigured())) { return false; }
    if (Case == TEXT("Boundary.NoRegenDebt"))
    { F.Tick(2); F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),500); F.Tick(1); TestTrue(TEXT("满血时间无回血积欠"),FMath::IsNearlyEqual(F.HP(F.ASC()),503.f,.01f)); return true; }
    TArray<AY01Monster*> Ms;
    if (Main)
    {
        TestTrue(TEXT("真实开始主线"), F.Mode->TryStartNextWave()); F.Tick(5.1f);
        if (Case == TEXT("Boundary.BossRanged"))
        {
            for (AY01Monster* M : F.Mode->GetWaveMonsters()) { M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),0); }
            TestTrue(TEXT("清普通波"),F.Mode->TryCompleteWave()); TestTrue(TEXT("真实开始Boss"),F.Mode->TryStartNextWave()); F.Tick(8.1f);
        }
        for (AY01Monster* M : F.Mode->GetWaveMonsters()) { Ms.Add(M); M->GetCharacterMovement()->DisableMovement(); F.Place(M,FVector(1500,0,0)); }
    }
    else { Ms = F.Training(F.A); }
    if (!TestTrue(TEXT("合法区域内存在怪物"), !Ms.IsEmpty())) { return false; }
    auto* M = Ms[0]; auto* T = M->GetCombatASC(); F.Place(M,FVector(RangedEnemy || BallPrecision || Case == TEXT("Boundary.TargetSwitch") ? 400 : 50,0,0));
    if (Case == TEXT("Boundary.Client"))
    {
        F.PS(F.A)->SetRole(ROLE_SimulatedProxy); TestFalse(TEXT("客户端不能自主开启普攻"), AY01TestAttack::TryAutoAttack(F.ASC()));
        TestTrue(TEXT("客户端无攻击Actor"),F.OwnAttacks().IsEmpty()); TestEqual(TEXT("客户端不消耗next"),F.Deadline(),0.); F.PS(F.A)->SetRole(ROLE_Authority); return true;
    }
    if (Case == TEXT("Boundary.FailedStart"))
    { TestFalse(TEXT("开启失败不消费"),F.ASC()->TryAutoAttack(M,[](float){return false;})); TestEqual(TEXT("失败next不变"),F.Deadline(),0.); return true; }
    if (Case == TEXT("Boundary.Reentry"))
    {
        int32 Starts=0; bool Nested=true;
        TestTrue(TEXT("外层开启成功"),F.ASC()->TryAutoAttack(M,[&](float){ ++Starts; Nested=F.ASC()->TryAutoAttack(M,[&](float){++Starts;return true;});return true;}));
        TestFalse(TEXT("重入不能二次开启"),Nested); TestEqual(TEXT("只有一次开启"),Starts,1); return true;
    }
    if (Case == TEXT("Boundary.ZeroXY")) { F.Place(M,FVector(0,0,100)); }
    if (Case == TEXT("Boundary.WaveReward"))
    { for (int32 I=0;I<Ms.Num();++I) { F.Place(Ms[I],FVector(50+I*60,0,0)); Ms[I]->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),20); } }
    if (Case == TEXT("Observer.TargetXY")) { F.Place(M,FVector(200,0,120)); if (Ms.Num()>1) { F.Place(Ms[1],FVector(210,0,0)); } }
    if (Case == TEXT("Boundary.TieStable")) { F.Place(M,FVector(100,0,0)); F.Place(Ms[1],FVector(-100,0,0)); }
    const int32 Gold = F.PS(F.A)->GetGold(); F.Step(.01f);
    if (Case == TEXT("Boundary.ZeroXY")) { TestEqual(TEXT("XY原点高度内必在扇形"),F.HP(T),980.f); return true; }
    if (Case == TEXT("Boundary.NoTraining"))
    { TestEqual(TEXT("训练关闭时主线仍真正自动"),F.HP(T),980.f); TestFalse(TEXT("训练关闭也拒绝玩家互打"),F.ASC()->IsLegalCombatTarget(F.B->GetPawn())); return true; }
    if (Case == TEXT("Boundary.WaveReward"))
    {
        for (auto* Monster : Ms) { TestEqual(TEXT("同出手快照全额清波"),F.HP(Monster->GetCombatASC()),0.f); }
        TestEqual(TEXT("每敌真实杀成长"),F.Mode->GetGrowth()->Observe(F.PS(F.A)).GrowthKills,int64(Ms.Num()));
        TestEqual(TEXT("清波奖励严格配表一次"),F.PS(F.A)->GetGold(),Gold+10); F.Tick(.5f); TestEqual(TEXT("不重复奖励"),F.PS(F.A)->GetGold(),Gold+10); return true;
    }
    if (RangedEnemy)
    { F.Tick(1); TestTrue(TEXT("怪物真实球命中玩家且减甲"),F.HP(F.ASC())<990); TestTrue(TEXT("怪独立普攻间隔"),F.Deadline(T)>F.World->GetTimeSeconds()); return true; }
    auto Own = F.OwnAttacks(); if (!TestEqual(TEXT("自动出手Actor"),Own.Num(),1)) { return false; } auto* Attack=Own[0];
    if (BallPrecision)
    {
        const auto& S = Attack->GetState();
        TestTrue(TEXT("非整除寿命保持服务器double精度"),FMath::Abs((S.EndsAt-S.StartedAt)-800./700.)<=1.e-12);
        F.Place(M,FVector(1000,0,0));
        if (Case == TEXT("Boundary.BallLifetimePrecision")) { F.Step(2); } else { F.Tick(1.2f); }
        TestTrue(TEXT("非整除寿命结束销毁"),Attack->IsActorBeingDestroyed());
        const double Travel = FVector::Dist(S.Position,S.Origin);
        TestTrue(FString::Printf(TEXT("长Tick及分步均不越最大行程：%.17g"),Travel),Travel<=800.+1.e-8);
        TestTrue(TEXT("终点达到且钳在Range"),FMath::Abs(Travel-800.)<=1.e-8);
        TestEqual(TEXT("最大距离外目标不受伤"),F.HP(T),1000.f); return true;
    }
    if (Case == TEXT("Boundary.TargetSwitch"))
    { F.Mode->GetTraining()->Request(F.A,false); F.Tick(2); TestTrue(TEXT("区域切换取消球不打旧目标"),Attack->IsActorBeingDestroyed()); TestEqual(TEXT("切区保留next"),F.Deadline(),10.01); return true; }
    if (Case == TEXT("Boundary.TieStable"))
    { const FVector Direction=Attack->GetState().Direction; F.Tick(1.02f); Own=F.OwnAttacks(); TestTrue(TEXT("相同平距重复出手稳定"),Own.Num()==1 && Own[0]->GetState().Direction.Equals(Direction)); return true; }
    if (Case == TEXT("Boundary.SectorDraw"))
    {
        const UWorld::ELineBatcherType Temporary[]={UWorld::ELineBatcherType::World,UWorld::ELineBatcherType::Foreground};
        F.World->FlushLineBatchers(Temporary); Attack->Tick(0); auto* Lines=F.World->GetLineBatcher(UWorld::ELineBatcherType::World);
        TestTrue(TEXT("扇形真实线段已绘制"),Lines && Lines->BatchedLines.Num()>=132);
        if (Lines) { for (const auto& L : Lines->BatchedLines)
        { for (FVector P : {FVector(L.Start),FVector(L.End)}) { const auto D=P-Attack->GetState().Origin; TestTrue(TEXT("绘制与复制半径/高度/半角同源"),D.Size2D()<=250.001 && FMath::Abs(D.Z)<=120.001 && (D.Size2D()<.001 || D.X/D.Size2D()>=FMath::Cos(UE_PI/4)-.0001)); } } }
        F.Tick(.36f); F.World->FlushLineBatchers(Temporary); Attack->Tick(0); TestEqual(TEXT("结束没有残留线"),Lines->BatchedLines.Num(),0); return true;
    }
    if (Case.StartsWith(TEXT("Log.")))
    {
        FString Kind=Case.Mid(4).ToUpper(); FString Identity;
        if (Kind==TEXT("CONFIG")) { Identity=TEXT("Source=")+F.PS(F.A)->GetName(); }
        else { Identity=TEXT("Actor=")+Attack->GetName(); }
        if (Kind==TEXT("CANCEL")) { F.ASC()->InvalidateCombatSession(); }
        if (Kind==TEXT("REGEN")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),500); F.Step(.1f); Identity=TEXT("Source=")+F.PS(F.A)->GetName(); }
        const auto* L=Log.Find(Kind,Identity); if (!TestNotNull(TEXT("真实业务路径输出验收日志：")+Kind,L)) { return false; }
        double DS=-1; TestTrue(TEXT("DS时间可机器解析"),FParse::Value(**L,TEXT("DS="),DS)); TestTrue(TEXT("DS来自世界时钟"),DS>=0 && DS<=F.World->GetTimeSeconds());
        TestTrue(TEXT("日志含源身份"),L->Contains(TEXT("Source=")));
        if (Kind==TEXT("CONFIG"))
        {
            for (const TCHAR* Key : {TEXT("Health="),TEXT("AttackDamage="),TEXT("AttackInterval="),TEXT("AttackType="),TEXT("Range="),TEXT("MeleeAngle="),TEXT("ProjectileSpeed="),TEXT("Armor="),TEXT("MoveSpeed="),TEXT("HealthRegen="),TEXT("Strength="),TEXT("Agility="),TEXT("Wisdom=")}) { TestTrue(Key,L->Contains(Key)); }
            TestNotNull(TEXT("每怪配置也记录"),Log.Find(Kind,TEXT("Source=")+M->GetName()));
        }
        if (Kind==TEXT("ATTACK") || Kind==TEXT("HIT")) { TestTrue(TEXT("日志含目标及快照"),L->Contains(TEXT("Target=")) && L->Contains(TEXT("Snapshot=20")) && L->Contains(TEXT("SourceSession=")) && L->Contains(TEXT("TargetSession="))); }
        if (Kind==TEXT("HIT")) { TestTrue(TEXT("命中HP实变及甲可验收"),L->Contains(TEXT("Armor=0")) && L->Contains(TEXT("BeforeHP=1000")) && L->Contains(TEXT("AfterHP=980")) && L->Contains(TEXT("Actual=20"))); }
        if (Kind==TEXT("CANCEL")) { TestTrue(TEXT("取消原因明确"),L->Contains(TEXT("Reason="))); }
        return true;
    }
    F.A->CaptureObserver(F.Mode); const auto& O=F.A->GetObserver();
    auto* StatsProperty=FindFProperty<FStructProperty>(FY01PlaygroundObserver::StaticStruct(),TEXT("UnitStats"));
    if (!TestNotNull(TEXT("Observer包含真实统一属性"),StatsProperty)) { return false; }
    const auto* Stats=StatsProperty->ContainerPtrToValuePtr<FY01UnitCombatStats>(&O);
    TestEqual(TEXT("上限而非当前HP"),Stats->Health,1000.f); TestEqual(TEXT("护甲快照"),Stats->Armor,7.f); TestEqual(TEXT("回血快照"),Stats->HealthRegen,3.f);
    TestEqual(TEXT("三维只展示"),Stats->Strength,9.f); TestEqual(TEXT("三维只展示"),Stats->Agility,8.f); TestEqual(TEXT("三维只展示"),Stats->Wisdom,6.f);
    TestEqual(TEXT("普攻间隔独立字段"),Stats->AttackInterval,1.f);
    auto* Deadline=FindFProperty<FDoubleProperty>(FY01PlaygroundObserver::StaticStruct(),TEXT("AutoAttackDeadline"));
    if (!TestNotNull(TEXT("服务器普攻截止观测"),Deadline)) { return false; }
    TestEqual(TEXT("观测真实普攻截止"),Deadline->GetPropertyValue_InContainer(&O),F.Deadline());
    if (Case==TEXT("Observer.TargetXY")) { TestTrue(TEXT("观测与普攻选择同一XY最近目标"),O.Target==M); }
    if (Case==TEXT("Observer.Replication"))
    {
        F.A->GetClass()->SetUpRuntimeReplicationData(); TArray<FLifetimeProperty> Props; F.A->GetLifetimeReplicatedProps(Props);
        auto* P=FindFProperty<FStructProperty>(F.A->GetClass(),TEXT("Observer")); TestTrue(TEXT("完整权威观测OwnerOnly复制"),P && Props.ContainsByPredicate([&](auto& R){return R.RepIndex==P->RepIndex && R.Condition==COND_OwnerOnly;}));
        for (const TCHAR* N : {TEXT("UnitStats"),TEXT("NextAutoAttackTime")}) { auto* R=FindFProperty<FProperty>(F.ASC()->GetClass(),N); TestTrue(TEXT("ASC真实字段复制"),R && R->HasAnyPropertyFlags(CPF_Net)); }
    }
    if (Case==TEXT("Observer.Snapshot"))
    {
        const double Sample=O.ServerSampleTime, Remaining=F.A->GetSnapshotRemaining(F.Deadline()).GetValue();
        F.A->SetActorTickEnabled(false); F.A->SetRole(ROLE_SimulatedProxy); F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(),90); F.Tick(.1f); F.A->CaptureObserver(F.Mode);
        TestEqual(TEXT("客户端不能再算属性"),Stats->Armor,7.f); TestEqual(TEXT("客户端不能推进观测时钟"),O.ServerSampleTime,Sample); TestEqual(TEXT("客户端无时钟外推"),F.A->GetSnapshotRemaining(F.Deadline()).GetValue(),Remaining); F.A->SetRole(ROLE_Authority);
    }
    if (Case.StartsWith(TEXT("HUD.")))
    {
        auto* HUD=F.World->SpawnActor<AY01PlaygroundHUD>(); HUD->PlayerOwner=F.A; HUD->NotifyHitBoxClick(TEXT("Attributes"));
        for (float Scale : {1.f,.75f})
        {
            TArray<FString> Texts; HUD->ObserveDrawForTest=[&](const FBox2D& B,const FString& Text)
            { Texts.Add(Text); if (B.Min.X>=932*Scale && B.Min.Y>=208*Scale && B.Min.Y<558*Scale) { TestTrue(TEXT("右栏不越界/不占中央：")+Text,B.Max.X<=1264*Scale && B.Max.Y<=558*Scale); } };
            FCanvas Batch(nullptr,nullptr,F.World,ERHIFeatureLevel::SM5); TStrongObjectPtr<UCanvas> C(NewObject<UCanvas>()); C->Init(1280*Scale,800*Scale,nullptr,&Batch); C->Update(); HUD->SetCanvas(C.Get(),nullptr); HUD->DrawHUD(); HUD->SetCanvas(nullptr,nullptr);
            FString All=FString::Join(Texts,TEXT("\n"));
            for (const TCHAR* Token : {TEXT("AttackInterval"),TEXT("AttackType"),TEXT("Range"),TEXT("MeleeAngle"),TEXT("ProjectileSpeed"),TEXT("Armor"),TEXT("HealthRegen"),TEXT("MoveSpeed"),TEXT("Strength"),TEXT("Agility"),TEXT("Wisdom"),TEXT("仅显示"),TEXT("独立普攻")}) { TestTrue(Token,All.Contains(Token)); }
            TestFalse(TEXT("不再错误提示自动普攻未接"),All.Contains(TEXT("自动普攻未接")));
        }
        HUD->ObserveDrawForTest=nullptr;
    }
    return true;
}
#endif
