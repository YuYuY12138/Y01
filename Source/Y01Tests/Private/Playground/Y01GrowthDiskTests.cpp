#include "Misc/AutomationTest.h"
#include "Playground/Y01PlaygroundGenerator.h"
#include "Playground/Y01PlaygroundMapBuilder.h"
#include "Playground/Y01CreatePlaygroundCommandlet.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "UObject/SavePackage.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
TMap<FString,TArray<uint8>> GrowthFiles(const FString& Root)
{
    TMap<FString,TArray<uint8>> R; TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files,*FPackageName::LongPackageNameToFilename(Root),TEXT("*"),true,false);
    for (const auto& File : Files) { TArray<uint8> Bytes; FFileHelper::LoadFileToArray(Bytes,*File); R.Add(File,MoveTemp(Bytes)); } return R;
}
bool SameGrowthFiles(const TMap<FString,TArray<uint8>>& A,const TMap<FString,TArray<uint8>>& B)
{
    if (A.Num()!=B.Num()) { return false; }
    for (const auto& P:A) { if (!B.Contains(P.Key) || B[P.Key]!=P.Value) { return false; } } return true;
}
bool SaveGrowthFixture(UObject* A)
{
    A->GetOutermost()->FullyLoad(); ResetLoaders(A->GetOutermost());
    FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone;
    return UPackage::SavePackage(A->GetOutermost(),A,*FPackageName::LongPackageNameToFilename(A->GetOutermost()->GetName(),FPackageName::GetAssetPackageExtension()),Args);
}
FY01CharacterRow* GrowthRow(UY01PlaygroundProfile* P)
{ return P->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(P->Character.RowName,TEXT("GrowthDisk")); }
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01GrowthDiskTests,"Y01.Playground.Growth.Disk",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ProductFilter)
void FY01GrowthDiskTests::GetTests(TArray<FString>& N,TArray<FString>& C) const
{
    for (const TCHAR* S:{TEXT("Isolation"),TEXT("PreciseMain"),TEXT("Baseline"),TEXT("GrowthCompare"),TEXT("Idempotent"),TEXT("FreshProcess"),
        TEXT("ReadOnly"),TEXT("UserGrowth"),TEXT("PreserveRows"),TEXT("SnapshotConflict"),TEXT("SnapshotRows"),TEXT("SnapshotPath"),
        TEXT("ProfileConflict"),TEXT("PartialOutput"),TEXT("MissingLegacy"),TEXT("SharedProfileUpgrade")}) { N.Add(S); C.Add(S); }
}
bool FY01GrowthDiskTests::RunTest(const FString& Case)
{
    using namespace Y01PlaygroundEditor;
    const FString Root=TEXT("/Game/Developers/Y01PlaygroundTests/GrowthDisk_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Legacy=Root+TEXT("_Legacy"), Compare=Root+TEXT("_Compare"), Growth=Root+TEXT("_Growth"), Baseline=Compare+TEXT("/Baseline");
    FY01PlaygroundGenerationResult R,L,C; FString Error;
    if (!TestTrue(TEXT("真实持久种子"),Generate(Root,R)) || !TestTrue(TEXT("真实原地图"),BuildMap(Root,R.Profile.Get(),false,Error))) { return false; }
    auto* P=R.Profile.Get(); auto* Table=P->Tables[EY01TableKind::Character].Get();
    GrowthRow(P)->MoveSpeed=543;
    FY01CharacterRow Extra=*GrowthRow(P); Extra.Health=321; Extra.R33TestGrowth.bEnabled=true; Extra.R33TestGrowth.AttackPerEffectiveSecond=7;
    Table->AddRow(TEXT("UserExtra"),Extra); TestTrue(TEXT("保存用户其他行和字段"),SaveGrowthFixture(Table));
    auto* Cmd=NewObject<UY01CreatePlaygroundCommandlet>();
    auto Run=[&](const FString& Args) { return Cmd->Main(TEXT("-Root=")+Root+TEXT(" ")+Args); };
    if (!TestEqual(TEXT("真实旧三包legacy"),Run(TEXT("-SkillLegacyCopy -CopyRoot=")+Legacy),0)
        || !TestEqual(TEXT("U2先升级"),Run(TEXT("-SkillUpgrade -LegacyRoot=")+Legacy),0)
        || !TestEqual(TEXT("真实旧三包compare"),Run(TEXT("-SkillCompareCopy -CopyRoot=")+Compare),0)) { return false; }
    TestTrue(TEXT("实际读盘legacy"),ValidateSaved(Legacy,L)); TestTrue(TEXT("实际读盘compare"),ValidateSaved(Compare,C));
    TestEqual(TEXT("升级前legacy三包保持原契约"),GrowthFiles(Legacy).Num(),3); TestEqual(TEXT("升级前compare三包"),GrowthFiles(Compare).Num(),3);
    const auto MainProtected=GrowthFiles(TEXT("/Game/Y01Playground"));
    const FString Args=TEXT("-GrowthUpgrade -LegacyRoot=")+Legacy+TEXT(" -CompareRoot=")+Compare+TEXT(" -GrowthCompareRoot=")+Growth;
    auto All=[&] { auto M=GrowthFiles(Root); M.Append(GrowthFiles(Legacy)); M.Append(GrowthFiles(Compare)); M.Append(GrowthFiles(Growth)); return M; };
    if (Case==TEXT("ReadOnly"))
    {
        const auto Before=All(); TestEqual(TEXT("显式只读优先"),Run(Args+TEXT(" -ValidateOnly")),0);
        TestFalse(TEXT("主成长未启用"),GrowthRow(P)->R33TestGrowth.bEnabled); TestTrue(TEXT("审计零写"),SameGrowthFiles(Before,All())); return true;
    }
    if (Case==TEXT("UserGrowth") || Case==TEXT("ProfileConflict") || Case==TEXT("PartialOutput") || Case==TEXT("MissingLegacy"))
    {
        if (Case==TEXT("UserGrowth")) { GrowthRow(P)->R33TestGrowth.AttackPerEffectiveSecond=3; TestTrue(TEXT("用户配置落盘"),SaveGrowthFixture(Table)); }
        if (Case==TEXT("ProfileConflict")) { L.Profile->OrderedWaves.Swap(0,1); TestTrue(TEXT("用户Profile落盘"),SaveGrowthFixture(L.Profile.Get())); }
        if (Case==TEXT("PartialOutput"))
        { auto* Block=DuplicateObject<UDataTable>(Table,CreatePackage(*(Growth+TEXT("/Data/DT_Character"))),TEXT("DT_Character")); TestTrue(TEXT("冲突半包落盘"),SaveGrowthFixture(Block)); }
        const auto Before=All(); AddExpectedError(TEXT("GROWTH_REJECT"),EAutomationExpectedErrorFlags::Contains,1);
        const FString Bad=Case==TEXT("MissingLegacy") ? Args.Replace(*Legacy,*(Legacy+TEXT("Missing"))) : Args;
        TestEqual(TEXT("预检拒绝不覆盖"),Run(Bad),1); TestTrue(TEXT("拒绝前后全部字节相同"),SameGrowthFiles(Before,All())); return true;
    }
    const auto Before=All(), MainBefore=GrowthFiles(Root);
    TestEqual(TEXT("精准成长迁移成功"),Run(Args),0);
    TestTrue(TEXT("主最后开启"),GrowthRow(P)->R33TestGrowth.bEnabled);
    TestEqual(TEXT("主攻击每秒1"),GrowthRow(P)->R33TestGrowth.AttackPerEffectiveSecond,1.);
    TestEqual(TEXT("主击杀2"),GrowthRow(P)->R33TestGrowth.AttackPerEffectiveKill,2.);
    TestEqual(TEXT("主金币每秒1"),GrowthRow(P)->R33TestGrowth.GoldPerEffectiveSecond,1.);
    for (auto* Old:{L.Profile.Get(),C.Profile.Get()})
    {
        auto* Snapshot=Old->Tables[EY01TableKind::Character].Get();
        TestTrue(TEXT("旧表真正独立"),Snapshot!=Table);
        TestEqual(TEXT("快照在自身根"),Snapshot->GetOutermost()->GetName(),FPackageName::GetLongPackagePath(Old->GetOutermost()->GetName())+TEXT("/Data/DT_Character"));
        TestFalse(TEXT("旧目标关闭"),GrowthRow(Old)->R33TestGrowth.bEnabled);
        TestEqual(TEXT("用户非成长字段保留"),GrowthRow(Old)->MoveSpeed,543.f);
        const auto* Other=Snapshot->FindRow<FY01CharacterRow>(TEXT("UserExtra"),TEXT("GrowthDisk"));
        TestTrue(TEXT("全表其他行原样保留"),Other && FY01CharacterRow::StaticStruct()->CompareScriptStruct(Other,&Extra,0));
    }
    const auto After=All();
    for (const auto& File:Before)
    {
        const bool Allowed=File.Key.EndsWith(TEXT("DT_Character.uasset")) || (File.Key.EndsWith(TEXT("DA_PlaygroundProfile.uasset")) && !MainBefore.Contains(File.Key));
        if (!Allowed) { TestTrue(TEXT("旧Map Skill Training Reward等逐字节不动：")+File.Key,After.Contains(File.Key) && After[File.Key]==File.Value); }
    }
    FY01PlaygroundGenerationResult B,G;
    const bool HasB=ValidateSaved(Baseline,B), HasG=ValidateSaved(Growth,G);
    TestTrue(TEXT("固定25基线实际落盘"),HasB); TestTrue(TEXT("成长对照实际落盘"),HasG);
    if (HasB)
    {
        TestEqual(TEXT("基线仅Profile与Map"),GrowthFiles(Baseline).Num(),2);
        TestTrue(TEXT("基线共用compare禁用Character"),B.Profile->Tables[EY01TableKind::Character]==C.Profile->Tables[EY01TableKind::Character]);
        TestTrue(TEXT("基线共用主400技能"),B.Profile->Tables[EY01TableKind::Skill]==P->Tables[EY01TableKind::Skill]);
        TestTrue(TEXT("基线地图引用验证"),BuildMap(Baseline,B.Profile.Get(),true,Error));
    }
    if (HasG)
    {
        TestEqual(TEXT("成长对照仅三包"),GrowthFiles(Growth).Num(),3); TestEqual(TEXT("对照每秒2"),GrowthRow(G.Profile.Get())->R33TestGrowth.AttackPerEffectiveSecond,2.);
        TestEqual(TEXT("对照其余rate不动"),GrowthRow(G.Profile.Get())->R33TestGrowth.AttackPerEffectiveKill,2.);
        for (const auto& T:P->Tables) { if (T.Key!=EY01TableKind::Character) { TestTrue(TEXT("成长对照共享其他表"),G.Profile->Tables[T.Key]==T.Value); } }
    }
    if (Case.StartsWith(TEXT("Snapshot")))
    {
        auto* S=L.Profile->Tables[EY01TableKind::Character].Get();
        if (Case==TEXT("SnapshotConflict")) { GrowthRow(L.Profile.Get())->Health+=1; }
        if (Case==TEXT("SnapshotRows")) { S->RemoveRow(TEXT("UserExtra")); }
        if (Case==TEXT("SnapshotPath")) { S=DuplicateObject<UDataTable>(S,CreatePackage(*(Legacy+TEXT("/Data/WrongCharacter"))),TEXT("WrongCharacter")); L.Profile->Tables[EY01TableKind::Character]=S; }
        TestTrue(TEXT("保存真实快照冲突"),SaveGrowthFixture(S)); TestTrue(TEXT("保存真实引用"),SaveGrowthFixture(L.Profile.Get()));
        const auto Guard=All(); AddExpectedError(TEXT("GROWTH_REJECT"),EAutomationExpectedErrorFlags::Contains,1);
        TestEqual(TEXT("严验快照例外"),Run(Args),1); TestTrue(TEXT("冲突零写"),SameGrowthFiles(Guard,All())); return true;
    }
    if (Case==TEXT("SharedProfileUpgrade"))
    {
        TestEqual(TEXT("隔离后旧升级仍可严格幂等"),Run(TEXT("-SkillUpgrade -LegacyRoot=")+Legacy),0);
        TestEqual(TEXT("隔离后legacy复制不重存"),Run(TEXT("-SkillLegacyCopy -CopyRoot=")+Legacy),0);
        TestEqual(TEXT("隔离后compare复制不重存"),Run(TEXT("-SkillCompareCopy -CopyRoot=")+Compare),0);
    }
    if (Case==TEXT("FreshProcess"))
    {
        const FString Log=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/(TEXT("R33-U3-Fresh-")+FGuid::NewGuid().ToString()+TEXT(".log")));
        const FString Command=FString::Printf(TEXT("\"%s\" -run=Y01CreatePlayground -Root=%s %s -unattended -nop4 -nullrhi -nosound -abslog=\"%s\""),*FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()),*Root,*Args,*Log);
        int32 Exit=-1; TestTrue(TEXT("跨新进程复读并重跑"),FPlatformProcess::ExecProcess(*FPlatformProcess::GenerateApplicationPath(TEXT("UnrealEditor-Cmd"),EBuildConfiguration::Development),*Command,&Exit,nullptr,nullptr));
        TestEqual(TEXT("新进程幂等exit0"),Exit,0); FString Text; FFileHelper::LoadFileToString(Text,*Log);
        TestTrue(TEXT("读盘真实字段证据"),Text.Contains(TEXT("GROWTH_AUDIT")) && Text.Contains(TEXT("Seconds=1 Kills=2 Gold=1")) && Text.Contains(TEXT("PRESERVED")));
    }
    TestEqual(TEXT("重复精准升级"),Run(Args),0); TestTrue(TEXT("重复全部包零写"),SameGrowthFiles(After,All()));
    TestTrue(TEXT("不污染真实main"),SameGrowthFiles(MainProtected,GrowthFiles(TEXT("/Game/Y01Playground")))); return true;
}
#endif
