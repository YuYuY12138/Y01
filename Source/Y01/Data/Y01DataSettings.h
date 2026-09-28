#pragma once

#include "Data/Y01DataTypes.h"
#include "Engine/DeveloperSettings.h"
#include "Y01DataSettings.generated.h"

UCLASS(Config=Game, DefaultConfig, meta=(DisplayName="Y01 数据表"))
class Y01_API UY01DataSettings : public UDeveloperSettings
{
    GENERATED_BODY()
public:
    UPROPERTY(Config, EditAnywhere, Category="数据表")
    TMap<EY01TableKind, TSoftObjectPtr<UDataTable>> Tables;
    UPROPERTY(Config, EditAnywhere, Category="对局") TArray<FY01DataRef> OrderedWaves;
    UPROPERTY(Config, EditAnywhere, Category="对局") FY01DataRef MatchCharacter;
};
