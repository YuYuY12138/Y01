#pragma once

#include "Data/Y01DataCatalog.h"
#include "Data/Y01DataSettings.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Y01DataSubsystem.generated.h"

UCLASS()
class Y01_API UY01DataSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    const FY01DataValidation& LoadFromSettings(const UY01DataSettings& Settings);
    const FY01DataValidation& GetValidation() const { return Validation; }
    bool IsReady() const { return bReady; }
    // 返回的指针仅在下一次加载或 Deinitialize 前有效。
    const FY01DataCatalog* GetCatalog() const { return bReady ? &Catalog : nullptr; }

private:
    UPROPERTY(Transient) TMap<EY01TableKind, TObjectPtr<UDataTable>> LoadedTables;
    FY01DataCatalog Catalog;
    FY01DataValidation Validation;
    bool bReady = false;
};
