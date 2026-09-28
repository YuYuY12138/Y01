#include "Playground/Y01CreatePlaygroundCommandlet.h"
#include "Misc/Parse.h"
#if WITH_EDITOR
#include "Playground/Y01PlaygroundGenerator.h"
#include "Playground/Y01PlaygroundMapBuilder.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogY01Playground, Log, All);

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
