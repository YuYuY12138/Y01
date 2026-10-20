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
#include "Player/Y01AttributeResolver.h"
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
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"
#include "Misc/Crc.h"

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
    FGrowthWorld(double Delta = 1, const FString& Scenario = TEXT(""), bool bKeepFrameCounter = false)
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
        if (Scenario == TEXT("SourceStagingFrozen"))
        {
            FY01SourceEffectRow Twenty; Twenty.Magnitude = 20; Data.Add(K::SourceEffect, Twenty, TEXT("Twenty"));
            Data.Row<FY01EquipmentRow>(K::Equipment, TEXT("Blue")).EffectIds = {Data.Ref(K::SourceEffect, TEXT("Twenty"))};
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
        if (Scenario == TEXT("TalentBasicAttack"))
        {
            // 对齐正式T10_Ranger：攻击效果+普攻触发玩家敏捷0.04，经真实R34自动普攻循环计数。
            Profile->bR34AutoAttackEnabled = true; C.AttackInterval = .5f; C.Agility = 10; M.Health = 1000000;
            FY01GrowthRuleRow Agi; Agi.Trigger = EY01GrowthTrigger::BasicAttack; Agi.Target = EY01GrowthTarget::Player;
            Agi.Attribute = EY01GrowthAttribute::Agility; Agi.Delta = .04; Data.Add(K::GrowthRule, Agi, TEXT("AgiAttack"));
            FY01TalentChoiceRow Choice;
            for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Two")), FName(TEXT("Three"))})
            {
                FY01TalentRow Talent; Talent.DisplayName = FText::FromString(TEXT("游侠"));
                Talent.EffectIds = {Data.Ref(K::SourceEffect, TEXT("Ten"))}; Talent.GrowthRuleIds = {Data.Ref(K::GrowthRule, TEXT("AgiAttack"))};
                Data.Add(K::Talent, Talent, Name); Choice.Candidates.Add(Data.Ref(K::Talent, Name));
            }
            Data.Add(K::TalentChoice, Choice); Profile->TalentChoice = Data.Ref(K::TalentChoice);
        }
        if (Scenario.StartsWith(TEXT("AttributeCondition")))
        {
            C.Strength = 10; C.AttackDamage = 0; C.Armor = 5;
            C.HealthRegen = Scenario.EndsWith(TEXT("NonAttackRows")) ? 2 : 0; C.AttackInterval = 1;
            FY01AttributeMappingRow Mapping; Mapping.SourceAttribute = EY01Attribute::Strength;
            Mapping.TargetAttribute = EY01Attribute::AttackDamage; Mapping.Coefficient = 3;
            Data.Add(K::AttributeMapping, Mapping, TEXT("StrengthAttack"));
            Profile->BaseAttributeMappings = {Data.Ref(K::AttributeMapping, TEXT("StrengthAttack"))};
            FY01SourceEffectRow Effect; Effect.Kind = EY01SourceEffectKind::AttributePercent;
            Effect.TargetAttribute = EY01Attribute::AttackDamage; Effect.Amount = 100;
            Effect.Condition = EY01EffectCondition::HealthBelow; Effect.ConditionThreshold = 30;
            Data.Add(K::SourceEffect, Effect, TEXT("LowAttack"));
            Effect.Condition = EY01EffectCondition::HealthAbove;
            Data.Add(K::SourceEffect, Effect, TEXT("AboveAttack"));
            Effect.ConditionThreshold = 40; Data.Add(K::SourceEffect, Effect, TEXT("Above40"));
            Effect.Condition = EY01EffectCondition::None; Effect.ConditionThreshold = 0; Effect.Amount = 50;
            Data.Add(K::SourceEffect, Effect, TEXT("Attack50"));
            Effect = {}; Effect.Kind = EY01SourceEffectKind::AttributeFlat;
            Effect.TargetAttribute = EY01Attribute::MaxHealth; Effect.Amount = 300;
            Data.Add(K::SourceEffect, Effect, TEXT("Max300"));
            Effect.Condition = EY01EffectCondition::HealthBelow; Effect.ConditionThreshold = 30;
            Effect.TargetAttribute = EY01Attribute::Armor; Effect.Amount = 7;
            Data.Add(K::SourceEffect, Effect, TEXT("LowArmor"));
            Effect.TargetAttribute = EY01Attribute::HealthRegen; Effect.Amount = 3;
            Data.Add(K::SourceEffect, Effect, TEXT("LowRegen"));
            Effect.Kind = EY01SourceEffectKind::AttributePercent; Effect.TargetAttribute = EY01Attribute::AttackRate; Effect.Amount = 100;
            Data.Add(K::SourceEffect, Effect, TEXT("LowRate"));
            Effect.TargetAttribute = EY01Attribute::GoldGain; Effect.Amount = 50;
            Data.Add(K::SourceEffect, Effect, TEXT("LowGoldGain"));
            for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Blue"))})
            {
                auto& Item = Data.Row<FY01EquipmentRow>(K::Equipment, Name);
                Item.EffectIds = {Data.Ref(K::SourceEffect, TEXT("LowAttack"))}; Item.GrowthRuleIds.Reset();
            }
        }
        else if (Scenario == TEXT("GrowthPercentPrecision"))
        {
            C.Strength = 10; C.AttackDamage = 0;
            FY01AttributeMappingRow Mapping; Mapping.SourceAttribute = EY01Attribute::Strength;
            Mapping.TargetAttribute = EY01Attribute::AttackDamage; Mapping.Coefficient = 3;
            Data.Add(K::AttributeMapping, Mapping, TEXT("StrengthAttack"));
            Profile->BaseAttributeMappings = {Data.Ref(K::AttributeMapping, TEXT("StrengthAttack"))};
            FY01SourceEffectRow Effect; Effect.Kind = EY01SourceEffectKind::AttributePercent;
            Effect.TargetAttribute = EY01Attribute::Strength; Effect.Amount = 100;
            Data.Add(K::SourceEffect, Effect, TEXT("Strength100"));
            Data.Row<FY01GrowthRuleRow>(K::GrowthRule, TEXT("PlayerStrength")).Delta = 7.e-7;
            for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Blue"))})
            {
                auto& Item = Data.Row<FY01EquipmentRow>(K::Equipment, Name);
                Item.EffectIds = {Data.Ref(K::SourceEffect, TEXT("Strength100"))};
                Item.GrowthRuleIds = {Data.Ref(K::GrowthRule, TEXT("PlayerStrength"))};
            }
        }
        else if (Scenario.StartsWith(TEXT("Attribute")))
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
        const uint64 InitialFrameCounter = GFrameCounter;
        ON_SCOPE_EXIT { if (!bKeepFrameCounter) { GFrameCounter = InitialFrameCounter; } };
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
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R37ViewBreakdownsTest, "Y01.Framework.R37.ViewBreakdowns",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R37ViewBreakdownsTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : {TEXT("FullAndDelta"), TEXT("BreakdownOnly"), TEXT("ExternalStats"), TEXT("Clear"), TEXT("Busy"), TEXT("LegacyEmpty"),
        TEXT("Condition101"), TEXT("UnknownCondition"), TEXT("NaN"), TEXT("Negative"), TEXT("DuplicateAttributes"), TEXT("MissingAttribute"),
        TEXT("UnknownAttribute"), TEXT("UnknownKind"), TEXT("NoneThreshold"), TEXT("InactiveContribution"), TEXT("NoneInactive"),
        TEXT("ConditionalTarget"), TEXT("BaseCondition"), TEXT("GrowthCondition"), TEXT("MappingSource"), TEXT("MappingTarget"),
        TEXT("NonMappingSource"), TEXT("IncomePercent"), TEXT("GoldGainSubtotal"), TEXT("GoldGainFinal"), TEXT("GoldGainBase"),
        TEXT("FinalCoherence"), TEXT("SubtotalCoherence"), TEXT("PercentCoherence"), TEXT("EmptySourceKey"),
        TEXT("DeltaUnknownBit"), TEXT("DeltaNoBitWithArray"), TEXT("DeltaCondition101"), TEXT("OldFullVersion"), TEXT("OldDeltaVersion")})
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01R37ViewBreakdownsTest::RunTest(const FString& Case)
{
    using A = EY01Attribute;
    using C = EY01ContributionKind;
    const FString Command = FCommandLine::Get(); FCommandLine::Set(TEXT("")); ON_SCOPE_EXIT { FCommandLine::Set(*Command); };
    FGrowthWorld F(1, TEXT("AttributeConditionView"));
    if (!TestTrue(TEXT("真实条件配置初始化"), F.Error.IsEmpty())) { AddError(F.Error); return false; }
    auto* L = F.P->GetLoadout(); auto* S = F.P->GetSourceEffects(); auto* ASC = F.P->GetCombatASC();
    auto& Equipment = F.Data.Row<FY01EquipmentRow>(K::Equipment);
    Equipment.EffectIds.Add(F.Data.Ref(K::SourceEffect, TEXT("LowGoldGain")));
    Equipment.GrowthRuleIds = {F.Data.Ref(K::GrowthRule, TEXT("ItemAttack")), F.Data.Ref(K::GrowthRule, TEXT("PlayerStrength"))};
    F.Data.Row<FY01SourceEffectRow>(K::SourceEffect, TEXT("LowGoldGain")).Amount = 20;
    ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 90);
    FY01ItemGrant Item{FGuid::NewGuid(), F.Data.Ref(K::Equipment)};
    if (!TestTrue(TEXT("真实装备入包"), L->AddItems(FY01DataCatalog(F.Data.Tables), {Item}).IsOk())
        || !TestTrue(TEXT("真实装备穿戴"), F.Send(Item.ItemId))) { return false; }
    if (!TestTrue(TEXT("真实有效击杀生成成长字段"), F.Hit(F.Mode->GetWaveMonsters()[0].Get()))) { return false; }
    if (Case == TEXT("FullAndDelta"))
    {
        TArray<FY01ItemGrant> Backpack;
        for (int32 I = 0; I < 500; ++I) { Backpack.Add({FGuid::NewGuid(), F.Data.Ref(K::Equipment)}); }
        if (!TestTrue(TEXT("五百件不生效包内实例"), L->AddItems(FY01DataCatalog(F.Data.Tables), Backpack).IsOk())) { return false; }
    }
    if (Case == TEXT("ExternalStats"))
    {
        auto Spec = ASC->MakeOutgoingSpec(UY01EquipmentAttackEffect::StaticClass(), 1, ASC->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_EquipmentAttack, 7);
        if (!TestTrue(TEXT("外部GE真实附加攻击"), ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data).IsValid())) { return false; }
    }
    auto Good = F.View(); FY01ResolvedAttributes Cached;
    if (!TestTrue(TEXT("读取来源已提交解析缓存"), S->ObserveAttributes(Cached).IsOk())
        || !TestEqual(TEXT("权威视图必须携带十行缓存"), Good.AttributeBreakdowns.Num(), 10)) { return false; }
    for (int32 I = 0; I < 10; ++I)
    { TestTrue(TEXT("视图逐行直接复制缓存而非重算"), FY01AttributeBreakdown::StaticStruct()->CompareScriptStruct(&Good.AttributeBreakdowns[I], &Cached.Breakdowns[I], 0)); }
    const auto* Equipped = Good.Items.FindByPredicate([&](const auto& I) { return I.ItemId == Item.ItemId; });
    if (!TestNotNull(TEXT("完整装备身份"), Equipped)) { return false; }
    const auto* Growth = Equipped->Growth.FindByPredicate([](const auto& G) { return G.Target == EY01GrowthTarget::EquipmentInstance; });
    if (!TestNotNull(TEXT("实例成长规则保留"), Growth)) { return false; }
    TestEqual(TEXT("实例成长计数"), Growth->AppliedCount, uint64(1));
    TestEqual(TEXT("实例成长累计"), Growth->Accumulated, 1.);
    FY01ViewSender Sender; FY01ViewReceiver Receiver; FY01ViewWire Wire; FString Error; TArray<uint8> Payload;
    auto SameView = [&](const FY01CommittedView& V)
    { return FY01CommittedView::StaticStruct()->CompareScriptStruct(&V, &Receiver.Get(), 0); };
    auto Drain = [&](const FY01CommittedView& Target)
    {
        const auto Previous = Receiver.Get(); Payload.Reset(); int32 Windows = 0;
        while (!Wire.Items.IsEmpty() && ++Windows < 10000)
        {
            TestTrue(TEXT("保持四片窗口"), Wire.Items.Num() <= 4);
            for (const auto& Chunk : Wire.Items)
            { TestTrue(TEXT("保持1024字节分片"), Chunk.Bytes.Num() <= 1024); Payload.Append(Chunk.Bytes); }
            FY01ViewAck Ack;
            if (!Receiver.Receive(Wire, Ack, Error)) { AddError(Error); return false; }
            if (Receiver.Get().ViewRevision != Target.ViewRevision)
            { TestTrue(TEXT("未齐窗口保留旧整份视图"), SameView(Previous)); }
            if (!Sender.Acknowledge(Ack, Wire, Error)) { AddError(Error); return false; }
        }
        return TestTrue(TEXT("全部窗口完成且完整反射相等"), Wire.Items.IsEmpty() && SameView(Target));
    };
    if (!TestTrue(TEXT("真实发送器生成完整包"), Sender.Offer(Good, Wire, Error))) { AddError(Error); return false; }
    const int32 FullBytes = Wire.Items[0].TotalBytes;
    if (!Drain(Good)) { return false; }
    {
        FMemoryReader Reader(Payload, true); uint32 Version = 0; Reader << Version;
        TestEqual(TEXT("完整包使用当前唯一版本"), Version, uint32(0x59303107));
    }
    if (Case == TEXT("ExternalStats"))
    {
        return TestEqual(TEXT("外部GE不伪造明细或阻止真实整份传输"), double(Receiver.Get().Stats.AttackDamage),
            Receiver.Get().AttributeBreakdowns[int32(A::AttackDamage) - 1].Final + 7.);
    }
    if (Case == TEXT("BreakdownOnly"))
    {
        auto Next = Good;
        auto* Entry = Next.AttributeBreakdowns[int32(A::AttackDamage) - 1].Contributions.FindByPredicate(
            [](const auto& X) { return X.Condition == EY01EffectCondition::HealthBelow; });
        if (!TestNotNull(TEXT("仅修改显示阈值的条件条目"), Entry)) { return false; }
        Entry->ConditionThreshold = 29;
        TestFalse(TEXT("同版本异明细必须拒绝"), Sender.Offer(Next, Wire, Error));
        ++Next.ViewRevision;
        if (!Sender.Offer(Next, Wire, Error) || !Drain(Next)) { AddError(Error); return false; }
        FY01ViewDeltaCommit Delta; FMemoryReader Reader(Payload, true); FObjectAndNameAsStringProxyArchive Ar(Reader, false);
        Ar.SetWantBinaryPropertySerialization(true); uint32 Version = 0; Ar << Version;
        FY01ViewDeltaCommit::StaticStruct()->SerializeBin(Ar, &Delta);
        return TestTrue(TEXT("仅明细变更只发bit15且无物品清单"), !Ar.IsError() && Version == 0x59303108
            && Delta.Fields == (1u << 15) && Delta.AttributeBreakdowns.Num() == 10 && Delta.Manifest.IsEmpty() && Delta.ChangedItems.IsEmpty());
    }
    if (Case == TEXT("FullAndDelta"))
    {
        TestTrue(TEXT("五百件完整包确实多窗"), FullBytes > FY01ViewWire::ChunkBytes * FY01ViewWire::WindowChunks);
        const auto& Attack = Receiver.Get().AttributeBreakdowns[int32(A::AttackDamage) - 1];
        const auto* Condition = Attack.Contributions.FindByPredicate([](const auto& X) { return X.Condition == EY01EffectCondition::HealthBelow; });
        if (!TestNotNull(TEXT("完整包保留条件贡献"), Condition)) { return false; }
        TestEqual(TEXT("完整包保留30百分点阈值"), Condition->ConditionThreshold, 30.);
        TestEqual(TEXT("完整包保留100百分点Amount"), Condition->Amount, 100.);
        TestFalse(TEXT("严格边界inactive"), Condition->bActive); TestEqual(TEXT("inactive贡献零"), Condition->Contribution, 0.);
        TestTrue(TEXT("完整包保留实例Growth贡献"), Attack.Contributions.ContainsByPredicate([](const auto& X) { return X.Kind == C::Growth && X.Contribution == 1.; }));
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 89);
        const auto Next = F.View();
        TestTrue(TEXT("条件翻转推进来源版本"), Next.SourceRevision > Good.SourceRevision);
        TestTrue(TEXT("条件翻转推进整份视图版本"), Next.ViewRevision > Good.ViewRevision);
        TestEqual(TEXT("不改变库存版本"), Next.Revision, Good.Revision);
        TestEqual(TEXT("不改变装备实体版本"), Next.Items[0].EntityRevision, Good.Items[0].EntityRevision);
        if (!TestTrue(TEXT("真实发送器生成条件差量"), Sender.Offer(Next, Wire, Error))) { return false; }
        TestTrue(TEXT("差量不重复五百件库存"), Wire.Items[0].TotalBytes < FullBytes);
        if (!Drain(Next)) { return false; }
        FY01ViewDeltaCommit Delta; FMemoryReader Reader(Payload, true); FObjectAndNameAsStringProxyArchive Ar(Reader, false);
        Ar.SetWantBinaryPropertySerialization(true); uint32 Version = 0; Ar << Version;
        TestEqual(TEXT("差量使用当前唯一版本"), Version, uint32(0x59303108));
        FY01ViewDeltaCommit::StaticStruct()->SerializeBin(Ar, &Delta);
        TestTrue(TEXT("差量完整解码"), !Ar.IsError() && Reader.Tell() == Payload.Num());
        TestTrue(TEXT("独立明细bit15"), (Delta.Fields & (1u << 15)) != 0);
        TestTrue(TEXT("条件翻转不生成任何物品manifest"), Delta.Manifest.IsEmpty() && Delta.ChangedItems.IsEmpty());
        TestEqual(TEXT("差量只替换十行明细"), Delta.AttributeBreakdowns.Num(), 10);
        const auto& GoldGain = Receiver.Get().AttributeBreakdowns[int32(A::GoldGain) - 1];
        TestEqual(TEXT("金币获得Final仍为20百分点不是0.2"), GoldGain.Final, 20.);
        TestEqual(TEXT("金币获得小计为零"), GoldGain.Subtotal, 0.);
        TestEqual(TEXT("只运行时归一化"), ([&] { FY01ResolvedAttributes V; S->ObserveAttributes(V); return V.GoldGainPercent; })(), .2);
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 90);
        const auto Inactive = F.View();
        if (!Sender.Offer(Inactive, Wire, Error) || !Drain(Inactive)) { return false; }
        return TestEqual(TEXT("恢复inactive也通过真实差量"), Receiver.Get().AttributeBreakdowns[int32(A::GoldGain) - 1].Final, 0.);
    }
    if (Case == TEXT("Clear") || Case == TEXT("LegacyEmpty"))
    {
        auto Empty = Good; ++Empty.ViewRevision; Empty.AttributeBreakdowns.Reset();
        if (Case == TEXT("LegacyEmpty")) { Sender.Reset(); Receiver.Reset(); }
        if (!TestTrue(TEXT("空明细旧夹具允许全量或清除差量"), Sender.Offer(Empty, Wire, Error)) || !Drain(Empty)) { return false; }
        if (Case == TEXT("Clear"))
        {
            FY01ViewDeltaCommit Delta; FMemoryReader Reader(Payload, true); FObjectAndNameAsStringProxyArchive Ar(Reader, false);
            Ar.SetWantBinaryPropertySerialization(true); uint32 Version = 0; Ar << Version;
            FY01ViewDeltaCommit::StaticStruct()->SerializeBin(Ar, &Delta);
            TestTrue(TEXT("置bit且空数组代表清除"), !Ar.IsError() && Version == 0x59303108 && (Delta.Fields & (1u << 15)) && Delta.AttributeBreakdowns.IsEmpty());
            ++Empty.ViewRevision; ++Empty.Gold;
            if (!Sender.Offer(Empty, Wire, Error) || !Drain(Empty)) { return false; }
            FMemoryReader UnchangedReader(Payload, true); FObjectAndNameAsStringProxyArchive UnchangedAr(UnchangedReader, false);
            UnchangedAr.SetWantBinaryPropertySerialization(true); UnchangedAr << Version; Delta = {};
            FY01ViewDeltaCommit::StaticStruct()->SerializeBin(UnchangedAr, &Delta);
            TestTrue(TEXT("未变化明细无bit无数组"), !UnchangedAr.IsError() && !(Delta.Fields & (1u << 15)) && Delta.AttributeBreakdowns.IsEmpty());
        }
        return true;
    }
    if (Case == TEXT("Busy"))
    {
        FY01PreparedMutation Plan; FY01SourceTransaction Tx;
        if (!TestTrue(TEXT("真实来源准备"), S->PrepareMutation({}, {}, Plan).IsOk())
            || !TestTrue(TEXT("真实来源暂存"), S->StageMutation(Plan, Tx).IsOk())) { return false; }
        TestEqual(TEXT("来源Busy不发布半明细"), L->PublishCommittedView().Code, EY01ItemError::Busy);
        FY01CommittedView During; L->ReadCommittedView(During);
        TestTrue(TEXT("Busy读取原整份视图"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Good, &During, 0));
        TestTrue(TEXT("真实回滚"), S->RollbackMutation(Tx).IsOk());
        if (!TestTrue(TEXT("持有库存事务"), FY01R36DeliveryTestAccess::Lock(L))) { return false; }
        TestEqual(TEXT("库存Busy不读取或发布明细"), L->PublishCommittedView().Code, EY01ItemError::Busy);
        L->ReadCommittedView(During); FY01R36DeliveryTestAccess::Unlock(L);
        return TestTrue(TEXT("库存Busy同样保留整份"), FY01CommittedView::StaticStruct()->CompareScriptStruct(&Good, &During, 0));
    }
    auto Bad = Good; ++Bad.ViewRevision;
    auto& Attack = Bad.AttributeBreakdowns[int32(A::AttackDamage) - 1];
    auto* Conditional = Attack.Contributions.FindByPredicate([](const auto& X) { return X.Condition != EY01EffectCondition::None; });
    auto* Mapping = Attack.Contributions.FindByPredicate([](const auto& X) { return X.Kind == C::Mapping; });
    if (!TestNotNull(TEXT("测试条件来源"), Conditional) || !TestNotNull(TEXT("测试映射来源"), Mapping)) { return false; }
    if (Case == TEXT("Condition101") || Case == TEXT("DeltaCondition101")) { Conditional->ConditionThreshold = 101; }
    if (Case == TEXT("UnknownCondition")) { Conditional->Condition = static_cast<EY01EffectCondition>(255); }
    if (Case == TEXT("NaN")) { Attack.Final = std::numeric_limits<double>::quiet_NaN(); }
    if (Case == TEXT("Negative")) { Conditional->Amount = -1; }
    if (Case == TEXT("UnknownKind")) { Conditional->Kind = static_cast<C>(255); }
    if (Case == TEXT("NoneThreshold")) { Conditional->Condition = EY01EffectCondition::None; }
    if (Case == TEXT("InactiveContribution")) { Conditional->Contribution = 100; }
    if (Case == TEXT("NoneInactive")) { Attack.Contributions[0].bActive = false; }
    if (Case == TEXT("ConditionalTarget")) { Bad.AttributeBreakdowns[0].Contributions[0].Condition = EY01EffectCondition::HealthBelow; }
    if (Case == TEXT("BaseCondition")) { Attack.Contributions[0].Condition = EY01EffectCondition::HealthBelow; }
    if (Case == TEXT("GrowthCondition")) { Conditional->Kind = C::Growth; }
    if (Case == TEXT("MappingSource")) { Mapping->SourceAttribute = A::None; }
    if (Case == TEXT("MappingTarget")) { Bad.AttributeBreakdowns[0].Contributions.Add(*Mapping); }
    if (Case == TEXT("NonMappingSource")) { Conditional->SourceAttribute = A::Strength; }
    if (Case == TEXT("IncomePercent")) { auto X = *Conditional; X.Condition = EY01EffectCondition::None; X.ConditionThreshold = 0; X.bActive = true; X.Contribution = X.Amount; Bad.AttributeBreakdowns[int32(A::GoldPerSecond) - 1].Contributions.Add(X); }
    auto& GoldGain = Bad.AttributeBreakdowns[int32(A::GoldGain) - 1];
    if (Case == TEXT("GoldGainSubtotal")) { GoldGain.Subtotal = 1; }
    if (Case == TEXT("GoldGainFinal")) { GoldGain.Final = .2; }
    if (Case == TEXT("GoldGainBase")) { GoldGain.Contributions.Add(Attack.Contributions[0]); }
    if (Case == TEXT("FinalCoherence")) { Attack.Final += 1; }
    if (Case == TEXT("SubtotalCoherence")) { Attack.Contributions[0].Contribution += 1; Attack.Contributions[0].Amount += 1; }
    if (Case == TEXT("PercentCoherence")) { Attack.PercentTotal = 1; Attack.Final = Attack.Subtotal * 1.01; }
    if (Case == TEXT("EmptySourceKey")) { Conditional->SourceKey.Reset(); }
    if (Case == TEXT("DuplicateAttributes")) { Bad.AttributeBreakdowns[0] = Bad.AttributeBreakdowns[1]; }
    if (Case == TEXT("MissingAttribute")) { Bad.AttributeBreakdowns.Pop(); }
    if (Case == TEXT("UnknownAttribute")) { Bad.AttributeBreakdowns[0].Attribute = static_cast<A>(255); }
    const bool DeltaCase = Case.StartsWith(TEXT("Delta")) || Case == TEXT("OldDeltaVersion");
    const bool OldVersion = Case.StartsWith(TEXT("Old"));
    if (!DeltaCase && !OldVersion)
    {
        FY01ViewSender Untrusted; FY01ViewWire UntrustedWire;
        TestFalse(TEXT("发送器拒绝非法明细"), Untrusted.Offer(Bad, UntrustedWire, Error));
    }
    TArray<uint8> Bytes; FMemoryWriter Writer(Bytes, true); FObjectAndNameAsStringProxyArchive Ar(Writer, false);
    Ar.SetWantBinaryPropertySerialization(true);
    uint32 Version = DeltaCase ? (OldVersion ? 0x59303106 : 0x59303108) : (OldVersion ? 0x59303105 : 0x59303107); Ar << Version;
    if (DeltaCase)
    {
        FY01ViewDeltaCommit Delta; Delta.BaseViewRevision = Good.ViewRevision;
        Delta.MatchId = Bad.MatchId; Delta.JoinSequence = Bad.JoinSequence; Delta.ConnectionId = Bad.ConnectionId;
        Delta.ViewRevision = Bad.ViewRevision; Delta.Revision = Bad.Revision; Delta.SourceRevision = Bad.SourceRevision;
        Delta.ConfigRevision = Bad.ConfigRevision; Delta.ExpectedSeq = Bad.ExpectedSeq; Delta.SampleTime = Bad.SampleTime;
        Delta.Fields = 1u << 15; Delta.AttributeBreakdowns = Bad.AttributeBreakdowns;
        if (Case == TEXT("DeltaUnknownBit")) { Delta.Fields |= 1u << 17; }
        if (Case == TEXT("DeltaNoBitWithArray")) { Delta.Fields = 0; }
        FY01ViewDeltaCommit::StaticStruct()->SerializeBin(Ar, &Delta);
    }
    else
    {
        FY01ViewCommit Commit; Commit.Header = Bad; Commit.Header.Items.Reset(); Commit.ChangedItems = Bad.Items;
        for (const auto& I : Bad.Items)
        { auto& M = Commit.Manifest.AddDefaulted_GetRef(); M.ItemId = I.ItemId; M.EntityRevision = I.EntityRevision; M.LastChangedRevision = I.LastChangedRevision; }
        FY01ViewCommit::StaticStruct()->SerializeBin(Ar, &Commit);
    }
    if (!TestFalse(TEXT("恶意包编码本身不出错"), Ar.IsError())) { return false; }
    const auto Transfer = FGuid::NewGuid(); bool Accepted = true; int32 Window = 0;
    for (int32 Offset = 0; Offset < Bytes.Num(); Offset += FY01ViewWire::ChunkBytes * FY01ViewWire::WindowChunks)
    {
        Wire.Items.Reset(); ++Window;
        for (int32 I = 0; I < FY01ViewWire::WindowChunks && Offset + I * FY01ViewWire::ChunkBytes < Bytes.Num(); ++I)
        {
            auto& Chunk = Wire.Items.AddDefaulted_GetRef(); Chunk.Scope = Y01ViewTiming::Scope(Bad);
            Chunk.Scope.TransferId = Transfer; Chunk.Scope.WindowSequence = Window;
            Chunk.Offset = Offset + I * FY01ViewWire::ChunkBytes; Chunk.TotalBytes = Bytes.Num();
            Chunk.Digest = FCrc::MemCrc32(Bytes.GetData(), Bytes.Num());
            Chunk.Bytes.Append(Bytes.GetData() + Chunk.Offset, FMath::Min(FY01ViewWire::ChunkBytes, Bytes.Num() - Chunk.Offset));
            Chunk.ChunkDigest = FCrc::MemCrc32(Chunk.Bytes.GetData(), Chunk.Bytes.Num());
        }
        FY01ViewAck Ack; Accepted = Receiver.Receive(Wire, Ack, Error);
        TestTrue(TEXT("任何恶意窗口均不覆盖已发布整份视图"), SameView(Good));
        if (!Accepted) { TestFalse(TEXT("拒绝不提供ACK"), Ack.TransferId.IsValid()); break; }
    }
    TestFalse(TEXT("校验和正确的恶意或旧版本提交必须被拒绝"), Accepted);
    TestEqual(TEXT("明确版本或业务校验诊断"), Error, FString(OldVersion ? TEXT("完整视图属性编码版本不兼容") : TEXT("清单、期望版本或槽位不完整，需要重同步")));
    return TestTrue(TEXT("拒绝保持接收器整份原版本"), SameView(Good));
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R37ResolverTest, "Y01.Framework.R37.Resolver",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R37ResolverTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : {TEXT("PercentOrder"), TEXT("PrimaryGrowth"), TEXT("EquipmentGrowth"), TEXT("HealthRate"),
        TEXT("GoldGain"), TEXT("Conditions"), TEXT("Determinism"), TEXT("Invalid"), TEXT("Precision")})
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01R37ResolverTest::RunTest(const FString& Case)
{
    using A = EY01Attribute;
    using E = EY01SourceEffectKind;
    using C = EY01ContributionKind;
    FY01UnitCombatStats Base; Base.Strength = 10; Base.AttackDamage = 5; Base.Health = 300; Base.AttackInterval = 1;
    auto Effect = [](const TCHAR* Id, E Kind, A Target, double Amount, A Source = A::None)
    {
        FY01SourceEffectSnapshot S; S.Source.Kind = EY01RuntimeSourceKind::Equipment; S.Source.LocalKey = TEXT("ItemA");
        S.EffectId = FName(Id); S.Kind = Kind; S.TargetAttribute = Target; S.SourceAttribute = Source;
        if (Kind == E::AttackFlat || Kind == E::GoldPerSecond) { S.Magnitude = Amount; }
        else { S.Amount = Amount; }
        return S;
    };
    const TArray<FY01SourceEffectSnapshot> Main = {
        Effect(TEXT("StrengthPercent"), E::AttributePercent, A::Strength, 20),
        Effect(TEXT("Mapping"), E::MappingCoefficientAdd, A::AttackDamage, 3, A::Strength),
        Effect(TEXT("LegacyAttack"), E::AttackFlat, A::None, 10),
        Effect(TEXT("Attack20"), E::AttributePercent, A::AttackDamage, 20),
        Effect(TEXT("Attack30"), E::AttributePercent, A::AttackDamage, 30)};
    FY01ResolvedAttributes Out;
    auto Row = [&](A Attribute) -> const FY01AttributeBreakdown& { return Out.Breakdowns[int32(Attribute) - 1]; };
    auto Resolve = [&](const TArray<FY01SourceEffectSnapshot>& Effects, double Growth = 0, double Ratio = 1)
    {
        if (!TestTrue(TEXT("纯解析成功"), FY01AttributeResolver::Resolve(Base, Growth, Effects, Out, Ratio).IsOk())) { return false; }
        return TestEqual(TEXT("每次成功均有稳定十行明细"), Out.Breakdowns.Num(), 10);
    };
    auto Reject = [&](const TArray<FY01SourceEffectSnapshot>& Effects, double Growth = 0, double Ratio = 1)
    {
        Out.Stats = Base; Out.AttackBonus = 99; Out.GoldPerSecond = 99; Out.GoldGainPercent = 99; Out.Breakdowns.SetNum(10);
        TestFalse(TEXT("非法或精度丢失输入拒绝"), FY01AttributeResolver::Resolve(Base, Growth, Effects, Out, Ratio).IsOk());
        const FY01UnitCombatStats Empty;
        TestTrue(TEXT("失败Stats恢复默认"), FY01UnitCombatStats::StaticStruct()->CompareScriptStruct(&Out.Stats, &Empty, 0));
        TestEqual(TEXT("失败无攻击输出"), Out.AttackBonus, 0.); TestEqual(TEXT("失败无收入输出"), Out.GoldPerSecond, 0.);
        TestEqual(TEXT("失败无金币获得输出"), Out.GoldGainPercent, 0.); TestTrue(TEXT("失败无部分明细"), Out.Breakdowns.IsEmpty());
    };
    if (Case == TEXT("PercentOrder"))
    {
        if (!Resolve(Main)) { return false; }
        TestEqual(TEXT("力量先结算百分比"), Out.Stats.Strength, 12.f);
        TestEqual(TEXT("基础攻击也参与加法百分比"), Out.Stats.AttackDamage, 76.5f);
        TestEqual(TEXT("单加法GE仅发final减base"), Out.AttackBonus, 71.5);
        if (!TestEqual(TEXT("固定十行"), Out.Breakdowns.Num(), 10)) { return false; }
        TestEqual(TEXT("攻击前百分比小计"), Row(A::AttackDamage).Subtotal, 51.);
        TestEqual(TEXT("攻击百分比相加不相乘"), Row(A::AttackDamage).PercentTotal, 50.);
        TestEqual(TEXT("明细final"), Row(A::AttackDamage).Final, 76.5);
        TestEqual(TEXT("力量小计保留百分比前10"), Row(A::Strength).Subtotal, 10.);
        for (int32 I = 0; I < 10; ++I) { TestEqual(TEXT("行按枚举稳定排序"), int32(Out.Breakdowns[I].Attribute), I + 1); }
        const auto& Contributions = Row(A::AttackDamage).Contributions;
        const auto* Mapping = Contributions.FindByPredicate([](const auto& X) { return X.Kind == C::Mapping; });
        if (!TestNotNull(TEXT("换算来源明细"), Mapping)) { return false; }
        TestEqual(TEXT("映射Amount是系数"), Mapping->Amount, 3.); TestEqual(TEXT("映射用有效力量"), Mapping->Contribution, 36.);
        TestEqual(TEXT("映射源属性"), Mapping->SourceAttribute, A::Strength);
        const auto* Legacy = Contributions.FindByPredicate([](const auto& X) { return X.EffectId == FName(TEXT("LegacyAttack")); });
        if (!TestNotNull(TEXT("旧攻击纳入解析"), Legacy)) { return false; }
        TestEqual(TEXT("固定值不是乘后归因"), Legacy->Contribution, 10.);
        for (const auto& X : Contributions)
        { if (X.Kind == C::Percent) { TestEqual(TEXT("百分比Amount与Contribution均为百分点"), X.Amount, X.Contribution); } }
        return true;
    }
    if (Case == TEXT("PrimaryGrowth"))
    {
        Base.Agility = 4; Base.Wisdom = 2;
        auto Effects = Main;
        Effects.Add(Effect(TEXT("StrengthFlat"), E::AttributeFlat, A::Strength, 5));
        Effects.Add(Effect(TEXT("AgilityFlat"), E::AttributeFlat, A::Agility, 1));
        Effects.Add(Effect(TEXT("AgilityPercent"), E::AttributePercent, A::Agility, 100));
        Effects.Add(Effect(TEXT("WisdomPercent"), E::AttributePercent, A::Wisdom, 50));
        Effects.Add(Effect(TEXT("AgilityAttack"), E::MappingCoefficientAdd, A::AttackDamage, 2, A::Agility));
        Effects.Add(Effect(TEXT("WisdomAttack"), E::MappingCoefficientAdd, A::AttackDamage, 4, A::Wisdom));
        if (!Resolve(Effects, 5)) { return false; }
        TestEqual(TEXT("基础固定和玩家成长均先于百分比"), Out.Stats.Strength, 24.f);
        TestEqual(TEXT("敏捷独立百分比"), Out.Stats.Agility, 10.f); TestEqual(TEXT("智慧独立百分比"), Out.Stats.Wisdom, 3.f);
        TestEqual(TEXT("三维有效值共同换算"), Out.Stats.AttackDamage, 178.5f);
        const auto* Growth = Row(A::Strength).Contributions.FindByPredicate([](const auto& X) { return X.Kind == C::Growth; });
        if (!TestNotNull(TEXT("玩家成长单列"), Growth)) { return false; }
        TestEqual(TEXT("玩家成长来源"), Growth->SourceKey, FString(TEXT("玩家成长")));
        TestEqual(TEXT("成长不冒充乘后固定值"), Growth->Contribution, 5.);
        return true;
    }
    if (Case == TEXT("EquipmentGrowth"))
    {
        auto Effects = Main;
        auto Growth = Effect(TEXT("GeneratedAttack"), E::AttackFlat, A::None, 4); Growth.bGrowthContribution = true;
        Effects.Add(Growth); Effects.Add(Effect(TEXT("DirectAttack"), E::AttributeFlat, A::AttackDamage, 2));
        if (!Resolve(Effects)) { return false; }
        TestEqual(TEXT("实例成长与直接固定攻击先于百分比"), Out.Stats.AttackDamage, 85.5f);
        const auto* X = Row(A::AttackDamage).Contributions.FindByPredicate([](const auto& V) { return V.EffectId == FName(TEXT("GeneratedAttack")); });
        if (!TestNotNull(TEXT("成长攻击明细"), X)) { return false; }
        TestEqual(TEXT("标记而非EffectId前缀识别成长"), X->Kind, C::Growth); TestEqual(TEXT("成长小计贡献"), X->Contribution, 4.);
        return true;
    }
    if (Case == TEXT("HealthRate"))
    {
        const TArray<FY01SourceEffectSnapshot> Effects = {
            Effect(TEXT("HealthFlat"), E::AttributeFlat, A::MaxHealth, 100),
            Effect(TEXT("HealthMapping"), E::MappingCoefficientAdd, A::MaxHealth, 10, A::Strength),
            Effect(TEXT("Health20"), E::AttributePercent, A::MaxHealth, 20),
            Effect(TEXT("Health30"), E::AttributePercent, A::MaxHealth, 30),
            Effect(TEXT("Rate100"), E::AttributePercent, A::AttackRate, 100)};
        if (!Resolve(Effects)) { return false; }
        TestEqual(TEXT("生命基础固定及映射同乘百分比"), Out.Stats.Health, 750.f);
        TestEqual(TEXT("速率百分比后取间隔倒数"), Out.Stats.AttackInterval, .5f);
        TestEqual(TEXT("明细速率而非间隔"), Row(A::AttackRate).Final, 2.);
        auto RateEffects = Effects; RateEffects.Add(Effect(TEXT("RateFlat"), E::AttributeFlat, A::AttackRate, .5));
        RateEffects.Add(Effect(TEXT("RateMapping"), E::MappingCoefficientAdd, A::AttackRate, .05, A::Strength));
        if (!Resolve(RateEffects)) { return false; }
        TestEqual(TEXT("速率小计包括基础倒数固定及映射"), Row(A::AttackRate).Subtotal, 2.);
        TestEqual(TEXT("速率完整公式间隔"), Out.Stats.AttackInterval, .25f);
        return true;
    }
    if (Case == TEXT("GoldGain"))
    {
        auto Effects = Main; Effects.Add(Effect(TEXT("Income"), E::GoldPerSecond, A::None, 7));
        Effects.Add(Effect(TEXT("IncomeFlat"), E::AttributeFlat, A::GoldPerSecond, 2));
        Effects.Add(Effect(TEXT("IncomeMapping"), E::MappingCoefficientAdd, A::GoldPerSecond, 1, A::Strength));
        Effects.Add(Effect(TEXT("Gold20"), E::AttributePercent, A::GoldGain, 20));
        if (!Resolve(Effects)) { return false; }
        TestEqual(TEXT("GoldGain只输出归一化不改变收入"), Out.GoldGainPercent, .2);
        TestEqual(TEXT("收入按固定及有效三维映射不乘金币获得"), Out.GoldPerSecond, 21.);
        TestEqual(TEXT("GoldGain无固定小计"), Row(A::GoldGain).Subtotal, 0.);
        TestEqual(TEXT("GoldGain明细final为百分点"), Row(A::GoldGain).Final, 20.);
        TestEqual(TEXT("GoldGain明细百分比总量"), Row(A::GoldGain).PercentTotal, 20.);
        TestEqual(TEXT("GoldGain只有百分比贡献无基础行"), Row(A::GoldGain).Contributions.Num(), 1);
        for (const auto& Entry : Row(A::GoldGain).Contributions)
        { TestEqual(TEXT("GoldGain贡献仅Percent"), Entry.Kind, C::Percent); }
        Effects.Add(Effect(TEXT("GoldZero"), E::AttributePercent, A::GoldGain, 0));
        Effects.Add(Effect(TEXT("Gold30"), E::AttributePercent, A::GoldGain, 30));
        if (!Resolve(Effects)) { return false; }
        TestEqual(TEXT("多个GoldGain百分比相加零合法"), Out.GoldGainPercent, .5);
        TestEqual(TEXT("多个GoldGain仍不影响收入"), Out.GoldPerSecond, 21.);
        return true;
    }
    if (Case == TEXT("Conditions"))
    {
        auto Effects = Main; auto Conditional = Effect(TEXT("Conditional"), E::AttributeFlat, A::AttackDamage, 20);
        Conditional.Condition = EY01EffectCondition::HealthBelow; Conditional.ConditionThreshold = 30; Effects.Add(Conditional);
        for (double Ratio : {.3, .29})
        {
            if (!Resolve(Effects, 0, Ratio)) { return false; }
            TestEqual(TEXT("Below严格小于，平局关闭"), Out.Stats.AttackDamage, Ratio == .3 ? 76.5f : 106.5f);
            const auto* X = Row(A::AttackDamage).Contributions.FindByPredicate([](const auto& V) { return V.EffectId == FName(TEXT("Conditional")); });
            if (!TestNotNull(TEXT("非激活来源仍保留"), X)) { return false; }
            TestEqual(TEXT("条件Amount保留"), X->Amount, 20.); TestEqual(TEXT("条件只计激活贡献"), X->Contribution, Ratio == .3 ? 0. : 20.);
            TestEqual(TEXT("严格条件状态"), X->bActive, Ratio < .3); TestEqual(TEXT("条件枚举保留"), X->Condition, EY01EffectCondition::HealthBelow);
            TestEqual(TEXT("阈值百分点保留"), X->ConditionThreshold, 30.);
        }
        Effects.Last().Condition = EY01EffectCondition::HealthAbove;
        for (double Ratio : {.3, .31})
        { if (!Resolve(Effects, 0, Ratio)) { return false; } TestEqual(TEXT("Above严格大于，平局关闭"), Out.Stats.AttackDamage, Ratio == .3 ? 76.5f : 106.5f); }
        Effects.Last() = Effect(TEXT("ConditionalPercent"), E::AttributePercent, A::AttackDamage, 20);
        Effects.Last().Condition = EY01EffectCondition::HealthBelow; Effects.Last().ConditionThreshold = 30;
        for (double Ratio : {.3, .29})
        {
            if (!Resolve(Effects, 0, Ratio)) { return false; }
            TestEqual(TEXT("条件百分比只叠加激活量"), Row(A::AttackDamage).PercentTotal, Ratio == .3 ? 50. : 70.);
        }
        TestTrue(TEXT("None恒激活"), FY01AttributeResolver::ConditionActive(EY01EffectCondition::None, 0, 0));
        TestFalse(TEXT("Below0永不触发"), FY01AttributeResolver::ConditionActive(EY01EffectCondition::HealthBelow, 0, 0));
        TestFalse(TEXT("Above100永不触发"), FY01AttributeResolver::ConditionActive(EY01EffectCondition::HealthAbove, 100, 1));
        return true;
    }
    if (Case == TEXT("Determinism"))
    {
        auto Effects = Main;
        auto Other = Effect(TEXT("LegacyAttack"), E::AttackFlat, A::None, 2); Other.Source.LocalKey = TEXT("ItemB"); Effects.Add(Other);
        Other.Source.Kind = EY01RuntimeSourceKind::Talent; Other.Source.LocalKey = TEXT("ItemA"); Effects.Add(Other);
        if (!Resolve(Effects)) { return false; }
        const auto First = Out;
        TArray<FY01SourceEffectSnapshot> Reversed; for (int32 I = Effects.Num() - 1; I >= 0; --I) { Reversed.Add(Effects[I]); }
        if (!Resolve(Reversed)) { return false; }
        TestEqual(TEXT("逆序计算攻击完全一致"), Out.Stats.AttackDamage, First.Stats.AttackDamage);
        TestEqual(TEXT("逆序单GE加值一致"), Out.AttackBonus, First.AttackBonus);
        TestEqual(TEXT("逆序行数一致"), Out.Breakdowns.Num(), First.Breakdowns.Num());
        for (int32 I = 0; I < Out.Breakdowns.Num(); ++I)
        { TestTrue(TEXT("逆序逐行全部反射字段一致"), FY01AttributeBreakdown::StaticStruct()->CompareScriptStruct(&Out.Breakdowns[I], &First.Breakdowns[I], 0)); }
        TSet<FString> Keys;
        for (const auto& X : Row(A::AttackDamage).Contributions)
        { if (X.EffectId == FName(TEXT("LegacyAttack"))) { Keys.Add(X.SourceKey); } }
        TestEqual(TEXT("来源kind及localkey均区分"), Keys.Num(), 3);
        const auto* B = Row(A::Strength).Contributions.FindByPredicate([](const auto& X) { return X.Kind == C::Base; });
        if (!TestNotNull(TEXT("基础来源"), B)) { return false; } TestEqual(TEXT("基础key"), B->SourceKey, FString(TEXT("基础")));
        if (!Resolve({Effect(TEXT("ZeroAttack"), E::AttributePercent, A::AttackDamage, 0)})) { return false; }
        TestEqual(TEXT("零百分比保留基础攻击"), Out.Stats.AttackDamage, 5.f);
        TestEqual(TEXT("零百分比明细"), Row(A::AttackDamage).PercentTotal, 0.);
        Base.Strength = 0;
        if (!Resolve({Effect(TEXT("ZeroSubtotal"), E::AttributePercent, A::Strength, 1.e-20)})) { return false; }
        TestEqual(TEXT("零小计允许因子吸收不产生虚假值"), Out.Stats.Strength, 0.f);
        return true;
    }
    if (Case == TEXT("Invalid"))
    {
        for (double Ratio : {-1., 1.01, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) { Reject({}, 0, Ratio); }
        for (double Growth : {-1., std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) { Reject({}, Growth); }
        Reject({Effect(TEXT("Negative"), E::AttributePercent, A::AttackDamage, -1)});
        Reject({Effect(TEXT("NaN"), E::AttributeFlat, A::Armor, std::numeric_limits<double>::quiet_NaN())});
        Reject({Effect(TEXT("Infinity"), E::AttributePercent, A::AttackDamage, std::numeric_limits<double>::infinity())});
        Reject({Effect(TEXT("IncomePercent"), E::AttributePercent, A::GoldPerSecond, 20)});
        auto Bad = Effect(TEXT("BadCondition"), E::AttributeFlat, A::AttackDamage, 20); Bad.Condition = EY01EffectCondition::HealthBelow;
        Bad.ConditionThreshold = 101; Reject({Bad}, 0, 1); Bad.ConditionThreshold = 30; Bad.Condition = static_cast<EY01EffectCondition>(255); Reject({Bad});
        Base.AttackInterval = 0; Reject({}); Base.AttackInterval = std::numeric_limits<float>::denorm_min(); Reject({});
        Base.AttackInterval = 1; Base.AttackDamage = -1; Reject({}); Base.AttackDamage = 5; Base.Strength = std::numeric_limits<float>::quiet_NaN(); Reject({});
        return true;
    }
    if (Case == TEXT("Precision"))
    {
        Reject({Effect(TEXT("FloatAbsorbed"), E::AttributeFlat, A::Strength, 1.e-10)});
        Reject({}, 1.e-300);
        Reject({Effect(TEXT("HugeFlat"), E::AttributeFlat, A::AttackDamage, std::numeric_limits<double>::max())});
        Reject({Effect(TEXT("HugePercent"), E::AttributePercent, A::MaxHealth, std::numeric_limits<double>::max())});
        Reject({Effect(TEXT("FactorAbsorbed"), E::AttributePercent, A::AttackDamage, 1.e-20)});
        Reject({Effect(TEXT("FloatPercentAbsorbed"), E::AttributePercent, A::AttackDamage, 1.e-8)});
        Reject({Effect(TEXT("PercentUnderflow"), E::AttributePercent, A::AttackDamage, std::numeric_limits<double>::denorm_min())});
        Reject({Effect(TEXT("GoldUnderflow"), E::AttributePercent, A::GoldGain, std::numeric_limits<double>::denorm_min())});
        Reject({Effect(TEXT("GasBaseAbsorbed"), E::AttributePercent, A::AttackDamage, 1.e12)});
        Reject({Effect(TEXT("AIncome"), E::GoldPerSecond, A::None, 1.e100), Effect(TEXT("BIncome"), E::GoldPerSecond, A::None, 1)});
        Reject({Effect(TEXT("AOverflow"), E::GoldPerSecond, A::None, std::numeric_limits<double>::max()),
            Effect(TEXT("BOverflow"), E::GoldPerSecond, A::None, std::numeric_limits<double>::max())});
        Reject({Effect(TEXT("APercent"), E::AttributePercent, A::GoldGain, 1.e100), Effect(TEXT("BPercent"), E::AttributePercent, A::GoldGain, 1)});
        Base.Strength = 0; Base.Agility = std::numeric_limits<float>::denorm_min();
        Reject({Effect(TEXT("MappingUnderflow"), E::MappingCoefficientAdd, A::GoldPerSecond, 1.e-300, A::Agility)});
        Base.Agility = 1;
        Reject({Effect(TEXT("MappingFloatUnderflow"), E::MappingCoefficientAdd, A::AttackDamage, 1.e-300, A::Agility)});
        return true;
    }
    AddError(TEXT("未知R37解析场景")); return false;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R37LifeConditionTest, "Y01.Framework.R37.LifeCondition",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R37LifeConditionTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : {TEXT("Below"), TEXT("Above"), TEXT("CandidateMax"), TEXT("CandidateMaxPrecision"), TEXT("CandidateRollback"),
        TEXT("RefreshRecrossing"), TEXT("PendingCrossing"), TEXT("PendingSameInterval"), TEXT("PendingMaxMutation"), TEXT("StageDelegateInjury"), TEXT("NonAttackRows"),
        TEXT("BaseAttack"), TEXT("IncomeSegment"), TEXT("LoadoutBusy"), TEXT("AutoAttack"), TEXT("TwoPlayers"), TEXT("LifeRestore"), TEXT("Ended"), TEXT("RefreshRejected")})
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01R37LifeConditionTest::RunTest(const FString& Case)
{
    const FString Command = FCommandLine::Get(); FCommandLine::Set(TEXT("")); ON_SCOPE_EXIT { FCommandLine::Set(*Command); };
    // 构造和后续Tick共用单调帧号，避免UE visited缓存跳过刷新；只在测试最外层恢复。
    TGuardValue<uint64> Frame(GFrameCounter, GFrameCounter);
    FGrowthWorld F(1, TEXT("AttributeCondition") + Case, true);
    if (!TestTrue(TEXT("条件独立配置初始化"), F.Error.IsEmpty())) { AddError(F.Error); return false; }
    auto* ASC = F.P->GetCombatASC(); auto* S = F.P->GetSourceEffects(); auto* L = F.P->GetLoadout();
    auto Health = [&](float Value) { ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), Value); };
    auto Tick = [&] { ++GFrameCounter; F.World->Tick(LEVELTICK_All, .01f); };
    auto Snapshot = [&] { FY01SourceSnapshot V; TestTrue(TEXT("读取已提交来源"), S->Observe(V).IsOk()); return V; };
    auto Attributes = [&] { FY01ResolvedAttributes V; TestTrue(TEXT("读取已提交条件明细"), S->ObserveAttributes(V).IsOk()); return V; };
    auto Handles = [&] { return ASC->GetActiveEffects(FGameplayEffectQuery()); };
    double Now = F.P->GetIncome()->Observe().State.LastTime;
    FY01R36DeliveryTestAccess::Clock(F.P->GetIncome(), [&] { return Now; });
    ON_SCOPE_EXIT { FY01R36DeliveryTestAccess::Clock(F.P->GetIncome(), {}); };
    auto& Row = F.Data.Row<FY01EquipmentRow>(K::Equipment);
    if (Case == TEXT("Above")) { Row.EffectIds = {F.Data.Ref(K::SourceEffect, TEXT("AboveAttack"))}; }
    if (Case == TEXT("CandidateMax") || Case == TEXT("CandidateMaxPrecision") || Case == TEXT("CandidateRollback"))
    { Row.EffectIds = {F.Data.Ref(K::SourceEffect, TEXT("Above40")), F.Data.Ref(K::SourceEffect, TEXT("Max300"))}; }
    if (Case == TEXT("CandidateMaxPrecision"))
    {
        F.Data.Row<FY01SourceEffectRow>(K::SourceEffect, TEXT("Above40")).Amount = 1.e-8;
        // 升上限同步加血到450使Above40真实激活，1e-8贡献亚精度被吞：条件排空记一次精度故障并停止。
        AddExpectedError(TEXT("属性条件刷新停止"), EAutomationExpectedErrorFlags::Contains, 1);
    }
    if (Case == TEXT("RefreshRecrossing"))
    {
        auto Low20 = F.Data.Row<FY01SourceEffectRow>(K::SourceEffect, TEXT("LowAttack")); Low20.ConditionThreshold = 20;
        F.Data.Add(K::SourceEffect, Low20, TEXT("Low20")); Row.EffectIds.Add(F.Data.Ref(K::SourceEffect, TEXT("Low20")));
    }
    if (Case == TEXT("NonAttackRows"))
    { Row.EffectIds = {F.Data.Ref(K::SourceEffect, TEXT("LowArmor")), F.Data.Ref(K::SourceEffect, TEXT("LowRegen")),
        F.Data.Ref(K::SourceEffect, TEXT("LowRate")), F.Data.Ref(K::SourceEffect, TEXT("LowGoldGain"))}; }
    if (Case == TEXT("BaseAttack")) { Row.EffectIds = {F.Data.Ref(K::SourceEffect, TEXT("Attack50"))}; }
    Health(Case.StartsWith(TEXT("Candidate")) ? 150.f : 90.f);
    const auto BeforeEquipHandles = Handles(); int32 AttackNotifications = 0, DerivedApplies = 0;
    const auto AttackHook = ASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).AddLambda(
        [&](const FOnAttributeChangeData&) { ++AttackNotifications; });
    ON_SCOPE_EXIT { ASC->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).Remove(AttackHook); };
    const auto DerivedHook = ASC->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
        [&](UAbilitySystemComponent*, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle)
        { if (Spec.Def && Spec.Def->IsA(UY01EquipmentAttackEffect::StaticClass())) { ++DerivedApplies; } });
    ON_SCOPE_EXIT { ASC->OnGameplayEffectAppliedDelegateToSelf.Remove(DerivedHook); };
    FY01ItemGrant Item{FGuid::NewGuid(), F.Data.Ref(K::Equipment)};
    if (!TestTrue(TEXT("条件装备真实入包"), L->AddItems(FY01DataCatalog(F.Data.Tables), {Item}).IsOk())
        || !TestTrue(TEXT("条件装备真实库存事务穿戴"), F.Send(Item.ItemId))) { return false; }
    const auto Equipped = Snapshot(); const auto EquippedHandles = Handles();
    if (Case.StartsWith(TEXT("Candidate")))
    {
        TestEqual(TEXT("候选600上限使用25%而非旧50%"), F.Attack(), 30.f);
        TestEqual(TEXT("候选穿戴不出现瞬态攻击GE"), AttackNotifications, 0);
        TestTrue(TEXT("候选无需更换攻击句柄"), Handles() == BeforeEquipHandles);
        TestEqual(TEXT("升上限同步加血150+300=450"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 450.f);
        TestEqual(TEXT("真实提交候选上限"), ASC->GetUnitStats().Health, 600.f);
        if (Case == TEXT("CandidateMaxPrecision"))
        {
            TestEqual(TEXT("无效微小百分比不产生新攻击GE"), DerivedApplies, 0);
            const auto V = Attributes(); const auto& Attack = V.Breakdowns[int32(EY01Attribute::AttackDamage) - 1];
            const auto* Inactive = Attack.Contributions.FindByPredicate([](const auto& X) { return X.EffectId == FName(TEXT("Above40")); });
            if (!TestNotNull(TEXT("候选保留微小条件明细"), Inactive)) { return false; }
            TestFalse(TEXT("同步加血激活路径走精度故障，提交明细保持Above40关闭"), Inactive->bActive);
            TestTrue(TEXT("inactive仍精确保留原Amount"), Inactive->Amount == 1.e-8);
            TestEqual(TEXT("inactive不贡献百分比"), Inactive->Contribution, 0.);
            TestEqual(TEXT("攻击百分比合计为零"), Attack.PercentTotal, 0.);
            Tick(); TestFalse(TEXT("候选提交dirty已排空"), S->IsComponentTickEnabled()); return true;
        }
        if (Case == TEXT("CandidateMax"))
        {
            TestTrue(TEXT("Max回调发生在线性化之后不令卸装失败"), F.Send(Item.ItemId, O::Unequip));
            TestEqual(TEXT("卸装后上限"), ASC->GetUnitStats().Health, 300.f);
            TestEqual(TEXT("卸装降上限钳血450到300"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 300.f);
            Tick(); TestFalse(TEXT("提交回调dirty已排空"), S->IsComponentTickEnabled()); return true;
        }
        Health(500);
        const auto StableHandles = Handles(); const auto Stable = Snapshot();
        const auto* Effect = Stable.Effects.FindByPredicate([](const auto& E) { return E.Source.Kind == EY01RuntimeSourceKind::Equipment; });
        if (!TestNotNull(TEXT("保留候选装备身份"), Effect)) { return false; }
        FY01PreparedMutation Plan; FY01SourceTransaction Tx;
        if (!TestTrue(TEXT("准备候选卸装"), S->PrepareMutation({}, {Effect->Source}, Plan).IsOk())
            || !TestTrue(TEXT("暂存候选卸装"), S->StageMutation(Plan, Tx).IsOk())) { return false; }
        TestEqual(TEXT("Stage绝不提前钳血"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 500.f);
        TestTrue(TEXT("候选精确回滚"), S->RollbackMutation(Tx).IsOk());
        TestEqual(TEXT("回滚不损血"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 500.f);
        TestEqual(TEXT("回滚不改上限"), ASC->GetUnitStats().Health, 600.f);
        TestTrue(TEXT("回滚原GE句柄"), Handles() == StableHandles); return true;
    }
    if (Case == TEXT("RefreshRecrossing"))
    {
        TestEqual(TEXT("初始Max300"), ASC->GetUnitStats().Health, 300.f);
        TestEqual(TEXT("初始映射攻击30"), F.Attack(), 30.f);
        if (!TestEqual(TEXT("初始仅一个汇总GE"), EquippedHandles.Num(), 1)) { return false; }
        auto Conditions = [&](const FY01ResolvedAttributes& V, bool Active)
        {
            int32 Count = 0;
            for (const auto& X : V.Breakdowns[int32(EY01Attribute::AttackDamage) - 1].Contributions)
            {
                if (X.Condition != EY01EffectCondition::HealthBelow) { continue; }
                ++Count; TestEqual(TEXT("两条件缓存active状态"), X.bActive, Active);
                TestEqual(TEXT("两条件原Amount100"), X.Amount, 100.);
                TestEqual(TEXT("两条件贡献随active切换"), X.Contribution, Active ? 100. : 0.);
            }
            TestEqual(TEXT("完整保留Below30与Below20两明细"), Count, 2);
        };
        Conditions(Attributes(), false);
        bool Once = false; int32 Applies = 0; FActiveGameplayEffectHandle InterruptedHandle;
        const auto Hook = ASC->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
            [&](UAbilitySystemComponent*, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle Handle)
            {
                if (!Spec.Def || !Spec.Def->IsA(UY01EquipmentAttackEffect::StaticClass())) { return; }
                ++Applies;
                if (Once) { return; } Once = true; InterruptedHandle = Handle;
                TestTrue(TEXT("自动Drain候选应用时仍持有来源事务"), S->IsBusy());
                TestEqual(TEXT("首次候选只开启Below30"), Spec.GetSetByCallerMagnitude(Y01Tags::Data_EquipmentAttack, false, -1), 60.f);
                Health(59);
            });
        ON_SCOPE_EXIT { ASC->OnGameplayEffectAppliedDelegateToSelf.Remove(Hook); };
        // 不吞“属性条件刷新停止”：Interrupted必须保留dirty，旧实现的意外Error应直接使case红。
        Health(89);
        TestTrue(TEXT("汇总GE回调真实再次损血且只执行一次"), Once);
        TestEqual(TEXT("失败候选仅应用一次"), Applies, 1);
        TestEqual(TEXT("Interrupted回滚旧攻击"), F.Attack(), 30.f);
        TestEqual(TEXT("Interrupted不推进来源版本"), Snapshot().Revision, Equipped.Revision);
        TestTrue(TEXT("Interrupted精确保留旧GE句柄"), Handles() == EquippedHandles);
        for (auto Handle : EquippedHandles)
        { const auto* GE = ASC->GetActiveGameplayEffect(Handle); TestTrue(TEXT("旧GE仍有效且未抑制"), GE && !GE->IsPendingRemove && !GE->bIsInhibited); }
        TestTrue(TEXT("失败候选确实是新句柄"), InterruptedHandle.IsValid() && !EquippedHandles.Contains(InterruptedHandle));
        TestNull(TEXT("失败候选GE已移除"), ASC->GetActiveGameplayEffect(InterruptedHandle));
        TestEqual(TEXT("真实Health59绝不还原"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 59.f);
        Conditions(Attributes(), false);
        TestTrue(TEXT("Interrupted保留dirty tick"), S->IsComponentTickEnabled());
        Tick();
        TestEqual(TEXT("下一真实World Tick开启两条件攻击90"), F.Attack(), 90.f);
        TestEqual(TEXT("重试成功仅推进一次来源版本"), Snapshot().Revision, Equipped.Revision + 1);
        TestEqual(TEXT("一次失败候选加一次成功候选"), Applies, 2);
        TestEqual(TEXT("重试后仍只有一个汇总GE"), Handles().Num(), 1);
        TestEqual(TEXT("重试不治疗真实损血"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 59.f);
        Conditions(Attributes(), true);
        TestFalse(TEXT("成功刷新排空tick"), S->IsComponentTickEnabled()); return true;
    }
    if (Case == TEXT("BaseAttack"))
    {
        int32 Applies = 0;
        const auto Hook = ASC->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
            [&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle) { ++Applies; });
        ON_SCOPE_EXIT { ASC->OnGameplayEffectAppliedDelegateToSelf.Remove(Hook); };
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 5);
        TestEqual(TEXT("动态base5参与50%"), F.Attack(), 52.5f);
        TestEqual(TEXT("base5单GEbonus"), Attributes().AttackBonus, 47.5);
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 15);
        TestEqual(TEXT("动态base15参与50%"), F.Attack(), 67.5f);
        TestEqual(TEXT("base15单GEbonus"), Attributes().AttackBonus, 52.5);
        TestEqual(TEXT("每次base变动恰好一次GE，无递归应用"), Applies, 2);
        TestEqual(TEXT("base两次版本更新"), Snapshot().Revision, Equipped.Revision + 2);
        Tick(); TestEqual(TEXT("dirty回调不重复提交"), Snapshot().Revision, Equipped.Revision + 2); return true;
    }
    if (Case == TEXT("NonAttackRows"))
    {
        auto* Target = F.Mode->GetWaveMonsters()[0].Get(); Target->SetActorLocation(F.PC->GetPawn()->GetActorLocation() + FVector(50, 0, 0));
        TestTrue(TEXT("建立真实正在等待的攻击CD"), ASC->TryAutoAttack(Target, [](float) { return true; }));
        const double Deadline = ASC->GetNextAutoAttackTime(); Now += 2; Health(89);
        const auto V = Attributes(); TestEqual(TEXT("无攻击变化仍更新版本"), Snapshot().Revision, Equipped.Revision + 1);
        TestEqual(TEXT("护甲提交"), ASC->GetUnitStats().Armor, 12.f); TestEqual(TEXT("恢复提交"), ASC->GetUnitStats().HealthRegen, 5.f);
        TestEqual(TEXT("速率百分比转换间隔"), ASC->GetUnitStats().AttackInterval, .5f);
        TestEqual(TEXT("金币获得只缓存归一化值"), V.GoldGainPercent, .5);
        TestEqual(TEXT("GoldGain不改变每秒收入"), F.P->GetIncome()->Observe().State.Rate, 0.);
        TestEqual(TEXT("无攻击变化不更换单GE"), F.Attack(), 30.f); TestTrue(TEXT("原攻击句柄"), Handles() == EquippedHandles);
        TestEqual(TEXT("不重置已等待的CD"), ASC->GetNextAutoAttackTime(), Deadline);
        for (const auto Attribute : {EY01Attribute::Armor, EY01Attribute::HealthRegen, EY01Attribute::AttackRate, EY01Attribute::GoldGain})
        {
            const auto& B = V.Breakdowns[int32(Attribute) - 1];
            const auto* E = B.Contributions.FindByPredicate([](const auto& X) { return X.Condition != EY01EffectCondition::None; });
            if (TestNotNull(TEXT("缓存保留非攻击条件行"), E)) { TestTrue(TEXT("缓存条件已激活"), E->bActive); }
        }
        Health(90); TestEqual(TEXT("恢复关闭也更新版本"), Snapshot().Revision, Equipped.Revision + 2);
        TestEqual(TEXT("恢复护甲"), ASC->GetUnitStats().Armor, 5.f); TestEqual(TEXT("恢复间隔"), ASC->GetUnitStats().AttackInterval, 1.f);
        TestEqual(TEXT("恢复GoldGain"), Attributes().GoldGainPercent, 0.); return true;
    }
    if (Case == TEXT("IncomeSegment"))
    {
        FY01ResolvedSource Income; Income.LocalKey = TEXT("ConditionIncome");
        Income.Effects.Add({TEXT("FivePerSecond"), EY01SourceEffectKind::GoldPerSecond, 5});
        FY01PreparedMutation Plan; FY01SourceTransaction Tx;
        if (!TestTrue(TEXT("正收入准备"), S->PrepareMutation({Income}, {}, Plan).IsOk())
            || !TestTrue(TEXT("正收入暂存"), S->StageMutation(Plan, Tx).IsOk())
            || !TestTrue(TEXT("正收入提交"), S->CommitMutation(Tx).IsOk())) { return false; }
        const int32 Gold = F.P->GetGold(); Now += 2; Health(89);
        TestEqual(TEXT("条件刷新兑现完整旧2秒段"), F.P->GetGold() - Gold, 10);
        const double LastTime = F.P->GetIncome()->Observe().State.LastTime;
        Now += 2; Health(80); TestEqual(TEXT("同区间不提前结算"), F.P->GetIncome()->Observe().State.LastTime, LastTime);
        TestTrue(TEXT("新段正常结算"), F.P->GetIncome()->Settle().IsOk());
        TestEqual(TEXT("收入不被条件切换吞掉或重复"), F.P->GetGold() - Gold, 20); return true;
    }
    if (Case == TEXT("PendingMaxMutation"))
    {
        FY01PreparedMutation Plan; FY01SourceTransaction Tx;
        if (!TestTrue(TEXT("外部Max变动前准备"), S->PrepareMutation({}, {}, Plan).IsOk())
            || !TestTrue(TEXT("外部Max变动前暂存"), S->StageMutation(Plan, Tx).IsOk())) { return false; }
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(), 600);
        TestEqual(TEXT("外部Max不能偷换固定候选上限"), S->CommitMutation(Tx).Code, EY01RuntimeError::Interrupted);
        TestEqual(TEXT("失败不丢已提交记录"), Snapshot().Effects.Num(), Equipped.Effects.Num());
        TestTrue(TEXT("失败保留单GE句柄"), Handles() == EquippedHandles);
        TestEqual(TEXT("失败不伪造Max备份恢复"), ASC->GetUnitStats().Health, 600.f);
        TestEqual(TEXT("失败不恢复生命(外部Max升600同步加血90+300=390)"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 390.f);
        // 升上限同步加血后条件不跨界：排空走干净成功路径，不再记条件刷新故障。
        Tick(); TestFalse(TEXT("权威Max中断后单次刷新收尾"), S->IsComponentTickEnabled());
        TestEqual(TEXT("干净刷新攻击不变"), F.Attack(), 30.f); return true;
    }
    if (Case == TEXT("PendingCrossing") || Case == TEXT("PendingSameInterval"))
    {
        if (Case == TEXT("PendingSameInterval")) { Health(89); }
        const auto Stable = Snapshot(); const auto StableHandles = Handles();
        FY01PreparedMutation Plan; FY01SourceTransaction Tx;
        if (!TestTrue(TEXT("空刷新准备"), S->PrepareMutation({}, {}, Plan).IsOk())
            || !TestTrue(TEXT("空刷新暂存"), S->StageMutation(Plan, Tx).IsOk())) { return false; }
        Health(Case == TEXT("PendingCrossing") ? 89.f : 80.f);
        TestTrue(TEXT("通知只标dirty仍持有事务"), S->IsBusy());
        if (Case == TEXT("PendingCrossing"))
        {
            TestEqual(TEXT("提交检测条件向量变化而中断"), S->CommitMutation(Tx).Code, EY01RuntimeError::Interrupted);
            TestEqual(TEXT("失败不丢记录"), Snapshot().Effects.Num(), Stable.Effects.Num());
            TestTrue(TEXT("失败保留精确句柄"), Handles() == StableHandles);
            TestEqual(TEXT("真实外部损血不回滚"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 89.f);
            TestEqual(TEXT("失败尚未改版本"), Snapshot().Revision, Stable.Revision);
            Tick(); TestEqual(TEXT("退事务后tick刷新条件"), F.Attack(), 60.f);
            TestEqual(TEXT("仅刷新版本加一次"), Snapshot().Revision, Stable.Revision + 1);
        }
        else
        {
            TestTrue(TEXT("同区间生命变化允许提交"), S->CommitMutation(Tx).IsOk()); Tick();
            TestEqual(TEXT("同区间无版本变化"), Snapshot().Revision, Stable.Revision);
            TestTrue(TEXT("同区间精确句柄未变"), Handles() == StableHandles);
        }
        return true;
    }
    if (Case == TEXT("StageDelegateInjury"))
    {
        auto& Blue = F.Data.Row<FY01EquipmentRow>(K::Equipment, TEXT("Blue"));
        Blue.EffectIds.Add(F.Data.Ref(K::SourceEffect, TEXT("Ten")));
        FY01ItemGrant Next{FGuid::NewGuid(), F.Data.Ref(K::Equipment, TEXT("Blue"))};
        if (!TestTrue(TEXT("候选装备入包"), L->AddItems(FY01DataCatalog(F.Data.Tables), {Next}).IsOk())) { return false; }
        bool Fired = false;
        const auto Hook = ASC->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
            [&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle)
            { if (!Fired) { Fired = true; Health(89); } });
        TestFalse(TEXT("Stage委托损血跨区间候选换装中止"), F.Send(Next.ItemId));
        ASC->OnGameplayEffectAppliedDelegateToSelf.Remove(Hook);
        TestTrue(TEXT("真实委托已执行"), Fired); TestEqual(TEXT("失败仍穿旧实例"), F.View().EquipmentSlots[0], Item.ItemId);
        TestTrue(TEXT("候选GE回滚恢复原句柄"), Handles() == EquippedHandles);
        TestEqual(TEXT("失败不还原真实损血"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 89.f);
        Tick(); TestEqual(TEXT("旧已提交装备随后刷新"), F.Attack(), 60.f); return true;
    }
    if (Case == TEXT("LoadoutBusy") || Case == TEXT("Ended"))
    {
        if (!TestTrue(TEXT("持有真实库存事务"), FY01R36DeliveryTestAccess::Lock(L))) { return false; }
        Health(89); TestEqual(TEXT("库存Busy延迟不改变攻击"), F.Attack(), 30.f);
        Tick(); TestTrue(TEXT("Busy保留dirty tick"), S->IsComponentTickEnabled());
        if (Case == TEXT("Ended")) { S->EndSession(); }
        FY01R36DeliveryTestAccess::Unlock(L); Tick();
        if (Case == TEXT("Ended"))
        { TestFalse(TEXT("结束关闭dirty tick"), S->IsComponentTickEnabled()); Health(80); TestFalse(TEXT("结束后不重启tick"), S->IsComponentTickEnabled()); }
        else { TestEqual(TEXT("库存退锁后条件生效"), F.Attack(), 60.f); TestFalse(TEXT("成功排空关闭tick"), S->IsComponentTickEnabled()); }
        return true;
    }
    if (Case == TEXT("AutoAttack"))
    {
        auto* Target = F.Mode->GetWaveMonsters()[0].Get(); Target->SetActorLocation(F.PC->GetPawn()->GetActorLocation() + FVector(50, 0, 0));
        TestTrue(TEXT("真实发动上下文"), ASC->TryAutoAttack(Target, [&](float Damage)
        { TestEqual(TEXT("本击冻结30"), Damage, 30.f); Health(89); TestEqual(TEXT("发动内不刷新"), F.Attack(), 30.f); return true; }));
        Tick(); TestEqual(TEXT("发动退出后条件刷新"), F.Attack(), 60.f);
        for (int32 I = 0; I < 22; ++I) { ++GFrameCounter; F.World->Tick(LEVELTICK_All, .05f); }
        TestTrue(TEXT("下击可发动"), ASC->TryAutoAttack(Target, [&](float Damage)
        { TestEqual(TEXT("下击使用提交条件60"), Damage, 60.f); return true; })); return true;
    }
    if (Case == TEXT("TwoPlayers"))
    {
        auto* PC = F.Player(); auto* Other = PC->GetPlayerState<AY01PlayerState>(); FString Error;
        if (!TestTrue(TEXT("第二角色初始化"), Other->TryInitializeCharacter(FY01DataCatalog(F.Data.Tables), F.Data.Ref(K::Character), Error))) { return false; }
        F.Mode->OnPlayerInitialized(Other);
        FY01ItemGrant OtherItem{FGuid::NewGuid(), F.Data.Ref(K::Equipment)};
        if (!TestTrue(TEXT("第二角色独立入包"), Other->GetLoadout()->AddItems(FY01DataCatalog(F.Data.Tables), {OtherItem}).IsOk())) { return false; }
        Other->GetLoadout()->PublishCommittedView(); FY01CommittedView V; Other->GetLoadout()->ReadCommittedView(V);
        FY01ItemRequest R; R.MatchId = V.MatchId; R.JoinSequence = V.JoinSequence; R.Seq = V.ExpectedSeq;
        R.ItemId = OtherItem.ItemId; R.ExpectedEntityRevision = 1; R.Slot = 0; R.ConfigRevision = V.ConfigRevision;
        if (!TestTrue(TEXT("第二角色真实穿戴"), Other->GetLoadout()->SubmitRequest(PC, R).IsOk())) { return false; }
        Health(89); TestEqual(TEXT("A独立激活"), F.Attack(), 60.f);
        TestEqual(TEXT("B仍关闭"), Other->GetCombatASC()->GetUnitStats().AttackDamage, 30.f);
        Other->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 89);
        Health(90); TestEqual(TEXT("A独立恢复关闭"), F.Attack(), 30.f);
        TestEqual(TEXT("B独立激活不随A变化"), Other->GetCombatASC()->GetUnitStats().AttackDamage, 60.f); return true;
    }
    if (Case == TEXT("LifeRestore"))
    {
        const uint64 Session = ASC->GetCombatSession(); Health(0);
        TestTrue(TEXT("死亡仍失效战斗会话"), ASC->GetCombatSession() > Session);
        TestEqual(TEXT("零生命Below激活"), F.Attack(), 60.f);
        Health(300); TestEqual(TEXT("生命恢复重新关闭条件"), F.Attack(), 30.f); return true;
    }
    if (Case == TEXT("RefreshRejected"))
    {
        int32 Queries = 0;
        ASC->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda(
            [&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec& Spec)
            {
                if (Spec.Def && Spec.Def->IsA(UY01EquipmentAttackEffect::StaticClass())) { ++Queries; return false; }
                return true;
            }));
        AddExpectedError(TEXT("属性条件刷新停止"), EAutomationExpectedErrorFlags::Contains, 1);
        Health(89); Tick(); Tick(); ASC->GameplayEffectApplicationQueries.Reset();
        TestEqual(TEXT("非Busy失败不无限重试"), Queries, 1); TestFalse(TEXT("失败关闭自动tick"), S->IsComponentTickEnabled());
        TestEqual(TEXT("失败不改版本"), Snapshot().Revision, Equipped.Revision);
        TestTrue(TEXT("失败保留单GE"), Handles() == EquippedHandles);
        TestTrue(TEXT("技术失败不引入收入Fault"), F.P->GetIncome()->Observe().State.Phase != EY01IncomePhase::Fault); return true;
    }
    const bool Above = Case == TEXT("Above");
    TestEqual(TEXT("30%严格边界条件关闭"), F.Attack(), 30.f);
    Now += 2; Health(Above ? 91.f : 89.f);
    TestEqual(TEXT("越过严格边界激活"), F.Attack(), 60.f);
    TestEqual(TEXT("激活只增加一个版本"), Snapshot().Revision, Equipped.Revision + 1);
    const auto ActiveHandles = Handles(); const auto LastTime = F.P->GetIncome()->Observe().State.LastTime;
    Now += 2; Health(Above ? 100.f : 80.f);
    TestEqual(TEXT("同区间攻击保持"), F.Attack(), 60.f);
    TestEqual(TEXT("同区间不增加版本"), Snapshot().Revision, Equipped.Revision + 1);
    TestTrue(TEXT("同区间不更换句柄"), Handles() == ActiveHandles);
    TestEqual(TEXT("同区间不结算收入"), F.P->GetIncome()->Observe().State.LastTime, LastTime);
    Health(90); TestEqual(TEXT("回到严格边界关闭"), F.Attack(), 30.f);
    TestEqual(TEXT("关闭再增加一个版本"), Snapshot().Revision, Equipped.Revision + 2);
    Tick(); TestFalse(TEXT("空dirty关闭tick"), S->IsComponentTickEnabled()); return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GrowthPercentPrecisionTest, "Y01.Framework.R37.Growth.PercentPrecision",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GrowthPercentPrecisionTest::RunTest(const FString&)
{
    const FString Command = FCommandLine::Get(); FCommandLine::Set(TEXT("")); ON_SCOPE_EXIT { FCommandLine::Set(*Command); };
    TGuardValue<uint64> Frame(GFrameCounter, GFrameCounter);
    constexpr double Delta = 7.e-7;
    FGrowthWorld F(Delta, TEXT("GrowthPercentPrecision"), true);
    if (!TestTrue(TEXT("独立百分比成长配置初始化"), F.Error.IsEmpty())) { AddError(F.Error); return false; }
    auto* ASC = F.P->GetCombatASC(); auto* S = F.P->GetSourceEffects(); auto* L = F.P->GetLoadout();
    auto Base = ASC->GetUnitStats(); Base.AttackDamage = ASC->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute());
    TestEqual(TEXT("基础Strength10"), Base.Strength, 10.f);
    FY01ItemGrant Item{FGuid::NewGuid(), F.Data.Ref(K::Equipment)};
    if (!TestTrue(TEXT("百分比装备真实入包"), L->AddItems(FY01DataCatalog(F.Data.Tables), {Item}).IsOk())
        || !TestTrue(TEXT("百分比装备真实穿戴"), F.Send(Item.ItemId))) { return false; }
    const auto Before = F.View(); FY01SourceSnapshot BeforeSources;
    if (!TestTrue(TEXT("读取百分比已提交来源"), S->Observe(BeforeSources).IsOk())) { return false; }
    TestEqual(TEXT("百分比后有效Strength20"), ASC->GetUnitStats().Strength, 20.f);
    TestEqual(TEXT("唯一必要映射产生攻击60"), F.Attack(), 60.f);
    TestEqual(TEXT("不混入收入mapping"), F.P->GetIncome()->Observe().State.Rate, 0.);
    if (!TestEqual(TEXT("装备仅持有玩家成长rule"), Before.Items[0].Growth.Num(), 1)) { return false; }
    TestEqual(TEXT("装备不含实例攻击成长"), Before.Items[0].Growth[0].Target, EY01GrowthTarget::Player);
    TestTrue(TEXT("配表精确保留raw Delta"), Before.Items[0].Growth[0].Delta == Delta);
    FY01ResolvedAttributes Expected;
    if (!TestTrue(TEXT("Resolver最终公式可表示"), FY01AttributeResolver::Resolve(Base, Delta, BeforeSources.Effects, Expected).IsOk())) { return false; }
    TestTrue(TEXT("最终公式先加raw再乘百分比"), Expected.Breakdowns[int32(EY01Attribute::Strength) - 1].Final == (10. + Delta) * 2.);
    TestTrue(TEXT("最终有效Strength确实上升"), Expected.Stats.Strength > 20.f);
    const auto BeforeHandles = ASC->GetActiveEffects(FGameplayEffectQuery());
    if (!TestEqual(TEXT("成长前单汇总GE"), BeforeHandles.Num(), 1)) { return false; }
    int32 Applied = 0;
    const auto Hook = ASC->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
        [&](UAbilitySystemComponent*, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle)
        { if (Spec.Def && Spec.Def->IsA(UY01EquipmentAttackEffect::StaticClass())) { ++Applied; } });
    ON_SCOPE_EXIT { ASC->OnGameplayEffectAppliedDelegateToSelf.Remove(Hook); };
    if (!TestTrue(TEXT("真实有效击杀触发微小玩家成长"), F.Hit(F.Mode->GetWaveMonsters()[0].Get()))) { return false; }
    const auto After = F.View(); FY01ResolvedAttributes Actual; FY01SourceSnapshot AfterSources;
    TestEqual(TEXT("合法最终公式不得产生GrowthFault"), After.Growth.GrowthFault, EY01ItemError::None);
    TestTrue(TEXT("raw力量账本精确累计"), After.Growth.Strength == Delta);
    TestEqual(TEXT("真实有效击杀计数一次"), After.Growth.AppliedKills, uint64(1));
    if (TestEqual(TEXT("仅一条玩家成长账本"), After.Growth.PlayerRules.Num(), 1))
    {
        TestTrue(TEXT("raw规则精确累计未乘百分比"), After.Growth.PlayerRules[0].Rule.Accumulated == Delta);
        TestEqual(TEXT("raw规则计数一次"), After.Growth.PlayerRules[0].Rule.AppliedCount, uint64(1));
    }
    TestTrue(TEXT("真实Strength精确对应Resolver"), ASC->GetUnitStats().Strength == Expected.Stats.Strength);
    TestTrue(TEXT("真实攻击精确对应Resolver"), F.Attack() == Expected.Stats.AttackDamage);
    TestTrue(TEXT("视图Strength同次精确提交"), After.Stats.Strength == Expected.Stats.Strength);
    TestEqual(TEXT("成长库存版本只推进一次"), After.Revision, Before.Revision + 1);
    if (!TestTrue(TEXT("读取成长后缓存"), S->ObserveAttributes(Actual).IsOk())
        || !TestTrue(TEXT("读取成长后来源"), S->Observe(AfterSources).IsOk())) { return false; }
    TestEqual(TEXT("成长来源版本只推进一次"), AfterSources.Revision, BeforeSources.Revision + 1);
    TestTrue(TEXT("攻击bonus精确对应Resolver"), Actual.AttackBonus == Expected.AttackBonus);
    if (TestEqual(TEXT("完整明细行数精确对应Resolver"), Actual.Breakdowns.Num(), Expected.Breakdowns.Num()))
    {
        for (int32 I = 0; I < Actual.Breakdowns.Num(); ++I)
        { TestTrue(TEXT("逐行全部明细精确对应Resolver"), FY01AttributeBreakdown::StaticStruct()->CompareScriptStruct(&Actual.Breakdowns[I], &Expected.Breakdowns[I], 0)); }
    }
    TestEqual(TEXT("一次成长仅应用一个候选汇总GE"), Applied, 1);
    const auto Handles = ASC->GetActiveEffects(FGameplayEffectQuery());
    if (!TestEqual(TEXT("成长后仍为单GE"), Handles.Num(), 1)) { return false; }
    const auto* GE = ASC->GetActiveGameplayEffect(Handles[0]);
    if (!TestNotNull(TEXT("最终攻击GE真实有效"), GE)) { return false; }
    TestTrue(TEXT("唯一GE数值精确对应Resolver"), GE->Spec.GetSetByCallerMagnitude(Y01Tags::Data_EquipmentAttack, false, -1) == static_cast<float>(Expected.AttackBonus));
    return true;
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
        TestEqual(TEXT("百分比种类追加值4"), int32(EY01SourceEffectKind::AttributePercent), 4);
        TestEqual(TEXT("GoldGain追加在AttackRate之后"), int32(EY01Attribute::GoldGain), int32(EY01Attribute::AttackRate) + 1);
        TestFalse(TEXT("GoldGain不属于映射衍生属性"), Y01Data::IsDerivedAttribute(EY01Attribute::GoldGain));
        E = {}; E.Kind = EY01SourceEffectKind::AttributePercent; E.Amount = 5;
        for (const auto Target : {EY01Attribute::Strength, EY01Attribute::Agility, EY01Attribute::Wisdom,
            EY01Attribute::AttackDamage, EY01Attribute::MaxHealth, EY01Attribute::Armor, EY01Attribute::HealthRegen,
            EY01Attribute::AttackRate, EY01Attribute::GoldGain})
        { E.TargetAttribute = Target; TestTrue(TEXT("三维及非收入衍生和GoldGain百分比合法"), Y01Data::ValidSourceEffect(E)); }
        E.TargetAttribute = EY01Attribute::GoldPerSecond;
        TestFalse(TEXT("金币每秒百分比拒绝"), Y01Data::ValidSourceEffect(E));
        for (const auto Kind : {EY01SourceEffectKind::AttributeFlat, EY01SourceEffectKind::AttributePercent})
        {
            E = {}; E.Kind = Kind; E.Amount = 5;
            for (const auto Condition : {EY01EffectCondition::HealthBelow, EY01EffectCondition::HealthAbove})
            {
                E.Condition = Condition; E.ConditionThreshold = 50;
                for (const auto Target : {EY01Attribute::Strength, EY01Attribute::Agility, EY01Attribute::Wisdom,
                    EY01Attribute::MaxHealth, EY01Attribute::GoldPerSecond})
                { E.TargetAttribute = Target; TestFalse(TEXT("条件三维、生命上限及收入拒绝"), Y01Data::ValidSourceEffect(E)); }
                for (const auto Target : {EY01Attribute::AttackDamage, EY01Attribute::Armor, EY01Attribute::HealthRegen,
                    EY01Attribute::AttackRate, EY01Attribute::GoldGain})
                {
                    E.TargetAttribute = Target;
                    TestEqual(TEXT("条件只接五个目标且GoldGain仅限百分比"), Y01Data::ValidSourceEffect(E),
                        Target != EY01Attribute::GoldGain || Kind == EY01SourceEffectKind::AttributePercent);
                }
                E.TargetAttribute = EY01Attribute::AttackDamage;
                for (double Threshold : {0., 50., 100.})
                { E.ConditionThreshold = Threshold; TestTrue(TEXT("条件阈值闭区间合法"), Y01Data::ValidSourceEffect(E)); }
                for (double Threshold : {101., -1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
                { E.ConditionThreshold = Threshold; TestFalse(TEXT("越界及非有限条件阈值拒绝"), Y01Data::ValidSourceEffect(E)); }
            }
        }
        E = {}; E.Kind = EY01SourceEffectKind::AttributePercent; E.TargetAttribute = EY01Attribute::AttackDamage; E.Amount = 5;
        E.ConditionThreshold = 1; TestFalse(TEXT("无条件必须零阈值"), Y01Data::ValidSourceEffect(E));
        E.ConditionThreshold = 0; TestTrue(TEXT("无条件零阈值合法"), Y01Data::ValidSourceEffect(E));
        E.Condition = static_cast<EY01EffectCondition>(255); TestFalse(TEXT("未知条件枚举拒绝"), Y01Data::ValidSourceEffect(E));
        E.Condition = EY01EffectCondition::None;
        for (double Amount : {-1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
        { E.Amount = Amount; TestFalse(TEXT("负数及非有限Amount拒绝"), Y01Data::ValidSourceEffect(E)); }
        E.Amount = 5; E.Kind = static_cast<EY01SourceEffectKind>(255);
        TestFalse(TEXT("未知效果枚举拒绝"), Y01Data::ValidSourceEffect(E));
        E.Kind = EY01SourceEffectKind::AttributePercent; E.TargetAttribute = static_cast<EY01Attribute>(255);
        TestFalse(TEXT("百分比未知目标拒绝"), Y01Data::ValidSourceEffect(E));
        E.TargetAttribute = EY01Attribute::None; TestFalse(TEXT("百分比空目标拒绝"), Y01Data::ValidSourceEffect(E));
        E.TargetAttribute = EY01Attribute::AttackDamage; E.SourceAttribute = static_cast<EY01Attribute>(255);
        TestFalse(TEXT("百分比未知来源拒绝"), Y01Data::ValidSourceEffect(E));
        E.SourceAttribute = EY01Attribute::Strength; TestFalse(TEXT("百分比不能配置来源属性"), Y01Data::ValidSourceEffect(E));
        E.SourceAttribute = EY01Attribute::None;
        for (double Magnitude : {1., -1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
        { E.Magnitude = Magnitude; TestFalse(TEXT("百分比Magnitude必须零"), Y01Data::ValidSourceEffect(E)); }
        E = {}; E.Kind = EY01SourceEffectKind::AttributeFlat; E.TargetAttribute = EY01Attribute::GoldGain; E.Amount = 5;
        TestFalse(TEXT("GoldGain固定值拒绝"), Y01Data::ValidSourceEffect(E));
        E.Kind = EY01SourceEffectKind::MappingCoefficientAdd; E.SourceAttribute = EY01Attribute::Strength;
        TestFalse(TEXT("GoldGain映射拒绝"), Y01Data::ValidSourceEffect(E));
        E.TargetAttribute = EY01Attribute::Armor; E.Condition = EY01EffectCondition::HealthBelow;
        TestFalse(TEXT("条件映射拒绝"), Y01Data::ValidSourceEffect(E));
        for (const auto Kind : {EY01SourceEffectKind::AttackFlat, EY01SourceEffectKind::GoldPerSecond})
        {
            E = {}; E.Kind = Kind; E.Magnitude = 5;
            TestTrue(TEXT("旧Magnitude载荷仍合法"), Y01Data::ValidSourceEffect(E));
            E.Condition = EY01EffectCondition::HealthBelow; TestFalse(TEXT("旧效果不能带生命条件"), Y01Data::ValidSourceEffect(E));
            E.Condition = EY01EffectCondition::None; E.Amount = 1; TestFalse(TEXT("旧效果Amount必须零"), Y01Data::ValidSourceEffect(E));
            E.Amount = 0; E.SourceAttribute = EY01Attribute::Strength; TestFalse(TEXT("旧效果来源必须空"), Y01Data::ValidSourceEffect(E));
            E.SourceAttribute = EY01Attribute::None; E.TargetAttribute = EY01Attribute::AttackDamage;
            TestFalse(TEXT("旧效果目标必须空"), Y01Data::ValidSourceEffect(E));
            E.TargetAttribute = EY01Attribute::None;
            for (double Magnitude : {-1., std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()})
            { E.Magnitude = Magnitude; TestFalse(TEXT("旧效果Magnitude有限非负"), Y01Data::ValidSourceEffect(E)); }
        }
        FY01MatchFixture ContractData;
        E = {}; E.Kind = EY01SourceEffectKind::AttributePercent; E.TargetAttribute = EY01Attribute::GoldGain; E.Amount = 5;
        ContractData.Add(K::SourceEffect, E);
        TestTrue(TEXT("目录接受合法GoldGain百分比行"), FY01DataCatalog(ContractData.Tables).Validate().IsValid());
        E.Condition = static_cast<EY01EffectCondition>(255); ContractData.Add(K::SourceEffect, E);
        TestTrue(TEXT("目录明确报告条件枚举错误"), FY01DataCatalog(ContractData.Tables).Validate().Errors.ContainsByPredicate([](const auto& Issue)
        { return Issue.Field == TEXT("Condition") && Issue.Code == EY01DataErrorCode::InvalidEnum; }));
        E.Condition = EY01EffectCondition::HealthBelow; E.ConditionThreshold = std::numeric_limits<double>::quiet_NaN();
        ContractData.Add(K::SourceEffect, E);
        TestTrue(TEXT("目录明确报告阈值数值错误"), FY01DataCatalog(ContractData.Tables).Validate().Errors.ContainsByPredicate([](const auto& Issue)
        { return Issue.Field == TEXT("ConditionThreshold") && Issue.Code == EY01DataErrorCode::InvalidNumber; }));
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
        TestEqual(TEXT("升上限同步加血300+100=400"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 400.f);
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
    FY01ItemGrant A{FGuid::NewGuid(), F.Data.Ref(K::Equipment)};
    FY01ItemGrant B{FGuid::NewGuid(), F.Data.Ref(K::Equipment, Case == TEXT("SourceStagingFrozen") ? TEXT("Blue") : TEXT("Default"))};
    if (!TestTrue(TEXT("两件装备独立入包"), F.P->GetLoadout()->AddItems(FY01DataCatalog(F.Data.Tables), {A, B}).IsOk())
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
        if (Case == TEXT("UIFault"))
        {
            auto* Status = Cast<UTextBlock>(Panel->WidgetTree->FindWidget(TEXT("Status")));
            if (!TestNotNull(TEXT("真实状态控件"), Status)) { return false; }
            TestTrue(TEXT("UI明确显示成长故障"), Status->GetText().ToString().Contains(TEXT("成长故障")));
        }
        else
        {
            auto* Strength = Cast<UTextBlock>(Panel->WidgetTree->FindWidget(TEXT("StatValue_Strength")));
            if (!TestNotNull(TEXT("真实力量控件"), Strength)) { return false; }
            TestEqual(TEXT("UI显示真实力量"), Strength->GetText().ToString(), FString(TEXT("1")));
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
        // R37第三期起故障配对单向：Tick故障无事件身份合法；反向"有身份无故障"仍拒绝。
        { auto Ticked = Good; Ticked.Growth.GrowthFault = EY01ItemError::Precision;
          FY01ViewSender TickSender; FY01ViewWire TickWire; FString TickError;
          TestTrue(TEXT("Tick故障无事件身份合法"), TickSender.Offer(Ticked, TickWire, TickError)); }
        Reject(TEXT("事件身份不得无故障"), [](auto& V) { V.Growth.FaultKillEventId = FGuid::NewGuid(); });
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
            [&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { ++Calls; return false; })); }
        TestTrue(TEXT("成长故障不阻止真实致死"), F.Hit(Targets[0])); ASC->GameplayEffectApplicationQueries.Reset();
        if (Case == TEXT("FaultGE")) { TestEqual(TEXT("成长汇总GE只查询一次"), Calls, 1); }
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GrowthTalentBasicAttackTest, "Y01.R37.Growth.TalentBasicAttack",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GrowthTalentBasicAttackTest::RunTest(const FString&)
{
    const FString Command = FCommandLine::Get(); FCommandLine::Set(TEXT("")); ON_SCOPE_EXIT { FCommandLine::Set(*Command); };
    TGuardValue<uint64> Frame(GFrameCounter, GFrameCounter);
    FGrowthWorld F(1, TEXT("TalentBasicAttack"), true);
    if (!TestTrue(TEXT("天赋普攻夹具初始化"), F.Error.IsEmpty())) { AddError(F.Error); return false; }
    if (!TestEqual(TEXT("已进入战斗阶段"), F.Mode->GetFlowState()->GetWaveSnapshot().Phase, EY01MatchPhase::Battle)) { return false; }
    auto* ASC = F.P->GetCombatASC();
    double Damage = 0;
    auto Run = [&](float Seconds)
    {
        for (float T = 0; T < Seconds; T += .05f)
        {
            float Before = 0;
            for (const auto& M : F.Mode->GetWaveMonsters()) { if (IsValid(M)) { Before += M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()); } }
            ++GFrameCounter; F.World->Tick(LEVELTICK_All, .05f);
            float After = 0;
            for (const auto& M : F.Mode->GetWaveMonsters()) { if (IsValid(M)) { After += M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()); } }
            Damage += FMath::Max(0.f, Before - After);
        }
    };
    Run(1.5f);
    TestTrue(TEXT("未选天赋时真实普攻已造成伤害"), Damage > 0);
    TestEqual(TEXT("未选天赋敏捷不变"), ASC->GetUnitStats().Agility, 10.f);
    auto R = F.Request({}, O::ChooseTalent); R.Slot = INDEX_NONE; R.ExpectedSlotItemId = {}; R.ExpectedSlotEntityRevision = 0;
    R.TalentChoiceId = F.View().TalentChoice.ChoiceId; R.Talent = F.Data.Ref(K::Talent);
    const auto Chosen = F.P->GetLoadout()->SubmitRequest(F.PC, R);
    if (!TestTrue(TEXT("真实选择游侠天赋"), Chosen.IsOk())) { AddError(Chosen.Message); return false; }
    Damage = 0; Run(3.f);
    const auto V = F.View();
    AddInfo(FString::Printf(TEXT("选后伤害=%g 敏捷=%g 账本敏捷=%g 规则数=%d 故障=%d %s"), Damage, ASC->GetUnitStats().Agility,
        V.Growth.Agility, V.Growth.PlayerRules.Num(), int32(V.Growth.GrowthFault), *V.Growth.FaultMessage));
    TestTrue(TEXT("选后真实普攻继续"), Damage > 0);
    TestTrue(TEXT("天赋普攻敏捷成长生效"), ASC->GetUnitStats().Agility > 10.f);
    TestTrue(TEXT("账本记录天赋普攻规则"), V.Growth.PlayerRules.ContainsByPredicate([](const auto& E) { return E.Rule.RuleId == TEXT("AgiAttack") && E.Rule.Accumulated > 0; }));
    TestEqual(TEXT("视图敏捷与权威一致"), V.Stats.Agility, ASC->GetUnitStats().Agility);
    return true;
}
#endif
