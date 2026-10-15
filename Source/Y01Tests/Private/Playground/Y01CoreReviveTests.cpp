#include "Misc/AutomationTest.h"
#include "Y01MatchFixture.h"
#include "Playground/Y01R36Runtime.h"
#include "Combat/Y01Core.h"
#include "Combat/Y01Monster.h"
#include "Match/Y01KillRewards.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01DropDeliveryComponent.h"
#include "Player/Y01EconomyComponent.h"
#include "Player/Y01DamageEffectsComponent.h"
#include "Combat/Y01DamageEffects.h"
#include "Playground/Y01R36UI.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Components/CanvasPanelSlot.h"
#include "EngineUtils.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Playground/Y01TestSkill.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/ScopeExit.h"
#if __has_include("Player/Y01ReviveComponent.h")
#include "Player/Y01ReviveComponent.h"
#define Y01_REVIVE_AVAILABLE 1
#endif

#if WITH_DEV_AUTOMATION_TESTS
struct FY01R36DeliveryTestAccess
{
    static bool Lock(UY01LoadoutComponent* L) { return L->Transactions.Enter(); }
    static void Unlock(UY01LoadoutComponent* L) { L->Transactions.Leave(); }
    static int32 Seed(UY01EconomyComponent* E) { return E->LootRandom.GetCurrentSeed(); }
    static void Exhaust(UY01LoadoutComponent* L) { L->AcquisitionSerial = MAX_uint64; }
    static void Clock(UY01IncomeComponent* I, TFunction<double()> F) { I->TestClock = MoveTemp(F); }
};
namespace
{
using K = EY01TableKind;
struct FCoreWorld
{
    FY01MatchFixture Data;
    UWorld* World = nullptr;
    AY01R36GameMode* Mode = nullptr;
    AY01Core* Core = nullptr;
    AY01R36Controller* PC = nullptr;
    AY01PlayerState* Player = nullptr;
    AY01PlaygroundCharacter* Pawn = nullptr;
    AStaticMeshActor* Floor = nullptr;
    double Time = 1000;
    FCoreWorld(bool Automatic = false, bool Start = true)
    {
        auto& C = Data.Row<FY01CharacterRow>(K::Character);
        C.InitialEquipment.Reset(); C.InitialSkills.Reset(); C.Health = 300; C.HealthRegen = 5; C.AttackDamage = 30;
        auto& M = Data.Row<FY01MonsterRow>(K::Monster); M.Health = 10; M.AttackDamage = 0; M.MoveSpeed = 0;
        FY01SourceEffectRow E; E.Magnitude = 5; Data.Add(K::SourceEffect, E, TEXT("Five"));
        for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Blue"))})
        {
            auto& Item = Data.Row<FY01EquipmentRow>(K::Equipment, Name);
            Item.DisplayName = FText::FromString(TEXT("复活验证装备"));
            Item.EffectIds = {Data.Ref(K::SourceEffect, TEXT("Five"))};
        }
        FY01RewardPackageRow Drop; Drop.Items.Add({Data.Ref(K::Equipment), 1}); Data.Add(K::RewardPackage, Drop, TEXT("Drop"));
        M.ItemDropChance = 1; M.DropReward = Data.Ref(K::RewardPackage, TEXT("Drop"));
        auto& W = Data.Row<FY01WaveRow>(K::Wave); W.PreparationSeconds = .1f; W.RespawnSeconds = 1;
        W.Spawns = {{Data.Ref(K::Monster), 3, 0}, {Data.Ref(K::Monster), 1, 20}};
        auto& W2 = Data.Row<FY01WaveRow>(K::Wave, TEXT("Boss")); W2.PreparationSeconds = .4f; W2.RespawnSeconds = 4;
        W2.Spawns = {{Data.Ref(K::Monster), 1, 0}};
        auto* Profile = NewObject<UY01R36Profile>(); Profile->bCoreDefenseEnabled = true;
        Profile->bEvidenceMode = !Automatic; Profile->bEconomyEnabled = true; Profile->bTrainingEnabled = true;
        Profile->InitialGold = 0; Profile->BaseGoldPerSecond = 1.5; Profile->DropFlightSeconds = 10;
        Profile->Character = Data.Ref(K::Character); Profile->OrderedWaves = Data.Waves();
        for (const auto& P : Data.Tables) { Profile->Tables.Add(P.Key, const_cast<UDataTable*>(P.Value)); }
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01R36GameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01R36GameMode>(); World->SpawnActor<AY01R36Config>()->Content = Profile;
        Core = World->SpawnActor<AY01Core>(FVector(0,0,90), FRotator::ZeroRotator);
        PC = World->SpawnActor<AY01R36Controller>(); Player = PC->GetPlayerState<AY01PlayerState>();
        if (!Player) { Player = World->SpawnActor<AY01PlayerState>(); }
        Player->SetOwner(PC);
        FY01R36DeliveryTestAccess::Clock(Player->GetIncome(), [this] { return Time; });
        Pawn = World->SpawnActor<AY01PlaygroundCharacter>(FVector(-700,0,90), FRotator::ZeroRotator);
        PC->PlayerState = Player; PC->Possess(Pawn); Player->GetCombatASC()->InitAbilityActorInfo(Player, Pawn);
        Mode->GetFlowState()->AddPlayerState(Player);
        Floor = World->SpawnActor<AStaticMeshActor>(FVector(0,0,-50), FRotator::ZeroRotator);
        Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
        Floor->SetActorScale3D(FVector(50,50,1)); Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
        Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay();
        if (!Automatic && Start) { Mode->TryStartNextWave(); }
        Tick(.15);
    }
    ~FCoreWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    void Tick(double Seconds) { for (int32 I = 0; I < FMath::CeilToInt(Seconds/.025); ++I) { ++GFrameCounter; Time += .025; World->Tick(LEVELTICK_All, .025f); } }
    FY01ItemRequest Request(FGuid Id, EY01ItemOperation Op, int32 Slot = 0)
    {
        FY01CommittedView V; Player->GetLoadout()->ReadCommittedView(V); FY01ItemInstance Item;
        Player->GetLoadout()->FindItem(Id, Item); FY01ItemRequest R;
        R.MatchId = V.MatchId; R.JoinSequence = V.JoinSequence; R.Seq = V.ExpectedSeq; R.ConfigRevision = V.ConfigRevision;
        R.ItemId = Id; R.ExpectedEntityRevision = Item.EntityRevision; R.Operation = Op; R.Slot = Slot;
        if ((Op == EY01ItemOperation::Equip || Op == EY01ItemOperation::Unequip) && V.EquipmentSlots.IsValidIndex(Slot))
        { R.ExpectedSlotItemId = V.EquipmentSlots[Slot]; FY01ItemInstance Old;
            if (Player->GetLoadout()->FindItem(R.ExpectedSlotItemId,Old).IsOk()) { R.ExpectedSlotEntityRevision = Old.EntityRevision; } }
        else if (V.Skills.IsValidIndex(Slot)) { R.ExpectedLearnedId = V.Skills[Slot].LearnedId; R.ExpectedLearnedRevision = V.Skills[Slot].EntityRevision; }
        return R;
    }
    int32 Products() const
    { int32 N = 0; for (TActorIterator<AY01DamageVisual> It(World); It; ++It)
        { if (It->GetOwner() == Player && !It->IsActorBeingDestroyed()) { ++N; } } return N; }
    void Die() { Player->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    float Health() const { return Player->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()); }
    void Fall() { Core->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    bool Drop()
    {
        if (Mode->GetWaveMonsters().IsEmpty()) { return false; }
        auto* Target = Mode->GetWaveMonsters()[0]->GetCombatASC(); auto* A = Player->GetCombatASC();
        Pawn->SetActorLocation(Target->GetAvatarActor()->GetActorLocation() + FVector(-150, 0, 0));
        auto Spec = A->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, A->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, 1000);
        A->ApplyGameplayEffectSpecToTarget(*Spec.Data, Target);
        return Player->GetDropDelivery()->GetLedger().Records.Num() == 1;
    }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01CoreReviveBehaviorTest, "Y01.Core.Step2",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01CoreReviveBehaviorTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : {TEXT("Lifecycle"), TEXT("WaveSnapshot"), TEXT("TrainingDeath"), TEXT("SafeGround"),
        TEXT("FallenSettlement"), TEXT("BusySettlement"), TEXT("Income"), TEXT("ReentrantSettlement"), TEXT("ZeroPlan"), TEXT("Success"), TEXT("BeforeWave"), TEXT("BeforeWaveFallen"),
        TEXT("ResumeReentry"), TEXT("ResumeMax"), TEXT("DeathETA"), TEXT("Gates"), TEXT("FrozenEffects"), TEXT("EffectsCD"), TEXT("HUDSnapshot"),
        TEXT("FallenPersistence"), TEXT("AllDead"), TEXT("FrozenKill"), TEXT("FaultSettlement"),
        TEXT("TrainingMeleeDeath"), TEXT("TrainingMeleeDeathRemove"), TEXT("SafeCore"), TEXT("SafeCoreScaled"),
        TEXT("ZeroPlanCore"), TEXT("ZeroPlanBlocked"), TEXT("SafeBlocked")})
    { Names.Add(Name); Commands.Add(Name); }
    for (const TCHAR* Name : {TEXT("CoreDamageAudience.Main"), TEXT("CoreDamageAudience.TrainingExcluded"),
        TEXT("CoreRegenAudience.Main"), TEXT("CoreRegenAudience.TrainingExcluded"), TEXT("ReviveNoDisplay")})
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01CoreReviveBehaviorTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FCoreWorld F(Case == TEXT("WaveSnapshot"), !Case.StartsWith(TEXT("BeforeWave")));
    if (!TestTrue(TEXT("真实核心经济会话配置"), F.Mode->IsMatchConfigured() && F.Player->GetDropDelivery()->IsEnabled())) { return false; }
    if (Case == TEXT("ReviveNoDisplay"))
    {
        TArray<FY01DamageEvent> Events;
        auto* Service = F.World->GetSubsystem<UY01DamageEvents>();
        const auto Handle = Service->OnDamageCommitted.AddLambda([&](const FY01DamageEvent& E) { Events.Add(E); });
        F.Die(); F.Tick(1.1);
        TestEqual(TEXT("真实复活恢复满血"), F.Health(), 300.f);
        TestEqual(TEXT("死亡管理调血与复活初始化都不产生飘字"), Events.Num(), 0);
        Service->OnDamageCommitted.Remove(Handle);
        return true;
    }
    if (Case.StartsWith(TEXT("CoreDamageAudience.")) || Case.StartsWith(TEXT("CoreRegenAudience.")))
    {
        F.PC->Player = NewObject<ULocalPlayer>(GEngine);
        F.PC->SetDamageRegion(INDEX_NONE); F.PC->SetDamageRegion(0);
        TArray<FY01DamageEvent> Received;
        const auto Handle = F.PC->OnDamageReceived.AddLambda([&](const FY01DamageEvent& E) { Received.Add(E); });
        const bool bTraining = Case.EndsWith(TEXT("TrainingExcluded"));
        if (bTraining && !TestTrue(TEXT("受众夹具真实进入训练区"), F.Mode->GetTraining()->Request(F.PC, true)))
        { F.PC->OnDamageReceived.Remove(Handle); return false; }
        if (Case.StartsWith(TEXT("CoreDamageAudience.")))
        {
            if (!TestTrue(TEXT("核心受伤夹具有主线怪"), !F.Mode->GetWaveMonsters().IsEmpty()))
            { F.PC->OnDamageReceived.Remove(Handle); return false; }
            auto* Source = F.Mode->GetWaveMonsters()[0]->GetCombatASC(); auto* Target = F.Core->GetCombatASC();
            auto Spec = Source->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, Source->MakeEffectContext());
            Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, 7);
            Source->ApplyGameplayEffectSpecToTarget(*Spec.Data, Target); F.Tick(.1);
        }
        else
        {
            F.Core->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 990);
            F.Tick(1.1);
        }
        const int32 Expected = bTraining ? 0 : 1;
        if (TestEqual(TEXT("核心世界数字只发送主线区域受众"), Received.Num(), Expected) && Expected == 1)
        {
            TestEqual(TEXT("核心世界数字固定属于主线区域"), Received[0].RegionId, 0);
            const bool bDamage = Case.StartsWith(TEXT("CoreDamageAudience."));
            TestEqual(TEXT("核心世界数字类型正确"), Received[0].Type,
                bDamage ? EY01DamageEventType::Damage : EY01DamageEventType::Healing);
            TestEqual(TEXT("核心世界数字显示结算量"), Received[0].DisplayAmount, bDamage ? 7.f : 5.f);
        }
        F.PC->OnDamageReceived.Remove(Handle);
        return true;
    }
    if (Case == TEXT("FallenSettlement") || Case == TEXT("BusySettlement") || Case == TEXT("ReentrantSettlement"))
    {
        if (!TestTrue(TEXT("真实致死产生已归属未到账掉落"), F.Drop())) { return false; }
        const auto Receipt = F.Player->GetDropDelivery()->GetLedger().Records[0];
        FY01SourceTransaction Tx;
        if (Case == TEXT("BusySettlement"))
        {
            FY01PreparedSource Plan;
            TestTrue(TEXT("真实来源暂存预检"), F.Player->GetSourceEffects()->PrepareSource(FY01DataCatalog(F.Data.Tables),
                EY01RuntimeSourceKind::Equipment, TEXT("Busy"), {F.Data.Ref(K::SourceEffect, TEXT("Five"))}, Plan).IsOk());
            TestTrue(TEXT("持有真实来源事务"), F.Player->GetSourceEffects()->StageSource(Plan, Tx).IsOk());
        }
        F.Die(); F.Fall();
        bool Reentered = false;
        if (Case == TEXT("ReentrantSettlement"))
        {
            FY01R36DeliveryTestAccess::Clock(F.Player->GetIncome(), [&]
            {
                if (!Reentered)
                {
                    Reentered = true; F.Player->GetDropDelivery()->Schedule();
                    TestFalse(TEXT("结清回调不可提前终局"), F.Mode->TryEndMatch(false));
                }
                return F.Time;
            });
        }
        TestTrue(TEXT("失守同步冻结"), F.Mode->IsGameplayFrozen());
        F.Tick(.1);
        if (Case == TEXT("BusySettlement"))
        {
            TestTrue(TEXT("忙时不能提前结束"), F.Mode->GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Ended);
            TestEqual(TEXT("忙保留待交付"), F.Player->GetDropDelivery()->GetLedger().Records[0].State, EY01DeliveryState::Pending);
            F.Player->GetSourceEffects()->RollbackSource(Tx);
        }
        F.Tick(1.2);
        TestEqual(TEXT("结清才失败终局"), F.Mode->GetFlowState()->GetWaveSnapshot().Result, EY01MatchResult::Failure);
        FY01ItemInstance Item;
        TestTrue(TEXT("ETA前已归属物品不能丢"), F.Player->GetLoadout()->FindItem(Receipt.ItemId, Item).IsOk());
        TestEqual(TEXT("失守取消同时到期复活"), F.Health(), 0.f);
        F.Player->GetDropDelivery()->Schedule(); F.Fall(); F.Tick(.1);
        FY01CommittedView View; F.Player->GetLoadout()->ReadCommittedView(View);
        TestEqual(TEXT("重复结清不重复入包"), View.Items.Num(), 1);
        if (Case == TEXT("ReentrantSettlement")) { TestTrue(TEXT("真实交付栈发生重入"), Reentered); }
        FY01R36DeliveryTestAccess::Clock(F.Player->GetIncome(), [&F] { return F.Time; });
        return true;
    }
    if (Case == TEXT("AllDead"))
    {
        auto* PC2 = F.World->SpawnActor<AY01R36Controller>();
        auto* P2 = PC2->GetPlayerState<AY01PlayerState>();
        if (!P2) { P2 = F.World->SpawnActor<AY01PlayerState>(); }
        P2->SetOwner(PC2); PC2->PlayerState = P2;
        auto* Pawn2 = F.World->SpawnActor<AY01PlaygroundCharacter>(FVector(-900,0,90), FRotator::ZeroRotator);
        PC2->Possess(Pawn2); P2->GetCombatASC()->InitAbilityActorInfo(P2, Pawn2);
        FY01R36DeliveryTestAccess::Clock(P2->GetIncome(), [&F] { return F.Time; });
        F.Mode->GetFlowState()->AddPlayerState(P2); PC2->Player = NewObject<ULocalPlayer>(GEngine); F.Mode->PostLogin(PC2);
        TestTrue(TEXT("第二位真实玩家初始化"), P2->IsCharacterInitialized());
        F.Die(); P2->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0);
        F.Tick(.5);
        TestTrue(TEXT("两位全死均等待"), F.Player->GetRevive()->GetSnapshot().bWaiting && P2->GetRevive()->GetSnapshot().bWaiting);
        TestTrue(TEXT("双玩家全队死亡不终局"), F.Mode->GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Ended);
        F.Tick(.6);
        TestEqual(TEXT("第二位同Pawn满血复活"), P2->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 300.f);
        TestTrue(TEXT("安全复活避开另一玩家"), FVector::DistSquared2D(F.Pawn->GetActorLocation(), Pawn2->GetActorLocation()) > FMath::Square(80.));
        return true;
    }
    if (Case == TEXT("FrozenKill"))
    {
        auto* Target = F.Mode->GetWaveMonsters()[0]->GetCombatASC(); bool Called = false;
        const auto Hook = Target->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda([&](const FOnAttributeChangeData& D)
        {
            if (D.NewValue != 0 || Called) { return; }
            Called = true; F.Fall(); F.Mode->Tick(0);
            TestTrue(TEXT("致死回调内等待冻结击杀发布"), F.World->GetSubsystem<UY01KillRewards>()->HasPending(F.Player));
            TestTrue(TEXT("致死回调内不能提前结束"), F.Mode->GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Ended);
        });
        TestTrue(TEXT("致死退栈后仍归属掉落"), F.Drop()); Target->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).Remove(Hook);
        TestTrue(TEXT("真实致死回调发生失守"), Called); F.Tick(.1);
        TestEqual(TEXT("冻结击杀完成后失败终局"), F.Mode->GetFlowState()->GetWaveSnapshot().Result, EY01MatchResult::Failure);
        TestEqual(TEXT("冻结击杀掉落结清"), F.Player->GetDropDelivery()->GetLedger().Records[0].State, EY01DeliveryState::Delivered);
        return true;
    }
    if (Case == TEXT("FaultSettlement"))
    {
        TestTrue(TEXT("真实归属物品"), F.Drop()); FY01R36DeliveryTestAccess::Exhaust(F.Player->GetLoadout());
        F.Fall(); F.Tick(.1);
        TestEqual(TEXT("不可交付故障保留"), F.Player->GetDropDelivery()->GetLedger().Records[0].State, EY01DeliveryState::Fault);
        TestTrue(TEXT("故障不可假报结清终局"), F.Mode->GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Ended);
        return true;
    }
    if (Case == TEXT("BeforeWaveFallen"))
    {
        F.Fall(); F.Tick(.1);
        TestEqual(TEXT("首波前失守也失败终局"), F.Mode->GetFlowState()->GetWaveSnapshot().Result, EY01MatchResult::Failure); return true;
    }
    if (Case == TEXT("BeforeWave"))
    {
        F.Die(); TestEqual(TEXT("首波前使用第一波"), F.Player->GetRevive()->GetSnapshot().WaveIndex, 0);
        TestTrue(TEXT("死亡期间允许服务器启动波次"), F.Mode->TryStartNextWave()); F.Tick(2.);
        TestEqual(TEXT("首波前死亡仍能复活"), F.Health(), 300.f);
        TestTrue(TEXT("首波等待暂停后收入能真正启动"), F.Player->GetIncome()->Observe().State.TotalIncome > 0); return true;
    }
    if (Case == TEXT("ResumeReentry") || Case == TEXT("ResumeMax"))
    {
        F.Die(); bool Called = false;
        FY01R36DeliveryTestAccess::Clock(F.Player->GetIncome(), [&]
        {
            if (!Called) { Called = true;
                if (Case == TEXT("ResumeReentry")) { F.Fall(); }
                else { F.Player->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(), 450); } }
            return F.Time;
        });
        F.Tick(1.1); TestTrue(TEXT("恢复时真实时钟重入"), Called);
        if (Case == TEXT("ResumeReentry"))
        { TestEqual(TEXT("失守重入取消复活"), F.Health(), 0.f); TestTrue(TEXT("失守重入不能恢复收入"), F.Player->GetIncome()->IsPaused()); }
        else { TestEqual(TEXT("恢复采用执行时有效上限"), F.Health(), 450.f); }
        FY01R36DeliveryTestAccess::Clock(F.Player->GetIncome(), [&F] { return F.Time; }); return true;
    }
    if (Case == TEXT("DeathETA"))
    {
        TestTrue(TEXT("先产生归属掉落"), F.Drop()); const auto Id = F.Player->GetDropDelivery()->GetLedger().Records[0].ItemId;
        F.Player->GetRevive()->PlanPolicy = [](const auto&, auto& P) { P.WaitSeconds = 20; };
        F.Die(); F.Tick(10.1); FY01ItemInstance Item;
        TestEqual(TEXT("仍在死亡等待"), F.Health(), 0.f);
        TestTrue(TEXT("死亡不阻止原ETA交付"), F.Player->GetLoadout()->FindItem(Id, Item).IsOk()); return true;
    }
    if (Case == TEXT("Gates") || Case == TEXT("FrozenEffects") || Case == TEXT("EffectsCD") || Case == TEXT("FallenPersistence"))
    {
        auto* L = F.Player->GetLoadout(); auto* A = F.Player->GetCombatASC();
        FY01DamageEffectRow Bound; Bound.FixedDamage = 1; Bound.Trigger = EY01DamageTrigger::Bound;
        Bound.Product = EY01DamageProduct::BoundArea; Bound.Targeting = EY01DamageTargeting::SelfCircle;
        Bound.Position = EY01DamagePosition::FollowPlayer; Bound.TickSeconds = 1; Bound.Lifetime = 0; Bound.Radius = 200;
        F.Data.Add(K::DamageEffect, Bound, TEXT("Bound"));
        auto Free = Bound; Free.Trigger = EY01DamageTrigger::Periodic; Free.Product = EY01DamageProduct::IndependentArea;
        Free.PeriodSeconds = 3; Free.Lifetime = 10; Free.Radius = 1000; F.Data.Add(K::DamageEffect, Free, TEXT("Free"));
        auto& Gear = F.Data.Row<FY01EquipmentRow>(K::Equipment); Gear.DamageEffectIds = {F.Data.Ref(K::DamageEffect,TEXT("Bound")),F.Data.Ref(K::DamageEffect,TEXT("Free"))};
        FY01GrowthRuleRow Growth; Growth.Delta = 3; F.Data.Add(K::GrowthRule, Growth);
        Gear.GrowthRuleIds = {F.Data.Ref(K::GrowthRule)};
        const FGuid Id = FGuid::NewGuid(); TestTrue(TEXT("真实装备入包"), L->AddItems(FY01DataCatalog(F.Data.Tables), {{Id,F.Data.Ref(K::Equipment)}}).IsOk());
        TestTrue(TEXT("真实穿戴来源"), L->TryEquip(F.PC,F.Request(Id,EY01ItemOperation::Equip)).IsOk());
        TestEqual(TEXT("已建绑定产物"), F.Products(), 1);
        if (Case == TEXT("FallenPersistence"))
        {
            const float Attack = A->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute());
            TestTrue(TEXT("真实进入训练"), F.Mode->GetTraining()->Request(F.PC,true));
            F.Fall(); F.Tick(.1); FY01ItemInstance Item; FY01SourceSnapshot Sources;
            TestEqual(TEXT("失守真正结束"), F.Mode->GetFlowState()->GetWaveSnapshot().Result, EY01MatchResult::Failure);
            TestTrue(TEXT("失守保留装备实体"), L->FindItem(Id,Item).IsOk() && Item.Location == EY01ItemLocation::EquipmentSlot);
            TestEqual(TEXT("失守不扣已得来源属性"), A->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), Attack);
            TestTrue(TEXT("失守保留已提交来源"), F.Player->GetSourceEffects()->Observe(Sources).IsOk() && Sources.Effects.Num() > 1);
            TestEqual(TEXT("结清后业务收入停止"), F.Player->GetIncome()->Observe().State.Phase, EY01IncomePhase::Stopped);
            TestFalse(TEXT("失守返回主线"), F.Mode->GetTraining()->Observe(F.Player).bInside);
            TestTrue(TEXT("失守清训练怪"), F.Mode->GetTraining()->Monsters(F.Player).IsEmpty());
            TestEqual(TEXT("失守返回后仍禁移动"), F.Pawn->GetCharacterMovement()->MovementMode, MOVE_None); return true;
        }
        if (Case == TEXT("Gates") || Case == TEXT("FrozenEffects"))
        {
            AY01Monster* FrozenMonster = nullptr;
            if (Case == TEXT("FrozenEffects"))
            {
                if (!TestTrue(TEXT("冻结前存在真实波次怪"), !F.Mode->GetWaveMonsters().IsEmpty())) { return false; }
                FrozenMonster = F.Mode->GetWaveMonsters()[0];
                FrozenMonster->GetCharacterMovement()->Velocity = FVector(100, 0, 0);
            }
            if (Case == TEXT("Gates")) { F.Die(); } else { F.Fall(); }
            if (FrozenMonster)
            {
                TestTrue(TEXT("失守冻结立即停止普通怪速度"), FrozenMonster->GetCharacterMovement()->Velocity.IsNearlyZero());
                TestEqual(TEXT("失守冻结禁用普通怪移动模式"), FrozenMonster->GetCharacterMovement()->MovementMode, MOVE_None);
            }
            const auto R = L->TryUnequip(F.PC,F.Request(Id,EY01ItemOperation::Unequip));
            const auto Expected = Case == TEXT("Gates") ? EY01ItemError::Dead : EY01ItemError::Stopped;
            TestEqual(TEXT("冻结或死亡权威拒绝主动卸装"), R.Code, Expected);
            for (const auto Op : {EY01ItemOperation::Purchase, EY01ItemOperation::Synthesize, EY01ItemOperation::ChooseTalent, EY01ItemOperation::PrepareLearn})
            {
                auto Request = F.Request({},Op);
                TestEqual(TEXT("经济与学习入口优先执行生命门禁"), L->SubmitRequest(F.PC,Request).Code, Expected);
            }
            TestFalse(TEXT("冻结或死亡拒绝技能"), F.Player->TryActivateSkillSlot(0));
            TestFalse(TEXT("冻结或死亡拒绝进入训练"), F.Mode->GetTraining()->Request(F.PC,true));
            TestEqual(TEXT("立即停止移动"), F.Pawn->GetCharacterMovement()->MovementMode, MOVE_None);
            F.Tick(.1); TestEqual(TEXT("冻结或死亡不重建绑定产物"), F.Products(), 0); return true;
        }
        FY01DamageEffectRow Active; Active.Trigger = EY01DamageTrigger::Active; Active.FixedDamage = 1; Active.SearchRadius = 2000;
        F.Data.Add(K::DamageEffect,Active,TEXT("Active")); FY01SkillRow Skill; Skill.DisplayName = FText::FromString(TEXT("复活技能"));
        Skill.Ability = UY01DamageAbility::StaticClass(); Skill.CooldownSeconds = 8; Skill.DamageEffectIds = {F.Data.Ref(K::DamageEffect,TEXT("Active"))};
        F.Data.Add(K::Skill,Skill,TEXT("Active")); const FGuid Book = FGuid::NewGuid();
        TestTrue(TEXT("技能书入包"),L->AddItems(FY01DataCatalog(F.Data.Tables),{{Book,F.Data.Ref(K::Skill,TEXT("Active"))}}).IsOk());
        TestTrue(TEXT("真实学习技能"),L->PrepareLearn(F.PC,F.Request(Book,EY01ItemOperation::PrepareLearn)).IsOk());
        TestTrue(TEXT("真实技能发动产生CD"),F.Player->TryActivateSkillSlot(0));
        TestTrue(TEXT("真实击杀成长"),F.Drop()); F.Tick(3.1);
        TestTrue(TEXT("独立产物已释放"),F.Products() >= 2);
        FY01LoadoutSnapshot Before; L->Observe(Before); FY01ItemInstance BeforeItem; L->FindItem(Id,BeforeItem);
        const float Attack = A->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute());
        F.Die(); TestEqual(TEXT("死亡立即清全部独立和绑定产物"),F.Products(),0); F.Tick(1.1);
        FY01LoadoutSnapshot After; L->Observe(After); FY01ItemInstance AfterItem; L->FindItem(Id,AfterItem);
        TestEqual(TEXT("技能deadline没有清理或刷新"),After.Skills[0].Deadline,Before.Skills[0].Deadline);
        TestEqual(TEXT("同一学习身份"),After.Skills[0].LearnedId,Before.Skills[0].LearnedId);
        TestEqual(TEXT("来源属性不叠加不丢失"),A->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()),Attack);
        TestEqual(TEXT("成长累计保留"),AfterItem.Growth[0].AppliedCount,BeforeItem.Growth[0].AppliedCount);
        TestTrue(TEXT("确实发生了成长"),BeforeItem.Growth[0].AppliedCount > 0);
        TestEqual(TEXT("复活立即只恢复绑定"),F.Products(),1); F.Tick(2.7);
        TestEqual(TEXT("周期未满完整间隔不发动"),F.Products(),1); F.Tick(.4);
        TestTrue(TEXT("周期满间隔恢复"),F.Products() >= 2); return true;
    }
    if (Case == TEXT("HUDSnapshot"))
    {
        F.Die(); auto S = F.Player->GetRevive()->GetSnapshot(); TArray<uint8> Bytes;
        FMemoryWriter Writer(Bytes); bool Saved = false; S.NetSerialize(Writer,nullptr,Saved);
        FY01ReviveSnapshot R; FMemoryReader Reader(Bytes); bool Loaded = false; R.NetSerialize(Reader,nullptr,Loaded);
        TestTrue(TEXT("同包复制死亡序列与服务器样本"),Saved && Loaded && R.DeathSequence == S.DeathSequence && R.Deadline == S.Deadline && R.ServerSampleTime == S.ServerSampleTime);
        // 此夹具的GameInstance没有世界上下文；UI使用真实World作为Outer。
        F.World->SetGameInstance(nullptr);
        auto* Panel = CreateWidget<UY01R36Panel>(F.World);
        if (!TestNotNull(TEXT("创建真实面板"),Panel)) { return false; }
        TSharedPtr<SWidget> Slate = Panel->TakeWidget();
        auto* Countdown = Cast<UTextBlock>(Panel->WidgetTree->FindWidget(TEXT("ReviveCountdown")));
        if (!TestNotNull(TEXT("独立复活倒计时Widget"),Countdown)) { return false; }
        Panel->PresentRevive(S);
        TestEqual(TEXT("显示权威一秒等待"),Countdown->GetText().ToString(),FString(TEXT("复活倒计时 1.0 秒")));
        F.Tick(.4); Panel->PresentRevive(S);
        TestEqual(TEXT("旧服务器样本不随客户端时间外推"),Countdown->GetText().ToString(),FString(TEXT("复活倒计时 1.0 秒")));
        Panel->PresentRevive(F.Player->GetRevive()->GetSnapshot());
        TestEqual(TEXT("新服务器样本推进倒计时"),Countdown->GetText().ToString(),FString(TEXT("复活倒计时 0.6 秒")));
        FY01CoreSnapshot Hit = F.Core->GetSnapshot(); Hit.AlertSequence = 1; Hit.LastAlertTime = Hit.ServerSampleTime;
        Panel->PresentCoreAlert(Hit);
        auto* ReviveSlot = Cast<UCanvasPanelSlot>(Countdown->Slot);
        auto* CoreSlot = Cast<UCanvasPanelSlot>(Panel->WidgetTree->FindWidget(TEXT("CoreAlert"))->Slot);
        auto* VitalsSlot = Cast<UCanvasPanelSlot>(Panel->WidgetTree->FindWidget(TEXT("TopVitals"))->Slot);
        TestTrue(TEXT("复活提示不覆盖核心提示和生命且不碰三面板"), ReviveSlot->GetPosition().X + ReviveSlot->GetSize().X <= CoreSlot->GetPosition().X
            && ReviveSlot->GetPosition().Y >= VitalsSlot->GetPosition().Y + VitalsSlot->GetSize().Y
            && ReviveSlot->GetPosition().Y + ReviveSlot->GetSize().Y <= 96);
        F.Fall(); Panel->PresentRevive(F.Player->GetRevive()->GetSnapshot());
        TestFalse(TEXT("失守取消复活倒计时"),Countdown->IsVisible());
        Panel->PresentCoreAlert(F.Core->GetSnapshot());
        auto* Alert = Cast<UTextBlock>(Panel->WidgetTree->FindWidget(TEXT("CoreAlert")));
        TestEqual(TEXT("全队准确失守文案"),Alert->GetText().ToString(),FString(TEXT("大本营已失守"))); Slate.Reset(); return true;
    }
    if (Case == TEXT("Success"))
    {
        F.Tick(20.1);
        for (AY01Monster* Monster : F.Mode->GetWaveMonsters())
        { Monster->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        TestTrue(TEXT("正常完成波次"), F.Mode->TryCompleteWave());
        TestTrue(TEXT("正常成功终局"), F.Mode->TryEndMatch(true));
        TestEqual(TEXT("保留成功结果"), F.Mode->GetFlowState()->GetWaveSnapshot().Result, EY01MatchResult::Success); return true;
    }
    if (Case == TEXT("TrainingMeleeDeath") || Case == TEXT("TrainingMeleeDeathRemove"))
    {
        const_cast<UY01PlaygroundProfile*>(F.Mode->GetProfile())->bR34AutoAttackEnabled = true;
        auto& MonsterRow = F.Data.Row<FY01MonsterRow>(K::Monster);
        MonsterRow.AttackType = EY01AttackType::Melee; MonsterRow.AttackDamage = 1000; MonsterRow.MeleeAngle = 360;
        auto& Stage = F.Data.Row<FY01TrainingStageRow>(K::TrainingStage);
        Stage.Spawns = {{F.Data.Ref(K::Monster), 2, 0}}; Stage.MaxAlive = 2; Stage.SpawnInterval = 60;
        auto* Training = F.Mode->GetTraining();
        if (!TestTrue(TEXT("真实进入双近战练功房"), Training->Request(F.PC, true))) { return false; }
        const auto Monsters = Training->Monsters(F.Player);
        if (!TestEqual(TEXT("立即生成两个真实近战怪"), Monsters.Num(), 2)) { return false; }
        const auto Before = Training->Observe(F.Player);
        TStrongObjectPtr<UY01CombatAbilitySystem> First(Monsters[0]->GetCombatASC()), Second(Monsters[1]->GetCombatASC());
        for (auto* Monster : Monsters)
        {
            TestEqual(TEXT("使用近战配置"), Monster->GetCombatASC()->GetUnitStats().AttackType, EY01AttackType::Melee);
            TestTrue(TEXT("两个怪均可发现并攻击房主"), Monster->RefreshDetectionTarget({F.Pawn}) == F.Pawn
                && Monster->GetCombatASC()->IsAutoAttackInRange(F.Pawn));
        }
        const double SecondDeadline = Second->GetNextAutoAttackTime();
        int32 FirstAttacks = 0, SecondAttacks = 0, DeathCallbacks = 0;
        const auto SpawnHook = F.World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([&](AActor* Actor)
        {
            if (!Cast<AY01TestAttack>(Actor)) { return; }
            if (Actor->GetOwner() == Monsters[0]) { ++FirstAttacks; }
            if (Actor->GetOwner() == Monsters[1]) { ++SecondAttacks; }
        }));
        ON_SCOPE_EXIT { F.World->RemoveOnActorSpawnedHandler(SpawnHook); F.Player->GetRevive()->PlanPolicy = {}; };
        F.Player->GetRevive()->PlanPolicy = [&](const auto&, auto&)
        {
            ++DeathCallbacks;
            TestFalse(TEXT("致死栈内已同步离房"), Training->Observe(F.Player).bInside);
            TestTrue(TEXT("致死栈内已同步清空记录"), Training->Monsters(F.Player).IsEmpty());
            TestTrue(TEXT("致死栈内会话已失效"), Training->Observe(F.Player).Session != Before.Session);
            // 同一真实死亡栈也覆盖外层TMap条目删除，不只覆盖Records.Reset。
            if (Case == TEXT("TrainingMeleeDeathRemove")) { Training->Remove(F.Player); }
        };
        Training->Update();
        TestEqual(TEXT("第一个近战怪同步杀死玩家"), F.Health(), 0.f);
        TestEqual(TEXT("死亡方案只执行一次"), DeathCallbacks, 1);
        TestEqual(TEXT("第一个怪确实发动攻击"), FirstAttacks, 1);
        TestEqual(TEXT("清房后第二个怪未发动攻击"), SecondAttacks, 0);
        TestEqual(TEXT("第二个怪普攻截止未推进"), Second->GetNextAutoAttackTime(), SecondDeadline);
        TestTrue(TEXT("第一个怪的真实普攻已提交"), First->GetNextAutoAttackTime() > F.World->GetTimeSeconds());
        TestTrue(TEXT("两个怪均被同步销毁"), Monsters[0]->IsActorBeingDestroyed() && Monsters[1]->IsActorBeingDestroyed());
        Training->Update();
        TestTrue(TEXT("再次更新仍无房内怪且等待复活"), Training->Monsters(F.Player).IsEmpty() && F.Player->GetRevive()->GetSnapshot().bWaiting);
        TestEqual(TEXT("再次更新不新增攻击"), FirstAttacks + SecondAttacks, 1);
        return true;
    }
    if (Case == TEXT("SafeCoreScaled")) { F.Core->SetActorScale3D(FVector(2,3,1)); }
    const auto OutsideCore = [&]
    {
        const float Radius = F.Pawn->GetCapsuleComponent()->GetScaledCapsuleRadius();
        const auto* CoreCapsule = F.Core->FindComponentByClass<UCapsuleComponent>();
        const auto* CoreShape = F.Core->FindComponentByClass<UStaticMeshComponent>();
        return CoreCapsule && CoreShape
            && FVector::DistSquared2D(F.Pawn->GetActorLocation(), CoreCapsule->GetComponentLocation())
                > FMath::Square(double(CoreCapsule->GetScaledCapsuleRadius() + Radius))
            && FVector::DistSquared2D(F.Pawn->GetActorLocation(), CoreShape->Bounds.GetBox().GetClosestPointTo(F.Pawn->GetActorLocation()))
                > FMath::Square(double(Radius));
    };
    if (Case == TEXT("ZeroPlanCore")) { F.Pawn->SetActorLocation(F.Core->GetActorLocation()); }
    AStaticMeshActor* Blocker = nullptr;
    if (Case == TEXT("SafeBlocked") || Case == TEXT("ZeroPlanBlocked"))
    {
        const FVector Center = Case == TEXT("ZeroPlanBlocked") ? F.Pawn->GetActorLocation() : F.Core->GetActorLocation();
        Blocker = F.World->SpawnActor<AStaticMeshActor>(Center, FRotator::ZeroRotator);
        // BeginPlay后Static组件拒绝换mesh；夹具必须先建立真实碰撞体。
        Blocker->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
        if (!TestTrue(TEXT("阻挡夹具成功设置mesh"), Blocker->GetStaticMeshComponent()->SetStaticMesh(F.Floor->GetStaticMeshComponent()->GetStaticMesh()))) { return false; }
        Blocker->SetActorScale3D(FVector(20,20,40)); Blocker->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
    }
    if (Case == TEXT("TrainingDeath")) { TestTrue(TEXT("进入练功房"), F.Mode->GetTraining()->Request(F.PC, true)); }
    if (Case == TEXT("SafeGround")) { F.Floor->SetActorEnableCollision(false); }
    const int32 GoldBefore = F.Player->GetGold();
    const double RemainderBefore = F.Player->GetIncome()->Observe().State.Remainder;
#ifdef Y01_REVIVE_AVAILABLE
    if (Case.StartsWith(TEXT("ZeroPlan")))
    { F.Player->GetRevive()->PlanPolicy = [](const FY01ReviveContext&, FY01RevivePlan& Plan)
        { Plan.WaitSeconds = 0; Plan.bAtDeathLocation = true; }; }
#endif
    const FVector DeathLocation = F.Pawn->GetActorLocation();
    F.Die();
#ifdef Y01_REVIVE_AVAILABLE
    const auto Death = F.Player->GetRevive()->GetSnapshot();
    TestEqual(TEXT("死亡即时快照序列"), Death.DeathSequence, uint64(1));
    TestEqual(TEXT("死亡时第一波规则"), Death.WaveIndex, 0);
    if (Case.StartsWith(TEXT("ZeroPlan")))
    {
        F.Tick(.025);
        if (Blocker)
        {
            TestEqual(TEXT("原地方案无安全点仍保持死亡"), F.Health(), 0.f);
            TestTrue(TEXT("原地方案无安全点仍等待"), F.Player->GetRevive()->GetSnapshot().bWaiting);
            Blocker->SetActorEnableCollision(false); F.Tick(.025);
        }
        TestEqual(TEXT("零等待方案真实复活"), F.Health(), 300.f);
        if (Case == TEXT("ZeroPlanCore"))
        { TestTrue(TEXT("死亡点在核心内部时必须另选安全点"), FVector::DistSquared2D(DeathLocation, F.Pawn->GetActorLocation()) > 1.); }
        else
        { TestTrue(TEXT("方案优先保留合法死亡位置"), FVector::DistSquared2D(DeathLocation, F.Pawn->GetActorLocation()) < 1.); }
        TestTrue(TEXT("原地方案也不重叠核心胶囊或外观"), OutsideCore());
        return true;
    }
#endif
    TestEqual(TEXT("死亡立即禁权威移动"), F.Pawn->GetCharacterMovement()->MovementMode, MOVE_None);
    TestTrue(TEXT("死亡收入不是永久停止"), F.Player->GetIncome()->Observe().State.Phase != EY01IncomePhase::Stopped);
    if (Case == TEXT("TrainingDeath"))
    {
        TestFalse(TEXT("死亡即时离开练功房"), F.Mode->GetTraining()->Observe(F.Player).bInside);
        TestTrue(TEXT("死亡即时清本房怪"), F.Mode->GetTraining()->Monsters(F.Player).IsEmpty());
        TestFalse(TEXT("死亡拒绝切区"), F.Mode->GetTraining()->Request(F.PC, true));
    }
    F.Tick(.5); TestEqual(TEXT("等待时无回血"), F.Health(), 0.f);
    TestEqual(TEXT("等待期自然收入暂停"), F.Player->GetGold(), GoldBefore);
    TestTrue(TEXT("死亡前合法小数不丢失"), FMath::IsNearlyEqual(F.Player->GetIncome()->Observe().State.Remainder, RemainderBefore, 1.e-7));
    TestTrue(TEXT("全队死亡不终局"), F.Mode->GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Ended);
    if (Case == TEXT("WaveSnapshot"))
    { TestEqual(TEXT("换波不重算死亡deadline"), F.Player->GetRevive()->GetSnapshot().Deadline, Death.Deadline); }
    F.Tick(.6);
    if (Case == TEXT("SafeGround"))
    {
        TestEqual(TEXT("没有地面不盲目复活"), F.Health(), 0.f);
        TestTrue(TEXT("没有地面继续等待而非取消"), F.Player->GetRevive()->GetSnapshot().bWaiting);
        F.Floor->SetActorEnableCollision(true); F.Tick(.1);
    }
    if (Blocker)
    {
        TestEqual(TEXT("核心周围全部阻挡时不复活"), F.Health(), 0.f);
        TestTrue(TEXT("阻挡未解除继续等待"), F.Player->GetRevive()->GetSnapshot().bWaiting);
        Blocker->SetActorEnableCollision(false); F.Tick(.1);
    }
    TestEqual(TEXT("同Pawn按有效MaxHealth满血复活"), F.Health(), F.Player->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()));
    TestTrue(TEXT("保留原Pawn"), F.PC->GetPawn() == F.Pawn);
    TestTrue(TEXT("出生位于核心旁且不在死亡远点"), FVector::DistSquared2D(F.Pawn->GetActorLocation(), F.Core->GetActorLocation()) < FMath::Square(600.));
    TestTrue(TEXT("普通复活不重叠核心实际胶囊或无碰撞外观"), OutsideCore());
    const int64 IncomeAtRevive = F.Player->GetIncome()->Observe().State.TotalIncome;
    F.Tick(1.);
    TestTrue(TEXT("复活后重新收入"), F.Player->GetIncome()->Observe().State.TotalIncome > IncomeAtRevive);
    TestTrue(TEXT("恢复权威移动"), F.Pawn->GetCharacterMovement()->MovementMode != MOVE_None);
#ifdef Y01_REVIVE_AVAILABLE
    TestEqual(TEXT("一次死亡一个序列"), F.Player->GetRevive()->GetSnapshot().DeathSequence, uint64(1));
#else
    AddError(TEXT("缺少权威死亡同包快照及复活方案执行接口"));
#endif
    return true;
}
#endif
