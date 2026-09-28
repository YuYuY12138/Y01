#include "Playground/Y01PlaygroundSeed.h"
#include "Playground/Y01PlaygroundProfile.h"

#if WITH_EDITOR
#include "Playground/Y01PlaygroundPassiveDamageEffect.h"
#include "Y01MatchFixture.h"

UY01PlaygroundProfile* Y01PlaygroundEditor::CreateSeed(UObject* Outer)
{
    using EKind = EY01TableKind;
    using FFixture = FY01MatchFixture;
    const FFixture Fixture;
    auto* Profile = NewObject<UY01PlaygroundProfile>(Outer);
    // 复制完整夹具再改示例；绝不修改原smoke夹具及其表。
    for (const auto& Pair : Fixture.Tables)
    {
        Profile->Tables.Add(Pair.Key, DuplicateObject<UDataTable>(Pair.Value, Profile));
    }
    Profile->OrderedWaves = Fixture.Waves();
    Profile->Character = FFixture::Ref(EKind::Character);
    auto* Skills = Profile->Tables[EKind::Skill].Get();
    Skills->EmptyTable();
    FY01SkillRow Active;
    Active.Ability = UY01BasicAttackAbility::StaticClass();
    Active.CooldownSeconds = 0.7f;
    Skills->AddRow(TEXT("BasicAttack"), Active);
    // 两主动使用同一GA，仅CD不同；伤害仍来自角色属性。
    Active.CooldownSeconds = 2.f;
    Skills->AddRow(TEXT("HeavyAttack"), Active);
    Active.CooldownSeconds = 1.5f;
    Skills->AddRow(TEXT("EnemyAttack"), Active);
    FY01SkillRow Passive;
    Passive.Type = EY01SkillType::Passive;
    Passive.Effect = UY01PlaygroundPassiveDamageEffect::StaticClass();
    Skills->AddRow(TEXT("Fortitude"), Passive);

    auto* Character = Profile->Tables[EKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("PlaygroundSeed"));
    Character->Health = 200; Character->AttackDamage = 20; Character->Range = 500; Character->MoveSpeed = 500;
    Character->InitialSkills = { FFixture::Ref(EKind::Skill, TEXT("BasicAttack")),
        FFixture::Ref(EKind::Skill, TEXT("HeavyAttack")), FFixture::Ref(EKind::Skill, TEXT("Fortitude")) };

    auto* Monsters = Profile->Tables[EKind::Monster].Get();
    auto* Normal = Monsters->FindRow<FY01MonsterRow>(TEXT("Default"), TEXT("PlaygroundSeed"));
    Normal->Health = 60; Normal->AttackDamage = 5; Normal->Range = 140; Normal->MoveSpeed = 120;
    Normal->Skills = { FFixture::Ref(EKind::Skill, TEXT("EnemyAttack")) };
    FY01MonsterRow Boss = *Normal;
    Boss.Health = 150; Boss.AttackDamage = 10; Boss.Range = 180; Boss.MoveSpeed = 90;
    Boss.DropReward = FFixture::Ref(EKind::RewardPackage, TEXT("BossReward"));
    Monsters->AddRow(TEXT("Boss"), Boss);

    FY01RewardPackageRow BossReward;
    BossReward.Gold = 25; BossReward.Experience = 12;
    Profile->Tables[EKind::RewardPackage]->AddRow(TEXT("BossReward"), BossReward);
    auto* Waves = Profile->Tables[EKind::Wave].Get();
    auto* NormalWave = Waves->FindRow<FY01WaveRow>(TEXT("Default"), TEXT("PlaygroundSeed"));
    NormalWave->PreparationSeconds = 5; NormalWave->Spawns[0].Count = 3;
    auto* BossWave = Waves->FindRow<FY01WaveRow>(TEXT("Boss"), TEXT("PlaygroundSeed"));
    BossWave->PreparationSeconds = 8; BossWave->Spawns[0].Count = 1;
    BossWave->Spawns[0].Monster = FFixture::Ref(EKind::Monster, TEXT("Boss"));
    BossWave->Reward = FFixture::Ref(EKind::RewardPackage, TEXT("BossReward"));

    // 非战斗表仅为合法数据占位，不实现商店、合成、爬塔等玩法。
    auto* Qualities = Profile->Tables[EKind::Quality].Get();
    Qualities->EmptyTable();
    const TCHAR* Names[] = { TEXT("Green"), TEXT("Blue"), TEXT("Purple"), TEXT("Gold"), TEXT("Red") };
    for (uint8 I = 0; I < UE_ARRAY_COUNT(Names); ++I)
    {
        FY01QualityRow Quality; Quality.Quality = static_cast<EY01Quality>(I);
        Quality.DisplayName = FText::FromString(Names[I]);
        Qualities->AddRow(FName(Names[I]), Quality);
    }
    return Profile;
}
#endif
