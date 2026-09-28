#include "Misc/AutomationTest.h"
#include "Playground/Y01PlaygroundProfile.h"
#include "Playground/Y01PlaygroundSeed.h"
#include "Playground/Y01PlaygroundPassiveDamageEffect.h"
#include "Y01MatchFixture.h"
#include "UObject/UnrealType.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
using EKind = EY01TableKind;
using FFixture = FY01MatchFixture;

TStrongObjectPtr<UY01PlaygroundProfile> FixtureProfile(const FFixture& Fixture)
{
    TStrongObjectPtr<UY01PlaygroundProfile> Profile(NewObject<UY01PlaygroundProfile>());
    for (const auto& Pair : Fixture.Tables) { Profile->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
    Profile->OrderedWaves = Fixture.Waves();
    Profile->Character = FFixture::Ref(EKind::Character);
    return Profile;
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01PlaygroundDataTest, "Y01.Playground.Data.Memory",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01PlaygroundDataTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("SeedComplete"), TEXT("Catalog"), TEXT("MissingTable"),
        TEXT("MissingCharacter"), TEXT("WrongCharacterKind"), TEXT("MissingWave"), TEXT("WrongWaveKind"),
        TEXT("EmptyWaves"), TEXT("NoRuntimeFallback"), TEXT("ReflectionOwnership"), TEXT("SkillSlots"), TEXT("PassiveEffect") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01PlaygroundDataTest::RunTest(const FString& Case)
{
    if (Case == TEXT("NoRuntimeFallback"))
    {
        TStrongObjectPtr<UY01PlaygroundProfile> Empty(NewObject<UY01PlaygroundProfile>());
        TestFalse(TEXT("空配置必须失败，禁止运行时补种子"), Empty->Validate().IsValid());
        TestFalse(TEXT("空目录不能伪装有效"), Empty->BuildCatalog().Validate().IsValid());
        TestEqual(TEXT("访问与验证不生成表"), Empty->Tables.Num(), 0);
        TestTrue(TEXT("访问与验证不生成波次"), Empty->OrderedWaves.IsEmpty());
        TestTrue(TEXT("访问与验证不生成角色"), Empty->Character.RowName.IsNone());
        return true;
    }
    if (Case == TEXT("PassiveEffect"))
    {
        const auto* Effect = GetDefault<UY01PlaygroundPassiveDamageEffect>();
        TestTrue(TEXT("被动GE无限持续"), Effect->DurationPolicy == EGameplayEffectDurationType::Infinite);
        if (!TestEqual(TEXT("只有一个示例修饰"), Effect->Modifiers.Num(), 1)) { return false; }
        const auto& Mod = Effect->Modifiers[0];
        TestTrue(TEXT("仅增加攻击力"), Mod.Attribute == UY01AttributeSet::GetAttackDamageAttribute());
        TestTrue(TEXT("加法"), Mod.ModifierOp == EGameplayModOp::Additive);
        float Magnitude = 0;
        TestTrue(TEXT("固定示例幅度"), Mod.ModifierMagnitude.GetStaticMagnitudeIfPossible(1.f, Magnitude));
        TestEqual(TEXT("固定示例攻击力加五"), Magnitude, 5.f);
        return true;
    }
    if (Case == TEXT("SeedComplete") || Case == TEXT("SkillSlots"))
    {
        TStrongObjectPtr<UY01PlaygroundProfile> Profile(Y01PlaygroundEditor::CreateSeed(GetTransientPackage()));
        if (!TestNotNull(TEXT("编辑器种子配置"), Profile.Get())) { return false; }
        if (!TestEqual(TEXT("完整19张表"), Profile->Tables.Num(), 19)) { return false; }
        TestTrue(TEXT("种子目录合法"), Profile->Validate().IsValid());
        const FY01DataCatalog Catalog = Profile->BuildCatalog();
        FY01DataValidation Errors;
        const auto* Character = Catalog.FindRow<FY01CharacterRow>(Profile->Character, Errors);
        if (!TestNotNull(TEXT("角色行可查询"), Character)) { return false; }
        if (Case == TEXT("SkillSlots"))
        {
            if (!TestEqual(TEXT("三个配置技能，第四槽留空"), Character->InitialSkills.Num(), 3)) { return false; }
            const FName Expected[] = { TEXT("BasicAttack"), TEXT("HeavyAttack"), TEXT("Fortitude") };
            for (int32 I = 0; I < 3; ++I) { TestEqual(TEXT("稳定技能槽顺序"), Character->InitialSkills[I].RowName, Expected[I]); }
            TArray<FY01DataRef> Slots = Character->InitialSkills;
            Slots.SetNum(Y01Data::SkillSlotCount);
            TestTrue(TEXT("第四槽补空身份"), Slots[3].Type == EKind::None && Slots[3].RowName.IsNone());
            const auto* Basic = Catalog.FindRow<FY01SkillRow>(Slots[0], Errors);
            const auto* Heavy = Catalog.FindRow<FY01SkillRow>(Slots[1], Errors);
            const auto* Passive = Catalog.FindRow<FY01SkillRow>(Slots[2], Errors);
            const auto* Enemy = Catalog.FindRow<FY01SkillRow>(FFixture::Ref(EKind::Skill, TEXT("EnemyAttack")), Errors);
            if (!Basic || !Heavy || !Passive || !Enemy) { AddError(TEXT("技能示例缺行")); return false; }
            TestTrue(TEXT("两主动"), Basic->Type == EY01SkillType::Active && Heavy->Type == EY01SkillType::Active);
            TestEqual(TEXT("基础攻击CD"), Basic->CooldownSeconds, 0.7f);
            TestEqual(TEXT("重攻击仅展示不同CD"), Heavy->CooldownSeconds, 2.f);
            TestEqual(TEXT("可选敌方攻击CD"), Enemy->CooldownSeconds, 1.5f);
            TestTrue(TEXT("两个主动复用同一GA而非冒充不同伤害"), Basic->Ability.LoadSynchronous() == UY01BasicAttackAbility::StaticClass()
                && Heavy->Ability.LoadSynchronous() == UY01BasicAttackAbility::StaticClass());
            TestTrue(TEXT("被动只引用测试GE"), Passive->Type == EY01SkillType::Passive && Passive->Ability.IsNull()
                && Passive->Effect.LoadSynchronous() == UY01PlaygroundPassiveDamageEffect::StaticClass());
            return true;
        }
        TestEqual(TEXT("玩家生命"), Character->Health, 200.f);
        TestEqual(TEXT("玩家基础攻击"), Character->AttackDamage, 20.f);
        TestEqual(TEXT("玩家范围"), Character->Range, 500.f);
        TestEqual(TEXT("玩家速度"), Character->MoveSpeed, 500.f);
        TestTrue(TEXT("装备示例不超过六槽"), Character->InitialEquipment.Num() > 0 && Character->InitialEquipment.Num() <= 6);
        if (!TestEqual(TEXT("两波顺序"), Profile->OrderedWaves.Num(), 2)) { return false; }
        for (int32 I = 0; I < 2; ++I)
        {
            const auto* Wave = Catalog.FindRow<FY01WaveRow>(Profile->OrderedWaves[I], Errors);
            if (!TestNotNull(TEXT("波次引用"), Wave)) { return false; }
            TestEqual(TEXT("波次英文行名"), Profile->OrderedWaves[I].RowName, FName(I == 0 ? TEXT("Default") : TEXT("Boss")));
            TestEqual(TEXT("准备时长"), Wave->PreparationSeconds, I == 0 ? 5.f : 8.f);
            TestTrue(TEXT("普通与Boss类型"), Wave->Type == (I == 0 ? EY01WaveType::Normal : EY01WaveType::Boss));
            if (!TestEqual(TEXT("刷怪条目"), Wave->Spawns.Num(), 1)) { return false; }
            TestEqual(TEXT("刷怪数量"), Wave->Spawns[0].Count, I == 0 ? 3 : 1);
            TestEqual(TEXT("刷怪延时"), Wave->Spawns[0].DelaySeconds, 0.5f);
            const auto* Monster = Catalog.FindRow<FY01MonsterRow>(Wave->Spawns[0].Monster, Errors);
            const auto* Reward = Catalog.FindRow<FY01RewardPackageRow>(Wave->Reward, Errors);
            if (!Monster || !Reward) { AddError(TEXT("怪物或奖励引用缺失")); return false; }
            TestEqual(TEXT("怪物生命"), Monster->Health, I == 0 ? 60.f : 150.f);
            TestEqual(TEXT("怪物攻击"), Monster->AttackDamage, I == 0 ? 5.f : 10.f);
            TestEqual(TEXT("怪物范围"), Monster->Range, I == 0 ? 140.f : 180.f);
            TestEqual(TEXT("怪物速度"), Monster->MoveSpeed, I == 0 ? 120.f : 90.f);
            if (!TestEqual(TEXT("种子怪物恰好一个技能"), Monster->Skills.Num(), 1)) { return false; }
            TestTrue(TEXT("种子怪物引用技能表"), Monster->Skills[0].Type == EKind::Skill);
            TestEqual(TEXT("种子怪物使用EnemyAttack行"), Monster->Skills[0].RowName, FName(TEXT("EnemyAttack")));
            const auto* MonsterSkill = Catalog.FindRow<FY01SkillRow>(Monster->Skills[0], Errors);
            if (!TestNotNull(TEXT("种子怪物实际技能可解析"), MonsterSkill)) { return false; }
            TestEqual(TEXT("种子怪物实际引用CD为1.5"), MonsterSkill->CooldownSeconds, 1.5f);
            TestEqual(TEXT("波次表金币奖励"), Reward->Gold, I == 0 ? 10 : 25);
            TestEqual(TEXT("波次表经验奖励"), Reward->Experience, I == 0 ? 5 : 12);
        }
        FFixture Original;
        TestEqual(TEXT("原smoke夹具生命不变"), Original.Row<FY01CharacterRow>(EKind::Character).Health, 100.f);
        TestEqual(TEXT("原smoke夹具怪物不变"), Original.Row<FY01MonsterRow>(EKind::Monster).Health, 20.f);
        TestTrue(TEXT("查询无错误"), Errors.IsValid());
        return true;
    }
    FFixture Fixture;
    auto Profile = FixtureProfile(Fixture);
    if (Case == TEXT("ReflectionOwnership"))
    {
        const auto* Property = FindFProperty<FMapProperty>(Profile->GetClass(), TEXT("Tables"));
        if (!TestNotNull(TEXT("表映射反射强持有"), Property)) { return false; }
        TestTrue(TEXT("映射值为对象属性"), CastField<FObjectPropertyBase>(Property->ValueProp) != nullptr);
        TestTrue(TEXT("映射可编辑且非瞬态"), Property->HasAnyPropertyFlags(CPF_Edit) && !Property->HasAnyPropertyFlags(CPF_Transient));
        for (const TCHAR* Name : { TEXT("OrderedWaves"), TEXT("Character") })
        { TestTrue(TEXT("配置引用可编辑"), FindFProperty<FProperty>(Profile->GetClass(), Name)->HasAnyPropertyFlags(CPF_Edit)); }
        return true;
    }
    if (Case == TEXT("Catalog"))
    {
        TestTrue(TEXT("目录读取配置中的全部表"), Profile->BuildCatalog().Validate().IsValid());
        Fixture.Row<FY01RewardPackageRow>(EKind::RewardPackage).Gold = 77;
        FY01DataValidation Errors;
        const auto* Reward = Profile->BuildCatalog().FindRow<FY01RewardPackageRow>(FFixture::Ref(EKind::RewardPackage), Errors);
        if (!TestNotNull(TEXT("只读查询真实编辑值"), Reward)) { return false; }
        TestEqual(TEXT("无运行时默认值回填"), Reward->Gold, 77);
        return true;
    }
    FString Field;
    if (Case == TEXT("MissingTable")) { Profile->Tables.Remove(EKind::Monster); Field = TEXT("Table"); }
    if (Case == TEXT("MissingCharacter")) { Profile->Character.RowName = TEXT("Missing"); Field = TEXT("Character"); }
    if (Case == TEXT("WrongCharacterKind")) { Profile->Character.Type = EKind::Monster; Field = TEXT("Character"); }
    if (Case == TEXT("MissingWave")) { Profile->OrderedWaves[0].RowName = TEXT("Missing"); Field = TEXT("OrderedWaves[0]"); }
    if (Case == TEXT("WrongWaveKind")) { Profile->OrderedWaves[0].Type = EKind::Monster; Field = TEXT("OrderedWaves[0]"); }
    if (Case == TEXT("EmptyWaves")) { Profile->OrderedWaves.Empty(); Field = TEXT("OrderedWaves"); }
    const FY01DataValidation Result = Profile->Validate();
    TestFalse(TEXT("配置缺失或错误引用必须失败"), Result.IsValid());
    TestTrue(TEXT("错误须定位具体字段并给出明细"), Result.Errors.ContainsByPredicate([&](const FY01DataError& Error)
    { return Error.Field == Field && !Error.Message.IsEmpty(); }));
    return true;
}
#endif
