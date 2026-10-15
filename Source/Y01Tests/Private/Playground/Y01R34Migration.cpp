#include "Playground/Y01R34Migration.h"
#if WITH_EDITOR
#include "Playground/Y01PlaygroundGenerator.h"
#include "Playground/Y01PlaygroundMapBuilder.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogY01R34Migration,Log,All);
namespace
{
using K=EY01TableKind;
const TArray<K> ChangedKinds{K::Character,K::Monster,K::TrainingStage,K::Wave,K::Equipment};
const FString MainRoot=TEXT("/Game/Y01Playground");
const FString TestRoot=TEXT("/Game/Developers/Y01PlaygroundTests/");
FString ProfilePackage(const FString& Root) { return Root+TEXT("/DA_PlaygroundProfile"); }
FString ProfilePath(const FString& Root) { return ProfilePackage(Root)+TEXT(".DA_PlaygroundProfile"); }
FString TablePackage(const FString& Root,K Kind) { return Root+TEXT("/Data/DT_")+StaticEnum<K>()->GetNameStringByValue(static_cast<int64>(Kind)); }
FString MapPackage(const FString& Root) { return Root+TEXT("/Maps/L_Y01Playground"); }
FString DiskFile(const FString& Package)
{ return FPackageName::LongPackageNameToFilename(Package,Package.EndsWith(TEXT("/L_Y01Playground"))?FPackageName::GetMapPackageExtension():FPackageName::GetAssetPackageExtension()); }
bool ValidRoot(const FString& Root) { return FPackageName::IsValidLongPackageName(Root) && (Root==MainRoot || Root.StartsWith(TestRoot)); }
bool SeparateRoot(const FString& Root,const FString& Other)
{ return ValidRoot(Other) && Other!=MainRoot && Root!=Other && !Other.StartsWith(Root+TEXT("/")) && !Root.StartsWith(Other+TEXT("/")); }
bool EmptyOutput(const FString& Root)
{
    TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files,*FPackageName::LongPackageNameToFilename(Root),TEXT("*"),true,false);
    if (!Files.IsEmpty() || FindPackage(nullptr,*ProfilePackage(Root)) || FindPackage(nullptr,*MapPackage(Root))) { return false; }
    for (K Kind:ChangedKinds) { if (FindPackage(nullptr,*TablePackage(Root,Kind))) { return false; } } return true;
}
template<class T> T* Row(UY01PlaygroundProfile* P,K Kind,FName Name=TEXT("Default"))
{ return P->Tables.FindRef(Kind)->FindRow<T>(Name,TEXT("R34Migration"),false); }
bool LocalTables(UY01PlaygroundProfile* P,const FString& Root)
{
    for (K Kind:ChangedKinds)
    { const auto* T=P->Tables.FindRef(Kind).Get(); if (!T || T->GetOutermost()->GetName()!=TablePackage(Root,Kind)) { return false; } } return true;
}
bool Load(const FString& Root,FY01PlaygroundGenerationResult& R,FString& Error)
{
    if (!ValidRoot(Root) || !Y01PlaygroundEditor::ValidateSaved(Root,R))
    { Error=TEXT("持久配置无效：")+Root+TEXT(" ")+FString::Join(R.Errors,TEXT(";")); return false; }
    return Y01PlaygroundEditor::BuildMap(Root,R.Profile.Get(),true,Error);
}
void Audit(UY01PlaygroundProfile* P)
{
    UE_LOG(LogY01R34Migration,Display,TEXT("R34_AUDIT Profile=%s Version=%d Variant=%s Source=%s AutoAttack=%d Training=%d Character=%s"),
        *P->GetPathName(),P->R34AssetVersion,*P->R34Variant.ToString(),*P->R34SourceProfile,P->bR34AutoAttackEnabled,P->bTrainingEnabled,*P->Character.RowName.ToString());
    for (int32 I=0;I<P->OrderedWaves.Num();++I)
    { UE_LOG(LogY01R34Migration,Display,TEXT("R34_AUDIT WaveIndex=%d Type=%d Row=%s"),I,static_cast<int32>(P->OrderedWaves[I].Type),*P->OrderedWaves[I].RowName.ToString()); }
    for (const auto& T:P->Tables)
    {
        for (const auto& R:T.Value->GetRowMap())
        {
            FString Text; T.Value->GetRowStruct()->ExportText(Text,R.Value,nullptr,nullptr,PPF_None,nullptr);
            UE_LOG(LogY01R34Migration,Display,TEXT("R34_AUDIT Table=%s Row=%s Fields=%s"),*T.Value->GetPathName(),*R.Key.ToString(),*Text);
        }
    }
}
bool NoUserEditor()
{
#if PLATFORM_WINDOWS
    FPlatformProcess::FProcEnumerator Processes;
    while (Processes.MoveNext())
    {
        const auto P=Processes.GetCurrent();
        if (P.GetPID()!=FPlatformProcess::GetCurrentProcessId() && P.GetName().Equals(TEXT("UnrealEditor.exe"),ESearchCase::IgnoreCase)) { return false; }
    }
#endif
    return true;
}
// 只服务本次精准迁移：预先固定白名单，备份所有读写依赖，每次保存前重查磁盘和编辑器。
struct FSaveGuard
{
    TSet<FString> White;
    TMap<FString,FString> Hashes;
    FString Backup=FPaths::ProjectSavedDir()/TEXT("R34MigrationBackups")/FGuid::NewGuid().ToString(EGuidFormats::Digits);
    FString Manifest=TEXT("Package,BeforeMD5,AfterMD5\n");
    int32 Saved=0;
    static FString Hash(const FString& Package)
    { const FString File=DiskFile(Package); return IFileManager::Get().FileExists(*File)?LexToString(FMD5Hash::HashFile(*File)):TEXT("MISSING"); }
    void AddOutput(const FString& Root)
    { White.Add(ProfilePackage(Root)); White.Add(MapPackage(Root)); for (K Kind:ChangedKinds) { White.Add(TablePackage(Root,Kind)); } }
    void Watch(UY01PlaygroundProfile* P,const FString& Root)
    {
        Hashes.Add(P->GetOutermost()->GetName(),Hash(P->GetOutermost()->GetName())); Hashes.Add(MapPackage(Root),Hash(MapPackage(Root)));
        for (const auto& T:P->Tables) { const FString Name=T.Value->GetOutermost()->GetName(); Hashes.Add(Name,Hash(Name)); }
    }
    bool Begin()
    {
        if (!NoUserEditor()) { return false; }
        for (const auto& Name:White) { if (!Hashes.Contains(Name)) { Hashes.Add(Name,Hash(Name)); } }
        if (!IFileManager::Get().MakeDirectory(*Backup,true)) { return false; }
        FString Whitelist;
        for (const auto& Name:White) { Whitelist+=Name+TEXT("\n"); }
        if (!FFileHelper::SaveStringToFile(Whitelist,*(Backup/TEXT("whitelist.txt")))) { return false; }
        for (const auto& H:Hashes)
        {
            if (H.Value!=Hash(H.Key)) { return false; }
            Manifest+=H.Key+TEXT(",")+H.Value+TEXT(",\n");
            if (H.Value==TEXT("MISSING")) { continue; }
            const FString Dest=Backup/H.Key.Mid(6)+FPaths::GetExtension(DiskFile(H.Key),true);
            if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Dest),true) || IFileManager::Get().Copy(*Dest,*DiskFile(H.Key),false)!=COPY_OK) { return false; }
        }
        UE_LOG(LogY01R34Migration,Display,TEXT("R34_BACKUP Directory=%s Whitelist=%d Dependencies=%d"),*Backup,White.Num(),Hashes.Num());
        return FFileHelper::SaveStringToFile(Manifest,*(Backup/TEXT("hashes.csv")));
    }
    bool Check(const FString& Name) const
    {
        if (!White.Contains(Name) || !NoUserEditor()) { return false; }
        for (const auto& H:Hashes) { if (H.Value!=Hash(H.Key)) { return false; } } return true;
    }
    bool Record(const FString& Name)
    {
        const FString After=Hash(Name); Manifest+=Name+TEXT(",")+Hashes.FindRef(Name)+TEXT(",")+After+TEXT("\n"); Hashes.Add(Name,After); ++Saved;
        UE_LOG(LogY01R34Migration,Display,TEXT("R34_SAVE Package=%s MD5=%s"),*Name,*After);
        return FFileHelper::SaveStringToFile(Manifest,*(Backup/TEXT("hashes.csv")));
    }
    bool Save(UObject* A)
    {
        auto* Package=A->GetOutermost(); const FString Name=Package->GetName();
        if (!Check(Name)) { return false; }
        const FString File=DiskFile(Name); IFileManager::Get().MakeDirectory(*FPaths::GetPath(File),true);
        Package->FullyLoad(); ResetLoaders(Package); A->SetFlags(RF_Public|RF_Standalone); A->ClearFlags(RF_Transient); Package->MarkPackageDirty();
        FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone; Args.SaveFlags=SAVE_NoError;
        return UPackage::SavePackage(Package,A,*File,Args) && Record(Name);
    }
    bool Map(const FString& Root,UY01PlaygroundProfile* P,FString& Error)
    { return Check(MapPackage(Root)) && Y01PlaygroundEditor::BuildMap(Root,P,false,Error) && Record(MapPackage(Root)); }
};
UY01PlaygroundProfile* Candidate(UY01PlaygroundProfile* P)
{
    auto* C=DuplicateObject<UY01PlaygroundProfile>(P,GetTransientPackage(),MakeUniqueObjectName(GetTransientPackage(),P->GetClass()));
    for (K Kind:ChangedKinds)
        { C->Tables[Kind]=DuplicateObject<UDataTable>(P->Tables[Kind],GetTransientPackage(),MakeUniqueObjectName(GetTransientPackage(),UDataTable::StaticClass())); } return C;
}
bool SaveCopy(UY01PlaygroundProfile* P,const FString& Root,FSaveGuard& Guard,UY01PlaygroundProfile*& Out,FString& Error)
{
    Out=DuplicateObject<UY01PlaygroundProfile>(P,CreatePackage(*ProfilePackage(Root)),TEXT("DA_PlaygroundProfile"));
    for (K Kind:ChangedKinds)
    {
        const FString Name=TablePackage(Root,Kind);
        auto* T=DuplicateObject<UDataTable>(P->Tables[Kind],CreatePackage(*Name),*FPackageName::GetLongPackageAssetName(Name));
        Out->Tables[Kind]=T; if (!Guard.Save(T)) { return false; }
    }
    return Out->Validate().IsValid() && Guard.Save(Out) && Guard.Map(Root,Out,Error);
}
bool OldFingerprint(UY01PlaygroundProfile* P)
{
    const auto* C=Row<FY01CharacterRow>(P,K::Character,P->Character.RowName);
    if (!C || P->Character.RowName!=TEXT("Default") || P->bR34AutoAttackEnabled || !P->bTrainingEnabled || P->R34AssetVersion!=0
        || C->Health!=200 || C->AttackDamage!=20 || C->AttackInterval!=1 || C->AttackType!=EY01AttackType::Melee || C->Range!=500
        || C->MeleeAngle!=90 || C->ProjectileSpeed!=400 || C->Armor!=0 || C->MoveSpeed!=500 || C->HealthRegen!=0
        || C->Strength!=0 || C->Agility!=0 || C->Wisdom!=0 || !C->R33TestGrowth.bEnabled || C->R33TestGrowth.AttackPerEffectiveSecond!=1
        || C->R33TestGrowth.AttackPerEffectiveKill!=2 || C->R33TestGrowth.GoldPerEffectiveSecond!=1 || C->InitialSkills.Num()!=3 || C->InitialEquipment.Num()!=6) { return false; }
    const FName Names[]{TEXT("BasicAttack"),TEXT("HeavyAttack"),TEXT("Fortitude")};
    for (int32 I=0;I<3;++I) { if (C->InitialSkills[I].Type!=K::Skill || C->InitialSkills[I].RowName!=Names[I]) { return false; } }
    for (const auto& E:C->InitialEquipment) { if (E.Type!=K::Equipment || E.RowName!=TEXT("Default")) { return false; } }
    const auto* Passive=Row<FY01SkillRow>(P,K::Skill,TEXT("Fortitude"));
    return Passive && Passive->Type==EY01SkillType::Passive && Passive->Effect.ToSoftObjectPath().ToString()==TEXT("/Script/Y01Tests.Y01PlaygroundPassiveDamageEffect");
}
bool ApplyReference(UY01PlaygroundProfile* P,FString& Error)
{
    auto* C=Row<FY01CharacterRow>(P,K::Character,P->Character.RowName);
    auto* T=Row<FY01TrainingStageRow>(P,K::TrainingStage);
    auto* W=Row<FY01WaveRow>(P,K::Wave,P->OrderedWaves[0].RowName);
    const auto* M=Row<FY01MonsterRow>(P,K::Monster);
    if (!OldFingerprint(P) || !M || !T || !W || T->Spawns.Num()!=1 || W->Spawns.Num()!=1
        || T->Spawns[0].Monster.Type!=K::Monster || T->Spawns[0].Monster.RowName!=TEXT("Default")
        || W->Spawns[0].Monster.Type!=K::Monster || W->Spawns[0].Monster.RowName!=TEXT("Default")
        || Row<FY01MonsterRow>(P,K::Monster,TEXT("TrainingDummy")) || Row<FY01MonsterRow>(P,K::Monster,TEXT("Wave1Minion"))
        || Row<FY01EquipmentRow>(P,K::Equipment,TEXT("EQ_0001"))) { Error=TEXT("目标行旧指纹、出生引用或新增RowName冲突；未写任何包"); return false; }
    C->Health=300; C->AttackDamage=30; C->AttackInterval=1; C->AttackType=EY01AttackType::Melee; C->Range=200;
    C->MeleeAngle=90; C->ProjectileSpeed=400; C->Armor=0; C->MoveSpeed=500; C->HealthRegen=0; C->Strength=C->Agility=C->Wisdom=10;
    C->R33TestGrowth.bEnabled=false; C->InitialSkills.SetNum(2); C->InitialEquipment.Reset();
    for (int32 I=0;I<2;++I)
    {
        auto New=*M; New.Health=I==0?70:100; New.AttackDamage=I==0?5:10; New.AttackInterval=I==0?2:1.5f;
        New.AttackType=EY01AttackType::Melee; New.Range=140; New.MeleeAngle=60; New.ProjectileSpeed=400; New.Armor=0; New.MoveSpeed=I==0?200:220; New.HealthRegen=0;
        P->Tables[K::Monster]->AddRow(I==0?TEXT("TrainingDummy"):TEXT("Wave1Minion"),New);
    }
    T->Spawns[0].Monster.RowName=TEXT("TrainingDummy"); W->Spawns[0].Monster.RowName=TEXT("Wave1Minion");
    FY01EquipmentRow Equipment; Equipment.AttackBonus=5; Equipment.Quality=EY01Quality::Green;
    P->Tables[K::Equipment]->AddRow(TEXT("EQ_0001"),Equipment); P->bR34AutoAttackEnabled=true; P->R34AssetVersion=1; P->R34Variant=TEXT("Main");
    return P->Validate().IsValid();
}
}
int32 Y01PlaygroundEditor::RunR34Migration(const FString& Root,const FString& Params)
{
    auto Reject=[](const FString& E) { UE_LOG(LogY01R34Migration,Error,TEXT("R34_REJECT %s"),*E); return 1; };
    FY01PlaygroundGenerationResult R; FString Error;
    if (!Load(Root,R,Error)) { return Reject(Error); }
    auto* P=R.Profile.Get(); Audit(P);
    if (FParse::Param(*Params,TEXT("ValidateOnly")) || FParse::Param(*Params,TEXT("R34Audit")))
    { UE_LOG(LogY01R34Migration,Display,TEXT("R34_RESULT ReadOnly=1 Saved=0")); return 0; }
    if (!LocalTables(P,Root)) { return Reject(TEXT("源五表必须位于自身根，禁止回写共享表")); }
    if (FParse::Param(*Params,TEXT("R34Copy")))
    {
        FString Output,Variant; FParse::Value(*Params,TEXT("CopyRoot="),Output); FParse::Value(*Params,TEXT("Variant="),Variant);
        if (P->R34AssetVersion!=1 || !SeparateRoot(Root,Output) || (Variant!=TEXT("Compare") && Variant!=TEXT("Ranged"))) { return Reject(TEXT("对照源、根或Variant非法")); }
        FY01PlaygroundGenerationResult Existing;
        if (FPackageName::DoesPackageExist(ProfilePackage(Output)))
        {
            if (!Load(Output,Existing,Error) || !LocalTables(Existing.Profile.Get(),Output) || Existing.Profile->R34AssetVersion!=1
                || Existing.Profile->R34Variant!=FName(*Variant) || Existing.Profile->R34SourceProfile!=P->GetPathName()) { return Reject(TEXT("已有输出身份冲突")); }
            Audit(Existing.Profile.Get()); UE_LOG(LogY01R34Migration,Display,TEXT("R34_RESULT Preserved=1 Saved=0")); return 0;
        }
        if (!EmptyOutput(Output)) { return Reject(TEXT("对照输出已占用，拒绝半套覆盖")); }
        TStrongObjectPtr<UY01PlaygroundProfile> Copy(Candidate(P));
        auto* C=Row<FY01CharacterRow>(Copy.Get(),K::Character,Copy->Character.RowName);
        auto* D=Row<FY01MonsterRow>(Copy.Get(),K::Monster,TEXT("TrainingDummy")); auto* M=Row<FY01MonsterRow>(Copy.Get(),K::Monster,TEXT("Wave1Minion"));
        auto* E=Row<FY01EquipmentRow>(Copy.Get(),K::Equipment,TEXT("EQ_0001"));
        if (!C || !D || !M || !E) { return Reject(TEXT("缺少R34参考行")); }
        if (Variant==TEXT("Compare"))
        {
            if (C->AttackDamage!=30 || D->AttackDamage!=5 || E->AttackBonus!=5) { return Reject(TEXT("对照源已改值，不盲目覆盖为36/8/11")); }
            C->AttackDamage=36; D->AttackDamage=8; E->AttackBonus=11;
        }
        else { C->AttackType=D->AttackType=M->AttackType=EY01AttackType::Ranged; C->Range=D->Range=M->Range=500; C->ProjectileSpeed=D->ProjectileSpeed=M->ProjectileSpeed=400; }
        Copy->R34SourceProfile=P->GetPathName(); Copy->R34Variant=*Variant;
        if (!Copy->Validate().IsValid()) { return Reject(TEXT("候选对照无效")); }
        FSaveGuard Guard; Guard.AddOutput(Output); Guard.Watch(P,Root);
        if (!Guard.Begin()) { return Reject(TEXT("备份/hash或用户编辑器检查失败；未保存")); }
        UY01PlaygroundProfile* Saved=nullptr;
        if (!SaveCopy(Copy.Get(),Output,Guard,Saved,Error)) { return Reject(TEXT("对照保存失败：")+Error+TEXT(" 备份=")+Guard.Backup); }
        Audit(Saved); UE_LOG(LogY01R34Migration,Display,TEXT("R34_RESULT Saved=%d"),Guard.Saved); return 0;
    }
    FString History=TestRoot+TEXT("R33Growth"),Isolation;
    const TArray<FString> Approved{TestRoot+TEXT("R33U2Legacy"),TestRoot+TEXT("R33U2Compare"),TestRoot+TEXT("R33U2Compare/Baseline"),TestRoot+TEXT("R33U3GrowthCompare"),TestRoot+TEXT("R33TrainingCompare")};
    FParse::Value(*Params,TEXT("HistoryRoot="),History); FParse::Value(*Params,TEXT("IsolationRoots="),Isolation,false);
    TArray<FString> Roots; if (Isolation.IsEmpty() && Root==MainRoot) { Roots=Approved; } else { Isolation.ParseIntoArray(Roots,TEXT(","),true); }
    if (!SeparateRoot(Root,History) || Roots.IsEmpty() || (Root==MainRoot && (History!=TestRoot+TEXT("R33Growth") || Roots!=Approved))) { return Reject(TEXT("历史根或隔离清单不是批准入口")); }
    TArray<TStrongObjectPtr<UY01PlaygroundProfile>> Old;
    for (int32 I=0;I<Roots.Num();++I)
    {
        FY01PlaygroundGenerationResult O;
        if (!SeparateRoot(Root,Roots[I]) || !SeparateRoot(History,Roots[I]) || Roots.Find(Roots[I])!=I || !Load(Roots[I],O,Error)) { return Reject(TEXT("旧入口不可用：")+Roots[I]+TEXT(" ")+Error); }
        Old.Add(TStrongObjectPtr<UY01PlaygroundProfile>(O.Profile.Get()));
    }
    if (P->R34AssetVersion==1)
    {
        FY01PlaygroundGenerationResult H;
        if (P->R34Variant!=TEXT("Main") || P->R34SourceProfile!=ProfilePath(History) || !Load(History,H,Error) || !LocalTables(H.Profile.Get(),History)) { return Reject(TEXT("迁移身份或历史快照不完整")); }
        for (const auto& O:Old) { for (K Kind:ChangedKinds) { if (O->Tables[Kind]==P->Tables[Kind]) { return Reject(TEXT("历史入口重新共享主表，拒绝隐式修复")); } } }
        UE_LOG(LogY01R34Migration,Display,TEXT("R34_RESULT Preserved=1 Saved=0")); return 0;
    }
    if (!EmptyOutput(History)) { return Reject(TEXT("历史快照输出已占用，拒绝覆盖")); }
    TStrongObjectPtr<UY01PlaygroundProfile> Desired(Candidate(P));
    if (!ApplyReference(Desired.Get(),Error)) { return Reject(Error); }
    Desired->R34SourceProfile=ProfilePath(History);
    FSaveGuard Guard; Guard.White.Add(ProfilePackage(Root)); for (K Kind:ChangedKinds) { Guard.White.Add(TablePackage(Root,Kind)); }
    Guard.AddOutput(History); Guard.Watch(P,Root);
    for (int32 I=0;I<Old.Num();++I)
    {
        Guard.Watch(Old[I].Get(),Roots[I]);
        bool Shared=false; for (K Kind:ChangedKinds) { Shared|=Old[I]->Tables[Kind]==P->Tables[Kind]; }
        if (Shared) { Guard.White.Add(ProfilePackage(Roots[I])); }
    }
    if (!Guard.Begin()) { return Reject(TEXT("备份/hash或用户编辑器检查失败；未保存")); }
    UY01PlaygroundProfile* Historical=nullptr;
    if (!SaveCopy(P,History,Guard,Historical,Error)) { return Reject(TEXT("历史快照保存失败，主未写：")+Error+TEXT(" 备份=")+Guard.Backup); }
    // 历史引用先切断并落盘，主表最后更新；已有独立Character/训练表不复制、不修改。
    for (const auto& O:Old)
    {
        bool Changed=false;
        for (K Kind:ChangedKinds) { if (O->Tables[Kind]==P->Tables[Kind]) { O->Tables[Kind]=Historical->Tables[Kind]; Changed=true; } }
        if (Changed && (!O->Validate().IsValid() || !Guard.Save(O.Get()))) { return Reject(TEXT("旧入口隔离保存失败，主未写；备份=")+Guard.Backup); }
    }
    for (K Kind:ChangedKinds)
    {
        auto* Target=P->Tables[Kind].Get(); auto* Source=Desired->Tables[Kind].Get();
        for (const auto& Rw:Source->GetRowMap())
        {
            if (auto* Existing=Target->FindRowUnchecked(Rw.Key)) { Target->GetRowStruct()->CopyScriptStruct(Existing,Rw.Value); }
            else { Target->AddRow(Rw.Key,Rw.Value,Target->GetRowStruct()); }
        }
        if (!Guard.Save(Target)) { return Reject(TEXT("主表保存失败；保留备份供恢复：")+Guard.Backup); }
    }
    P->bR34AutoAttackEnabled=Desired->bR34AutoAttackEnabled; P->R34AssetVersion=1; P->R34SourceProfile=Desired->R34SourceProfile; P->R34Variant=Desired->R34Variant;
    if (!P->Validate().IsValid() || !Guard.Save(P)) { return Reject(TEXT("主Profile保存失败；备份=")+Guard.Backup); }
    Audit(P); UE_LOG(LogY01R34Migration,Display,TEXT("R34_RESULT Saved=%d MainMapPreserved=1"),Guard.Saved); return 0;
}
#endif
