#include "Data/Y01DataSubsystem.h"

void UY01DataSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    LoadFromSettings(*GetDefault<UY01DataSettings>());
}

void UY01DataSubsystem::Deinitialize()
{
    bReady = false;
    Catalog = FY01DataCatalog();
    LoadedTables.Reset();
    Validation.Errors.Reset();
    Super::Deinitialize();
}

const FY01DataValidation& UY01DataSubsystem::LoadFromSettings(const UY01DataSettings& Settings)
{
    bReady = false;
    Catalog = FY01DataCatalog();
    LoadedTables.Reset();
    FY01TableMap Tables;
    for (const auto& Pair : Settings.Tables)
    {
        UDataTable* Table = Pair.Value.LoadSynchronous();
        LoadedTables.Add(Pair.Key, Table);
        Tables.Add(Pair.Key, Table);
    }
    Catalog = FY01DataCatalog(Tables);
    Validation = Catalog.Validate();
    bReady = Validation.IsValid();
    return Validation;
}
