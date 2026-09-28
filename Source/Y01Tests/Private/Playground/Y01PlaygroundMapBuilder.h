#pragma once
#include "CoreMinimal.h"
class UY01PlaygroundProfile;
#if WITH_EDITOR
namespace Y01PlaygroundEditor
{
    bool BuildMap(const FString& Root, UY01PlaygroundProfile* Profile, bool bValidateOnly, FString& Error);
    bool RepairSeedEnemyBinding(UY01PlaygroundProfile* Profile, FString& Error);
}
#endif
