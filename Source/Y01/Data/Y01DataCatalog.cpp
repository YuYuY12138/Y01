#include "Data/Y01DataCatalog.h"

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

    auto Visit = [&]<typename T>(EKind Kind, TFunctionRef<void(const T&, const FValidator&)> Callback)
    {
        TArray<FName> Names = Tables[Kind]->GetRowNames();
        Names.Sort(FNameLexicalLess());
        for (FName Name : Names)
        {
            const FY01DataRef Source = Identity(Kind, Name);
            if (const T* Row = FindRow<T>(Source, Result)) { Callback(*Row, FValidator{*this, Result, Source}); }
        }
    };
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
    });
    Visit.operator()<FY01EquipmentRow>(EKind::Equipment, [](const auto& Row, const FValidator& V)
    {
        V.Quality(Row.Quality);
    });
    Visit.operator()<FY01TalentRow>(EKind::Talent, [](const auto& Row, const FValidator& V) { V.Quality(Row.Quality); });
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
        for (int32 I = 0; I < Row.Skills.Num(); ++I)
        {
            V.Reference<FY01SkillRow>(Row.Skills[I], EKind::Skill, FString::Printf(TEXT("Skills[%d]"), I));
        }
        V.Reference<FY01RewardPackageRow>(Row.DropReward, EKind::RewardPackage, TEXT("DropReward"));
    });
    float MaxNormalPreparation = -1;
    Visit.operator()<FY01WaveRow>(EKind::Wave, [&](const auto& Row, const FValidator& V)
    {
        V.Enum(Row.Type, TEXT("Type")); V.Number(Row.PreparationSeconds, TEXT("PreparationSeconds")); V.Spawns(Row.Spawns);
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
        V.Number(Row.Price, TEXT("Price")); V.Number(Row.Quantity, TEXT("Quantity"), true); V.Quality(Row.Quality);
        if (const auto* Pool = V.Reference<FY01LocalItemPoolRow>(Row.Pool, EKind::LocalItemPool, TEXT("Pool")))
        {
            V.Check(IsLocalItem(Pool->ItemType), ECode::TypeMismatch, TEXT("Pool.ItemType"), TEXT("商店产物池须为装备或技能池"));
            V.Quality(Pool->Quality, TEXT("Pool.Quality"));
            V.Check(Pool->Quality == Row.Quality, ECode::QualityMismatch, TEXT("Pool.Quality"), TEXT("商店产物池品质须与标示品质一致"));
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
