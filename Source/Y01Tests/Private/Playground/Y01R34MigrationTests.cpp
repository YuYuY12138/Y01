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
using K = EY01TableKind;
TMap<FString,TArray<uint8>> MigrationFiles(const FString& Root)
{
    TMap<FString,TArray<uint8>> R; TArray<FString> Files;
    IFileManager::Get().FindFilesRecursive(Files,*FPackageName::LongPackageNameToFilename(Root),TEXT("*"),true,false);
    for (const auto& File:Files) { TArray<uint8> B; FFileHelper::LoadFileToArray(B,*File); R.Add(File,MoveTemp(B)); } return R;
}
bool SameMigrationFiles(const TMap<FString,TArray<uint8>>& A,const TMap<FString,TArray<uint8>>& B)
{
    if (A.Num()!=B.Num()) { return false; }
    for (const auto& E:A) { if (!B.Contains(E.Key) || B[E.Key]!=E.Value) { return false; } } return true;
}
bool SaveMigrationFixture(UObject* A)
{
    const FString Package=A->GetOutermost()->GetName();
    if (!Package.StartsWith(TEXT("/Game/Developers/Y01PlaygroundTests/R34Disk_"))) { return false; }
    const FString File=FPackageName::LongPackageNameToFilename(Package,FPackageName::GetAssetPackageExtension());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(File),true);
    A->GetOutermost()->FullyLoad(); ResetLoaders(A->GetOutermost());
    FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone;
    return UPackage::SavePackage(A->GetOutermost(),A,*File,Args);
}
template<class T> T* MigrationRow(UY01PlaygroundProfile* P,K Kind,FName Name=TEXT("Default"))
{ return P->Tables[Kind]->FindRow<T>(Name,TEXT("R34MigrationTest")); }
bool MigrationChild(const FString& Root,const FString& Extra,FString& Text,int32& Exit)
{
    const FString Log=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/(TEXT("R34-DiskChild-")+FGuid::NewGuid().ToString()+TEXT(".log")));
    const FString Args=FString::Printf(TEXT("\"%s\" -run=Y01CreatePlayground -Root=%s %s -unattended -nop4 -nullrhi -nosound -abslog=\"%s\""),
        *FPaths::ConvertRelativePathToFull(FPaths::GetProjectFilePath()),*Root,*Extra,*Log);
    const bool Ok=FPlatformProcess::ExecProcess(*FPlatformProcess::GenerateApplicationPath(TEXT("UnrealEditor-Cmd"),EBuildConfiguration::Development),*Args,&Exit,nullptr,nullptr);
    FFileHelper::LoadFileToString(Text,*Log); return Ok;
}
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R34MigrationTests,"Y01.Playground.R34.Migration",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::ProductFilter)
void FY01R34MigrationTests::GetTests(TArray<FString>& N,TArray<FString>& C) const
{
    for (const TCHAR* S:{TEXT("ReferenceFields"),TEXT("DisableGrowthPassive"),TEXT("SeparateMonsterRows"),TEXT("EquipmentEffectFree"),
        TEXT("PreserveRowsFields"),TEXT("SharedIsolation"),TEXT("HistorySnapshot"),TEXT("Idempotent"),TEXT("PreserveR34UserEdits"),
        TEXT("ReadOnly"),TEXT("ConflictCharacter"),TEXT("ConflictNewRow"),TEXT("ConflictOutput"),TEXT("MissingIsolation"),
        TEXT("FreshProcess"),TEXT("CompareCopy"),TEXT("RangedCopy"),TEXT("CopyConflict"),TEXT("AuditMainLegal"),
        TEXT("MissingLastIsolation"),TEXT("HistoryCommands"),TEXT("HistoryRejectSharedWrite"),TEXT("TrainingReadOnly"),TEXT("RejectR34HistoricalUpgrade")}) { N.Add(S); C.Add(S); }
}
bool FY01R34MigrationTests::RunTest(const FString& Case)
{
    using namespace Y01PlaygroundEditor;
    if (Case==TEXT("AuditMainLegal"))
    {
        const FString Root=TEXT("/Game/Y01Playground"); const auto Before=MigrationFiles(Root);
        FString Text; int32 Exit=-1;
        TestTrue(TEXT("独立进程审计真实主盘而非内存"),MigrationChild(Root,TEXT("-R34Audit -ValidateOnly"),Text,Exit));
        TestEqual(TEXT("主用户配置合法即可，不固定模板数值"),Exit,0);
        TestTrue(TEXT("明确逐表逐行审计证据"),Text.Contains(TEXT("R34_AUDIT")) && Text.Contains(TEXT("DT_Character")));
        TestTrue(TEXT("主审计零写"),SameMigrationFiles(Before,MigrationFiles(Root))); return true;
    }
    const FString Root=TEXT("/Game/Developers/Y01PlaygroundTests/R34Disk_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    const FString Legacy=Root+TEXT("_Legacy"),Compare=Root+TEXT("_U2Compare"),Growth=Root+TEXT("_Growth"),Baseline=Compare+TEXT("/Baseline");
    const FString History=Root+TEXT("_History"),Output=Root+TEXT("_Output");
    FY01PlaygroundGenerationResult R,L,C,G,B,H,O; FString Error;
    if (!TestTrue(TEXT("真实旧种子落盘"),Generate(Root,R))) { return false; }
    auto* P=R.Profile.Get(); auto* Character=MigrationRow<FY01CharacterRow>(P,K::Character);
    auto* Training=MigrationRow<FY01TrainingStageRow>(P,K::TrainingStage);
    P->bTrainingEnabled=true; Training->SpawnInterval=3; Training->MaxAlive=6; Training->Spawns[0].Count=2; Training->Spawns[0].DelaySeconds=0;
    TestTrue(TEXT("训练旧参考落盘"),SaveMigrationFixture(P->Tables[K::TrainingStage])); TestTrue(TEXT("旧Profile落盘"),SaveMigrationFixture(P));
    if (!TestTrue(TEXT("旧地图落盘"),BuildMap(Root,P,false,Error))) { return false; }
    auto* Cmd=NewObject<UY01CreatePlaygroundCommandlet>();
    auto Run=[&](const FString& Args) { return Cmd->Main(TEXT("-Root=")+Root+TEXT(" ")+Args); };
    if (!TestEqual(TEXT("旧即时隔离"),Run(TEXT("-SkillLegacyCopy -CopyRoot=")+Legacy),0)
        || !TestEqual(TEXT("旧手动技能升级"),Run(TEXT("-SkillUpgrade -LegacyRoot=")+Legacy),0)
        || !TestEqual(TEXT("旧200速度对照"),Run(TEXT("-SkillCompareCopy -CopyRoot=")+Compare),0)
        || !TestEqual(TEXT("旧成长资产"),Run(TEXT("-GrowthUpgrade -LegacyRoot=")+Legacy+TEXT(" -CompareRoot=")+Compare+TEXT(" -GrowthCompareRoot=")+Growth),0)) { return false; }
    ValidateSaved(Legacy,L); ValidateSaved(Compare,C); ValidateSaved(Growth,G); ValidateSaved(Baseline,B);
    if (!L.Profile || !C.Profile || !G.Profile || !B.Profile) { return false; }
    if (Case.StartsWith(TEXT("History")) && Case!=TEXT("HistorySnapshot"))
    {
        const FString Upgrade=TEXT("-R34Upgrade -HistoryRoot=")+History+TEXT(" -IsolationRoots=")+Legacy+TEXT(",")+Compare+TEXT(",")+Baseline+TEXT(",")+Growth;
        TestEqual(TEXT("历史兼容前完成真实迁移"),Run(Upgrade),0);
        if (!TestTrue(TEXT("显式历史快照存在"),ValidateSaved(History,H))) { return false; }
        auto HistoryRun=[&](const FString& Options) { return Cmd->Main(TEXT("-Root=")+History+TEXT(" ")+Options); };
        if (Case==TEXT("HistoryRejectSharedWrite"))
        {
            auto* Skill=MigrationRow<FY01SkillRow>(P,K::Skill,TEXT("BasicAttack"));
            *Skill=*MigrationRow<FY01SkillRow>(L.Profile.Get(),K::Skill,TEXT("BasicAttack"));
            SaveMigrationFixture(P->Tables[K::Skill]);
            const auto MainBefore=MigrationFiles(Root),HistoryBefore=MigrationFiles(History);
            AddExpectedError(TEXT("SKILL_REJECT"),EAutomationExpectedErrorFlags::Contains,1);
            TestEqual(TEXT("历史入口不得回写共享主技能"),HistoryRun(TEXT("-SkillUpgrade -LegacyRoot=")+Legacy),1);
            TestTrue(TEXT("共享主技能零写"),SameMigrationFiles(MainBefore,MigrationFiles(Root)));
            TestTrue(TEXT("历史零写"),SameMigrationFiles(HistoryBefore,MigrationFiles(History))); return true;
        }
        auto AllHistory=[&] { auto Files=MigrationFiles(Root); for (const auto& Path:{Legacy,Compare,Growth,History}) { Files.Append(MigrationFiles(Path)); } return Files; };
        const auto Before=AllHistory();
        TestEqual(TEXT("显式历史技能升级仅严验"),HistoryRun(TEXT("-SkillUpgrade -LegacyRoot=")+Legacy),0);
        TestEqual(TEXT("显式历史legacy仅严验"),HistoryRun(TEXT("-SkillLegacyCopy -CopyRoot=")+Legacy),0);
        TestEqual(TEXT("显式历史compare仍200"),HistoryRun(TEXT("-SkillCompareCopy -CopyRoot=")+Compare),0);
        TestEqual(TEXT("显式历史成长幂等"),HistoryRun(TEXT("-GrowthUpgrade -LegacyRoot=")+Legacy+TEXT(" -CompareRoot=")+Compare+TEXT(" -GrowthCompareRoot=")+Growth),0);
        TestEqual(TEXT("显式历史训练幂等"),HistoryRun(TEXT("-TrainingUpgrade")),0);
        TestTrue(TEXT("历史命令全部零写"),SameMigrationFiles(Before,AllHistory())); return true;
    }
    if (Case==TEXT("TrainingReadOnly"))
    {
        const auto Before=MigrationFiles(Root);
        TestEqual(TEXT("训练ValidateOnly优先返回且不要求主根"),Run(TEXT("-TrainingUpgrade -ValidateOnly")),0);
        TestTrue(TEXT("训练只读零写"),SameMigrationFiles(Before,MigrationFiles(Root))); return true;
    }
    // 额外行与非指定字段必须逐字节保持其序列化内容。
    const auto Original=*Character; const auto OriginalTraining=*Training;
    auto Extra=Original; Extra.Health=321; Extra.R33TestGrowth.AttackPerEffectiveSecond=7;
    P->Tables[K::Character]->AddRow(TEXT("UserExtra"),Extra);
    auto* Monster=MigrationRow<FY01MonsterRow>(P,K::Monster); Monster->Gold=37;
    auto* Wave=MigrationRow<FY01WaveRow>(P,K::Wave); Wave->PreparationSeconds=6.25f;
    SaveMigrationFixture(P->Tables[K::Character]); SaveMigrationFixture(P->Tables[K::Monster]); SaveMigrationFixture(P->Tables[K::Wave]);
    const auto OriginalMonster=*Monster; const auto OriginalWave=*Wave;
    const FString Args=TEXT("-R34Upgrade -HistoryRoot=")+History+TEXT(" -IsolationRoots=")+Legacy+TEXT(",")+Compare+TEXT(",")+Baseline+TEXT(",")+Growth;
    auto All=[&] { auto F=MigrationFiles(Root); for (const auto& Path:{Legacy,Compare,Growth,History,Output}) { F.Append(MigrationFiles(Path)); } return F; };
    const auto Protected=MigrationFiles(TEXT("/Game/Y01Playground"));
    if (Case==TEXT("ReadOnly"))
    {
        const auto Before=All(); TestEqual(TEXT("只读覆盖升级选项"),Run(Args+TEXT(" -ValidateOnly")),0);
        TestTrue(TEXT("只读不关闭成长"),Character->R33TestGrowth.bEnabled);
        TestTrue(TEXT("只读零新增零保存"),SameMigrationFiles(Before,All())); return true;
    }
    if (Case.StartsWith(TEXT("Conflict")) || Case==TEXT("MissingIsolation") || Case==TEXT("MissingLastIsolation"))
    {
        if (Case==TEXT("ConflictCharacter")) { Character->AttackDamage=21; SaveMigrationFixture(P->Tables[K::Character]); }
        if (Case==TEXT("ConflictNewRow")) { P->Tables[K::Monster]->AddRow(TEXT("TrainingDummy"),OriginalMonster); SaveMigrationFixture(P->Tables[K::Monster]); }
        if (Case==TEXT("ConflictOutput"))
        { auto* Block=DuplicateObject<UDataTable>(P->Tables[K::Character],CreatePackage(*(History+TEXT("/Data/DT_Character"))),TEXT("DT_Character")); SaveMigrationFixture(Block); }
        const auto Before=All(); AddExpectedError(TEXT("R34_REJECT"),EAutomationExpectedErrorFlags::Contains,1);
        const FString Bad=Case==TEXT("MissingIsolation") ? Args.Replace(*Legacy,*(Legacy+TEXT("Missing")))
            : Case==TEXT("MissingLastIsolation") ? Args.Replace(*Growth,*(Growth+TEXT("Missing"))) : Args;
        TestEqual(TEXT("未知用户值/占用/漏隔离拒绝"),Run(Bad),1);
        TestTrue(TEXT("冲突预检零写"),SameMigrationFiles(Before,All())); return true;
    }
    const auto Before=All();
    if (!TestEqual(TEXT("精准迁移成功"),Run(Args),0)) { return false; }
    Character=MigrationRow<FY01CharacterRow>(P,K::Character); Training=MigrationRow<FY01TrainingStageRow>(P,K::TrainingStage); Wave=MigrationRow<FY01WaveRow>(P,K::Wave);
    auto Expected=Original;
    Expected.Health=300; Expected.AttackDamage=30; Expected.AttackInterval=1; Expected.AttackType=EY01AttackType::Melee;
    Expected.Range=200; Expected.MeleeAngle=90; Expected.ProjectileSpeed=400; Expected.Armor=0; Expected.MoveSpeed=500; Expected.HealthRegen=0;
    Expected.Strength=10; Expected.Agility=10; Expected.Wisdom=10; Expected.R33TestGrowth.bEnabled=false; Expected.InitialSkills.SetNum(2); Expected.InitialEquipment.Reset();
    TestTrue(TEXT("参考全部字段且非指定字段保持"),FY01CharacterRow::StaticStruct()->CompareScriptStruct(Character,&Expected,0));
    TestTrue(TEXT("自动普攻显式开启"),P->bR34AutoAttackEnabled);
    TestTrue(TEXT("额外角色行不变"),FY01CharacterRow::StaticStruct()->CompareScriptStruct(MigrationRow<FY01CharacterRow>(P,K::Character,TEXT("UserExtra")),&Extra,0));
    TestTrue(TEXT("旧怪Default完整保留"),FY01MonsterRow::StaticStruct()->CompareScriptStruct(MigrationRow<FY01MonsterRow>(P,K::Monster),&OriginalMonster,0));
    const auto* Dummy=MigrationRow<FY01MonsterRow>(P,K::Monster,TEXT("TrainingDummy"));
    const auto* Minion=MigrationRow<FY01MonsterRow>(P,K::Monster,TEXT("Wave1Minion"));
    if (!TestNotNull(TEXT("训练独立行"),Dummy) || !TestNotNull(TEXT("首波独立行"),Minion)) { return false; }
    for (int32 I=0;I<2;++I)
    {
        auto M=OriginalMonster; M.Health=I==0?70:100; M.AttackDamage=I==0?5:10; M.AttackInterval=I==0?2:1.5f;
        M.AttackType=EY01AttackType::Melee; M.Range=140; M.MeleeAngle=60; M.ProjectileSpeed=400; M.Armor=0; M.MoveSpeed=I==0?200:220; M.HealthRegen=0;
        TestTrue(TEXT("新怪仅指定属性变化，其他继承旧行"),FY01MonsterRow::StaticStruct()->CompareScriptStruct(I==0?Dummy:Minion,&M,0));
    }
    auto T=OriginalTraining; T.Spawns[0].Monster.RowName=TEXT("TrainingDummy");
    TestTrue(TEXT("训练2/3/6奖励等不变，仅改ref"),FY01TrainingStageRow::StaticStruct()->CompareScriptStruct(Training,&T,0));
    auto W=OriginalWave; W.Spawns[0].Monster.RowName=TEXT("Wave1Minion");
    TestTrue(TEXT("首波结构/奖励/准备时间不变"),FY01WaveRow::StaticStruct()->CompareScriptStruct(Wave,&W,0));
    const auto* Equipment=MigrationRow<FY01EquipmentRow>(P,K::Equipment,TEXT("EQ_0001"));
    if (!TestNotNull(TEXT("稳定RowName装备"),Equipment)) { return false; }
    TestEqual(TEXT("纯AttackBonus5"),Equipment->AttackBonus,5.f); TestTrue(TEXT("无旧Effect"),Equipment->Effect.IsNull()); TestEqual(TEXT("Green"),Equipment->Quality,EY01Quality::Green);
    if (!TestTrue(TEXT("R33快照磁盘存在"),ValidateSaved(History,H))) { return false; }
    TestTrue(TEXT("R33角色完整不变"),FY01CharacterRow::StaticStruct()->CompareScriptStruct(MigrationRow<FY01CharacterRow>(H.Profile.Get(),K::Character),&Original,0));
    for (auto* Old:{L.Profile.Get(),C.Profile.Get(),B.Profile.Get(),G.Profile.Get(),H.Profile.Get()})
    {
        for (K Kind:{K::Character,K::Monster,K::TrainingStage,K::Wave,K::Equipment})
        { TestTrue(TEXT("旧入口不再共享将改的主表"),Old->Tables[Kind]!=P->Tables[Kind]); }
    }
    TestEqual(TEXT("成长对照仍每秒2"),MigrationRow<FY01CharacterRow>(G.Profile.Get(),K::Character)->R33TestGrowth.AttackPerEffectiveSecond,2.);
    TestEqual(TEXT("旧200技能保留"),MigrationRow<FY01SkillRow>(C.Profile.Get(),K::Skill,TEXT("HeavyAttack"))->TestSkill.Speed,200.f);
    const auto After=All();
    for (const auto& File:Before)
    {
        const bool Allowed=File.Key.EndsWith(TEXT("DA_PlaygroundProfile.uasset")) || (MigrationFiles(Root).Contains(File.Key)
            && (File.Key.EndsWith(TEXT("DT_Character.uasset")) || File.Key.EndsWith(TEXT("DT_Monster.uasset")) || File.Key.EndsWith(TEXT("DT_TrainingStage.uasset"))
                || File.Key.EndsWith(TEXT("DT_Wave.uasset")) || File.Key.EndsWith(TEXT("DT_Equipment.uasset"))));
        if (!Allowed) { TestTrue(TEXT("非白名单包字节保留：")+File.Key,After.Contains(File.Key) && After[File.Key]==File.Value); }
    }
    if (Case==TEXT("PreserveR34UserEdits"))
    {
        Character->AttackDamage=47; Character->Health=456; P->bR34AutoAttackEnabled=false;
        MigrationRow<FY01EquipmentRow>(P,K::Equipment,TEXT("EQ_0001"))->AttackBonus=17;
        SaveMigrationFixture(P->Tables[K::Character]); SaveMigrationFixture(P->Tables[K::Equipment]); SaveMigrationFixture(P);
    }
    if (Case==TEXT("RejectR34HistoricalUpgrade"))
    {
        const auto BeforeOld=All(); AddExpectedError(TEXT("R34配置禁止旧升级"),EAutomationExpectedErrorFlags::Contains,3);
        TestEqual(TEXT("R34不允许旧技能升级"),Run(TEXT("-SkillUpgrade -LegacyRoot=")+Legacy),1);
        TestEqual(TEXT("R34不允许旧成长升级"),Run(TEXT("-GrowthUpgrade -LegacyRoot=")+Legacy+TEXT(" -CompareRoot=")+Compare+TEXT(" -GrowthCompareRoot=")+Growth),1);
        TestEqual(TEXT("R34不允许旧训练升级"),Run(TEXT("-TrainingUpgrade")),1);
        TestTrue(TEXT("旧命令拒绝零写"),SameMigrationFiles(BeforeOld,All())); return true;
    }
    const auto Guard=All(); TestEqual(TEXT("重复升级成功"),Run(Args),0); TestTrue(TEXT("重复含用户R34值零保存"),SameMigrationFiles(Guard,All()));
    if (Case==TEXT("FreshProcess"))
    {
        FString Text; int32 Exit=-1; TestTrue(TEXT("全新UE从磁盘重读迁移"),MigrationChild(Root,Args+TEXT(" -R34Audit"),Text,Exit));
        TestEqual(TEXT("新进程审计exit0"),Exit,0);
        TestTrue(TEXT("实际读盘含参考字段与三种行"),Text.Contains(TEXT("AttackDamage=30")) && Text.Contains(TEXT("TrainingDummy")) && Text.Contains(TEXT("Wave1Minion")) && Text.Contains(TEXT("AttackBonus=5")));
        TestTrue(TEXT("新进程零写"),SameMigrationFiles(Guard,All()));
    }
    if (Case==TEXT("CompareCopy") || Case==TEXT("RangedCopy") || Case==TEXT("CopyConflict"))
    {
        const FString CopyArgs=TEXT("-R34Copy -CopyRoot=")+Output+TEXT(" -Variant=")+(Case==TEXT("RangedCopy")?TEXT("Ranged"):TEXT("Compare"));
        if (Case==TEXT("CopyConflict"))
        {
            auto* Block=DuplicateObject<UDataTable>(P->Tables[K::Character],CreatePackage(*(Output+TEXT("/Data/DT_Character"))),TEXT("DT_Character")); SaveMigrationFixture(Block);
            const auto Conflict=All(); AddExpectedError(TEXT("R34_REJECT"),EAutomationExpectedErrorFlags::Contains,1);
            TestEqual(TEXT("对照占用不得覆盖"),Run(CopyArgs),1); TestTrue(TEXT("对照冲突零写"),SameMigrationFiles(Conflict,All())); return true;
        }
        TestEqual(TEXT("隔离对照落盘"),Run(CopyArgs),0);
        if (!TestTrue(TEXT("磁盘对照可读"),ValidateSaved(Output,O))) { return false; }
        for (K Kind:{K::Character,K::Monster,K::TrainingStage,K::Wave,K::Equipment}) { TestTrue(TEXT("对照表独立"),O.Profile->Tables[Kind]!=P->Tables[Kind]); }
        auto EC=*Character; auto ED=*Dummy; auto EM=*Minion; auto EE=*Equipment;
        if (Case==TEXT("RangedCopy"))
        { EC.AttackType=EY01AttackType::Ranged; EC.Range=500; EC.ProjectileSpeed=400; ED.AttackType=EM.AttackType=EY01AttackType::Ranged; ED.Range=EM.Range=500; ED.ProjectileSpeed=EM.ProjectileSpeed=400; }
        else { EC.AttackDamage=36; ED.AttackDamage=8; EE.AttackBonus=11; }
        TestTrue(TEXT("对照仅批准玩家字段"),FY01CharacterRow::StaticStruct()->CompareScriptStruct(MigrationRow<FY01CharacterRow>(O.Profile.Get(),K::Character),&EC,0));
        TestTrue(TEXT("对照仅批准训练怪字段"),FY01MonsterRow::StaticStruct()->CompareScriptStruct(MigrationRow<FY01MonsterRow>(O.Profile.Get(),K::Monster,TEXT("TrainingDummy")),&ED,0));
        TestTrue(TEXT("远程首波配置"),FY01MonsterRow::StaticStruct()->CompareScriptStruct(MigrationRow<FY01MonsterRow>(O.Profile.Get(),K::Monster,TEXT("Wave1Minion")),&EM,0));
        TestTrue(TEXT("对照仅批准装备字段"),FY01EquipmentRow::StaticStruct()->CompareScriptStruct(MigrationRow<FY01EquipmentRow>(O.Profile.Get(),K::Equipment,TEXT("EQ_0001")),&EE,0));
        const auto CopyGuard=All(); TestEqual(TEXT("对照重复幂等"),Run(CopyArgs),0); TestTrue(TEXT("对照零重存"),SameMigrationFiles(CopyGuard,All()));
    }
    TestTrue(TEXT("隔离测试未碰用户主资产"),SameMigrationFiles(Protected,MigrationFiles(TEXT("/Game/Y01Playground")))); return true;
}
#endif
