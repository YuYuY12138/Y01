#include "Misc/AutomationTest.h"
#include "Playground/Y01CreatePlaygroundCommandlet.h"
#include "Playground/Y01PlaygroundGenerator.h"
#include "Playground/Y01PlaygroundRuntime.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerStart.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformProcess.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01PlaygroundMapTest, "Y01.Playground.Map",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01PlaygroundMapTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("SavedWorldAndPreserve"), TEXT("ValidateRequiresMap"), TEXT("FreshProcess"), TEXT("ReloadRuntime"), TEXT("RejectNonLegacyVerify") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01PlaygroundMapTest::RunTest(const FString& Case)
{
    const FString Root = TEXT("/Game/Developers/Y01PlaygroundTests/Map_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Package = Root + TEXT("/Maps/L_Y01Playground");
    const FString Filename = FPackageName::LongPackageNameToFilename(Package, FPackageName::GetMapPackageExtension());
    TStrongObjectPtr<UY01CreatePlaygroundCommandlet> Command(NewObject<UY01CreatePlaygroundCommandlet>());
    FY01PlaygroundGenerationResult Data;
    if (!TestTrue(TEXT("隔离持久19表"), Y01PlaygroundEditor::Generate(Root, Data))) { return false; }
    if (Case == TEXT("ValidateRequiresMap"))
    {
        AddExpectedError(TEXT("PlaygroundMap"), EAutomationExpectedErrorFlags::Contains, 1);
        TestEqual(TEXT("缺地图只读验证必须失败"), Command->Main(TEXT("-Root=") + Root + TEXT(" -WithMap -ValidateOnly")), 1);
        TestFalse(TEXT("不静默创建地图"), FPackageName::DoesPackageExist(Package)); return true;
    }
    TestEqual(TEXT("WithMap命令成功"), Command->Main(TEXT("-Root=") + Root + TEXT(" -WithMap")), 0);
    if (!TestTrue(TEXT("真实umap存在"), FPackageName::DoesPackageExist(Package))) { return false; }
    UPackage* Loaded = LoadPackage(nullptr, *Package, LOAD_None);
    UWorld* World = Loaded ? UWorld::FindWorldInPackage(Loaded) : nullptr;
    if (!TestNotNull(TEXT("可加载真实UWorld"), World)) { return false; }
    TestTrue(TEXT("非WP普通世界"), World->GetWorldPartition() == nullptr);
    TestTrue(TEXT("显式测试ModeOverride"), World->GetWorldSettings()->DefaultGameMode == AY01PlaygroundGameMode::StaticClass());
    int32 Configs = 0, Starts = 0;
    for (TActorIterator<AY01PlaygroundConfig> It(World); It; ++It)
    { ++Configs; TestTrue(TEXT("场景强持有正确profile"), It->Profile == Data.Profile.Get()); }
    for (TActorIterator<APlayerStart> It(World); It; ++It) { ++Starts; }
    TestEqual(TEXT("唯一配置Actor"), Configs, 1); TestTrue(TEXT("至少两个出生点"), Starts >= 2);
    TArray<uint8> Before, After; FFileHelper::LoadFileToArray(Before, *Filename);
    TestEqual(TEXT("重复WithMap不覆盖"), Command->Main(TEXT("-Root=") + Root + TEXT(" -WithMap")), 0);
    FFileHelper::LoadFileToArray(After, *Filename); TestTrue(TEXT("地图字节保留"), Before == After);
    if (Case == TEXT("ReloadRuntime") || Case == TEXT("RejectNonLegacyVerify"))
    {
        if (Case == TEXT("RejectNonLegacyVerify"))
        {
            const FString Legacy = Root + TEXT("_Legacy");
            if (!TestEqual(TEXT("先隔离旧即时配置"), Command->Main(TEXT("-Root=") + Root + TEXT(" -SkillLegacyCopy -CopyRoot=") + Legacy), 0)) { return false; }
            if (!TestEqual(TEXT("真实升级为新机制"), Command->Main(TEXT("-Root=") + Root + TEXT(" -SkillUpgrade -LegacyRoot=") + Legacy), 0)) { return false; }
        }
        auto* Characters = Data.Profile->Tables[EY01TableKind::Character].Get();
        auto* Rewards = Data.Profile->Tables[EY01TableKind::RewardPackage].Get();
        Characters->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("ReloadProbe"))->Health = 321;
        Rewards->FindRow<FY01RewardPackageRow>(TEXT("Default"), TEXT("ReloadProbe"))->Gold = 73;
        FSavePackageArgs SaveArgs; SaveArgs.TopLevelFlags = RF_Public | RF_Standalone; SaveArgs.SaveFlags = SAVE_NoError;
        for (UDataTable* Table : { Characters, Rewards })
        {
            const FString File = FPackageName::LongPackageNameToFilename(Table->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
            if (!TestTrue(TEXT("保存隔离表用户改值"), UPackage::SavePackage(Table->GetOutermost(), Table, *File, SaveArgs))) { return false; }
        }
        const FString LogFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / (TEXT("Playground-ReloadChild-") + FGuid::NewGuid().ToString() + TEXT(".log")));
        const FString Args = FString::Printf(TEXT("\"%s\" %s -game -nullrhi -nosound -nop4 -Y01PlaygroundVerify -abslog=\"%s\""),
            *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), *Package, *LogFile);
        auto Child = FPlatformProcess::CreateProc(*FPlatformProcess::GenerateApplicationPath(TEXT("UnrealEditor-Cmd"), EBuildConfiguration::Development), *Args, false, true, true, nullptr, 0, nullptr, nullptr);
        if (!TestTrue(TEXT("新进程真正运行改值后的地图"), Child.IsValid())) { return false; }
        const double Deadline = FPlatformTime::Seconds() + 70;
        while (FPlatformProcess::IsProcRunning(Child) && FPlatformTime::Seconds() < Deadline) { FPlatformProcess::Sleep(0.1f); }
        int32 Exit = -1;
        if (FPlatformProcess::IsProcRunning(Child)) { FPlatformProcess::TerminateProc(Child); }
        else { FPlatformProcess::GetProcReturnCode(Child, &Exit); }
        FPlatformProcess::CloseProc(Child);
        FString Log; FFileHelper::LoadFileToString(Log, *LogFile); AddInfo(LogFile);
        if (Case == TEXT("RejectNonLegacyVerify"))
        {
            TestEqual(TEXT("旧Verify对新技能必须明确失败退出"), Exit, 1);
            TestTrue(TEXT("失败说明只能使用legacy即时配置"), Log.Contains(TEXT("LEGACY_VERIFY_REJECT")));
            TestFalse(TEXT("不得误报新技能通过旧验收"), Log.Contains(TEXT("VERIFY_CLIENT_PASS")));
            return true;
        }
        TestTrue(TEXT("旧改表验收明确标识legacy"), Log.Contains(TEXT("LEGACY_VERIFY_ONLY")));
        TestEqual(TEXT("新游戏正常结束验证"), Exit, 0);
        TestTrue(TEXT("玩家ASC读取改后HP321"), Log.Contains(TEXT("HP=321 Attack=25")));
        TestTrue(TEXT("真实结算改后Gold98和XP17"), Log.Contains(TEXT("Gold=98 XP=17")));
        TestTrue(TEXT("真实技能、移动、伤害通过"), Log.Contains(TEXT("VERIFY_CLIENT_PASS")));
    }
    if (Case == TEXT("FreshProcess"))
    {
        const FString LogFile = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / (TEXT("Playground-MapChild-") + FGuid::NewGuid().ToString() + TEXT(".log")));
        const FString Args = FString::Printf(TEXT("\"%s\" -run=Y01CreatePlayground -Root=%s -WithMap -ValidateOnly -nullrhi -unattended -nop4 -abslog=\"%s\""),
            *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()), *Root, *LogFile);
        int32 ExitCode = -1;
        TestTrue(TEXT("新进程载入"), FPlatformProcess::ExecProcess(*FPlatformProcess::GenerateApplicationPath(TEXT("UnrealEditor-Cmd"), EBuildConfiguration::Development), *Args, &ExitCode, nullptr, nullptr));
        TestEqual(TEXT("新进程验证成功"), ExitCode, 0);
        FString Log; FFileHelper::LoadFileToString(Log, *LogFile);
        TestTrue(TEXT("确实加载了地图"), Log.Contains(TEXT("PlaygroundMap VALID")));
    }
    return true;
}
#endif
