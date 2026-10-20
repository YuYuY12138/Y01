#pragma once

#include "Components/ActorComponent.h"
#include "Player/Y01SourceRuntimeTypes.h"
#include "Y01ShieldComponent.generated.h"

class AY01PlayerState;

// 服务器唯一真源：按来源分层护盾；仅复制总量供血条。同来源累加至层上限并刷新时长；
// 受伤先扣最快到期层；死亡即清空（Y01Combat 死亡分支 ResetLayers），复活后保持空，
// 由仍在身的周期护盾来源按其周期重新获得（复活后首个结算点若周期已到，立即获得一层视为合法的"再次获得"）。
// 死亡等待/复活期间 Tick 挂起（Revive->BlocksActions 门）：不结算层、不清理 NextPeriodic，护盾不累积；
// 复活后按既定周期继续。"离局"指 PlayerState 销毁随之析构（EndSession 不显式清层，层自然过期）。
// 不计入生命条件、不可被吸血回复（治疗只触Health）。
UCLASS(ClassGroup=(Y01), meta=(BlueprintSpawnableComponent))
class Y01_API UY01ShieldComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01ShieldComponent();
    void ResetLayers();
    void GrantLayer(const FString& LayerKey, double Value, double LayerCap, double DurationSeconds);
    void Consume(double Amount);
    double GetTotal() const { return TotalShield; }
    // ShieldOnCast 入口：LayerKey 须形如 SourceId/EffectId。只读已提交的 Records，不得在来源事务暂存期间调用。
    void NotifyCast(const FString& CastSourceId);
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
private:
    struct FLayer { FString Key; double Value = 0; double Cap = 0; double Expires = 0; };
    UPROPERTY(Replicated) double TotalShield = 0;
    TArray<FLayer> Layers;
    TMap<FString, double> NextPeriodic;   // ShieldPeriodic 来源下次获得时刻
    void RecomputeTotal();
    AY01PlayerState* Player() const;
};
