#include "Misc/AutomationTest.h"
#include "Y01MatchFixture.h"
#include "UObject/UnrealType.h"
#include "UObject/StructOnScope.h"
#include <limits>
#include <type_traits>

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
using EKind = EY01TableKind;
using ECode = EY01DataErrorCode;
using FFixture = FY01MatchFixture;

void AddSourceEffects(FFixture& F)
{
    FY01SourceEffectRow Attack;
    Attack.Magnitude = 3;
    F.Add(EKind::SourceEffect, Attack, TEXT("Attack"));
    FY01SourceEffectRow Gold;
    Gold.Kind = EY01SourceEffectKind::GoldPerSecond;
    Gold.Magnitude = 0.25;
    F.Add(EKind::SourceEffect, Gold, TEXT("Gold"));
}

TArray<FY01DataRef>& SourceRefs(FFixture& F, EKind Kind, FName Name = TEXT("Default"))
{
    switch (Kind)
    {
    case EKind::Equipment: return F.Row<FY01EquipmentRow>(Kind, Name).EffectIds;
    case EKind::Skill: return F.Row<FY01SkillRow>(Kind, Name).EffectIds;
    case EKind::Talent: return F.Row<FY01TalentRow>(Kind, Name).EffectIds;
    default: return F.Row<FY01ShopOfferRow>(Kind, Name).EffectIds;
    }
}

void DirectOffer(FFixture& F)
{
    auto& Row = F.Row<FY01ShopOfferRow>(EKind::ShopOffer);
    Row.ShopKind = EY01ShopKind::DirectEffect;
    Row.Pool = FY01DataRef();
    Row.EffectIds = { FFixture::Ref(EKind::SourceEffect, TEXT("Attack")) };
}

void CopySource(FFixture& F, EKind Kind)
{
    switch (Kind)
    {
    case EKind::Equipment: F.Add(Kind, F.Row<FY01EquipmentRow>(Kind), TEXT("Second")); break;
    case EKind::Skill: F.Add(Kind, F.Row<FY01SkillRow>(Kind), TEXT("Second")); break;
    case EKind::Talent: F.Add(Kind, F.Row<FY01TalentRow>(Kind), TEXT("Second")); break;
    default: F.Add(Kind, F.Row<FY01ShopOfferRow>(Kind), TEXT("Second")); break;
    }
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36DataTest, "Y01.Framework.R36.Data",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

void FY01R36DataTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    auto Add = [&](const FString& Name) { Names.Add(Name); Commands.Add(Name); };
    for (const TCHAR* Case : { TEXT("Schema"), TEXT("Legacy19"), TEXT("Optional.Valid"), TEXT("Optional.Empty"),
        TEXT("Optional.NullTable"), TEXT("Optional.WrongSchema"), TEXT("Optional.NullSchema"),
        TEXT("Optional.UnknownTableKind"), TEXT("Optional.NullRow"), TEXT("SharedCrossSources") }) { Add(Case); }
    for (const TCHAR* Kind : { TEXT("AttackFlat"), TEXT("GoldPerSecond") })
    {
        for (const TCHAR* Case : { TEXT("Zero"), TEXT("Fraction"), TEXT("Negative"), TEXT("NaN"),
            TEXT("Infinity"), TEXT("NegativeInfinity"), TEXT("MaxFinite"), TEXT("UnknownKind"), TEXT("SentinelKind") })
        { Add(FString(TEXT("Value.")) + Kind + TEXT(".") + Case); }
    }
    for (const TCHAR* Source : { TEXT("Equipment"), TEXT("Skill"), TEXT("Talent"), TEXT("ShopOffer") })
    {
        for (const TCHAR* Case : { TEXT("Valid"), TEXT("MissingTable"), TEXT("NullTable"), TEXT("WrongSchema"),
            TEXT("NoneType"), TEXT("WrongType"), TEXT("UnknownType"), TEXT("MissingRow"), TEXT("EmptyName"),
            TEXT("EmptyRef"), TEXT("Duplicate"), TEXT("DuplicateCaseInsensitive"), TEXT("SharedOtherRow"), TEXT("EmptyTable") })
        { Add(FString(TEXT("Ref.")) + Source + TEXT(".") + Case); }
    }
    for (const TCHAR* Case : { TEXT("Equipment.Valid"), TEXT("Equipment.AttackBonus"), TEXT("Equipment.Effect"),
        TEXT("Active.Valid"), TEXT("Active.MissingAbility"), TEXT("Active.Effect"), TEXT("Active.TestSkillValid"),
        TEXT("Active.TestSkillInvalid"), TEXT("Active.NegativeCooldown"), TEXT("Passive.Valid"), TEXT("Passive.Ability"),
        TEXT("Passive.Effect"), TEXT("Passive.Cooldown"), TEXT("Passive.TestSkill"), TEXT("Talent.Valid"),
        TEXT("Talent.Ability"), TEXT("Talent.Effect"), TEXT("Legacy.Equipment"), TEXT("Legacy.Active"),
        TEXT("Legacy.Passive"), TEXT("Legacy.Talent"), TEXT("Legacy.EmptySkill") }) { Add(Case); }
    for (const TCHAR* Case : { TEXT("Valid"), TEXT("NoEffects"), TEXT("PoolBoth"), TEXT("PoolTypeOnly"),
        TEXT("PoolNameOnly"), TEXT("PoolUnknownType"), TEXT("QuantityZero"), TEXT("QuantityNegative"),
        TEXT("QuantityTwo"), TEXT("QuantityMax"), TEXT("PriceNegative"), TEXT("PriceMax"),
        TEXT("QualityBlue"), TEXT("QualityPurple"), TEXT("QualityGold"), TEXT("QualityRed"), TEXT("QualityUnknown") })
    { Add(FString(TEXT("Direct.")) + Case); }
    for (const TCHAR* Case : { TEXT("Valid"), TEXT("Effects"), TEXT("MissingPool"), TEXT("WrongPoolType"),
        TEXT("EmptyPoolRef"), TEXT("QualityMismatch"), TEXT("QualityUnknown"), TEXT("PriceNegative"),
        TEXT("PriceMax"), TEXT("QuantityZero"), TEXT("QuantityNegative"), TEXT("QuantityTwo"), TEXT("QuantityMax"),
        TEXT("UnknownKind"), TEXT("SentinelKind") }) { Add(FString(TEXT("Random.")) + Case); }
}

bool FY01R36DataTest::RunTest(const FString& Case)
{
    FFixture F;
    TestEqual(TEXT("复用旧夹具，必填表仍为19张"), F.Tables.Num(), 19);
    if (!TestTrue(TEXT("每例先验证未改动旧夹具合法"), FY01DataCatalog(F.Tables).Validate().IsValid())) { return false; }
    auto ExpectValid = [&]()
    {
        const auto Result = FY01DataCatalog(F.Tables).Validate();
        TestTrue(*FString::Printf(TEXT("%s 应通过真实目录校验"), *Case), Result.IsValid());
        for (const auto& Error : Result.Errors)
        {
            AddInfo(FString::Printf(TEXT("表=%d 行=%s 字段=%s 错误=%d %s"), static_cast<int32>(Error.Source.Type),
                *Error.Source.RowName.ToString(), *Error.Field, static_cast<int32>(Error.Code), *Error.Message));
        }
    };
    auto ExpectError = [&](EKind Kind, ECode Code, const FString& Field, FName Name = TEXT("Default"))
    {
        const auto Result = FY01DataCatalog(F.Tables).Validate();
        TestFalse(*FString::Printf(TEXT("%s 非法内容必须拒绝"), *Case), Result.IsValid());
        TestTrue(*FString::Printf(TEXT("%s 须精确定位表=%d 行=%s 字段=%s 错误=%d"), *Case,
            static_cast<int32>(Kind), *Name.ToString(), *Field, static_cast<int32>(Code)),
            Result.Errors.ContainsByPredicate([&](const FY01DataError& Error)
            {
                return Error.Source.Type == Kind && Error.Source.RowName == Name && Error.Field == Field
                    && Error.Code == Code && !Error.Message.IsEmpty();
            }));
    };
    const TSoftClassPtr<UGameplayAbility> Ability(FSoftObjectPath(TEXT("/Game/Tests/UnloadedR36GA.UnloadedR36GA_C")));
    const TSoftClassPtr<UGameplayEffect> Effect(FSoftObjectPath(TEXT("/Game/Tests/UnloadedR36GE.UnloadedR36GE_C")));
    if (Case == TEXT("Schema"))
    {
        static_assert(std::is_same_v<decltype(FY01SourceEffectRow::Magnitude), double>);
        static_assert(std::is_same_v<decltype(FY01ShopOfferRow::Price), int32>);
        static_assert(std::is_base_of_v<FTableRowBase, FY01SourceEffectRow>);
        const TCHAR* OldNames[] = { TEXT("None"), TEXT("Wave"), TEXT("TowerFloor"), TEXT("TrainingStage"),
            TEXT("Monster"), TEXT("Skill"), TEXT("Talent"), TEXT("Equipment"), TEXT("LevelExperience"), TEXT("MetaLootPool"),
            TEXT("MatchRules"), TEXT("Character"), TEXT("RewardPackage"), TEXT("LocalItemPool"), TEXT("ShopOffer"),
            TEXT("Synthesis"), TEXT("Quality"), TEXT("MetaItem"), TEXT("WarehouseEvent"), TEXT("TalentChoice") };
        for (int32 Index = 0; Index < UE_ARRAY_COUNT(OldNames); ++Index)
        { TestEqual(TEXT("旧枚举数值不可变化"), StaticEnum<EKind>()->GetValueByNameString(OldNames[Index]), int64(Index)); }
        TestEqual(TEXT("新表仅追加"), static_cast<uint8>(EKind::SourceEffect), uint8(20));
        TestEqual(TEXT("效果默认攻击固定值"), FY01SourceEffectRow().Kind, EY01SourceEffectKind::AttackFlat);
        TestEqual(TEXT("效果默认0"), FY01SourceEffectRow().Magnitude, 0.0);
        TestEqual(TEXT("商店默认随机池"), FY01ShopOfferRow().ShopKind, EY01ShopKind::RandomPool);
        for (auto* Type : { FY01EquipmentRow::StaticStruct(), FY01SkillRow::StaticStruct(),
            FY01TalentRow::StaticStruct(), FY01ShopOfferRow::StaticStruct() })
        {
            auto* Array = FindFProperty<FArrayProperty>(Type, TEXT("EffectIds"));
            if (!TestNotNull(TEXT("四来源均提供反射数组"), Array)) { continue; }
            auto* Inner = CastField<FStructProperty>(Array->Inner);
            TestTrue(TEXT("元素为显式类型引用"), Inner && Inner->Struct == FY01DataRef::StaticStruct());
            TestTrue(TEXT("数组可编辑且蓝图只读"), Array->HasAllPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly));
        }
        for (EKind Kind : { EKind::Equipment, EKind::Skill, EKind::Talent, EKind::ShopOffer })
        { TestTrue(TEXT("旧内容效果列表默认空"), SourceRefs(F, Kind).IsEmpty()); }
        auto* Magnitude = FindFProperty<FDoubleProperty>(FY01SourceEffectRow::StaticStruct(), TEXT("Magnitude"));
        TestNotNull(TEXT("幅度反射为double"), Magnitude);
        for (const TCHAR* Name : { TEXT("Kind"), TEXT("Magnitude"), TEXT("SourceAttribute"), TEXT("TargetAttribute"), TEXT("Amount") })
        {
            const auto* Property = FindFProperty<FProperty>(FY01SourceEffectRow::StaticStruct(), Name);
            TestTrue(TEXT("效果字段可编辑且蓝图只读"), Property && Property->HasAllPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly));
        }
        const auto* ShopKind = FindFProperty<FEnumProperty>(FY01ShopOfferRow::StaticStruct(), TEXT("ShopKind"));
        TestTrue(TEXT("商店种类为可编辑蓝图只读枚举"), ShopKind && ShopKind->GetEnum() == StaticEnum<EY01ShopKind>()
            && ShopKind->HasAllPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly));
        int32 OwnFields = 0;
        for (TFieldIterator<FProperty> It(FY01SourceEffectRow::StaticStruct(), EFieldIteratorFlags::ExcludeSuper); It; ++It) { ++OwnFields; }
        TestEqual(TEXT("效果行保留现有种类、幅度及三项属性映射字段"), OwnFields, 5);
        return true;
    }
    if (Case == TEXT("Legacy19")) { ExpectValid(); return true; }
    if (Case.StartsWith(TEXT("Legacy.")))
    {
        if (Case == TEXT("Legacy.Equipment")) { auto& Row = F.Row<FY01EquipmentRow>(EKind::Equipment); Row.AttackBonus = 5; Row.Effect = Effect; }
        else if (Case == TEXT("Legacy.Talent")) { auto& Row = F.Row<FY01TalentRow>(EKind::Talent); Row.Ability = Ability; Row.Effect = Effect; }
        else
        {
            auto& Row = F.Row<FY01SkillRow>(EKind::Skill);
            if (Case == TEXT("Legacy.EmptySkill")) { Row.Ability.Reset(); }
            else { Row.Type = Case == TEXT("Legacy.Passive") ? EY01SkillType::Passive : EY01SkillType::Active; Row.Ability = Ability; Row.Effect = Effect; }
        }
        ExpectValid(); return true;
    }
    AddSourceEffects(F);
    if (Case.StartsWith(TEXT("Optional.")))
    {
        if (Case == TEXT("Optional.NullTable")) { F.Tables[EKind::SourceEffect] = nullptr; ExpectError(EKind::SourceEffect, ECode::MissingTable, TEXT("Table"), NAME_None); }
        else if (Case == TEXT("Optional.WrongSchema")) { F.Tables[EKind::SourceEffect] = F.Tables[EKind::Equipment]; ExpectError(EKind::SourceEffect, ECode::WrongSchema, TEXT("RowStruct"), NAME_None); }
        else if (Case == TEXT("Optional.NullSchema"))
        {
            auto* Table = NewObject<UDataTable>(); F.Owners.Emplace(Table); F.Tables[EKind::SourceEffect] = Table;
            ExpectError(EKind::SourceEffect, ECode::WrongSchema, TEXT("RowStruct"), NAME_None);
        }
        else if (Case == TEXT("Optional.UnknownTableKind"))
        { F.Tables.Add(static_cast<EKind>(255), F.Tables[EKind::SourceEffect]); ExpectError(static_cast<EKind>(255), ECode::InvalidEnum, TEXT("Tables"), NAME_None); }
        else if (Case == TEXT("Optional.NullRow"))
        {
            auto& Rows = const_cast<TMap<FName, uint8*>&>(F.Tables[EKind::SourceEffect]->GetRowMap());
            TGuardValue<uint8*> Guard(Rows.FindChecked(TEXT("Attack")), nullptr);
            ExpectError(EKind::SourceEffect, ECode::InvalidReference, TEXT("RowName"), TEXT("Attack"));
        }
        else
        {
            if (Case == TEXT("Optional.Empty")) { const_cast<UDataTable*>(F.Tables[EKind::SourceEffect])->EmptyTable(); }
            ExpectValid();
            if (Case == TEXT("Optional.Valid"))
            {
                FY01DataValidation Query;
                const auto* Row = FY01DataCatalog(F.Tables).FindRow<FY01SourceEffectRow>(FFixture::Ref(EKind::SourceEffect, TEXT("Gold")), Query);
                if (TestNotNull(TEXT("可选表支持类型化只读查询"), Row)) { TestEqual(TEXT("小数精确保留"), Row->Magnitude, 0.25); }
                TestTrue(TEXT("类型化查询无错误"), Query.IsValid());
            }
        }
        return true;
    }
    if (Case.StartsWith(TEXT("Value.")))
    {
        // 修改第二个、无人引用的行，证明每行都验证，而非仅校验引用闭包。
        auto& Row = F.Row<FY01SourceEffectRow>(EKind::SourceEffect, TEXT("Gold"));
        Row.Kind = Case.Contains(TEXT("GoldPerSecond")) ? EY01SourceEffectKind::GoldPerSecond : EY01SourceEffectKind::AttackFlat;
        if (Case.EndsWith(TEXT(".Zero"))) { Row.Magnitude = 0; ExpectValid(); }
        else if (Case.EndsWith(TEXT(".Fraction"))) { Row.Magnitude = 0.25; ExpectValid(); }
        else if (Case.EndsWith(TEXT(".MaxFinite"))) { Row.Magnitude = std::numeric_limits<double>::max(); ExpectValid(); }
        else if (Case.EndsWith(TEXT("Kind")))
        { Row.Kind = static_cast<EY01SourceEffectKind>(Case.EndsWith(TEXT("UnknownKind")) ? 255 : static_cast<uint8>(EY01SourceEffectKind::MappingCoefficientAdd) + 1); ExpectError(EKind::SourceEffect, ECode::InvalidEnum, TEXT("Kind"), TEXT("Gold")); }
        else
        {
            Row.Magnitude = Case.EndsWith(TEXT(".Negative")) ? -0.25 : Case.EndsWith(TEXT(".NaN")) ? std::numeric_limits<double>::quiet_NaN()
                : Case.EndsWith(TEXT(".NegativeInfinity")) ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
            ExpectError(EKind::SourceEffect, ECode::InvalidNumber, TEXT("Magnitude"), TEXT("Gold"));
        }
        return true;
    }
    if (Case == TEXT("SharedCrossSources"))
    {
        DirectOffer(F);
        for (EKind Kind : { EKind::Equipment, EKind::Skill, EKind::Talent, EKind::ShopOffer })
        { SourceRefs(F, Kind) = { FFixture::Ref(EKind::SourceEffect, TEXT("Attack")), FFixture::Ref(EKind::SourceEffect, TEXT("Gold")) }; }
        ExpectValid(); return true;
    }
    if (Case.StartsWith(TEXT("Ref.")))
    {
        const EKind Kind = Case.Contains(TEXT(".Equipment.")) ? EKind::Equipment : Case.Contains(TEXT(".Skill.")) ? EKind::Skill
            : Case.Contains(TEXT(".Talent.")) ? EKind::Talent : EKind::ShopOffer;
        if (Kind == EKind::ShopOffer) { DirectOffer(F); }
        auto& Refs = SourceRefs(F, Kind);
        Refs = { FFixture::Ref(EKind::SourceEffect, TEXT("Attack")), FFixture::Ref(EKind::SourceEffect, TEXT("Gold")) };
        ECode Code = ECode::InvalidReference;
        FString Field = TEXT("EffectIds[1]");
        if (Case.EndsWith(TEXT(".Valid"))) { ExpectValid(); return true; }
        if (Case.EndsWith(TEXT(".SharedOtherRow"))) { CopySource(F, Kind); ExpectValid(); return true; }
        if (Case.EndsWith(TEXT(".MissingTable"))) { F.Tables.Remove(EKind::SourceEffect); Code = ECode::MissingTable; }
        else if (Case.EndsWith(TEXT(".NullTable"))) { F.Tables[EKind::SourceEffect] = nullptr; Code = ECode::MissingTable; }
        else if (Case.EndsWith(TEXT(".WrongSchema"))) { F.Tables[EKind::SourceEffect] = F.Tables[EKind::Equipment]; Code = ECode::WrongSchema; }
        else if (Case.EndsWith(TEXT(".EmptyTable"))) { const_cast<UDataTable*>(F.Tables[EKind::SourceEffect])->EmptyTable(); }
        else if (Case.EndsWith(TEXT(".NoneType"))) { Refs[1].Type = EKind::None; }
        else if (Case.EndsWith(TEXT(".WrongType"))) { Refs[1].Type = EKind::Equipment; Code = ECode::TypeMismatch; }
        else if (Case.EndsWith(TEXT(".UnknownType"))) { Refs[1].Type = static_cast<EKind>(255); Code = ECode::TypeMismatch; }
        else if (Case.EndsWith(TEXT(".MissingRow"))) { Refs[1].RowName = TEXT("Missing"); }
        else if (Case.EndsWith(TEXT(".EmptyName"))) { Refs[1].RowName = NAME_None; }
        else if (Case.EndsWith(TEXT(".EmptyRef"))) { Refs[1] = FY01DataRef(); }
        else { Refs[1] = Refs[0]; if (Case.EndsWith(TEXT(".DuplicateCaseInsensitive"))) { Refs[1].RowName = TEXT("attack"); } Code = ECode::DuplicateValue; }
        ExpectError(Kind, Code, Field); return true;
    }
    if (Case.StartsWith(TEXT("Equipment.")) || Case.StartsWith(TEXT("Talent.")) || Case.StartsWith(TEXT("Active.")) || Case.StartsWith(TEXT("Passive.")))
    {
        const EKind Kind = Case.StartsWith(TEXT("Equipment.")) ? EKind::Equipment : Case.StartsWith(TEXT("Talent.")) ? EKind::Talent : EKind::Skill;
        SourceRefs(F, Kind) = { FFixture::Ref(EKind::SourceEffect, TEXT("Attack")) };
        FString Field;
        ECode Code = ECode::Invariant;
        if (Kind == EKind::Equipment)
        {
            auto& Row = F.Row<FY01EquipmentRow>(Kind);
            if (Case.EndsWith(TEXT(".AttackBonus"))) { Row.AttackBonus = 1; Field = TEXT("AttackBonus"); }
            if (Case.EndsWith(TEXT(".Effect"))) { Row.Effect = Effect; Field = TEXT("Effect"); }
        }
        else if (Kind == EKind::Talent)
        {
            auto& Row = F.Row<FY01TalentRow>(Kind);
            if (Case.EndsWith(TEXT(".Ability"))) { Row.Ability = Ability; Field = TEXT("Ability"); }
            if (Case.EndsWith(TEXT(".Effect"))) { Row.Effect = Effect; Field = TEXT("Effect"); }
        }
        else
        {
            auto& Row = F.Row<FY01SkillRow>(Kind);
            const bool bPassive = Case.StartsWith(TEXT("Passive."));
            Row.Type = bPassive ? EY01SkillType::Passive : EY01SkillType::Active;
            Row.Ability = bPassive ? TSoftClassPtr<UGameplayAbility>() : Ability;
            if (Case.EndsWith(TEXT(".MissingAbility"))) { Row.Ability.Reset(); Field = TEXT("Ability"); }
            if (Case.EndsWith(TEXT(".Ability"))) { Row.Ability = Ability; Field = TEXT("Ability"); }
            if (Case.EndsWith(TEXT(".Effect"))) { Row.Effect = Effect; Field = TEXT("Effect"); }
            if (Case.EndsWith(TEXT(".Cooldown"))) { Row.CooldownSeconds = 0.25; Field = TEXT("CooldownSeconds"); }
            if (Case.EndsWith(TEXT(".NegativeCooldown"))) { Row.CooldownSeconds = -1; Field = TEXT("CooldownSeconds"); Code = ECode::InvalidNumber; }
            if (Case.Contains(TEXT("TestSkill")))
            {
                Row.TestSkill.Mode = EY01TestSkillMode::Area; Row.TestSkill.Radius = 100; Row.TestSkill.ActiveSeconds = 1;
                if (bPassive) { Field = TEXT("TestSkill.Mode"); }
                if (Case.EndsWith(TEXT("Invalid"))) { Row.TestSkill.Radius = 0; Field = TEXT("TestSkill"); Code = ECode::InvalidNumber; }
            }
        }
        if (Field.IsEmpty()) { ExpectValid(); } else { ExpectError(Kind, Code, Field); }
        TestFalse(TEXT("目录校验不得加载GA软引用"), Ability.IsValid());
        TestFalse(TEXT("目录校验不得加载GE软引用"), Effect.IsValid());
        return true;
    }
    if (Case.StartsWith(TEXT("Direct.")) || Case.StartsWith(TEXT("Random.")))
    {
        const bool bDirect = Case.StartsWith(TEXT("Direct."));
        if (bDirect) { DirectOffer(F); }
        auto& Row = F.Row<FY01ShopOfferRow>(EKind::ShopOffer);
        FString Field;
        ECode Code = ECode::Invariant;
        if (Case.EndsWith(TEXT(".NoEffects"))) { Row.EffectIds.Reset(); Field = TEXT("EffectIds"); }
        if (Case.EndsWith(TEXT(".Effects"))) { Row.EffectIds.Add(FFixture::Ref(EKind::SourceEffect, TEXT("Attack"))); Field = TEXT("EffectIds"); }
        if (Case.EndsWith(TEXT(".PoolBoth"))) { Row.Pool = FFixture::Ref(EKind::LocalItemPool); Field = TEXT("Pool"); }
        if (Case.EndsWith(TEXT(".PoolTypeOnly"))) { Row.Pool.Type = EKind::LocalItemPool; Field = TEXT("Pool"); }
        if (Case.EndsWith(TEXT(".PoolNameOnly"))) { Row.Pool.RowName = TEXT("Default"); Field = TEXT("Pool"); }
        if (Case.EndsWith(TEXT(".PoolUnknownType"))) { Row.Pool.Type = static_cast<EKind>(255); Field = TEXT("Pool"); }
        if (Case.EndsWith(TEXT(".MissingPool"))) { Row.Pool.RowName = TEXT("Missing"); Field = TEXT("Pool"); Code = ECode::InvalidReference; }
        if (Case.EndsWith(TEXT(".WrongPoolType"))) { Row.Pool.Type = EKind::Equipment; Field = TEXT("Pool"); Code = ECode::TypeMismatch; }
        if (Case.EndsWith(TEXT(".EmptyPoolRef"))) { Row.Pool = FY01DataRef(); Field = TEXT("Pool"); Code = ECode::InvalidReference; }
        if (Case.EndsWith(TEXT(".QualityMismatch"))) { Row.Quality = EY01Quality::Blue; Field = TEXT("Pool.Quality"); Code = ECode::QualityMismatch; }
        if (Case.Contains(TEXT(".Quality")) && bDirect)
        {
            Row.Quality = Case.EndsWith(TEXT("Blue")) ? EY01Quality::Blue : Case.EndsWith(TEXT("Purple")) ? EY01Quality::Purple
                : Case.EndsWith(TEXT("Gold")) ? EY01Quality::Gold : EY01Quality::Red;
            Field = TEXT("Quality");
        }
        if (Case.EndsWith(TEXT(".QualityUnknown"))) { Row.Quality = static_cast<EY01Quality>(255); Field = TEXT("Quality"); Code = ECode::InvalidEnum; }
        if (Case.EndsWith(TEXT(".PriceNegative"))) { Row.Price = -1; Field = TEXT("Price"); Code = ECode::InvalidNumber; }
        if (Case.EndsWith(TEXT(".PriceMax"))) { Row.Price = MAX_int32; }
        if (Case.Contains(TEXT(".Quantity")))
        {
            Row.Quantity = Case.EndsWith(TEXT("Zero")) ? 0 : Case.EndsWith(TEXT("Negative")) ? -1 : Case.EndsWith(TEXT("Two")) ? 2 : MAX_int32;
            if (bDirect || Row.Quantity <= 0) { Field = TEXT("Quantity"); Code = bDirect ? ECode::Invariant : ECode::InvalidNumber; }
        }
        if (Case.EndsWith(TEXT("Kind"))) { Row.ShopKind = static_cast<EY01ShopKind>(Case.EndsWith(TEXT("UnknownKind")) ? 255 : 3); Field = TEXT("ShopKind"); Code = ECode::InvalidEnum; }
        if (Field.IsEmpty()) { ExpectValid(); } else { ExpectError(EKind::ShopOffer, Code, Field); }
        return true;
    }
    AddError(TEXT("未知R36数据测试用例"));
    return false;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36S1SchemaTest, "Y01.Framework.R36.Data.S1.Schema",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

void FY01R36S1SchemaTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("Enums"), TEXT("GrowthRow"), TEXT("TierRow"), TEXT("TierEntry"),
        TEXT("Sources"), TEXT("Icons"), TEXT("Monster"), TEXT("SynthesisLegacy") })
    { Names.Add(Name); Commands.Add(Name); }
    for (const TCHAR* Name : { TEXT("FormalRowDefaults"), TEXT("MatchRulesFormal") }) { Names.Add(Name); Commands.Add(Name); }
}

bool FY01R36S1SchemaTest::RunTest(const FString& Case)
{
    FFixture F;
    TestEqual(TEXT("真实旧夹具19表"), F.Tables.Num(), 19);
    TestTrue(TEXT("无新增引用的旧目录仍合法"), FY01DataCatalog(F.Tables).Validate().IsValid());
    auto Property = [&](UStruct* Type, const TCHAR* Name) -> FProperty*
    {
        auto* P = Type ? FindFProperty<FProperty>(Type, Name) : nullptr;
        TestNotNull(*FString::Printf(TEXT("反射字段必须存在：%s"), Name), P);
        if (P) { TestTrue(TEXT("可编辑、蓝图可见且只读"), P->HasAllPropertyFlags(CPF_Edit | CPF_BlueprintVisible | CPF_BlueprintReadOnly)); }
        return P;
    };
    auto RefProperty = [&](FProperty* P)
    {
        const auto* S = CastField<FStructProperty>(P);
        TestTrue(TEXT("字段是显式FY01DataRef"), S && S->Struct == FY01DataRef::StaticStruct());
    };
    auto RowStruct = [&](const TCHAR* Name) -> UScriptStruct*
    {
        auto* S = FindObject<UScriptStruct>(nullptr, Name);
        TestNotNull(TEXT("已注册新增行结构"), S);
        if (S) { TestTrue(TEXT("新行继承FTableRowBase"), S->IsChildOf(FTableRowBase::StaticStruct())); }
        return S;
    };
    if (Case == TEXT("Enums"))
    {
        const TCHAR* Names[] = { TEXT("None"), TEXT("Wave"), TEXT("TowerFloor"), TEXT("TrainingStage"), TEXT("Monster"),
            TEXT("Skill"), TEXT("Talent"), TEXT("Equipment"), TEXT("LevelExperience"), TEXT("MetaLootPool"), TEXT("MatchRules"),
            TEXT("Character"), TEXT("RewardPackage"), TEXT("LocalItemPool"), TEXT("ShopOffer"), TEXT("Synthesis"), TEXT("Quality"),
            TEXT("MetaItem"), TEXT("WarehouseEvent"), TEXT("TalentChoice"), TEXT("SourceEffect"), TEXT("GrowthRule"), TEXT("QualityRollPool") };
        for (int32 I = 0; I < UE_ARRAY_COUNT(Names); ++I)
        { TestEqual(*FString::Printf(TEXT("表枚举 %s 固定序号"), Names[I]), StaticEnum<EKind>()->GetValueByNameString(Names[I]), int64(I)); }
        TestEqual(TEXT("RandomPool保留0"), StaticEnum<EY01ShopKind>()->GetValueByNameString(TEXT("RandomPool")), int64(0));
        TestEqual(TEXT("DirectEffect保留1"), StaticEnum<EY01ShopKind>()->GetValueByNameString(TEXT("DirectEffect")), int64(1));
        TestEqual(TEXT("RandomTier追加2"), StaticEnum<EY01ShopKind>()->GetValueByNameString(TEXT("RandomTier")), int64(2));
    }
    else if (Case == TEXT("GrowthRow"))
    {
        auto* S = RowStruct(TEXT("/Script/Y01.Y01GrowthRuleRow"));
        if (!S) { return false; }
        FStructOnScope Default(S);
        auto* Delta = CastField<FDoubleProperty>(Property(S, TEXT("Delta")));
        if (TestNotNull(TEXT("Delta为double"), Delta)) { TestEqual(TEXT("Delta默认零"), Delta->GetPropertyValue_InContainer(Default.GetStructMemory()), 0.0); }
        const TCHAR* Fields[] = { TEXT("Trigger"), TEXT("Target"), TEXT("Attribute") };
        const TCHAR* Enums[] = { TEXT("EY01GrowthTrigger"), TEXT("EY01GrowthTarget"), TEXT("EY01GrowthAttribute") };
        const TCHAR* Defaults[] = { TEXT("EffectiveKill"), TEXT("EquipmentInstance"), TEXT("AttackDamage") };
        for (int32 I = 0; I < 3; ++I)
        {
            const auto* P = CastField<FEnumProperty>(Property(S, Fields[I]));
            if (!TestNotNull(TEXT("成长字段是强类型反射枚举"), P)) { continue; }
            TestEqual(TEXT("枚举类型"), P->GetEnum()->GetName(), FString(Enums[I]));
            TestEqual(TEXT("默认枚举值为零"), P->GetUnderlyingProperty()->GetSignedIntPropertyValue(P->ContainerPtrToValuePtr<void>(Default.GetStructMemory())), int64(0));
            TestEqual(TEXT("默认枚举语义"), P->GetEnum()->GetValueByNameString(Defaults[I]), int64(0));
            if (I == 1) { TestEqual(TEXT("玩家目标"), P->GetEnum()->GetValueByNameString(TEXT("Player")), int64(1)); }
            if (I == 2) { TestEqual(TEXT("力量属性"), P->GetEnum()->GetValueByNameString(TEXT("Strength")), int64(1)); }
        }
    }
    else if (Case == TEXT("TierRow"))
    {
        auto* S = RowStruct(TEXT("/Script/Y01.Y01QualityRollPoolRow"));
        if (!S) { return false; }
        FStructOnScope Default(S);
        auto* Type = CastField<FEnumProperty>(Property(S, TEXT("ItemType")));
        TestTrue(TEXT("物品类型是表枚举"), Type && Type->GetEnum() == StaticEnum<EKind>());
        if (Type) { TestEqual(TEXT("默认装备"), Type->GetUnderlyingProperty()->GetSignedIntPropertyValue(Type->ContainerPtrToValuePtr<void>(Default.GetStructMemory())), int64(EKind::Equipment)); }
        const auto* Entries = CastField<FArrayProperty>(Property(S, TEXT("Entries")));
        const auto* Inner = Entries ? CastField<FStructProperty>(Entries->Inner) : nullptr;
        TestTrue(TEXT("品质条目结构数组"), Inner && Inner->Struct->GetName() == TEXT("Y01QualityRollEntry"));
        if (Entries) { FScriptArrayHelper Array(Entries, Entries->ContainerPtrToValuePtr<void>(Default.GetStructMemory())); TestEqual(TEXT("默认空条目"), Array.Num(), 0); }
    }
    else if (Case == TEXT("TierEntry"))
    {
        auto* S = FindObject<UScriptStruct>(nullptr, TEXT("/Script/Y01.Y01QualityRollEntry"));
        if (!TestNotNull(TEXT("品质条目必须存在"), S)) { return false; }
        FStructOnScope Default(S);
        auto* Weight = CastField<FDoubleProperty>(Property(S, TEXT("Weight")));
        if (TestNotNull(TEXT("权重为double"), Weight)) { TestEqual(TEXT("默认权重1"), Weight->GetPropertyValue_InContainer(Default.GetStructMemory()), 1.0); }
        RefProperty(Property(S, TEXT("Pool")));
        const auto* Q = CastField<FEnumProperty>(Property(S, TEXT("Quality")));
        TestTrue(TEXT("品质枚举"), Q && Q->GetEnum() == StaticEnum<EY01Quality>());
        if (Q) { TestEqual(TEXT("默认Green"), Q->GetUnderlyingProperty()->GetSignedIntPropertyValue(Q->ContainerPtrToValuePtr<void>(Default.GetStructMemory())), int64(0)); }
    }
    else if (Case == TEXT("Sources"))
    {
        for (auto* S : { FY01EquipmentRow::StaticStruct(), FY01SkillRow::StaticStruct(), FY01TalentRow::StaticStruct(), FY01ShopOfferRow::StaticStruct() })
        {
            FStructOnScope Default(S);
            const auto* A = CastField<FArrayProperty>(Property(S, TEXT("GrowthRuleIds")));
            if (TestNotNull(TEXT("成长显式引用数组"), A))
            {
                RefProperty(A->Inner);
                FScriptArrayHelper Array(A, A->ContainerPtrToValuePtr<void>(Default.GetStructMemory()));
                TestEqual(TEXT("默认无成长引用"), Array.Num(), 0);
            }
            auto* Name = CastField<FTextProperty>(Property(S, TEXT("DisplayName")));
            if (TestNotNull(TEXT("展示名是FText"), Name)) { TestTrue(TEXT("旧默认空名合法"), Name->GetPropertyValue_InContainer(Default.GetStructMemory()).IsEmpty()); }
        }
        auto* Tier = Property(FY01ShopOfferRow::StaticStruct(), TEXT("TierPool"));
        RefProperty(Tier);
        if (Tier)
        {
            FY01ShopOfferRow Default;
            const auto* Ref = Tier->ContainerPtrToValuePtr<FY01DataRef>(&Default);
            TestTrue(TEXT("TierPool默认完全空"), Ref->Type == EKind::None && Ref->RowName.IsNone());
        }
    }
    else if (Case == TEXT("Icons"))
    {
        for (auto* S : { FY01EquipmentRow::StaticStruct(), FY01SkillRow::StaticStruct() })
        {
            auto* P = CastField<FSoftObjectProperty>(Property(S, TEXT("Icon")));
            TestTrue(TEXT("图标是Texture2D软引用"), P && P->PropertyClass->GetFName() == TEXT("Texture2D"));
            if (P) { FStructOnScope Default(S); TestTrue(TEXT("图标默认可空"), P->GetPropertyValue_InContainer(Default.GetStructMemory()).IsNull()); }
        }
    }
    else if (Case == TEXT("Monster"))
    {
        auto* P = CastField<FDoubleProperty>(Property(FY01MonsterRow::StaticStruct(), TEXT("ItemDropChance")));
        if (TestNotNull(TEXT("掉率double"), P)) { FY01MonsterRow Row; TestEqual(TEXT("默认不掉物品"), P->GetPropertyValue_InContainer(&Row), 0.0); }
    }
    else if (Case == TEXT("SynthesisLegacy"))
    {
        int32 Count = 0;
        for (TFieldIterator<FProperty> It(FY01SynthesisRow::StaticStruct(), EFieldIteratorFlags::ExcludeSuper); It; ++It) { ++Count; }
        TestEqual(TEXT("合成新增入口字段且保留四个原字段"), Count, 6);
        for (const TCHAR* Name : { TEXT("InputType"), TEXT("InputQuality"), TEXT("InputCount"), TEXT("OutputPool") }) { Property(FY01SynthesisRow::StaticStruct(), Name); }
    }
    else if (Case == TEXT("FormalRowDefaults"))
    {
        for (UScriptStruct* S : { FY01ShopOfferRow::StaticStruct(), FY01SynthesisRow::StaticStruct() })
        {
            FStructOnScope Default(S);
            const auto* Enabled = CastField<FBoolProperty>(Property(S, TEXT("Enabled")));
            const auto* SortOrder = CastField<FIntProperty>(Property(S, TEXT("SortOrder")));
            if (Enabled) { TestTrue(TEXT("商品与合成默认启用"), Enabled->GetPropertyValue_InContainer(Default.GetStructMemory())); }
            if (SortOrder) { TestEqual(TEXT("商品与合成默认排序为零"), SortOrder->GetPropertyValue_InContainer(Default.GetStructMemory()), 0); }
        }
        {
            FY01WaveRow Default;
            const auto* Enabled = CastField<FBoolProperty>(Property(FY01WaveRow::StaticStruct(), TEXT("Enabled")));
            const auto* Order = CastField<FIntProperty>(Property(FY01WaveRow::StaticStruct(), TEXT("Order")));
            if (Enabled) { TestTrue(TEXT("波次默认启用"), Enabled->GetPropertyValue_InContainer(&Default)); }
            if (Order) { TestEqual(TEXT("波次默认顺序为零"), Order->GetPropertyValue_InContainer(&Default), 0); }
        }
        {
            FY01AttributeMappingRow Default;
            const auto* BaseEnabled = CastField<FBoolProperty>(Property(FY01AttributeMappingRow::StaticStruct(), TEXT("bBaseEnabled")));
            if (BaseEnabled) { TestFalse(TEXT("属性映射默认不作为基础映射"), BaseEnabled->GetPropertyValue_InContainer(&Default)); }
        }
    }
    else if (Case == TEXT("MatchRulesFormal"))
    {
        FY01MatchRulesRow Default;
        const auto* Startup = CastField<FStructProperty>(Property(FY01MatchRulesRow::StaticStruct(), TEXT("Startup")));
        const auto* Presentation = CastField<FStructProperty>(Property(FY01MatchRulesRow::StaticStruct(), TEXT("Presentation")));
        if (Startup)
        {
            for (const TCHAR* Name : { TEXT("Character"), TEXT("bTrainingEnabled"), TEXT("bR34AutoAttackEnabled"), TEXT("bEconomyEnabled"),
                TEXT("bCoreDefenseEnabled"), TEXT("TalentChoice"), TEXT("InitialGold"), TEXT("BaseGoldPerSecond"), TEXT("DropFlightSeconds") })
            { Property(Startup->Struct, Name); }
        }
        if (Presentation)
        {
            const void* Value = Presentation->ContainerPtrToValuePtr<void>(&Default);
            const auto* Damage = CastField<FStructProperty>(Property(Presentation->Struct, TEXT("DamageColor")));
            const auto* Healing = CastField<FStructProperty>(Property(Presentation->Struct, TEXT("HealingColor")));
            const auto* Merge = CastField<FDoubleProperty>(Property(Presentation->Struct, TEXT("RegenMergeSeconds")));
            if (Damage) { TestEqual(TEXT("伤害色默认红"), *Damage->ContainerPtrToValuePtr<FLinearColor>(Value), FLinearColor::Red); }
            if (Healing) { TestEqual(TEXT("治疗色默认绿"), *Healing->ContainerPtrToValuePtr<FLinearColor>(Value), FLinearColor::Green); }
            if (Merge) { TestEqual(TEXT("自然回血默认一秒合并"), Merge->GetPropertyValue_InContainer(Value), 1.0); }
        }
    }
    else { AddError(TEXT("未知schema用例")); return false; }
    return true;
}

namespace
{
TArray<FY01DataRef>& GrowthRefs(FFixture& F, EKind Kind)
{
    switch (Kind)
    {
    case EKind::Equipment: return F.Row<FY01EquipmentRow>(Kind).GrowthRuleIds;
    case EKind::Skill: return F.Row<FY01SkillRow>(Kind).GrowthRuleIds;
    case EKind::Talent: return F.Row<FY01TalentRow>(Kind).GrowthRuleIds;
    default: return F.Row<FY01ShopOfferRow>(Kind).GrowthRuleIds;
    }
}

void AddS1Tables(FFixture& F, EKind ItemType = EKind::Equipment)
{
    AddSourceEffects(F);
    FY01GrowthRuleRow Growth;
    F.Add(EKind::GrowthRule, Growth, TEXT("Equipment"));
    Growth.Target = EY01GrowthTarget::Player; Growth.Attribute = EY01GrowthAttribute::Strength; Growth.Delta = 0.25;
    F.Add(EKind::GrowthRule, Growth, TEXT("Player"));
    F.Add(EKind::GrowthRule, Growth, TEXT("Other"));
    if (ItemType == EKind::Skill)
    {
        auto Skill = F.Row<FY01SkillRow>(EKind::Skill); Skill.Quality = EY01Quality::Blue;
        F.Add(EKind::Skill, Skill, TEXT("Blue"));
        for (FName Name : { FName(TEXT("Default")), FName(TEXT("Blue")) })
        {
            auto& Pool = F.Row<FY01LocalItemPoolRow>(EKind::LocalItemPool, Name);
            Pool.ItemType = EKind::Skill; Pool.Entries[0].Item.Type = EKind::Skill;
        }
        F.Row<FY01SynthesisRow>(EKind::Synthesis).InputType = EKind::Skill;
    }
    FY01QualityRollPoolRow Pool; Pool.ItemType = ItemType;
    FY01QualityRollEntry Entry; Entry.Pool = FFixture::Ref(EKind::LocalItemPool); Pool.Entries.Add(Entry);
    Entry.Quality = EY01Quality::Blue; Entry.Pool.RowName = TEXT("Blue"); Pool.Entries.Add(Entry);
    F.Add(EKind::QualityRollPool, Pool);
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36S1CatalogTest, "Y01.Framework.R36.Data.S1.Catalog",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)

void FY01R36S1CatalogTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    auto Add = [&](const FString& Name) { Names.Add(Name); Commands.Add(Name); };
    for (const TCHAR* Kind : { TEXT("Growth"), TEXT("Tier") })
        for (const TCHAR* Case : { TEXT("Absent"), TEXT("Valid"), TEXT("Empty"), TEXT("NullTable"), TEXT("WrongSchema"),
            TEXT("NullSchema"), TEXT("NullRow"), TEXT("WrongTypedQuery") }) { Add(FString(TEXT("Optional.")) + Kind + TEXT(".") + Case); }
    for (const TCHAR* Case : { TEXT("None"), TEXT("Sentinel"), TEXT("Unknown") }) { Add(FString(TEXT("Registration.")) + Case); }
    for (const TCHAR* Target : { TEXT("Equipment"), TEXT("Player") })
        for (const TCHAR* Case : { TEXT("Zero"), TEXT("Fraction"), TEXT("MaxFinite"), TEXT("Negative"), TEXT("NaN"), TEXT("Infinity"),
            TEXT("NegativeInfinity"), TEXT("TriggerUnknown"), TEXT("TriggerSentinel"), TEXT("TargetUnknown"), TEXT("TargetSentinel"),
            TEXT("AttributeUnknown"), TEXT("AttributeSentinel"), TEXT("Mismatch") }) { Add(FString(TEXT("Growth.")) + Target + TEXT(".") + Case); }
    for (const TCHAR* Source : { TEXT("Equipment"), TEXT("Skill"), TEXT("Talent"), TEXT("ShopOffer") })
        for (const TCHAR* Case : { TEXT("Valid"), TEXT("MissingTable"), TEXT("NullTable"), TEXT("WrongSchema"), TEXT("NullSchema"),
            TEXT("NoneType"), TEXT("WrongType"), TEXT("UnknownType"), TEXT("MissingRow"), TEXT("EmptyName"), TEXT("EmptyRef"),
            TEXT("Duplicate"), TEXT("DuplicateCaseInsensitive"), TEXT("SharedOtherRow"), TEXT("EmptyTable"), TEXT("EquipmentTarget") })
        { Add(FString(TEXT("GrowthRef.")) + Source + TEXT(".") + Case); }
    for (const TCHAR* Case : { TEXT("Equipment.Valid"), TEXT("Equipment.AttackBonus"), TEXT("Equipment.Effect"), TEXT("Equipment.Both"),
        TEXT("Active.Valid"), TEXT("Active.MissingAbility"), TEXT("Active.Effect"), TEXT("Active.Both"), TEXT("Passive.Valid"),
        TEXT("Passive.Ability"), TEXT("Passive.Effect"), TEXT("Passive.Cooldown"), TEXT("Talent.Valid"), TEXT("Talent.Ability"), TEXT("Talent.Effect") })
    { Add(FString(TEXT("Mixed.")) + Case); }
    Add(TEXT("SharedCrossSources")); Add(TEXT("PresentationSoftOnly"));
    for (const TCHAR* Type : { TEXT("Equipment"), TEXT("Skill") })
        for (const TCHAR* Case : { TEXT("Valid"), TEXT("Empty"), TEXT("TypeNone"), TEXT("TypeTalent"), TEXT("TypeMeta"), TEXT("TypeUnknown"),
            TEXT("Fraction"), TEXT("MaxFinite"), TEXT("ZeroEntry"), TEXT("Negative"), TEXT("NaN"), TEXT("Infinity"), TEXT("NegativeInfinity"),
            TEXT("ZeroTotal"), TEXT("Overflow"), TEXT("Duplicate"), TEXT("QualityUnknown"), TEXT("QualitySentinel"), TEXT("PoolQuality"),
            TEXT("PoolType"), TEXT("MissingTable"), TEXT("NullTable"), TEXT("WrongSchema"), TEXT("NoneType"), TEXT("WrongType"),
            TEXT("UnknownType"), TEXT("EmptyName"), TEXT("EmptyRef"), TEXT("MissingRow"), TEXT("EmptyTable"), TEXT("ZeroMissingRow"),
            TEXT("EmptyLocalPool"), TEXT("LocalQuantityWeight"), TEXT("LocalMissingItem") })
        { Add(FString(TEXT("Tier.")) + Type + TEXT(".") + Case); }
    for (const TCHAR* Case : { TEXT("Valid"), TEXT("Skill"), TEXT("PoolBoth"), TEXT("PoolTypeOnly"), TEXT("PoolNameOnly"), TEXT("PoolUnknownType"),
        TEXT("Effects"), TEXT("Growth"), TEXT("QualityBlue"), TEXT("QualityPurple"), TEXT("QualityGold"), TEXT("QualityRed"), TEXT("QualityUnknown"),
        TEXT("QuantityZero"), TEXT("QuantityNegative"), TEXT("QuantityTwo"), TEXT("QuantityMax"), TEXT("PriceNegative"), TEXT("PriceMax"),
        TEXT("MissingTable"), TEXT("NullTable"), TEXT("WrongSchema"), TEXT("NoneType"), TEXT("WrongType"), TEXT("UnknownType"),
        TEXT("EmptyName"), TEXT("EmptyRef"), TEXT("MissingRow"), TEXT("EmptyTable"), TEXT("IllegalItemType") })
    { Add(FString(TEXT("ShopTier.")) + Case); }
    for (const TCHAR* Kind : { TEXT("Random"), TEXT("Direct") })
        for (const TCHAR* Case : { TEXT("TierBoth"), TEXT("TierTypeOnly"), TEXT("TierNameOnly"), TEXT("TierUnknownType"), TEXT("GrowthOnly"), TEXT("BothEffects") })
        { Add(FString(TEXT("ShopCompat.")) + Kind + TEXT(".") + Case); }
    for (const TCHAR* Case : { TEXT("Zero"), TEXT("Fraction"), TEXT("One"), TEXT("Negative"), TEXT("AboveOne"), TEXT("NaN"), TEXT("Infinity"),
        TEXT("NegativeInfinity"), TEXT("MissingRewardAtZero"), TEXT("MissingReward"), TEXT("EmptyReward"), TEXT("RewardWrongType"), TEXT("RewardNullTable"),
        TEXT("RewardWrongSchema"), TEXT("Gold"), TEXT("Experience"), TEXT("Domain"), TEXT("EmptyItems"), TEXT("ItemMissing"), TEXT("ItemWrongType"),
        TEXT("QuantityZero"), TEXT("QuantityNegative"), TEXT("PoolReward"), TEXT("SkillReward") }) { Add(FString(TEXT("Drop.")) + Case); }
    for (const TCHAR* Case : { TEXT("ShopOffer"), TEXT("Synthesis"), TEXT("Wave"), TEXT("AttributeMapping") })
    { Add(FString(TEXT("Formal.Scan.")) + Case); }
    for (const TCHAR* Case : { TEXT("DamageColor"), TEXT("HealingColor"), TEXT("RegenMergeSeconds") })
    { Add(FString(TEXT("Formal.Validation.Presentation.")) + Case); }
    for (const TCHAR* Case : { TEXT("Character"), TEXT("InitialGold"), TEXT("BaseGoldPerSecond"), TEXT("DropFlightSeconds") })
    { Add(FString(TEXT("Formal.Validation.Startup.")) + Case); }
}

bool FY01R36S1CatalogTest::RunTest(const FString& Case)
{
    static_assert(std::is_same_v<decltype(FY01GrowthRuleRow::Delta), double>);
    static_assert(std::is_same_v<decltype(FY01QualityRollEntry::Weight), double>);
    static_assert(std::is_same_v<decltype(FY01MonsterRow::ItemDropChance), double>);
    FFixture F;
    TestEqual(TEXT("复用真19表"), F.Tables.Num(), 19);
    if (!TestTrue(TEXT("每例先校验未改动旧数据"), FY01DataCatalog(F.Tables).Validate().IsValid())) { return false; }
    AddS1Tables(F, Case.Contains(TEXT("Tier.Skill.")) || Case == TEXT("ShopTier.Skill") ? EKind::Skill : EKind::Equipment);
    auto Valid = [&]()
    {
        const auto R = FY01DataCatalog(F.Tables).Validate();
        TestTrue(*FString::Printf(TEXT("%s 真实目录必须接受"), *Case), R.IsValid());
        for (const auto& E : R.Errors) { AddInfo(FString::Printf(TEXT("表%d 行%s 字段%s 错误%d %s"), int32(E.Source.Type), *E.Source.RowName.ToString(), *E.Field, int32(E.Code), *E.Message)); }
    };
    auto Error = [&](EKind Kind, ECode Code, const FString& Field, FName Name = TEXT("Default"))
    {
        const auto R = FY01DataCatalog(F.Tables).Validate();
        TestFalse(*FString::Printf(TEXT("%s 非法数据必须拒绝"), *Case), R.IsValid());
        TestTrue(*FString::Printf(TEXT("%s 必须定位表%d 行%s 字段%s 错误%d"), *Case, int32(Kind), *Name.ToString(), *Field, int32(Code)),
            R.Errors.ContainsByPredicate([&](const FY01DataError& E) { return E.Source.Type == Kind && E.Source.RowName == Name && E.Field == Field && E.Code == Code && !E.Message.IsEmpty(); }));
    };
    auto Is = [&](const TCHAR* Suffix) { return Case.EndsWith(FString(TEXT(".")) + Suffix); };
    auto Number = [&]() -> double
    {
        if (Is(TEXT("Negative"))) { return -0.25; }
        if (Is(TEXT("NaN"))) { return std::numeric_limits<double>::quiet_NaN(); }
        if (Is(TEXT("NegativeInfinity"))) { return -std::numeric_limits<double>::infinity(); }
        return std::numeric_limits<double>::infinity();
    };
    auto NullSchema = [&](EKind Kind) { auto* T = NewObject<UDataTable>(); F.Owners.Emplace(T); F.Tables[Kind] = T; };
    auto BadRef = [&](FY01DataRef& Ref, EKind Kind, ECode& Code)
    {
        Code = ECode::InvalidReference;
        if (Is(TEXT("MissingTable"))) { F.Tables.Remove(Kind); Code = ECode::MissingTable; }
        else if (Is(TEXT("NullTable"))) { F.Tables[Kind] = nullptr; Code = ECode::MissingTable; }
        else if (Is(TEXT("WrongSchema"))) { F.Tables[Kind] = F.Tables[EKind::Equipment]; Code = ECode::WrongSchema; }
        else if (Is(TEXT("NullSchema"))) { NullSchema(Kind); Code = ECode::WrongSchema; }
        else if (Is(TEXT("EmptyTable"))) { const_cast<UDataTable*>(F.Tables[Kind])->EmptyTable(); }
        else if (Is(TEXT("NoneType"))) { Ref.Type = EKind::None; }
        else if (Is(TEXT("WrongType"))) { Ref.Type = EKind::Equipment; Code = ECode::TypeMismatch; }
        else if (Is(TEXT("UnknownType"))) { Ref.Type = static_cast<EKind>(255); Code = ECode::TypeMismatch; }
        else if (Is(TEXT("EmptyName"))) { Ref.RowName = NAME_None; }
        else if (Is(TEXT("EmptyRef"))) { Ref = FY01DataRef(); }
        else if (Is(TEXT("MissingRow")) || Is(TEXT("ZeroMissingRow"))) { Ref.RowName = TEXT("Missing"); }
        else { AddError(TEXT("未识别引用变异")); }
    };
    const TSoftClassPtr<UGameplayAbility> Ability(FSoftObjectPath(TEXT("/Game/Tests/UnloadedS1GA.UnloadedS1GA_C")));
    const TSoftClassPtr<UGameplayEffect> Effect(FSoftObjectPath(TEXT("/Game/Tests/UnloadedS1GE.UnloadedS1GE_C")));
    if (Case.StartsWith(TEXT("Optional.")))
    {
        const EKind Kind = Case.Contains(TEXT(".Growth.")) ? EKind::GrowthRule : EKind::QualityRollPool;
        const FName Name = Kind == EKind::GrowthRule ? FName(TEXT("Player")) : FName(TEXT("Default"));
        if (Is(TEXT("Absent"))) { F.Tables.Remove(Kind); Valid(); }
        else if (Is(TEXT("Empty"))) { const_cast<UDataTable*>(F.Tables[Kind])->EmptyTable(); Valid(); }
        else if (Is(TEXT("NullTable"))) { F.Tables[Kind] = nullptr; Error(Kind, ECode::MissingTable, TEXT("Table"), NAME_None); }
        else if (Is(TEXT("WrongSchema"))) { F.Tables[Kind] = F.Tables[EKind::Equipment]; Error(Kind, ECode::WrongSchema, TEXT("RowStruct"), NAME_None); }
        else if (Is(TEXT("NullSchema"))) { NullSchema(Kind); Error(Kind, ECode::WrongSchema, TEXT("RowStruct"), NAME_None); }
        else if (Is(TEXT("NullRow")))
        {
            auto& Rows = const_cast<TMap<FName, uint8*>&>(F.Tables[Kind]->GetRowMap());
            TGuardValue<uint8*> Guard(Rows.FindChecked(Name), nullptr);
            Error(Kind, ECode::InvalidReference, TEXT("RowName"), Name);
        }
        else
        {
            FY01DataValidation Q; FY01DataCatalog Catalog(F.Tables);
            if (Is(TEXT("WrongTypedQuery")))
            {
                TestNull(TEXT("不允许跨schema强转"), Catalog.FindRow<FY01EquipmentRow>(FFixture::Ref(Kind, Name), Q));
                TestTrue(TEXT("类型查询精确报错"), Q.Errors.ContainsByPredicate([&](const auto& E) { return E.Code == ECode::TypeMismatch && E.Source.Type == Kind && E.Source.RowName == Name && E.Field == TEXT("Reference.Type"); }));
            }
            else
            {
                Valid();
                if (Kind == EKind::GrowthRule)
                { const auto* R = Catalog.FindRow<FY01GrowthRuleRow>(FFixture::Ref(Kind, Name), Q); if (TestNotNull(TEXT("成长可类型化查询"), R)) { TestEqual(TEXT("double小数保留"), R->Delta, 0.25); } }
                else
                { const auto* R = Catalog.FindRow<FY01QualityRollPoolRow>(FFixture::Ref(Kind, Name), Q); if (TestNotNull(TEXT("品质池可类型化查询"), R)) { TestEqual(TEXT("保留两品质"), R->Entries.Num(), 2); } }
                TestTrue(TEXT("查询无错误"), Q.IsValid());
            }
        }
        return true;
    }
    if (Case.StartsWith(TEXT("Registration.")))
    {
        const EKind Kind = static_cast<EKind>(Is(TEXT("None")) ? 0 : Is(TEXT("Sentinel")) ? 25 : 255);
        F.Tables.Add(Kind, F.Tables[EKind::Equipment]); Error(Kind, ECode::InvalidEnum, TEXT("Tables"), NAME_None); return true;
    }
    if (Case.StartsWith(TEXT("Growth.")))
    {
        const FName Name = Case.Contains(TEXT(".Player.")) ? FName(TEXT("Player")) : FName(TEXT("Equipment"));
        auto& R = F.Row<FY01GrowthRuleRow>(EKind::GrowthRule, Name);
        if (Is(TEXT("Zero"))) { R.Delta = 0; Valid(); }
        else if (Is(TEXT("Fraction"))) { R.Delta = 0.125; Valid(); }
        else if (Is(TEXT("MaxFinite"))) { R.Delta = std::numeric_limits<double>::max(); Valid(); }
        else if (Case.Contains(TEXT(".Trigger"))) { R.Trigger = static_cast<EY01GrowthTrigger>(Is(TEXT("TriggerUnknown")) ? 255 : 1); Error(EKind::GrowthRule, ECode::InvalidEnum, TEXT("Trigger"), Name); }
        else if (Case.Contains(TEXT(".Target"))) { R.Target = static_cast<EY01GrowthTarget>(Is(TEXT("TargetUnknown")) ? 255 : 2); Error(EKind::GrowthRule, ECode::InvalidEnum, TEXT("Target"), Name); }
        else if (Case.Contains(TEXT(".Attribute"))) { R.Attribute = static_cast<EY01GrowthAttribute>(Is(TEXT("AttributeUnknown")) ? 255 : 2); Error(EKind::GrowthRule, ECode::InvalidEnum, TEXT("Attribute"), Name); }
        else if (Is(TEXT("Mismatch"))) { R.Attribute = R.Target == EY01GrowthTarget::Player ? EY01GrowthAttribute::AttackDamage : EY01GrowthAttribute::Strength; Error(EKind::GrowthRule, ECode::Invariant, TEXT("Attribute"), Name); }
        else { R.Delta = Number(); Error(EKind::GrowthRule, ECode::InvalidNumber, TEXT("Delta"), Name); }
        return true;
    }
    if (Case == TEXT("SharedCrossSources"))
    {
        DirectOffer(F);
        for (EKind Kind : { EKind::Equipment, EKind::Skill, EKind::Talent, EKind::ShopOffer }) { GrowthRefs(F, Kind) = { FFixture::Ref(EKind::GrowthRule, TEXT("Player")) }; }
        Valid(); return true;
    }
    if (Case == TEXT("PresentationSoftOnly"))
    {
        const TSoftObjectPtr<UTexture2D> Icon(FSoftObjectPath(TEXT("/Game/Tests/UnloadedS1Icon.UnloadedS1Icon")));
        auto& Equip = F.Row<FY01EquipmentRow>(EKind::Equipment); Equip.Icon = Icon; Equip.DisplayName = FText::FromString(TEXT("装备"));
        auto& Skill = F.Row<FY01SkillRow>(EKind::Skill); Skill.Icon = Icon; Skill.DisplayName = FText::FromString(TEXT("技能")); Skill.Ability = Ability;
        F.Row<FY01TalentRow>(EKind::Talent).DisplayName = FText::FromString(TEXT("天赋"));
        F.Row<FY01ShopOfferRow>(EKind::ShopOffer).DisplayName = FText::FromString(TEXT("商品"));
        Valid(); TestFalse(TEXT("不加载图标"), Icon.IsValid()); TestFalse(TEXT("不加载软类"), Ability.IsValid()); return true;
    }
    if (Case.StartsWith(TEXT("GrowthRef.")))
    {
        const EKind Kind = Case.Contains(TEXT(".Equipment.")) ? EKind::Equipment : Case.Contains(TEXT(".Skill.")) ? EKind::Skill : Case.Contains(TEXT(".Talent.")) ? EKind::Talent : EKind::ShopOffer;
        if (Kind == EKind::ShopOffer) { DirectOffer(F); SourceRefs(F, Kind).Reset(); }
        auto& Refs = GrowthRefs(F, Kind); Refs = { FFixture::Ref(EKind::GrowthRule, TEXT("Player")), FFixture::Ref(EKind::GrowthRule, TEXT("Other")) };
        if (Is(TEXT("Valid"))) { Valid(); return true; }
        if (Is(TEXT("SharedOtherRow"))) { CopySource(F, Kind); Valid(); return true; }
        ECode Code = ECode::InvalidReference;
        if (Is(TEXT("EquipmentTarget")))
        {
            Refs[1].RowName = TEXT("Equipment");
            if (Kind == EKind::Equipment) { Valid(); return true; }
            Code = ECode::Invariant;
        }
        else if (Is(TEXT("Duplicate")) || Is(TEXT("DuplicateCaseInsensitive")))
        { Refs[1] = Refs[0]; if (Is(TEXT("DuplicateCaseInsensitive"))) { Refs[1].RowName = TEXT("pLaYeR"); } Code = ECode::DuplicateValue; }
        else { BadRef(Refs[1], EKind::GrowthRule, Code); }
        Error(Kind, Code, TEXT("GrowthRuleIds[1]")); return true;
    }
    if (Case.StartsWith(TEXT("Mixed.")))
    {
        const EKind Kind = Case.Contains(TEXT(".Equipment.")) ? EKind::Equipment : Case.Contains(TEXT(".Talent.")) ? EKind::Talent : EKind::Skill;
        GrowthRefs(F, Kind) = { FFixture::Ref(EKind::GrowthRule, TEXT("Player")) };
        if (Is(TEXT("Both"))) { SourceRefs(F, Kind) = { FFixture::Ref(EKind::SourceEffect, TEXT("Attack")) }; }
        FString Field;
        if (Kind == EKind::Equipment)
        {
            auto& R = F.Row<FY01EquipmentRow>(Kind);
            if (Is(TEXT("AttackBonus"))) { R.AttackBonus = 1; Field = TEXT("AttackBonus"); }
            if (Is(TEXT("Effect"))) { R.Effect = Effect; Field = TEXT("Effect"); }
        }
        else if (Kind == EKind::Talent)
        {
            auto& R = F.Row<FY01TalentRow>(Kind);
            if (Is(TEXT("Ability"))) { R.Ability = Ability; Field = TEXT("Ability"); }
            if (Is(TEXT("Effect"))) { R.Effect = Effect; Field = TEXT("Effect"); }
        }
        else
        {
            auto& R = F.Row<FY01SkillRow>(Kind); const bool bPassive = Case.Contains(TEXT(".Passive."));
            R.Type = bPassive ? EY01SkillType::Passive : EY01SkillType::Active; R.Ability = bPassive ? TSoftClassPtr<UGameplayAbility>() : Ability;
            if (Is(TEXT("MissingAbility"))) { R.Ability.Reset(); Field = TEXT("Ability"); }
            if (Is(TEXT("Ability"))) { R.Ability = Ability; Field = TEXT("Ability"); }
            if (Is(TEXT("Effect"))) { R.Effect = Effect; Field = TEXT("Effect"); }
            if (Is(TEXT("Cooldown"))) { R.CooldownSeconds = 0.25; Field = TEXT("CooldownSeconds"); }
        }
        if (Field.IsEmpty()) { Valid(); } else { Error(Kind, ECode::Invariant, Field); }
        TestFalse(TEXT("不加载GA"), Ability.IsValid()); TestFalse(TEXT("不加载GE"), Effect.IsValid()); return true;
    }
    if (Case.StartsWith(TEXT("Tier.")))
    {
        auto& R = F.Row<FY01QualityRollPoolRow>(EKind::QualityRollPool); auto& Entry = R.Entries[1];
        ECode Code = ECode::InvalidPool; FString Field = TEXT("Entries");
        if (Is(TEXT("Valid"))) { Valid(); return true; }
        if (Is(TEXT("Fraction"))) { Entry.Weight = 0.125; Valid(); return true; }
        if (Is(TEXT("MaxFinite"))) { R.Entries[0].Weight = 0; Entry.Weight = std::numeric_limits<double>::max(); Valid(); return true; }
        if (Is(TEXT("ZeroEntry"))) { Entry.Weight = 0; Valid(); return true; }
        if (Case.Contains(TEXT(".Type")))
        { R.ItemType = Is(TEXT("TypeNone")) ? EKind::None : Is(TEXT("TypeTalent")) ? EKind::Talent : Is(TEXT("TypeMeta")) ? EKind::MetaItem : static_cast<EKind>(255); Field = TEXT("ItemType"); Code = ECode::TypeMismatch; }
        else if (Is(TEXT("Empty"))) { R.Entries.Reset(); }
        else if (Is(TEXT("ZeroTotal"))) { R.Entries[0].Weight = 0; Entry.Weight = 0; }
        else if (Is(TEXT("Overflow"))) { R.Entries[0].Weight = std::numeric_limits<double>::max(); Entry.Weight = std::numeric_limits<double>::max(); }
        else if (Is(TEXT("Negative")) || Is(TEXT("NaN")) || Is(TEXT("Infinity")) || Is(TEXT("NegativeInfinity"))) { Entry.Weight = Number(); Field = TEXT("Entries[1].Weight"); Code = ECode::InvalidNumber; }
        else if (Is(TEXT("Duplicate"))) { Entry.Quality = R.Entries[0].Quality; Entry.Pool = R.Entries[0].Pool; Field = TEXT("Entries[1].Quality"); Code = ECode::DuplicateValue; }
        else if (Is(TEXT("QualityUnknown")) || Is(TEXT("QualitySentinel"))) { Entry.Quality = static_cast<EY01Quality>(Is(TEXT("QualityUnknown")) ? 255 : 5); Field = TEXT("Entries[1].Quality"); Code = ECode::InvalidEnum; }
        else if (Is(TEXT("PoolQuality"))) { Entry.Pool.RowName = TEXT("Default"); Field = TEXT("Entries[1].Pool.Quality"); Code = ECode::QualityMismatch; }
        else if (Is(TEXT("PoolType")))
        { F.Row<FY01LocalItemPoolRow>(EKind::LocalItemPool, TEXT("Blue")).ItemType = R.ItemType == EKind::Equipment ? EKind::Skill : EKind::Equipment; Field = TEXT("Entries[1].Pool.ItemType"); Code = ECode::TypeMismatch; }
        else if (Is(TEXT("EmptyLocalPool")) || Is(TEXT("LocalQuantityWeight")) || Is(TEXT("LocalMissingItem")))
        {
            auto& P = F.Row<FY01LocalItemPoolRow>(EKind::LocalItemPool, TEXT("Blue"));
            if (Is(TEXT("EmptyLocalPool"))) { P.Entries.Reset(); Error(EKind::LocalItemPool, ECode::InvalidPool, TEXT("Entries"), TEXT("Blue")); }
            else if (Is(TEXT("LocalQuantityWeight"))) { P.Entries[0].Weight = -1; Error(EKind::LocalItemPool, ECode::InvalidNumber, TEXT("Entries[0].Weight"), TEXT("Blue")); }
            else { P.Entries[0].Item.RowName = TEXT("Missing"); Error(EKind::LocalItemPool, ECode::InvalidReference, TEXT("Entries[0].Item"), TEXT("Blue")); }
            return true;
        }
        else
        {
            if (Is(TEXT("ZeroMissingRow"))) { Entry.Weight = 0; }
            BadRef(Entry.Pool, EKind::LocalItemPool, Code); Field = TEXT("Entries[1].Pool");
            if (Is(TEXT("MissingTable")) || Is(TEXT("NullTable"))) { Error(EKind::LocalItemPool, Code, TEXT("Table"), NAME_None); return true; }
            if (Is(TEXT("WrongSchema"))) { Error(EKind::LocalItemPool, Code, TEXT("RowStruct"), NAME_None); return true; }
            if (Is(TEXT("EmptyTable"))) { Error(EKind::LocalItemPool, ECode::EmptyTable, TEXT("Rows"), NAME_None); return true; }
        }
        Error(EKind::QualityRollPool, Code, Field); return true;
    }
    if (Case.StartsWith(TEXT("ShopTier.")) || Case.StartsWith(TEXT("ShopCompat.")))
    {
        auto& R = F.Row<FY01ShopOfferRow>(EKind::ShopOffer); FString Field; ECode Code = ECode::Invariant;
        if (Case.StartsWith(TEXT("ShopTier.")))
        {
            R.ShopKind = EY01ShopKind::RandomTier; R.Pool = FY01DataRef(); R.TierPool = FFixture::Ref(EKind::QualityRollPool);
            if (Is(TEXT("PoolBoth"))) { R.Pool = FFixture::Ref(EKind::LocalItemPool); Field = TEXT("Pool"); }
            else if (Is(TEXT("PoolTypeOnly"))) { R.Pool.Type = EKind::LocalItemPool; Field = TEXT("Pool"); }
            else if (Is(TEXT("PoolNameOnly"))) { R.Pool.RowName = TEXT("Default"); Field = TEXT("Pool"); }
            else if (Is(TEXT("PoolUnknownType"))) { R.Pool.Type = static_cast<EKind>(255); Field = TEXT("Pool"); }
            else if (Is(TEXT("Effects"))) { R.EffectIds = { FFixture::Ref(EKind::SourceEffect, TEXT("Attack")) }; Field = TEXT("EffectIds"); }
            else if (Is(TEXT("Growth"))) { R.GrowthRuleIds = { FFixture::Ref(EKind::GrowthRule, TEXT("Player")) }; Field = TEXT("GrowthRuleIds"); }
            else if (Case.Contains(TEXT(".Quality")))
            { R.Quality = Is(TEXT("QualityBlue")) ? EY01Quality::Blue : Is(TEXT("QualityPurple")) ? EY01Quality::Purple : Is(TEXT("QualityGold")) ? EY01Quality::Gold : Is(TEXT("QualityRed")) ? EY01Quality::Red : static_cast<EY01Quality>(255); Field = TEXT("Quality"); if (Is(TEXT("QualityUnknown"))) { Code = ECode::InvalidEnum; } }
            else if (Case.Contains(TEXT(".Quantity")))
            { R.Quantity = Is(TEXT("QuantityZero")) ? 0 : Is(TEXT("QuantityNegative")) ? -1 : Is(TEXT("QuantityTwo")) ? 2 : MAX_int32; if (R.Quantity <= 0) { Field = TEXT("Quantity"); Code = ECode::InvalidNumber; } }
            else if (Is(TEXT("PriceNegative"))) { R.Price = -1; Field = TEXT("Price"); Code = ECode::InvalidNumber; }
            else if (Is(TEXT("PriceMax"))) { R.Price = MAX_int32; }
            else if (Is(TEXT("IllegalItemType"))) { F.Row<FY01QualityRollPoolRow>(EKind::QualityRollPool).ItemType = EKind::Talent; Field = TEXT("TierPool.ItemType"); Code = ECode::TypeMismatch; }
            else if (!Is(TEXT("Valid")) && !Is(TEXT("Skill"))) { BadRef(R.TierPool, EKind::QualityRollPool, Code); Field = TEXT("TierPool"); }
        }
        else
        {
            const bool bDirect = Case.Contains(TEXT(".Direct.")); if (bDirect) { DirectOffer(F); }
            if (Is(TEXT("GrowthOnly")) || Is(TEXT("BothEffects")))
            {
                R.GrowthRuleIds = { FFixture::Ref(EKind::GrowthRule, TEXT("Player")) };
                if (Is(TEXT("GrowthOnly"))) { R.EffectIds.Reset(); } else { R.EffectIds = { FFixture::Ref(EKind::SourceEffect, TEXT("Attack")) }; }
                if (!bDirect) { Field = TEXT("GrowthRuleIds"); }
            }
            else
            {
                if (Is(TEXT("TierBoth"))) { R.TierPool = FFixture::Ref(EKind::QualityRollPool); }
                else if (Is(TEXT("TierTypeOnly"))) { R.TierPool.Type = EKind::QualityRollPool; }
                else if (Is(TEXT("TierNameOnly"))) { R.TierPool.RowName = TEXT("Default"); }
                else { R.TierPool.Type = static_cast<EKind>(255); }
                Field = TEXT("TierPool");
            }
        }
        if (Field.IsEmpty()) { Valid(); } else { Error(EKind::ShopOffer, Code, Field); }
        return true;
    }
    if (Case.StartsWith(TEXT("Formal.Scan.")))
    {
        const EKind Kind = Case.EndsWith(TEXT("ShopOffer")) ? EKind::ShopOffer : Case.EndsWith(TEXT("Synthesis")) ? EKind::Synthesis
            : Case.EndsWith(TEXT("Wave")) ? EKind::Wave : EKind::AttributeMapping;
        if (Kind == EKind::AttributeMapping)
        {
            FY01AttributeMappingRow A; A.bBaseEnabled = true; F.Add(Kind, A, TEXT("Zulu"));
            F.Add(Kind, FY01AttributeMappingRow(), TEXT("Disabled"));
            A.bBaseEnabled = true; F.Add(Kind, A, TEXT("Alpha"));
        }
        else if (Kind == EKind::ShopOffer)
        {
            auto A = F.Row<FY01ShopOfferRow>(Kind); A.SortOrder = 20; F.Add(Kind, A, TEXT("Zulu"));
            A.Enabled = false; A.SortOrder = -10; F.Add(Kind, A, TEXT("Disabled"));
            A.Enabled = true; A.SortOrder = 20; F.Add(Kind, A, TEXT("Alpha"));
        }
        else if (Kind == EKind::Synthesis)
        {
            auto A = F.Row<FY01SynthesisRow>(Kind); A.SortOrder = 20; F.Add(Kind, A, TEXT("Zulu"));
            A.Enabled = false; A.SortOrder = -10; F.Add(Kind, A, TEXT("Disabled"));
            A.Enabled = true; A.SortOrder = 20; F.Add(Kind, A, TEXT("Alpha"));
        }
        else
        {
            auto A = F.Row<FY01WaveRow>(Kind); A.Order = 20; F.Add(Kind, A, TEXT("Zulu"));
            A.Enabled = false; A.Order = -10; F.Add(Kind, A, TEXT("Disabled"));
            A.Enabled = true; A.Order = 20; F.Add(Kind, A, TEXT("Alpha"));
        }
        FY01DataValidation ScanErrors;
        const TArray<FY01DataRef> Rows = FY01DataCatalog(F.Tables).CollectEnabled(Kind, ScanErrors);
        TestTrue(TEXT("正式表扫描无错误"), ScanErrors.IsValid());
        TestFalse(TEXT("禁用行不进入正式入口"), Rows.ContainsByPredicate([](const FY01DataRef& R) { return R.RowName == TEXT("Disabled"); }));
        const int32 Alpha = Rows.IndexOfByPredicate([](const FY01DataRef& R) { return R.RowName == TEXT("Alpha"); });
        const int32 Zulu = Rows.IndexOfByPredicate([](const FY01DataRef& R) { return R.RowName == TEXT("Zulu"); });
        TestTrue(TEXT("同序号按RowName稳定排序且新增行自动发现"), Alpha != INDEX_NONE && Zulu != INDEX_NONE && Alpha < Zulu);
        return true;
    }
    if (Case.StartsWith(TEXT("Formal.Validation.Presentation.")))
    {
        auto& P = F.Row<FY01MatchRulesRow>(EKind::MatchRules).Presentation;
        const FString Field = Case.EndsWith(TEXT("DamageColor")) ? TEXT("Presentation.DamageColor")
            : Case.EndsWith(TEXT("HealingColor")) ? TEXT("Presentation.HealingColor") : TEXT("Presentation.RegenMergeSeconds");
        if (Case.EndsWith(TEXT("DamageColor"))) { P.DamageColor.R = std::numeric_limits<float>::quiet_NaN(); }
        else if (Case.EndsWith(TEXT("HealingColor"))) { P.HealingColor.G = std::numeric_limits<float>::infinity(); }
        else { P.RegenMergeSeconds = 0; }
        Error(EKind::MatchRules, ECode::InvalidNumber, Field);
        return true;
    }
    if (Case.StartsWith(TEXT("Formal.Validation.Startup.")))
    {
        auto& S = F.Row<FY01MatchRulesRow>(EKind::MatchRules).Startup;
        FString Field; ECode Code = ECode::InvalidNumber;
        if (Case.EndsWith(TEXT("Character"))) { S.Character = FFixture::Ref(EKind::Equipment); Field = TEXT("Startup.Character"); Code = ECode::TypeMismatch; }
        else if (Case.EndsWith(TEXT("InitialGold"))) { S.InitialGold = -1; Field = TEXT("Startup.InitialGold"); }
        else if (Case.EndsWith(TEXT("BaseGoldPerSecond"))) { S.BaseGoldPerSecond = -1; Field = TEXT("Startup.BaseGoldPerSecond"); }
        else { S.DropFlightSeconds = std::numeric_limits<double>::infinity(); Field = TEXT("Startup.DropFlightSeconds"); }
        Error(EKind::MatchRules, Code, Field);
        return true;
    }
    if (Case.StartsWith(TEXT("Drop.")))
    {
        auto& R = F.Row<FY01MonsterRow>(EKind::Monster);
        FY01RewardPackageRow Reward; FY01RewardEntry Item; Item.Item = FFixture::Ref(EKind::Equipment); Reward.Items.Add(Item);
        F.Add(EKind::RewardPackage, Reward, TEXT("Drop")); auto& Package = F.Row<FY01RewardPackageRow>(EKind::RewardPackage, TEXT("Drop"));
        R.DropReward = FFixture::Ref(EKind::RewardPackage, TEXT("Drop")); R.ItemDropChance = 0.25;
        ECode Code = ECode::InvalidNumber; FString Field = TEXT("ItemDropChance");
        if (Is(TEXT("Zero"))) { R.ItemDropChance = 0; R.DropReward = FFixture::Ref(EKind::RewardPackage); Valid(); return true; }
        if (Is(TEXT("Fraction"))) { Valid(); return true; }
        if (Is(TEXT("One"))) { R.ItemDropChance = 1; Valid(); return true; }
        if (Is(TEXT("PoolReward")) || Is(TEXT("SkillReward"))) { Package.Items[0].Item.Type = Is(TEXT("PoolReward")) ? EKind::LocalItemPool : EKind::Skill; Valid(); return true; }
        if (Is(TEXT("AboveOne"))) { R.ItemDropChance = 1.00001; }
        else if (Is(TEXT("MissingReward")) || Is(TEXT("MissingRewardAtZero"))) { if (Is(TEXT("MissingRewardAtZero"))) { R.ItemDropChance = 0; } R.DropReward.RowName = TEXT("Missing"); Field = TEXT("DropReward"); Code = ECode::InvalidReference; }
        else if (Is(TEXT("EmptyReward"))) { R.DropReward = FY01DataRef(); Field = TEXT("DropReward"); Code = ECode::InvalidReference; }
        else if (Is(TEXT("RewardWrongType"))) { R.DropReward.Type = EKind::Equipment; Field = TEXT("DropReward"); Code = ECode::TypeMismatch; }
        else if (Is(TEXT("RewardNullTable"))) { F.Tables[EKind::RewardPackage] = nullptr; Error(EKind::RewardPackage, ECode::MissingTable, TEXT("Table"), NAME_None); return true; }
        else if (Is(TEXT("RewardWrongSchema"))) { F.Tables[EKind::RewardPackage] = F.Tables[EKind::Equipment]; Error(EKind::RewardPackage, ECode::WrongSchema, TEXT("RowStruct"), NAME_None); return true; }
        else if (Is(TEXT("Gold"))) { Package.Gold = 1; Field = TEXT("DropReward.Gold"); Code = ECode::Invariant; }
        else if (Is(TEXT("Experience"))) { Package.Experience = 1; Field = TEXT("DropReward.Experience"); Code = ECode::Invariant; }
        else if (Is(TEXT("Domain"))) { Package.Domain = EY01RewardDomain::MetaPublic; Package.Items[0].Item.Type = EKind::MetaItem; Field = TEXT("DropReward.Domain"); Code = ECode::Invariant; }
        else if (Is(TEXT("EmptyItems"))) { Package.Items.Reset(); Field = TEXT("DropReward.Items"); Code = ECode::Invariant; }
        else if (Is(TEXT("ItemMissing"))) { Package.Items[0].Item.RowName = TEXT("Missing"); Error(EKind::RewardPackage, ECode::InvalidReference, TEXT("Items[0]"), TEXT("Drop")); return true; }
        else if (Is(TEXT("ItemWrongType"))) { Package.Items[0].Item.Type = EKind::MetaItem; Error(EKind::RewardPackage, ECode::TypeMismatch, TEXT("Items[0]"), TEXT("Drop")); return true; }
        else if (Is(TEXT("QuantityZero")) || Is(TEXT("QuantityNegative"))) { Package.Items[0].Quantity = Is(TEXT("QuantityZero")) ? 0 : -1; Error(EKind::RewardPackage, ECode::InvalidNumber, TEXT("Items[0].Quantity"), TEXT("Drop")); return true; }
        else { R.ItemDropChance = Number(); }
        Error(EKind::Monster, Code, Field); return true;
    }
    AddError(TEXT("未知S1 Catalog用例")); return false;
}
#endif
