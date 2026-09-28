#include "Playground/Y01PlaygroundProfile.h"

FY01DataCatalog UY01PlaygroundProfile::BuildCatalog() const
{
    FY01TableMap ReadOnlyTables;
    for (const auto& Pair : Tables) { ReadOnlyTables.Add(Pair.Key, Pair.Value.Get()); }
    return FY01DataCatalog(ReadOnlyTables);
}

FY01DataValidation UY01PlaygroundProfile::Validate() const
{
    const FY01DataCatalog Catalog = BuildCatalog();
    FY01DataValidation Result = Catalog.Validate();
    auto Reference = [&]<typename T>(const FY01DataRef& Ref, EY01TableKind Required, const FString& Field)
    {
        if (Ref.Type != Required)
        {
            Result.Errors.Add({EY01DataErrorCode::TypeMismatch, Ref, Field, TEXT("配置引用类型不匹配")});
            return;
        }
        FY01DataValidation Query;
        Catalog.FindRow<T>(Ref, Query);
        for (const auto& Error : Query.Errors)
        {
            Result.Errors.Add({Error.Code, Ref, Field, FString::Printf(TEXT("引用 %s：%s"), *Ref.RowName.ToString(), *Error.Message)});
        }
    };
    Reference.operator()<FY01CharacterRow>(Character, EY01TableKind::Character, TEXT("Character"));
    if (OrderedWaves.IsEmpty())
    {
        Result.Errors.Add({EY01DataErrorCode::InvalidReference, {}, TEXT("OrderedWaves"), TEXT("测试配置至少需要一个有序波次")});
    }
    for (int32 Index = 0; Index < OrderedWaves.Num(); ++Index)
    {
        Reference.operator()<FY01WaveRow>(OrderedWaves[Index], EY01TableKind::Wave, FString::Printf(TEXT("OrderedWaves[%d]"), Index));
    }
    return Result;
}
