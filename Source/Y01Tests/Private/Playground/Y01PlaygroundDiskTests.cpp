#include "Misc/AutomationTest.h"
#include "Playground/Y01PlaygroundGenerator.h"
#include "Playground/Y01PlaygroundProfile.h"
#include "Playground/Y01PlaygroundSeed.h"
#include "Playground/Y01PlaygroundMapBuilder.h"
#include "Playground/Y01CreatePlaygroundCommandlet.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
FString PackageFile(const FString& Package)
{
    return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

bool SaveTestAsset(UObject* Asset)
{
    // 自动化编辑只允许独立测试挂载目录；不触碰主示例资产。
    UPackage* Package = Asset->GetOutermost();
    if (!Package->GetName().StartsWith(TEXT("/Game/Developers/Y01PlaygroundTests/"))) { return false; }
    const FString Filename = PackageFile(Package->GetName());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
    Asset->SetFlags(RF_Public | RF_Standalone);
    Asset->ClearFlags(RF_Transient);
    Package->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    return UPackage::SavePackage(Package, Asset, *Filename, Args);
}

TMap<FString, TArray<uint8>> Snapshot(const FString& Root)
{
    TMap<FString, TArray<uint8>> Files;
    TArray<FString> Names;
    IFileManager::Get().FindFilesRecursive(Names, *FPackageName::LongPackageNameToFilename(Root), TEXT("*.uasset"), true, false);
    for (const FString& Name : Names)
    {
        TArray<uint8> Bytes;
        if (FFileHelper::LoadFileToArray(Bytes, *Name)) { Files.Add(Name, MoveTemp(Bytes)); }
    }
    return Files;
}

bool SameFiles(const TMap<FString, TArray<uint8>>& Before, const TMap<FString, TArray<uint8>>& After)
{
    if (Before.Num() != After.Num()) { return false; }
    for (const auto& Pair : Before)
    {
        const auto* Bytes = After.Find(Pair.Key);
        if (!Bytes || *Bytes != Pair.Value) { return false; }
    }
    return true;
}

bool ChildCommandlet(const FString& Root, bool bValidateOnly, FString& Log, int32& ExitCode)
{
    const FString ChildLogFilename = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() /
        (TEXT("Playground-Child-") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".log")));
    const FString Args = FString::Printf(TEXT("\"%s\" -run=Y01CreatePlayground Root=%s %s -unattended -nop4 -nullrhi -nosound -abslog=\"%s\""),
        *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), *Root, bValidateOnly ? TEXT("-ValidateOnly") : TEXT(""), *ChildLogFilename);
    const FString Executable = FPlatformProcess::GenerateApplicationPath(TEXT("UnrealEditor-Cmd"), EBuildConfiguration::Development);
    const bool bLaunched = FPlatformProcess::ExecProcess(*Executable, *Args, &ExitCode, nullptr, nullptr);
    FFileHelper::LoadFileToString(Log, *ChildLogFilename);
    return bLaunched;
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01PlaygroundDiskTest, "Y01.Playground.Data.Disk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01PlaygroundDiskTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("SaveAndFreshProcess"), TEXT("PreserveEdits"), TEXT("FillMissingOnly"),
        TEXT("RejectInvalidEdit"), TEXT("RejectInvalidPartial"), TEXT("RejectWrongAssetClass"), TEXT("RejectInvalidRoot"), TEXT("ValidateOnlyNeverSeeds"), TEXT("CommandletFreshRoot"),
        TEXT("SeedMonsterBinding"), TEXT("RepairOptIn"), TEXT("RepairPreservesEdits") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01PlaygroundDiskTest::RunTest(const FString& Case)
{
    using namespace Y01PlaygroundEditor;
    const FString Root = TEXT("/Game/Developers/Y01PlaygroundTests/") + Case + TEXT("_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    AddInfo(TEXT("保留隔离测试资产：") + Root);
    FY01PlaygroundGenerationResult Result;
    if (Case == TEXT("CommandletFreshRoot"))
    {
        FString Log;
        int32 ExitCode = -1;
        TestTrue(TEXT("新进程运行生成器"), ChildCommandlet(Root, false, Log, ExitCode));
        TestEqual(TEXT("Commandlet生成退出成功"), ExitCode, 0);
        TestTrue(TEXT("新进程生成20资产"), Log.Contains(TEXT("Created=20")) && Snapshot(Root).Num() == 20);
        return true;
    }
    if (Case == TEXT("RejectInvalidRoot"))
    {
        for (const FString& Invalid : { FString(TEXT("/Engine/Y01Playground")), FString(TEXT("Saved/Playground")),
            FString(TEXT("/Game/../Danger")), FString(TEXT("/Game")), FString(TEXT("/Game/Bad.Name")) })
        {
            TestFalse(TEXT("拒绝非项目或非法包根目录"), Generate(Invalid, Result));
            TestFalse(TEXT("根目录失败须有明细"), Result.Errors.IsEmpty());
            TestEqual(TEXT("根目录失败不得保存"), Result.CreatedPackages.Num(), 0);
        }
        return true;
    }
    if (Case == TEXT("ValidateOnlyNeverSeeds"))
    {
        TestFalse(TEXT("只读验证不能生成缺失profile"), ValidateSaved(Root, Result));
        TestFalse(TEXT("缺失profile返回路径明细"), Result.Errors.IsEmpty());
        TestTrue(TEXT("不产生任何文件"), Snapshot(Root).IsEmpty());
        return true;
    }
    const FString RewardPackage = Root + TEXT("/Data/DT_RewardPackage");
    if (Case == TEXT("FillMissingOnly") || Case == TEXT("RejectInvalidPartial"))
    {
        TStrongObjectPtr<UY01PlaygroundProfile> Seed(CreateSeed(GetTransientPackage()));
        auto* Reward = DuplicateObject<UDataTable>(Seed->Tables[EY01TableKind::RewardPackage], CreatePackage(*RewardPackage), TEXT("DT_RewardPackage"));
        Reward->FindRow<FY01RewardPackageRow>(TEXT("Default"), TEXT("DiskTest"))->Gold = Case == TEXT("FillMissingOnly") ? 73 : -1;
        if (!TestTrue(TEXT("预存半套中的一张真实编辑表"), SaveTestAsset(Reward))) { return false; }
        const auto Before = Snapshot(Root);
        if (Case == TEXT("RejectInvalidPartial"))
        {
            TestFalse(TEXT("非法半套先验证失败"), Generate(Root, Result));
            TestTrue(TEXT("不得修改或增补非法半套"), SameFiles(Before, Snapshot(Root)));
            TestTrue(TEXT("错误包含行和字段"), Result.Errors.ContainsByPredicate([](const FString& Error)
            { return Error.Contains(TEXT("Default")) && Error.Contains(TEXT("Gold")); }));
            return true;
        }
        if (!TestTrue(TEXT("有效半套仅补缺失"), Generate(Root, Result))) { return false; }
        TestEqual(TEXT("仅新增18表及profile"), Result.CreatedPackages.Num(), 19);
        TestEqual(TEXT("保留已有表"), Result.PreservedPackages.Num(), 1);
        TestTrue(TEXT("profile使用已有表"), Result.Profile->Tables[EY01TableKind::RewardPackage] == Reward);
        const auto After = Snapshot(Root);
        for (const auto& Pair : Before) { TestTrue(TEXT("已有表字节不变"), After.Contains(Pair.Key) && After[Pair.Key] == Pair.Value); }
        return true;
    }
    if (Case == TEXT("RejectWrongAssetClass"))
    {
        auto* Wrong = NewObject<UY01PlaygroundProfile>(CreatePackage(*RewardPackage), TEXT("DT_RewardPackage"), RF_Public | RF_Standalone);
        if (!TestTrue(TEXT("预存同名不同类型资产"), SaveTestAsset(Wrong))) { return false; }
        const auto Before = Snapshot(Root);
        TestFalse(TEXT("不得覆盖同名不同类资产"), Generate(Root, Result));
        TestFalse(TEXT("冲突报告明细"), Result.Errors.IsEmpty());
        TestTrue(TEXT("类型冲突不改磁盘"), SameFiles(Before, Snapshot(Root)));
        return true;
    }
    if (!TestTrue(TEXT("生成真实表及profile"), Generate(Root, Result))) { return false; }
    TestEqual(TEXT("首次新增20包"), Result.CreatedPackages.Num(), 20);
    if (!TestNotNull(TEXT("返回持有配置"), Result.Profile.Get())) { return false; }
    TestTrue(TEXT("生成配置合法"), Result.Profile->Validate().IsValid());
    TestEqual(TEXT("磁盘20个uasset"), Snapshot(Root).Num(), 20);
    for (uint8 Index = 1; Index <= static_cast<uint8>(EY01TableKind::TalentChoice); ++Index)
    {
        const auto Kind = static_cast<EY01TableKind>(Index);
        const auto* Table = Result.Profile->Tables.Find(Kind);
        if (!TestTrue(TEXT("每种表均强引用"), Table && *Table)) { return false; }
        const FString Expected = Root + TEXT("/Data/DT_") + StaticEnum<EY01TableKind>()->GetNameStringByValue(Index);
        TestEqual(TEXT("真实具名Package"), (*Table)->GetOutermost()->GetName(), Expected);
        TestTrue(TEXT("非瞬态公开资产"), (*Table)->HasAllFlags(RF_Public | RF_Standalone) && !(*Table)->HasAnyFlags(RF_Transient));
    }
    if (Case == TEXT("SeedMonsterBinding"))
    {
        const auto Catalog = Result.Profile->BuildCatalog();
        FY01DataValidation Query;
        for (const FName Name : Result.Profile->Tables[EY01TableKind::Monster]->GetRowNames())
        {
            const auto* Row = Result.Profile->Tables[EY01TableKind::Monster]->FindRow<FY01MonsterRow>(Name, TEXT("DiskSeedTest"));
            if (!TestEqual(TEXT("隔离新种子怪物技能数量"), Row->Skills.Num(), 1)) { return false; }
            TestEqual(TEXT("隔离新种子绑定EnemyAttack"), Row->Skills[0].RowName, FName(TEXT("EnemyAttack")));
            const auto* Skill = Catalog.FindRow<FY01SkillRow>(Row->Skills[0], Query);
            if (!TestNotNull(TEXT("隔离种子实际引用可解析"), Skill)) { return false; }
            TestEqual(TEXT("隔离种子实际技能CD"), Skill->CooldownSeconds, 1.5f);
        }
        TestTrue(TEXT("隔离种子引用无误"), Query.IsValid());
    }
    if (Case == TEXT("RepairOptIn") || Case == TEXT("RepairPreservesEdits"))
    {
        auto* Monsters = Result.Profile->Tables[EY01TableKind::Monster].Get();
        auto* Normal = Monsters->FindRow<FY01MonsterRow>(TEXT("Default"), TEXT("RepairTest"));
        auto* Boss = Monsters->FindRow<FY01MonsterRow>(TEXT("Boss"), TEXT("RepairTest"));
        Normal->Skills[0].RowName = TEXT("BasicAttack"); Boss->Skills[0].RowName = TEXT("BasicAttack");
        // 额外字段编辑必须保留；迁移只处理已知旧技能身份。
        Normal->Gold = 37; Boss->Experience = 81;
        if (Case == TEXT("RepairPreservesEdits")) { Boss->Skills[0].RowName = TEXT("HeavyAttack"); }
        if (!TestTrue(TEXT("保存隔离旧引用及用户编辑"), SaveTestAsset(Monsters))) { return false; }
        const FString BeforeCSV = Monsters->GetTableAsCSV();
        const auto BeforeRepair = Snapshot(Root);
        TestTrue(TEXT("日常Generate不自动迁移旧引用"), Generate(Root, Result));
        TestTrue(TEXT("日常Generate不改任何文件"), SameFiles(BeforeRepair, Snapshot(Root)));
        TStrongObjectPtr<UY01CreatePlaygroundCommandlet> Command(NewObject<UY01CreatePlaygroundCommandlet>());
        TestEqual(TEXT("ValidateOnly即使带迁移开关仍只读"),
            Command->Main(TEXT("-Root=") + Root + TEXT(" -ValidateOnly -RepairSeedEnemyBinding")), 0);
        TestTrue(TEXT("只读命令不回写"), SameFiles(BeforeRepair, Snapshot(Root)));
        FString Error;
        if (Case == TEXT("RepairPreservesEdits"))
        {
            TestFalse(TEXT("显式迁移遇到用户其他合法ref必须拒绝"), RepairSeedEnemyBinding(Result.Profile.Get(), Error));
            TestFalse(TEXT("拒绝有明细"), Error.IsEmpty());
            TestEqual(TEXT("拒绝不得部分修改内存"), Monsters->GetTableAsCSV(), BeforeCSV);
            TestTrue(TEXT("拒绝不得修改磁盘"), SameFiles(BeforeRepair, Snapshot(Root)));
            return true;
        }
        if (!TestTrue(TEXT("显式opt-in迁移旧绑定"), RepairSeedEnemyBinding(Result.Profile.Get(), Error)))
        { AddError(Error); return false; }
        TestEqual(TEXT("只变更两个引用，全部其他字段保留"), Monsters->GetTableAsCSV(),
            BeforeCSV.Replace(TEXT("BasicAttack"), TEXT("EnemyAttack")));
        const auto AfterRepair = Snapshot(Root);
        for (const auto& Pair : BeforeRepair)
        {
            if (Pair.Key == PackageFile(Monsters->GetOutermost()->GetName())) { continue; }
            TestTrue(TEXT("迁移不写其他资产"), AfterRepair.Contains(Pair.Key) && AfterRepair[Pair.Key] == Pair.Value);
        }
        TestTrue(TEXT("重复显式迁移幂等"), RepairSeedEnemyBinding(Result.Profile.Get(), Error));
        TestTrue(TEXT("重复迁移不保存任何文件"), SameFiles(AfterRepair, Snapshot(Root)));
        FString Log; int32 ExitCode = -1;
        TestTrue(TEXT("新进程读取迁移后持久表"), ChildCommandlet(Root, true, Log, ExitCode));
        TestEqual(TEXT("迁移后持久引用合法"), ExitCode, 0);
        TestTrue(TEXT("新进程不改变迁移结果"), SameFiles(AfterRepair, Snapshot(Root)));
        return true;
    }
    if (Case == TEXT("RejectInvalidEdit"))
    {
        auto* Rewards = Result.Profile->Tables[EY01TableKind::RewardPackage].Get();
        Rewards->FindRow<FY01RewardPackageRow>(TEXT("Default"), TEXT("DiskTest"))->Gold = -5;
        if (!TestTrue(TEXT("保存非法用户编辑"), SaveTestAsset(Rewards))) { return false; }
        const auto Before = Snapshot(Root);
        TestFalse(TEXT("已有非法编辑须报错"), Generate(Root, Result));
        TestTrue(TEXT("具体指出非法Gold"), Result.Errors.ContainsByPredicate([](const FString& Error)
        { return Error.Contains(TEXT("Default")) && Error.Contains(TEXT("Gold")); }));
        TestTrue(TEXT("不修复、不回写profile"), SameFiles(Before, Snapshot(Root)));
        return true;
    }
    if (Case == TEXT("PreserveEdits"))
    {
        auto* Rewards = Result.Profile->Tables[EY01TableKind::RewardPackage].Get();
        auto* Characters = Result.Profile->Tables[EY01TableKind::Character].Get();
        Rewards->FindRow<FY01RewardPackageRow>(TEXT("Default"), TEXT("DiskTest"))->Gold = 73;
        Characters->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("DiskTest"))->Health = 321.f;
        Result.Profile->OrderedWaves.SetNum(1);
        auto* Monsters = Result.Profile->Tables[EY01TableKind::Monster].Get();
        auto* Skills = Result.Profile->Tables[EY01TableKind::Skill].Get();
        Monsters->FindRow<FY01MonsterRow>(TEXT("Default"), TEXT("DiskTest"))->Skills[0].RowName = TEXT("HeavyAttack");
        Skills->FindRow<FY01SkillRow>(TEXT("HeavyAttack"), TEXT("DiskTest"))->CooldownSeconds = 2.75f;
        if (!TestTrue(TEXT("保存用户编辑的表和profile"), SaveTestAsset(Rewards) && SaveTestAsset(Characters)
            && SaveTestAsset(Monsters) && SaveTestAsset(Skills) && SaveTestAsset(Result.Profile.Get()))) { return false; }
    }
    const auto Before = Snapshot(Root);
    FString Log;
    int32 ExitCode = -1;
    TestTrue(TEXT("新进程执行Commandlet"), ChildCommandlet(Root, Case != TEXT("PreserveEdits"), Log, ExitCode));
    TestEqual(TEXT("新进程真实加载成功"), ExitCode, 0);
    TestTrue(TEXT("新进程日志来自持久目录"), Log.Contains(TEXT("Tables=19")));
    TestTrue(TEXT("新进程读取真实编辑或种子金币"), Log.Contains(Case == TEXT("PreserveEdits") ? TEXT("DefaultGold=73") : TEXT("DefaultGold=10")));
    TestTrue(TEXT("新进程读取真实角色生命"), Log.Contains(Case == TEXT("PreserveEdits") ? TEXT("CharacterHealth=321") : TEXT("CharacterHealth=200")));
    TestTrue(TEXT("二次运行无字节变化"), SameFiles(Before, Snapshot(Root)));
    TestTrue(TEXT("同进程重复运行同样幂等"), Generate(Root, Result));
    TestEqual(TEXT("重复运行无保存"), Result.CreatedPackages.Num(), 0);
    TestEqual(TEXT("重复运行保留全部资产"), Result.PreservedPackages.Num(), 20);
    TestTrue(TEXT("全部资产字节仍一致"), SameFiles(Before, Snapshot(Root)));
    return true;
}
#endif
