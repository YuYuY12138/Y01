#pragma once
#include "GameFramework/HUD.h"
#include "Y01PlaygroundHUD.generated.h"

UCLASS()
class AY01PlaygroundHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
    virtual void NotifyHitBoxClick(FName BoxName) override;
private:
    bool bSkillDetailsExpanded = false;
    bool bAttributesExpanded = false;
#if WITH_DEV_AUTOMATION_TESTS
    friend class FY01TrainingTest;
    friend class FY01TestSkills;
    friend class FY01GrowthTests;
    friend class FY01R34Acceptance;
    friend class FY01R34EquipmentView;
    // 仅观测已提交给Canvas的实际绘制，不参与布局或状态决策。
    TFunction<void(const FBox2D&, const FString&)> ObserveDrawForTest;
#endif
};
