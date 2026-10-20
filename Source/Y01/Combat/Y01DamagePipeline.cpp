#include "Combat/Y01DamagePipeline.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01ShieldComponent.h"
#include "Match/Y01GameFlow.h"
#include "Engine/World.h"

// 暴击判定：仅服务器权威，按攻击者快照属性掷一次概率。
bool Y01DamagePipeline::RollCritical(const UY01CombatAbilitySystem* Source)
{
    if (!Source || !Source->GetOwner() || !Source->GetOwner()->HasAuthority()) { return false; }
    const float Chance = Source->GetNumericAttribute(UY01AttributeSet::GetCritChanceAttribute());
    return FMath::IsFinite(Chance) && Chance > 0 && FMath::FRand() < Chance;
}
// 共享结算：基础 → ×暴击倍率(若暴击) → 减甲(下限0) → 护盾吸收(仅玩家目标) → 扣血(不超当前生命)。
// 护盾在Resolve内扣减是安全的：执行器均以IsLegalCombatTarget为前置（内含IsCombatPairAllowed），
// PreGameplayEffectExecute唯一拒绝路径(:84)在同一配对上不会再拒绝已通过执行器的spec。
FY01DamageResolve Y01DamagePipeline::Resolve(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target,
    float BaseDamage, bool bCritical)
{
    FY01DamageResolve R;
    if (!Source || !Target || !FMath::IsFinite(BaseDamage) || BaseDamage < 0) { return R; }
    float Base = BaseDamage;
    if (bCritical)
    {
        double Multiplier = 2.;
        if (const auto* Mode = Source->GetWorld() ? Source->GetWorld()->GetAuthGameMode<AY01GameMode>() : nullptr)
        { Multiplier = Mode->GetDefaultCritMultiplier(); }
        const float Bonus = Source->GetNumericAttribute(UY01AttributeSet::GetCritMultiplierBonusAttribute());
        // 非有限加成按0处理，防NaN穿透乘算变成全额伤害。
        Base = static_cast<float>(FMath::Clamp(static_cast<double>(Base)
            * (Multiplier + (FMath::IsFinite(Bonus) ? FMath::Max(0.f, Bonus) : 0.f)), 0., static_cast<double>(MAX_flt)));
    }
    const float Armor = Target->GetNumericAttribute(UY01AttributeSet::GetArmorAttribute());
    R.bCritical = bCritical;
    R.AfterArmor = FMath::Max(0.f, Base - (FMath::IsFinite(Armor) ? FMath::Max(0.f, Armor) : 0.f));
    float ReachHealth = R.AfterArmor;
    if (auto* TargetPlayer = Cast<AY01PlayerState>(Target->GetOwner()))
    {
        if (auto* Shield = TargetPlayer->GetShield())
        {
            const double Total = Shield->GetTotal();
            if (Total > 0)
            {
                ReachHealth = R.AfterArmor - static_cast<float>(FMath::Min(Total, static_cast<double>(R.AfterArmor)));
                R.ShieldAbsorbed = R.AfterArmor - ReachHealth;
                if (R.ShieldAbsorbed > 0) { Shield->Consume(R.ShieldAbsorbed); }
            }
        }
    }
    const float Health = Target->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    R.HealthLoss = FMath::IsFinite(Health) && Health > 0 ? FMath::Min(ReachHealth, Health) : 0.f;
    R.bValid = true;
    return R;
}
