#pragma once

#include "Data/Y01DataTypes.h"

// 外部目录不拥有表；调用者须保证表生命周期覆盖目录。Subsystem 负责持有正式表。
using FY01TableMap = TMap<EY01TableKind, const UDataTable*>;

enum class EY01DataErrorCode : uint8
{
    MissingTable, WrongSchema, EmptyTable, InvalidNumber, InvalidReference,
    TypeMismatch, QualityMismatch, InvalidPool, MissingLevel, DuplicateValue,
    MissingQuality, InvalidEnum, BossPreparation, Invariant
};

struct Y01_API FY01DataError
{
    EY01DataErrorCode Code = EY01DataErrorCode::InvalidReference;
    FY01DataRef Source;
    FString Field;
    FString Message;
};

struct Y01_API FY01DataValidation
{
    TArray<FY01DataError> Errors;
    bool IsValid() const { return Errors.IsEmpty(); }
};

class Y01_API FY01DataCatalog
{
public:
    explicit FY01DataCatalog(const FY01TableMap& InTables = {}) : Tables(InTables) {}
    FY01DataValidation Validate() const;

    template <typename T>
    const T* FindRow(const FY01DataRef& Ref, FY01DataValidation& OutErrors) const
    {
        return static_cast<const T*>(FindRaw(Ref, T::StaticStruct(), OutErrors));
    }

    // 失败时清空输出，且不消耗随机流；成功仅返回静态身份，不写入表。
    bool SamplePool(const FY01DataRef& Pool, FRandomStream& Random, FY01DataRef& OutItem, FY01DataValidation& OutErrors) const;

private:
    const void* FindRaw(const FY01DataRef& Ref, const UScriptStruct* Expected, FY01DataValidation& OutErrors) const;
    FY01TableMap Tables;
};
