#pragma once

#include "GameFramework/Actor.h"
#include "Data/Y01DataTypes.h"
#include "Y01SpawnPoint.generated.h"

// 使用Actor Tags中唯一的规范点名；编辑器复制后需修改数字Tag以保持唯一。
UCLASS(BlueprintType)
class Y01_API AY01SpawnPoint : public AActor
{
    GENERATED_BODY()
public:
    AY01SpawnPoint();
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Y01|Spawn") EY01SpawnPointKind PointKind = EY01SpawnPointKind::Normal;
    bool ReadPointTag(FName& OutTag, FString& OutError) const;
    static bool BuildRegistry(UWorld* World, TMap<FName, TWeakObjectPtr<AY01SpawnPoint>>& OutPoints, FString& OutError);
    // 确定性方形螺旋；单怪最多128个候选，单批次最多4096个，共用游标不回退。
    bool TryFindMonsterTransform(int32& CandidateCursor, FTransform& OutTransform, FString& OutError) const;
};
