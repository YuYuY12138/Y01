#include "Playground/Y01CreatePlaygroundCommandlet.h"
#include "Misc/Parse.h"
#if WITH_EDITOR
#include "Playground/Y01PlaygroundGenerator.h"
#include "Playground/Y01PlaygroundMapBuilder.h"
#include "Playground/Y01TestSkill.h"
#include "Playground/Y01R34Migration.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "UObject/SavePackage.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogY01Playground, Log, All);

#if WITH_EDITOR
namespace
{
bool SkillRoot(const FString& Root)
{
    return FPackageName::IsValidLongPackageName(Root) && (Root == TEXT("/Game/Y01Playground")
        || Root.StartsWith(TEXT("/Game/Developers/Y01PlaygroundTests/")));
}
// R34主Profile显式指回的R33快照可严验旧契约，但不能借共享技能表回写主资产。
bool R33History(UY01PlaygroundProfile* P, const FString& Root)
{
    if (Root == TEXT("/Game/Y01Playground") || !SkillRoot(Root) || P->R34AssetVersion != 0) { return false; }
    for (auto Kind : {EY01TableKind::Character, EY01TableKind::Monster, EY01TableKind::TrainingStage, EY01TableKind::Wave, EY01TableKind::Equipment})
    {
        auto* Table = P->Tables.FindRef(Kind).Get();
        if (!Table || Table->GetOutermost()->GetName() != Root + TEXT("/Data/DT_") + StaticEnum<EY01TableKind>()->GetNameStringByValue(static_cast<int64>(Kind))) { return false; }
    }
    auto* Skill = P->Tables.FindRef(EY01TableKind::Skill).Get();
    const FString SourceRoot = FPackageName::GetLongPackagePath(FPackageName::GetLongPackagePath(Skill->GetOutermost()->GetName()));
    FY01PlaygroundGenerationResult Source;
    return SkillRoot(SourceRoot) && Y01PlaygroundEditor::ValidateSaved(SourceRoot, Source)
        && Source.Profile->R34AssetVersion == 1 && Source.Profile->R34Variant == TEXT("Main")
        && Source.Profile->R34SourceProfile == P->GetPathName() && Source.Profile->Tables.FindRef(EY01TableKind::Skill) == Skill;
}
bool SkillRows(UY01PlaygroundProfile* P, TArray<FName>& Names)
{
    FY01DataValidation V;
    const auto* C = P->BuildCatalog().FindRow<FY01CharacterRow>(P->Character, V);
    if (!C || C->InitialSkills.Num() < 2) { return false; }
    for (int32 I = 0; I < 2; ++I)
    {
        if (C->InitialSkills[I].Type != EY01TableKind::Skill) { return false; }
        Names.Add(C->InitialSkills[I].RowName);
    }
    // 这是精准迁移的旧资产指纹，不参与运行时技能行为分派。
    return Names[0] == TEXT("BasicAttack") && Names[1] == TEXT("HeavyAttack");
}
FY01SkillRow UpgradedSkill(FY01SkillRow Row, int32 Index)
{
    Row.Ability = UY01TestSkillAbility::StaticClass(); Row.TestSkill = FY01TestSkillConfig();
    auto& C = Row.TestSkill; C.DamageCoefficient = 1;
    if (Index == 0)
    { C.Mode = EY01TestSkillMode::Area; C.Radius = 250; C.HalfHeight = 120; C.ActiveSeconds = 0.35f; Row.CooldownSeconds = 1.5f; }
    else
    { C.Mode = EY01TestSkillMode::Projectile; C.Radius = 12; C.TargetRange = 800; C.Speed = 400; C.Lifetime = 2.5f; C.HitFeedbackSeconds = 0.1f; }
    return Row;
}
bool SameSkill(const FY01SkillRow* A, const FY01SkillRow* B)
{ return A && B && FY01SkillRow::StaticStruct()->CompareScriptStruct(A, B, 0); }
bool LegacySkills(UDataTable* Table, const TArray<FName>& Names)
{
    const FY01TestSkillConfig Empty;
    for (FName Name : Names)
    {
        const auto* R = Table->FindRow<FY01SkillRow>(Name, TEXT("SkillLegacy"));
        if (!R || R->Type != EY01SkillType::Active || R->Ability.ToSoftObjectPath() != FSoftObjectPath(UY01BasicAttackAbility::StaticClass())
            || !FY01TestSkillConfig::StaticStruct()->CompareScriptStruct(&R->TestSkill, &Empty, 0)) { return false; }
    }
    return true;
}
FY01R33TestGrowth ApprovedGrowth(double Seconds)
{
    FY01R33TestGrowth G; G.bEnabled = true; G.AttackPerEffectiveSecond = Seconds;
    G.AttackPerEffectiveKill = 2; G.GoldPerEffectiveSecond = 1; return G;
}
bool SameGrowth(const FY01R33TestGrowth& A, const FY01R33TestGrowth& B)
{ return FY01R33TestGrowth::StaticStruct()->CompareScriptStruct(&A, &B, 0); }
bool CharacterSnapshot(UDataTable* Source, UDataTable* Other, FName Target, const FY01R33TestGrowth& Growth)
{
    if (!Source || !Other || Other->GetRowStruct() != FY01CharacterRow::StaticStruct()
        || Source->GetRowNames().Num() != Other->GetRowNames().Num()) { return false; }
    for (FName Name : Source->GetRowNames())
    {
        const auto* A = Source->FindRow<FY01CharacterRow>(Name, TEXT("GrowthSnapshot"));
        const auto* B = Other->FindRow<FY01CharacterRow>(Name, TEXT("GrowthSnapshot"));
        if (!A || !B) { return false; }
        auto Expected = *A; if (Name == Target) { Expected.R33TestGrowth = Growth; }
        if (!FY01CharacterRow::StaticStruct()->CompareScriptStruct(&Expected, B, 0)) { return false; }
    }
    return true;
}
bool ProfileShape(UY01PlaygroundProfile* A, UY01PlaygroundProfile* B)
{
    if (A->Character.Type != B->Character.Type || A->Character.RowName != B->Character.RowName
        || A->OrderedWaves.Num() != B->OrderedWaves.Num() || A->bTrainingEnabled != B->bTrainingEnabled
        || A->Tables.Num() != B->Tables.Num()) { return false; }
    for (int32 I = 0; I < A->OrderedWaves.Num(); ++I)
    { if (A->OrderedWaves[I].Type != B->OrderedWaves[I].Type || A->OrderedWaves[I].RowName != B->OrderedWaves[I].RowName) { return false; } }
    return true;
}
bool SharedProfile(UY01PlaygroundProfile* A, UY01PlaygroundProfile* B)
{
    if (!ProfileShape(A, B)) { return false; }
    for (const auto& Pair : A->Tables)
    {
        if (Pair.Key == EY01TableKind::Skill || B->Tables.FindRef(Pair.Key) == Pair.Value) { continue; }
        auto* Other = B->Tables.FindRef(Pair.Key).Get();
        const FString Root = FPackageName::GetLongPackagePath(B->GetOutermost()->GetName());
        if (Pair.Key != EY01TableKind::Character || !Root.StartsWith(TEXT("/Game/Developers/Y01PlaygroundTests/"))
            || !Other || Other->GetOutermost()->GetName() != Root + TEXT("/Data/DT_Character")
            || !FPackageName::DoesPackageExist(Other->GetOutermost()->GetName())
            || !CharacterSnapshot(Pair.Value, Other, A->Character.RowName, FY01R33TestGrowth())) { return false; }
    }
    return true;
}
void AuditSkills(UY01PlaygroundProfile* P, const TCHAR* Stage)
{
    auto* Table = P->Tables.FindRef(EY01TableKind::Skill).Get();
    UE_LOG(LogY01Playground, Display, TEXT("SKILL_AUDIT Stage=%s Profile=%s Character=%s Table=%s Training=%d TestConfig=1"),
        Stage, *P->GetPathName(), *P->Character.RowName.ToString(), *Table->GetPathName(), P->bTrainingEnabled);
    for (FName Name : Table->GetRowNames())
    {
        const auto* R = Table->FindRow<FY01SkillRow>(Name, TEXT("SkillAudit")); const auto& C = R->TestSkill;
        UE_LOG(LogY01Playground, Display, TEXT("SKILL_AUDIT Stage=%s Row=%s Class=%s Type=%d CD=%g Mode=%d Coefficient=%g Radius=%g HalfHeight=%g ActiveSeconds=%g TargetRange=%g Speed=%g Lifetime=%g Feedback=%g"),
            Stage, *Name.ToString(), *R->Ability.ToSoftObjectPath().ToString(), static_cast<int32>(R->Type), R->CooldownSeconds,
            static_cast<int32>(C.Mode), C.DamageCoefficient, C.Radius, C.HalfHeight, C.ActiveSeconds, C.TargetRange, C.Speed, C.Lifetime, C.HitFeedbackSeconds);
    }
}
bool SaveSkillAsset(UObject* Asset)
{
    auto* Package = Asset->GetOutermost(); Package->FullyLoad(); ResetLoaders(Package);
    Asset->SetFlags(RF_Public | RF_Standalone); Asset->ClearFlags(RF_Transient); Package->MarkPackageDirty();
    const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    return UPackage::SavePackage(Package, Asset, *File, Args);
}
int32 RunSkills(const FString& Root, const FString& Params)
{
    auto Reject = [](const FString& Reason) { UE_LOG(LogY01Playground, Error, TEXT("SKILL_REJECT %s"), *Reason); return 1; };
    FY01PlaygroundGenerationResult Loaded;
    if (!SkillRoot(Root) || !Y01PlaygroundEditor::ValidateSaved(Root, Loaded)) { return Reject(TEXT("Root/Profile未通过持久校验")); }
    auto* P = Loaded.Profile.Get(); auto* Table = P->Tables.FindRef(EY01TableKind::Skill).Get();
    AuditSkills(P, TEXT("BEFORE"));
    if (FParse::Param(*Params, TEXT("SkillAudit")) || FParse::Param(*Params, TEXT("ValidateOnly"))) { return 0; }
    if (P->R34AssetVersion != 0) { return Reject(TEXT("R34配置禁止旧升级；请显式选择r33growth历史根")); }
    const bool History = R33History(P, Root);
    if (!History && Table->GetOutermost()->GetName() != Root + TEXT("/Data/DT_Skill")) { return Reject(TEXT("拒绝回写共享或非批准技能表")); }
    TArray<FName> Names;
    if (!SkillRows(P, Names)) { return Reject(TEXT("角色授予引用与批准迁移指纹不符")); }
    const bool bLegacy = FParse::Param(*Params, TEXT("SkillLegacyCopy"));
    const bool bCompare = FParse::Param(*Params, TEXT("SkillCompareCopy"));
    if (bLegacy || bCompare)
    {
        FString CopyRoot; FParse::Value(*Params, TEXT("CopyRoot="), CopyRoot);
        if (!SkillRoot(CopyRoot) || CopyRoot == TEXT("/Game/Y01Playground") || CopyRoot == Root
            || CopyRoot.StartsWith(Root + TEXT("/")) || Root.StartsWith(CopyRoot + TEXT("/"))) { return Reject(TEXT("隔离输出目录非法")); }
        const FString PP = CopyRoot + TEXT("/DA_PlaygroundProfile"), TP = CopyRoot + TEXT("/Data/DT_Skill"), MP = CopyRoot + TEXT("/Maps/L_Y01Playground");
        FY01PlaygroundGenerationResult Existing;
        if (FPackageName::DoesPackageExist(PP))
        {
            if (!Y01PlaygroundEditor::ValidateSaved(CopyRoot, Existing) || !SharedProfile(P, Existing.Profile.Get())
                || Existing.Profile->Tables.FindRef(EY01TableKind::Skill)->GetOutermost()->GetName() != TP)
            { return Reject(TEXT("现有副本不是同源隔离配置，拒绝覆盖")); }
            auto* Other = Existing.Profile->Tables.FindRef(EY01TableKind::Skill).Get();
            if (bLegacy && !LegacySkills(Other, Names)) { return Reject(TEXT("现有legacy并非旧即时配置")); }
            if (Other->GetRowNames().Num() != Table->GetRowNames().Num()) { return Reject(TEXT("现有副本行集合不同")); }
            for (FName Name : Table->GetRowNames())
            {
                const auto* A = Table->FindRow<FY01SkillRow>(Name, TEXT("SkillCopy"));
                const auto* B = Other->FindRow<FY01SkillRow>(Name, TEXT("SkillCopy"));
                if (!A || !B) { return Reject(TEXT("现有副本缺行")); }
                const int32 Index = Names.IndexOfByKey(Name);
                FY01SkillRow Expected = bLegacy ? *B : *A;
                if (bLegacy && Index != INDEX_NONE && !SameSkill(A, B)) { Expected = UpgradedSkill(*B, Index); }
                if (bCompare && Index == 1) { Expected.TestSkill.Speed = 200; }
                if (!SameSkill(bLegacy ? A : B, &Expected)) { return Reject(TEXT("现有副本与源不同，拒绝覆盖")); }
            }
            FString Error;
            if (!Y01PlaygroundEditor::BuildMap(CopyRoot, Existing.Profile.Get(), true, Error)) { return Reject(Error); }
            AuditSkills(Existing.Profile.Get(), TEXT("PRESERVED")); return 0;
        }
        if (History) { return Reject(TEXT("历史入口仅严验已存在副本，不创建或回写共享表")); }
        if (FPackageName::DoesPackageExist(TP) || FPackageName::DoesPackageExist(MP)
            || FindPackage(nullptr, *PP) || FindPackage(nullptr, *TP) || FindPackage(nullptr, *MP))
        { return Reject(TEXT("输出存在半套包，不覆盖")); }
        if (bLegacy && !LegacySkills(Table, Names)) { return Reject(TEXT("必须在升级前保存旧即时legacy")); }
        if (bCompare)
        {
            for (int32 I = 0; I < 2; ++I)
            {
                const auto* R = Table->FindRow<FY01SkillRow>(Names[I], TEXT("SkillCompare"));
                if (!R) { return Reject(TEXT("compare源缺行")); }
                const auto Expected = UpgradedSkill(*R, I);
                if (!SameSkill(R, &Expected)) { return Reject(TEXT("compare源不是批准的新技能配置")); }
            }
        }
        auto* Copy = DuplicateObject<UY01PlaygroundProfile>(P, CreatePackage(*PP), TEXT("DA_PlaygroundProfile"));
        auto* CopyTable = DuplicateObject<UDataTable>(Table, CreatePackage(*TP), TEXT("DT_Skill"));
        Copy->Tables[EY01TableKind::Skill] = CopyTable;
        if (bCompare) { CopyTable->FindRow<FY01SkillRow>(Names[1], TEXT("SkillCompare"))->TestSkill.Speed = 200; }
        if (!Copy->Validate().IsValid() || !SaveSkillAsset(CopyTable) || !SaveSkillAsset(Copy)) { return Reject(TEXT("隔离技能表/Profile保存失败")); }
        FString Error;
        if (!Y01PlaygroundEditor::BuildMap(CopyRoot, Copy, false, Error)) { return Reject(Error); }
        AuditSkills(Copy, TEXT("SAVED_COPY")); return 0;
    }
    FString LegacyRoot; FParse::Value(*Params, TEXT("LegacyRoot="), LegacyRoot);
    FY01PlaygroundGenerationResult Legacy;
    if (!SkillRoot(LegacyRoot) || LegacyRoot == Root || !LegacyRoot.StartsWith(TEXT("/Game/Developers/Y01PlaygroundTests/"))
        || !Y01PlaygroundEditor::ValidateSaved(LegacyRoot, Legacy) || !SharedProfile(P, Legacy.Profile.Get()))
    { return Reject(TEXT("升级要求已保存且同源的隔离legacy Profile/技能表/地图")); }
    auto* OldTable = Legacy.Profile->Tables.FindRef(EY01TableKind::Skill).Get(); FString MapError;
    if (OldTable->GetOutermost()->GetName() != LegacyRoot + TEXT("/Data/DT_Skill") || !LegacySkills(OldTable, Names)
        || !Y01PlaygroundEditor::BuildMap(LegacyRoot, Legacy.Profile.Get(), true, MapError)
        || Table->GetRowNames().Num() != OldTable->GetRowNames().Num()) { return Reject(TEXT("legacy指纹或地图无效")); }
    TArray<FY01SkillRow> Desired; bool bChanged = false;
    for (int32 I = 0; I < 2; ++I) { Desired.Add(UpgradedSkill(*OldTable->FindRow<FY01SkillRow>(Names[I], TEXT("SkillUpgrade")), I)); }
    for (FName Name : Table->GetRowNames())
    {
        const auto* Current = Table->FindRow<FY01SkillRow>(Name, TEXT("SkillUpgrade"));
        const auto* Old = OldTable->FindRow<FY01SkillRow>(Name, TEXT("SkillUpgrade"));
        const int32 I = Names.IndexOfByKey(Name);
        if (SameSkill(Current, Old)) { if (I != INDEX_NONE) { bChanged = true; } continue; }
        if (I == INDEX_NONE || !SameSkill(Current, &Desired[I])) { return Reject(TEXT("未知用户类/配置与legacy不符，保留全部包")); }
    }
    if (!bChanged) { AuditSkills(P, TEXT("PRESERVED")); return 0; }
    if (History) { return Reject(TEXT("历史入口不得回写共享主技能表")); }
    TArray<FY01SkillRow> Before;
    for (int32 I = 0; I < 2; ++I) { auto* R = Table->FindRow<FY01SkillRow>(Names[I], TEXT("SkillUpgrade")); Before.Add(*R); *R = Desired[I]; }
    if (!P->Validate().IsValid() || !SaveSkillAsset(Table))
    {
        for (int32 I = 0; I < 2; ++I) { *Table->FindRow<FY01SkillRow>(Names[I], TEXT("SkillUpgrade")) = Before[I]; }
        return Reject(TEXT("精准技能表验证/保存失败"));
    }
    AuditSkills(P, TEXT("SAVED_UPGRADE")); return 0;
}
void AuditGrowth(UY01PlaygroundProfile* P, const TCHAR* Stage)
{
    auto* Table = P->Tables.FindRef(EY01TableKind::Character).Get();
    for (FName Name : Table->GetRowNames())
    {
        const auto* R = Table->FindRow<FY01CharacterRow>(Name, TEXT("GrowthAudit")); const auto& G = R->R33TestGrowth;
        UE_LOG(LogY01Playground, Display, TEXT("GROWTH_AUDIT Stage=%s Profile=%s Character=%s Table=%s Row=%s Enabled=%d Seconds=%g Kills=%g Gold=%g HP=%g Attack=%g MoveSpeed=%g TestOnly=1"),
            Stage, *P->GetPathName(), *P->Character.RowName.ToString(), *Table->GetPathName(), *Name.ToString(), G.bEnabled,
            G.AttackPerEffectiveSecond, G.AttackPerEffectiveKill, G.GoldPerEffectiveSecond, R->Health, R->AttackDamage, R->MoveSpeed);
    }
}
int32 RunGrowth(const FString& Root, const FString& Params)
{
    using namespace Y01PlaygroundEditor;
    auto Reject = [](const FString& E) { UE_LOG(LogY01Playground, Error, TEXT("GROWTH_REJECT %s"), *E); return 1; };
    FY01PlaygroundGenerationResult R, L, C, B, G; FString Error;
    if (!SkillRoot(Root) || !ValidateSaved(Root, R)) { return Reject(TEXT("源Profile未通过真实持久校验")); }
    auto* P = R.Profile.Get(); AuditGrowth(P, TEXT("BEFORE"));
    if (FParse::Param(*Params, TEXT("GrowthAudit")) || FParse::Param(*Params, TEXT("ValidateOnly"))) { return 0; }
    if (P->R34AssetVersion != 0) { return Reject(TEXT("R34配置禁止旧升级；请显式选择r33growth历史根")); }
    const bool History = R33History(P, Root);
    FString Legacy, Compare, Growth;
    FParse::Value(*Params, TEXT("LegacyRoot="), Legacy); FParse::Value(*Params, TEXT("CompareRoot="), Compare);
    FParse::Value(*Params, TEXT("GrowthCompareRoot="), Growth);
    const TArray<FString> Roots{Root, Legacy, Compare, Growth};
    for (int32 I = 0; I < Roots.Num(); ++I)
    {
        if (!SkillRoot(Roots[I]) || (I > 0 && Roots[I] == TEXT("/Game/Y01Playground"))) { return Reject(TEXT("隔离根非法")); }
        for (int32 J = I + 1; J < Roots.Num(); ++J)
        { if (Roots[I] == Roots[J] || Roots[I].StartsWith(Roots[J] + TEXT("/")) || Roots[J].StartsWith(Roots[I] + TEXT("/"))) { return Reject(TEXT("根重叠")); } }
    }
    if (Root == TEXT("/Game/Y01Playground") && (Legacy != TEXT("/Game/Developers/Y01PlaygroundTests/R33U2Legacy")
        || Compare != TEXT("/Game/Developers/Y01PlaygroundTests/R33U2Compare") || Growth != TEXT("/Game/Developers/Y01PlaygroundTests/R33U3GrowthCompare")))
    { return Reject(TEXT("主资产仅允许批准的三个隔离根")); }
    auto* MainTable = P->Tables.FindRef(EY01TableKind::Character).Get();
    auto* MainRow = MainTable->FindRow<FY01CharacterRow>(P->Character.RowName, TEXT("GrowthUpgrade"));
    if (!MainRow || MainTable->GetOutermost()->GetName() != Root + TEXT("/Data/DT_Character")
        || (!SameGrowth(MainRow->R33TestGrowth, FY01R33TestGrowth()) && !SameGrowth(MainRow->R33TestGrowth, ApprovedGrowth(1))))
    { return Reject(TEXT("主目标成长为未知用户值或共享表，拒绝覆盖")); }
    if (!BuildMap(Root, P, true, Error) || !ValidateSaved(Legacy, L) || !ValidateSaved(Compare, C)
        || !SharedProfile(P, L.Profile.Get()) || !SharedProfile(P, C.Profile.Get())) { return Reject(TEXT("旧Profile形状/引用/快照不一致")); }
    TArray<FName> Names;
    auto* MainSkill = P->Tables.FindRef(EY01TableKind::Skill).Get();
    auto* LegacySkill = L.Profile->Tables.FindRef(EY01TableKind::Skill).Get();
    auto* CompareSkill = C.Profile->Tables.FindRef(EY01TableKind::Skill).Get();
    if (!SkillRows(P, Names) || (!History && MainSkill->GetOutermost()->GetName() != Root + TEXT("/Data/DT_Skill"))
        || LegacySkill->GetOutermost()->GetName() != Legacy + TEXT("/Data/DT_Skill")
        || CompareSkill->GetOutermost()->GetName() != Compare + TEXT("/Data/DT_Skill")
        || !LegacySkills(LegacySkill, Names) || MainSkill->GetRowNames().Num() != LegacySkill->GetRowNames().Num()
        || MainSkill->GetRowNames().Num() != CompareSkill->GetRowNames().Num()) { return Reject(TEXT("旧U2技能指纹/路径/行集合不符")); }
    for (FName Name : MainSkill->GetRowNames())
    {
        const auto* A = MainSkill->FindRow<FY01SkillRow>(Name, TEXT("GrowthUpgrade"));
        const auto* Old = LegacySkill->FindRow<FY01SkillRow>(Name, TEXT("GrowthUpgrade"));
        const auto* Other = CompareSkill->FindRow<FY01SkillRow>(Name, TEXT("GrowthUpgrade"));
        if (!Old || !Other) { return Reject(TEXT("旧技能缺行")); }
        const int32 Index = Names.IndexOfByKey(Name); const auto Expected = Index == INDEX_NONE ? *Old : UpgradedSkill(*Old, Index);
        auto ExpectedCompare = Expected; if (Index == 1) { ExpectedCompare.TestSkill.Speed = 200; }
        if (!SameSkill(A, &Expected) || !SameSkill(Other, &ExpectedCompare)) { return Reject(TEXT("U2技能非批准配置")); }
    }
    auto Occupied = [](const FString& Package) { return FPackageName::DoesPackageExist(Package) || FindPackage(nullptr, *Package); };
    const TArray<UY01PlaygroundProfile*> OldProfiles{L.Profile.Get(), C.Profile.Get()};
    for (int32 I = 0; I < OldProfiles.Num(); ++I)
    {
        auto* Old = OldProfiles[I]; const FString OldRoot = I == 0 ? Legacy : Compare;
        if (!BuildMap(OldRoot, Old, true, Error)) { return Reject(TEXT("旧地图不符合")); }
        const bool Shared = Old->Tables.FindRef(EY01TableKind::Character) == MainTable;
        if (Shared && (MainRow->R33TestGrowth.bEnabled || Occupied(OldRoot + TEXT("/Data/DT_Character"))))
        { return Reject(TEXT("旧快照缺失但主已开启，或同名快照冲突")); }
    }
    const FString Baseline = Compare + TEXT("/Baseline");
    // 完整预检所有输出，之后才允许第一次SavePackage；冲突不会留下半套升级。
    auto CheckOutput = [&](const FString& Out, FY01PlaygroundGenerationResult& Loaded, bool IsGrowth)
    {
        const FString PP = Out + TEXT("/DA_PlaygroundProfile"), TP = Out + TEXT("/Data/DT_Character"), MP = Out + TEXT("/Maps/L_Y01Playground");
        if (!FPackageName::DoesPackageExist(PP))
        {
            TArray<FString> Files; IFileManager::Get().FindFilesRecursive(Files, *FPackageName::LongPackageNameToFilename(Out), TEXT("*"), true, false);
            return Files.IsEmpty() && !Occupied(PP) && !Occupied(TP) && !Occupied(MP);
        }
        if (!ValidateSaved(Out, Loaded) || !ProfileShape(P, Loaded.Profile.Get()) || !BuildMap(Out, Loaded.Profile.Get(), true, Error)) { return false; }
        auto* Copy = Loaded.Profile.Get();
        for (const auto& Pair : P->Tables)
        {
            if (Pair.Key != EY01TableKind::Character && Copy->Tables.FindRef(Pair.Key) != Pair.Value) { return false; }
        }
        auto* CT = Copy->Tables.FindRef(EY01TableKind::Character).Get();
        return IsGrowth ? CT && CT->GetOutermost()->GetName() == TP && CharacterSnapshot(MainTable, CT, P->Character.RowName, ApprovedGrowth(2))
            : CT == C.Profile->Tables.FindRef(EY01TableKind::Character) && CT != MainTable;
    };
    if (!CheckOutput(Baseline, B, false) || !CheckOutput(Growth, G, true)) { return Reject(TEXT("基线/成长对照输出冲突")); }
    if (History && (!MainRow->R33TestGrowth.bEnabled || !B.Profile || !G.Profile
        || L.Profile->Tables.FindRef(EY01TableKind::Character) == MainTable || C.Profile->Tables.FindRef(EY01TableKind::Character) == MainTable))
    { return Reject(TEXT("历史快照不完整；禁止旧升级补写")); }
    bool Changed = false;
    for (int32 I = 0; I < OldProfiles.Num(); ++I)
    {
        auto* Old = OldProfiles[I];
        if (Old->Tables.FindRef(EY01TableKind::Character) != MainTable) { continue; }
        const FString TP = (I == 0 ? Legacy : Compare) + TEXT("/Data/DT_Character");
        auto* Snapshot = DuplicateObject<UDataTable>(MainTable, CreatePackage(*TP), TEXT("DT_Character"));
        Snapshot->FindRow<FY01CharacterRow>(P->Character.RowName, TEXT("GrowthUpgrade"))->R33TestGrowth.bEnabled = false;
        if (!SaveSkillAsset(Snapshot)) { return Reject(TEXT("旧Character快照保存失败；主未开启")); }
        Old->Tables[EY01TableKind::Character] = Snapshot;
        if (!SaveSkillAsset(Old)) { return Reject(TEXT("旧Profile引用保存失败；主未开启")); } Changed = true;
        AuditGrowth(Old, TEXT("SAVED_ISOLATION"));
    }
    auto SaveOutput = [&](const FString& Out, FY01PlaygroundGenerationResult& Loaded, bool IsGrowth)
    {
        if (Loaded.Profile.IsValid()) { return true; }
        auto* Copy = DuplicateObject<UY01PlaygroundProfile>(P, CreatePackage(*(Out + TEXT("/DA_PlaygroundProfile"))), TEXT("DA_PlaygroundProfile"));
        auto* CT = C.Profile->Tables.FindRef(EY01TableKind::Character).Get();
        if (IsGrowth)
        {
            CT = DuplicateObject<UDataTable>(MainTable, CreatePackage(*(Out + TEXT("/Data/DT_Character"))), TEXT("DT_Character"));
            CT->FindRow<FY01CharacterRow>(P->Character.RowName, TEXT("GrowthUpgrade"))->R33TestGrowth = ApprovedGrowth(2);
            if (!SaveSkillAsset(CT)) { return false; }
        }
        Copy->Tables[EY01TableKind::Character] = CT;
        if (!Copy->Validate().IsValid() || !SaveSkillAsset(Copy) || !BuildMap(Out, Copy, false, Error)) { return false; }
        Loaded.Profile.Reset(Copy); Changed = true; return true;
    };
    if (!SaveOutput(Baseline, B, false) || !SaveOutput(Growth, G, true)) { return Reject(TEXT("输出保存失败；主未开启")); }
    if (!MainRow->R33TestGrowth.bEnabled)
    {
        MainRow->R33TestGrowth = ApprovedGrowth(1);
        if (!SaveSkillAsset(MainTable)) { MainRow->R33TestGrowth = FY01R33TestGrowth(); return Reject(TEXT("主成长保存失败")); } Changed = true;
    }
    for (auto* Profile : {P, L.Profile.Get(), C.Profile.Get(), B.Profile.Get(), G.Profile.Get()}) { AuditGrowth(Profile, Changed ? TEXT("SAVED") : TEXT("PRESERVED")); }
    return 0;
}
}
#endif

UY01CreatePlaygroundCommandlet::UY01CreatePlaygroundCommandlet()
{
    IsClient = false;
    IsServer = false;
    IsEditor = true;
    LogToConsole = true;
}

int32 UY01CreatePlaygroundCommandlet::Main(const FString& Params)
{
#if WITH_EDITOR
    FString Root = TEXT("/Game/Y01Playground");
    FParse::Value(*Params, TEXT("Root="), Root);
    FY01PlaygroundGenerationResult Result;
    if (FParse::Param(*Params, TEXT("R34Audit")) || FParse::Param(*Params, TEXT("R34Upgrade")) || FParse::Param(*Params, TEXT("R34Copy")))
    { return Y01PlaygroundEditor::RunR34Migration(Root, Params); }
    if (FParse::Param(*Params, TEXT("GrowthAudit")) || FParse::Param(*Params, TEXT("GrowthUpgrade"))) { return RunGrowth(Root, Params); }
    if (FParse::Param(*Params, TEXT("SkillAudit")) || FParse::Param(*Params, TEXT("SkillUpgrade"))
        || FParse::Param(*Params, TEXT("SkillLegacyCopy")) || FParse::Param(*Params, TEXT("SkillCompareCopy")))
    { return RunSkills(Root, Params); }
    if (FParse::Param(*Params, TEXT("TrainingAudit")) || FParse::Param(*Params, TEXT("TrainingUpgrade")) || FParse::Param(*Params, TEXT("TrainingCopy")))
    {
        if (!Y01PlaygroundEditor::ValidateSaved(Root, Result))
        { for (const auto& Error : Result.Errors) { UE_LOG(LogY01Playground, Error, TEXT("%s"), *Error); } return 1; }
        auto* Profile = Result.Profile.Get();
        auto* Table = Profile->Tables.FindRef(EY01TableKind::TrainingStage).Get();
        auto Dump = [](const UDataTable* Value)
        {
            FString Text;
            for (const FName Name : Value->GetRowNames())
            {
                const auto* R = Value->FindRow<FY01TrainingStageRow>(Name, TEXT("TrainingAudit"));
                Text += FString::Printf(TEXT("\nRow=%s Stage=%d StartSeconds=%g SpawnInterval=%g MaxAlive=%d Reward=%s"),
                    *Name.ToString(), R->Stage, R->StartSeconds, R->SpawnInterval, R->MaxAlive, *R->Reward.RowName.ToString());
                for (int32 I = 0; I < R->Spawns.Num(); ++I)
                { const auto& E = R->Spawns[I]; Text += FString::Printf(TEXT("\nSpawns[%d] Monster=%s Count=%d DelaySeconds=%g"), I, *E.Monster.RowName.ToString(), E.Count, E.DelaySeconds); }
            }
            return Text;
        };
        UE_LOG(LogY01Playground, Display, TEXT("TRAINING_BEFORE Profile=%s Enabled=%d Table=%s %s"),
            *Profile->GetPathName(), Profile->bTrainingEnabled, *Table->GetPathName(), *Dump(Table));
        FY01DataValidation Query;
        for (const FName Name : Table->GetRowNames())
        {
            const auto* Row = Table->FindRow<FY01TrainingStageRow>(Name, TEXT("TrainingAudit"));
            const auto* Reward = Profile->BuildCatalog().FindRow<FY01RewardPackageRow>(Row->Reward, Query);
            UE_LOG(LogY01Playground, Display, TEXT("TRAINING_REWARD Row=%s Reward=%s Gold=%d XP=%d Items=%d"),
                *Name.ToString(), *Row->Reward.RowName.ToString(), Reward ? Reward->Gold : -1, Reward ? Reward->Experience : -1, Reward ? Reward->Items.Num() : -1);
        }
        if (FParse::Param(*Params, TEXT("TrainingAudit")) || FParse::Param(*Params, TEXT("ValidateOnly"))) { return 0; }
        if (Profile->R34AssetVersion != 0) { UE_LOG(LogY01Playground, Error, TEXT("R34配置禁止旧升级；请显式选择r33growth历史根")); return 1; }
        if (R33History(Profile, Root))
        {
            const auto* Row = Table->FindRow<FY01TrainingStageRow>(TEXT("Default"), TEXT("HistoricalTraining"));
            FString Error;
            if (FParse::Param(*Params, TEXT("TrainingCopy")) || !Profile->bTrainingEnabled || !Row || Row->Stage != 1
                || Row->StartSeconds != 0 || Row->SpawnInterval != 3 || Row->MaxAlive != 6 || Row->Spawns.Num() != 1
                || Row->Spawns[0].Count != 2 || Row->Spawns[0].DelaySeconds != 0 || Row->Spawns[0].Monster.RowName != TEXT("Default")
                || !Y01PlaygroundEditor::BuildMap(Root, Profile, true, Error))
            { UE_LOG(LogY01Playground, Error, TEXT("历史训练配置冲突，拒绝回写")); return 1; }
            UE_LOG(LogY01Playground, Display, TEXT("TRAINING_PRESERVED Profile=%s Saved=0"), *Profile->GetPathName()); return 0;
        }
        const bool bCopy = FParse::Param(*Params, TEXT("TrainingCopy"));
        if (Root != TEXT("/Game/Y01Playground") || Table->GetOutermost()->GetName() != Root + TEXT("/Data/DT_TrainingStage"))
        { UE_LOG(LogY01Playground, Error, TEXT("精准升级仅允许已批准主程序测试Profile及训练表")); return 1; }
        const auto* OriginalRow = Table->FindRow<FY01TrainingStageRow>(TEXT("Default"), TEXT("TrainingUpgrade"));
        if (!OriginalRow || OriginalRow->Stage != 1 || OriginalRow->StartSeconds != 0 || OriginalRow->Spawns.Num() != 1)
        { UE_LOG(LogY01Playground, Error, TEXT("首阶段形状与批准配置不符，拒绝盲目升级")); return 1; }
        FString OutputRoot = Root;
        if (bCopy)
        {
            FParse::Value(*Params, TEXT("CopyRoot="), OutputRoot);
            if (!OutputRoot.StartsWith(TEXT("/Game/Developers/Y01PlaygroundTests/")) || !FPackageName::IsValidLongPackageName(OutputRoot)) { return 1; }
            const FString PP = OutputRoot + TEXT("/DA_PlaygroundProfile"), TP = OutputRoot + TEXT("/Data/DT_TrainingStage");
            if (FPackageName::DoesPackageExist(PP) || FPackageName::DoesPackageExist(TP))
            { UE_LOG(LogY01Playground, Error, TEXT("隔离副本已存在，拒绝覆盖")); return 1; }
            Profile = DuplicateObject<UY01PlaygroundProfile>(Profile, CreatePackage(*PP), TEXT("DA_PlaygroundProfile"));
            Table = DuplicateObject<UDataTable>(Table, CreatePackage(*TP), TEXT("DT_TrainingStage"));
            Profile->Tables[EY01TableKind::TrainingStage] = Table;
        }
        // 只改批准的首阶段周期、容量、每批数量、延迟与启用开关；保留其余行、引用、表及用户数值。
        auto* Row = Table->FindRow<FY01TrainingStageRow>(TEXT("Default"), TEXT("TrainingUpgrade"));
        Row->SpawnInterval = bCopy ? 1 : 3; Row->MaxAlive = 6; Row->Spawns[0].Count = bCopy ? 1 : 2; Row->Spawns[0].DelaySeconds = 0;
        Profile->bTrainingEnabled = true;
        const auto Validation = Profile->Validate();
        if (!Validation.IsValid())
        { for (const auto& E : Validation.Errors) { UE_LOG(LogY01Playground, Error, TEXT("%s.%s %s"), *E.Source.RowName.ToString(), *E.Field, *E.Message); } return 1; }
        auto Save = [](UObject* Asset)
        {
            auto* Package = Asset->GetOutermost(); Asset->SetFlags(RF_Public | RF_Standalone); Asset->ClearFlags(RF_Transient); Package->MarkPackageDirty();
            const FString File = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
            IFileManager::Get().MakeDirectory(*FPaths::GetPath(File), true);
            FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
            return UPackage::SavePackage(Package, Asset, *File, Args);
        };
        if (!Save(Table) || !Save(Profile)) { return 1; }
        FString Error;
        if (bCopy && !Y01PlaygroundEditor::BuildMap(OutputRoot, Profile, false, Error))
        { UE_LOG(LogY01Playground, Error, TEXT("%s"), *Error); return 1; }
        UE_LOG(LogY01Playground, Display, TEXT("TRAINING_SAVED Profile=%s Enabled=%d Table=%s %s"),
            *Profile->GetPathName(), Profile->bTrainingEnabled, *Table->GetPathName(), *Dump(Table));
        return 0;
    }
    const bool bSuccess = FParse::Param(*Params, TEXT("ValidateOnly"))
        ? Y01PlaygroundEditor::ValidateSaved(Root, Result)
        : Y01PlaygroundEditor::Generate(Root, Result);
    for (const FString& Error : Result.Errors) { UE_LOG(LogY01Playground, Error, TEXT("%s"), *Error); }
    if (!bSuccess) { return 1; }
    UY01PlaygroundProfile* Profile = Result.Profile.Get();
    FString MapError;
    if (FParse::Param(*Params, TEXT("RepairSeedEnemyBinding")) && !FParse::Param(*Params, TEXT("ValidateOnly"))
        && !Y01PlaygroundEditor::RepairSeedEnemyBinding(Profile, MapError))
    { UE_LOG(LogY01Playground, Error, TEXT("%s"), *MapError); return 1; }
    if (FParse::Param(*Params, TEXT("WithMap"))
        && !Y01PlaygroundEditor::BuildMap(Root, Profile, FParse::Param(*Params, TEXT("ValidateOnly")), MapError))
    { UE_LOG(LogY01Playground, Error, TEXT("%s"), *MapError); return 1; }
    const FY01DataCatalog Catalog = Profile->BuildCatalog();
    FY01DataValidation Query;
    const auto* Character = Catalog.FindRow<FY01CharacterRow>(Profile->Character, Query);
    FY01DataRef RewardRef;
    RewardRef.Type = EY01TableKind::RewardPackage;
    RewardRef.RowName = TEXT("Default");
    const auto* Reward = Catalog.FindRow<FY01RewardPackageRow>(RewardRef, Query);
    UE_LOG(LogY01Playground, Display, TEXT("Profile=%s Tables=%d Waves=%d Created=%d Preserved=%d CharacterHealth=%g DefaultGold=%d"),
        *Profile->GetPathName(), Profile->Tables.Num(), Profile->OrderedWaves.Num(), Result.CreatedPackages.Num(),
        Result.PreservedPackages.Num(), Character ? Character->Health : 0.f, Reward ? Reward->Gold : -1);
    UE_LOG(LogY01Playground, Display, TEXT("非战斗表仅为合法数据占位，不代表已实现玩法。已有资产均未回写。"));
    return 0;
#else
    UE_LOG(LogY01Playground, Error, TEXT("生成测试资产只支持 Editor 构建；当前构建不会创建种子或保存包。"));
    return 1;
#endif
}
