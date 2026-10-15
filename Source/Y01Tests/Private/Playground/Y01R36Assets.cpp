#include "Playground/Y01R36Assets.h"
UY01R36AssetsCommandlet::UY01R36AssetsCommandlet()
{ IsClient = false; IsServer = false; IsEditor = true; LogToConsole = true; }
#if WITH_EDITOR
#include "Y01MatchFixture.h"
#include "Playground/Y01R36Runtime.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Misc/Parse.h"
#include "GameFramework/PlayerStart.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
UY01R36Profile* Y01R36Editor::CreateSeed(UObject* Outer, bool bCooldownFixture)
{
    using K = EY01TableKind;
    FY01MatchFixture F;
    FY01SourceEffectRow Effect; Effect.Magnitude = 10; F.Add(K::SourceEffect, Effect);
    // 可选表建独立空表供后续阶段扩展；S1不伪装成长或抽样消费者。
    F.Add(K::GrowthRule, FY01GrowthRuleRow()); const_cast<UDataTable*>(F.Tables[K::GrowthRule])->EmptyTable();
    F.Add(K::QualityRollPool, FY01QualityRollPoolRow()); const_cast<UDataTable*>(F.Tables[K::QualityRollPool])->EmptyTable();
    for (const FName Name : { FName(TEXT("Default")), FName(TEXT("Blue")) })
    {
        auto& E = F.Row<FY01EquipmentRow>(K::Equipment, Name);
        E.DisplayName = FText::FromString(TEXT("程序测试装备")); E.EffectIds = {F.Ref(K::SourceEffect)};
    }
    auto& Skill = F.Row<FY01SkillRow>(K::Skill); Skill.DisplayName = FText::FromString(TEXT("程序测试主动")); Skill.CooldownSeconds = bCooldownFixture ? 60 : 8;
    F.Add(K::Skill, Skill, TEXT("Second"));
    FY01SkillRow Passive; Passive.Type = EY01SkillType::Passive; Passive.DisplayName = FText::FromString(TEXT("程序测试被动"));
    Passive.EffectIds = {F.Ref(K::SourceEffect)}; F.Add(K::Skill, Passive, TEXT("Passive"));
    auto& Character = F.Row<FY01CharacterRow>(K::Character);
    Character.InitialEquipment.Reset(); Character.InitialSkills.Reset(); Character.AttackDamage = 30; Character.Health = 300;
    if (bCooldownFixture)
    {
        auto& Monster = F.Row<FY01MonsterRow>(K::Monster);
        Monster.Health = 1000000; Monster.AttackDamage = 0; Monster.MoveSpeed = 0;
    }
    auto* P = NewObject<UY01R36Profile>(Outer);
    for (const auto& Pair : F.Tables) { P->Tables.Add(Pair.Key, DuplicateObject<UDataTable>(Pair.Value, P)); }
    P->Character = F.Ref(K::Character); P->OrderedWaves = F.Waves();
    P->bTrainingEnabled = true; P->bR34AutoAttackEnabled = true;
    P->ContentEquipment = {F.Ref(K::Equipment), F.Ref(K::Equipment, TEXT("Blue"))};
    P->ContentSkills = {F.Ref(K::Skill), F.Ref(K::Skill, TEXT("Second")), F.Ref(K::Skill, TEXT("Passive"))};
    return P;
}
namespace
{
using K = EY01TableKind;
UY01R36Profile* CreateEconomySeed(UObject* Outer, int32 Variant)
{
    auto* P = Y01R36Editor::CreateSeed(Outer, true);
    const auto Ref = FY01MatchFixture::Ref;
    auto Add = [&](K Kind, const auto& Row, FName Name) { P->Tables[Kind]->AddRow(Name, Row); };
    auto Clear = [&](K Kind) { P->Tables[Kind]->EmptyTable(); };
    for (const auto Kind : {K::LocalItemPool, K::ShopOffer, K::Synthesis, K::Talent, K::TalentChoice}) { Clear(Kind); }
    FY01SourceEffectRow Attack; Attack.Magnitude = 5;
    FY01SourceEffectRow Income; Income.Kind = EY01SourceEffectKind::GoldPerSecond; Income.Magnitude = 1;
    Add(K::SourceEffect, Attack, TEXT("Default")); Add(K::SourceEffect, Income, TEXT("Income"));
    Attack.Magnitude = Variant == 2 ? 7 : 5; Income.Magnitude = Variant == 2 ? 2 : 1;
    Add(K::SourceEffect, Attack, TEXT("EvidenceAttack")); Add(K::SourceEffect, Income, TEXT("EvidenceIncome"));
    const TArray<FY01DataRef> Effects = {Ref(K::SourceEffect, TEXT("Default")), Ref(K::SourceEffect, TEXT("Income"))};
    FY01GrowthRuleRow Growth; Growth.Target = EY01GrowthTarget::Player; Growth.Attribute = EY01GrowthAttribute::Strength; Growth.Delta = 1;
    Add(K::GrowthRule, Growth, TEXT("PlayerStrength"));
    const TArray<FY01DataRef> PlayerGrowth = {Ref(K::GrowthRule, TEXT("PlayerStrength"))};
    auto* Character = P->Tables[K::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("S2"));
    Character->Strength = 0;
    for (const auto& Pair : P->Tables[K::Wave]->GetRowMap())
    {
        auto& Wave = *reinterpret_cast<FY01WaveRow*>(Pair.Value);
        Wave.PreparationSeconds = 120; Wave.Type = EY01WaveType::Normal;
    }
    Add(K::RewardPackage, FY01RewardPackageRow(), TEXT("Default"));
    for (const auto& Pair : P->Tables[K::Equipment]->GetRowMap())
    { reinterpret_cast<FY01EquipmentRow*>(Pair.Value)->EffectIds = Effects; }
    auto* Passive = P->Tables[K::Skill]->FindRow<FY01SkillRow>(TEXT("Passive"), TEXT("S2"));
    Passive->EffectIds = Effects; Passive->GrowthRuleIds = PlayerGrowth;
    Passive->DisplayName = FText::FromString(TEXT("研习之书"));
    FY01EquipmentRow Evidence; Evidence.DisplayName = FText::FromString(TEXT("试炼之刃"));
    Evidence.EffectIds = {Ref(K::SourceEffect, TEXT("EvidenceAttack")), Ref(K::SourceEffect, TEXT("EvidenceIncome"))};
    Add(K::Equipment, Evidence, TEXT("EvidenceEquipment"));
    P->ContentEquipment.Insert(Ref(K::Equipment, TEXT("EvidenceEquipment")), 0);
    auto AddFixedOffer = [&](FName Name, const FY01DataRef& Item, const TCHAR* Display)
    {
        FY01LocalItemPoolRow Pool; Pool.ItemType = Item.Type;
        FY01WeightedEntry Entry; Entry.Item = Item; Pool.Entries.Add(Entry); Add(K::LocalItemPool, Pool, Name);
        FY01ShopOfferRow Offer; Offer.Price = 10; Offer.Pool = Ref(K::LocalItemPool, Name); Offer.DisplayName = FText::FromString(Display);
        Add(K::ShopOffer, Offer, Name); P->ContentShopOffers.Add(Ref(K::ShopOffer, Name));
    };
    AddFixedOffer(TEXT("EvidenceEquipment"), Ref(K::Equipment, TEXT("EvidenceEquipment")), TEXT("试炼之刃"));
    AddFixedOffer(TEXT("EvidenceBook"), Ref(K::Skill, TEXT("Passive")), TEXT("研习之书"));
    FY01ShopOfferRow Direct; Direct.ShopKind = EY01ShopKind::DirectEffect; Direct.Price = 50;
    Direct.DisplayName = FText::FromString(TEXT("力量祝福（限购一次）")); Direct.EffectIds = Effects; Direct.GrowthRuleIds = PlayerGrowth;
    Add(K::ShopOffer, Direct, TEXT("EvidenceBlessing")); P->ContentShopOffers.Add(Ref(K::ShopOffer, TEXT("EvidenceBlessing")));
    FY01TalentChoiceRow Choice;
    const TCHAR* TalentNames[] = {TEXT("坚毅研习"), TEXT("灵巧研习"), TEXT("专注研习")};
    for (int32 I = 0; I < 3; ++I)
    {
        const FName Name(*FString::Printf(TEXT("EvidenceTalent%d"), I + 1));
        FY01TalentRow Talent; Talent.DisplayName = FText::FromString(TalentNames[I]); Talent.EffectIds = Effects; Talent.GrowthRuleIds = PlayerGrowth;
        Add(K::Talent, Talent, Name); Choice.Candidates.Add(Ref(K::Talent, Name));
    }
    Add(K::TalentChoice, Choice, TEXT("EvidenceChoice")); P->TalentChoice = Ref(K::TalentChoice, TEXT("EvidenceChoice"));
    const TCHAR* Grades[] = {TEXT("D"), TEXT("C"), TEXT("B"), TEXT("A"), TEXT("S")};
    for (const K Type : {K::Equipment, K::Skill})
    {
        const TCHAR* Prefix = Type == K::Equipment ? TEXT("Equipment") : TEXT("Book");
        auto PoolName = [&](int32 Grade) { return FName(*FString::Printf(TEXT("%s_%s"), Prefix, Grades[Grade])); };
        for (int32 Grade = 0; Grade < 5; ++Grade)
        {
            FY01LocalItemPoolRow Pool; Pool.ItemType = Type; Pool.Quality = static_cast<EY01Quality>(Grade);
            for (int32 Item = 0; Item < 2; ++Item)
            {
                const FName Name(*FString::Printf(TEXT("%s_%s_%d"), Prefix, Grades[Grade], Item + 1));
                if (Type == K::Equipment)
                {
                    FY01EquipmentRow Row; Row.Quality = Pool.Quality; Row.EffectIds = Effects;
                    Row.DisplayName = FText::FromString(FString::Printf(TEXT("%s级%s"), Grades[Grade], Item ? TEXT("守卫护符") : TEXT("远行长剑")));
                    Add(Type, Row, Name); P->ContentEquipment.Add(Ref(Type, Name));
                }
                else
                {
                    FY01SkillRow Row; Row.Quality = Pool.Quality;
                    Row.DisplayName = FText::FromString(FString::Printf(TEXT("%s级%s"), Grades[Grade], Item ? TEXT("主动攻击书") : TEXT("被动研习书")));
                    if (Item) { Row.Ability = UY01BasicAttackAbility::StaticClass(); Row.CooldownSeconds = 60; }
                    else { Row.Type = EY01SkillType::Passive; Row.EffectIds = Effects; Row.GrowthRuleIds = PlayerGrowth; }
                    Add(Type, Row, Name); P->ContentSkills.Add(Ref(Type, Name));
                }
                FY01WeightedEntry Entry; Entry.Item = Ref(Type, Name); Pool.Entries.Add(Entry);
            }
            Add(K::LocalItemPool, Pool, PoolName(Grade));
        }
        for (int32 Grade = 0; Grade < 4; ++Grade)
        {
            FY01QualityRollPoolRow Tier; Tier.ItemType = Type;
            for (int32 Branch = 0; Branch < 2; ++Branch)
            {
                FY01QualityRollEntry Entry; Entry.Quality = static_cast<EY01Quality>(Grade + Branch);
                Entry.Pool = Ref(K::LocalItemPool, PoolName(Grade + Branch));
                Entry.Weight = Branch ? (Grade == 3 ? 10 : 25) : (Grade == 3 ? 90 : 75); Tier.Entries.Add(Entry);
            }
            Add(K::QualityRollPool, Tier, PoolName(Grade));
            FY01ShopOfferRow Offer; Offer.ShopKind = EY01ShopKind::RandomTier; Offer.Price = 100 * (1 << Grade);
            Offer.TierPool = Ref(K::QualityRollPool, PoolName(Grade));
            Offer.DisplayName = FText::FromString(FString::Printf(TEXT("%s级随机%s"), Grades[Grade], Type == K::Equipment ? TEXT("装备") : TEXT("技能书")));
            Add(K::ShopOffer, Offer, PoolName(Grade)); P->ContentShopOffers.Add(Ref(K::ShopOffer, PoolName(Grade)));
            FY01SynthesisRow Recipe; Recipe.InputType = Type; Recipe.InputQuality = static_cast<EY01Quality>(Grade);
            Recipe.OutputPool = Ref(K::LocalItemPool, PoolName(Grade + 1));
            Add(K::Synthesis, Recipe, PoolName(Grade)); P->ContentRecipes.Add(Ref(K::Synthesis, PoolName(Grade)));
        }
    }
    P->bEvidenceMode = true; P->bEconomyEnabled = true; P->InitialGold = Variant == 1 ? 0 : 200; P->BaseGoldPerSecond = 1;
    return P;
}
UY01R36Profile* CreatePresentationSeed(UObject* Outer, int32 Variant)
{
    auto* P = Y01R36Editor::CreateSeed(Outer);
    const auto Ref = FY01MatchFixture::Ref;
    auto Add = [&](K Kind, const auto& Row, FName Name) { P->Tables[Kind]->AddRow(Name, Row); };
    P->Tables[K::ShopOffer]->EmptyTable();
    FY01GrowthRuleRow Attack; Attack.Delta = 1; Add(K::GrowthRule, Attack, TEXT("S3Attack"));
    FY01GrowthRuleRow Strength; Strength.Target = EY01GrowthTarget::Player; Strength.Attribute = EY01GrowthAttribute::Strength; Strength.Delta = 1;
    Add(K::GrowthRule, Strength, TEXT("S3Strength"));
    FY01EquipmentRow Blade; Blade.DisplayName = FText::FromString(TEXT("S3 试炼成长之刃"));
    Blade.EffectIds = {Ref(K::SourceEffect, TEXT("Default"))};
    Blade.GrowthRuleIds = {Ref(K::GrowthRule, TEXT("S3Attack")), Ref(K::GrowthRule, TEXT("S3Strength"))};
    Add(K::Equipment, Blade, TEXT("S3Blade"));
    FY01WeightedEntry Entry; Entry.Item = Ref(K::Equipment, TEXT("S3Blade"));
    FY01LocalItemPoolRow Pool; Pool.Entries.Add(Entry); Add(K::LocalItemPool, Pool, TEXT("S3Blade"));
    FY01ShopOfferRow Offer; Offer.Pool = Ref(K::LocalItemPool, TEXT("S3Blade")); Offer.Price = 10; Offer.DisplayName = Blade.DisplayName;
    Add(K::ShopOffer, Offer, TEXT("S3Blade")); P->ContentShopOffers = {Ref(K::ShopOffer, TEXT("S3Blade"))};
    P->ContentEquipment = {Ref(K::Equipment, TEXT("S3Blade"))}; P->ContentSkills.Reset();
    FY01RewardPackageRow Empty; Add(K::RewardPackage, Empty, TEXT("Default"));
    FY01RewardPackageRow Drop; FY01RewardEntry RewardItem; RewardItem.Item = Entry.Item; Drop.Items.Add(RewardItem);
    Add(K::RewardPackage, Drop, TEXT("S3Drop"));
    auto* Monster = P->Tables[K::Monster]->FindRow<FY01MonsterRow>(TEXT("Default"), TEXT("S3"));
    Monster->Health = 20; Monster->AttackDamage = 0; Monster->MoveSpeed = 0; Monster->Skills.Reset();
    Monster->DropReward = Ref(K::RewardPackage, TEXT("S3Drop")); Monster->ItemDropChance = Variant == 0 ? .35 : Variant == 1 ? 1. : 0.;
    auto* Character = P->Tables[K::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("S3")); Character->Strength = 0;
    FY01SpawnEntry Spawn; Spawn.Monster = Ref(K::Monster, TEXT("Default"));
    FY01TrainingStageRow Training; Training.SpawnInterval = 4; Training.MaxAlive = 1; Training.Spawns.Add(Spawn); Training.Reward = Ref(K::RewardPackage, TEXT("Default"));
    Add(K::TrainingStage, Training, TEXT("Default"));
    FY01WaveRow Wave; Wave.PreparationSeconds = 120; Wave.Spawns.Add(Spawn); Wave.Reward = Training.Reward;
    P->Tables[K::Wave]->EmptyTable();
    Add(K::Wave, Wave, TEXT("Default")); P->OrderedWaves = {Ref(K::Wave, TEXT("Default"))};
    P->bEvidenceMode = true; P->bEconomyEnabled = true; P->InitialGold = 0; P->BaseGoldPerSecond = 1; P->DropFlightSeconds = Variant == 1 ? 6. : 1.;
    return P;
}
UY01R36Profile* CreateContentSeed(UObject* Outer, int32 Variant)
{
    auto* P = CreateEconomySeed(Outer, Variant);
    const auto Ref = FY01MatchFixture::Ref;
    auto Add = [&](K Kind, const auto& Row, FName Name) { P->Tables[Kind]->AddRow(Name, Row); };
    P->ContentVersion = 4; P->bEvidenceMode = Variant != 1; P->DropFlightSeconds = 1;
    auto* Character = P->Tables[K::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("S4"));
    Character->R33TestGrowth.bEnabled = false;
    if (Variant != 1) { return P; }
    // 自然场仅展示正常随机档位及通用直接商品；廉价四来源入口留在证据场。
    for (const FName Name : {FName(TEXT("EvidenceEquipment")), FName(TEXT("EvidenceBook"))})
    {
        P->Tables[K::ShopOffer]->RemoveRow(Name);
        P->ContentShopOffers.RemoveAll([&](const FY01DataRef& R) { return R.RowName == Name; });
    }
    P->Tables[K::Monster]->EmptyTable(); P->Tables[K::Wave]->EmptyTable(); P->OrderedWaves.Reset();
    const FName Names[] = {TEXT("Melee"), TEXT("Ranged"), TEXT("Heavy")};
    const float Health[] = {60, 90, 180}; const double Chance[] = {.25, .40, .75};
    const FY01DataRef Items[] = {Ref(K::Equipment, TEXT("Equipment_D_1")), Ref(K::Skill, TEXT("Book_D_2")), Ref(K::Equipment, TEXT("Equipment_C_2"))};
    for (int32 I = 0; I < 3; ++I)
    {
        FY01RewardPackageRow Drop; FY01RewardEntry Item; Item.Item = Items[I]; Drop.Items.Add(Item);
        Add(K::RewardPackage, Drop, Names[I]);
        FY01MonsterRow Monster; Monster.Health = Health[I]; Monster.AttackDamage = I + 1; Monster.AttackInterval = 2;
        Monster.MoveSpeed = 100; Monster.Armor = 0; Monster.HealthRegen = 0;
        Monster.AttackType = I == 1 ? EY01AttackType::Ranged : EY01AttackType::Melee;
        Monster.Range = I == 1 ? 400 : 150; Monster.MeleeAngle = 90; Monster.ProjectileSpeed = 200;
        Monster.ItemDropChance = Chance[I]; Monster.DropReward = Ref(K::RewardPackage, Names[I]);
        Add(K::Monster, Monster, Names[I]);
    }
    FY01RewardPackageRow Clear; Clear.Gold = 300; Add(K::RewardPackage, Clear, TEXT("WaveClear"));
    const int32 First[] = {2, 3, 2}, Last[] = {1, 1, 2}; const float Delay[] = {2, 3, 2};
    for (int32 I = 0; I < 3; ++I)
    {
        FY01WaveRow Wave; Wave.PreparationSeconds = 15;
        FY01SpawnEntry A; A.Monster = Ref(K::Monster, Names[I == 2 ? 1 : 0]); A.Count = First[I];
        FY01SpawnEntry B; B.Monster = Ref(K::Monster, Names[I == 0 ? 1 : 2]); B.Count = Last[I]; B.DelaySeconds = Delay[I];
        Wave.Spawns = {A, B}; Wave.Reward = Ref(K::RewardPackage, I < 2 ? TEXT("WaveClear") : TEXT("Default"));
        const FName Name(*FString::Printf(TEXT("Wave%d"), I + 1)); Add(K::Wave, Wave, Name); P->OrderedWaves.Add(Ref(K::Wave, Name));
    }
    for (const auto& Pair : P->Tables[K::TrainingStage]->GetRowMap())
    { for (auto& Spawn : reinterpret_cast<FY01TrainingStageRow*>(Pair.Value)->Spawns) { Spawn.Monster = Ref(K::Monster, Names[0]); } }
    for (const auto& Pair : P->Tables[K::TowerFloor]->GetRowMap())
    { for (auto& Spawn : reinterpret_cast<FY01TowerFloorRow*>(Pair.Value)->Spawns) { Spawn.Monster = Ref(K::Monster, Names[0]); } }
    return P;
}
bool Exists(const FString& Name)
{
    return FPackageName::DoesPackageExist(Name)
        || IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(Name, FPackageName::GetAssetPackageExtension()))
        || IFileManager::Get().FileExists(*FPackageName::LongPackageNameToFilename(Name, FPackageName::GetMapPackageExtension()));
}
bool Occupied(const FString& Name) { return Exists(Name) || FindPackage(nullptr, *Name); }
FString TableName(const FString& Base, K Kind)
{ return Base + TEXT("/Data/DT_") + StaticEnum<K>()->GetNameStringByValue(static_cast<int64>(Kind)); }
UObject* Existing(const FString& Name, UClass* Type, Y01R36Editor::FGeneration& R)
{
    if (!Exists(Name)) { R.Error = TEXT("仅内存同名包，拒绝覆盖：") + Name; return nullptr; }
    auto* Package = LoadPackage(nullptr, *Name, LOAD_None);
    auto* Object = Package ? FindObject<UObject>(Package, *FPackageName::GetLongPackageAssetName(Name)) : nullptr;
    if (!Object || !Object->IsA(Type)) { R.Error = TEXT("持久资产加载失败或类型冲突：") + Name; return nullptr; }
    R.Preserved.AddUnique(Name); return Object;
}
bool ValidProfile(const UY01R36Profile* P, Y01R36Editor::FGeneration& R)
{
    if (P->Tables.Num() != 22) { R.Error = TEXT("隔离入口必须包含22张独立表：") + P->GetPathName(); return false; }
    const auto V = P->Validate();
    for (const auto& E : V.Errors) { R.Error += P->GetPathName() + TEXT(" | ") + E.Field + TEXT(": ") + E.Message + TEXT("\n"); }
    return V.IsValid();
}
bool SaveNew(UObject* Object, Y01R36Editor::FGeneration& R)
{
    auto* Package = Object->GetOutermost(); const auto Name = Package->GetName();
    if (Exists(Name)) { R.Error = TEXT("保存前发现同名磁盘包，停止且保留已有输出：") + Name; return false; }
    const auto File = FPackageName::LongPackageNameToFilename(Name, Object->IsA<UWorld>()
        ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true)) { R.Error = TEXT("不能创建隔离目录：") + File; return false; }
    Object->SetFlags(RF_Public | RF_Standalone); Object->ClearFlags(RF_Transient); Package->MarkPackageDirty();
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    if (!UPackage::SavePackage(Package, Object, *File, Args)) { R.Error = TEXT("保存失败，保留全部已完成包：") + File; return false; }
    FAssetRegistryModule::AssetCreated(Object); R.Created.Add(Name); return true;
}
}
bool Y01R36Editor::Generate(const FString& Root, FGeneration& Result, bool bValidateOnly, bool bCooldownFixture, bool bEconomyS2, bool bPresentationS3, bool bContentS4)
{
    Result = FGeneration();
    const FString Allowed = TEXT("/Game/Developers/Y01PlaygroundTests/R36"); FString DiskRoot;
    if ((Root != Allowed && !Root.StartsWith(Allowed + TEXT("/Runs/")))
        || !FPackageName::IsValidLongPackageName(Root) || Root.EndsWith(TEXT("/"))
        || !FPackageName::TryConvertLongPackageNameToFilename(Root, DiskRoot)
        || !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(DiskRoot), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir())))
    { Result.Error = TEXT("只允许R36隔离根或其Runs子目录：") + Root; return false; }
    if (bContentS4 && (bEconomyS2 || bPresentationS3 || bCooldownFixture || !Root.StartsWith(Allowed + TEXT("/Runs/S4_"))))
    { Result.Error = TEXT("S4仅允许独立S4_Run，不混用历史种子：") + Root; return false; }
    const auto MapName = Root + TEXT("/Maps/L_Y01R36");
    const bool bNewEconomy = bEconomyS2 && !bValidateOnly;
    const bool bNewPresentation = bPresentationS3 && !bValidateOnly;
    if (bPresentationS3 && (bEconomyS2 || bCooldownFixture || Root == Allowed
        || (bNewPresentation && (IFileManager::Get().DirectoryExists(*DiskRoot) || Occupied(MapName)))))
    { Result.Error = TEXT("S3只允许全新隔离Run，不混用S2/CD；既有Run仅冷读：") + Root; return false; }
    if (bEconomyS2 && (bCooldownFixture || Root == Allowed || (bNewEconomy && (IFileManager::Get().DirectoryExists(*DiskRoot) || Occupied(MapName)))))
    { Result.Error = TEXT("S2只允许全新隔离Run；已有资产仅ValidateOnly，不混用CD种子：") + Root; return false; }
    if (bCooldownFixture && (Root == Allowed || IFileManager::Get().DirectoryExists(*DiskRoot) || Occupied(MapName)))
    { Result.Error = TEXT("CD证据场只允许全新隔离Run，禁止混入或修改既有资产：") + Root; return false; }
    TStrongObjectPtr<UWorld> SavedWorld;
    if (Occupied(MapName))
    {
        SavedWorld.Reset(Cast<UWorld>(Existing(MapName, UWorld::StaticClass(), Result)));
        if (!SavedWorld) { return false; }
        if (SavedWorld->GetWorldPartition() || SavedWorld->GetWorldSettings()->DefaultGameMode != AY01R36GameMode::StaticClass())
        { Result.Error = TEXT("隔离地图Mode或分区冲突：") + MapName; return false; }
    }
    const TCHAR* Variants[] = {TEXT("Baseline"), TEXT("Content"), TEXT("Compare")};
    TArray<TArray<K>> MissingTables; TArray<bool> MissingProfiles;
    for (const auto* Variant : Variants)
    {
        const auto Base = Root + TEXT("/") + Variant; const auto ProfileName = Base + TEXT("/DA_R36Profile");
        const bool Saved = Occupied(ProfileName);
        if ((bCooldownFixture || bNewEconomy || bNewPresentation) && Saved) { Result.Error = TEXT("新证据Run已有Profile：") + ProfileName; return false; }
        TStrongObjectPtr<UY01R36Profile> P(Saved ? Cast<UY01R36Profile>(Existing(ProfileName, UY01R36Profile::StaticClass(), Result))
            : bContentS4 ? CreateContentSeed(GetTransientPackage(), Result.Profiles.Num())
            : bPresentationS3 ? CreatePresentationSeed(GetTransientPackage(), Result.Profiles.Num())
            : bEconomyS2 ? CreateEconomySeed(GetTransientPackage(), Result.Profiles.Num()) : CreateSeed(GetTransientPackage(), bCooldownFixture));
        if (!P) { return false; }
        if (bContentS4 && P->ContentVersion != 4) { Result.Error = TEXT("既有Profile不是S4版本，拒绝冒认或升级：") + ProfileName; return false; }
        if (bEconomyS2 && !P->bEconomyEnabled) { Result.Error = TEXT("S2冷读发现经济未启用，不修复既有Profile：") + ProfileName; return false; }
        TArray<K> Missing;
        for (uint8 I = 1; I <= static_cast<uint8>(K::QualityRollPool); ++I)
        {
            const auto Kind = static_cast<K>(I); const auto Name = TableName(Base, Kind);
            if ((bCooldownFixture || bNewEconomy || bNewPresentation) && Occupied(Name)) { Result.Error = TEXT("新证据Run已有表：") + Name; return false; }
            if (Saved)
            {
                const auto* Table = P->Tables.FindRef(Kind).Get();
                if (!Table || Table->GetOutermost()->GetName() != Name || Table->GetOuter() != Table->GetOutermost()
                    || Table->HasAnyFlags(RF_Transient) || !Exists(Name))
                { Result.Error = TEXT("既有Profile引用不属于本套持久表，禁止自动修复：") + ProfileName; return false; }
                if (Existing(Name, UDataTable::StaticClass(), Result) != Table) { Result.Error = TEXT("表对象身份冲突：") + Name; return false; }
            }
            else if (Occupied(Name))
            {
                auto* Table = Cast<UDataTable>(Existing(Name, UDataTable::StaticClass(), Result));
                if (!Table) { return false; }
                P->Tables[Kind] = Table;
            }
            else { Missing.Add(Kind); }
        }
        if (!ValidProfile(P.Get(), Result)) { return false; }
        Result.Profiles.Add(MoveTemp(P)); MissingTables.Add(MoveTemp(Missing)); MissingProfiles.Add(!Saved);
    }
    if (SavedWorld)
    {
        int32 Count = 0; bool Matches = false;
        for (TActorIterator<AY01R36Config> It(SavedWorld.Get(), AY01R36Config::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It)
        {
            ++Count; Matches = It->Baseline.Get() == Result.Profiles[0].Get() && It->Content.Get() == Result.Profiles[1].Get()
                && It->Compare.Get() == Result.Profiles[2].Get();
        }
        if (Count != 1 || !Matches) { Result.Error = TEXT("既有地图Config或三Profile引用冲突：") + MapName; return false; }
    }
    if (bValidateOnly)
    {
        if (!SavedWorld || MissingProfiles.Contains(true))
        { Result.Error = TEXT("只读校验发现缺失包，不创建：") + Root; return false; }
        return true;
    }
    // 三套配置和地图全部预检后才写入；已有表值与配置引用始终权威。
    for (int32 I = 0; I < 3; ++I)
    {
        const auto Base = Root + TEXT("/") + Variants[I]; auto* P = Result.Profiles[I].Get();
        for (const auto Kind : MissingTables[I])
        {
            const auto Name = TableName(Base, Kind);
            if (Occupied(Name)) { Result.Error = TEXT("生成中出现同名表，拒绝覆盖：") + Name; return false; }
            auto* Table = DuplicateObject<UDataTable>(P->Tables[Kind], CreatePackage(*Name), *FPackageName::GetLongPackageAssetName(Name));
            P->Tables[Kind] = Table;
            if (!SaveNew(Table, Result)) { return false; }
        }
        if (MissingProfiles[I])
        {
            const auto Name = Base + TEXT("/DA_R36Profile");
            if (Occupied(Name)) { Result.Error = TEXT("生成中出现同名配置，拒绝覆盖：") + Name; return false; }
            auto* Saved = DuplicateObject<UY01R36Profile>(P, CreatePackage(*Name), TEXT("DA_R36Profile"));
            Result.Profiles[I].Reset(Saved);
            if (!SaveNew(Saved, Result)) { return false; }
        }
    }
    if (SavedWorld) { return true; }
    if (Occupied(MapName)) { Result.Error = TEXT("生成中出现同名地图，拒绝覆盖：") + MapName; return false; }
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .CreateNavigation(false).CreateAISystem(false).RequiresHitProxies(false);
    auto* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("L_Y01R36"), CreatePackage(*MapName), false, ERHIFeatureLevel::Num, &IVS);
    if (!World) { Result.Error = TEXT("隔离地图创建失败：") + MapName; return false; }
    World->GetWorldSettings()->DefaultGameMode = AY01R36GameMode::StaticClass();
    auto* Config = World->SpawnActor<AY01R36Config>();
    Config->Baseline = Result.Profiles[0].Get(); Config->Content = Result.Profiles[1].Get(); Config->Compare = Result.Profiles[2].Get();
    auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    auto* Floor = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, -50), FRotator::ZeroRotator);
    Floor->GetStaticMeshComponent()->SetStaticMesh(Cube); Floor->SetActorScale3D(FVector(24, 24, 1));
    Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll")); Floor->Tags.Add(TEXT("R36Floor"));
    for (int32 I = 0; I < 4; ++I) { World->SpawnActor<APlayerStart>(FVector(-150, (I - 1.5) * 150, 100), FRotator::ZeroRotator); }
    auto* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 600), FRotator(-55, -30, 0));
    Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable); Sun->GetLightComponent()->SetIntensity(3.f);
    auto* Sky = World->SpawnActor<ASkyLight>();
    Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable); Sky->GetLightComponent()->SetIntensity(.8f);
    const bool Saved = SaveNew(World, Result); World->CleanupWorld(); return Saved;
}
#endif
int32 UY01R36AssetsCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
    FString Root;
    if (!FParse::Value(*Params, TEXT("Root="), Root) || Root.IsEmpty())
    { UE_LOG(LogTemp, Display, TEXT("R36_ASSETS_REJECT 需要显式Root；不会默认写入任何目录")); return 1; }
    Y01R36Editor::FGeneration R;
    const bool bEconomy = FParse::Param(*Params, TEXT("EconomyS2"));
    const bool bPresentation = FParse::Param(*Params, TEXT("PresentationS3"));
    const bool bContent = FParse::Param(*Params, TEXT("ContentS4"));
    const bool Ok = Y01R36Editor::Generate(Root, R, FParse::Param(*Params, TEXT("ValidateOnly")), FParse::Param(*Params, TEXT("CooldownFixture")), bEconomy, bPresentation, bContent);
    UE_LOG(LogTemp, Display, TEXT("R36_ASSETS Result=%d Created=%d Preserved=%d Root=%s Error=%s S1_ONLY=%d ECONOMY_S2=%d PRESENTATION_S3=%d CONTENT_S4=%d"), Ok, R.Created.Num(), R.Preserved.Num(), *Root, *R.Error, !bEconomy && !bPresentation && !bContent, bEconomy, bPresentation, bContent);
    return Ok ? 0 : 1;
#else
    return 1;
#endif
}
