#pragma once
#include "Playground/Y01R36Profile.h"
#include "UObject/StrongObjectPtr.h"
#include "Commandlets/Commandlet.h"
#include "Y01R36Assets.generated.h"

UCLASS()
class UY01R36AssetsCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UY01R36AssetsCommandlet();
    virtual int32 Main(const FString& Params) override;
};

#if WITH_EDITOR
namespace Y01R36Editor
{
// 仅S1最小内容，三Profile不代表S2经济或S4完整样例交付。
UY01R36Profile* CreateSeed(UObject* Outer, bool bCooldownFixture = false);
struct FGeneration
{
    TArray<TStrongObjectPtr<UY01R36Profile>> Profiles;
    TArray<FString> Created;
    TArray<FString> Preserved;
    FString Error;
};
bool Generate(const FString& Root, FGeneration& Result, bool bValidateOnly = false, bool bCooldownFixture = false, bool bEconomyS2 = false, bool bPresentationS3 = false, bool bContentS4 = false);
}
#endif
