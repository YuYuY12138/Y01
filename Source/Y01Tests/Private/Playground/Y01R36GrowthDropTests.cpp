#include "Misc/AutomationTest.h"
#include "Y01MatchFixture.h"
#include "Playground/Y01R36Verification.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01GrowthComponent.h"
#include "Player/Y01DropDeliveryComponent.h"
#include "Player/Y01EconomyComponent.h"
#include "Player/Y01IncomeComponent.h"
#include "Playground/Y01TestSkill.h"
#include "Playground/Y01R36UI.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "UObject/UnrealType.h"
#include <limits>
#include "Player/Y01Character.h"
#include "Combat/Y01Monster.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/ScopeExit.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_DEV_AUTOMATION_TESTS
struct FY01R36DeliveryTestAccess
{
    static bool Lock(UY01LoadoutComponent* L) { return L->Transactions.Enter(); }
    static void Unlock(UY01LoadoutComponent* L) { L->Transactions.Leave(); }
    static int32 Seed(UY01EconomyComponent* E) { return E->LootRandom.GetCurrentSeed(); }
    static void Exhaust(UY01LoadoutComponent* L) { L->AcquisitionSerial = MAX_uint64; }
    static void Clock(UY01IncomeComponent* I, TFunction<double()> F) { I->TestClock = MoveTemp(F); }
};
struct FY01R36GrowthTestAccess
{
    static void Replay(UY01GrowthComponent* Growth, const FY01KillEvent& Event) { Growth->Consume(Event); }
};
namespace
{
using K = EY01TableKind;
using O = EY01ItemOperation;
struct FGrowthWorld
{
    FY01MatchFixture Data;
    UWorld* World = nullptr;
    AY01R36RuntimeProbe* Mode = nullptr;
    AY01R36Controller* PC = nullptr;
    AY01PlayerState* P = nullptr;
    FString Error;
    FGrowthWorld(double Delta = 1, const FString& Scenario = TEXT(""))
    {
        auto& C = Data.Row<FY01CharacterRow>(K::Character);
        C.InitialEquipment.Reset(); C.InitialSkills.Reset(); C.AttackDamage = 30; C.Strength = 0; C.Health = 300; C.Range = 1000;
        FY01SourceEffectRow E; E.Magnitude = 10; Data.Add(K::SourceEffect, E, TEXT("Ten"));
        FY01GrowthRuleRow G; G.Delta = Delta; Data.Add(K::GrowthRule, G, TEXT("ItemAttack"));
        G.Target = EY01GrowthTarget::Player; G.Attribute = EY01GrowthAttribute::Strength;
        if (Scenario == TEXT("FaultFloat") || Scenario == TEXT("UIFault")) { G.Delta = double(MAX_flt) * 2; }
        if (Scenario == TEXT("FaultUnderflow")) { G.Delta = 1.e-300; }
        if (Scenario == TEXT("FaultInitialStrength")) { C.Strength = 1; G.Delta = double(MAX_flt); }
        if (Scenario == TEXT("FaultInitialFloat")) { C.Strength = 1; G.Delta = 1.e10; }
        Data.Add(K::GrowthRule, G, TEXT("PlayerStrength"));
        if (Scenario == TEXT("FaultAccumulate")) { C.Strength = MAX_flt; }
        if (Scenario == TEXT("EffectIdentityCollision")) { Data.Add(K::SourceEffect, E, TEXT("Growth:ItemAttack")); }
        for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Blue"))})
        {
            auto& Item = Data.Row<FY01EquipmentRow>(K::Equipment, Name); Item.DisplayName = FText::FromString(TEXT("成长装备"));
            Item.EffectIds = {Data.Ref(K::SourceEffect, Scenario == TEXT("EffectIdentityCollision") ? TEXT("Growth:ItemAttack") : TEXT("Ten"))};
            Item.GrowthRuleIds = {Data.Ref(K::GrowthRule, TEXT("ItemAttack")), Data.Ref(K::GrowthRule, TEXT("PlayerStrength"))};
        }
        auto& M = Data.Row<FY01MonsterRow>(K::Monster); M.Health = 20; M.AttackDamage = 0;
        if (Scenario.StartsWith(TEXT("Delivery")) || Scenario.StartsWith(TEXT("Automatic")))
        {
            FY01RewardPackageRow Reward; Reward.Items.Add({Data.Ref(K::Equipment), Scenario == TEXT("DeliveryRetention") ? 65 : 1});
            if (Scenario == TEXT("DeliveryPoolQuantity"))
            { Reward.Items.Add({Data.Ref(K::LocalItemPool, TEXT("Blue")), 3});
              auto& Skill = Data.Row<FY01SkillRow>(K::Skill); Skill.Type = EY01SkillType::Passive; Skill.Ability = nullptr;
              Skill.DisplayName = FText::FromString(TEXT("掉落技能书")); Reward.Items.Add({Data.Ref(K::Skill), 2}); }
            if (Scenario == TEXT("DeliveryGrowthFault"))
            { Data.Row<FY01GrowthRuleRow>(K::GrowthRule, TEXT("PlayerStrength")).Delta = double(MAX_flt) * 2; }
            Data.Add(K::RewardPackage, Reward, TEXT("DropOnly"));
            M.DropReward = Data.Ref(K::RewardPackage, TEXT("DropOnly")); M.ItemDropChance = Scenario == TEXT("DeliveryZero") ? 0 : 1;
            if (Scenario == TEXT("DeliveryProbability")) { M.ItemDropChance = .5; }
        }
        auto& W = Data.Row<FY01WaveRow>(K::Wave); W.PreparationSeconds = .01f; W.Spawns[0].DelaySeconds = 0; W.Spawns[0].Count = 8;
        auto& T = Data.Row<FY01TrainingStageRow>(K::TrainingStage); T.Spawns[0].DelaySeconds = 0; T.Spawns[0].Count = 8; T.MaxAlive = 8;
        auto* Profile = NewObject<UY01R36Profile>(); Profile->bEvidenceMode = true; Profile->bEconomyEnabled = true; Profile->bTrainingEnabled = true;
        Profile->Character = Data.Ref(K::Character); Profile->OrderedWaves = Data.Waves();
        if (Scenario.StartsWith(TEXT("Automatic")))
        {
            Profile->bEvidenceMode = false;
            W.Spawns = {{Data.Ref(K::Monster), 1, 0}, {Data.Ref(K::Monster), 1, 6}};
            auto& Next = Data.Row<FY01WaveRow>(K::Wave, TEXT("Boss"));
            Next.PreparationSeconds = .2f; Next.Spawns = {{Data.Ref(K::Monster), 1, 0}};
            if (Scenario == TEXT("AutomaticFlight"))
            {
                Profile->bR34AutoAttackEnabled = true; C.AttackType = EY01AttackType::Ranged;
                C.AttackInterval = 10; C.ProjectileSpeed = 200;
            }
        }
        if (Scenario == TEXT("DeliveryImmediate")) { Profile->DropFlightSeconds = 0; }
        if (Scenario == TEXT("SourcesPassive") || Scenario == TEXT("SourcesActive") || Scenario == TEXT("SourcesDormant"))
        {
            auto& Skill = Data.Row<FY01SkillRow>(K::Skill); Skill.DisplayName = FText::FromString(TEXT("成长技能"));
            Skill.Type = Scenario == TEXT("SourcesPassive") ? EY01SkillType::Passive : EY01SkillType::Active;
            Skill.Ability = nullptr; Skill.TestSkill = {};
            if (Skill.Type == EY01SkillType::Active)
            { Skill.Ability = UY01TestSkillAbility::StaticClass(); Skill.TestSkill.Mode = EY01TestSkillMode::Area;
              Skill.TestSkill.Radius = 100; Skill.TestSkill.ActiveSeconds = .1f; }
            Skill.GrowthRuleIds = {Data.Ref(K::GrowthRule, TEXT("PlayerStrength"))};
            FY01ShopOfferRow Offer; Offer.ShopKind = EY01ShopKind::DirectEffect; Offer.Price = 0; Offer.DisplayName = FText::FromString(TEXT("成长商品"));
            Offer.GrowthRuleIds = Skill.GrowthRuleIds; Data.Add(K::ShopOffer, Offer, TEXT("Direct"));
            FY01TalentChoiceRow Choice;
            for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Two")), FName(TEXT("Three"))})
            {
                FY01TalentRow Talent; Talent.DisplayName = FText::FromString(TEXT("成长天赋")); Talent.GrowthRuleIds = Skill.GrowthRuleIds;
                Data.Add(K::Talent, Talent, Name); Choice.Candidates.Add(Data.Ref(K::Talent, Name));
            }
            Data.Add(K::TalentChoice, Choice); Profile->TalentChoice = Data.Ref(K::TalentChoice);
            Profile->ContentShopOffers = {Data.Ref(K::ShopOffer, TEXT("Direct"))};
        }
        if (Scenario.StartsWith(TEXT("Attribute")))
        {
            C.Strength = 10; C.AttackDamage = 0;
            FY01AttributeMappingRow Mapping; Mapping.SourceAttribute = EY01Attribute::Strength;
            Mapping.TargetAttribute = EY01Attribute::AttackDamage; Mapping.Coefficient = 3;
            Data.Add(K::AttributeMapping, Mapping, TEXT("StrengthAttack"));
            Mapping.TargetAttribute = EY01Attribute::GoldPerSecond; Mapping.Coefficient = 1;
            Data.Add(K::AttributeMapping, Mapping, TEXT("StrengthIncome"));
            Profile->BaseAttributeMappings = {Data.Ref(K::AttributeMapping, TEXT("StrengthAttack")), Data.Ref(K::AttributeMapping, TEXT("StrengthIncome"))};
            Profile->InitialGold = 7;
            FY01SourceEffectRow Effect; Effect.Kind = EY01SourceEffectKind::MappingCoefficientAdd;
            Effect.SourceAttribute = EY01Attribute::Strength; Effect.TargetAttribute = EY01Attribute::AttackDamage; Effect.Amount = 1;
            Data.Add(K::SourceEffect, Effect, TEXT("AttackCoefficient"));
            Effect.TargetAttribute = EY01Attribute::GoldPerSecond; Effect.Amount = 2;
            Data.Add(K::SourceEffect, Effect, TEXT("IncomeCoefficient"));
            auto& Item = Data.Row<FY01EquipmentRow>(K::Equipment);
            Item.EffectIds = {Data.Ref(K::SourceEffect, TEXT("AttackCoefficient")), Data.Ref(K::SourceEffect, TEXT("IncomeCoefficient"))};
            Item.GrowthRuleIds = {Data.Ref(K::GrowthRule, TEXT("PlayerStrength"))};
            Data.Row<FY01GrowthRuleRow>(K::GrowthRule, TEXT("PlayerStrength")).Delta = 10;
            if (Scenario == TEXT("AttributeCombined"))
            {
                Effect = {}; Effect.Kind = EY01SourceEffectKind::AttributeFlat; Effect.TargetAttribute = EY01Attribute::Strength; Effect.Amount = 5;
                Data.Add(K::SourceEffect, Effect, TEXT("DirectStrength")); Item.EffectIds.Add(Data.Ref(K::SourceEffect, TEXT("DirectStrength")));
            }
            if (Scenario == TEXT("AttributeHealth") || Scenario == TEXT("AttributeRollback"))
            {
                Effect = {}; Effect.Kind = EY01SourceEffectKind::AttributeFlat; Effect.Amount = 100; Effect.TargetAttribute = EY01Attribute::MaxHealth;
                Data.Add(K::SourceEffect, Effect, TEXT("Health")); Item.EffectIds.Add(Data.Ref(K::SourceEffect, TEXT("Health")));
                Effect.Amount = 1; Effect.TargetAttribute = EY01Attribute::AttackRate;
                Data.Add(K::SourceEffect, Effect, TEXT("Rate")); Item.EffectIds.Add(Data.Ref(K::SourceEffect, TEXT("Rate")));
            }
        }
        for (const auto& Pair : Data.Tables) { Profile->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01R36RuntimeProbe::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01R36RuntimeProbe>(); World->SpawnActor<AY01R36Config>()->Content = Profile;
        PC = Player(); P = PC->GetPlayerState<AY01PlayerState>();
        if (Scenario.StartsWith(TEXT("Automatic"))) { PC->GetPawn()->SetActorLocation(FVector(10000, 0, 100)); }
        Mode->LoadMatchConfiguration(Error); World->BeginPlay();
        if (!Scenario.StartsWith(TEXT("Automatic"))) { Mode->TryStartNextWave(); }
        TGuardValue<uint64> Frame(GFrameCounter, GFrameCounter);
        for (int32 I = 0; I < 4 && Mode->GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Battle; ++I)
        { ++GFrameCounter; World->Tick(LEVELTICK_All, .05f); }
    }
    ~FGrowthWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    AY01R36Controller* Player()
    {
        auto* Controller = World->SpawnActor<AY01R36Controller>(); auto* State = Controller->GetPlayerState<AY01PlayerState>();
        if (!State) { State = World->SpawnActor<AY01PlayerState>(); Controller->PlayerState = State; }
        State->SetOwner(Controller); Mode->GetFlowState()->AddPlayerState(State);
        FActorSpawnParameters Params; Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(0, 0, 100), FRotator::ZeroRotator, Params);
        Controller->Possess(Pawn); State->GetCombatASC()->InitAbilityActorInfo(State, Pawn); Pawn->GetCharacterMovement()->DisableMovement();
        return Controller;
    }
    FY01CommittedView View() const { P->GetLoadout()->PublishCommittedView(); FY01CommittedView V; P->GetLoadout()->ReadCommittedView(V); return V; }
    FY01ItemRequest Request(FGuid Id, O Op = O::Equip) const
    {
        auto V = View(); FY01ItemRequest R; R.MatchId = V.MatchId; R.JoinSequence = V.JoinSequence; R.Seq = V.ExpectedSeq;
        R.Operation = Op; R.ItemId = Id; R.Slot = 0; R.ConfigRevision = V.ConfigRevision;
        for (const auto& I : V.Items) { if (I.ItemId == Id) { R.ExpectedEntityRevision = I.EntityRevision; } }
        R.ExpectedSlotItemId = V.EquipmentSlots[0];
        for (const auto& I : V.Items) { if (I.ItemId == R.ExpectedSlotItemId) { R.ExpectedSlotEntityRevision = I.EntityRevision; } }
        return R;
    }
    bool Send(FGuid Id, O Op = O::Equip) { return P->GetLoadout()->SubmitRequest(PC, Request(Id, Op)).IsOk(); }
    float Attack() const { return P->GetCombatASC()->GetUnitStats().AttackDamage; }
    bool Hit(AY01Monster* Target, AY01PlayerState* Source = nullptr, float Damage = 100)
    {
        auto* ASC = (Source ? Source : P)->GetCombatASC(); const float Before = Target->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
        auto Spec = ASC->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, ASC->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, Damage); ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data, Target->GetCombatASC());
        return Target->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) < Before;
    }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36AttributeTest, "Y01.Framework.R36.Attribute",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36AttributeTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : {TEXT("BaseAndIdempotency"), TEXT("EquipmentGrowth"), TEXT("Combined"), TEXT("Health"),
        TEXT("Rollback"), TEXT("FutureIncome"), TEXT("TwoPlayers"), TEXT("LegacyAndBaseAttack"), TEXT("Wire"), TEXT("Validation")})
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01R36AttributeTest::RunTest(const FString& Case)
{
    const FString Command = FCommandLine::Get(); FCommandLine::Set(TEXT("")); ON_SCOPE_EXIT { FCommandLine::Set(*Command); };
    if (Case == TEXT("Validation"))
    {
        FY01MatchFixture Data;
        TestTrue(TEXT("无人引用可缺映射表，仍只必填旧19表"), FY01DataCatalog(Data.Tables).Validate().IsValid());
        TestEqual(TEXT("追加表值23"), int32(K::AttributeMapping), 23);
        FY01AttributeMappingRow Mapping; Mapping.TargetAttribute = EY01Attribute::Strength;
        Data.Add(K::AttributeMapping, Mapping);
        TestFalse(TEXT("存在即校验，拒绝三维互转"), FY01DataCatalog(Data.Tables).Validate().IsValid());
        Mapping.TargetAttribute = EY01Attribute::AttackDamage; Data.Add(K::AttributeMapping, Mapping);
        FY01SourceEffectRow E; E.Kind = EY01SourceEffectKind::AttributeFlat; E.TargetAttribute = EY01Attribute::Strength; E.Amount = 5;
        Data.Add(K::SourceEffect, E); Data.Row<FY01SkillRow>(K::Skill).EffectIds = {Data.Ref(K::SourceEffect)};
        TestFalse(TEXT("未接新能力的技能严格拒绝"), FY01DataCatalog(Data.Tables).Validate().IsValid());
        E.Kind = EY01SourceEffectKind::MappingCoefficientAdd; E.SourceAttribute = EY01Attribute::Strength;
        E.TargetAttribute = static_cast<EY01Attribute>(255); TestFalse(TEXT("未知目标拒绝"), Y01Data::ValidSourceEffect(E));
        E.TargetAttribute = EY01Attribute::Armor; E.Amount = std::numeric_limits<double>::infinity();
        TestFalse(TEXT("非有限系数拒绝"), Y01Data::ValidSourceEffect(E));
        FY01UnitCombatStats Base; Base.Strength = 10; Base.AttackInterval = 1;
        FY01SourceEffectSnapshot Direct; Direct.Kind = EY01SourceEffectKind::AttributeFlat;
        Direct.TargetAttribute = EY01Attribute::Strength; Direct.Amount = 1.e-10; FY01ResolvedAttributes Resolved;
        TestFalse(TEXT("float完全吞掉正三维增量必须拒绝"), FY01AttributeResolver::Resolve(Base, 0, {Direct}, Resolved).IsOk());
        return true;
    }
    FGrowthWorld F(10, TEXT("Attribute") + Case);
    if (!TestTrue(TEXT("属性配置初始化"), F.Error.IsEmpty())) { AddError(F.Error); return false; }
    auto* S = F.P->GetSourceEffects(); auto* ASC = F.P->GetCombatASC(); auto* L = F.P->GetLoadout();
    TestEqual(TEXT("10力量按3换算30攻击"), F.Attack(), 30.f);
    TestEqual(TEXT("10力量按1换算10收入"), F.P->GetIncome()->Observe().State.Rate, 10.);
    if (Case == TEXT("BaseAndIdempotency"))
    {
        const int32 Gold = F.P->GetGold(); F.Mode->OnPlayerInitialized(F.P); F.Mode->OnPlayerInitialized(F.P);
        const TArray<FY01DataRef> Mappings = {F.Data.Ref(K::AttributeMapping, TEXT("StrengthAttack")), F.Data.Ref(K::AttributeMapping, TEXT("StrengthIncome"))};
        TestTrue(TEXT("Profile自身重复入口幂等"), S->InitializeProfile(FY01DataCatalog(F.Data.Tables), Mappings, 7, 0).IsOk());
        TestFalse(TEXT("初始金币冲突拒绝"), S->InitializeProfile(FY01DataCatalog(F.Data.Tables), Mappings, 8, 0).IsOk());
        TestFalse(TEXT("基础收入冲突拒绝"), S->InitializeProfile(FY01DataCatalog(F.Data.Tables), Mappings, 7, 1).IsOk());
        TestFalse(TEXT("引用缺行拒绝"), S->InitializeProfile(FY01DataCatalog(F.Data.Tables), {F.Data.Ref(K::AttributeMapping, TEXT("Missing"))}, 7, 0).IsOk());
        TestEqual(TEXT("重复初始化不重复授金"), F.P->GetGold(), Gold);
        TestEqual(TEXT("初始金币入口"), Gold, 7); return true;
    }
    if (Case == TEXT("LegacyAndBaseAttack"))
    {
        FY01PreparedSource Plan; FY01SourceTransaction Tx;
        TestTrue(TEXT("旧AttackFlat来源准备"), S->PrepareSource(FY01DataCatalog(F.Data.Tables), EY01RuntimeSourceKind::Base,
            TEXT("Legacy"), {F.Data.Ref(K::SourceEffect, TEXT("Ten"))}, Plan).IsOk());
        TestTrue(TEXT("旧来源暂存"), S->StageSource(Plan, Tx).IsOk()); TestTrue(TEXT("旧来源提交"), S->CommitSource(Tx).IsOk());
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 5);
        TestEqual(TEXT("基础攻击API保持有效"), F.Attack(), 45.f);
        TestTrue(TEXT("移除旧来源"), S->RemoveSource(EY01RuntimeSourceKind::Base, TEXT("Legacy")).IsOk());
        TestEqual(TEXT("重算不覆盖新基础攻击"), F.Attack(), 35.f); return true;
    }
    double Now = F.P->GetIncome()->Observe().State.LastTime;
    FY01R36DeliveryTestAccess::Clock(F.P->GetIncome(), [&] { return Now; });
    ON_SCOPE_EXIT { FY01R36DeliveryTestAccess::Clock(F.P->GetIncome(), {}); };
    const int32 BeforeGold = F.P->GetGold(); Now += 2;
    double Deadline = ASC->GetNextAutoAttackTime();
    if (Case == TEXT("Health"))
    {
        auto* Target = F.Mode->GetWaveMonsters()[0].Get(); Target->SetActorLocation(F.PC->GetPawn()->GetActorLocation() + FVector(50, 0, 0));
        TestTrue(TEXT("建立正在等待的普攻CD"), ASC->TryAutoAttack(Target, [](float) { return true; }));
        Deadline = ASC->GetNextAutoAttackTime();
    }
    FY01ItemGrant Item{FGuid::NewGuid(), F.Data.Ref(K::Equipment)};
    if (!TestTrue(TEXT("新装备入包"), L->AddItems(FY01DataCatalog(F.Data.Tables), {Item}).IsOk())
        || !TestTrue(TEXT("新装备完整事务穿戴"), F.Send(Item.ItemId))) { return false; }
    const bool Combined = Case == TEXT("Combined");
    TestEqual(TEXT("同次三维及系数换算"), F.Attack(), Combined ? 60.f : 40.f);
    TestEqual(TEXT("装备收入系数加2"), F.P->GetIncome()->Observe().State.Rate, Combined ? 45. : 30.);
    if (Case == TEXT("Wire"))
    {
        const auto V = F.View(); FY01ViewSender Sender; FY01ViewReceiver Receiver; FY01ViewWire Wire; FString Error;
        if (!TestTrue(TEXT("新字段完整视图编码"), Sender.Offer(V, Wire, Error))) { return false; }
        for (int32 I = 0; I < 100 && !Wire.Items.IsEmpty(); ++I)
        { FY01ViewAck Ack; if (!TestTrue(TEXT("完整窗口接收"), Receiver.Receive(Wire, Ack, Error))) { return false; }
          if (!TestTrue(TEXT("完整窗口确认"), Sender.Acknowledge(Ack, Wire, Error))) { return false; } }
        TestTrue(TEXT("属性和系数新字段往返一致"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Receiver.Get(), &V, 0));
        auto Bad = V; Bad.Items[0].Effects[0].TargetAttribute = EY01Attribute::Wisdom;
        FY01ViewSender InvalidSender; TestFalse(TEXT("二进制入口拒绝三维互转"), InvalidSender.Offer(Bad, Wire, Error)); return true;
    }
    if (Case == TEXT("FutureIncome"))
    {
        TestEqual(TEXT("变化前2秒仍按10"), F.P->GetGold() - BeforeGold, 20);
        Now += 1; TestTrue(TEXT("新段结算"), F.P->GetIncome()->Settle().IsOk());
        TestEqual(TEXT("未来1秒才按30"), F.P->GetGold() - BeforeGold, 50); return true;
    }
    if (Case == TEXT("Health") || Case == TEXT("Rollback"))
    {
        TestEqual(TEXT("上限上涨不治疗"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 300.f);
        TestEqual(TEXT("新上限真实GAS"), ASC->GetUnitStats().Health, 400.f);
        TestEqual(TEXT("攻速取倒数"), ASC->GetUnitStats().AttackInterval, .5f);
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 350);
        if (Case == TEXT("Rollback"))
        {
            FY01SourceSnapshot Snapshot; S->Observe(Snapshot);
            const auto* Effect = Snapshot.Effects.FindByPredicate([](const auto& E) { return E.Source.Kind == EY01RuntimeSourceKind::Equipment; });
            if (!TestNotNull(TEXT("装备来源"), Effect)) { return false; }
            FY01PreparedMutation Plan; FY01SourceTransaction Tx;
            TestTrue(TEXT("准备卸下"), S->PrepareMutation({}, {Effect->Source}, Plan).IsOk());
            TestTrue(TEXT("暂存卸下"), S->StageMutation(Plan, Tx).IsOk());
            TestEqual(TEXT("Stage不提前损血"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 350.f);
            TestFalse(TEXT("Stage禁止GA新攻击"), ASC->IsAbilityReady(ASC->GetBasicAttackHandle()));
            TestEqual(TEXT("重入请求拒绝并中断外层"), S->RemoveSource(EY01RuntimeSourceKind::Equipment, Effect->Source.LocalKey).Code, EY01RuntimeError::Busy);
            TestFalse(TEXT("实际失败提交自动回滚"), S->CommitMutation(Tx).IsOk());
            TestEqual(TEXT("失败路径仍350血"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 350.f);
            TestEqual(TEXT("失败保留上限"), ASC->GetUnitStats().Health, 400.f);
            TestEqual(TEXT("失败恢复换算攻击"), F.Attack(), 40.f); return true;
        }
        TestEqual(TEXT("攻速变化不重置正在等待CD"), ASC->GetNextAutoAttackTime(), Deadline);
        bool EndedInside = false;
        const auto Hook = ASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetMaxHealthAttribute()).AddLambda(
            [&](const FOnAttributeChangeData&) { EndedInside = true; S->EndSession(); });
        TestTrue(TEXT("Commit广播重入结束仍为成功卸下"), F.Send(Item.ItemId, O::Unequip));
        ASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetMaxHealthAttribute()).Remove(Hook);
        TestTrue(TEXT("真实执行提交重入"), EndedInside);
        TestEqual(TEXT("成功后才钳血"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 300.f); return true;
    }
    if (Case == TEXT("TwoPlayers"))
    {
        auto* Other = F.Player()->GetPlayerState<AY01PlayerState>(); FString Error;
        if (!TestTrue(TEXT("第二玩家真实角色初始化"), Other->TryInitializeCharacter(FY01DataCatalog(F.Data.Tables), F.Data.Ref(K::Character), Error))) { return false; }
        F.Mode->OnPlayerInitialized(Other);
        TestEqual(TEXT("另一玩家没有装备系数"), Other->GetCombatASC()->GetUnitStats().AttackDamage, 30.f);
        TestEqual(TEXT("另一玩家收入隔离"), Other->GetIncome()->Observe().State.Rate, 10.); return true;
    }
    if (Case == TEXT("EquipmentGrowth"))
    {
        if (!TestTrue(TEXT("实际击杀触发成长"), F.Hit(F.Mode->GetWaveMonsters()[0]))) { return false; }
        TestEqual(TEXT("成长至20力量"), ASC->GetUnitStats().Strength, 20.f);
        TestEqual(TEXT("同次成长换算80攻击"), F.Attack(), 80.f);
        TestEqual(TEXT("同次成长换算60收入"), F.P->GetIncome()->Observe().State.Rate, 60.);
    }
    TestTrue(TEXT("卸装"), F.Send(Item.ItemId, O::Unequip));
    TestEqual(TEXT("卸装移除系数及装备直接值，保留玩家成长"), F.Attack(), Case == TEXT("EquipmentGrowth") ? 60.f : 30.f);
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36GrowthDropTest, "Y01.Framework.R36.GrowthDrop",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36GrowthDropTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Mainline"), TEXT("Training"), TEXT("PermanentItem"), TEXT("LastHit"), TEXT("Fraction"),
        TEXT("Zero"), TEXT("DirectZeroRejected"), TEXT("DestroyRejected"), TEXT("UnregisteredRejected"), TEXT("CrossRegionRejected"),
        TEXT("DuplicateInitialization"), TEXT("DeathSessionFrozen"), TEXT("DamageSnapshot"), TEXT("KillBeforeUnequip"), TEXT("SourceStagingFrozen"),
        TEXT("SourcesPassive"), TEXT("SourcesActive"), TEXT("SourcesDormant"), TEXT("FaultFloat"), TEXT("FaultUnderflow"), TEXT("FaultAccumulate"), TEXT("FaultInitialStrength"), TEXT("FaultInitialFloat"),
        TEXT("FaultGE"), TEXT("GrowthReentry"), TEXT("NestedDeathOrder"), TEXT("ConsumerReentry"), TEXT("EventIdempotency"),
        TEXT("RegisterIdempotency"), TEXT("Ended"), TEXT("Logout"), TEXT("AutoAttackSnapshot"), TEXT("WireLedger"), TEXT("WireInvalidLedger"),
        TEXT("SourceOnlyBusy"), TEXT("EffectIdentityCollision"), TEXT("UIGrowth"), TEXT("UIFault"), TEXT("WireLedgerValidation"),
        TEXT("SourceStageCommit"), TEXT("SourceStageRollback"), TEXT("DuringGrowthEnded"), TEXT("DuringDeathEnded"),
        TEXT("DuringDeathLogout"), TEXT("LegacyDisabled"), TEXT("DeliveryETA"), TEXT("DeliveryZero"),
        TEXT("DeliveryPoolQuantity"), TEXT("DeliveryDuplicate"), TEXT("DeliveryBusy"), TEXT("DeliveryDeath"),
        TEXT("DeliveryRegion"), TEXT("DeliveryTrainingClear"), TEXT("DeliveryLogout"), TEXT("DeliveryEnded"),
        TEXT("DeliveryEndedReentry"), TEXT("DeliveryLastHit"), TEXT("DeliveryGrowthFault"), TEXT("DeliveryTechnical"),
        TEXT("DeliverySnapshot"), TEXT("DeliveryDirectZero"), TEXT("DeliveryDestroy"), TEXT("DeliveryDuration"),
        TEXT("DeliveryWire"), TEXT("DeliveryWireInvalid"), TEXT("DeliveryRetention"), TEXT("DeliveryImmediate"),
        TEXT("DeliveryProfileDuration"), TEXT("DeliveryIdCollision"), TEXT("DeliveryTrace"),
        TEXT("DeliveryProbability"), TEXT("DeliveryWireAtomic"), TEXT("AutomaticOverlap"), TEXT("AutomaticFlight")}) { Names.Add(N); Commands.Add(N); }
}
bool FY01R36GrowthDropTest::RunTest(const FString& Case)
{
    const FString Command = FCommandLine::Get(); FCommandLine::Set(TEXT("")); ON_SCOPE_EXIT { FCommandLine::Set(*Command); };
    const double Delta = Case == TEXT("Fraction") ? .25 : Case == TEXT("Zero") ? 0 : 1;
    if (Case == TEXT("LegacyDisabled"))
    {
        FY01MatchFixture Data; auto& C = Data.Row<FY01CharacterRow>(K::Character); C.InitialEquipment.Reset(); C.InitialSkills.Reset();
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).CreateNavigation(false).CreateAISystem(false);
        auto* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        ON_SCOPE_EXIT { World->DestroyWorld(false); GEngine->DestroyWorldContext(World); };
        World->InitializeActorsForPlay(FURL());
        auto* P = World->SpawnActor<AY01PlayerState>(); auto* M = World->SpawnActor<AY01Monster>(); FString Error;
        const bool PlayerReady = P->TryInitializeCharacter(FY01DataCatalog(Data.Tables), Data.Ref(K::Character), Error);
        TestTrue(FString(TEXT("旧框架真实角色初始化：")) + Error, PlayerReady);
        const bool MonsterReady = M->TryInitializeMonster(FY01DataCatalog(Data.Tables), Data.Ref(K::Monster), Error);
        TestTrue(FString(TEXT("旧框架真实怪物初始化：")) + Error, MonsterReady);
        if (!PlayerReady || !MonsterReady) { return false; }
        TestTrue(TEXT("旧框架目标致死前有正生命"), M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) > 0);
        auto* Kills = World->GetSubsystem<UY01KillRewards>(); int32 Count = 0;
        Kills->OnEffectiveKill.AddLambda([&](const FY01KillEvent&) { ++Count; });
        TestFalse(TEXT("默认成长不启用"), P->GetGrowth()->IsEnabled()); TestFalse(TEXT("默认登记不启用"), Kills->RegisterMonster(M).IsValid());
        auto* ASC = P->GetCombatASC(); const auto Before = ASC->GetUnitStats();
        auto Spec = ASC->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, ASC->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, 100); ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data, M->GetCombatASC());
        TestEqual(TEXT("旧框架伤害真实致死"), M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 0.f);
        TestEqual(TEXT("旧框架不产生新奖励事件"), Count, 0); TestEqual(TEXT("旧框架力量不变"), ASC->GetUnitStats().Strength, Before.Strength);
        Kills->OnEffectiveKill.Clear(); return true;
    }
    FGrowthWorld F(Delta, Case);
    if (!TestTrue(TEXT("真实R36配置"), F.Error.IsEmpty())) { AddError(F.Error); return false; }
    TestTrue(TEXT("实际Battle"), F.Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Battle);
    if (Case.StartsWith(TEXT("Automatic")))
    {
        TGuardValue<uint64> Frame(GFrameCounter, GFrameCounter);
        auto Tick = [&](int32 Count) { for (int32 I = 0; I < Count; ++I) { ++GFrameCounter; F.World->Tick(LEVELTICK_All, .05f); } };
        TestTrue(TEXT("真实日常策略且未调用手动开始"), F.Mode->IsAutomaticWaves());
        if (!TestEqual(TEXT("仅首波立即批次已生成"), F.Mode->GetWaveMonsters().Num(), 1)) { return false; }
        auto* Old = F.Mode->GetWaveMonsters()[0].Get();
        Old->GetCharacterMovement()->DisableMovement();
        Old->SetActorLocation(F.PC->GetPawn()->GetActorLocation() + FVector(400, 0, 0));
        FY01ItemGrant Item{FGuid::NewGuid(), F.Data.Ref(K::Equipment)};
        if (!TestTrue(TEXT("成长装备真实入包"), F.P->GetLoadout()->AddItems(FY01DataCatalog(F.Data.Tables), {Item}).IsOk())
            || !TestTrue(TEXT("成长装备真实穿戴"), F.Send(Item.ItemId))) { return false; }
        auto* ASC = F.P->GetCombatASC(); const auto Session = ASC->GetCombatSession();
        auto* Kills = F.World->GetSubsystem<UY01KillRewards>(); TArray<FY01KillEvent> Events;
        const auto Hook = Kills->OnEffectiveKill.AddLambda([&](const auto& Event) { Events.Add(Event); });
        ON_SCOPE_EXIT { Kills->OnEffectiveKill.Remove(Hook); };
        TWeakObjectPtr<AY01TestAttack> Flight;
        if (Case == TEXT("AutomaticFlight"))
        {
            ASC->SetCombatTarget(Old);
            TestTrue(TEXT("下一波前真实发射在途普攻"), AY01TestAttack::TryAutoAttack(ASC));
            for (TActorIterator<AY01TestAttack> It(F.World); It; ++It)
            { if (It->GetState().Caster == F.PC->GetPawn()) { Flight = *It; } }
            if (!TestTrue(TEXT("真实飞行物存在"), Flight.IsValid())) { return false; }
        }
        Tick(6);
        if (!TestEqual(TEXT("旧怪未死也生成末波"), F.Mode->GetWaveMonsters().Num(), 2)) { return false; }
        F.Mode->GetWaveMonsters()[1]->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); Tick(1);
        TestTrue(TEXT("末波单独完成"), F.Mode->GetFlowState()->GetWaveSnapshot().bWaveCompleted);
        TestTrue(TEXT("末波完成后旧怪仍是合法战斗目标"), ASC->IsLegalCombatTarget(Old));
        TestEqual(TEXT("换波和末波奖励均不失效玩家攻击会话"), ASC->GetCombatSession(), Session);
        if (Case == TEXT("AutomaticFlight"))
        {
            TestTrue(TEXT("跨波飞行物未被取消"), Flight.IsValid() && !Flight->IsActorBeingDestroyed()
                && Flight->GetState().Phase == EY01TestAttackPhase::Active);
            Tick(35);
            TestEqual(TEXT("旧飞行物跨波真实致死旧怪"), Y01Playground::Health(Old->GetCombatASC()), 0.f);
        }
        else { TestTrue(TEXT("末波已完成后GE仍可杀旧怪"), F.Hit(Old)); }
        TestEqual(TEXT("旧怪有效击杀恰好一次"), Events.Num(), 1);
        TestEqual(TEXT("旧怪击杀增加装备攻击"), F.Attack(), 41.f);
        TestEqual(TEXT("旧怪击杀增加玩家力量"), F.View().Stats.Strength, 1.f);
        auto* Delivery = F.P->GetDropDelivery();
        if (!TestEqual(TEXT("旧怪死亡产生真实交付"), Delivery->GetLedger().Records.Num(), 1)) { return false; }
        const auto Receipt = Delivery->GetLedger().Records[0];
        Tick(22);
        TestEqual(TEXT("主线旧延迟仍未生成时保持Battle"), F.Mode->GetFlowState()->GetWaveSnapshot().Phase, EY01MatchPhase::Battle);
        TestEqual(TEXT("交付到期实际入包"), Delivery->GetLedger().Records[0].State, EY01DeliveryState::Delivered);
        TestEqual(TEXT("原装备加掉落物"), F.View().Items.Num(), 2);
        FY01ItemInstance Dropped; TestTrue(TEXT("掉落精确ItemId存在"), F.P->GetLoadout()->FindItem(Receipt.ItemId, Dropped).IsOk());
        if (!Events.IsEmpty()) { const auto Event = Events[0]; Kills->OnEffectiveKill.Broadcast(Event); }
        Delivery->Schedule(); TestEqual(TEXT("重放旧怪事件不重复掉落"), F.View().Items.Num(), 2);
        TestEqual(TEXT("重放不重复成长"), F.Attack(), 41.f);
        return true;
    }
    if (Case.StartsWith(TEXT("Delivery")))
    {
        auto* D = F.P->GetDropDelivery();
        TestTrue(TEXT("R36已显式启用而不是测试替其启用"), D->IsEnabled());
        TestTrue(TEXT("服务器重复初始化幂等"), D->Initialize(Case == TEXT("DeliveryImmediate") ? 0 : 1).IsOk());
        if (Case == TEXT("DeliveryProfileDuration"))
        {
            auto* Profile = NewObject<UY01R36Profile>(); Profile->bEconomyEnabled = true;
            Profile->Character = F.Data.Ref(K::Character); Profile->OrderedWaves = F.Data.Waves();
            for (const auto& Pair : F.Data.Tables) { Profile->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
            for (double Seconds : {1., 0., -1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
            {
                Profile->DropFlightSeconds = Seconds;
                TestEqual(TEXT("Profile基类入口校验有限非负飞行秒数"), Profile->Validate().IsValid(), FMath::IsFinite(Seconds) && Seconds >= 0);
            }
            return true;
        }
        if (Case == TEXT("DeliveryDuration"))
        {
            TestFalse(TEXT("拒绝负飞行时间"), D->Initialize(-1).IsOk());
            TestFalse(TEXT("拒绝无穷飞行时间"), D->Initialize(std::numeric_limits<double>::infinity()).IsOk());
            TestFalse(TEXT("拒绝NaN飞行时间"), D->Initialize(std::numeric_limits<double>::quiet_NaN()).IsOk());
            return true;
        }
        TArray<AY01Monster*> Targets; for (AY01Monster* M : F.Mode->GetWaveMonsters()) { Targets.Add(M); }
        if (Case == TEXT("DeliveryTrainingClear"))
        { TestTrue(TEXT("实际进入训练区"), F.Mode->GetTraining()->Request(F.PC, true)); Targets = F.Mode->GetTraining()->Monsters(F.P); }
        if (!TestTrue(TEXT("交付夹具真实存活目标"), !Targets.IsEmpty())) { return false; }
        AY01PlayerState* Receiver = F.P;
        if (Case == TEXT("DeliveryLastHit"))
        {
            auto* PC = F.Player(); Receiver = PC->GetPlayerState<AY01PlayerState>(); FString Error;
            TestTrue(TEXT("B真实初始化"), Receiver->TryInitializeCharacter(FY01DataCatalog(F.Data.Tables), F.Data.Ref(K::Character), Error));
            F.Mode->OnPlayerInitialized(Receiver); TestTrue(TEXT("B启用交付"), Receiver->GetDropDelivery()->Initialize(1).IsOk());
            TestTrue(TEXT("A助攻"), F.Hit(Targets[0], F.P, 5)); D = Receiver->GetDropDelivery();
        }
        const int32 Expected = Case == TEXT("DeliveryPoolQuantity") ? 6 : Case == TEXT("DeliveryRetention") ? 65 : 1;
        int32 Initial = 0;
        if (Case == TEXT("DeliveryGrowthFault"))
        {
            FY01ItemGrant Item{FGuid::NewGuid(), F.Data.Ref(K::Equipment)};
            TestTrue(TEXT("真实装备故障成长来源"), F.P->GetLoadout()->AddItems(FY01DataCatalog(F.Data.Tables), {Item}).IsOk());
            TestTrue(TEXT("穿戴来源"), F.Send(Item.ItemId)); Initial = 1;
        }
        if (Case == TEXT("DeliverySnapshot"))
        { F.Data.Row<FY01MonsterRow>(K::Monster).ItemDropChance = 0;
          F.Data.Row<FY01RewardPackageRow>(K::RewardPackage, TEXT("DropOnly")).Items.Reset();
          F.Data.Row<FY01SourceEffectRow>(K::SourceEffect, TEXT("Ten")).Magnitude = 999; }
        auto Advance = [&]
        {
            const double Before = F.Mode->GetFlowState()->GetServerWorldTimeSeconds();
            TGuardValue<uint64> Frame(GFrameCounter, GFrameCounter);
            for (int32 I = 0; I < 22; ++I) { ++GFrameCounter; F.World->Tick(LEVELTICK_All, .05f); }
            TestTrue(TEXT("真实世界时间至少推进一秒，不把被钳制的大帧当ETA已到"),
                F.Mode->GetFlowState()->GetServerWorldTimeSeconds() - Before >= 1.);
        };
        auto* Kills = F.World->GetSubsystem<UY01KillRewards>(); TArray<FY01KillEvent> Events;
        const auto Hook = Kills->OnEffectiveKill.AddLambda([&](const auto& Event) { Events.Add(Event); });
        ON_SCOPE_EXIT { Kills->OnEffectiveKill.Remove(Hook); };
        const auto Origin = Targets[0]->GetActorLocation(); const auto Seed = FY01R36DeliveryTestAccess::Seed(Receiver->GetEconomy());
        if (Case == TEXT("DeliveryProbability"))
        {
            FRandomStream ExpectedStream(Seed); int32 Successes = 0;
            for (auto* Target : Targets)
            {
                Successes += ExpectedStream.GetFraction() < .5f;
                TestTrue(TEXT("不指定seed，真实概率击杀"), F.Hit(Target));
                TestEqual(TEXT("每事件一次概率决策"), D->GetLedger().Records.Num(), Successes);
                TestEqual(TEXT("成功归队或概率未中才提交票流"), FY01R36DeliveryTestAccess::Seed(Receiver->GetEconomy()), ExpectedStream.GetCurrentSeed());
            }
            Advance(); D->Schedule(); TestEqual(TEXT("所有抽中结果按ETA到货"), F.View().Items.Num(), Successes); return true;
        }
        if (Case == TEXT("DeliveryDirectZero")) { Targets[0]->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        else if (Case == TEXT("DeliveryDestroy")) { Targets[0]->Destroy(); }
        else { TestTrue(TEXT("合法伤害真实致死"), F.Hit(Targets[0], Receiver)); }
        if (Case == TEXT("DeliveryZero") || Case == TEXT("DeliveryDirectZero") || Case == TEXT("DeliveryDestroy"))
        { Advance(); D->Schedule(); TestEqual(TEXT("未判定掉落不产生记录"), D->GetLedger().Records.Num(), 0);
          TestEqual(TEXT("未判定掉落零入包"), F.View().Items.Num(), 0);
          TestEqual(TEXT("零概率或非击杀不消耗随机流"), FY01R36DeliveryTestAccess::Seed(Receiver->GetEconomy()), Seed); return true; }
        const auto Pending = D->GetLedger();
        if (!TestEqual(TEXT("合法致死必须生成每份唯一ETA交付"), Pending.Records.Num(), Expected)) { return false; }
        if (Case == TEXT("DeliveryGrowthFault")) { TestTrue(TEXT("实际成长Fault"), F.View().Growth.GrowthFault != EY01ItemError::None); }
        if (Case == TEXT("DeliveryImmediate"))
        {
            TestEqual(TEXT("零秒仍先归队"), Pending.Records[0].State, EY01DeliveryState::Pending);
            TestEqual(TEXT("零秒ETA等于开始"), Pending.Records[0].ETA, Pending.Records[0].StartTime);
            TestEqual(TEXT("广播栈内不发物"), F.View().Items.Num(), 0);
            D->Schedule(); TestEqual(TEXT("零秒下次合法调度到货"), F.View().Items.Num(), 1); return true;
        }
        if (Case == TEXT("DeliveryIdCollision"))
        {
            const auto R = Pending.Records[0];
            TestTrue(TEXT("模拟预定ID已被另一真实事务占用"), Receiver->GetLoadout()->AddItems(FY01DataCatalog(F.Data.Tables), {{R.ItemId, R.Definition}}).IsOk());
            Advance(); D->Schedule();
            TestEqual(TEXT("精确ID冲突不能冒认Delivered"), D->GetLedger().Records[0].State, EY01DeliveryState::Fault);
            TestEqual(TEXT("冲突有明确DuplicateItem"), D->GetLedger().Records[0].Fault, EY01ItemError::DuplicateItem);
            TestEqual(TEXT("冲突不复制或替换旧物"), F.View().Items.Num(), 1); return true;
        }
        TestEqual(TEXT("冻结死亡原点"), Pending.Records[0].Origin, Origin);
        TestEqual(TEXT("使用GameState同步世界时间"), Pending.Records[0].StartTime, F.Mode->GetFlowState()->GetServerWorldTimeSeconds());
        FY01CommittedView BeforeETA; Receiver->GetLoadout()->PublishCommittedView(); Receiver->GetLoadout()->ReadCommittedView(BeforeETA);
        D->Schedule(); TestEqual(TEXT("ETA前包无掉落物"), BeforeETA.Items.Num(), Initial);
        TestEqual(TEXT("ETA前权威仍Pending"), D->GetLedger().Records[0].State, EY01DeliveryState::Pending);
        const auto DrawnSeed = FY01R36DeliveryTestAccess::Seed(Receiver->GetEconomy());
        if (Case == TEXT("DeliveryDuplicate"))
        { if (!Events.IsEmpty()) { const auto Event = Events[0]; Kills->OnEffectiveKill.Broadcast(Event); Kills->OnEffectiveKill.Broadcast(Event); }
          TestEqual(TEXT("重复事件不再抽样"), FY01R36DeliveryTestAccess::Seed(Receiver->GetEconomy()), DrawnSeed);
          TestEqual(TEXT("重复消息不增交付"), D->GetLedger().Records.Num(), Expected); }
        if (Case == TEXT("DeliveryRegion")) { TestTrue(TEXT("待交付时切区"), F.Mode->GetTraining()->Request(F.PC, true)); }
        if (Case == TEXT("DeliveryTrainingClear")) { F.Mode->GetTraining()->StopAll(); }
        if (Case == TEXT("DeliveryDeath")) { Receiver->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        if (Case == TEXT("DeliveryLogout") || Case == TEXT("DeliveryEnded"))
        {
            if (Case == TEXT("DeliveryLogout")) { F.Mode->Logout(F.PC); } else { F.Mode->TryEndMatch(false); }
            Advance(); D->Schedule(); const auto Ended = D->GetLedger();
            TestEqual(TEXT("结束待交付明确Cancelled"), Ended.Records[0].State, EY01DeliveryState::Cancelled);
            TestTrue(TEXT("取消有明确原因"), Ended.Records[0].CancelReason != EY01DeliveryCancel::None);
            TestFalse(TEXT("原Join不得重启"), D->Initialize(1).IsOk());
            FY01ItemInstance Item; TestFalse(TEXT("局外不补发"), Receiver->GetLoadout()->FindItem(Pending.Records[0].ItemId, Item).IsOk());
            if (Case == TEXT("DeliveryLogout"))
            { auto* NewPC = F.Player(); auto* NewP = NewPC->GetPlayerState<AY01PlayerState>(); FString Error;
              TestTrue(TEXT("新连接角色初始化"), NewP->TryInitializeCharacter(FY01DataCatalog(F.Data.Tables), F.Data.Ref(K::Character), Error));
              F.Mode->OnPlayerInitialized(NewP); NewP->GetDropDelivery()->Schedule();
              TestEqual(TEXT("新Join不恢复旧队列"), NewP->GetDropDelivery()->GetLedger().Records.Num(), 0); }
            return true;
        }
        const bool Busy = Case == TEXT("DeliveryBusy") || Case == TEXT("DeliveryTechnical") || Case == TEXT("DeliveryEndedReentry");
        if (Busy) { TestTrue(TEXT("持有真实库存锁"), FY01R36DeliveryTestAccess::Lock(Receiver->GetLoadout())); }
        Advance();
        if (Busy)
        {
            D->Schedule(); TestEqual(TEXT("Busy不丢队列不假Delivered"), D->GetLedger().Records[0].State, EY01DeliveryState::Pending);
            TestTrue(TEXT("Busy记录延迟重试"), D->GetLedger().Records[0].RetryCount > 0);
            FY01R36DeliveryTestAccess::Unlock(Receiver->GetLoadout());
        }
        if (Case == TEXT("DeliveryTechnical")) { FY01R36DeliveryTestAccess::Exhaust(Receiver->GetLoadout()); }
        bool EndedInside = false;
        if (Case == TEXT("DeliveryEndedReentry"))
        { FY01R36DeliveryTestAccess::Clock(Receiver->GetIncome(), [&] { if (!EndedInside) { EndedInside = true; F.Mode->TryEndMatch(false); } return FPlatformTime::Seconds(); }); }
        D->Schedule(); FY01R36DeliveryTestAccess::Clock(Receiver->GetIncome(), {});
        if (Case == TEXT("DeliveryTechnical") || Case == TEXT("DeliveryEndedReentry"))
        {
            const auto R = D->GetLedger().Records[0];
            TestEqual(TEXT("技术失败或结束不假Delivered"), R.State, EndedInside ? EY01DeliveryState::Cancelled : EY01DeliveryState::Fault);
            TestTrue(TEXT("失败有明确错误"), EndedInside || R.Fault != EY01ItemError::None);
            FY01ItemInstance Item; TestFalse(TEXT("失败无部分入包"), Receiver->GetLoadout()->FindItem(R.ItemId, Item).IsOk());
            if (Case == TEXT("DeliveryEndedReentry")) { TestTrue(TEXT("真实交付中重入结束"), EndedInside); }
            return true;
        }
        const auto Record = Pending.Records[0];
        TestEqual(TEXT("默认飞行一秒"), Record.ETA - Record.StartTime, 1.);
        FY01CommittedView View; Receiver->GetLoadout()->PublishCommittedView(); Receiver->GetLoadout()->ReadCommittedView(View);
        TestEqual(TEXT("ETA后首次可执行调度入包所有份"), View.Items.Num(), Expected + Initial);
        TSet<FGuid> ItemIds, DeliveryIds;
        for (const auto& R : Pending.Records)
        {
            TestFalse(TEXT("每份ItemId独立"), ItemIds.Contains(R.ItemId)); ItemIds.Add(R.ItemId);
            TestFalse(TEXT("每份DeliveryId独立"), DeliveryIds.Contains(R.DeliveryId)); DeliveryIds.Add(R.DeliveryId);
            const auto* Item = View.Items.FindByPredicate([&](const auto& I) { return I.ItemId == R.ItemId; });
            if (TestNotNull(TEXT("精确预定ItemId已交付"), Item))
            { TestTrue(TEXT("已抽定定义不重抽"), Item->Definition.Type == R.Definition.Type && Item->Definition.RowName == R.Definition.RowName);
              for (const auto& G : Item->Growth) { TestEqual(TEXT("包中成长累计零"), G.Accumulated, 0.); TestEqual(TEXT("包中成长计次零"), G.AppliedCount, uint64(0)); }
              if (Case == TEXT("DeliverySnapshot")) { TestEqual(TEXT("全部静态依赖来自服务器快照"), Item->Effects[0].Magnitude, 10.); } }
        }
        TestEqual(TEXT("权威状态Delivered"), D->GetLedger().Records[0].State, EY01DeliveryState::Delivered);
        D->Schedule(); if (!Events.IsEmpty()) { const auto Event = Events[0]; Kills->OnEffectiveKill.Broadcast(Event); }
        TestEqual(TEXT("交付和重播不再耗随机流"), FY01R36DeliveryTestAccess::Seed(Receiver->GetEconomy()), DrawnSeed);
        Receiver->GetLoadout()->ReadCommittedView(View); TestEqual(TEXT("重复调度不重发"), View.Items.Num(), Expected + Initial);
        if (Case == TEXT("DeliveryLastHit")) { TestEqual(TEXT("助攻A隔离零物"), F.View().Items.Num(), 0); }
        TestEqual(TEXT("物品交付不叠加既有训练金币"), Receiver->GetGold(), Case == TEXT("DeliveryTrainingClear") ? 10 : 0);
        TestEqual(TEXT("物品交付不叠加既有训练经验"), Receiver->GetExperience(), Case == TEXT("DeliveryTrainingClear") ? 5 : 0);
        if (Case == TEXT("DeliveryRetention"))
        {
            TestEqual(TEXT("终态仅保留64条，待交付不受容量裁剪"), D->GetLedger().Records.Num(), 64);
            auto* Mark = FindFProperty<FStructProperty>(FY01ViewItem::StaticStruct(), TEXT("ArrivalDeliveryId"));
            if (!TestNotNull(TEXT("被裁剪回执仍需物品级持久到货身份供未打开格显示"), Mark)) { return false; }
            for (const auto& R : Pending.Records)
            {
                const auto* Item = View.Items.FindByPredicate([&](const auto& I) { return I.ItemId == R.ItemId; });
                if (TestNotNull(TEXT("65份无背包截断"), Item))
                { TestEqual(TEXT("每件持久到货标记对应精确交付"), *Mark->ContainerPtrToValuePtr<FGuid>(Item), R.DeliveryId); }
            }
        }
        if (Case == TEXT("DeliveryTrace"))
        {
            for (const auto* V : {&BeforeETA, &View})
            {
                const auto Saved = *V; TSharedPtr<FJsonObject> J;
                const FString Trace = Y01R36Trace::ViewRecord(*V); AddInfo(TEXT("Y01R36_TRACE ") + Trace);
                TestTrue(TEXT("交付观测使用现有JSON trace"), FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Trace), J));
                if (!J) { return false; }
                const TArray<TSharedPtr<FJsonValue>> *Records = nullptr, *Arrivals = nullptr;
                if (!TestTrue(TEXT("交付trace不漏权威ETA记录"), J->TryGetArrayField(TEXT("deliveries"), Records) && Records->Num() == 1)) { return false; }
                const auto R = (*Records)[0]->AsObject(); FString Id; double Time = -1, State = -1;
                R->TryGetStringField(TEXT("id"), Id); TestEqual(TEXT("trace精确DeliveryId"), Id, Record.DeliveryId.ToString());
                R->TryGetNumberField(TEXT("eta"), Time); TestEqual(TEXT("trace原ETA"), Time, Record.ETA);
                R->TryGetNumberField(TEXT("state"), State); TestEqual(TEXT("trace权威状态"), State, V == &BeforeETA ? 0. : 1.);
                TestTrue(TEXT("trace包含到货物品标记"), J->TryGetArrayField(TEXT("arrivals"), Arrivals) && Arrivals->Num() == (V == &BeforeETA ? 0 : 1));
                TestTrue(TEXT("只读观测不改提交"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Saved, V, 0));
            }
            return true;
        }
        if (Case == TEXT("DeliveryWireAtomic"))
        {
            auto V = View; V.Delivery.Records[0].State = EY01DeliveryState::Pending;
            V.Delivery.Records[0].DeliveredTime = 0; V.Delivery.Records[0].DelaySeconds = 0;
            FY01ViewSender Sender; FY01ViewWire Wire; FString Error;
            TestFalse(TEXT("Pending不能带已到货身份半状态"), Sender.Offer(V, Wire, Error));
            V.Delivery.Records[0].State = EY01DeliveryState::Fault; V.Delivery.Records[0].Fault = EY01ItemError::Range;
            Sender.Reset();
            TestFalse(TEXT("Fault不能带已到货身份半状态"), Sender.Offer(V, Wire, Error)); return true;
        }
        if (Case == TEXT("DeliveryWireInvalid"))
        {
            auto Reject = [&](const TCHAR* Name, TFunctionRef<void(FY01CommittedView&)> Mutate)
            { auto V = View; Mutate(V); FY01ViewSender Sender; FY01ViewWire Wire; FString Error; TestFalse(Name, Sender.Offer(V, Wire, Error)); };
            Reject(TEXT("交付不能跨Match"), [](auto& V) { V.Delivery.Records[0].MatchId = FGuid::NewGuid(); });
            Reject(TEXT("交付不能跨Join"), [](auto& V) { ++V.Delivery.Records[0].JoinSequence; });
            Reject(TEXT("交付身份必须有效"), [](auto& V) { V.Delivery.Records[0].DeliveryId.Invalidate(); });
            Reject(TEXT("合法致死身份必须有效"), [](auto& V) { V.Delivery.Records[0].KillEventId.Invalidate(); });
            Reject(TEXT("交付必须精确ItemId"), [](auto& V) { V.Delivery.Records[0].ItemId.Invalidate(); });
            Reject(TEXT("拒绝重复交付记录"), [](auto& V) { const auto R = V.Delivery.Records[0]; V.Delivery.Records.Add(R); });
            Reject(TEXT("拒绝NaN ETA"), [](auto& V) { V.Delivery.Records[0].ETA = std::numeric_limits<double>::quiet_NaN(); });
            Reject(TEXT("拒绝负延迟"), [](auto& V) { V.Delivery.Records[0].DelaySeconds = -1; });
            Reject(TEXT("Delivered不能早于ETA"), [](auto& V) { V.Delivery.Records[0].DeliveredTime = 0; });
            Reject(TEXT("拒绝非物品抽定结果"), [](auto& V) { V.Delivery.Records[0].Definition.Type = K::Monster; });
            Reject(TEXT("未启用不能有交付"), [](auto& V) { V.Delivery.bEnabled = false; });
            Reject(TEXT("Fault必须有错误"), [](auto& V) { V.Delivery.Records[0].State = EY01DeliveryState::Fault; });
            Reject(TEXT("Cancelled必须有原因"), [](auto& V) { V.Delivery.Records[0].State = EY01DeliveryState::Cancelled; });
            Reject(TEXT("抽样故障必须有事件"), [](auto& V) { V.Delivery.Fault = EY01ItemError::Precision; });
            return true;
        }
        if (Case == TEXT("DeliveryWire") || Case == TEXT("DeliveryRetention"))
        {
            FY01ViewSender Sender; FY01ViewReceiver Reader; FY01ViewWire Wire; FString Error;
            TestTrue(TEXT("包含交付的完整视图可编码"), Sender.Offer(View, Wire, Error));
            for (int32 I = 0; I < 200 && !Wire.Items.IsEmpty(); ++I)
            { FY01ViewAck Ack; if (!TestTrue(TEXT("交付窗口可接收"), Reader.Receive(Wire, Ack, Error))) { break; }
              TestTrue(TEXT("交付窗口可确认"), Sender.Acknowledge(Ack, Wire, Error)); }
            TestEqual(TEXT("完整复制交付与物品同提交"), Reader.Get().Delivery.Records.Num(), FMath::Min(Expected, 64));
            TestEqual(TEXT("完整传输不裁剪物品"), Reader.Get().Items.Num(), Expected);
            TestTrue(TEXT("交付账本一致"), FY01DeliveryLedger::StaticStruct()->CompareScriptStruct(&Reader.Get().Delivery, &View.Delivery, 0));
        }
        return true;
    }
    FY01ItemGrant A{FGuid::NewGuid(), F.Data.Ref(K::Equipment)}, B{FGuid::NewGuid(), F.Data.Ref(K::Equipment)};
    if (!TestTrue(TEXT("两件同定义独立入包"), F.P->GetLoadout()->AddItems(FY01DataCatalog(F.Data.Tables), {A, B}).IsOk())
        || !TestTrue(TEXT("真实穿戴"), F.Send(A.ItemId))) { return false; }
    TestEqual(TEXT("裸30加静态10"), F.Attack(), 40.f);
    TArray<AY01Monster*> Targets; for (AY01Monster* M : F.Mode->GetWaveMonsters()) { Targets.Add(M); }
    if (Case == TEXT("Training") || Case == TEXT("CrossRegionRejected"))
    {
        if (!TestTrue(TEXT("真实进入训练区"), F.Mode->GetTraining()->Request(F.PC, true))) { return false; }
        if (Case == TEXT("Training")) { Targets = F.Mode->GetTraining()->Monsters(F.P); }
    }
    if (!TestTrue(TEXT("自然生成足够怪"), Targets.Num() >= 5)) { return false; }
    auto* Kills = F.World->GetSubsystem<UY01KillRewards>();
    TArray<FY01KillEvent> Events;
    const auto EventHook = Kills->OnEffectiveKill.AddLambda([&](const FY01KillEvent& Event) { Events.Add(Event); });
    ON_SCOPE_EXIT { Kills->OnEffectiveKill.Remove(EventHook); };
    if (Case == TEXT("UIGrowth") || Case == TEXT("UIFault"))
    {
        TestTrue(TEXT("UI只消费真实击杀提交"), F.Hit(Targets[0]));
        F.World->SetGameInstance(nullptr);
        TStrongObjectPtr<UY01R36Panel> Panel(CreateWidget<UY01R36Panel>(F.World)); Panel->TakeWidget();
        TestTrue(TEXT("UI接收完整成长视图"), Panel->Present(F.View(), EY01MatchPhase::Battle, 1));
        auto* Summary = Cast<UTextBlock>(Panel->WidgetTree->FindWidget(TEXT("Summary")));
        if (!TestNotNull(TEXT("真实摘要控件"), Summary)) { return false; }
        if (Case == TEXT("UIFault")) { TestTrue(TEXT("UI明确显示成长故障"), Summary->GetText().ToString().Contains(TEXT("成长故障"))); }
        else
        {
            TestTrue(TEXT("UI显示真实力量"), Summary->GetText().ToString().Contains(TEXT("力量 1")));
            const FString Tip = Panel->GetCell(EY01R36Zone::Equipment, 0)->GetToolTipText().ToString();
            TestTrue(TEXT("装备提示显示永久实例累计"), Tip.Contains(TEXT("累计 1")));
            TestFalse(TEXT("不保留过时未触发文案"), Tip.Contains(TEXT("本场不触发")));
        }
        return true;
    }
    if (Case == TEXT("SourceStageCommit") || Case == TEXT("SourceStageRollback"))
    {
        auto* S = F.P->GetSourceEffects(); FY01PreparedSource Plan; FY01SourceTransaction Tx;
        TestTrue(TEXT("真实独立来源准备"), S->PrepareSource(FY01DataCatalog(F.Data.Tables), EY01RuntimeSourceKind::Skill,
            TEXT("PendingOnly"), {F.Data.Ref(K::SourceEffect, TEXT("Ten"))}, Plan).IsOk());
        TestTrue(TEXT("独立来源暂存成功"), S->StageSource(Plan, Tx).IsOk());
        TestTrue(TEXT("持有来源事务期间真实致死"), F.Hit(Targets[0])); TestEqual(TEXT("事务退栈前不广播"), Events.Num(), 0);
        if (Case == TEXT("SourceStageCommit")) { TestFalse(TEXT("致死使未提交事务中断"), S->CommitSource(Tx).IsOk()); }
        else { TestTrue(TEXT("精确回滚独立来源事务"), S->RollbackSource(Tx).IsOk()); }
        TestEqual(TEXT("退来源调用栈后消费一次"), Events.Num(), 1); TestEqual(TEXT("仅原有效装备贡献成长"), F.Attack(), 41.f);
        TestEqual(TEXT("暂存虚拟技能不发玩家成长"), F.View().Stats.Strength, 1.f); return true;
    }
    if (Case == TEXT("DuringGrowthEnded") || Case == TEXT("DuringDeathEnded") || Case == TEXT("DuringDeathLogout"))
    {
        bool Fired = false; auto End = [&]
        { if (Fired) { return; } Fired = true;
          if (Case == TEXT("DuringDeathLogout")) { F.Mode->Logout(F.PC); }
          else { TestTrue(TEXT("回调中真实结束比赛"), F.Mode->TryEndMatch(false)); } };
        FDelegateHandle H;
        if (Case == TEXT("DuringGrowthEnded")) { H = F.P->GetCombatASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
            [&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle) { End(); }); }
        else { H = Targets[0]->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda(
            [&](const FOnAttributeChangeData&) { End(); }); }
        TestTrue(TEXT("生命周期回调之前已真实致死"), F.Hit(Targets[0]));
        if (Case == TEXT("DuringGrowthEnded")) { F.P->GetCombatASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(H); }
        else { Targets[0]->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).Remove(H); }
        TestTrue(TEXT("实际执行清理回调"), Fired); TestEqual(TEXT("已授权合法事件不被成长停止吞掉"), Events.Num(), 1);
        TestEqual(TEXT("停止批次无部分力量"), F.P->GetCombatASC()->GetUnitStats().Strength, 0.f);
        TestFalse(TEXT("清理后成长关闭"), F.P->GetGrowth()->IsEnabled()); return true;
    }
    if (Case == TEXT("WireLedgerValidation"))
    {
        TestTrue(TEXT("校验来自真实成长账本"), F.Hit(Targets[0])); const auto Good = F.View();
        auto Reject = [&](const TCHAR* Name, TFunctionRef<void(FY01CommittedView&)> Mutate)
        { auto V = Good; Mutate(V); FY01ViewSender Sender; FY01ViewWire Wire; FString Error; TestFalse(Name, Sender.Offer(V, Wire, Error)); };
        Reject(TEXT("负玩家累计"), [](auto& V) { V.Growth.PlayerRules[0].Rule.Accumulated = -1; });
        Reject(TEXT("非有限玩家规则"), [](auto& V) { V.Growth.PlayerRules[0].Rule.Delta = std::numeric_limits<double>::quiet_NaN(); });
        Reject(TEXT("重复来源规则账本"), [](auto& V) { const auto Entry = V.Growth.PlayerRules[0]; V.Growth.PlayerRules.Add(Entry); });
        Reject(TEXT("玩家规则目标错误"), [](auto& V) { V.Growth.PlayerRules[0].Rule.Target = EY01GrowthTarget::EquipmentInstance; });
        Reject(TEXT("跨会话来源身份"), [](auto& V) { V.Growth.PlayerRules[0].SourceId = TEXT("foreign"); });
        Reject(TEXT("故障必须有事件身份"), [](auto& V) { V.Growth.GrowthFault = EY01ItemError::Precision; });
        Reject(TEXT("成功计次必须有事件身份"), [](auto& V) { V.Growth.LastKillEventId.Invalidate(); });
        Reject(TEXT("关闭账本不伪装已成长"), [](auto& V) { V.Growth.bEnabled = false; });
        Reject(TEXT("实例非有限累计"), [](auto& V) { V.Items[0].Growth[0].Accumulated = std::numeric_limits<double>::infinity(); });
        return true;
    }
    if (Case == TEXT("SourcesPassive") || Case == TEXT("SourcesActive") || Case == TEXT("SourcesDormant"))
    {
        FY01ItemGrant Book{FGuid::NewGuid(), F.Data.Ref(K::Skill)};
        TestTrue(TEXT("主动被动书真实入包"), F.P->GetLoadout()->AddItems(FY01DataCatalog(F.Data.Tables), {Book}).IsOk());
        TestTrue(TEXT("未学未买未选时真实致死"), F.Hit(Targets[0]));
        TestEqual(TEXT("包内书和候选不生效"), F.P->GetCombatASC()->GetUnitStats().Strength, 1.f);
        if (Case == TEXT("SourcesDormant")) { return true; }
        auto R = F.Request(Book.ItemId, O::PrepareLearn); R.ExpectedSlotItemId = {}; R.ExpectedSlotEntityRevision = 0;
        TestTrue(TEXT("实际学习技能"), F.P->GetLoadout()->SubmitRequest(F.PC, R).IsOk());
        R = F.Request({}, O::Purchase); R.Slot = INDEX_NONE; R.ExpectedSlotItemId = {}; R.ExpectedSlotEntityRevision = 0;
        R.Offer = F.Data.Ref(K::ShopOffer, TEXT("Direct"));
        TestTrue(TEXT("实际购买仅成长商品"), F.P->GetLoadout()->SubmitRequest(F.PC, R).IsOk());
        R = F.Request({}, O::ChooseTalent); R.Slot = INDEX_NONE; R.ExpectedSlotItemId = {}; R.ExpectedSlotEntityRevision = 0;
        R.TalentChoiceId = F.View().TalentChoice.ChoiceId; R.Talent = F.Data.Ref(K::Talent);
        TestTrue(TEXT("实际选择仅成长天赋"), F.P->GetLoadout()->SubmitRequest(F.PC, R).IsOk());
        TestTrue(TEXT("四类来源有效后真实击杀"), F.Hit(Targets[1]));
        auto V = F.View(); TestEqual(TEXT("四类来源各一次玩家成长"), V.Stats.Strength, 5.f);
        TestEqual(TEXT("玩家账本四个独立来源"), V.Growth.PlayerRules.Num(), 4);
        TestEqual(TEXT("只有装备增加实例攻击"), F.Attack(), 42.f);
        TestTrue(TEXT("卸装"), F.Send(A.ItemId, O::Unequip));
        V = F.View(); R = F.Request({}, O::RemoveSkill); R.ExpectedSlotItemId = {}; R.ExpectedSlotEntityRevision = 0;
        R.ExpectedLearnedId = V.Skills[0].LearnedId; R.ExpectedLearnedRevision = V.Skills[0].EntityRevision;
        TestTrue(TEXT("移除技能"), F.P->GetLoadout()->SubmitRequest(F.PC, R).IsOk());
        TestTrue(TEXT("移除两来源后真实击杀"), F.Hit(Targets[2]));
        TestEqual(TEXT("仅商店天赋继续且不追扣"), F.View().Stats.Strength, 7.f); return true;
    }
    if (Case.StartsWith(TEXT("Fault")))
    {
        auto* ASC = F.P->GetCombatASC(); const auto Handles = ASC->GetActiveEffects(FGameplayEffectQuery());
        const auto Before = F.View(); int32 Calls = 0;
        if (Case == TEXT("FaultGE")) { ASC->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda(
            [&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { return ++Calls != 2; })); }
        TestTrue(TEXT("成长故障不阻止真实致死"), F.Hit(Targets[0])); ASC->GameplayEffectApplicationQueries.Reset();
        auto V = F.View(); TestEqual(TEXT("故障仍广播独立合法事件"), Events.Num(), 1);
        TestTrue(TEXT("故障账本有明确错误"), V.Growth.GrowthFault != EY01ItemError::None && V.Growth.FaultKillEventId.IsValid());
        TestEqual(TEXT("实例攻击无部分提交"), F.Attack(), 40.f);
        TestEqual(TEXT("力量无部分提交"), V.Stats.Strength, Before.Stats.Strength);
        TestEqual(TEXT("库存版本回滚"), V.Revision, Before.Revision);
        TestTrue(TEXT("精确旧GE句柄恢复"), ASC->GetActiveEffects(FGameplayEffectQuery()) == Handles);
        for (auto H : Handles) { const auto* GE = ASC->GetActiveGameplayEffect(H); TestTrue(TEXT("旧GE仍有效且未抑制"), GE && !GE->bIsInhibited); }
        TestEqual(TEXT("失败无成功计次"), V.Growth.AppliedKills, uint64(0));
        if (!Events.IsEmpty()) { FY01R36GrowthTestAccess::Replay(F.P->GetGrowth(), Events[0]); }
        TestEqual(TEXT("失败事件也不重试"), F.View().Revision, Before.Revision); return true;
    }
    if (Case == TEXT("RegisterIdempotency") || Case == TEXT("EventIdempotency"))
    {
        const auto Id = Kills->RegisterMonster(Targets[0]); TestTrue(TEXT("已登记有效身份"), Id.IsValid());
        TestEqual(TEXT("重复登记同对象返回同ID"), Kills->RegisterMonster(Targets[0]), Id);
        TestTrue(TEXT("真实击杀"), F.Hit(Targets[0]));
        TestFalse(TEXT("重复真实伤害不能再杀尸体"), F.Hit(Targets[0]));
        if (Events.Num() == 1)
        {
            const auto Event = Events[0]; FY01R36GrowthTestAccess::Replay(F.P->GetGrowth(), Event); FY01R36GrowthTestAccess::Replay(F.P->GetGrowth(), Event);
            TestEqual(TEXT("事件冻结原Spawn身份"), Event.SpawnId, Id);
        }
        F.Mode->OnPlayerInitialized(F.P); TestTrue(TEXT("重复显式初始化"), F.P->GetGrowth()->Initialize());
        TestEqual(TEXT("事件和重复初始化不重发成长"), F.Attack(), 41.f); TestEqual(TEXT("独立消费者只有一次"), Events.Num(), 1); return true;
    }
    if (Case == TEXT("Ended") || Case == TEXT("Logout"))
    {
        TestTrue(TEXT("清理前真实击杀"), F.Hit(Targets[0]));
        if (Case == TEXT("Ended")) { TestTrue(TEXT("真实Ended"), F.Mode->TryEndMatch(false)); }
        else { F.Mode->Logout(F.PC); }
        F.P->GetGrowth()->EndSession(); F.P->GetGrowth()->EndSession();
        TestFalse(TEXT("终止不可重启"), F.P->GetGrowth()->Initialize());
        F.Hit(Targets[1]); TestEqual(TEXT("停止后不再合法事件"), Events.Num(), 1);
        TestEqual(TEXT("局内已得力量不追扣"), F.P->GetCombatASC()->GetUnitStats().Strength, 1.f);
        TestTrue(TEXT("完整账本标记结束"), F.View().Growth.bEnded); return true;
    }
    if (Case == TEXT("AutoAttackSnapshot"))
    {
        Targets[0]->SetActorLocation(F.PC->GetPawn()->GetActorLocation() + FVector(50, 0, 0));
        float Snapshot = 0; const bool Started = F.P->GetCombatASC()->TryAutoAttack(Targets[0], [&](float Damage)
        { Snapshot = Damage; TestTrue(TEXT("普攻互斥中真实伤害执行"), F.Hit(Targets[0], nullptr, Damage));
          TestEqual(TEXT("发动栈内成长未改原攻击"), F.Attack(), 40.f); return true; });
        TestTrue(TEXT("真实非GA普攻发动"), Started); TestEqual(TEXT("本击发动快照40"), Snapshot, 40.f);
        TestEqual(TEXT("退攻击锁后成长41"), F.Attack(), 41.f); TestEqual(TEXT("只通知一次"), Events.Num(), 1); return true;
    }
    if (Case == TEXT("GrowthReentry") || Case == TEXT("NestedDeathOrder") || Case == TEXT("ConsumerReentry"))
    {
        bool Fired = false; FDelegateHandle Hook;
        auto Nested = [&] { if (!Fired) { Fired = true; TestTrue(TEXT("重入只真实执行一次内层伤害"), F.Hit(Targets[1])); } };
        if (Case == TEXT("GrowthReentry")) { Hook = F.P->GetCombatASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
            [&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle) { Nested(); }); }
        else if (Case == TEXT("NestedDeathOrder")) { Hook = Targets[0]->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda(
            [&](const FOnAttributeChangeData&) { Nested(); }); }
        else { Hook = Kills->OnEffectiveKill.AddLambda([&](const FY01KillEvent&) { Nested(); }); }
        const auto FirstId = Kills->RegisterMonster(Targets[0]); const auto SecondId = Kills->RegisterMonster(Targets[1]);
        TestTrue(TEXT("外层真实致死"), F.Hit(Targets[0]));
        if (Case == TEXT("GrowthReentry")) { F.P->GetCombatASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(Hook); }
        else if (Case == TEXT("NestedDeathOrder")) { Targets[0]->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).Remove(Hook); }
        else { Kills->OnEffectiveKill.Remove(Hook); }
        TestTrue(TEXT("内层实际执行"), Fired); TestEqual(TEXT("两个事件都独立广播"), Events.Num(), 2);
        if (Events.Num() == 2) { TestEqual(TEXT("先致死先消费"), Events[0].SpawnId, FirstId); TestEqual(TEXT("后致死不越队"), Events[1].SpawnId, SecondId);
            TestTrue(TEXT("全局单调序列"), Events[0].Sequence < Events[1].Sequence); }
        TestEqual(TEXT("两次实例原子成长"), F.Attack(), 42.f); TestEqual(TEXT("两次力量原子成长"), F.View().Stats.Strength, 2.f); return true;
    }
    if (Case == TEXT("SourceOnlyBusy"))
    {
        const auto Hook = Targets[0]->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda(
            [&](const FOnAttributeChangeData&)
            { TestEqual(TEXT("来源独立入口不得越过待消费致死"), F.P->GetSourceEffects()->RemoveSource(EY01RuntimeSourceKind::Equipment,
                A.ItemId.ToString(EGuidFormats::DigitsWithHyphens)).Code, EY01RuntimeError::Busy); });
        TestTrue(TEXT("真实致死"), F.Hit(Targets[0]));
        Targets[0]->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).Remove(Hook);
        TestEqual(TEXT("先消费再允许来源修改"), F.Attack(), 41.f); return true;
    }
    if (Case == TEXT("WireLedger") || Case == TEXT("WireInvalidLedger"))
    {
        TestTrue(TEXT("复制前真实成长"), F.Hit(Targets[0])); auto V = F.View();
        FY01ViewSender Sender; FY01ViewReceiver Receiver; FY01ViewWire Wire; FString Error;
        if (Case == TEXT("WireInvalidLedger")) { V.Growth.Strength = std::numeric_limits<double>::quiet_NaN();
            TestFalse(TEXT("非有限成长账本拒绝发送"), Sender.Offer(V, Wire, Error)); return true; }
        TestTrue(TEXT("成长完整视图编码"), Sender.Offer(V, Wire, Error));
        for (int32 I = 0; I < 100 && !Wire.Items.IsEmpty(); ++I)
        { FY01ViewAck Ack; if (!TestTrue(TEXT("完整窗口接收"), Receiver.Receive(Wire, Ack, Error))) { break; }
          TestTrue(TEXT("确认完整窗口"), Sender.Acknowledge(Ack, Wire, Error)); }
        TestTrue(TEXT("真实账本完整复制"), FY01EffectiveKillGrowthLedger::StaticStruct()->CompareScriptStruct(&Receiver.Get().Growth, &V.Growth, 0));
        TestEqual(TEXT("力量与账本同提交"), Receiver.Get().Stats.Strength, 1.f); return true;
    }
    if (Case == TEXT("EffectIdentityCollision"))
    { TestTrue(TEXT("含内部前缀同名静态效果的合法装备真实击杀"), F.Hit(Targets[0]));
      TestEqual(TEXT("内部成长身份不得与合法静态EffectId碰撞"), F.Attack(), 41.f); return true; }
    if (Case == TEXT("DuplicateInitialization")) { F.Mode->OnPlayerInitialized(F.P); F.Mode->OnPlayerInitialized(F.P); }
    if (Case == TEXT("DirectZeroRejected")) { Targets[0]->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    else if (Case == TEXT("DestroyRejected")) { Targets[0]->Destroy(); }
    else if (Case == TEXT("UnregisteredRejected"))
    {
        auto* Monster = F.World->SpawnActor<AY01Monster>(); FString Error;
        TestTrue(TEXT("初始化不等于合法登记"), Monster->TryInitializeMonster(FY01DataCatalog(F.Data.Tables), F.Data.Ref(K::Monster), Error));
        F.P->GetCombatASC()->TargetPermission = {};
        TestTrue(TEXT("未登记目标仍实际受到致死伤害"), F.Hit(Monster));
    }
    else if (Case == TEXT("CrossRegionRejected")) { TestFalse(TEXT("越区真实GE不扣血"), F.Hit(Targets[0])); }
    else
    {
        AY01PlayerState* Killer = F.P;
        if (Case == TEXT("LastHit"))
        {
            auto* PC = F.Player(); Killer = PC->GetPlayerState<AY01PlayerState>(); FString Error;
            TestTrue(TEXT("B真实初始化"), Killer->TryInitializeCharacter(FY01DataCatalog(F.Data.Tables), F.Data.Ref(K::Character), Error));
            F.Mode->OnPlayerInitialized(Killer);
            FY01ItemGrant C{FGuid::NewGuid(), F.Data.Ref(K::Equipment)};
            TestTrue(TEXT("B独立装备入包"), Killer->GetLoadout()->AddItems(FY01DataCatalog(F.Data.Tables), {C}).IsOk());
            FY01CommittedView BV; Killer->GetLoadout()->ReadCommittedView(BV);
            FY01ItemRequest BR; BR.MatchId = BV.MatchId; BR.JoinSequence = BV.JoinSequence; BR.Seq = BV.ExpectedSeq;
            BR.ItemId = C.ItemId; BR.ExpectedEntityRevision = 1; BR.Slot = 0; BR.ConfigRevision = BV.ConfigRevision;
            TestTrue(TEXT("B真实装备独立来源"), Killer->GetLoadout()->SubmitRequest(PC, BR).IsOk());
            TestTrue(TEXT("A仅助攻真实扣血"), F.Hit(Targets[0], F.P, 5));
            TestTrue(TEXT("B最后致死"), F.Hit(Targets[0], Killer));
            TestEqual(TEXT("A助攻不增长"), F.Attack(), 40.f);
            TestEqual(TEXT("B最后一击仅B装备成长"), Killer->GetCombatASC()->GetUnitStats().AttackDamage, 41.f);
            TestEqual(TEXT("B最后一击仅B力量成长"), Killer->GetCombatASC()->GetUnitStats().Strength, 1.f); return true;
        }
        if (Case == TEXT("SourceStagingFrozen"))
        {
            bool Fired = false;
            const auto H = F.P->GetCombatASC()->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
                [&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle)
                { if (!Fired) { Fired = true; TestTrue(TEXT("暂存新装备GE期间真实致死"), F.Hit(Targets[0])); } });
            TestFalse(TEXT("致死冻结后尚未提交换装必须中断"), F.Send(B.ItemId));
            F.P->GetCombatASC()->OnGameplayEffectAppliedDelegateToSelf.Remove(H);
            TestTrue(TEXT("真实暂存回调已执行"), Fired);
            auto V = F.View(); TestEqual(TEXT("暂存来源不算生效仍是A"), V.EquipmentSlots[0], A.ItemId);
            TestEqual(TEXT("旧已提交来源获得本次成长"), F.Attack(), 41.f);
            for (const auto& I : V.Items) for (const auto& G : I.Growth)
            { if (G.Target == EY01GrowthTarget::EquipmentInstance) { TestEqual(TEXT("仅旧实例累计"), G.Accumulated, I.ItemId == A.ItemId ? 1. : 0.); } }
            return true;
        }
        const uint64 Session = Targets[0]->GetCombatASC()->GetCombatSession();
        float FrozenAttack = 0;
        auto Hook = Targets[0]->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).AddLambda(
            [&](const FOnAttributeChangeData&)
            {
                FrozenAttack = F.Attack();
                if (Case == TEXT("KillBeforeUnequip"))
                { TestFalse(TEXT("已提交致死未消费前拒绝卸装"), F.Send(A.ItemId, O::Unequip)); }
            });
        TestTrue(TEXT("真实伤害执行致死一"), F.Hit(Targets[0]));
        Targets[0]->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).Remove(Hook);
        TestTrue(TEXT("死亡使原CombatSession失效"), Targets[0]->GetCombatASC()->GetCombatSession() > Session);
        TestEqual(TEXT("致死回调仍是本击旧攻击快照"), FrozenAttack, 40.f);
        TestFalse(TEXT("尸体再次GE无伤害"), F.Hit(Targets[0]));
        TestTrue(TEXT("真实伤害执行致死二"), F.Hit(Targets[1])); TestTrue(TEXT("真实伤害执行致死三"), F.Hit(Targets[2]));
    }
    const bool Rejected = Case.EndsWith(TEXT("Rejected")); const double Expected = Rejected ? 0 : Delta * 3;
    TestEqual(TEXT("有效致死永久增加实例攻击"), F.Attack(), float(40 + Expected));
    TestEqual(TEXT("力量写入真实UnitCombatStats"), F.P->GetCombatASC()->GetUnitStats().Strength, float(Expected));
    TestEqual(TEXT("攻击base没有冒充装备成长"), F.P->GetCombatASC()->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute()), 30.f);
    auto V = F.View(); TestEqual(TEXT("完整提交视图力量"), V.Stats.Strength, float(Expected));
    for (const auto& I : V.Items)
    {
        for (const auto& G : I.Growth)
        {
            if (G.Target == EY01GrowthTarget::EquipmentInstance)
            {
                TestEqual(TEXT("成长按ItemId不按同定义串实例"), G.Accumulated, I.ItemId == A.ItemId ? Expected : 0.);
                TestEqual(TEXT("零与小数均逐次计数"), G.AppliedCount, uint64(!Rejected && I.ItemId == A.ItemId ? 3 : 0));
            }
        }
    }
    if (Case == TEXT("PermanentItem"))
    {
        TestTrue(TEXT("实际卸下"), F.Send(A.ItemId, O::Unequip)); TestEqual(TEXT("卸下恢复裸30"), F.Attack(), 30.f);
        TestTrue(TEXT("卸下后真实杀四"), F.Hit(Targets[3])); TestTrue(TEXT("卸下后真实杀五"), F.Hit(Targets[4]));
        TestEqual(TEXT("卸下来源不增长且不追扣力量"), F.P->GetCombatASC()->GetUnitStats().Strength, 3.f);
        TestTrue(TEXT("同实例重穿"), F.Send(A.ItemId)); TestEqual(TEXT("重穿恢复永久13贡献"), F.Attack(), 43.f);
        TestEqual(TEXT("重穿不重复发玩家成长"), F.P->GetCombatASC()->GetUnitStats().Strength, 3.f);
    }
    TestEqual(TEXT("静态规则未修改"), F.Data.Row<FY01GrowthRuleRow>(K::GrowthRule, TEXT("ItemAttack")).Delta, Delta);
    return true;
}
#endif
