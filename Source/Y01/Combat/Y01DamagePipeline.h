#pragma once
#include "CoreMinimal.h"
#include "Combat/Y01Combat.h"

// 伤害管线结算结果：只承载本次攻击的最终数字，不做任何事件发布或奖励结算。
struct Y01_API FY01DamageResolve
{
    bool bValid = false;
    bool bCritical = false;
    float AfterArmor = 0;    // 扣甲后，飘字展示值
    float HealthLoss = 0;    // 实际生命损失（护盾吸收后）
    float ShieldAbsorbed = 0;
};

// 三执行器共享：基础 → ×暴击 → 减甲(下限0) → 护盾吸收(仅玩家) → 扣血(不超当前)。
// 吸血按实际生命损失在 AttributeSet 提交链内应用，不在此处。
class Y01_API Y01DamagePipeline
{
public:
    static bool RollCritical(const UY01CombatAbilitySystem* Source);
    static FY01DamageResolve Resolve(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target,
        float BaseDamage, bool bCritical);
};
