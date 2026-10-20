#include "Presentation/Y01DamageNumberWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/TextBlock.h"
#include "Engine/Font.h"
#include "Player/Y01AttributeResolver.h"
#include "Widgets/SWidget.h"

TSharedRef<SWidget> UY01DamageNumberWidget::RebuildWidget()
{
    if (!Number)
    {
        Number = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("DamageText"));
        WidgetTree->RootWidget = Number;
        Number->SetFont(FSlateFontInfo(LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/Roboto.Roboto")), 32, TEXT("Bold")));
        Number->SetColorAndOpacity(FSlateColor(FLinearColor::White));
        Number->SetJustification(ETextJustify::Center);
        Number->SetVisibility(ESlateVisibility::HitTestInvisible);
    }
    return Super::RebuildWidget();
}
void UY01DamageNumberWidget::NativeConstruct()
{
    Super::NativeConstruct();
    // 原生UUserWidget默认自动请求Slate Tick；本类不使用它，只有池推进生命周期。
    if (auto Slate = GetCachedWidget()) { Slate->SetCanTick(false); }
}
void UY01DamageNumberWidget::SetNumber(float Amount, const FLinearColor& Color)
{
    ResetNumber();
    if (!Number) { TakeWidget(); }
    if (!FMath::IsFinite(Amount) || Amount <= 0 || !FMath::IsFinite(Color.R) || !FMath::IsFinite(Color.G)
        || !FMath::IsFinite(Color.B) || !FMath::IsFinite(Color.A)) { ResetNumber(); return; }
    Number->SetText(FText::FromString(FY01AttributeResolver::WholeText(Amount)));
    Number->SetColorAndOpacity(FSlateColor(Color));
    SetRenderTransform(FWidgetTransform());
    SetRenderOpacity(1.f);
    SetVisibility(ESlateVisibility::HitTestInvisible);
}
void UY01DamageNumberWidget::ResetNumber()
{
    if (Number) { Number->SetText(FText::GetEmpty()); Number->SetColorAndOpacity(FSlateColor(FLinearColor::White)); }
    SetRenderOpacity(0.f);
    SetRenderTransform(FWidgetTransform());
    SetVisibility(ESlateVisibility::Hidden);
}

UY01DamageWorldWidgetComponent::UY01DamageWorldWidgetComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    SetTickMode(ETickMode::Disabled);
    SetWidgetSpace(EWidgetSpace::World);
    SetDrawSize(FVector2D(256,64));
    SetPivot(FVector2D(.5,.5));
    SetBlendMode(EWidgetBlendMode::Transparent);
    SetBackgroundColor(FLinearColor::Transparent);
    SetTintColorAndOpacity(FLinearColor::White);
    SetCollisionEnabled(ECollisionEnabled::NoCollision);
    SetGenerateOverlapEvents(false);
    SetCastShadow(false);
    SetOnlyOwnerSee(true);
    SetWindowFocusable(false);
}
void UY01DamageWorldWidgetComponent::DrawFromPool(float DeltaSeconds)
{
#if !UE_SERVER
    if (IsVisible() && GetWorld() && GetNetMode() != NM_DedicatedServer)
    {
        UpdateWidget();
        DrawWidgetToRenderTarget(DeltaSeconds);
    }
#endif
}
