#pragma once

#include "CoreMinimal.h"

class AActor;
class UY01CombatAbilitySystem;

namespace Y01Targeting
{
    // 纯候选几何工具，不枚举World，不负责攻击授权或伤害；半径0明确禁用发现。
    Y01_API bool InRadiusXY(const UY01CombatAbilitySystem* Source, const AActor* Target, double Radius);
    // 等距离按对象完整路径词典序稳定选择，不依赖候选容器顺序。
    Y01_API AActor* NearestXY(const UY01CombatAbilitySystem* Source, TConstArrayView<AActor*> Candidates, double Radius);
}
