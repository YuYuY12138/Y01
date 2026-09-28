#pragma once

#include "Playground/Y01PlaygroundProfile.h"
#include "UObject/StrongObjectPtr.h"

#if WITH_EDITOR
struct FY01PlaygroundGenerationResult
{
    TStrongObjectPtr<UY01PlaygroundProfile> Profile;
    TArray<FString> CreatedPackages;
    TArray<FString> PreservedPackages;
    TArray<FString> Errors;
};

namespace Y01PlaygroundEditor
{
    // 仅补缺失包，不提供覆盖开关；完整配置只验证、不回写。
    bool Generate(const FString& Root, FY01PlaygroundGenerationResult& OutResult);
    // 只加载真实资产，不创建种子、不保存任何包。
    bool ValidateSaved(const FString& Root, FY01PlaygroundGenerationResult& OutResult);
}
#endif
