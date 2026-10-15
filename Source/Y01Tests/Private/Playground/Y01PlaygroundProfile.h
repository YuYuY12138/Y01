#pragma once

#include "Engine/DataAsset.h"
#include "Data/Y01DataCatalog.h"
#include "Y01PlaygroundProfile.generated.h"

// 仅开发测试配置；生命周期内强持有表，不改变正式数据子系统。
UCLASS(BlueprintType)
class UY01PlaygroundProfile : public UDataAsset
{
    GENERATED_BODY()
public:
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Playground")
    TMap<EY01TableKind, TObjectPtr<UDataTable>> Tables;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Playground")
    TArray<FY01DataRef> OrderedWaves;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Playground")
    FY01DataRef Character;

    // 旧内存夹具默认禁用；普通磁盘Playground显式启用。
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Playground")
    bool bTrainingEnabled = false;

    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Playground")
    bool bR34AutoAttackEnabled = false;

    // 迁移身份独立于可编辑玩法开关；重复升级不得恢复用户数值。
    UPROPERTY(VisibleAnywhere, Category="程序测试迁移")
    int32 R34AssetVersion = 0;

    UPROPERTY(VisibleAnywhere, Category="程序测试迁移")
    FString R34SourceProfile;

    UPROPERTY(VisibleAnywhere, Category="程序测试迁移")
    FName R34Variant;

    // 返回只读表目录；调用者须在使用目录期间持有本配置，编辑后重新构建。
    FY01DataCatalog BuildCatalog() const;
    FY01DataValidation Validate() const;
protected:
    virtual void ValidateExtension(FY01DataValidation& Result) const {}
};
