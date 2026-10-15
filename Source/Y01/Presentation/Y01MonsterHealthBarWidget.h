#pragma once

#include "Blueprint/UserWidget.h"
#include "Components/WidgetComponent.h"
#include "Y01MonsterHealthBarWidget.generated.h"

class UProgressBar;

UCLASS()
class Y01_API UY01MonsterHealthBarWidget : public UUserWidget
{
    GENERATED_BODY()
public:
    void SetHealth(float Health, float MaxHealth);
    void ResetBar();
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
    virtual void NativeConstruct() override;
private:
    UPROPERTY(Transient) TObjectPtr<UProgressBar> Bar;
};

UCLASS()
class Y01_API UY01MonsterHealthBarWorldWidgetComponent : public UWidgetComponent
{
    GENERATED_BODY()
public:
    UY01MonsterHealthBarWorldWidgetComponent();
    // 不依赖可见性或上次渲染时间；借出前在隐藏状态准备新纹理。
    bool DrawPrepared(float DeltaSeconds);
};
