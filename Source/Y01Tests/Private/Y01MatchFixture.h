#pragma once
#include "Data/Y01DataCatalog.h"
#include "Combat/Y01Combat.h"
#include "UObject/StrongObjectPtr.h"

// 完整19表只存在于测试模块；不注入全局Settings或正式Subsystem。
struct FY01MatchFixture
{
    TArray<TStrongObjectPtr<UDataTable>> Owners;
    FY01TableMap Tables;
    static FY01DataRef Ref(EY01TableKind Kind, FName Name = TEXT("Default"))
    { FY01DataRef R; R.Type = Kind; R.RowName = Name; return R; }
    template<class T> void Add(EY01TableKind Kind, const T& Row, FName Name = TEXT("Default"))
    {
        if (!Tables.Contains(Kind))
        {
            auto* TBL = NewObject<UDataTable>(); TBL->RowStruct = T::StaticStruct();
            Owners.Emplace(TBL); Tables.Add(Kind, TBL);
        }
        const_cast<UDataTable*>(Tables[Kind])->AddRow(Name, Row);
    }
    template<class T> T& Row(EY01TableKind Kind, FName Name = TEXT("Default"))
    { return *const_cast<UDataTable*>(Tables[Kind])->FindRow<T>(Name, TEXT("MatchFixture")); }
    FY01MatchFixture()
    {
        for (int32 I = 0; I < 5; ++I)
        { FY01QualityRow Q; Q.Quality = static_cast<EY01Quality>(I); Add(EY01TableKind::Quality, Q, FName(*FString::FromInt(I))); }
        FY01SkillRow Skill; Skill.Ability = UY01BasicAttackAbility::StaticClass(); Add(EY01TableKind::Skill, Skill);
        Add(EY01TableKind::Talent, FY01TalentRow());
        FY01EquipmentRow Equipment; Add(EY01TableKind::Equipment, Equipment);
        Equipment.Quality = EY01Quality::Blue; Add(EY01TableKind::Equipment, Equipment, TEXT("Blue"));
        Add(EY01TableKind::MetaItem, FY01MetaItemRow());
        FY01WeightedEntry Entry; Entry.Item = Ref(EY01TableKind::Equipment);
        FY01LocalItemPoolRow Pool; Pool.Entries.Add(Entry); Add(EY01TableKind::LocalItemPool, Pool);
        Pool.Quality = EY01Quality::Blue; Pool.Entries[0].Item.RowName = TEXT("Blue"); Add(EY01TableKind::LocalItemPool, Pool, TEXT("Blue"));
        FY01MetaLootPoolRow Meta; Entry.Item = Ref(EY01TableKind::MetaItem); Meta.Entries.Add(Entry); Add(EY01TableKind::MetaLootPool, Meta);
        FY01RewardPackageRow Reward; Reward.Gold = 10; Reward.Experience = 5; Add(EY01TableKind::RewardPackage, Reward);
        FY01MonsterRow Monster; Monster.Health = 20; Monster.AttackDamage = 25; Monster.Range = 500;
        Monster.Skills.Add(Ref(EY01TableKind::Skill)); Monster.DropReward = Ref(EY01TableKind::RewardPackage); Add(EY01TableKind::Monster, Monster);
        FY01SpawnEntry Spawn; Spawn.Monster = Ref(EY01TableKind::Monster); Spawn.Count = 2; Spawn.DelaySeconds = 0.5f;
        FY01WaveRow Wave; Wave.PreparationSeconds = 1; Wave.Spawns.Add(Spawn); Wave.Reward = Ref(EY01TableKind::RewardPackage);
        Add(EY01TableKind::Wave, Wave); Wave.Type = EY01WaveType::Boss; Wave.PreparationSeconds = 2;
        Wave.Spawns[0].Count = 1; Add(EY01TableKind::Wave, Wave, TEXT("Boss"));
        FY01TowerFloorRow Floor; Floor.Spawns.Add(Spawn); Floor.Reward = Wave.Reward; Add(EY01TableKind::TowerFloor, Floor);
        FY01TrainingStageRow Training; Training.Spawns.Add(Spawn); Training.Reward = Wave.Reward; Add(EY01TableKind::TrainingStage, Training);
        Add(EY01TableKind::LevelExperience, FY01LevelExperienceRow()); Add(EY01TableKind::MatchRules, FY01MatchRulesRow());
        FY01CharacterRow Character; Character.Health = 100; Character.AttackDamage = 20; Character.Range = 500;
        Character.InitialEquipment.Init(Ref(EY01TableKind::Equipment), 6); Character.InitialSkills.Init(Ref(EY01TableKind::Skill), 4);
        Add(EY01TableKind::Character, Character);
        FY01ShopOfferRow Shop; Shop.Pool = Ref(EY01TableKind::LocalItemPool); Add(EY01TableKind::ShopOffer, Shop);
        FY01SynthesisRow Synthesis; Synthesis.OutputPool = Ref(EY01TableKind::LocalItemPool, TEXT("Blue")); Add(EY01TableKind::Synthesis, Synthesis);
        Add(EY01TableKind::WarehouseEvent, FY01WarehouseEventRow());
        FY01TalentChoiceRow Choice; Choice.Candidates.Init(Ref(EY01TableKind::Talent), 3); Add(EY01TableKind::TalentChoice, Choice);
    }
    TArray<FY01DataRef> Waves() const { return { Ref(EY01TableKind::Wave), Ref(EY01TableKind::Wave, TEXT("Boss")) }; }
};
