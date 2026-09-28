#pragma once

#include "CoreMinimal.h"

class UY01PlaygroundProfile;

#if WITH_EDITOR
// 种子仅供编辑器生成与自动化测试使用；运行时只能加载真实资产。
namespace Y01PlaygroundEditor
{
    UY01PlaygroundProfile* CreateSeed(UObject* Outer);
}
#endif
