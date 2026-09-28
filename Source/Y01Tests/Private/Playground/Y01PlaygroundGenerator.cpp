#include "Playground/Y01PlaygroundGenerator.h"

#if WITH_EDITOR
#include "Playground/Y01PlaygroundSeed.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

namespace
{
void ResetResult(FY01PlaygroundGenerationResult& Result)
{
    Result.Profile.Reset();
    Result.CreatedPackages.Reset();
    Result.PreservedPackages.Reset();
    Result.Errors.Reset();
}

FString FilenameFor(const FString& Package)
{
    return FPackageName::LongPackageNameToFilename(Package, FPackageName::GetAssetPackageExtension());
}

bool CheckRoot(const FString& Root, FY01PlaygroundGenerationResult& Result)
{
    FText Reason;
    if (!Root.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Root, false, &Reason))
    {
        Result.Errors.Add(FString::Printf(TEXT("Root=%s：必须是合法的 /Game/ 子包目录。%s"), *Root, *Reason.ToString()));
        return false;
    }
    FString Filename;
    if (!FPackageName::TryConvertLongPackageNameToFilename(Root, Filename)
        || !FPaths::IsUnderDirectory(FPaths::ConvertRelativePathToFull(Filename), FPaths::ConvertRelativePathToFull(FPaths::ProjectContentDir())))
    {
        Result.Errors.Add(TEXT("Root 未挂载到本项目 Content：") + Root);
        return false;
    }
    return true;
}

bool ExistsOnDisk(const FString& Package)
{
    return FPackageName::DoesPackageExist(Package) || IFileManager::Get().FileExists(*FilenameFor(Package));
}

bool IsOccupied(const FString& Package)
{
    return ExistsOnDisk(Package) || FindPackage(nullptr, *Package) != nullptr;
}

UObject* LoadExisting(const FString& Package, UClass* Expected, FY01PlaygroundGenerationResult& Result)
{
    if (!ExistsOnDisk(Package))
    {
        Result.Errors.Add(TEXT("缺少已保存包，或同名包只存在内存；拒绝覆盖：") + Package);
        return nullptr;
    }
    UPackage* Loaded = LoadPackage(nullptr, *Package, LOAD_None);
    UObject* Asset = Loaded ? FindObject<UObject>(Loaded, *FPackageName::GetLongPackageAssetName(Package)) : nullptr;
    if (!Asset || !Asset->IsA(Expected))
    {
        Result.Errors.Add(FString::Printf(TEXT("资产 %s 加载失败或类型冲突；需要 %s，实际 %s。保留原文件。"),
            *Package, *Expected->GetName(), *GetNameSafe(Asset ? Asset->GetClass() : nullptr)));
        return nullptr;
    }
    Result.PreservedPackages.AddUnique(Package);
    return Asset;
}

bool ValidateProfile(const UY01PlaygroundProfile& Profile, FY01PlaygroundGenerationResult& Result)
{
    for (const auto& Error : Profile.Validate().Errors)
    {
        Result.Errors.Add(FString::Printf(TEXT("%s | %s.%s | %s | Code=%d | %s"), *Profile.GetPathName(),
            *StaticEnum<EY01TableKind>()->GetNameStringByValue(static_cast<int64>(Error.Source.Type)),
            *Error.Source.RowName.ToString(), *Error.Field, static_cast<int32>(Error.Code), *Error.Message));
    }
    return Result.Errors.IsEmpty();
}

bool SaveNewAsset(UObject* Asset, FY01PlaygroundGenerationResult& Result)
{
    UPackage* Package = Asset->GetOutermost();
    const FString Name = Package->GetName();
    // 保存前再次检查，绝不把已有磁盘文件当作可覆盖输出。
    if (ExistsOnDisk(Name))
    {
        Result.Errors.Add(TEXT("保存前发现已有包，拒绝覆盖：") + Name);
        return false;
    }
    const FString Filename = FilenameFor(Name);
    if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true))
    {
        Result.Errors.Add(TEXT("无法创建资产目录：") + Filename);
        return false;
    }
    Asset->SetFlags(RF_Public | RF_Standalone);
    Asset->ClearFlags(RF_Transient);
    Package->MarkPackageDirty();
    FSavePackageArgs Args;
    Args.TopLevelFlags = RF_Public | RF_Standalone;
    Args.SaveFlags = SAVE_NoError;
    if (!UPackage::SavePackage(Package, Asset, *Filename, Args))
    {
        Result.Errors.Add(TEXT("SavePackage 失败；保留已经完成的包，下次只补缺失：") + Filename);
        return false;
    }
    FAssetRegistryModule::AssetCreated(Asset);
    Result.CreatedPackages.Add(Name);
    return true;
}
}

bool Y01PlaygroundEditor::Generate(const FString& Root, FY01PlaygroundGenerationResult& OutResult)
{
    ResetResult(OutResult);
    if (!CheckRoot(Root, OutResult)) { return false; }
    const FString ProfilePackage = Root + TEXT("/DA_PlaygroundProfile");
    // 已有配置的引用和编辑具有权威性；即使非法，也不通过种子自动修复。
    if (IsOccupied(ProfilePackage)) { return ValidateSaved(Root, OutResult); }

    TStrongObjectPtr<UY01PlaygroundProfile> Candidate(CreateSeed(GetTransientPackage()));
    TMap<EY01TableKind, FString> Missing;
    for (uint8 Index = 1; Index <= static_cast<uint8>(EY01TableKind::TalentChoice); ++Index)
    {
        const auto Kind = static_cast<EY01TableKind>(Index);
        const FString Package = Root + TEXT("/Data/DT_") + StaticEnum<EY01TableKind>()->GetNameStringByValue(Index);
        if (IsOccupied(Package))
        {
            auto* Table = Cast<UDataTable>(LoadExisting(Package, UDataTable::StaticClass(), OutResult));
            if (!Table) { return false; }
            Candidate->Tables[Kind] = Table;
        }
        else { Missing.Add(Kind, Package); }
    }
    // 全套候选先验证，非法半套不留下额外磁盘资产。
    if (!ValidateProfile(*Candidate, OutResult)) { return false; }
    for (const auto& Pair : Missing)
    {
        if (IsOccupied(Pair.Value))
        {
            OutResult.Errors.Add(TEXT("生成时出现同名包；停止而不覆盖：") + Pair.Value);
            return false;
        }
        auto* Table = DuplicateObject<UDataTable>(Candidate->Tables[Pair.Key], CreatePackage(*Pair.Value),
            FName(*FPackageName::GetLongPackageAssetName(Pair.Value)));
        Candidate->Tables[Pair.Key] = Table;
        if (!SaveNewAsset(Table, OutResult)) { return false; }
    }
    if (IsOccupied(ProfilePackage))
    {
        OutResult.Errors.Add(TEXT("生成时出现同名profile；停止而不覆盖：") + ProfilePackage);
        return false;
    }
    auto* Profile = NewObject<UY01PlaygroundProfile>(CreatePackage(*ProfilePackage), TEXT("DA_PlaygroundProfile"), RF_Public | RF_Standalone);
    Profile->Tables = Candidate->Tables;
    Profile->OrderedWaves = Candidate->OrderedWaves;
    Profile->Character = Candidate->Character;
    OutResult.Profile.Reset(Profile);
    return SaveNewAsset(Profile, OutResult);
}

bool Y01PlaygroundEditor::ValidateSaved(const FString& Root, FY01PlaygroundGenerationResult& OutResult)
{
    ResetResult(OutResult);
    if (!CheckRoot(Root, OutResult)) { return false; }
    const FString ProfilePackage = Root + TEXT("/DA_PlaygroundProfile");
    auto* Profile = Cast<UY01PlaygroundProfile>(LoadExisting(ProfilePackage, UY01PlaygroundProfile::StaticClass(), OutResult));
    if (!Profile) { return false; }
    OutResult.Profile.Reset(Profile);
    if (!ValidateProfile(*Profile, OutResult)) { return false; }
    for (const auto& Pair : Profile->Tables)
    {
        const UDataTable* Table = Pair.Value;
        const FString Package = Table->GetOutermost()->GetName();
        if (Table->HasAnyFlags(RF_Transient) || !ExistsOnDisk(Package))
        {
            OutResult.Errors.Add(TEXT("配置引用的表不是持久资产：") + Table->GetPathName());
        }
        else { OutResult.PreservedPackages.AddUnique(Package); }
    }
    return OutResult.Errors.IsEmpty();
}
#endif
