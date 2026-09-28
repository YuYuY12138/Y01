#pragma once

#include "GameplayEffect.h"
#include "Y01PlaygroundPassiveDamageEffect.generated.h"

// 固定加五攻击力的测试专用示例，不代表正式配表支持任意效果。
UCLASS()
class UY01PlaygroundPassiveDamageEffect : public UGameplayEffect
{
    GENERATED_BODY()
public:
    UY01PlaygroundPassiveDamageEffect();
};
