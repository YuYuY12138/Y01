#include "Misc/AutomationTest.h"
#include "Playground/Y01R36Assets.h"
#include "Playground/Y01R36Runtime.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "Misc/PackageName.h"
#include "Misc/SecureHash.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "GameFramework/PlayerStart.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Components/StaticMeshComponent.h"

#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36SeedTest, "Y01.Framework.R36.Disk.Seed",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R36SeedTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Valid22"), TEXT("EmptyCharacter"), TEXT("DiscoverContent"), TEXT("Independent") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36SeedTest::RunTest(const FString& Case)
{
    using K = EY01TableKind;
    TStrongObjectPtr<UY01R36Profile> P(Y01R36Editor::CreateSeed(GetTransientPackage()));
    if (!TestNotNull(TEXT("真实独立Profile"), P.Get())) { return false; }
    if (Case == TEXT("Valid22"))
    {
        TestEqual(TEXT("独立22表"), P->Tables.Num(), 22);
        const auto Validation = P->Validate(); TestTrue(TEXT("真实加载GA与所有引用合法"), Validation.IsValid());
        for (const auto& E : Validation.Errors) { AddInfo(E.Field + TEXT(": ") + E.Message); }
    }
    if (Case == TEXT("EmptyCharacter"))
    {
        FY01DataValidation V; const auto* C = P->BuildCatalog().FindRow<FY01CharacterRow>(P->Character, V);
        if (!TestNotNull(TEXT("真实角色定义"), C)) { return false; }
        TestTrue(TEXT("初始包槽由实例系统管理"), C->InitialSkills.IsEmpty() && C->InitialEquipment.IsEmpty());
        TestEqual(TEXT("基础攻击30"), C->AttackDamage, 30.f); TestEqual(TEXT("生命300"), C->Health, 300.f);
        TestTrue(TEXT("保留训练与普攻"), P->bTrainingEnabled && P->bR34AutoAttackEnabled);
    }
    if (Case == TEXT("DiscoverContent"))
    {
        TestTrue(TEXT("公开数组发现装备与技能"), P->ContentEquipment.Num() >= 2 && P->ContentSkills.Num() >= 3);
        bool Active = false, Passive = false; FY01DataValidation V;
        for (const auto& Ref : P->ContentSkills)
        {
            if (const auto* S = P->BuildCatalog().FindRow<FY01SkillRow>(Ref, V))
            { Active |= S->Type == EY01SkillType::Active; Passive |= S->Type == EY01SkillType::Passive; TestFalse(TEXT("显示名不空"), S->DisplayName.IsEmpty()); }
        }
        TestTrue(TEXT("主动被动均有真实消费者样例"), Active && Passive);
    }
    if (Case == TEXT("Independent"))
    {
        TStrongObjectPtr<UY01R36Profile> Other(Y01R36Editor::CreateSeed(GetTransientPackage()));
        TestEqual(TEXT("两套完整表"), P->Tables.Num(), 22);
        for (const auto& Pair : P->Tables) { TestNotEqual(TEXT("不共享可变表"), Pair.Value.Get(), Other->Tables.FindRef(Pair.Key).Get()); }
    }
    return true;
}

namespace
{
bool SaveFixture(UObject* Asset)
{
    const auto File = FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
    if (IFileManager::Get().FileExists(*File)) { return false; }
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
    Asset->SetFlags(RF_Public | RF_Standalone); Asset->ClearFlags(RF_Transient);
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    return UPackage::SavePackage(Asset->GetOutermost(), Asset, *File, Args);
}
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36GenerationTest, "Y01.Framework.R36.Disk.Generation",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R36GenerationTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Create"), TEXT("Repeat"), TEXT("PartialEdited"), TEXT("InvalidProfile"),
        TEXT("ForeignReference"), TEXT("InvalidRoot"), TEXT("OccupiedMap") }) { Names.Add(N); Commands.Add(N); }
}
bool FY01R36GenerationTest::RunTest(const FString& Case)
{
    using K = EY01TableKind;
    FString Root = TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/Disk_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    AddInfo(TEXT("保留隔离夹具：") + Root);
    Y01R36Editor::FGeneration Result;
    if (Case == TEXT("InvalidRoot"))
    {
        TestFalse(TEXT("拒绝旧main目录"), Y01R36Editor::Generate(TEXT("/Game/Y01Playground"), Result));
        TestTrue(TEXT("拒绝不得写资产"), Result.Created.IsEmpty()); TestFalse(TEXT("拒绝有诊断"), Result.Error.IsEmpty()); return true;
    }
    if (Case == TEXT("OccupiedMap"))
    {
        const FString Name = Root + TEXT("/Maps/L_Y01R36");
        auto* Wrong = NewObject<UY01R36Profile>(CreatePackage(*Name), TEXT("L_Y01R36"));
        TestTrue(TEXT("新隔离冲突夹具保存"), SaveFixture(Wrong));
        TestFalse(TEXT("地图类型冲突预检"), Y01R36Editor::Generate(Root, Result));
        TestTrue(TEXT("冲突前不写70包"), Result.Created.IsEmpty()); return true;
    }
    if (Case == TEXT("InvalidProfile") || Case == TEXT("ForeignReference"))
    {
        const FString Name = Root + TEXT("/Baseline/DA_R36Profile");
        auto* P = Case == TEXT("InvalidProfile") ? NewObject<UY01R36Profile>(CreatePackage(*Name), TEXT("DA_R36Profile"))
            : DuplicateObject<UY01R36Profile>(Y01R36Editor::CreateSeed(GetTransientPackage()), CreatePackage(*Name), TEXT("DA_R36Profile"));
        TestTrue(TEXT("新隔离非法配置保存"), SaveFixture(P));
        TestFalse(TEXT("非法或非独立表引用必须停止"), Y01R36Editor::Generate(Root, Result));
        TestTrue(TEXT("全部预检后才保存"), Result.Created.IsEmpty()); return true;
    }
    if (Case == TEXT("PartialEdited"))
    {
        TStrongObjectPtr<UY01R36Profile> Seed(Y01R36Editor::CreateSeed(GetTransientPackage()));
        const FString Name = Root + TEXT("/Compare/Data/DT_SourceEffect");
        auto* T = DuplicateObject<UDataTable>(Seed->Tables[K::SourceEffect], CreatePackage(*Name), TEXT("DT_SourceEffect"));
        T->FindRow<FY01SourceEffectRow>(TEXT("Default"), TEXT("R36"))->Magnitude = 17;
        TestTrue(TEXT("保留用户值夹具保存"), SaveFixture(T));
    }
    if (!TestTrue(TEXT("生成三Profile及唯一地图"), Y01R36Editor::Generate(Root, Result))) { AddInfo(Result.Error); return false; }
    TestEqual(TEXT("三套配置"), Result.Profiles.Num(), 3);
    TestEqual(TEXT("只创建缺失包"), Result.Created.Num(), Case == TEXT("PartialEdited") ? 69 : 70);
    TSet<UDataTable*> Tables;
    for (const auto& P : Result.Profiles)
    {
        TestTrue(TEXT("保存后真实校验"), P->Validate().IsValid()); TestEqual(TEXT("每套22表"), P->Tables.Num(), 22);
        const auto Base = FPackageName::GetLongPackagePath(P->GetOutermost()->GetName()) + TEXT("/Data/");
        for (const auto& Pair : P->Tables)
        { TestTrue(TEXT("全部重绑定本套目录"), Pair.Value->GetOutermost()->GetName().StartsWith(Base)); Tables.Add(Pair.Value); }
    }
    TestEqual(TEXT("66张独立表"), Tables.Num(), 66);
    if (Case == TEXT("PartialEdited"))
    { TestEqual(TEXT("预存修改不恢复模板"), Result.Profiles[2]->Tables[K::SourceEffect]->FindRow<FY01SourceEffectRow>(TEXT("Default"), TEXT("R36"))->Magnitude, 17.); }
    const auto MapName = Root + TEXT("/Maps/L_Y01R36"); auto* Package = LoadPackage(nullptr, *MapName, LOAD_None);
    auto* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
    if (TestNotNull(TEXT("磁盘地图"), World))
    {
        TestEqual(TEXT("独立Mode"), World->GetWorldSettings()->DefaultGameMode.Get(), AY01R36GameMode::StaticClass());
        int32 Configs = 0;
        for (TActorIterator<AY01R36Config> It(World); It; ++It)
        { ++Configs; TestEqual(TEXT("默认Content"), It->Resolve(TEXT("")), Result.Profiles[1].Get()); }
        TestEqual(TEXT("唯一配置"), Configs, 1);
    }
    if (Case == TEXT("Repeat"))
    {
        TMap<FString, FString> Hashes;
        for (const auto& Name : Result.Created)
        {
            FString File; TestTrue(TEXT("真实磁盘包"), FPackageName::DoesPackageExist(Name, &File)); Hashes.Add(File, LexToString(FMD5Hash::HashFile(*File)));
        }
        Y01R36Editor::FGeneration Again; TestTrue(TEXT("重复只验证"), Y01R36Editor::Generate(Root, Again));
        TestEqual(TEXT("零覆盖"), Again.Created.Num(), 0); TestEqual(TEXT("保留全部70包"), Again.Preserved.Num(), 70);
        for (const auto& Pair : Hashes) { TestEqual(TEXT("逐包原字节不变"), LexToString(FMD5Hash::HashFile(*Pair.Key)), Pair.Value); }
    }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36EntryTest, "Y01.Framework.R36.Disk.Entry",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R36EntryTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Scene"), TEXT("ValidateMissing"), TEXT("CommandCreate"), TEXT("CommandValidate"),
        TEXT("CommandReject"), TEXT("InactiveLevel") }) { Names.Add(N); Commands.Add(N); }
}
bool FY01R36EntryTest::RunTest(const FString& Case)
{
    const auto Root = TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/Entry_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    AddInfo(TEXT("保留入口夹具：") + Root);
    Y01R36Editor::FGeneration R;
    auto* Command = NewObject<UY01R36AssetsCommandlet>();
    if (Case == TEXT("CommandReject"))
    {
        TestNotEqual(TEXT("不允许省略显式根"), Command->Main(TEXT("")), 0);
        TestNotEqual(TEXT("不能写旧main"), Command->Main(TEXT("-Root=/Game/Y01Playground")), 0); return true;
    }
    if (Case == TEXT("ValidateMissing"))
    {
        TestFalse(TEXT("只读校验不能生成缺失资产"), Y01R36Editor::Generate(Root, R, true));
        TestEqual(TEXT("没有写入"), R.Created.Num(), 0); TestFalse(TEXT("缺失有诊断"), R.Error.IsEmpty()); return true;
    }
    if (Case == TEXT("CommandCreate"))
    {
        if (!TestEqual(TEXT("显式Commandlet生成入口"), Command->Main(TEXT("-Root=") + Root), 0)) { return false; }
        TestTrue(TEXT("Commandlet输出可独立读取"), Y01R36Editor::Generate(Root, R, true));
        TestEqual(TEXT("全部70包只读保留"), R.Preserved.Num(), 70); return true;
    }
    if (!TestTrue(TEXT("准备完整隔离入口"), Y01R36Editor::Generate(Root, R))) { AddInfo(R.Error); return false; }
    if (Case == TEXT("CommandValidate"))
    { TestEqual(TEXT("Commandlet只读校验"), Command->Main(TEXT("-ValidateOnly -Root=") + Root), 0); return true; }
    const auto Name = Root + TEXT("/Maps/L_Y01R36"); auto* Package = LoadPackage(nullptr, *Name, LOAD_None);
    auto* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
    if (!TestNotNull(TEXT("持久地图"), World)) { return false; }
    if (Case == TEXT("InactiveLevel"))
    {
        const bool WasVisible = World->PersistentLevel->bIsVisible; World->PersistentLevel->bIsVisible = false;
        Y01R36Editor::FGeneration ReadOnly;
        TestTrue(TEXT("未激活的磁盘关卡也必须读取Config"), Y01R36Editor::Generate(Root, ReadOnly, true));
        TestEqual(TEXT("只读不写包"), ReadOnly.Created.Num(), 0);
        TestEqual(TEXT("冷关卡保留70包"), ReadOnly.Preserved.Num(), 70); AddInfo(ReadOnly.Error);
        World->PersistentLevel->bIsVisible = WasVisible; return true;
    }
    int32 Starts = 0, Floors = 0, Lights = 0;
    for (TActorIterator<APlayerStart> It(World); It; ++It) { ++Starts; }
    for (TActorIterator<ADirectionalLight> It(World); It; ++It) { ++Lights; }
    for (TActorIterator<AStaticMeshActor> It(World); It; ++It)
    { if (It->Tags.Contains(TEXT("R36Floor")) && It->GetStaticMeshComponent()->GetStaticMesh() && It->GetActorEnableCollision()) { ++Floors; } }
    TestTrue(TEXT("双客户端有独立出生点"), Starts >= 2); TestTrue(TEXT("真实承载地面"), Floors >= 1);
    TestTrue(TEXT("D3D12场景有光照"), Lights >= 1); return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36CooldownDiskTest, "Y01.Framework.R36.Disk.Cooldown",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R36CooldownDiskTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("Seed"), TEXT("Generate"), TEXT("RejectFixedRoot"), TEXT("RejectExisting") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36CooldownDiskTest::RunTest(const FString& Case)
{
    using K = EY01TableKind;
    auto Check = [&](const UY01R36Profile* P)
    {
        TestTrue(TEXT("CD夹具仍经22表真实GA验证"), P->Validate().IsValid());
        const auto* M = P->Tables[K::Monster]->FindRow<FY01MonsterRow>(TEXT("Default"), TEXT("R36CD"));
        TestEqual(TEXT("高血靶保留真实普攻但不提前清波"), M->Health, 1000000.f);
        TestEqual(TEXT("靶不杀玩家"), M->AttackDamage, 0.f); TestEqual(TEXT("静止靶"), M->MoveSpeed, 0.f);
        for (const auto& Ref : P->ContentSkills)
        {
            const auto* S = P->Tables[K::Skill]->FindRow<FY01SkillRow>(Ref.RowName, TEXT("R36CD"));
            if (S->Type == EY01SkillType::Active) { TestEqual(TEXT("显式长CD供多步真实截图"), S->CooldownSeconds, 60.f); }
        }
        TestTrue(TEXT("保留训练和非GA普攻"), P->bTrainingEnabled && P->bR34AutoAttackEnabled);
    };
    if (Case == TEXT("Seed"))
    {
        TStrongObjectPtr<UY01R36Profile> P(Y01R36Editor::CreateSeed(GetTransientPackage(), true)); Check(P.Get());
        TStrongObjectPtr<UY01R36Profile> Default(Y01R36Editor::CreateSeed(GetTransientPackage()));
        TestEqual(TEXT("普通种子仍8秒"), Default->Tables[K::Skill]->FindRow<FY01SkillRow>(TEXT("Default"), TEXT("R36CD"))->CooldownSeconds, 8.f);
        TestEqual(TEXT("普通怪未改"), Default->Tables[K::Monster]->FindRow<FY01MonsterRow>(TEXT("Default"), TEXT("R36CD"))->Health, 20.f);
        return true;
    }
    Y01R36Editor::FGeneration R;
    if (Case == TEXT("RejectFixedRoot"))
    {
        TestFalse(TEXT("CD证据场必须新隔离Run"), Y01R36Editor::Generate(TEXT("/Game/Developers/Y01PlaygroundTests/R36"), R, false, true));
        TestTrue(TEXT("拒绝不写包"), R.Created.IsEmpty()); TestFalse(TEXT("明确拒绝诊断"), R.Error.IsEmpty()); return true;
    }
    const FString Root = TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/CD_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    AddInfo(TEXT("CD_CONTROLLED_RUN=") + Root);
    if (Case == TEXT("RejectExisting"))
    {
        const auto Name = Root + TEXT("/Content/Data/DT_Monster");
        TStrongObjectPtr<UY01R36Profile> Seed(Y01R36Editor::CreateSeed(GetTransientPackage()));
        auto* T = DuplicateObject<UDataTable>(Seed->Tables[K::Monster], CreatePackage(*Name), TEXT("DT_Monster"));
        TestTrue(TEXT("预存夹具"), SaveFixture(T));
        TestFalse(TEXT("不能混入既有Run"), Y01R36Editor::Generate(Root, R, false, true));
        TestTrue(TEXT("预检失败零新写"), R.Created.IsEmpty()); return true;
    }
    auto* Command = NewObject<UY01R36AssetsCommandlet>();
    if (!TestEqual(TEXT("显式CD Commandlet入口"), Command->Main(TEXT("-CooldownFixture -Root=") + Root), 0)) { return false; }
    if (!TestTrue(TEXT("磁盘70包冷校验入口"), Y01R36Editor::Generate(Root, R, true))) { AddInfo(R.Error); return false; }
    TestEqual(TEXT("不覆盖70包"), R.Preserved.Num(), 70); TestEqual(TEXT("只读无创建"), R.Created.Num(), 0);
    for (const auto& P : R.Profiles) { Check(P.Get()); }
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R36EconomyDiskTest, "Y01.Framework.R36.Disk.EconomyS2",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FY01R36EconomyDiskTest::RunTest(const FString&)
{
    using K = EY01TableKind;
    const FString Root = TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/S2_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    AddInfo(TEXT("S2隔离夹具保留：") + Root);
    auto* Command = NewObject<UY01R36AssetsCommandlet>();
    if (!TestEqual(TEXT("显式S2生成"), Command->Main(TEXT("-EconomyS2 -Root=") + Root), 0)) { return false; }
    Y01R36Editor::FGeneration R;
    if (!TestTrue(TEXT("只读70包"), Y01R36Editor::Generate(Root, R, true))) { AddError(R.Error); return false; }
    TestEqual(TEXT("保留70包"), R.Preserved.Num(), 70);
    for (int32 Index = 0; Index < R.Profiles.Num(); ++Index)
    {
        const auto* P = R.Profiles[Index].Get(); const auto Catalog = P->BuildCatalog(); FY01DataValidation Q;
        TestTrue(TEXT("明确启用经济"), P->bEconomyEnabled); TestTrue(TEXT("真实完整校验"), P->Validate().IsValid());
        TestEqual(TEXT("初始金币来自明确Profile"), P->InitialGold, Index == 1 ? 0 : 200);
        TestEqual(TEXT("基础GPS1"), P->BaseGoldPerSecond, 1.);
        const auto* C = Catalog.FindRow<FY01CharacterRow>(P->Character, Q);
        if (!TestNotNull(TEXT("基础角色"), C)) { return false; }
        TestTrue(TEXT("空包槽初始化"), C->InitialSkills.IsEmpty() && C->InitialEquipment.IsEmpty());
        TestEqual(TEXT("ATK30"), C->AttackDamage, 30.f); TestEqual(TEXT("HP300"), C->Health, 300.f); TestEqual(TEXT("力量0"), C->Strength, 0.f);
        for (const auto& Ref : P->OrderedWaves)
        {
            const auto* W = Catalog.FindRow<FY01WaveRow>(Ref, Q);
            if (!W) { AddError(TEXT("缺波次")); continue; }
            TestEqual(TEXT("准备120秒"), W->PreparationSeconds, 120.f);
            const auto* Reward = Catalog.FindRow<FY01RewardPackageRow>(W->Reward, Q);
            TestTrue(TEXT("无金币经验掉落"), Reward && Reward->Gold == 0 && Reward->Experience == 0 && Reward->Items.IsEmpty());
        }
        for (const auto& Pair : P->Tables[K::Monster]->GetRowMap())
        {
            const auto& M = *reinterpret_cast<const FY01MonsterRow*>(Pair.Value);
            TestTrue(TEXT("静止高血零奖励怪"), M.Health >= 1000000 && M.AttackDamage == 0 && M.MoveSpeed == 0
                && M.Gold == 0 && M.Experience == 0 && M.ItemDropChance == 0);
        }
        auto CheckEffects = [&](const TArray<FY01DataRef>& Refs, double ExpectedAttack, double ExpectedIncome)
        {
            double Attack = 0, Income = 0;
            for (const auto& Ref : Refs)
            {
                const auto* E = Catalog.FindRow<FY01SourceEffectRow>(Ref, Q);
                if (E)
                {
                    TestTrue(TEXT("旧证据夹具只允许平面攻击及收入"), E->Kind == EY01SourceEffectKind::AttackFlat || E->Kind == EY01SourceEffectKind::GoldPerSecond);
                    if (E->Kind == EY01SourceEffectKind::AttackFlat) { Attack += E->Magnitude; }
                    else if (E->Kind == EY01SourceEffectKind::GoldPerSecond) { Income += E->Magnitude; }
                }
            }
            TestEqual(TEXT("来源攻击精确值"), Attack, ExpectedAttack); TestEqual(TEXT("来源收入精确值"), Income, ExpectedIncome);
        };
        for (const auto Name : {FName(TEXT("EvidenceEquipment")), FName(TEXT("EvidenceBook")), FName(TEXT("EvidenceBlessing"))})
        {
            FY01DataRef OfferRef; OfferRef.Type = K::ShopOffer; OfferRef.RowName = Name;
            const auto* Offer = Catalog.FindRow<FY01ShopOfferRow>(OfferRef, Q);
            if (!TestNotNull(TEXT("四来源固定商品"), Offer)) { continue; }
            TestEqual(TEXT("固定商品价格"), Offer->Price, Name == TEXT("EvidenceBlessing") ? 50 : 10);
            if (Offer->ShopKind == EY01ShopKind::DirectEffect)
            { CheckEffects(Offer->EffectIds, 5, 1); TestEqual(TEXT("直接商品保存玩家成长"), Offer->GrowthRuleIds.Num(), 1); }
            else
            {
                const auto* Pool = Catalog.FindRow<FY01LocalItemPoolRow>(Offer->Pool, Q);
                if (Pool && Pool->ItemType == K::Skill && Pool->Entries.Num() == 1)
                {
                    const auto* Book = Catalog.FindRow<FY01SkillRow>(Pool->Entries[0].Item, Q);
                    if (TestNotNull(TEXT("四来源被动书"), Book))
                    { TestEqual(TEXT("证据书为被动"), Book->Type, EY01SkillType::Passive); CheckEffects(Book->EffectIds, 5, 1); }
                }
            }
        }
        for (const auto& Pair : P->Tables[K::GrowthRule]->GetRowMap())
        {
            const auto& G = *reinterpret_cast<const FY01GrowthRuleRow*>(Pair.Value);
            TestTrue(TEXT("保存Player.Strength规则，不在本步触发"), G.Target == EY01GrowthTarget::Player && G.Attribute == EY01GrowthAttribute::Strength && G.Delta == 1);
        }
        bool Active = false, Passive = false;
        for (const auto& Ref : P->ContentSkills)
        { const auto* Skill = Catalog.FindRow<FY01SkillRow>(Ref, Q); if (Skill) { Active |= Skill->Type == EY01SkillType::Active; Passive |= Skill->Type == EY01SkillType::Passive; } }
        TestTrue(TEXT("主动与被动样例都公开"), Active && Passive);
        TestEqual(TEXT("两类四档八配方"), P->ContentRecipes.Num(), 8);
        TestTrue(TEXT("三个经济商品及两类四档商品"), P->ContentShopOffers.Num() >= 11);
        const auto* Choice = Catalog.FindRow<FY01TalentChoiceRow>(P->TalentChoice, Q);
        TestTrue(TEXT("固定三个不同候选"), Choice && Choice->Candidates.Num() == 3
            && Choice->Candidates[0].RowName != Choice->Candidates[1].RowName && Choice->Candidates[1].RowName != Choice->Candidates[2].RowName && Choice->Candidates[0].RowName != Choice->Candidates[2].RowName);
        if (Choice) { for (const auto& Ref : Choice->Candidates)
        { const auto* T = Catalog.FindRow<FY01TalentRow>(Ref, Q); if (T) { CheckEffects(T->EffectIds, 5, 1); TestEqual(TEXT("天赋保存成长"), T->GrowthRuleIds.Num(), 1); } } }
        int32 Tiers = 0;
        for (const auto& Ref : P->ContentShopOffers)
        {
            const auto* S = Catalog.FindRow<FY01ShopOfferRow>(Ref, Q); if (!S || S->ShopKind != EY01ShopKind::RandomTier) { continue; }
            ++Tiers; TestTrue(TEXT("四档价格"), S->Price == 100 || S->Price == 200 || S->Price == 400 || S->Price == 800);
            const auto* Pool = Catalog.FindRow<FY01QualityRollPoolRow>(S->TierPool, Q);
            if (!TestTrue(TEXT("两品质分支"), Pool && Pool->Entries.Num() == 2)) { continue; }
            TestEqual(TEXT("主品质比例"), Pool->Entries[0].Weight, S->Price == 800 ? 90. : 75.);
            TestEqual(TEXT("升级比例"), Pool->Entries[1].Weight, S->Price == 800 ? 10. : 25.);
            for (const auto& Entry : Pool->Entries)
            {
                const auto* Local = Catalog.FindRow<FY01LocalItemPoolRow>(Entry.Pool, Q);
                TestTrue(TEXT("每品质至少两个不同内容"), Local && Local->Entries.Num() >= 2 && Local->Entries[0].Item.RowName != Local->Entries[1].Item.RowName);
            }
        }
        TestEqual(TEXT("八个品质随机商品"), Tiers, 8);
        const auto* Equipment = P->Tables[K::Equipment]->FindRow<FY01EquipmentRow>(TEXT("EvidenceEquipment"), TEXT("S2"));
        if (TestNotNull(TEXT("四来源目标装备"), Equipment))
        {
            double Attack = 0, Income = 0;
            for (const auto& Ref : Equipment->EffectIds)
            {
                const auto* E = Catalog.FindRow<FY01SourceEffectRow>(Ref, Q);
                if (E)
                {
                    TestTrue(TEXT("旧证据夹具只允许平面攻击及收入"), E->Kind == EY01SourceEffectKind::AttackFlat || E->Kind == EY01SourceEffectKind::GoldPerSecond);
                    if (E->Kind == EY01SourceEffectKind::AttackFlat) { Attack += E->Magnitude; }
                    else if (E->Kind == EY01SourceEffectKind::GoldPerSecond) { Income += E->Magnitude; }
                }
            }
            TestEqual(TEXT("Baseline与Compare目标攻击"), Attack, Index == 2 ? 7. : 5.);
            TestEqual(TEXT("Baseline与Compare目标收入"), Income, Index == 2 ? 2. : 1.);
        }
    }
    for (const auto& Pair : R.Profiles[0]->Tables)
    {
        const auto* Base = Pair.Value.Get(); const auto* Other = R.Profiles[2]->Tables[Pair.Key].Get();
        TestTrue(TEXT("Compare所有可变表独立"), Base != Other && Base->GetOutermost() != Other->GetOutermost());
        TestEqual(TEXT("Compare行集合一致"), Base->GetRowMap().Num(), Other->GetRowMap().Num());
        for (const auto& Row : Base->GetRowMap())
        {
            const auto* Copy = Other->GetRowMap().Find(Row.Key);
            if (!TestNotNull(TEXT("Compare保留每行"), Copy)) { continue; }
            if (Pair.Key == K::SourceEffect && (Row.Key == TEXT("EvidenceAttack") || Row.Key == TEXT("EvidenceIncome"))) { continue; }
            TestTrue(TEXT("Compare除目标装备效果外逐行一致"), Base->GetRowStruct()->CompareScriptStruct(Row.Value, *Copy, 0));
        }
    }
    TMap<FString, FString> Hashes;
    for (const auto& Name : R.Preserved)
    { FString File; FPackageName::DoesPackageExist(Name, &File); Hashes.Add(File, LexToString(FMD5Hash::HashFile(*File))); }
    TestNotEqual(TEXT("S2不得混入既有Run"), Command->Main(TEXT("-EconomyS2 -Root=") + Root), 0);
    TestEqual(TEXT("S2显式冷读允许既有Run"), Command->Main(TEXT("-EconomyS2 -ValidateOnly -Root=") + Root), 0);
    for (const auto& Pair : Hashes) { TestEqual(TEXT("既有包逐字节保持"), LexToString(FMD5Hash::HashFile(*Pair.Key)), Pair.Value); }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36PresentationDiskTest, "Y01.Framework.R36.Disk.PresentationS3",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R36PresentationDiskTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Data"), TEXT("Preserve"), TEXT("RejectMixed"), TEXT("RejectExisting")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36PresentationDiskTest::RunTest(const FString& Case)
{
    using K = EY01TableKind;
    const FString Root = TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/S3_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    AddInfo(TEXT("S3有界表现夹具保留：") + Root);
    auto* Command = NewObject<UY01R36AssetsCommandlet>();
    if (Case == TEXT("RejectMixed"))
    {
        TestNotEqual(TEXT("S3不能混用CD种子"), Command->Main(TEXT("-PresentationS3 -CooldownFixture -Root=") + Root), 0);
        TestNotEqual(TEXT("S3不能混用S2种子"), Command->Main(TEXT("-PresentationS3 -EconomyS2 -Root=") + Root + TEXT("_S2")), 0);
        return true;
    }
    if (!TestEqual(TEXT("显式S3新Run入口"), Command->Main(TEXT("-PresentationS3 -Root=") + Root), 0)) { return false; }
    Y01R36Editor::FGeneration R;
    if (!TestTrue(TEXT("70包真实冷读"), Y01R36Editor::Generate(Root, R, true))) { AddError(R.Error); return false; }
    TestEqual(TEXT("三Profile独立包保留"), R.Preserved.Num(), 70);
    if (Case == TEXT("Data"))
    {
        for (int32 I = 0; I < R.Profiles.Num(); ++I)
        {
            const auto* P = R.Profiles[I].Get();
            TestTrue(TEXT("S3启用真实经济交付"), P->bEconomyEnabled);
            TestTrue(TEXT("22表完整校验"), P->Validate().IsValid());
            TestEqual(TEXT("不预注入金币"), P->InitialGold, 0);
            TestEqual(TEXT("正常收入用于购买"), P->BaseGoldPerSecond, 1.);
            TestEqual(TEXT("controlled延长ETA，其余默认1秒"), P->DropFlightSeconds, I == 1 ? 6. : 1.);
            TestEqual(TEXT("只公开一个S3成长商品"), P->ContentShopOffers.Num(), 1);
            TestEqual(TEXT("不是S4完整三波"), P->OrderedWaves.Num(), 1);
            const auto* C = P->Tables[K::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("S3"));
            TestTrue(TEXT("裸攻30力量0空包空槽"), C && C->AttackDamage == 30 && C->Strength == 0 && C->InitialEquipment.IsEmpty() && C->InitialSkills.IsEmpty());
            const auto* M = P->Tables[K::Monster]->FindRow<FY01MonsterRow>(TEXT("Default"), TEXT("S3"));
            TestTrue(TEXT("真实可击杀靶，不直接置零"), M && M->Health == 20 && M->AttackDamage == 0 && M->MoveSpeed == 0 && M->Skills.IsEmpty());
            if (M)
            {
                TestEqual(TEXT("natural随机，controlled落货必出，成长专场不落"), M->ItemDropChance, I == 0 ? .35 : I == 1 ? 1. : 0.);
                FY01DataValidation Q; const auto* Drop = P->BuildCatalog().FindRow<FY01RewardPackageRow>(M->DropReward, Q);
                TestTrue(TEXT("真实掉落纯物品包且每次一件"), Drop && Drop->Domain == EY01RewardDomain::MatchPersonal && Drop->Gold == 0 && Drop->Experience == 0
                    && Drop->Items.Num() == 1 && Drop->Items[0].Quantity == 1 && Drop->Items[0].Item.Type == K::Equipment && Drop->Items[0].Item.RowName == TEXT("S3Blade"));
            }
            FY01DataValidation Q;
            const auto* Offer = P->ContentShopOffers.Num() == 1 ? P->BuildCatalog().FindRow<FY01ShopOfferRow>(P->ContentShopOffers[0], Q) : nullptr;
            TestTrue(TEXT("正常10金购买一件成长装"), Offer && Offer->Price == 10 && Offer->Quantity == 1 && Offer->ShopKind == EY01ShopKind::RandomPool);
            const auto* Pool = Offer ? P->BuildCatalog().FindRow<FY01LocalItemPoolRow>(Offer->Pool, Q) : nullptr;
            TestTrue(TEXT("商品池实际指向成长装"), Pool && Pool->Entries.Num() == 1 && Pool->Entries[0].Item.Type == K::Equipment && Pool->Entries[0].Item.RowName == TEXT("S3Blade"));
            const auto* E = P->Tables[K::Equipment]->FindRow<FY01EquipmentRow>(TEXT("S3Blade"), TEXT("S3"));
            if (TestNotNull(TEXT("明确S3成长装备，不冒充S2"), E))
            {
                TestTrue(TEXT("显示名明确测试场"), E->DisplayName.ToString().Contains(TEXT("S3")));
                TestEqual(TEXT("双成长来源"), E->GrowthRuleIds.Num(), 2);
                FY01DataValidation V; const auto Catalog = P->BuildCatalog(); double Attack = 0; bool Item = false, Player = false;
                for (const auto& Ref : E->EffectIds) { const auto* Effect = Catalog.FindRow<FY01SourceEffectRow>(Ref, V); if (Effect && Effect->Kind == EY01SourceEffectKind::AttackFlat) { Attack += Effect->Magnitude; } }
                for (const auto& Ref : E->GrowthRuleIds)
                {
                    const auto* G = Catalog.FindRow<FY01GrowthRuleRow>(Ref, V);
                    if (!G) { continue; }
                    TestEqual(TEXT("每杀真实累计1"), G->Delta, 1.);
                    Item |= G->Target == EY01GrowthTarget::EquipmentInstance && G->Attribute == EY01GrowthAttribute::AttackDamage;
                    Player |= G->Target == EY01GrowthTarget::Player && G->Attribute == EY01GrowthAttribute::Strength;
                }
                TestEqual(TEXT("静态攻击10"), Attack, 10.); TestTrue(TEXT("装备攻击与玩家力量双成长"), Item && Player);
            }
            const auto* T = P->Tables[K::TrainingStage]->FindRow<FY01TrainingStageRow>(TEXT("Default"), TEXT("S3"));
            TestTrue(TEXT("单靶正常采样周期，不是10k长场"), T && T->MaxAlive == 1 && T->SpawnInterval == 4 && T->Spawns.Num() == 1 && T->Spawns[0].Count == 1);
            const auto* Reward = T ? P->BuildCatalog().FindRow<FY01RewardPackageRow>(T->Reward, Q) : nullptr;
            TestTrue(TEXT("训练奖励与怪物物品掉落分离"), Reward && Reward->Domain == EY01RewardDomain::MatchPersonal && Reward->Items.IsEmpty() && Reward->Gold == 0 && Reward->Experience == 0);
        }
    }
    TMap<FString, FString> Hashes;
    for (const auto& Name : R.Preserved)
    { FString File; FPackageName::DoesPackageExist(Name, &File); Hashes.Add(File, LexToString(FMD5Hash::HashFile(*File))); }
    if (Case == TEXT("RejectExisting")) { TestNotEqual(TEXT("S3禁止覆盖既有Run"), Command->Main(TEXT("-PresentationS3 -Root=") + Root), 0); }
    if (Case == TEXT("Preserve")) { TestEqual(TEXT("S3只读冷验证"), Command->Main(TEXT("-PresentationS3 -ValidateOnly -Root=") + Root), 0); }
    for (const auto& Pair : Hashes) { TestEqual(TEXT("原包哈希不变"), LexToString(FMD5Hash::HashFile(*Pair.Key)), Pair.Value); }
    return true;
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36S4DiskTest, "Y01.Framework.R36.S4.Disk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R36S4DiskTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Data"), TEXT("Independent"), TEXT("Compare"), TEXT("Repeat"), TEXT("PartialEdited"),
        TEXT("RejectMixed"), TEXT("RejectFixed"), TEXT("InvalidPartial"), TEXT("RejectOldRun"), TEXT("LegacyCold")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01R36S4DiskTest::RunTest(const FString& Case)
{
    using K = EY01TableKind;
    auto* Command = NewObject<UY01R36AssetsCommandlet>();
    const FString Root = TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/S4_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    AddInfo(TEXT("S4_RUN=") + Root);
    auto Generate = [&]() { return Command->Main(TEXT("-ContentS4 -Root=") + Root); };
    if (Case == TEXT("LegacyCold"))
    {
        for (const TCHAR* Run : {TEXT("S2_Integration_20261001_01"), TEXT("S2_Integration_20261001_04"), TEXT("S3_Visible_20261001_01")})
        {
            const FString Old = FString(TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/")) + Run;
            Y01R36Editor::FGeneration R;
            if (!TestTrue(TEXT("历史主Run只读合法"), Y01R36Editor::Generate(Old, R, true))) { AddError(R.Error); continue; }
            TestTrue(TEXT("历史读取零写入"), R.Created.IsEmpty());
            for (const auto& P : R.Profiles)
            {
                TestTrue(TEXT("历史经济场以版本兼容保留证据模式"), P->bEvidenceMode);
                const bool Economy = P->bEconomyEnabled; P->bEconomyEnabled = false;
                TestTrue(TEXT("加载后证据模式不依赖经济开关"), P->bEvidenceMode); P->bEconomyEnabled = Economy;
            }
        }
        return true;
    }
    if (Case == TEXT("RejectMixed"))
    {
        TestNotEqual(TEXT("S4不得混用S2"), Command->Main(TEXT("-ContentS4 -EconomyS2 -Root=") + Root), 0);
        TestFalse(TEXT("错误不写地图"), FPackageName::DoesPackageExist(Root + TEXT("/Maps/L_Y01R36"))); return true;
    }
    if (Case == TEXT("RejectFixed"))
    {
        TestNotEqual(TEXT("S4不得写固定旧入口"), Command->Main(TEXT("-ContentS4 -Root=/Game/Developers/Y01PlaygroundTests/R36")), 0); return true;
    }
    if (Case == TEXT("RejectOldRun"))
    {
        Y01R36Editor::FGeneration Old; TestTrue(TEXT("新隔离S1夹具"), Y01R36Editor::Generate(Root, Old));
        TestNotEqual(TEXT("S4不能冒认旧Run"), Generate(), 0); return true;
    }
    if (Case == TEXT("PartialEdited") || Case == TEXT("InvalidPartial"))
    {
        const FString Source = TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/S2_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
        Y01R36Editor::FGeneration S2;
        if (!TestTrue(TEXT("独立S2夹具提供局部表"), Y01R36Editor::Generate(Source, S2, false, false, true))) { AddError(S2.Error); return false; }
        const FString Name = Root + TEXT("/Compare/Data/DT_SourceEffect");
        auto* Table = DuplicateObject<UDataTable>(S2.Profiles[2]->Tables[K::SourceEffect], CreatePackage(*Name), TEXT("DT_SourceEffect"));
        Table->FindRow<FY01SourceEffectRow>(TEXT("EvidenceAttack"), TEXT("S4"))->Magnitude = Case == TEXT("InvalidPartial") ? -1 : 17;
        if (!TestTrue(TEXT("先存用户修改局部表"), SaveFixture(Table))) { return false; }
        if (Case == TEXT("InvalidPartial"))
        {
            TestNotEqual(TEXT("全套预检拒绝非法用户表"), Generate(), 0);
            TestFalse(TEXT("失败前不写Baseline"), FPackageName::DoesPackageExist(Root + TEXT("/Baseline/DA_R36Profile")));
            TestFalse(TEXT("失败前不写地图"), FPackageName::DoesPackageExist(Root + TEXT("/Maps/L_Y01R36"))); return true;
        }
    }
    if (!TestEqual(TEXT("S4事务生成成功"), Generate(), 0)) { return false; }
    Y01R36Editor::FGeneration R;
    if (!TestTrue(TEXT("磁盘三配置只读"), Y01R36Editor::Generate(Root, R, true))) { AddError(R.Error); return false; }
    TestEqual(TEXT("完整70包"), R.Preserved.Num(), 70);
    const auto* Base = R.Profiles[0].Get(); const auto* P = R.Profiles[1].Get(); const auto* Compare = R.Profiles[2].Get();
    TestTrue(TEXT("Baseline/Compare显式证据，Content自然"), Base->bEvidenceMode && Compare->bEvidenceMode && !P->bEvidenceMode);
    TSet<const UDataTable*> Unique;
    for (const auto& Profile : R.Profiles)
    {
        TestEqual(TEXT("每套22类表"), Profile->Tables.Num(), 22);
        TestTrue(TEXT("真实扩展校验"), Profile->Validate().IsValid());
        const FString Own = FPackageName::GetLongPackagePath(Profile->GetOutermost()->GetName()) + TEXT("/Data/");
        for (uint8 I = 1; I <= 22; ++I)
        {
            const auto* Table = Profile->Tables.FindRef(static_cast<K>(I)).Get();
            if (TestNotNull(TEXT("每类引用存在"), Table)) { Unique.Add(Table); TestTrue(TEXT("本套根引用"), Table->GetOutermost()->GetName().StartsWith(Own)); }
        }
    }
    TestEqual(TEXT("66个互不共享可变表"), Unique.Num(), 66);
    auto* Package = LoadPackage(nullptr, *(Root + TEXT("/Maps/L_Y01R36")), LOAD_None);
    auto* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
    if (!TestNotNull(TEXT("同一持久地图"), World)) { return false; }
    int32 ConfigCount = 0;
    for (TActorIterator<AY01R36Config> It(World, AY01R36Config::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It)
    {
        ++ConfigCount; TestTrue(TEXT("默认Content"), It->Resolve(TEXT("")) == P);
        TestTrue(TEXT("content选择"), It->Resolve(TEXT("content")) == P);
        TestTrue(TEXT("evidence选择"), It->Resolve(TEXT("evidence")) == Base);
        TestTrue(TEXT("compare选择"), It->Resolve(TEXT("compare")) == Compare);
    }
    TestEqual(TEXT("唯一Config"), ConfigCount, 1);
    if (Case == TEXT("Data"))
    {
        FY01DataValidation Q; const auto Catalog = P->BuildCatalog();
        const auto* C = Catalog.FindRow<FY01CharacterRow>(P->Character, Q);
        if (!TestNotNull(TEXT("自然角色"), C)) { return false; }
        TestTrue(TEXT("自然空槽且关闭旧R33"), C->InitialEquipment.IsEmpty() && C->InitialSkills.IsEmpty() && !C->R33TestGrowth.bEnabled);
        TestTrue(TEXT("ATK30/HP300/STR0"), C->AttackDamage == 30 && C->Health == 300 && C->Strength == 0);
        TestEqual(TEXT("自然Gold0"), P->InitialGold, 0); TestEqual(TEXT("基础GPS1"), P->BaseGoldPerSecond, 1.);
        TestEqual(TEXT("ETA1s"), P->DropFlightSeconds, 1.);
        TestEqual(TEXT("自然三波"), P->OrderedWaves.Num(), 3);
        const FName Monsters[] = {TEXT("Melee"), TEXT("Ranged"), TEXT("Heavy")};
        const float HP[] = {60,90,180}, Attack[] = {1,2,3}; const double Chance[] = {.25,.40,.75};
        TSet<FName> Drops;
        for (int32 I = 0; I < 3; ++I)
        {
            const auto* M = P->Tables[K::Monster]->FindRow<FY01MonsterRow>(Monsters[I], TEXT("S4"), false);
            if (!TestNotNull(TEXT("三类自然怪"), M)) { continue; }
            TestEqual(TEXT("怪HP"), M->Health, HP[I]); TestEqual(TEXT("怪ATK"), M->AttackDamage, Attack[I]);
            TestTrue(TEXT("怪间隔2/速度100/无护甲回复"), M->AttackInterval == 2 && M->MoveSpeed == 100 && M->Armor == 0 && M->HealthRegen == 0);
            TestEqual(TEXT("掉率"), M->ItemDropChance, Chance[I]); TestTrue(TEXT("怪物Gold/XP不冒充均分"), M->Gold == 0 && M->Experience == 0);
            TestEqual(TEXT("攻击类型"), M->AttackType, I == 1 ? EY01AttackType::Ranged : EY01AttackType::Melee);
            TestEqual(TEXT("射程"), M->Range, I == 1 ? 400.f : 150.f);
            if (I == 1) { TestEqual(TEXT("球速200"), M->ProjectileSpeed, 200.f); } else { TestEqual(TEXT("近战角90"), M->MeleeAngle, 90.f); }
            const auto* Drop = Catalog.FindRow<FY01RewardPackageRow>(M->DropReward, Q); Drops.Add(M->DropReward.RowName);
            TestTrue(TEXT("独立纯物品个人掉落"), Drop && Drop->Domain == EY01RewardDomain::MatchPersonal && Drop->Gold == 0 && Drop->Experience == 0 && !Drop->Items.IsEmpty());
            if (Drop) { for (const auto& Item : Drop->Items) { TestTrue(TEXT("掉落仅装备或书"), Item.Item.Type == K::Equipment || Item.Item.Type == K::Skill); } }
        }
        TestEqual(TEXT("三独立掉落包"), Drops.Num(), 3);
        const int32 First[] = {2,3,2}, Last[] = {1,1,2}; const float Delay[] = {2,3,2};
        for (int32 I = 0; I < P->OrderedWaves.Num(); ++I)
        {
            const auto* W = Catalog.FindRow<FY01WaveRow>(P->OrderedWaves[I], Q);
            if (!W || I >= 3) { continue; }
            TestEqual(TEXT("每波准备15s"), W->PreparationSeconds, 15.f);
            if (!TestEqual(TEXT("两批刷怪"), W->Spawns.Num(), 2)) { continue; }
            TestEqual(TEXT("首批数量"), W->Spawns[0].Count, First[I]); TestEqual(TEXT("首批立即"), W->Spawns[0].DelaySeconds, 0.f);
            TestEqual(TEXT("末批数量"), W->Spawns[1].Count, Last[I]); TestEqual(TEXT("末批延迟"), W->Spawns[1].DelaySeconds, Delay[I]);
            TestEqual(TEXT("首批类型"), W->Spawns[0].Monster.RowName, Monsters[I == 2 ? 1 : 0]);
            TestEqual(TEXT("末批类型"), W->Spawns[1].Monster.RowName, Monsters[I == 0 ? 1 : 2]);
            const auto* Reward = Catalog.FindRow<FY01RewardPackageRow>(W->Reward, Q);
            TestTrue(TEXT("旧清波个人链奖励300/300/0"), Reward && Reward->Gold == (I < 2 ? 300 : 0) && Reward->Experience == 0 && Reward->Items.IsEmpty() && Reward->Domain == EY01RewardDomain::MatchPersonal);
        }
        TestEqual(TEXT("两类四配方永久免费接口"), P->ContentRecipes.Num(), 8);
        int32 Tiers = 0; TSet<FName> Items[5]; bool Active = false, Passive = false;
        for (const auto& Ref : P->ContentEquipment) { const auto* E = Catalog.FindRow<FY01EquipmentRow>(Ref, Q); if (E) { Items[static_cast<uint8>(E->Quality)].Add(Ref.RowName); } }
        for (const auto& IDs : Items) { TestTrue(TEXT("每品质两不同装备ID"), IDs.Num() >= 2); }
        for (const auto& Ref : P->ContentSkills) { const auto* S = Catalog.FindRow<FY01SkillRow>(Ref, Q); if (S) { Active |= S->Type == EY01SkillType::Active; Passive |= S->Type == EY01SkillType::Passive; } }
        TestTrue(TEXT("主动被动均公开"), Active && Passive);
        for (const auto& Ref : P->ContentShopOffers)
        {
            const auto* O = Catalog.FindRow<FY01ShopOfferRow>(Ref, Q); if (!O || O->ShopKind != EY01ShopKind::RandomTier) { continue; }
            ++Tiers; const auto* Tier = Catalog.FindRow<FY01QualityRollPoolRow>(O->TierPool, Q);
            if (!TestTrue(TEXT("两分支品质池"), Tier && Tier->Entries.Num() == 2)) { continue; }
            const int32 Grade = static_cast<uint8>(Tier->Entries[0].Quality);
            TestEqual(TEXT("四档价格"), O->Price, 100 * (1 << Grade));
            TestEqual(TEXT("主品质权重"), Tier->Entries[0].Weight, Grade == 3 ? 90. : 75.);
            TestEqual(TEXT("升级权重"), Tier->Entries[1].Weight, Grade == 3 ? 10. : 25.);
        }
        TestEqual(TEXT("八档随机商品"), Tiers, 8);
    }
    if (Case == TEXT("Compare") || Case == TEXT("Data"))
    {
        TestEqual(TEXT("证据初始200"), Base->InitialGold, 200);
        for (const auto& Ref : Base->OrderedWaves)
        {
            FY01DataValidation Q; const auto* W = Base->BuildCatalog().FindRow<FY01WaveRow>(Ref, Q);
            TestEqual(TEXT("证据准备120s"), W->PreparationSeconds, 120.f);
            const auto* Reward = Base->BuildCatalog().FindRow<FY01RewardPackageRow>(W->Reward, Q);
            TestTrue(TEXT("证据波奖零"), Reward && Reward->Gold == 0 && Reward->Experience == 0 && Reward->Items.IsEmpty());
        }
        for (const auto& Row : Base->Tables[K::Monster]->GetRowMap())
        { const auto* M = reinterpret_cast<const FY01MonsterRow*>(Row.Value); TestTrue(TEXT("证据静止高血怪"), M->Health >= 1000000 && M->MoveSpeed == 0 && M->AttackDamage == 0); }
        for (const auto& Pair : Base->Tables)
        {
            const auto* Copy = Compare->Tables[Pair.Key].Get(); TestEqual(TEXT("Compare行数一致"), Pair.Value->GetRowMap().Num(), Copy->GetRowMap().Num());
            for (const auto& Row : Pair.Value->GetRowMap())
            {
                const auto* Other = Copy->GetRowMap().Find(Row.Key); if (!TestNotNull(TEXT("Compare每行存在"), Other)) { continue; }
                if (Pair.Key == K::SourceEffect && (Row.Key == TEXT("EvidenceAttack") || Row.Key == TEXT("EvidenceIncome"))) { continue; }
                TestTrue(TEXT("除目标装备效果两值外逐行相同"), Copy->GetRowStruct()->CompareScriptStruct(Row.Value, *Other, 0));
            }
        }
        const auto* Attack = Compare->Tables[K::SourceEffect]->FindRow<FY01SourceEffectRow>(TEXT("EvidenceAttack"), TEXT("S4"), false);
        const auto* Income = Compare->Tables[K::SourceEffect]->FindRow<FY01SourceEffectRow>(TEXT("EvidenceIncome"), TEXT("S4"), false);
        if (TestNotNull(TEXT("Compare目标攻击来源"), Attack)) { TestEqual(TEXT("Compare目标攻击7"), Attack->Magnitude, 7.); }
        if (TestNotNull(TEXT("Compare目标收入来源"), Income)) { TestEqual(TEXT("Compare目标GPS2"), Income->Magnitude, 2.); }
    }
    if (Case == TEXT("PartialEdited"))
    { TestEqual(TEXT("Compare用户改值17保留"), Compare->Tables[K::SourceEffect]->FindRow<FY01SourceEffectRow>(TEXT("EvidenceAttack"), TEXT("S4"))->Magnitude, 17.); }
    if (Case == TEXT("Repeat") || Case == TEXT("PartialEdited"))
    {
        TMap<FString,FString> Hashes;
        for (const auto& Name : R.Preserved) { FString File; FPackageName::DoesPackageExist(Name, &File); Hashes.Add(File, LexToString(FMD5Hash::HashFile(*File))); }
        TestEqual(TEXT("S4重复生成只保留"), Generate(), 0);
        for (const auto& Pair : Hashes) { TestEqual(TEXT("重复逐包零覆盖"), LexToString(FMD5Hash::HashFile(*Pair.Key)), Pair.Value); }
    }
    return true;
}
#endif
