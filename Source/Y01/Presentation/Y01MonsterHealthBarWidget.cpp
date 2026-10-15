#include "Presentation/Y01MonsterHealthBarWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/ProgressBar.h"
#include "Brushes/SlateColorBrush.h"
#include "Widgets/SWidget.h"

TSharedRef<SWidget> UY01MonsterHealthBarWidget::RebuildWidget()
{
    if (!Bar)
    {
        Bar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), TEXT("HealthBar"));
        WidgetTree->RootWidget = Bar;
        FProgressBarStyle Style;
        Style.SetBackgroundImage(FSlateColorBrush(FLinearColor(.025f,.025f,.025f,1)));
        Style.SetFillImage(FSlateColorBrush(FLinearColor::White));
        Bar->SetWidgetStyle(Style); Bar->SetFillColorAndOpacity(FLinearColor(.08f,.8f,.12f,1));
        Bar->SetBarFillType(EProgressBarFillType::LeftToRight); Bar->SetBarFillStyle(EProgressBarFillStyle::Scale);
        Bar->SetBorderPadding(FVector2D(2,2)); Bar->SetIsMarquee(false); Bar->SetPercent(0);
        Bar->SetVisibility(ESlateVisibility::HitTestInvisible);
    }
    return Super::RebuildWidget();
}
void UY01MonsterHealthBarWidget::NativeConstruct()
{
    Super::NativeConstruct();
    if (auto Slate = GetCachedWidget()) { Slate->SetCanTick(false); }
}
void UY01MonsterHealthBarWidget::SetHealth(float Health, float MaxHealth)
{
    if (!Bar) { TakeWidget(); }
    const float Fraction = FMath::IsFinite(Health) && FMath::IsFinite(MaxHealth) && MaxHealth > 0 ? FMath::Clamp(Health / MaxHealth, 0.f, 1.f) : 0.f;
    Bar->SetPercent(Fraction); SetRenderOpacity(1); SetVisibility(ESlateVisibility::HitTestInvisible);
}
void UY01MonsterHealthBarWidget::ResetBar()
{
    if (Bar) { Bar->SetPercent(0); }
    SetRenderOpacity(0); SetRenderTransform(FWidgetTransform()); SetVisibility(ESlateVisibility::Hidden);
}
UY01MonsterHealthBarWorldWidgetComponent::UY01MonsterHealthBarWorldWidgetComponent()
{
    PrimaryComponentTick.bCanEverTick = false; PrimaryComponentTick.bStartWithTickEnabled = false;
    SetTickMode(ETickMode::Disabled); SetWidgetSpace(EWidgetSpace::World); SetDrawSize(FVector2D(128,16));
    SetPivot(FVector2D(.5,.5)); SetBlendMode(EWidgetBlendMode::Transparent);
    SetBackgroundColor(FLinearColor::Transparent); SetTintColorAndOpacity(FLinearColor::White);
    SetCollisionEnabled(ECollisionEnabled::NoCollision); SetGenerateOverlapEvents(false);
    SetCastShadow(false); SetOnlyOwnerSee(true); SetWindowFocusable(false);
}
bool UY01MonsterHealthBarWorldWidgetComponent::DrawPrepared(float DeltaSeconds)
{
#if UE_SERVER
    return false;
#else
    if (!GetWorld() || GetNetMode() == NM_DedicatedServer) { return false; }
    UpdateWidget(); RequestRedraw(); DrawWidgetToRenderTarget(DeltaSeconds); SetComponentTickEnabled(false);
    // NullRHI仅让自动化观察CPU状态；真实渲染必须成功提交新内容后才允许显现。
    return GUsingNullRHI || (GetRenderTarget() && !bRedrawRequested);
#endif
}
