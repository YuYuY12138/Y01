#include "Playground/Y01R36Profile.h"
#include "Playground/Y01TestSkill.h"
#include "Combat/Y01DamageEffects.h"
#include "Serialization/CustomVersion.h"

namespace
{
const FGuid R36ProfileVersion(0x3188B7AC, 0x416F47A2, 0xAE045FC1, 0x8894D663);
const FCustomVersionRegistration R36ProfileRegistration(R36ProfileVersion, 1, TEXT("R36ExplicitEvidenceMode"));
}
void UY01R36Profile::Serialize(FArchive& Ar)
{
    Super::Serialize(Ar);
    Ar.UsingCustomVersion(R36ProfileVersion);
}
void UY01R36Profile::PostLoad()
{
    Super::PostLoad();
    // 仅无显式生命周期版本的旧磁盘配置沿用旧语义；不标脏、不重存。
    if (GetLinkerCustomVersion(R36ProfileVersion) < 1) { bEvidenceMode = bEconomyEnabled; }
}

void UY01R36Profile::ValidateExtension(FY01DataValidation& Result) const
{
    const auto Catalog = BuildCatalog();
    auto Reject = [&](const FY01DataRef& Ref, const TCHAR* Field, const TCHAR* Message)
    { Result.Errors.Add({EY01DataErrorCode::Invariant, Ref, Field, Message}); };
    if (!FMath::IsFinite(DropFlightSeconds) || DropFlightSeconds < 0)
    { Reject(Character, TEXT("DropFlightSeconds"), TEXT("飞行秒数必须为有限非负double，零表示下次可执行调度")); }
    TSet<FName> MappingNames;
    for (const auto& Ref : BaseAttributeMappings)
    {
        const auto* Mapping = Catalog.FindRow<FY01AttributeMappingRow>(Ref, Result);
        if (Ref.Type != EY01TableKind::AttributeMapping || !Mapping || !Mapping->IsValid() || MappingNames.Contains(Ref.RowName))
        { Reject(Ref, TEXT("BaseAttributeMappings"), TEXT("基础映射必须存在、合法且不重复引用")); }
        MappingNames.Add(Ref.RowName);
    }
    FY01DataValidation Query;
    if (const auto* Row = Catalog.FindRow<FY01CharacterRow>(Character, Query))
    {
        if (!Row->InitialEquipment.IsEmpty() || !Row->InitialSkills.IsEmpty())
        { Reject(Character, TEXT("InitialEquipment/InitialSkills"), TEXT("R36角色必须空槽初始化，物品和学习仅走实例事务")); }
    }
    auto ValidateContent = [&](const TArray<FY01DataRef>& Refs, EY01TableKind Kind)
    {
        TSet<FName> Seen;
        for (const auto& Ref : Refs)
        {
            if (Ref.Type != Kind || Ref.RowName.IsNone() || Seen.Contains(Ref.RowName))
            { Reject(Ref, TEXT("Content"), TEXT("内容引用类型、身份或重复项无效")); continue; }
            Seen.Add(Ref.RowName);
            if (Kind == EY01TableKind::Equipment)
            {
                const auto* Row = Catalog.FindRow<FY01EquipmentRow>(Ref, Result);
                if (!Row) { continue; }
                if (Row->DisplayName.ToString().TrimStartAndEnd().IsEmpty())
                { Reject(Ref, TEXT("DisplayName"), TEXT("装备显示名不能为空")); }
                if (Row->AttackBonus != 0 || !Row->Effect.IsNull() || (Row->EffectIds.IsEmpty() && Row->GrowthRuleIds.IsEmpty() && Row->DamageEffectIds.IsEmpty()))
                { Reject(Ref, TEXT("EffectIds/GrowthRuleIds/DamageEffectIds"), TEXT("R36内容装备必须使用实例来源，不能混入旧装备授予")); }
            }
            else
            {
                const auto* Row = Catalog.FindRow<FY01SkillRow>(Ref, Result);
                if (!Row) { continue; }
                if (Row->DisplayName.ToString().TrimStartAndEnd().IsEmpty())
                { Reject(Ref, TEXT("DisplayName"), TEXT("技能显示名不能为空")); }
                if (!Row->Effect.IsNull()) { Reject(Ref, TEXT("Effect"), TEXT("R36技能不能混入旧Effect授予")); }
                if (Row->Type != EY01SkillType::Active)
                {
                    if (bEconomyEnabled && (!Row->Ability.IsNull() || Row->CooldownSeconds != 0 || Row->TestSkill.Mode != EY01TestSkillMode::None))
                    { Reject(Ref, TEXT("Ability/Cooldown/TestSkill"), TEXT("被动书不能带主动技能字段")); }
                    continue;
                }
                // 实际加载并检查CDO契约；只允许已验收的两个原生模板，不按SkillID列白名单。
                UClass* Ability = Row->Ability.IsNull() ? nullptr : StaticLoadClass(UGameplayAbility::StaticClass(), nullptr,
                    *Row->Ability.ToSoftObjectPath().ToString(), nullptr, LOAD_NoWarn);
                const auto* Template = Ability ? Cast<UY01LoadoutAbility>(Ability->GetDefaultObject()) : nullptr;
                if (!Ability || !Ability->HasAnyClassFlags(CLASS_Native)
                    || (Ability != UY01BasicAttackAbility::StaticClass() && Ability != UY01TestSkillAbility::StaticClass() && Ability != UY01DamageAbility::StaticClass())
                    || ((!Row->DamageEffectIds.IsEmpty()) != (Ability == UY01DamageAbility::StaticClass()))
                    || !Template || !Template->SupportsLoadoutSkill(Row->TestSkill))
                { Reject(Ref, TEXT("Ability"), TEXT("主动技能必须真实加载允许的原生模板且匹配技能配置")); }
            }
        }
    };
    ValidateContent(ContentEquipment, EY01TableKind::Equipment);
    ValidateContent(ContentSkills, EY01TableKind::Skill);
    if (bCoreDefenseEnabled)
    {
        const auto* Rules = Catalog.FindRow<FY01MatchRulesRow>(CoreRules, Result);
        if (CoreRules.Type != EY01TableKind::MatchRules || !Rules || !Rules->Core.IsValid())
        { Reject(CoreRules, TEXT("CoreRules"), TEXT("启用核心防守必须引用完整合法的MatchRules核心配置")); }
    }
    if (!bEconomyEnabled) { return; }
    using K = EY01TableKind;
    if (InitialGold < 0 || !FMath::IsFinite(BaseGoldPerSecond) || BaseGoldPerSecond < 0)
    { Reject(Character, TEXT("InitialGold/BaseGoldPerSecond"), TEXT("初始金币及基础收入必须为有限非负数")); }
    // 校验所有已配置局部池，包括仅由品质池或配方间接引用的内容。
    if (const auto* Table = Tables.Find(K::LocalItemPool); Table && *Table && (*Table)->GetRowStruct() == FY01LocalItemPoolRow::StaticStruct())
    {
        for (const auto& Pair : (*Table)->GetRowMap())
        {
            const auto& Pool = *reinterpret_cast<const FY01LocalItemPoolRow*>(Pair.Value);
            TArray<FY01DataRef> Refs; for (const auto& Entry : Pool.Entries) { Refs.Add(Entry.Item); }
            ValidateContent(Refs, Pool.ItemType);
        }
    }
    auto Unique = [&](const TArray<FY01DataRef>& Refs, K Kind)
    {
        TSet<FName> Seen;
        for (const auto& Ref : Refs)
        {
            if (Ref.Type != Kind || Ref.RowName.IsNone() || Seen.Contains(Ref.RowName))
            { Reject(Ref, TEXT("EconomyReferences"), TEXT("经济公开引用类型、身份或重复项无效")); }
            Seen.Add(Ref.RowName);
        }
    };
    Unique(ContentShopOffers, K::ShopOffer); Unique(ContentRecipes, K::Synthesis);
    for (const auto& Ref : ContentShopOffers)
    {
        if (const auto* Row = Catalog.FindRow<FY01ShopOfferRow>(Ref, Result))
        {
            if (Row->DisplayName.ToString().TrimStartAndEnd().IsEmpty())
            { Reject(Ref, TEXT("DisplayName"), TEXT("商品显示名不能为空")); }
        }
    }
    TSet<uint16> RecipeKinds;
    for (const auto& Ref : ContentRecipes)
    {
        if (const auto* Row = Catalog.FindRow<FY01SynthesisRow>(Ref, Result))
        {
            const uint16 Key = uint16(static_cast<uint8>(Row->InputType)) * 256 + static_cast<uint8>(Row->InputQuality);
            if (RecipeKinds.Contains(Key)) { Reject(Ref, TEXT("InputType/InputQuality"), TEXT("同类型同品质只能启用一个配方")); }
            RecipeKinds.Add(Key);
        }
    }
    if (TalentChoice.Type != K::None || !TalentChoice.RowName.IsNone())
    {
        if (TalentChoice.Type != K::TalentChoice) { Reject(TalentChoice, TEXT("TalentChoice"), TEXT("天赋选择引用类型无效")); }
        if (const auto* Choice = Catalog.FindRow<FY01TalentChoiceRow>(TalentChoice, Result))
        {
            Unique(Choice->Candidates, K::Talent);
            for (const auto& Ref : Choice->Candidates)
            {
                if (const auto* Row = Catalog.FindRow<FY01TalentRow>(Ref, Result))
                {
                    if (Row->DisplayName.ToString().TrimStartAndEnd().IsEmpty())
                    { Reject(Ref, TEXT("DisplayName"), TEXT("天赋显示名不能为空")); }
                    if (!Row->Ability.IsNull() || !Row->Effect.IsNull() || (Row->EffectIds.IsEmpty() && Row->GrowthRuleIds.IsEmpty()))
                    { Reject(Ref, TEXT("EffectIds/GrowthRuleIds"), TEXT("天赋只允许实例效果及玩家成长，不允许旧授予字段")); }
                }
            }
        }
    }
}
