#include "Misc/AutomationTest.h"
#include "Playground/Y01R36Profile.h"
#include "Playground/Y01TestSkill.h"
#include "Y01MatchFixture.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36ProfileTest, "Y01.Framework.R36.Profile",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36ProfileTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("LegacyDefault"), TEXT("OptionalTables"), TEXT("All22Tables"), TEXT("Required19"),
        TEXT("EquipmentName"), TEXT("SkillName"), TEXT("WhitespaceName"), TEXT("MissingAbility"), TEXT("ForeignAbility"),
        TEXT("NativeProbeRejected"), TEXT("TemplateMismatch"), TEXT("AreaTemplate"), TEXT("ProjectileTemplate"),
        TEXT("Passive"), TEXT("CharacterEquipment"), TEXT("CharacterSkills"), TEXT("ContentMissing"),
        TEXT("ContentWrongType"), TEXT("ContentDuplicate"), TEXT("ContentReflection"), TEXT("LegacyEquipment"), TEXT("LegacySkillEffect") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36ProfileTest::RunTest(const FString& Case)
{
    using K = EY01TableKind;
    FY01MatchFixture Data;
    auto* Profile = NewObject<UY01R36Profile>();
    auto& Character = Data.Row<FY01CharacterRow>(K::Character);
    Character.InitialEquipment.Reset(); Character.InitialSkills.Reset();
    auto& Equipment = Data.Row<FY01EquipmentRow>(K::Equipment);
    auto& Skill = Data.Row<FY01SkillRow>(K::Skill);
    Equipment.DisplayName = FText::FromString(TEXT("程序装备"));
    Data.Row<FY01EquipmentRow>(K::Equipment, TEXT("Blue")).DisplayName = FText::FromString(TEXT("蓝色程序装备"));
    Skill.DisplayName = FText::FromString(TEXT("程序主动技能"));
    Profile->Character = FY01MatchFixture::Ref(K::Character); Profile->OrderedWaves = Data.Waves();
    Profile->ContentSkills.Add(FY01MatchFixture::Ref(K::Skill));
    if (Case != TEXT("OptionalTables"))
    {
        FY01SourceEffectRow E; E.Magnitude = 10; Data.Add(K::SourceEffect, E);
        Equipment.EffectIds.Add(FY01MatchFixture::Ref(K::SourceEffect));
        Profile->ContentEquipment.Add(FY01MatchFixture::Ref(K::Equipment));
    }
    if (Case == TEXT("All22Tables"))
    {
        Data.Add(K::GrowthRule, FY01GrowthRuleRow());
        FY01QualityRollPoolRow Q; FY01QualityRollEntry E; E.Pool = FY01MatchFixture::Ref(K::LocalItemPool);
        Q.Entries.Add(E); Data.Add(K::QualityRollPool, Q);
    }
    if (Case == TEXT("Required19")) { Data.Tables.Remove(K::Talent); }
    if (Case == TEXT("EquipmentName")) { Equipment.DisplayName = FText(); }
    if (Case == TEXT("SkillName")) { Skill.DisplayName = FText(); }
    if (Case == TEXT("WhitespaceName")) { Skill.DisplayName = FText::FromString(TEXT("  \t")); }
    if (Case == TEXT("MissingAbility")) { Skill.Ability = FSoftObjectPath(TEXT("/Script/Y01.NoSuchAbility")); }
    if (Case == TEXT("ForeignAbility")) { Skill.Ability = UGameplayAbility::StaticClass(); }
    if (Case == TEXT("NativeProbeRejected")) { Skill.Ability = UY01LearnProbeAbility::StaticClass(); }
    if (Case == TEXT("TemplateMismatch")) { Skill.Ability = UY01TestSkillAbility::StaticClass(); }
    if (Case == TEXT("AreaTemplate") || Case == TEXT("ProjectileTemplate"))
    {
        Skill.Ability = UY01TestSkillAbility::StaticClass();
        Skill.TestSkill.Mode = Case == TEXT("AreaTemplate") ? EY01TestSkillMode::Area : EY01TestSkillMode::Projectile;
        Skill.TestSkill.Radius = 100; Skill.TestSkill.ActiveSeconds = 1; Skill.TestSkill.TargetRange = 500;
        Skill.TestSkill.Speed = 400; Skill.TestSkill.Lifetime = 2;
    }
    if (Case == TEXT("Passive")) { Skill.Type = EY01SkillType::Passive; Skill.Ability.Reset(); Skill.CooldownSeconds = 0; }
    if (Case == TEXT("CharacterEquipment")) { Character.InitialEquipment.Add(FY01MatchFixture::Ref(K::Equipment, TEXT("Blue"))); }
    if (Case == TEXT("CharacterSkills")) { Character.InitialSkills.Add(FY01MatchFixture::Ref(K::Skill)); }
    if (Case == TEXT("ContentMissing")) { Profile->ContentSkills[0].RowName = TEXT("不存在"); }
    if (Case == TEXT("ContentWrongType")) { Profile->ContentSkills[0].Type = K::Equipment; }
    if (Case == TEXT("ContentDuplicate")) { Profile->ContentSkills.Add(FY01MatchFixture::Ref(K::Skill)); }
    if (Case == TEXT("LegacyEquipment")) { Equipment.AttackBonus = 5; }
    if (Case == TEXT("LegacySkillEffect")) { Skill.Effect = UY01EquipmentAttackEffect::StaticClass(); }
    for (const auto& Pair : Data.Tables) { Profile->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
    if (Case == TEXT("LegacyDefault"))
    {
        auto* Legacy = NewObject<UY01PlaygroundProfile>(); Legacy->Tables = Profile->Tables;
        Legacy->Character = Profile->Character; Legacy->OrderedWaves = Profile->OrderedWaves;
        Equipment.DisplayName = FText(); Skill.DisplayName = FText();
        TestTrue(TEXT("旧Profile默认扩展不增加限制"), Legacy->Validate().IsValid()); return true;
    }
    if (Case == TEXT("ContentReflection"))
    {
        TestNotNull(TEXT("装备内容公开数组可发现"), FindFProperty<FArrayProperty>(Profile->GetClass(), TEXT("ContentEquipment")));
        TestNotNull(TEXT("技能内容公开数组可发现"), FindFProperty<FArrayProperty>(Profile->GetClass(), TEXT("ContentSkills"))); return true;
    }
    const UY01PlaygroundProfile* Base = Profile;
    const auto Result = Base->Validate();
    const bool Valid = Case == TEXT("OptionalTables") || Case == TEXT("All22Tables") || Case == TEXT("AreaTemplate")
        || Case == TEXT("ProjectileTemplate") || Case == TEXT("Passive");
    if (Valid)
    {
        for (const auto& Error : Result.Errors) { AddInfo(Error.Field + TEXT(": ") + Error.Message); }
        TestTrue(TEXT("合法R36配置通过非virtual基类入口"), Result.IsValid());
    }
    else { TestFalse(TEXT("基类Validate必须实际调用R36扩展并拒绝非法配置"), Result.IsValid()); }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36EconomyProfileTest, "Y01.Framework.R36.RuntimeIntegration.Profile",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01R36EconomyProfileTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Explicit"), TEXT("OfferName"), TEXT("PoolEquipmentName"), TEXT("PoolLegacyEquipment"),
        TEXT("PoolSkillName"), TEXT("PoolForeignAbility"), TEXT("PoolTemplateMismatch"), TEXT("PoolPassiveLegacy"),
        TEXT("RecipeMissing"), TEXT("OfferMissing"), TEXT("OfferDuplicate"), TEXT("ChoiceMissing"), TEXT("ChoiceDuplicate"),
        TEXT("TalentName"), TEXT("TalentLegacy"), TEXT("NegativeGold"), TEXT("NegativeRate"), TEXT("RecipeDuplicate")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36EconomyProfileTest::RunTest(const FString& Case)
{
    using K = EY01TableKind;
    FY01MatchFixture F; auto* P = NewObject<UY01R36Profile>();
    auto* Enabled = FindFProperty<FBoolProperty>(P->GetClass(), TEXT("bEconomyEnabled"));
    auto* Offers = FindFProperty<FArrayProperty>(P->GetClass(), TEXT("ContentShopOffers"));
    auto* Recipes = FindFProperty<FArrayProperty>(P->GetClass(), TEXT("ContentRecipes"));
    auto* Choice = FindFProperty<FStructProperty>(P->GetClass(), TEXT("TalentChoice"));
    auto* Gold = FindFProperty<FIntProperty>(P->GetClass(), TEXT("InitialGold"));
    auto* Rate = FindFProperty<FDoubleProperty>(P->GetClass(), TEXT("BaseGoldPerSecond"));
    if (!TestTrue(TEXT("经济必须有显式公开配置且默认关闭"), Enabled && Offers && Recipes && Choice && Gold && Rate)) { return false; }
    TestFalse(TEXT("旧S1默认不启用经济"), Enabled->GetPropertyValue_InContainer(P));
    Enabled->SetPropertyValue_InContainer(P, true);
    Gold->SetPropertyValue_InContainer(P, Case == TEXT("NegativeGold") ? -1 : 200);
    Rate->SetPropertyValue_InContainer(P, Case == TEXT("NegativeRate") ? -1 : 1);
    auto& C = F.Row<FY01CharacterRow>(K::Character); C.InitialEquipment.Reset(); C.InitialSkills.Reset();
    FY01SourceEffectRow Effect; Effect.Magnitude = 5; F.Add(K::SourceEffect, Effect);
    for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Blue"))})
    {
        auto& E = F.Row<FY01EquipmentRow>(K::Equipment, Name);
        E.DisplayName = FText::FromString(TEXT("测试装备")); E.EffectIds = {F.Ref(K::SourceEffect)};
    }
    auto& E = F.Row<FY01EquipmentRow>(K::Equipment);
    auto& S = F.Row<FY01SkillRow>(K::Skill); S.DisplayName = FText::FromString(TEXT("测试技能"));
    FY01LocalItemPoolRow Pool; Pool.ItemType = K::Skill; FY01WeightedEntry Entry; Entry.Item = F.Ref(K::Skill);
    Pool.Entries.Add(Entry); F.Add(K::LocalItemPool, Pool, TEXT("Books"));
    auto& Offer = F.Row<FY01ShopOfferRow>(K::ShopOffer); Offer.DisplayName = FText::FromString(TEXT("测试商品")); Offer.Price = 10;
    FY01TalentRow T; T.DisplayName = FText::FromString(TEXT("测试天赋")); T.EffectIds = {F.Ref(K::SourceEffect)};
    FY01TalentChoiceRow TC;
    for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Two")), FName(TEXT("Three"))})
    { F.Add(K::Talent, T, Name); TC.Candidates.Add(F.Ref(K::Talent, Name)); }
    F.Add(K::TalentChoice, TC);
    TArray<FY01DataRef> OfferRefs = {F.Ref(K::ShopOffer)}, RecipeRefs = {F.Ref(K::Synthesis)};
    FY01DataRef ChoiceRef = F.Ref(K::TalentChoice);
    if (Case == TEXT("OfferName")) { Offer.DisplayName = FText::FromString(TEXT(" \t")); }
    if (Case == TEXT("PoolEquipmentName")) { E.DisplayName = FText(); }
    if (Case == TEXT("PoolLegacyEquipment")) { E.AttackBonus = 3; }
    if (Case == TEXT("PoolSkillName")) { S.DisplayName = FText(); }
    if (Case == TEXT("PoolForeignAbility")) { S.Ability = UY01LearnProbeAbility::StaticClass(); }
    if (Case == TEXT("PoolTemplateMismatch")) { S.Ability = UY01TestSkillAbility::StaticClass(); }
    if (Case == TEXT("PoolPassiveLegacy")) { S.Type = EY01SkillType::Passive; }
    if (Case == TEXT("RecipeMissing")) { RecipeRefs[0].RowName = TEXT("Missing"); }
    if (Case == TEXT("OfferMissing")) { OfferRefs[0].RowName = TEXT("Missing"); }
    if (Case == TEXT("OfferDuplicate")) { OfferRefs.Add(F.Ref(K::ShopOffer)); }
    if (Case == TEXT("RecipeDuplicate")) { RecipeRefs.Add(F.Ref(K::Synthesis)); }
    if (Case == TEXT("ChoiceMissing")) { ChoiceRef.RowName = TEXT("Missing"); }
    if (Case == TEXT("ChoiceDuplicate")) { F.Row<FY01TalentChoiceRow>(K::TalentChoice).Candidates[1] = TC.Candidates[0]; }
    if (Case == TEXT("TalentName")) { F.Row<FY01TalentRow>(K::Talent).DisplayName = FText::FromString(TEXT(" \t")); }
    if (Case == TEXT("TalentLegacy")) { F.Row<FY01TalentRow>(K::Talent).Ability = UY01BasicAttackAbility::StaticClass(); }
    Offers->CopyCompleteValue(Offers->ContainerPtrToValuePtr<void>(P), &OfferRefs);
    Recipes->CopyCompleteValue(Recipes->ContainerPtrToValuePtr<void>(P), &RecipeRefs);
    Choice->CopyCompleteValue(Choice->ContainerPtrToValuePtr<void>(P), &ChoiceRef);
    P->Character = F.Ref(K::Character); P->OrderedWaves = F.Waves();
    for (const auto& Pair : F.Tables) { P->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
    const UY01PlaygroundProfile* Base = P; const auto Result = Base->Validate();
    if (Case == TEXT("Explicit"))
    { for (const auto& Error : Result.Errors) { AddInfo(Error.Field + TEXT(": ") + Error.Message); } TestTrue(TEXT("合法显式经济配置"), Result.IsValid()); }
    else { TestFalse(TEXT("真实基类入口必须拒绝经济字段或池内错误，不能只检查ContentEquipment"), Result.IsValid()); }
    return true;
}
#endif
