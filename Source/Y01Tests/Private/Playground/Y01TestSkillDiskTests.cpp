#include "Misc/AutomationTest.h"
#include "Playground/Y01PlaygroundGenerator.h"
#include "Playground/Y01PlaygroundMapBuilder.h"
#include "Playground/Y01CreatePlaygroundCommandlet.h"
#include "Playground/Y01TestSkill.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
TMap<FString, TArray<uint8>> SkillFiles(const FString& Root)
{
    TMap<FString, TArray<uint8>> Result; TArray<FString> Names;
    IFileManager::Get().FindFilesRecursive(Names, *FPackageName::LongPackageNameToFilename(Root), TEXT("*"), true, false);
    for (const auto& Name : Names) { TArray<uint8> Data; FFileHelper::LoadFileToArray(Data, *Name); Result.Add(Name, MoveTemp(Data)); }
    return Result;
}
bool SameSkillFiles(const TMap<FString, TArray<uint8>>& A, const TMap<FString, TArray<uint8>>& B)
{
    if (A.Num() != B.Num()) { return false; }
    for (const auto& P : A) { if (!B.Contains(P.Key) || B[P.Key] != P.Value) { return false; } } return true;
}
FY01SkillRow* SkillRow(UY01PlaygroundProfile* Profile, int32 Slot)
{
    FY01DataValidation V; auto* Character = Profile->BuildCatalog().FindRow<FY01CharacterRow>(Profile->Character, V);
    return const_cast<FY01SkillRow*>(Profile->BuildCatalog().FindRow<FY01SkillRow>(Character->InitialSkills[Slot], V));
}
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01SkillDiskTest, "Y01.Playground.TestSkillDisk",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01SkillDiskTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : { TEXT("UpgradeOnlySkill"), TEXT("Idempotent"), TEXT("LegacyIsolation"), TEXT("CompareIsolation"),
        TEXT("FreshAudit"), TEXT("ReadOnlyWins"), TEXT("PreserveCustomCooldown"), TEXT("RejectUnexpectedClass"), TEXT("RequireLegacy") })
    { Names.Add(N); Commands.Add(N); }
}
bool FY01SkillDiskTest::RunTest(const FString& Case)
{
    using namespace Y01PlaygroundEditor;
    const FString Root = TEXT("/Game/Developers/Y01PlaygroundTests/SkillDisk_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Legacy = Root + TEXT("_Legacy"), Compare = Root + TEXT("_Compare");
    FY01PlaygroundGenerationResult R; if (!TestTrue(TEXT("隔离持久夹具生成"), Generate(Root, R))) { return false; }
    FString Error; if (!TestTrue(TEXT("隔离持久原地图"), BuildMap(Root, R.Profile.Get(), false, Error))) { return false; }
    auto* P = R.Profile.Get(); auto* Skills = P->Tables[EY01TableKind::Skill].Get();
    const auto MainBefore = SkillFiles(TEXT("/Game/Y01Playground"));
    auto* Cmd = NewObject<UY01CreatePlaygroundCommandlet>();
    auto Run = [&](FString Args) { return Cmd->Main(TEXT("-Root=") + Root + TEXT(" ") + Args); };
    if (Case == TEXT("PreserveCustomCooldown"))
    {
        SkillRow(P, 1)->CooldownSeconds = 2.75f;
        const FString File = FPackageName::LongPackageNameToFilename(Skills->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
        FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone;
        TestTrue(TEXT("保存真实自定义CD"), UPackage::SavePackage(Skills->GetOutermost(), Skills, *File, Args));
    }
    if (Case == TEXT("RequireLegacy"))
    {
        const auto Before = SkillFiles(Root);
        AddExpectedError(TEXT("SKILL_REJECT"), EAutomationExpectedErrorFlags::Contains, 1);
        TestEqual(TEXT("升级缺少隔离旧表时必须拒绝"), Run(TEXT("-SkillUpgrade -LegacyRoot=") + Legacy), 1);
        TestTrue(TEXT("失败不动任何包"), SameSkillFiles(Before, SkillFiles(Root))); return true;
    }
    const auto Original = *SkillRow(P, 0); const auto OriginalBall = *SkillRow(P, 1);
    if (!TestEqual(TEXT("保存旧即时隔离副本"), Run(TEXT("-SkillLegacyCopy -CopyRoot=") + Legacy), 0)) { return false; }
    if (Case == TEXT("RejectUnexpectedClass"))
    {
        SkillRow(P, 0)->Ability = UY01TestSkillAbility::StaticClass();
        const auto Before = SkillFiles(Root);
        AddExpectedError(TEXT("SKILL_REJECT"), EAutomationExpectedErrorFlags::Contains, 1);
        TestEqual(TEXT("不覆盖未知用户配置"), Run(TEXT("-SkillUpgrade -LegacyRoot=") + Legacy), 1);
        TestTrue(TEXT("拒绝不写包"), SameSkillFiles(Before, SkillFiles(Root))); return true;
    }
    const auto Before = SkillFiles(Root);
    if (Case == TEXT("ReadOnlyWins"))
    {
        TestEqual(TEXT("显式只读优先于升级"), Run(TEXT("-ValidateOnly -SkillUpgrade -LegacyRoot=") + Legacy), 0);
        TestEqual(TEXT("审计不升级"), Run(TEXT("-SkillAudit")), 0);
        TestTrue(TEXT("只读不改字节"), SameSkillFiles(Before, SkillFiles(Root)));
        TestEqual(TEXT("只读保留原GA"), SkillRow(P, 0)->Ability.ToSoftObjectPath(), Original.Ability.ToSoftObjectPath()); return true;
    }
    if (!TestEqual(TEXT("精准升级成功"), Run(TEXT("-SkillUpgrade -LegacyRoot=") + Legacy), 0)) { return false; }
    TestEqual(TEXT("主入口引用新测试GA"), SkillRow(P, 0)->Ability.ToSoftObjectPath(), FSoftObjectPath(UY01TestSkillAbility::StaticClass()));
    TestEqual(TEXT("范围测试配置半径250"), SkillRow(P, 0)->TestSkill.Radius, 250.f);
    TestEqual(TEXT("范围测试CD1.5"), SkillRow(P, 0)->CooldownSeconds, 1.5f);
    TestEqual(TEXT("球速度400"), SkillRow(P, 1)->TestSkill.Speed, 400.f);
    TestEqual(TEXT("球CD保留磁盘旧值"), SkillRow(P, 1)->CooldownSeconds, OriginalBall.CooldownSeconds);
    const auto After = SkillFiles(Root); int32 Changed = 0;
    for (const auto& Pair : Before)
    {
        const bool Different = !After.Contains(Pair.Key) || Pair.Value != After[Pair.Key];
        if (Different) { ++Changed; TestTrue(TEXT("只能保存技能表"), Pair.Key.EndsWith(TEXT("DT_Skill.uasset"))); }
    }
    TestEqual(TEXT("没有新增或缺失主包"), After.Num(), Before.Num()); TestEqual(TEXT("精确只改一张表"), Changed, 1);
    FY01PlaygroundGenerationResult L;
    if (!TestTrue(TEXT("legacy实际已落盘"), ValidateSaved(Legacy, L))) { return false; }
    TestEqual(TEXT("legacy原GA"), SkillRow(L.Profile.Get(), 0)->Ability.ToSoftObjectPath(), Original.Ability.ToSoftObjectPath());
    TestEqual(TEXT("legacy原CD"), SkillRow(L.Profile.Get(), 0)->CooldownSeconds, Original.CooldownSeconds);
    TestEqual(TEXT("legacy只含技能表/Profile/地图三包"), SkillFiles(Legacy).Num(), 3);
    TestTrue(TEXT("legacy启用训练与原Profile一致"), L.Profile->bTrainingEnabled == P->bTrainingEnabled);
    TestTrue(TEXT("legacy地图持久"), FPackageName::DoesPackageExist(Legacy + TEXT("/Maps/L_Y01Playground")));
    if (Case == TEXT("Idempotent"))
    {
        TestEqual(TEXT("重复升级"), Run(TEXT("-SkillUpgrade -LegacyRoot=") + Legacy), 0);
        TestTrue(TEXT("重复升级不保存"), SameSkillFiles(After, SkillFiles(Root)));
        const auto LB = SkillFiles(Legacy); TestEqual(TEXT("重复legacy不覆盖"), Run(TEXT("-SkillLegacyCopy -CopyRoot=") + Legacy), 0);
        TestTrue(TEXT("原legacy字节不变"), SameSkillFiles(LB, SkillFiles(Legacy)));
    }
    if (Case == TEXT("CompareIsolation"))
    {
        TestEqual(TEXT("对照配置生成"), Run(TEXT("-SkillCompareCopy -CopyRoot=") + Compare), 0);
        FY01PlaygroundGenerationResult C; if (!TestTrue(TEXT("compare实际落盘"), ValidateSaved(Compare, C))) { return false; }
        TestEqual(TEXT("compare仅球速度200"), SkillRow(C.Profile.Get(), 1)->TestSkill.Speed, 200.f);
        TestEqual(TEXT("compare半径相同"), SkillRow(C.Profile.Get(), 1)->TestSkill.Radius, 12.f);
        TestEqual(TEXT("compare只有三包"), SkillFiles(Compare).Num(), 3);
        const auto CB = SkillFiles(Compare); TestEqual(TEXT("重复对照幂等"), Run(TEXT("-SkillCompareCopy -CopyRoot=") + Compare), 0);
        TestTrue(TEXT("对照幂等不保存"), SameSkillFiles(CB, SkillFiles(Compare)));
        TestTrue(TEXT("对照生成不污染原表"), SameSkillFiles(After, SkillFiles(Root)));
    }
    if (Case == TEXT("FreshAudit"))
    {
        const FString AuditFilename = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / (TEXT("SkillAudit-") + FGuid::NewGuid().ToString() + TEXT(".log")));
        FString Args = FString::Printf(TEXT("\"%s\" -run=Y01CreatePlayground -Root=%s -SkillAudit -unattended -nop4 -nullrhi -nosound -abslog=\"%s\""),
            *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), *Root, *AuditFilename);
        int32 Exit = -1; TestTrue(TEXT("新进程读盘审计"), FPlatformProcess::ExecProcess(*FPlatformProcess::GenerateApplicationPath(TEXT("UnrealEditor-Cmd"), EBuildConfiguration::Development), *Args, &Exit, nullptr, nullptr));
        TestEqual(TEXT("审计exit0"), Exit, 0); FString Log; FFileHelper::LoadFileToString(Log, *AuditFilename);
        TestTrue(TEXT("审计给出类路径行名和真实数值"), Log.Contains(TEXT("SKILL_AUDIT")) && Log.Contains(TEXT("Row=HeavyAttack")) && Log.Contains(TEXT("Speed=400")) && Log.Contains(TEXT("Y01TestSkillAbility")));
        TestTrue(TEXT("新进程审计只读"), SameSkillFiles(After, SkillFiles(Root)));
    }
    TestTrue(TEXT("隔离回归不污染普通主表"), SameSkillFiles(MainBefore, SkillFiles(TEXT("/Game/Y01Playground")))); return true;
}
#endif
