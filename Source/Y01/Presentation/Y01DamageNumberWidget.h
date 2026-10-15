#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/WidgetComponent.h"
#include "Y01DamageNumberWidget.generated.h"

class UTextBlock;

// 纯显示，不绑定受击Actor，不使用蓝图、屏幕Canvas或动画回调。
UCLASS()
class Y01_API UY01DamageNumberWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetNumber(float Amount, const FLinearColor& Color);
    void ResetNumber();
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
private:
    UPROPERTY(Transient) TObjectPtr<UTextBlock> Number;
};

// 引擎WidgetComponent的绘制由池统一调用，组件自身不注册Tick。
UCLASS()
class Y01_API UY01DamageWorldWidgetComponent : public UWidgetComponent
{
    GENERATED_BODY()
public:
    UY01DamageWorldWidgetComponent();
    void DrawFromPool(float DeltaSeconds);
};
