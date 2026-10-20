#include "Data/Y01DataCatalog.h"
#include "Combat/Y01DamageEffects.h"

namespace
{
using EKind = EY01TableKind;
using ECode = EY01DataErrorCode;

const UScriptStruct* Schema(EKind Kind)
{
    switch (Kind)
    {
    case EKind::Wave: return FY01WaveRow::StaticStruct();
    case EKind::TowerFloor: return FY01TowerFloorRow::StaticStruct();
    case EKind::TrainingStage: return FY01TrainingStageRow::StaticStruct();
    case EKind::Monster: return FY01MonsterRow::StaticStruct();
    case EKind::Skill: return FY01SkillRow::StaticStruct();
    case EKind::Talent: return FY01TalentRow::StaticStruct();
    case EKind::Equipment: return FY01EquipmentRow::StaticStruct();
    case EKind::LevelExperience: return FY01LevelExperienceRow::StaticStruct();
    case EKind::MetaLootPool: return FY01MetaLootPoolRow::StaticStruct();
    case EKind::MatchRules: return FY01MatchRulesRow::StaticStruct();
    case EKind::Character: return FY01CharacterRow::StaticStruct();
    case EKind::RewardPackage: return FY01RewardPackageRow::StaticStruct();
    case EKind::LocalItemPool: return FY01LocalItemPoolRow::StaticStruct();
    case EKind::ShopOffer: return FY01ShopOfferRow::StaticStruct();
    case EKind::Synthesis: return FY01SynthesisRow::StaticStruct();
    case EKind::Quality: return FY01QualityRow::StaticStruct();
    case EKind::MetaItem: return FY01MetaItemRow::StaticStruct();
    case EKind::WarehouseEvent: return FY01WarehouseEventRow::StaticStruct();
    case EKind::TalentChoice: return FY01TalentChoiceRow::StaticStruct();
    case EKind::SourceEffect: return FY01SourceEffectRow::StaticStruct();
    case EKind::GrowthRule: return FY01GrowthRuleRow::StaticStruct();
    case EKind::QualityRollPool: return FY01QualityRollPoolRow::StaticStruct();
    case EKind::AttributeMapping: return FY01AttributeMappingRow::StaticStruct();
    case EKind::DamageEffect: return FY01DamageEffectRow::StaticStruct();
    case EKind::Consumable: return FY01ConsumableRow::StaticStruct();
    default: return nullptr;
    }
}

FY01DataRef Identity(EKind Type, FName Name = NAME_None)
{
    FY01DataRef Ref;
    Ref.Type = Type;
    Ref.RowName = Name;
    return Ref;
}

void AddDataError(FY01DataValidation& Result, ECode Code, const FY01DataRef& Source, const FString& Field, const FString& Message)
{
    Result.Errors.Add({Code, Source, Field, Message});
}

bool IsLocalItem(EKind Kind)
{
    return Kind == EKind::Equipment || Kind == EKind::Skill;
}

struct FValidator
{
    const FY01DataCatalog& Catalog;
    FY01DataValidation& Result;
    FY01DataRef Source;

    void Check(bool bCondition, ECode Code, const FString& Field, const TCHAR* Message) const
    {
        if (!bCondition) { AddDataError(Result, Code, Source, Field, Message); }
    }

    void Number(double Value, const FString& Field, bool bPositive = false) const
    {
        Check(FMath::IsFinite(Value) && (bPositive ? Value > 0 : Value >= 0), ECode::InvalidNumber, Field,
            bPositive ? TEXT("数值必须有限且大于零") : TEXT("数值必须有限且非负"));
    }

    template <typename T>
    void Enum(T Value, const FString& Field) const
    {
        Check(StaticEnum<T>()->IsValidEnumValue(static_cast<int64>(Value)) &&
            static_cast<int64>(Value) < StaticEnum<T>()->GetMaxEnumValue(), ECode::InvalidEnum, Field, TEXT("枚举值无效"));
    }

    void Quality(EY01Quality Value, const FString& Field = TEXT("Quality")) const
    {
        Enum(Value, Field);
    }

    template <typename T>
    const T* Reference(const FY01DataRef& Ref, EKind Kind, const FString& Field) const
    {
        if (Ref.RowName.IsNone() || Ref.Type == EKind::None)
        {
            AddDataError(Result, ECode::InvalidReference, Source, Field, TEXT("引用须同时指定类型与 RowName"));
            return nullptr;
        }
        if (Ref.Type != Kind)
        {
            AddDataError(Result, ECode::TypeMismatch, Source, Field, TEXT("引用类型与字段要求不匹配"));
            return nullptr;
        }
        FY01DataValidation QueryErrors;
        const T* Row = Catalog.FindRow<T>(Ref, QueryErrors);
        for (auto& Issue : QueryErrors.Errors)
        {
            AddDataError(Result, Issue.Code, Source, Field, FString::Printf(TEXT("引用 %s：%s"), *Ref.RowName.ToString(), *Issue.Message));
        }
        return Row;
    }

    void SourceEffects(const TArray<FY01DataRef>& Refs) const
    {
        TSet<FName> Seen;
        for (int32 Index = 0; Index < Refs.Num(); ++Index)
        {
            const auto& Ref = Refs[Index];
            const FString Field = FString::Printf(TEXT("EffectIds[%d]"), Index);
            if (const auto* Effect = Reference<FY01SourceEffectRow>(Ref, EKind::SourceEffect, Field))
            {
                Check((Source.Type == EKind::Equipment && Effect->Kind != EY01SourceEffectKind::ShieldOnCast)
                    || Effect->Kind == EY01SourceEffectKind::AttackFlat
                    || Effect->Kind == EY01SourceEffectKind::GoldPerSecond
                    || (Source.Type == EKind::Skill && Effect->Kind == EY01SourceEffectKind::ShieldOnCast), ECode::Invariant, Field,
                    TEXT("暴击、倍率、吸血与周期护盾首版仅接通装备；施放护盾仅接通技能"));
            }
            if (Ref.Type == EKind::SourceEffect && !Ref.RowName.IsNone())
            {
                Check(!Seen.Contains(Ref.RowName), ECode::DuplicateValue, Field, TEXT("同一内容不能重复引用同一效果ID"));
                Seen.Add(Ref.RowName);
            }
        }
    }

    void DamageEffects(const TArray<FY01DataRef>& Refs, EY01SkillType Type = EY01SkillType::Passive) const
    {
        TSet<FName> Seen;
        for (int32 I = 0; I < Refs.Num(); ++I)
        {
            const auto& Ref = Refs[I]; const FString Field = FString::Printf(TEXT("DamageEffectIds[%d]"), I);
            Check(!Seen.Contains(Ref.RowName), ECode::DuplicateValue, Field, TEXT("伤害效果ID不得重复")); Seen.Add(Ref.RowName);
            if (const auto* Row = Reference<FY01DamageEffectRow>(Ref, EKind::DamageEffect, Field))
            {
                FY01ItemDamageEffect E; E.EffectId = Ref.RowName; E.Config = *Row;
                Check(Y01Damage::ValidateSource({E}, Source.Type, Type), ECode::Invariant, Field, TEXT("伤害模板无效或来源发动方式不支持"));
            }
        }
    }

    void GrowthRules(const TArray<FY01DataRef>& Refs) const
    {
        TSet<FName> Seen;
        for (int32 Index = 0; Index < Refs.Num(); ++Index)
        {
            const auto& Ref = Refs[Index];
            const FString Field = FString::Printf(TEXT("GrowthRuleIds[%d]"), Index);
            if (const auto* Row = Reference<FY01GrowthRuleRow>(Ref, EKind::GrowthRule, Field))
            {
                Check(Row->Target != EY01GrowthTarget::EquipmentInstance || Source.Type == EKind::Equipment,
                    ECode::Invariant, Field, TEXT("装备实例成长只能由装备来源引用"));
            }
            if (Ref.Type == EKind::GrowthRule && !Ref.RowName.IsNone())
            {
                Check(!Seen.Contains(Ref.RowName), ECode::DuplicateValue, Field, TEXT("同一内容不能重复引用同一成长规则"));
                Seen.Add(Ref.RowName);
            }
        }
    }

    void Item(const FY01DataRef& Ref, EKind Expected, EY01Quality ExpectedQuality, const FString& Field) const
    {
        const EY01Quality* Actual = nullptr;
        switch (Expected)
        {
        case EKind::Equipment:
            if (const auto* Row = Reference<FY01EquipmentRow>(Ref, Expected, Field)) { Actual = &Row->Quality; }
            break;
        case EKind::Skill:
            if (const auto* Row = Reference<FY01SkillRow>(Ref, Expected, Field)) { Actual = &Row->Quality; }
            break;
        case EKind::MetaItem:
            if (const auto* Row = Reference<FY01MetaItemRow>(Ref, Expected, Field)) { Actual = &Row->Quality; }
            break;
        default: AddDataError(Result, ECode::TypeMismatch, Source, Field, TEXT("此字段不支持该物品类型")); break;
        }
        if (Actual)
        {
            Quality(*Actual, Field);
            Check(*Actual == ExpectedQuality, ECode::QualityMismatch, Field, TEXT("引用物品品质不匹配"));
        }
    }

    double Pool(const TArray<FY01WeightedEntry>& Entries, EKind Kind, EY01Quality PoolQuality) const
    {
        Quality(PoolQuality);
        Check(!Entries.IsEmpty(), ECode::InvalidPool, TEXT("Entries"), TEXT("抽样池不能为空"));
        double Total = 0;
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            const auto& Entry = Entries[Index];
            const FString Field = FString::Printf(TEXT("Entries[%d]"), Index);
            Number(Entry.Weight, Field + TEXT(".Weight"));
            Item(Entry.Item, Kind, PoolQuality, Field + TEXT(".Item"));
            Total += static_cast<double>(Entry.Weight);
        }
        Check(FMath::IsFinite(Total) && Total > 0, ECode::InvalidPool, TEXT("Entries"), TEXT("池总权重必须有限且大于零"));
        return Total;
    }

    void Spawns(const TArray<FY01SpawnEntry>& Entries) const
    {
        Check(!Entries.IsEmpty(), ECode::InvalidPool, TEXT("Spawns"), TEXT("刷怪列表不能为空"));
        for (int32 Index = 0; Index < Entries.Num(); ++Index)
        {
            const auto& Entry = Entries[Index];
            const FString Field = FString::Printf(TEXT("Spawns[%d]"), Index);
            Reference<FY01MonsterRow>(Entry.Monster, EKind::Monster, Field + TEXT(".Monster"));
            Number(Entry.Count, Field + TEXT(".Count"), true);
            Number(Entry.DelaySeconds, Field + TEXT(".DelaySeconds"));
        }
    }

    template <typename T>
    void Stats(const T& Row) const
    {
        Number(Row.Health, TEXT("Health"), true);
        Number(Row.AttackDamage, TEXT("AttackDamage"));
        Number(Row.Range, TEXT("Range"));
        Number(Row.MoveSpeed, TEXT("MoveSpeed"));
        Number(Row.AttackInterval, TEXT("AttackInterval"), true);
        Enum(Row.AttackType, TEXT("AttackType"));
        Number(Row.MeleeAngle, TEXT("MeleeAngle"), true);
        Check(Row.MeleeAngle <= 360, ECode::InvalidNumber, TEXT("MeleeAngle"), TEXT("完整扇形角度不能超过360度"));
        Number(Row.ProjectileSpeed, TEXT("ProjectileSpeed"), true);
        Number(Row.Armor, TEXT("Armor")); Number(Row.HealthRegen, TEXT("HealthRegen"));
    }
};
}

const void* FY01DataCatalog::FindRaw(const FY01DataRef& Ref, const UScriptStruct* Expected, FY01DataValidation& OutErrors) const
{
    const UScriptStruct* Required = Schema(Ref.Type);
    if (!Required || Ref.RowName.IsNone())
    {
        AddDataError(OutErrors, ECode::InvalidReference, Ref, TEXT("Reference"), TEXT("引用类型或 RowName 无效"));
        return nullptr;
    }
    if (Expected != Required)
    {
        AddDataError(OutErrors, ECode::TypeMismatch, Ref, TEXT("Reference.Type"), TEXT("查询行结构与显式引用类型不匹配"));
        return nullptr;
    }
    const UDataTable* const* Table = Tables.Find(Ref.Type);
    if (!Table || !*Table)
    {
        AddDataError(OutErrors, ECode::MissingTable, Ref, TEXT("Table"), TEXT("所需数据表未配置或加载失败"));
        return nullptr;
    }
    if ((*Table)->GetRowStruct() != Required)
    {
        AddDataError(OutErrors, ECode::WrongSchema, Ref, TEXT("RowStruct"), TEXT("表的行结构与登记类型不匹配"));
        return nullptr;
    }
    const uint8* const* Row = (*Table)->GetRowMap().Find(Ref.RowName);
    if (!Row || !*Row)
    {
        AddDataError(OutErrors, ECode::InvalidReference, Ref, TEXT("RowName"), TEXT("引用行不存在"));
        return nullptr;
    }
    return *Row;
}

FY01DataValidation FY01DataCatalog::Validate() const
{
    FY01DataValidation Result;
    for (const auto& Pair : Tables)
    {
        if (!Schema(Pair.Key)) { AddDataError(Result, ECode::InvalidEnum, Identity(Pair.Key), TEXT("Tables"), TEXT("数据表登记类型无效")); }
    }
    for (uint8 Index = 1; Index <= static_cast<uint8>(EKind::TalentChoice); ++Index)
    {
        const EKind Kind = static_cast<EKind>(Index);
        const UDataTable* const* Table = Tables.Find(Kind);
        if (!Table || !*Table) { AddDataError(Result, ECode::MissingTable, Identity(Kind), TEXT("Table"), TEXT("正式数据表缺失")); }
        else if ((*Table)->GetRowStruct() != Schema(Kind)) { AddDataError(Result, ECode::WrongSchema, Identity(Kind), TEXT("RowStruct"), TEXT("数据表行结构错误")); }
        else if ((*Table)->GetRowMap().IsEmpty()) { AddDataError(Result, ECode::EmptyTable, Identity(Kind), TEXT("Rows"), TEXT("正式数据表不能为空")); }
    }
    // 结构错误先返回，避免在错误结构上解释内存。
    if (!Result.IsValid()) { return Result; }

    // 可选表只允许未登记；已登记的空指针或错误结构仍须报错，并继续定位引用来源。
    for (EKind Kind : { EKind::SourceEffect, EKind::GrowthRule, EKind::QualityRollPool, EKind::AttributeMapping, EKind::DamageEffect, EKind::Consumable })
    {
        if (const UDataTable* const* Table = Tables.Find(Kind))
        {
            if (!*Table) { AddDataError(Result, ECode::MissingTable, Identity(Kind), TEXT("Table"), TEXT("可选表已配置但加载失败")); }
            else if ((*Table)->GetRowStruct() != Schema(Kind))
            { AddDataError(Result, ECode::WrongSchema, Identity(Kind), TEXT("RowStruct"), TEXT("可选表行结构错误")); }
        }
    }

    auto Visit = [&]<typename T>(EKind Kind, TFunctionRef<void(const T&, const FValidator&)> Callback)
    {
        const UDataTable* Table = Tables.FindRef(Kind);
        if (!Table || Table->GetRowStruct() != T::StaticStruct()) { return; }
        TArray<FName> Names = Table->GetRowNames();
        Names.Sort(FNameLexicalLess());
        for (FName Name : Names)
        {
            const FY01DataRef Source = Identity(Kind, Name);
            if (const T* Row = FindRow<T>(Source, Result)) { Callback(*Row, FValidator{*this, Result, Source}); }
        }
    };
    Visit.operator()<FY01SourceEffectRow>(EKind::SourceEffect, [](const auto& Row, const FValidator& V)
    {
        V.Enum(Row.Kind, TEXT("Kind"));
        V.Number(Row.Magnitude, TEXT("Magnitude"));
        V.Enum(Row.Condition, TEXT("Condition"));
        V.Number(Row.ConditionThreshold, TEXT("ConditionThreshold"));
        V.Number(Row.DurationSeconds, TEXT("DurationSeconds"));
        V.Number(Row.LayerCap, TEXT("LayerCap"));
        V.Number(Row.IntervalSeconds, TEXT("IntervalSeconds"));
        V.Check(Y01Data::ValidSourceEffect(Row), ECode::Invariant, TEXT("Attribute/Amount"),
            TEXT("效果字段须匹配种类且数值有限非负；百分比仅支持三维、除每秒金币外的衍生及金币获得，金币获得不支持固定值或映射；生命条件仅支持固定值或百分比的攻击、护甲、生命恢复、攻速及金币获得，阈值须为0至100，无条件时阈值须为零；护盾数值、持续时间、本层上限与间隔须为正，施放护盾间隔须为零"));
    });
    Visit.operator()<FY01DamageEffectRow>(EKind::DamageEffect, [](const auto& Row, const FValidator& V)
    {
        V.Enum(Row.Trigger, TEXT("Trigger")); V.Enum(Row.Targeting, TEXT("Targeting")); V.Enum(Row.Order, TEXT("Order"));
        V.Enum(Row.Center, TEXT("Center")); V.Enum(Row.Product, TEXT("Product")); V.Enum(Row.Position, TEXT("Position")); V.Enum(Row.Stacking, TEXT("Stacking"));
        V.Number(Row.SearchRadius, TEXT("SearchRadius")); V.Number(Row.Radius, TEXT("Radius")); V.Number(Row.FixedDamage, TEXT("FixedDamage"));
        V.Number(Row.PeriodSeconds, TEXT("PeriodSeconds"), Row.Trigger == EY01DamageTrigger::Periodic);
        V.Number(Row.Lifetime, TEXT("Lifetime")); V.Number(Row.TickSeconds, TEXT("TickSeconds")); V.Number(Row.Speed, TEXT("Speed"));
        V.Check(Row.IsValid(), ECode::Invariant, TEXT("DamageEffect"), TEXT("伤害模板的公式、时间、产物、位置或叠加组合无效"));
    });
    Visit.operator()<FY01AttributeMappingRow>(EKind::AttributeMapping, [](const auto& Row, const FValidator& V)
    {
        V.Check(Row.IsValid(), ECode::Invariant, TEXT("SourceAttribute/TargetAttribute/Coefficient"), TEXT("映射只允许三维到已接通衍生，系数须有限非负"));
    });
    Visit.operator()<FY01ConsumableRow>(EKind::Consumable, [](const auto& Row, const FValidator& V)
    {
        V.Enum(Row.Kind, TEXT("Kind")); V.Quality(Row.Quality);
        V.Check(Row.IsValid(), ECode::Invariant, TEXT("DisplayName"), TEXT("局内道具须为已接通种类且显示名非空"));
    });
    Visit.operator()<FY01GrowthRuleRow>(EKind::GrowthRule, [](const auto& Row, const FValidator& V)
    {
        V.Enum(Row.Trigger, TEXT("Trigger")); V.Enum(Row.Target, TEXT("Target")); V.Enum(Row.Attribute, TEXT("Attribute"));
        V.Number(Row.Delta, TEXT("Delta"));
        V.Check((Row.Target == EY01GrowthTarget::EquipmentInstance && Row.Attribute == EY01GrowthAttribute::AttackDamage)
            || (Row.Target == EY01GrowthTarget::Player && Row.Attribute != EY01GrowthAttribute::AttackDamage),
            ECode::Invariant, TEXT("Attribute"), TEXT("装备实例成长仅支持攻击；玩家侧成长支持力量/生命上限/敏捷/智慧"));
    });
    Visit.operator()<FY01QualityRollPoolRow>(EKind::QualityRollPool, [](const auto& Row, const FValidator& V)
    {
        V.Check(IsLocalItem(Row.ItemType), ECode::TypeMismatch, TEXT("ItemType"), TEXT("品质抽样池仅支持装备或技能"));
        V.Check(!Row.Entries.IsEmpty(), ECode::InvalidPool, TEXT("Entries"), TEXT("品质抽样池不能为空"));
        TSet<EY01Quality> Seen;
        double Total = 0;
        for (int32 Index = 0; Index < Row.Entries.Num(); ++Index)
        {
            const auto& Entry = Row.Entries[Index];
            const FString Field = FString::Printf(TEXT("Entries[%d]"), Index);
            V.Quality(Entry.Quality, Field + TEXT(".Quality"));
            V.Check(!Seen.Contains(Entry.Quality), ECode::DuplicateValue, Field + TEXT(".Quality"), TEXT("品质抽样池内品质不得重复"));
            Seen.Add(Entry.Quality);
            V.Number(Entry.Weight, Field + TEXT(".Weight"));
            Total += Entry.Weight;
            if (const auto* Pool = V.Reference<FY01LocalItemPoolRow>(Entry.Pool, EKind::LocalItemPool, Field + TEXT(".Pool")))
            {
                V.Check(IsLocalItem(Pool->ItemType) && Pool->ItemType == Row.ItemType,
                    ECode::TypeMismatch, Field + TEXT(".Pool.ItemType"), TEXT("品质条目引用池须与物品类型一致"));
                V.Quality(Pool->Quality, Field + TEXT(".Pool.Quality"));
                V.Check(Pool->Quality == Entry.Quality, ECode::QualityMismatch, Field + TEXT(".Pool.Quality"), TEXT("品质条目引用池须与品质一致"));
            }
        }
        V.Check(FMath::IsFinite(Total) && Total > 0, ECode::InvalidPool, TEXT("Entries"), TEXT("品质池总权重必须有限且大于零"));
    });
    TSet<EY01Quality> Qualities;
    Visit.operator()<FY01QualityRow>(EKind::Quality, [&](const auto& Row, const FValidator& V)
    {
        V.Quality(Row.Quality);
        V.Check(!Qualities.Contains(Row.Quality), ECode::DuplicateValue, TEXT("Quality"), TEXT("品质定义重复"));
        Qualities.Add(Row.Quality);
    });
    for (uint8 Q = 0; Q <= static_cast<uint8>(EY01Quality::Red); ++Q)
    {
        if (!Qualities.Contains(static_cast<EY01Quality>(Q)))
        {
            AddDataError(Result, ECode::MissingQuality, Identity(EKind::Quality), TEXT("Quality"), FString::Printf(TEXT("缺少品质 %d"), Q));
        }
    }
    Visit.operator()<FY01SkillRow>(EKind::Skill, [](const auto& Row, const FValidator& V)
    {
        V.Enum(Row.Type, TEXT("Type")); V.Quality(Row.Quality); V.Number(Row.CooldownSeconds, TEXT("CooldownSeconds"));
        V.Check(Row.TestSkill.IsValid(), ECode::InvalidNumber, TEXT("TestSkill"), TEXT("程序测试技能配置须有限、非负，启用的几何与持续时间须为正"));
        V.Check(Row.TestSkill.Mode == EY01TestSkillMode::None || Row.Type == EY01SkillType::Active,
            ECode::Invariant, TEXT("TestSkill.Mode"), TEXT("程序测试攻击必须是主动技能"));
        V.SourceEffects(Row.EffectIds);
        V.GrowthRules(Row.GrowthRuleIds); V.DamageEffects(Row.DamageEffectIds, Row.Type);
        V.Check(Row.DamageEffectIds.IsEmpty() || Row.TestSkill.Mode == EY01TestSkillMode::None,
            ECode::Invariant, TEXT("TestSkill"), TEXT("共享伤害模板不能混入旧测试攻击"));
        if (Row.Type == EY01SkillType::Active)
        { V.Check((!Row.DamageEffectIds.IsEmpty()) == (Row.Ability.ToSoftObjectPath() == FSoftObjectPath(UY01DamageAbility::StaticClass())),
            ECode::Invariant, TEXT("Ability"), TEXT("主动伤害效果必须且只能使用原生DamageAbility")); }
        if (!Row.EffectIds.IsEmpty() || !Row.GrowthRuleIds.IsEmpty() || !Row.DamageEffectIds.IsEmpty())
        {
            V.Check(Row.Effect.IsNull(), ECode::Invariant, TEXT("Effect"), TEXT("效果ID不能与旧技能Effect混填"));
            if (Row.Type == EY01SkillType::Active)
            {
                V.Check(!Row.Ability.IsNull(), ECode::Invariant, TEXT("Ability"), TEXT("带效果ID的主动技能仍须配置Ability"));
            }
            else if (Row.Type == EY01SkillType::Passive)
            {
                V.Check(Row.Ability.IsNull(), ECode::Invariant, TEXT("Ability"), TEXT("效果ID被动技能不能混填Ability"));
                V.Check(Row.CooldownSeconds == 0, ECode::Invariant, TEXT("CooldownSeconds"), TEXT("效果ID被动技能冷却须为零"));
            }
        }
    });
    Visit.operator()<FY01EquipmentRow>(EKind::Equipment, [](const auto& Row, const FValidator& V)
    {
        V.Quality(Row.Quality); V.Number(Row.AttackBonus, TEXT("AttackBonus"));
        V.SourceEffects(Row.EffectIds);
        V.GrowthRules(Row.GrowthRuleIds); V.DamageEffects(Row.DamageEffectIds);
        if (!Row.EffectIds.IsEmpty() || !Row.GrowthRuleIds.IsEmpty() || !Row.DamageEffectIds.IsEmpty())
        {
            V.Check(Row.AttackBonus == 0, ECode::Invariant, TEXT("AttackBonus"), TEXT("效果ID装备的旧AttackBonus须为零"));
            V.Check(Row.Effect.IsNull(), ECode::Invariant, TEXT("Effect"), TEXT("效果ID不能与旧装备Effect混填"));
        }
    });
    Visit.operator()<FY01TalentRow>(EKind::Talent, [](const auto& Row, const FValidator& V)
    {
        V.Quality(Row.Quality);
        V.SourceEffects(Row.EffectIds);
        V.GrowthRules(Row.GrowthRuleIds);
        if (!Row.EffectIds.IsEmpty() || !Row.GrowthRuleIds.IsEmpty())
        {
            V.Check(Row.Ability.IsNull(), ECode::Invariant, TEXT("Ability"), TEXT("效果ID不能与旧天赋Ability混填"));
            V.Check(Row.Effect.IsNull(), ECode::Invariant, TEXT("Effect"), TEXT("效果ID不能与旧天赋Effect混填"));
        }
    });
    Visit.operator()<FY01MetaItemRow>(EKind::MetaItem, [](const auto& Row, const FValidator& V) { V.Quality(Row.Quality); });
    Visit.operator()<FY01LocalItemPoolRow>(EKind::LocalItemPool, [](const auto& Row, const FValidator& V)
    {
        V.Check(IsLocalItem(Row.ItemType), ECode::TypeMismatch, TEXT("ItemType"), TEXT("局内池仅支持装备或技能"));
        V.Pool(Row.Entries, Row.ItemType, Row.Quality);
    });
    Visit.operator()<FY01MetaLootPoolRow>(EKind::MetaLootPool, [](const auto& Row, const FValidator& V)
    {
        V.Pool(Row.Entries, EKind::MetaItem, Row.Quality);
    });
    Visit.operator()<FY01RewardPackageRow>(EKind::RewardPackage, [](const auto& Row, const FValidator& V)
    {
        V.Enum(Row.Domain, TEXT("Domain")); V.Number(Row.Experience, TEXT("Experience")); V.Number(Row.Gold, TEXT("Gold"));
        for (int32 I = 0; I < Row.Items.Num(); ++I)
        {
            const auto& Entry = Row.Items[I];
            const FString Field = FString::Printf(TEXT("Items[%d]"), I);
            V.Number(Entry.Quantity, Field + TEXT(".Quantity"), true);
            if (Row.Domain == EY01RewardDomain::MatchPersonal)
            {
                switch (Entry.Item.Type)
                {
                case EKind::Equipment: V.Reference<FY01EquipmentRow>(Entry.Item, EKind::Equipment, Field); break;
                case EKind::Skill: V.Reference<FY01SkillRow>(Entry.Item, EKind::Skill, Field); break;
                case EKind::LocalItemPool: V.Reference<FY01LocalItemPoolRow>(Entry.Item, EKind::LocalItemPool, Field); break;
                case EKind::Consumable: V.Reference<FY01ConsumableRow>(Entry.Item, EKind::Consumable, Field); break;
                default: V.Check(false, ECode::TypeMismatch, Field, TEXT("个人局内奖励类型无效")); break;
                }
            }
            else if (Row.Domain == EY01RewardDomain::MetaPublic)
            {
                switch (Entry.Item.Type)
                {
                case EKind::MetaItem: V.Reference<FY01MetaItemRow>(Entry.Item, EKind::MetaItem, Field); break;
                case EKind::MetaLootPool: V.Reference<FY01MetaLootPoolRow>(Entry.Item, EKind::MetaLootPool, Field); break;
                default: V.Check(false, ECode::TypeMismatch, Field, TEXT("公共局外奖励类型无效")); break;
                }
            }
        }
    });
    Visit.operator()<FY01MonsterRow>(EKind::Monster, [](const auto& Row, const FValidator& V)
    {
        V.Stats(Row); V.Number(Row.Experience, TEXT("Experience")); V.Number(Row.Gold, TEXT("Gold"));
        V.Number(Row.DetectionRadius, TEXT("DetectionRadius"));
        for (int32 I = 0; I < Row.Skills.Num(); ++I)
        {
            V.Reference<FY01SkillRow>(Row.Skills[I], EKind::Skill, FString::Printf(TEXT("Skills[%d]"), I));
        }
        V.Number(Row.ItemDropChance, TEXT("ItemDropChance"));
        V.Check(Row.ItemDropChance <= 1, ECode::InvalidNumber, TEXT("ItemDropChance"), TEXT("物品掉率不得超过1"));
        if (const auto* Reward = V.Reference<FY01RewardPackageRow>(Row.DropReward, EKind::RewardPackage, TEXT("DropReward")))
        {
            if (Row.ItemDropChance > 0)
            {
                V.Check(Reward->Domain == EY01RewardDomain::MatchPersonal, ECode::Invariant, TEXT("DropReward.Domain"), TEXT("物品掉落奖励必须属于局内个人"));
                V.Check(Reward->Gold == 0, ECode::Invariant, TEXT("DropReward.Gold"), TEXT("物品掉落奖励不得附带金币"));
                V.Check(Reward->Experience == 0, ECode::Invariant, TEXT("DropReward.Experience"), TEXT("物品掉落奖励不得附带经验"));
                V.Check(!Reward->Items.IsEmpty(), ECode::Invariant, TEXT("DropReward.Items"), TEXT("正物品掉率要求奖励物品非空"));
            }
        }
    });
    float MaxNormalPreparation = -1;
    Visit.operator()<FY01WaveRow>(EKind::Wave, [&](const auto& Row, const FValidator& V)
    {
        V.Enum(Row.Type, TEXT("Type")); V.Number(Row.PreparationSeconds, TEXT("PreparationSeconds"));
        V.Number(Row.RespawnSeconds, TEXT("RespawnSeconds"), true);
        V.Enum(Row.Schedule, TEXT("Schedule"));
        int64 Total = 0;
        if (Row.Schedule == EY01WaveSchedule::Legacy)
        {
            V.Check(Row.Batches.IsEmpty(), ECode::Invariant, TEXT("Batches"), TEXT("Legacy只消费Spawns，禁止混填Batches"));
            V.Spawns(Row.Spawns);
            for (const auto& Entry : Row.Spawns) { Total += Entry.Count; }
        }
        else if (Row.Schedule == EY01WaveSchedule::Batches)
        {
            V.Check(Row.Spawns.IsEmpty(), ECode::Invariant, TEXT("Spawns"), TEXT("Batches只消费Batches，禁止混填Spawns"));
            V.Check(!Row.Batches.IsEmpty(), ECode::InvalidPool, TEXT("Batches"), TEXT("批次列表不能为空"));
            TSet<FName> Ids;
            for (int32 I = 0; I < Row.Batches.Num(); ++I)
            {
                const auto& B = Row.Batches[I];
                const FString Field = FString::Printf(TEXT("Batches[%d](%s)"), I, *B.BatchId.ToString());
                V.Check(!B.BatchId.IsNone() && !Ids.Contains(B.BatchId), ECode::DuplicateValue, Field + TEXT(".BatchId"), TEXT("批次ID不能为空或波内重复"));
                Ids.Add(B.BatchId); V.Number(B.OffsetSeconds, Field + TEXT(".OffsetSeconds")); V.Enum(B.PointKind, Field + TEXT(".PointKind"));
                V.Check(Y01Data::ValidSpawnPointTag(B.PointKind, B.PointTag), ECode::Invariant, Field + TEXT(".PointTag"),
                    TEXT("Normal点须为无前导零的正int32数字Tag，Boss点须为Boss类型和Boss Tag"));
                V.Check(!B.Monsters.IsEmpty(), ECode::InvalidPool, Field + TEXT(".Monsters"), TEXT("批次怪物列表不能为空"));
                for (int32 J = 0; J < B.Monsters.Num(); ++J)
                {
                    const auto& M = B.Monsters[J]; const FString Item = Field + FString::Printf(TEXT(".Monsters[%d]"), J);
                    V.Reference<FY01MonsterRow>(M.Monster, EKind::Monster, Item + TEXT(".Monster"));
                    V.Number(M.Count, Item + TEXT(".Count"), true); Total += M.Count;
                }
            }
        }
        V.Check(Total > 0 && Total <= MAX_int32, ECode::InvalidNumber, TEXT("Count"), TEXT("每波总Count须为正int32，人数不参与计算"));
        V.Reference<FY01RewardPackageRow>(Row.Reward, EKind::RewardPackage, TEXT("Reward"));
        if (Row.Type == EY01WaveType::Normal && FMath::IsFinite(Row.PreparationSeconds))
        {
            MaxNormalPreparation = FMath::Max(MaxNormalPreparation, Row.PreparationSeconds);
        }
    });
    Visit.operator()<FY01WaveRow>(EKind::Wave, [&](const auto& Row, const FValidator& V)
    {
        if (Row.Type == EY01WaveType::Boss && MaxNormalPreparation >= 0)
        {
            V.Check(Row.PreparationSeconds > MaxNormalPreparation, ECode::BossPreparation, TEXT("PreparationSeconds"), TEXT("BOSS 准备时长须长于所有普通波"));
        }
    });
    TSet<int32> Floors, Stages;
    Visit.operator()<FY01TowerFloorRow>(EKind::TowerFloor, [&](const auto& Row, const FValidator& V)
    {
        V.Number(Row.Floor, TEXT("Floor"), true); V.Spawns(Row.Spawns);
        V.Check(!Floors.Contains(Row.Floor), ECode::DuplicateValue, TEXT("Floor"), TEXT("塔层编号重复")); Floors.Add(Row.Floor);
        V.Reference<FY01RewardPackageRow>(Row.Reward, EKind::RewardPackage, TEXT("Reward"));
    });
    struct FTrainingStart
    {
        int32 Stage;
        float StartSeconds;
        FY01DataRef Source;
    };
    TArray<FTrainingStart> TrainingStarts;
    TSet<float> StartTimes;
    Visit.operator()<FY01TrainingStageRow>(EKind::TrainingStage, [&](const auto& Row, const FValidator& V)
    {
        V.Number(Row.Stage, TEXT("Stage"), true); V.Spawns(Row.Spawns);
        V.Number(Row.StartSeconds, TEXT("StartSeconds"));
        V.Number(Row.SpawnInterval, TEXT("SpawnInterval"), true);
        V.Check(Row.MaxAlive > 0 && Row.MaxAlive <= 256, ECode::InvalidNumber, TEXT("MaxAlive"), TEXT("训练存活上限须为1至256"));
        V.Check(!Stages.Contains(Row.Stage), ECode::DuplicateValue, TEXT("Stage"), TEXT("训练阶段编号重复")); Stages.Add(Row.Stage);
        V.Reference<FY01RewardPackageRow>(Row.Reward, EKind::RewardPackage, TEXT("Reward"));
        if (FMath::IsFinite(Row.StartSeconds) && Row.StartSeconds >= 0)
        {
            V.Check(!StartTimes.Contains(Row.StartSeconds), ECode::DuplicateValue, TEXT("StartSeconds"), TEXT("训练阶段生效起点重复"));
            StartTimes.Add(Row.StartSeconds);
            TrainingStarts.Add({Row.Stage, Row.StartSeconds, V.Source});
        }
    });
    TrainingStarts.Sort([](const FTrainingStart& A, const FTrainingStart& B) { return A.Stage < B.Stage; });
    for (int32 Index = 0; Index < TrainingStarts.Num(); ++Index)
    {
        const auto& Start = TrainingStarts[Index];
        const FValidator V{*this, Result, Start.Source};
        if (Index == 0)
        {
            V.Check(Start.StartSeconds == 0, ECode::Invariant, TEXT("StartSeconds"), TEXT("第一训练阶段须在零时起始"));
        }
        else
        {
            V.Check(Start.StartSeconds > TrainingStarts[Index - 1].StartSeconds, ECode::Invariant, TEXT("StartSeconds"), TEXT("生效起点须随阶段编号严格递增"));
        }
    }
    TSet<int32> Levels;
    Visit.operator()<FY01LevelExperienceRow>(EKind::LevelExperience, [&](const auto& Row, const FValidator& V)
    {
        V.Number(Row.Level, TEXT("Level"), true); V.Number(Row.RequiredExperience, TEXT("RequiredExperience"), true);
        V.Check(!Levels.Contains(Row.Level), ECode::DuplicateValue, TEXT("Level"), TEXT("等级定义重复")); Levels.Add(Row.Level);
    });
    Visit.operator()<FY01MatchRulesRow>(EKind::MatchRules, [&](const auto& Row, const FValidator& V)
    {
        V.Number(Row.MaxLevel, TEXT("MaxLevel"), true);
        V.Number(Row.DefaultCritMultiplier, TEXT("DefaultCritMultiplier"), true);
        V.Number(Row.Core.MaxHealth, TEXT("Core.MaxHealth"), true);
        V.Number(Row.Core.HealthRegen, TEXT("Core.HealthRegen"));
        V.Number(Row.Core.HitAlertInterval, TEXT("Core.HitAlertInterval"), true);
        V.Number(Row.Core.CriticalHealthRatio, TEXT("Core.CriticalHealthRatio"), true);
        V.Check(Row.Core.CriticalHealthRatio <= 1, ECode::InvalidNumber, TEXT("Core.CriticalHealthRatio"), TEXT("危急生命比例不能大于1"));
        // 空引用表示该行不承担正式启动入口；运行时正式入口另行要求角色存在。
        if (Row.Startup.Character.Type != EKind::None || !Row.Startup.Character.RowName.IsNone())
        { V.Reference<FY01CharacterRow>(Row.Startup.Character, EKind::Character, TEXT("Startup.Character")); }
        if (Row.Startup.TalentChoice.Type != EKind::None || !Row.Startup.TalentChoice.RowName.IsNone())
        { V.Reference<FY01TalentChoiceRow>(Row.Startup.TalentChoice, EKind::TalentChoice, TEXT("Startup.TalentChoice")); }
        V.Number(Row.Startup.InitialGold, TEXT("Startup.InitialGold"));
        V.Number(Row.Startup.BaseGoldPerSecond, TEXT("Startup.BaseGoldPerSecond"));
        V.Number(Row.Startup.DropFlightSeconds, TEXT("Startup.DropFlightSeconds"));
        auto Color = [&](const FLinearColor& C, const TCHAR* Field)
        {
            V.Check(FMath::IsFinite(C.R) && FMath::IsFinite(C.G) && FMath::IsFinite(C.B) && FMath::IsFinite(C.A),
                ECode::InvalidNumber, Field, TEXT("颜色分量必须有限"));
        };
        Color(Row.Presentation.DamageColor, TEXT("Presentation.DamageColor"));
        Color(Row.Presentation.HealingColor, TEXT("Presentation.HealingColor"));
        Color(Row.Presentation.CriticalColor, TEXT("Presentation.CriticalColor"));
        V.Number(Row.Presentation.RegenMergeSeconds, TEXT("Presentation.RegenMergeSeconds"), true);
        // 最多检查已配置等级数加一，防止异常 MaxLevel 造成无界遍历。
        const int64 Limit = FMath::Min<int64>(Row.MaxLevel, static_cast<int64>(Levels.Num()) + 1);
        for (int64 Level = 1; Level <= Limit; ++Level)
        {
            if (!Levels.Contains(static_cast<int32>(Level)))
            {
                AddDataError(Result, ECode::MissingLevel, V.Source, TEXT("MaxLevel"), FString::Printf(TEXT("缺少等级 %lld 的经验配置"), Level));
                break;
            }
        }
    });
    Visit.operator()<FY01CharacterRow>(EKind::Character, [](const auto& Row, const FValidator& V)
    {
        V.Stats(Row);
        V.Number(Row.Strength, TEXT("Strength")); V.Number(Row.Agility, TEXT("Agility")); V.Number(Row.Wisdom, TEXT("Wisdom"));
        V.Number(Row.R33TestGrowth.AttackPerEffectiveSecond, TEXT("R33TestGrowth.AttackPerEffectiveSecond"));
        V.Number(Row.R33TestGrowth.AttackPerEffectiveKill, TEXT("R33TestGrowth.AttackPerEffectiveKill"));
        V.Number(Row.R33TestGrowth.GoldPerEffectiveSecond, TEXT("R33TestGrowth.GoldPerEffectiveSecond"));
        V.Check(Row.InitialEquipment.Num() <= Y01Data::EquipmentSlotCount, ECode::Invariant, TEXT("InitialEquipment"), TEXT("装备不能超过六槽"));
        V.Check(Row.InitialSkills.Num() <= Y01Data::SkillSlotCount, ECode::Invariant, TEXT("InitialSkills"), TEXT("技能不能超过四槽"));
        for (int32 I = 0; I < Row.InitialEquipment.Num(); ++I)
        {
            V.Reference<FY01EquipmentRow>(Row.InitialEquipment[I], EKind::Equipment, FString::Printf(TEXT("InitialEquipment[%d]"), I));
        }
        for (int32 I = 0; I < Row.InitialSkills.Num(); ++I)
        {
            V.Reference<FY01SkillRow>(Row.InitialSkills[I], EKind::Skill, FString::Printf(TEXT("InitialSkills[%d]"), I));
        }
    });
    Visit.operator()<FY01ShopOfferRow>(EKind::ShopOffer, [](const auto& Row, const FValidator& V)
    {
        V.Enum(Row.ShopKind, TEXT("ShopKind"));
        V.Number(Row.Price, TEXT("Price")); V.Quality(Row.Quality);
        V.SourceEffects(Row.EffectIds);
        V.GrowthRules(Row.GrowthRuleIds);
        if (Row.ShopKind == EY01ShopKind::DirectEffect)
        {
            V.Check(!Row.EffectIds.IsEmpty() || !Row.GrowthRuleIds.IsEmpty(), ECode::Invariant, TEXT("EffectIds"), TEXT("直接效果商品须配置效果ID或成长规则"));
            V.Check(Row.TierPool.Type == EKind::None && Row.TierPool.RowName.IsNone(), ECode::Invariant, TEXT("TierPool"), TEXT("直接效果商品的TierPool须完全为空"));
            V.Check(Row.Pool.Type == EKind::None && Row.Pool.RowName.IsNone(), ECode::Invariant, TEXT("Pool"), TEXT("直接效果商品的Pool类型与行名须完全为空"));
            V.Check(Row.Quantity == 1, ECode::Invariant, TEXT("Quantity"), TEXT("直接效果商品数量须为1"));
            V.Check(Row.Quality == EY01Quality::Green, ECode::Invariant, TEXT("Quality"), TEXT("直接效果商品须以Green作为中性占位品质，不进行品质抽样"));
        }
        else if (Row.ShopKind == EY01ShopKind::RandomPool)
        {
            V.Check(Row.EffectIds.IsEmpty(), ECode::Invariant, TEXT("EffectIds"), TEXT("随机池商品不能配置直接效果ID"));
            V.Check(Row.GrowthRuleIds.IsEmpty(), ECode::Invariant, TEXT("GrowthRuleIds"), TEXT("随机池商品不能配置成长规则"));
            V.Check(Row.TierPool.Type == EKind::None && Row.TierPool.RowName.IsNone(), ECode::Invariant, TEXT("TierPool"), TEXT("随机池商品的TierPool须完全为空"));
            V.Number(Row.Quantity, TEXT("Quantity"), true);
            if (const auto* Pool = V.Reference<FY01LocalItemPoolRow>(Row.Pool, EKind::LocalItemPool, TEXT("Pool")))
            {
                V.Check(IsLocalItem(Pool->ItemType), ECode::TypeMismatch, TEXT("Pool.ItemType"), TEXT("商店产物池须为装备或技能池"));
                V.Quality(Pool->Quality, TEXT("Pool.Quality"));
                V.Check(Pool->Quality == Row.Quality, ECode::QualityMismatch, TEXT("Pool.Quality"), TEXT("商店产物池品质须与标示品质一致"));
            }
        }
        else if (Row.ShopKind == EY01ShopKind::RandomTier)
        {
            V.Check(Row.Pool.Type == EKind::None && Row.Pool.RowName.IsNone(), ECode::Invariant, TEXT("Pool"), TEXT("随机品质商品的旧Pool须完全为空"));
            V.Check(Row.EffectIds.IsEmpty(), ECode::Invariant, TEXT("EffectIds"), TEXT("随机品质商品不能配置直接效果ID"));
            V.Check(Row.GrowthRuleIds.IsEmpty(), ECode::Invariant, TEXT("GrowthRuleIds"), TEXT("随机品质商品不能配置成长规则"));
            V.Check(Row.Quality == EY01Quality::Green, ECode::Invariant, TEXT("Quality"), TEXT("随机品质商品须以Green作为中性占位品质"));
            V.Number(Row.Quantity, TEXT("Quantity"), true);
            if (const auto* Pool = V.Reference<FY01QualityRollPoolRow>(Row.TierPool, EKind::QualityRollPool, TEXT("TierPool")))
            {
                V.Check(IsLocalItem(Pool->ItemType), ECode::TypeMismatch, TEXT("TierPool.ItemType"), TEXT("商品品质池须为装备或技能类型"));
            }
        }
        else if (Row.ShopKind == EY01ShopKind::DirectItem)
        {
            V.Check(Row.TierPool.Type == EKind::None && Row.TierPool.RowName.IsNone(), ECode::Invariant, TEXT("TierPool"), TEXT("直接道具商品的TierPool须完全为空"));
            V.Check(Row.EffectIds.IsEmpty(), ECode::Invariant, TEXT("EffectIds"), TEXT("直接道具商品不能配置直接效果ID"));
            V.Check(Row.GrowthRuleIds.IsEmpty(), ECode::Invariant, TEXT("GrowthRuleIds"), TEXT("直接道具商品不能配置成长规则"));
            V.Number(Row.Quantity, TEXT("Quantity"), true);
            if (const auto* Item = V.Reference<FY01ConsumableRow>(Row.Pool, EKind::Consumable, TEXT("Pool")))
            {
                V.Check(Item->Quality == Row.Quality, ECode::QualityMismatch, TEXT("Quality"), TEXT("直接道具商品标示品质须与道具品质一致"));
            }
        }
    });
    Visit.operator()<FY01SynthesisRow>(EKind::Synthesis, [](const auto& Row, const FValidator& V)
    {
        V.Quality(Row.InputQuality, TEXT("InputQuality"));
        V.Check(IsLocalItem(Row.InputType), ECode::TypeMismatch, TEXT("InputType"), TEXT("合成类型须为装备或技能"));
        V.Check(Row.InputCount == Y01Data::SynthesisInputCount, ECode::Invariant, TEXT("InputCount"), TEXT("合成须消耗五份同类同品质物品"));
        V.Check(Row.InputQuality != EY01Quality::Red, ECode::Invariant, TEXT("InputQuality"), TEXT("红色品质不得定义升级配方"));
        if (const auto* Pool = V.Reference<FY01LocalItemPoolRow>(Row.OutputPool, EKind::LocalItemPool, TEXT("OutputPool")))
        {
            V.Check(IsLocalItem(Pool->ItemType) && Pool->ItemType == Row.InputType, ECode::TypeMismatch, TEXT("OutputPool.ItemType"), TEXT("合成产物池须与输入物品同类型"));
            V.Quality(Pool->Quality, TEXT("OutputPool.Quality"));
            if (static_cast<uint8>(Row.InputQuality) < static_cast<uint8>(EY01Quality::Red))
            {
                const EY01Quality NextQuality = static_cast<EY01Quality>(static_cast<uint8>(Row.InputQuality) + 1);
                V.Check(Pool->Quality == NextQuality, ECode::QualityMismatch, TEXT("OutputPool.Quality"), TEXT("合成产物池品质须恰好高一级"));
            }
        }
    });
    Visit.operator()<FY01WarehouseEventRow>(EKind::WarehouseEvent, [](const auto& Row, const FValidator& V)
    {
        V.Number(Row.UpgradeCount, TEXT("UpgradeCount"), true);
    });
    Visit.operator()<FY01TalentChoiceRow>(EKind::TalentChoice, [](const auto& Row, const FValidator& V)
    {
        V.Check(Row.Candidates.Num() == Y01Data::TalentCandidateCount, ECode::Invariant, TEXT("Candidates"), TEXT("天赋选择须提供三个候选"));
        V.Check(Row.RefreshesPerCandidate == Y01Data::TalentRefreshPerCandidate, ECode::Invariant, TEXT("RefreshesPerCandidate"), TEXT("每个候选独立拥有一次刷新机会"));
        for (int32 I = 0; I < Row.Candidates.Num(); ++I)
        {
            V.Reference<FY01TalentRow>(Row.Candidates[I], EKind::Talent, FString::Printf(TEXT("Candidates[%d]"), I));
        }
    });
    return Result;
}

bool FY01DataCatalog::SamplePool(const FY01DataRef& Pool, FRandomStream& Random, FY01DataRef& OutItem, FY01DataValidation& OutErrors) const
{
    OutItem = FY01DataRef();
    const TArray<FY01WeightedEntry>* Entries = nullptr;
    FY01DataValidation Result;
    const FValidator V{*this, Result, Pool};
    double Total = 0;
    if (Pool.Type == EKind::LocalItemPool)
    {
        if (const auto* Row = FindRow<FY01LocalItemPoolRow>(Pool, Result))
        {
            V.Check(IsLocalItem(Row->ItemType), ECode::TypeMismatch, TEXT("ItemType"), TEXT("局内池仅支持装备或技能"));
            Entries = &Row->Entries;
            Total = V.Pool(*Entries, Row->ItemType, Row->Quality);
        }
    }
    else if (Pool.Type == EKind::MetaLootPool)
    {
        if (const auto* Row = FindRow<FY01MetaLootPoolRow>(Pool, Result))
        {
            Entries = &Row->Entries;
            Total = V.Pool(*Entries, EKind::MetaItem, Row->Quality);
        }
    }
    else { AddDataError(Result, ECode::TypeMismatch, Pool, TEXT("Pool.Type"), TEXT("抽样引用必须指向局内池或局外池")); }
    if (!Result.IsValid() || !Entries)
    {
        OutErrors.Errors.Append(Result.Errors);
        return false;
    }
    // 以 double 累计 float 权重，避免有效大权重求和溢出。
    double Ticket = static_cast<double>(Random.GetFraction()) * Total;
    const FY01WeightedEntry* LastPositive = nullptr;
    for (const auto& Entry : *Entries)
    {
        if (Entry.Weight <= 0) { continue; }
        LastPositive = &Entry;
        if (Ticket < Entry.Weight) { OutItem = Entry.Item; return true; }
        Ticket -= Entry.Weight;
    }
    // 仅防护浮点舍入；永不退回零权重项。
    OutItem = LastPositive->Item;
    return true;
}

TArray<FY01DataRef> FY01DataCatalog::CollectEnabled(EY01TableKind Kind, FY01DataValidation& OutErrors) const
{
    TArray<FY01DataRef> Result;
    const bool bSupported = Kind == EKind::ShopOffer || Kind == EKind::Synthesis || Kind == EKind::Wave
        || Kind == EKind::AttributeMapping || Kind == EKind::Equipment || Kind == EKind::Skill;
    if (!bSupported)
    {
        AddDataError(OutErrors, ECode::InvalidEnum, Identity(Kind), TEXT("Kind"), TEXT("该表不支持正式入口扫描"));
        return Result;
    }
    const UDataTable* const* Table = Tables.Find(Kind);
    if (!Table)
    {
        // 可选映射表未登记即无基础映射；其余正式扫描表必须存在。
        if (Kind != EKind::AttributeMapping) { AddDataError(OutErrors, ECode::MissingTable, Identity(Kind), TEXT("Table"), TEXT("正式数据表缺失")); }
        return Result;
    }
    if (!*Table) { AddDataError(OutErrors, ECode::MissingTable, Identity(Kind), TEXT("Table"), TEXT("正式数据表加载失败")); return Result; }
    if ((*Table)->GetRowStruct() != Schema(Kind)) { AddDataError(OutErrors, ECode::WrongSchema, Identity(Kind), TEXT("RowStruct"), TEXT("数据表行结构错误")); return Result; }
    struct FEntry { int32 Order; FName Name; };
    TArray<FEntry> Entries;
    for (const auto& Pair : (*Table)->GetRowMap())
    {
        if (!Pair.Value) { continue; }
        const uint8* Row = Pair.Value;
        switch (Kind)
        {
        case EKind::ShopOffer:
            if (const auto* R = reinterpret_cast<const FY01ShopOfferRow*>(Row); R->Enabled) { Entries.Add({R->SortOrder, Pair.Key}); }
            break;
        case EKind::Synthesis:
            if (const auto* R = reinterpret_cast<const FY01SynthesisRow*>(Row); R->Enabled) { Entries.Add({R->SortOrder, Pair.Key}); }
            break;
        case EKind::Wave:
            if (const auto* R = reinterpret_cast<const FY01WaveRow*>(Row); R->Enabled) { Entries.Add({R->Order, Pair.Key}); }
            break;
        case EKind::AttributeMapping:
            if (reinterpret_cast<const FY01AttributeMappingRow*>(Row)->bBaseEnabled) { Entries.Add({0, Pair.Key}); }
            break;
        default: Entries.Add({0, Pair.Key}); break;
        }
    }
    Entries.Sort([](const FEntry& A, const FEntry& B)
    {
        return A.Order != B.Order ? A.Order < B.Order : FNameLexicalLess()(A.Name, B.Name);
    });
    for (const auto& Entry : Entries) { Result.Add(Identity(Kind, Entry.Name)); }
    return Result;
}
