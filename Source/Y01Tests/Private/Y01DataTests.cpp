#include "Misc/AutomationTest.h"
#include "Data/Y01DataCatalog.h"
#include "Data/Y01DataSubsystem.h"
#include "Engine/GameInstance.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include <limits>
#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FY01DataRef Ref(EY01TableKind Kind, FName Name = TEXT("Default"))
{
    FY01DataRef Result;
    Result.Type = Kind;
    Result.RowName = Name;
    return Result;
}

struct FDataFixture
{
    TArray<TStrongObjectPtr<UDataTable>> Owners;
    FY01TableMap Tables;

    template <typename T>
    UDataTable* Add(EY01TableKind Kind, const T& Row = T(), FName Name = TEXT("Default"))
    {
        UDataTable* Table = NewObject<UDataTable>();
        Table->RowStruct = T::StaticStruct();
        Table->AddRow(Name, Row);
        Owners.Emplace(Table);
        Tables.Add(Kind, Table);
        return Table;
    }

    template <typename T>
    T& Row(EY01TableKind Kind, FName Name = TEXT("Default"))
    {
        return *const_cast<UDataTable*>(Tables[Kind])->FindRow<T>(Name, TEXT("Test"));
    }

    FDataFixture()
    {
        FY01QualityRow Quality;
        UDataTable* Qualities = Add(EY01TableKind::Quality, Quality, TEXT("Green"));
        const TCHAR* Names[] = { TEXT("Green"), TEXT("Blue"), TEXT("Purple"), TEXT("Gold"), TEXT("Red") };
        for (int32 Index = 1; Index < 5; ++Index)
        {
            Quality.Quality = static_cast<EY01Quality>(Index);
            Qualities->AddRow(FName(Names[Index]), Quality);
        }
        Add<FY01SkillRow>(EY01TableKind::Skill);
        Add<FY01TalentRow>(EY01TableKind::Talent);
        UDataTable* Equipment = Add<FY01EquipmentRow>(EY01TableKind::Equipment);
        FY01EquipmentRow BlueEquipment;
        BlueEquipment.Quality = EY01Quality::Blue;
        Equipment->AddRow(TEXT("Blue"), BlueEquipment);
        Add<FY01MetaItemRow>(EY01TableKind::MetaItem);
        FY01WeightedEntry Entry;
        Entry.Item = Ref(EY01TableKind::Equipment);
        FY01LocalItemPoolRow LocalPool;
        LocalPool.Entries.Add(Entry);
        UDataTable* LocalPools = Add(EY01TableKind::LocalItemPool, LocalPool);
        LocalPool.Quality = EY01Quality::Blue;
        LocalPool.Entries[0].Item = Ref(EY01TableKind::Equipment, TEXT("Blue"));
        LocalPools->AddRow(TEXT("Blue"), LocalPool);
        FY01MetaLootPoolRow MetaPool;
        Entry.Item = Ref(EY01TableKind::MetaItem);
        MetaPool.Entries.Add(Entry);
        Add(EY01TableKind::MetaLootPool, MetaPool);
        FY01RewardPackageRow Reward;
        FY01RewardEntry RewardEntry;
        RewardEntry.Item = Ref(EY01TableKind::Equipment);
        Reward.Items.Add(RewardEntry);
        Add(EY01TableKind::RewardPackage, Reward);
        FY01MonsterRow Monster;
        Monster.Skills.Add(Ref(EY01TableKind::Skill));
        Monster.DropReward = Ref(EY01TableKind::RewardPackage);
        Add(EY01TableKind::Monster, Monster);
        FY01SpawnEntry Spawn;
        Spawn.Monster = Ref(EY01TableKind::Monster);
        FY01WaveRow Wave;
        Wave.Spawns.Add(Spawn);
        Wave.Reward = Ref(EY01TableKind::RewardPackage);
        UDataTable* Waves = Add(EY01TableKind::Wave, Wave);
        Wave.Type = EY01WaveType::Boss;
        Wave.PreparationSeconds = 20.0f;
        Waves->AddRow(TEXT("Boss"), Wave);
        FY01TowerFloorRow Floor;
        Floor.Spawns.Add(Spawn);
        Floor.Reward = Ref(EY01TableKind::RewardPackage);
        Add(EY01TableKind::TowerFloor, Floor);
        FY01TrainingStageRow Training;
        Training.Spawns.Add(Spawn);
        Training.Reward = Ref(EY01TableKind::RewardPackage);
        Add(EY01TableKind::TrainingStage, Training);
        FY01LevelExperienceRow Level;
        UDataTable* Levels = Add(EY01TableKind::LevelExperience, Level);
        Level.Level = 2;
        Level.RequiredExperience = 200;
        Levels->AddRow(TEXT("Level2"), Level);
        FY01MatchRulesRow Rules;
        Rules.MaxLevel = 2;
        Add(EY01TableKind::MatchRules, Rules);
        FY01CharacterRow Character;
        Character.InitialEquipment.Add(Ref(EY01TableKind::Equipment));
        Character.InitialSkills.Add(Ref(EY01TableKind::Skill));
        Add(EY01TableKind::Character, Character);
        FY01ShopOfferRow Offer;
        Offer.Pool = Ref(EY01TableKind::LocalItemPool);
        Add(EY01TableKind::ShopOffer, Offer);
        FY01SynthesisRow Synthesis;
        Synthesis.OutputPool = Ref(EY01TableKind::LocalItemPool, TEXT("Blue"));
        Add(EY01TableKind::Synthesis, Synthesis);
        FY01WarehouseEventRow Event;
        Event.UpgradeCount = 1;
        Add(EY01TableKind::WarehouseEvent, Event);
        FY01TalentChoiceRow Choice;
        Choice.Candidates.Init(Ref(EY01TableKind::Talent), 3);
        Add(EY01TableKind::TalentChoice, Choice);
    }
};

bool HasError(const FY01DataValidation& Result, EY01DataErrorCode Code)
{
    return Result.Errors.ContainsByPredicate([Code](const FY01DataError& Error)
    {
        return Error.Code == Code && !Error.Message.IsEmpty() && !Error.Field.IsEmpty();
    });
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DataValidationTest, "Y01.Data.Validation",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

void FY01DataValidationTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    const TCHAR* Cases[] = {
        TEXT("Valid"), TEXT("MissingTable"), TEXT("WrongSchema"), TEXT("EmptyTable"),
        TEXT("NegativePrice"), TEXT("NegativeCooldown"), TEXT("NonFiniteStats"), TEXT("NegativeStats"),
        TEXT("ZeroQuantity"), TEXT("InvalidReference"), TEXT("WrongReferenceType"), TEXT("EmptyReference"),
        TEXT("EmptyPool"), TEXT("NegativeWeight"), TEXT("NonFiniteWeight"), TEXT("ZeroPool"),
        TEXT("PoolQualityMismatch"), TEXT("PoolTypeMismatch"), TEXT("MissingLevel"), TEXT("DuplicateLevel"),
        TEXT("MissingQuality"), TEXT("InvalidQuality"), TEXT("BossPreparation"), TEXT("EmptySpawns"),
        TEXT("SpawnQuantity"), TEXT("SpawnDelay"), TEXT("RewardDomain"), TEXT("RewardQuantity"),
        TEXT("SynthesisCount"), TEXT("SynthesisRed"), TEXT("SynthesisQuality"), TEXT("SynthesisType"),
        TEXT("TalentCandidates"), TEXT("TalentRefresh"), TEXT("EquipmentSlots"), TEXT("SkillSlots"),
        TEXT("UniversalEquipment"), TEXT("InvalidEquipmentReference"), TEXT("InvalidEnum"), TEXT("TowerReference"),
        TEXT("TrainingQuantity"), TEXT("WarehouseUpgradeCount"), TEXT("CharacterStats"), TEXT("NegativeExperience")
    };
    for (const TCHAR* Name : Cases) { Names.Add(Name); Commands.Add(Name); }
}

bool FY01DataValidationTest::RunTest(const FString& Case)
{
    FDataFixture F;
    EY01DataErrorCode Expected = EY01DataErrorCode::InvalidNumber;
    if (Case == TEXT("Valid"))
    {
        TestTrue(TEXT("完整内存配表必须通过"), FY01DataCatalog(F.Tables).Validate().IsValid());
        TestEqual(TEXT("装备六槽"), Y01Data::EquipmentSlotCount, 6);
        TestEqual(TEXT("技能四槽"), Y01Data::SkillSlotCount, 4);
        return true;
    }
    if (Case == TEXT("MissingTable")) { F.Tables.Remove(EY01TableKind::Monster); Expected = EY01DataErrorCode::MissingTable; }
    else if (Case == TEXT("WrongSchema")) { F.Tables[EY01TableKind::Monster] = F.Tables[EY01TableKind::Skill]; Expected = EY01DataErrorCode::WrongSchema; }
    else if (Case == TEXT("EmptyTable")) { const_cast<UDataTable*>(F.Tables[EY01TableKind::ShopOffer])->EmptyTable(); Expected = EY01DataErrorCode::EmptyTable; }
    else if (Case == TEXT("NegativePrice")) { F.Row<FY01ShopOfferRow>(EY01TableKind::ShopOffer).Price = -1; }
    else if (Case == TEXT("NegativeCooldown")) { F.Row<FY01SkillRow>(EY01TableKind::Skill).CooldownSeconds = -1; }
    else if (Case == TEXT("NonFiniteStats")) { F.Row<FY01MonsterRow>(EY01TableKind::Monster).Health = std::numeric_limits<float>::infinity(); }
    else if (Case == TEXT("NegativeStats")) { F.Row<FY01MonsterRow>(EY01TableKind::Monster).AttackDamage = -1; }
    else if (Case == TEXT("ZeroQuantity")) { F.Row<FY01ShopOfferRow>(EY01TableKind::ShopOffer).Quantity = 0; }
    else if (Case == TEXT("InvalidReference")) { F.Row<FY01MonsterRow>(EY01TableKind::Monster).Skills[0].RowName = TEXT("Missing"); Expected = EY01DataErrorCode::InvalidReference; }
    else if (Case == TEXT("WrongReferenceType")) { F.Row<FY01MonsterRow>(EY01TableKind::Monster).Skills[0].Type = EY01TableKind::Equipment; Expected = EY01DataErrorCode::TypeMismatch; }
    else if (Case == TEXT("EmptyReference")) { F.Row<FY01WaveRow>(EY01TableKind::Wave).Reward = FY01DataRef(); Expected = EY01DataErrorCode::InvalidReference; }
    else if (Case == TEXT("EmptyPool")) { F.Row<FY01LocalItemPoolRow>(EY01TableKind::LocalItemPool).Entries.Empty(); Expected = EY01DataErrorCode::InvalidPool; }
    else if (Case == TEXT("NegativeWeight")) { F.Row<FY01LocalItemPoolRow>(EY01TableKind::LocalItemPool).Entries[0].Weight = -1; }
    else if (Case == TEXT("NonFiniteWeight")) { F.Row<FY01MetaLootPoolRow>(EY01TableKind::MetaLootPool).Entries[0].Weight = std::numeric_limits<float>::quiet_NaN(); }
    else if (Case == TEXT("ZeroPool")) { F.Row<FY01MetaLootPoolRow>(EY01TableKind::MetaLootPool).Entries[0].Weight = 0; Expected = EY01DataErrorCode::InvalidPool; }
    else if (Case == TEXT("PoolQualityMismatch")) { F.Row<FY01LocalItemPoolRow>(EY01TableKind::LocalItemPool).Quality = EY01Quality::Red; Expected = EY01DataErrorCode::QualityMismatch; }
    else if (Case == TEXT("PoolTypeMismatch")) { F.Row<FY01LocalItemPoolRow>(EY01TableKind::LocalItemPool).Entries[0].Item = Ref(EY01TableKind::Skill); Expected = EY01DataErrorCode::TypeMismatch; }
    else if (Case == TEXT("MissingLevel")) { const_cast<UDataTable*>(F.Tables[EY01TableKind::LevelExperience])->RemoveRow(TEXT("Level2")); Expected = EY01DataErrorCode::MissingLevel; }
    else if (Case == TEXT("DuplicateLevel")) { F.Row<FY01LevelExperienceRow>(EY01TableKind::LevelExperience, TEXT("Level2")).Level = 1; Expected = EY01DataErrorCode::DuplicateValue; }
    else if (Case == TEXT("MissingQuality")) { const_cast<UDataTable*>(F.Tables[EY01TableKind::Quality])->RemoveRow(TEXT("Red")); Expected = EY01DataErrorCode::MissingQuality; }
    else if (Case == TEXT("InvalidQuality")) { F.Row<FY01EquipmentRow>(EY01TableKind::Equipment).Quality = static_cast<EY01Quality>(255); Expected = EY01DataErrorCode::InvalidEnum; }
    else if (Case == TEXT("BossPreparation")) { F.Row<FY01WaveRow>(EY01TableKind::Wave, TEXT("Boss")).PreparationSeconds = 10; Expected = EY01DataErrorCode::BossPreparation; }
    else if (Case == TEXT("EmptySpawns")) { F.Row<FY01WaveRow>(EY01TableKind::Wave).Spawns.Empty(); Expected = EY01DataErrorCode::InvalidPool; }
    else if (Case == TEXT("SpawnQuantity")) { F.Row<FY01WaveRow>(EY01TableKind::Wave).Spawns[0].Count = 0; }
    else if (Case == TEXT("SpawnDelay")) { F.Row<FY01WaveRow>(EY01TableKind::Wave).Spawns[0].DelaySeconds = -1; }
    else if (Case == TEXT("RewardDomain")) { F.Row<FY01RewardPackageRow>(EY01TableKind::RewardPackage).Domain = EY01RewardDomain::MetaPublic; Expected = EY01DataErrorCode::TypeMismatch; }
    else if (Case == TEXT("RewardQuantity")) { F.Row<FY01RewardPackageRow>(EY01TableKind::RewardPackage).Items[0].Quantity = 0; }
    else if (Case == TEXT("SynthesisCount")) { F.Row<FY01SynthesisRow>(EY01TableKind::Synthesis).InputCount = 4; Expected = EY01DataErrorCode::Invariant; }
    else if (Case == TEXT("SynthesisRed")) { F.Row<FY01SynthesisRow>(EY01TableKind::Synthesis).InputQuality = EY01Quality::Red; Expected = EY01DataErrorCode::Invariant; }
    else if (Case == TEXT("SynthesisQuality")) { F.Row<FY01SynthesisRow>(EY01TableKind::Synthesis).OutputPool.RowName = TEXT("Default"); Expected = EY01DataErrorCode::QualityMismatch; }
    else if (Case == TEXT("SynthesisType")) { F.Row<FY01SynthesisRow>(EY01TableKind::Synthesis).OutputPool = Ref(EY01TableKind::Skill); Expected = EY01DataErrorCode::TypeMismatch; }
    else if (Case == TEXT("TalentCandidates")) { F.Row<FY01TalentChoiceRow>(EY01TableKind::TalentChoice).Candidates.Pop(); Expected = EY01DataErrorCode::Invariant; }
    else if (Case == TEXT("TalentRefresh")) { F.Row<FY01TalentChoiceRow>(EY01TableKind::TalentChoice).RefreshesPerCandidate = 2; Expected = EY01DataErrorCode::Invariant; }
    else if (Case == TEXT("EquipmentSlots")) { F.Row<FY01CharacterRow>(EY01TableKind::Character).InitialEquipment.Init(Ref(EY01TableKind::Equipment), 7); Expected = EY01DataErrorCode::Invariant; }
    else if (Case == TEXT("SkillSlots")) { F.Row<FY01CharacterRow>(EY01TableKind::Character).InitialSkills.Init(Ref(EY01TableKind::Skill), 5); Expected = EY01DataErrorCode::Invariant; }
    else if (Case == TEXT("UniversalEquipment"))
    {
        F.Row<FY01CharacterRow>(EY01TableKind::Character).InitialEquipment.Add(Ref(EY01TableKind::Equipment, TEXT("Blue")));
        TestTrue(TEXT("两件不同装备按数组索引占据通用槽，不应互斥"), FY01DataCatalog(F.Tables).Validate().IsValid());
        return true;
    }
    else if (Case == TEXT("InvalidEquipmentReference")) { F.Row<FY01CharacterRow>(EY01TableKind::Character).InitialEquipment[0] = Ref(EY01TableKind::Skill); Expected = EY01DataErrorCode::TypeMismatch; }
    else if (Case == TEXT("InvalidEnum")) { F.Row<FY01SkillRow>(EY01TableKind::Skill).Type = static_cast<EY01SkillType>(255); Expected = EY01DataErrorCode::InvalidEnum; }
    else if (Case == TEXT("TowerReference")) { F.Row<FY01TowerFloorRow>(EY01TableKind::TowerFloor).Spawns[0].Monster.RowName = TEXT("Missing"); Expected = EY01DataErrorCode::InvalidReference; }
    else if (Case == TEXT("TrainingQuantity")) { F.Row<FY01TrainingStageRow>(EY01TableKind::TrainingStage).Spawns[0].Count = -1; }
    else if (Case == TEXT("WarehouseUpgradeCount")) { F.Row<FY01WarehouseEventRow>(EY01TableKind::WarehouseEvent).UpgradeCount = 0; }
    else if (Case == TEXT("CharacterStats")) { F.Row<FY01CharacterRow>(EY01TableKind::Character).MoveSpeed = std::numeric_limits<float>::quiet_NaN(); }
    else if (Case == TEXT("NegativeExperience")) { F.Row<FY01LevelExperienceRow>(EY01TableKind::LevelExperience).RequiredExperience = -1; }
    const FY01DataValidation Result = FY01DataCatalog(F.Tables).Validate();
    TestFalse(TEXT("非法配表不得通过"), Result.IsValid());
    TestTrue(*FString::Printf(TEXT("%s 必须返回具体错误 %d"), *Case, static_cast<int32>(Expected)), HasError(Result, Expected));
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DataCorrectionsTest, "Y01.Data.Corrections",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01DataCorrectionsTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    const TCHAR* Cases[] = {
        TEXT("EquipmentSchema"), TEXT("SixUniversalSlots"),
        TEXT("ShopSchema"), TEXT("ShopEquipmentPool"), TEXT("ShopSkillPool"), TEXT("ShopQuality"),
        TEXT("ShopPoolType"), TEXT("ShopMissingPool"), TEXT("ShopEmptyPool"), TEXT("ShopInvalidItemType"),
        TEXT("SynthesisSchema"), TEXT("SynthesisEquipmentPool"), TEXT("SynthesisSkillPool"),
        TEXT("SynthesisBlueToPurple"), TEXT("SynthesisPurpleToGold"), TEXT("SynthesisGoldToRed"),
        TEXT("SynthesisPoolType"), TEXT("SynthesisMissingPool"), TEXT("SynthesisEmptyPool"),
        TEXT("SynthesisItemType"), TEXT("SynthesisPoolQuality"), TEXT("SynthesisSkippedQuality"),
        TEXT("TrainingSchema"), TEXT("TrainingOrdered"), TEXT("TrainingNegative"), TEXT("TrainingNaN"),
        TEXT("TrainingInfinity"), TEXT("TrainingDuplicateStart"), TEXT("TrainingFirstNonZero"),
        TEXT("TrainingReversed"), TEXT("TrainingDuplicateStage"),
        TEXT("TalentSchema"), TEXT("TalentAbilityOnly"), TEXT("TalentEffectOnly"), TEXT("TalentNeither"), TEXT("TalentBoth"),
        TEXT("WarehouseSchema"), TEXT("WarehouseUpgrade"), TEXT("WarehouseZero"), TEXT("WarehouseNegative")
    };
    for (const TCHAR* Name : Cases) { Names.Add(Name); Commands.Add(Name); }
}

bool FY01DataCorrectionsTest::RunTest(const FString& Case)
{
    FDataFixture F;
    using EKind = EY01TableKind;
    using ECode = EY01DataErrorCode;
    TestTrue(TEXT("每个回归以有效的完整19表夹具起步"), FY01DataCatalog(F.Tables).Validate().IsValid());
    auto ExpectValid = [&]()
    {
        const auto Result = FY01DataCatalog(F.Tables).Validate();
        TestTrue(*FString::Printf(TEXT("%s 必须通过真实目录验证"), *Case), Result.IsValid());
        for (const auto& Error : Result.Errors)
        {
            AddInfo(FString::Printf(TEXT("%s.%s: %s"), *Error.Source.RowName.ToString(), *Error.Field, *Error.Message));
        }
    };
    auto ExpectError = [&](EKind Kind, ECode Code, const TCHAR* Field)
    {
        const auto Result = FY01DataCatalog(F.Tables).Validate();
        TestFalse(TEXT("非法数据必须被真实目录拒绝"), Result.IsValid());
        TestTrue(*FString::Printf(TEXT("%s 必须在目标表字段 %s 返回错误 %d"), *Case, Field, static_cast<int32>(Code)),
            Result.Errors.ContainsByPredicate([&](const FY01DataError& Error)
            {
                return Error.Source.Type == Kind && Error.Field == Field && Error.Code == Code && !Error.Message.IsEmpty();
            }));
    };
    auto RefProperty = [&](UScriptStruct* Struct, const TCHAR* Name) -> FStructProperty*
    {
        auto* Property = FindFProperty<FStructProperty>(Struct, Name);
        if (!TestNotNull(*FString::Printf(TEXT("反射结构必须提供 %s"), Name), Property)) { return nullptr; }
        if (!TestTrue(TEXT("池引用须使用 FY01DataRef"), Property->Struct == FY01DataRef::StaticStruct())) { return nullptr; }
        return Property;
    };
    auto AddPool = [&](EKind ItemType, EY01Quality Quality)
    {
        const FName Name(TEXT("CorrectionPool"));
        if (ItemType == EKind::Equipment)
        {
            FY01EquipmentRow Item;
            Item.Quality = Quality;
            const_cast<UDataTable*>(F.Tables[EKind::Equipment])->AddRow(Name, Item);
        }
        else
        {
            FY01SkillRow Item;
            Item.Quality = Quality;
            const_cast<UDataTable*>(F.Tables[EKind::Skill])->AddRow(Name, Item);
        }
        FY01LocalItemPoolRow Pool;
        Pool.ItemType = ItemType;
        Pool.Quality = Quality;
        FY01WeightedEntry Entry;
        Entry.Item = Ref(ItemType, Name);
        Pool.Entries.Add(Entry);
        const_cast<UDataTable*>(F.Tables[EKind::LocalItemPool])->AddRow(Name, Pool);
        return Ref(EKind::LocalItemPool, Name);
    };

    if (Case == TEXT("EquipmentSchema"))
    {
        TestNull(TEXT("装备静态定义不得包含固定 Slot"), FY01EquipmentRow::StaticStruct()->FindPropertyByName(TEXT("Slot")));
    }
    else if (Case == TEXT("SixUniversalSlots"))
    {
        auto& Character = F.Row<FY01CharacterRow>(EKind::Character);
        Character.InitialEquipment.Reset();
        for (int32 Index = 0; Index < Y01Data::EquipmentSlotCount; ++Index)
        {
            const FName Name(*FString::Printf(TEXT("Equipment%d"), Index));
            const_cast<UDataTable*>(F.Tables[EKind::Equipment])->AddRow(Name, FY01EquipmentRow());
            Character.InitialEquipment.Add(Ref(EKind::Equipment, Name));
        }
        ExpectValid();
        FY01DataValidation Errors;
        const auto* Loaded = FY01DataCatalog(F.Tables).FindRow<FY01CharacterRow>(Ref(EKind::Character), Errors);
        if (TestNotNull(TEXT("可查询通用槽"), Loaded))
        {
            for (int32 Index = 0; Index < Y01Data::EquipmentSlotCount; ++Index)
            {
                TestEqual(TEXT("数组索引保留通用槽顺序"), Loaded->InitialEquipment[Index].RowName,
                    FName(*FString::Printf(TEXT("Equipment%d"), Index)));
            }
        }
        const_cast<UDataTable*>(F.Tables[EKind::Equipment])->AddRow(TEXT("Seventh"), FY01EquipmentRow());
        Character.InitialEquipment.Add(Ref(EKind::Equipment, TEXT("Seventh")));
        ExpectError(EKind::Character, ECode::Invariant, TEXT("InitialEquipment"));
    }
    else if (Case.StartsWith(TEXT("Shop")))
    {
        auto* Property = RefProperty(FY01ShopOfferRow::StaticStruct(), TEXT("Pool"));
        TestNull(TEXT("商店不再提供具体 Item"), FY01ShopOfferRow::StaticStruct()->FindPropertyByName(TEXT("Item")));
        if (!Property) { return false; }
        auto& Offer = F.Row<FY01ShopOfferRow>(EKind::ShopOffer);
        auto& PoolRef = *Property->ContainerPtrToValuePtr<FY01DataRef>(&Offer);
        PoolRef = AddPool(Case == TEXT("ShopSkillPool") ? EKind::Skill : EKind::Equipment, EY01Quality::Green);
        if (Case == TEXT("ShopQuality"))
        {
            Offer.Quality = EY01Quality::Blue;
            ExpectError(EKind::ShopOffer, ECode::QualityMismatch, TEXT("Pool.Quality"));
        }
        else if (Case == TEXT("ShopPoolType"))
        {
            PoolRef = Ref(EKind::Equipment);
            ExpectError(EKind::ShopOffer, ECode::TypeMismatch, TEXT("Pool"));
        }
        else if (Case == TEXT("ShopMissingPool") || Case == TEXT("ShopEmptyPool"))
        {
            PoolRef.RowName = Case == TEXT("ShopMissingPool") ? FName(TEXT("Missing")) : NAME_None;
            ExpectError(EKind::ShopOffer, ECode::InvalidReference, TEXT("Pool"));
        }
        else if (Case == TEXT("ShopInvalidItemType"))
        {
            F.Row<FY01LocalItemPoolRow>(EKind::LocalItemPool, PoolRef.RowName).ItemType = EKind::MetaItem;
            ExpectError(EKind::ShopOffer, ECode::TypeMismatch, TEXT("Pool.ItemType"));
        }
        else { ExpectValid(); }
    }
    else if (Case.StartsWith(TEXT("Synthesis")))
    {
        auto* Property = RefProperty(FY01SynthesisRow::StaticStruct(), TEXT("OutputPool"));
        TestNull(TEXT("合成不再提供具体 Output"), FY01SynthesisRow::StaticStruct()->FindPropertyByName(TEXT("Output")));
        if (!Property) { return false; }
        auto& Recipe = F.Row<FY01SynthesisRow>(EKind::Synthesis);
        if (Case == TEXT("SynthesisSkillPool")) { Recipe.InputType = EKind::Skill; }
        if (Case == TEXT("SynthesisBlueToPurple")) { Recipe.InputQuality = EY01Quality::Blue; }
        if (Case == TEXT("SynthesisPurpleToGold")) { Recipe.InputQuality = EY01Quality::Purple; }
        if (Case == TEXT("SynthesisGoldToRed")) { Recipe.InputQuality = EY01Quality::Gold; }
        const auto OutputQuality = static_cast<EY01Quality>(static_cast<uint8>(Recipe.InputQuality) + 1);
        auto& PoolRef = *Property->ContainerPtrToValuePtr<FY01DataRef>(&Recipe);
        PoolRef = AddPool(Recipe.InputType, OutputQuality);
        if (Case == TEXT("SynthesisPoolType"))
        {
            PoolRef = Ref(EKind::Equipment, TEXT("Blue"));
            ExpectError(EKind::Synthesis, ECode::TypeMismatch, TEXT("OutputPool"));
        }
        else if (Case == TEXT("SynthesisMissingPool") || Case == TEXT("SynthesisEmptyPool"))
        {
            PoolRef.RowName = Case == TEXT("SynthesisMissingPool") ? FName(TEXT("Missing")) : NAME_None;
            ExpectError(EKind::Synthesis, ECode::InvalidReference, TEXT("OutputPool"));
        }
        else if (Case == TEXT("SynthesisItemType"))
        {
            PoolRef = AddPool(EKind::Skill, OutputQuality);
            ExpectError(EKind::Synthesis, ECode::TypeMismatch, TEXT("OutputPool.ItemType"));
        }
        else if (Case == TEXT("SynthesisPoolQuality") || Case == TEXT("SynthesisSkippedQuality"))
        {
            PoolRef = AddPool(Recipe.InputType, Case == TEXT("SynthesisSkippedQuality") ? EY01Quality::Purple : EY01Quality::Green);
            ExpectError(EKind::Synthesis, ECode::QualityMismatch, TEXT("OutputPool.Quality"));
        }
        else { ExpectValid(); }
    }
    else if (Case.StartsWith(TEXT("Training")))
    {
        auto* Property = FindFProperty<FFloatProperty>(FY01TrainingStageRow::StaticStruct(), TEXT("StartSeconds"));
        if (!TestNotNull(TEXT("训练阶段必须有浮点 StartSeconds 生效起点"), Property)) { return false; }
        auto& First = F.Row<FY01TrainingStageRow>(EKind::TrainingStage);
        TestEqual(TEXT("默认阶段在零时起始"), Property->GetPropertyValue_InContainer(&First), 0.0f);
        if (Case == TEXT("TrainingSchema")) { ExpectValid(); return true; }
        if (Case == TEXT("TrainingNegative") || Case == TEXT("TrainingNaN") || Case == TEXT("TrainingInfinity"))
        {
            const float Value = Case == TEXT("TrainingNegative") ? -1.0f : Case == TEXT("TrainingNaN")
                ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
            Property->SetPropertyValue_InContainer(&First, Value);
            ExpectError(EKind::TrainingStage, ECode::InvalidNumber, TEXT("StartSeconds"));
            return true;
        }
        if (Case == TEXT("TrainingFirstNonZero"))
        {
            Property->SetPropertyValue_InContainer(&First, 1.0f);
            ExpectError(EKind::TrainingStage, ECode::Invariant, TEXT("StartSeconds"));
            return true;
        }
        FY01TrainingStageRow Second = First, Third = First;
        Second.Stage = 2;
        Third.Stage = 3;
        Property->SetPropertyValue_InContainer(&Second, 10.0f);
        Property->SetPropertyValue_InContainer(&Third, 20.0f);
        if (Case == TEXT("TrainingDuplicateStart")) { Property->SetPropertyValue_InContainer(&Third, 10.0f); }
        if (Case == TEXT("TrainingReversed")) { Property->SetPropertyValue_InContainer(&Third, 5.0f); }
        if (Case == TEXT("TrainingDuplicateStage")) { Third.Stage = 2; }
        auto* Table = const_cast<UDataTable*>(F.Tables[EKind::TrainingStage]);
        Table->AddRow(TEXT("A_Third"), Third);
        Table->AddRow(TEXT("Z_Second"), Second);
        if (Case == TEXT("TrainingDuplicateStart")) { ExpectError(EKind::TrainingStage, ECode::DuplicateValue, TEXT("StartSeconds")); }
        else if (Case == TEXT("TrainingReversed")) { ExpectError(EKind::TrainingStage, ECode::Invariant, TEXT("StartSeconds")); }
        else if (Case == TEXT("TrainingDuplicateStage")) { ExpectError(EKind::TrainingStage, ECode::DuplicateValue, TEXT("Stage")); }
        else { ExpectValid(); }
    }
    else if (Case.StartsWith(TEXT("Talent")))
    {
        auto* Ability = FindFProperty<FSoftClassProperty>(FY01TalentRow::StaticStruct(), TEXT("Ability"));
        auto* Effect = FindFProperty<FSoftClassProperty>(FY01TalentRow::StaticStruct(), TEXT("Effect"));
        const auto* SkillAbility = FindFProperty<FSoftClassProperty>(FY01SkillRow::StaticStruct(), TEXT("Ability"));
        if (!TestNotNull(TEXT("天赋必须支持 GA 软类引用"), Ability) || !TestNotNull(TEXT("天赋保留 GE 软类引用"), Effect)
            || !TestNotNull(TEXT("技能 GA 反射基准"), SkillAbility)) { return false; }
        TestTrue(TEXT("天赋 GA 基类必须与技能一致"), Ability->MetaClass == SkillAbility->MetaClass);
        auto& Talent = F.Row<FY01TalentRow>(EKind::Talent);
        const FSoftObjectPtr AbilityPath(FSoftObjectPath(TEXT("/Game/Tests/UnloadedTalentGA.UnloadedTalentGA_C")));
        const FSoftObjectPtr EffectPath(FSoftObjectPath(TEXT("/Game/Tests/UnloadedTalentGE.UnloadedTalentGE_C")));
        if (Case == TEXT("TalentAbilityOnly") || Case == TEXT("TalentBoth")) { Ability->SetPropertyValue_InContainer(&Talent, AbilityPath); }
        if (Case == TEXT("TalentEffectOnly") || Case == TEXT("TalentBoth")) { Effect->SetPropertyValue_InContainer(&Talent, EffectPath); }
        ExpectValid();
        TestFalse(TEXT("验证不加载天赋 GA"), Ability->GetPropertyValue_InContainer(&Talent).IsValid());
        TestFalse(TEXT("验证不加载天赋 GE"), Effect->GetPropertyValue_InContainer(&Talent).IsValid());
    }
    else if (Case.StartsWith(TEXT("Warehouse")))
    {
        auto* Property = FindFProperty<FIntProperty>(FY01WarehouseEventRow::StaticStruct(), TEXT("UpgradeCount"));
        TestNull(TEXT("仓库事件不绑定产物池"), FY01WarehouseEventRow::StaticStruct()->FindPropertyByName(TEXT("LootPool")));
        TestNull(TEXT("仓库事件不抽新物品"), FY01WarehouseEventRow::StaticStruct()->FindPropertyByName(TEXT("DrawCount")));
        if (!TestNotNull(TEXT("仓库事件必须提供提升现有物品数量"), Property)) { return false; }
        auto& Event = F.Row<FY01WarehouseEventRow>(EKind::WarehouseEvent);
        Property->SetPropertyValue_InContainer(&Event, Case == TEXT("WarehouseZero") ? 0 : Case == TEXT("WarehouseNegative") ? -1 : 3);
        if (Case == TEXT("WarehouseZero") || Case == TEXT("WarehouseNegative"))
        {
            ExpectError(EKind::WarehouseEvent, ECode::InvalidNumber, TEXT("UpgradeCount"));
        }
        else
        {
            // 保留完整19表，但改名局外池，证明事件不依赖任何固定池引用。
            const auto Pool = F.Row<FY01MetaLootPoolRow>(EKind::MetaLootPool);
            auto* Table = const_cast<UDataTable*>(F.Tables[EKind::MetaLootPool]);
            Table->RemoveRow(TEXT("Default"));
            Table->AddRow(TEXT("UnrelatedPool"), Pool);
            ExpectValid();
        }
    }
    else { AddError(TEXT("未知纠正回归用例")); }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01DataQueryTest, "Y01.Data.ReadOnlyQuery",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01DataQueryTest::RunTest(const FString& Parameters)
{
    FDataFixture F;
    FY01DataCatalog Catalog(F.Tables);
    FY01DataValidation Errors;
    const auto* Row = Catalog.FindRow<FY01MonsterRow>(Ref(EY01TableKind::Monster), Errors);
    static_assert(std::is_same_v<decltype(Row), const FY01MonsterRow*>);
    TestNotNull(TEXT("类型化只读查询"), Row);
    TestTrue(TEXT("有效查询无错误"), Errors.IsValid());
    Errors.Errors.Reset();
    TestNull(TEXT("禁止跨行类型转换"), Catalog.FindRow<FY01SkillRow>(Ref(EY01TableKind::Monster), Errors));
    TestTrue(TEXT("查询返回类型错误"), HasError(Errors, EY01DataErrorCode::TypeMismatch));
    Errors.Errors.Reset();
    TestNull(TEXT("缺行返回空"), Catalog.FindRow<FY01MonsterRow>(Ref(EY01TableKind::Monster, TEXT("Missing")), Errors));
    TestTrue(TEXT("缺行错误"), HasError(Errors, EY01DataErrorCode::InvalidReference));
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DataSamplingTest, "Y01.Data.Sampling",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01DataSamplingTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    const TCHAR* Cases[] = { TEXT("Deterministic"), TEXT("ZeroExcluded"), TEXT("Empty"), TEXT("Negative"), TEXT("NaN"), TEXT("Infinity"), TEXT("Missing"), TEXT("Type"), TEXT("Quality"), TEXT("AllZero"), TEXT("Meta"), TEXT("OverflowSafe") };
    for (const TCHAR* Name : Cases) { Names.Add(Name); Commands.Add(Name); }
}
bool FY01DataSamplingTest::RunTest(const FString& Case)
{
    FDataFixture F;
    auto& Pool = F.Row<FY01LocalItemPoolRow>(EY01TableKind::LocalItemPool);
    FY01EquipmentRow Second;
    const_cast<UDataTable*>(F.Tables[EY01TableKind::Equipment])->AddRow(TEXT("Second"), Second);
    FY01WeightedEntry Entry;
    Entry.Item = Ref(EY01TableKind::Equipment, TEXT("Second"));
    Entry.Weight = 3;
    Pool.Entries.Add(Entry);
    bool bValid = Case == TEXT("Deterministic") || Case == TEXT("ZeroExcluded") || Case == TEXT("Meta") || Case == TEXT("OverflowSafe");
    if (Case == TEXT("ZeroExcluded")) { Pool.Entries[0].Weight = 0; }
    if (Case == TEXT("Empty")) { Pool.Entries.Empty(); }
    if (Case == TEXT("Negative")) { Pool.Entries[0].Weight = -1; }
    if (Case == TEXT("NaN")) { Pool.Entries[0].Weight = std::numeric_limits<float>::quiet_NaN(); }
    if (Case == TEXT("Infinity")) { Pool.Entries[0].Weight = std::numeric_limits<float>::infinity(); }
    if (Case == TEXT("Missing")) { Pool.Entries[0].Item.RowName = TEXT("Missing"); }
    if (Case == TEXT("Type")) { Pool.Entries[0].Item.Type = EY01TableKind::Skill; }
    if (Case == TEXT("Quality")) { Pool.Quality = EY01Quality::Red; }
    if (Case == TEXT("AllZero")) { for (auto& E : Pool.Entries) { E.Weight = 0; } }
    if (Case == TEXT("OverflowSafe")) { for (auto& E : Pool.Entries) { E.Weight = std::numeric_limits<float>::max(); } }
    FY01DataCatalog Catalog(F.Tables);
    FRandomStream A(83471), B(83471);
    const auto PoolRef = Ref(Case == TEXT("Meta") ? EY01TableKind::MetaLootPool : EY01TableKind::LocalItemPool);
    int32 FirstCount = 0;
    for (int32 I = 0; I < (bValid ? 256 : 1); ++I)
    {
        FY01DataValidation Errors;
        FY01DataRef Pick = Ref(EY01TableKind::Monster), Repeat;
        const int32 SeedBefore = A.GetCurrentSeed();
        const bool bPicked = Catalog.SamplePool(PoolRef, A, Pick, Errors);
        TestEqual(TEXT("抽样结果"), bPicked, bValid);
        if (!bValid)
        {
            TestFalse(TEXT("拒绝抽样须有错误"), Errors.IsValid());
            TestTrue(TEXT("失败清空输出"), Pick.RowName.IsNone());
            TestEqual(TEXT("失败不消费随机流"), A.GetCurrentSeed(), SeedBefore);
            break;
        }
        if (!bPicked) { break; }
        TestTrue(TEXT("重复种子抽样成功"), Catalog.SamplePool(PoolRef, B, Repeat, Errors));
        TestTrue(TEXT("随机流结果确定"), Pick.Type == Repeat.Type && Pick.RowName == Repeat.RowName);
        TestTrue(TEXT("抽样不修改配表"), Pool.Entries.Num() == 2);
        if (Pick.RowName == TEXT("Default")) { ++FirstCount; }
        if (Case == TEXT("ZeroExcluded")) { TestTrue(TEXT("零权重永不抽中"), Pick.RowName == TEXT("Second")); }
        if (Case == TEXT("Meta")) { TestTrue(TEXT("公共局外抽样类型"), Pick.Type == EY01TableKind::MetaItem); }
    }
    if (Case == TEXT("Deterministic")) { TestTrue(TEXT("权重影响分布且两项可达"), FirstCount > 20 && FirstCount < 110); }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01DataSubsystemTest, "Y01.Data.Subsystem",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01DataSubsystemTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("MissingSettings"), TEXT("SoftTablesAndReload"), TEXT("InitializeMissing") }) { Names.Add(Name); Commands.Add(Name); }
}
bool FY01DataSubsystemTest::RunTest(const FString& Case)
{
    TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>());
    TStrongObjectPtr<UY01DataSubsystem> Subsystem(NewObject<UY01DataSubsystem>(Instance.Get()));
    TStrongObjectPtr<UY01DataSettings> Settings(NewObject<UY01DataSettings>());
    if (Case == TEXT("InitializeMissing"))
    {
        FSubsystemCollection<UGameInstanceSubsystem> Collection;
        Subsystem->Initialize(Collection);
        TestFalse(TEXT("正式表未配置禁止就绪"), Subsystem->IsReady());
        TestTrue(TEXT("初始化返回缺表原因"), HasError(Subsystem->GetValidation(), EY01DataErrorCode::MissingTable));
        Subsystem->Deinitialize();
        return true;
    }
    if (Case == TEXT("MissingSettings"))
    {
        TestFalse(TEXT("缺配置加载失败"), Subsystem->LoadFromSettings(*Settings).IsValid());
        TestTrue(TEXT("保留具体配置错误"), HasError(Subsystem->GetValidation(), EY01DataErrorCode::MissingTable));
        TestNull(TEXT("未就绪不暴露目录"), Subsystem->GetCatalog());
        return true;
    }
    FDataFixture F;
    for (const auto& Pair : F.Tables) { Settings->Tables.Add(Pair.Key, TSoftObjectPtr<UDataTable>(const_cast<UDataTable*>(Pair.Value))); }
    TestTrue(TEXT("从软引用加载内存表"), Subsystem->LoadFromSettings(*Settings).IsValid());
    TestTrue(TEXT("完整配置就绪"), Subsystem->IsReady());
    const FY01DataCatalog* Catalog = Subsystem->GetCatalog();
    TestNotNull(TEXT("提供只读目录"), Catalog);
    if (Catalog)
    {
        FY01DataValidation Errors;
        const auto* Monster = Catalog->FindRow<FY01MonsterRow>(Ref(EY01TableKind::Monster), Errors);
        TestNotNull(TEXT("已加载查询可用"), Monster);
    }
    Settings->Tables.Remove(EY01TableKind::Monster);
    TestFalse(TEXT("重新加载缺表失败"), Subsystem->LoadFromSettings(*Settings).IsValid());
    TestFalse(TEXT("失败不得保留旧就绪状态"), Subsystem->IsReady());
    TestNull(TEXT("失败不得暴露旧目录"), Subsystem->GetCatalog());
    return true;
}
#endif
