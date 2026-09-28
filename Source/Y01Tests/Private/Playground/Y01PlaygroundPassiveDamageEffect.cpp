#include "Playground/Y01PlaygroundPassiveDamageEffect.h"
#include "Combat/Y01Combat.h"

UY01PlaygroundPassiveDamageEffect::UY01PlaygroundPassiveDamageEffect()
{
    DurationPolicy = EGameplayEffectDurationType::Infinite;
    FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
    Modifier.Attribute = UY01AttributeSet::GetAttackDamageAttribute();
    Modifier.ModifierOp = EGameplayModOp::Additive;
    Modifier.ModifierMagnitude = FScalableFloat(5.f);
}
