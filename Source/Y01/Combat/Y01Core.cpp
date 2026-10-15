#include "Combat/Y01Core.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/SizeBox.h"
#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateColorBrush.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

FString FY01CoreSnapshot::AlertText() const
{
    if (!bInitialized) { return {}; }
    if (bFallen) { return TEXT("大本营已失守"); }
    if (AlertSequence > 0 && ServerSampleTime >= LastAlertTime && ServerSampleTime - LastAlertTime < Settings.HitAlertInterval)
    { return IsCritical() ? TEXT("大本营遭到攻击 · 生命危急") : TEXT("大本营遭到攻击"); }
    return {};
}
bool FY01CoreSnapshot::NetSerialize(FArchive& Ar, UPackageMap*, bool& bOutSuccess)
{
    Ar << bInitialized << bFallen << bFrozen << Health << MaxHealth;
    Ar << Settings.MaxHealth << Settings.HealthRegen << Settings.HitAlertInterval << Settings.CriticalHealthRatio;
    Ar << HitSequence << LastHitTime << AlertSequence << LastAlertTime << ServerSampleTime;
    bOutSuccess = !Ar.IsError() && Settings.IsValid() && FMath::IsFinite(Health) && FMath::IsFinite(MaxHealth)
        && Health >= 0 && Health <= MaxHealth && (!bInitialized || MaxHealth > 0)
        && FMath::IsFinite(ServerSampleTime) && FMath::IsFinite(LastHitTime) && FMath::IsFinite(LastAlertTime);
    return true;
}
TSharedRef<SWidget> UY01CoreHealthWidget::RebuildWidget()
{
    if (!Bar)
    {
        auto* Box = WidgetTree->ConstructWidget<UVerticalBox>(); WidgetTree->RootWidget = Box;
        Label = WidgetTree->ConstructWidget<UTextBlock>(); Label->SetJustification(ETextJustify::Center);
        auto Font = Label->GetFont(); Font.Size = 18; Label->SetFont(Font); Box->AddChild(Label);
        auto* Size = WidgetTree->ConstructWidget<USizeBox>(); Size->SetHeightOverride(18); Box->AddChild(Size);
        Bar = WidgetTree->ConstructWidget<UProgressBar>(); Size->SetContent(Bar);
        FProgressBarStyle Style;
        Style.SetBackgroundImage(FSlateColorBrush(FLinearColor(.025f,.025f,.025f,1)));
        Style.SetFillImage(FSlateColorBrush(FLinearColor::White)); Bar->SetWidgetStyle(Style);
        Bar->SetBarFillStyle(EProgressBarFillStyle::Scale); Bar->SetBorderPadding(FVector2D(2,2));
        SetVisibility(ESlateVisibility::HitTestInvisible);
    }
    return Super::RebuildWidget();
}
void UY01CoreHealthWidget::Present(const FY01CoreSnapshot& S)
{
    if (!Bar) { TakeWidget(); }
    const bool Red = S.bFallen || S.IsCritical() || S.IsHitFlash();
    Bar->SetPercent(S.MaxHealth > 0 ? S.Health / S.MaxHealth : 0);
    Bar->SetFillColorAndOpacity(Red ? FLinearColor(.95f,.04f,.03f) : FLinearColor(.05f,.8f,.25f));
    Label->SetColorAndOpacity(FSlateColor(Red ? FLinearColor(1,.12f,.08f) : FLinearColor::White));
    Label->SetText(FText::FromString(FString::Printf(TEXT("大本营 %.0f / %.0f%s"), S.Health, S.MaxHealth,
        S.bFallen ? TEXT(" · 已失守") : S.IsCritical() ? TEXT(" · 生命危急") : TEXT(""))));
}
AY01Core::AY01Core()
{
    bReplicates = true; bAlwaysRelevant = true; AutoPossessAI = EAutoPossessAI::Disabled;
    PrimaryActorTick.bCanEverTick = true; PrimaryActorTick.TickInterval = .05f; SetNetUpdateFrequency(20);
    Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("CombatCapsule")); RootComponent = Capsule;
    Capsule->InitCapsuleSize(60, 90); Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
    Capsule->SetCollisionObjectType(ECC_Pawn); Capsule->SetCollisionResponseToAllChannels(ECR_Overlap);
    Capsule->SetGenerateOverlapEvents(false); Capsule->SetCanEverAffectNavigation(false);
    Shape = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("CoreShape")); Shape->SetupAttachment(RootComponent);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    Shape->SetStaticMesh(Cube.Object); Shape->SetRelativeScale3D(FVector(1.2,1.2,1.8));
    Shape->SetCollisionEnabled(ECollisionEnabled::NoCollision); Shape->SetCanEverAffectNavigation(false);
    AbilitySystem = CreateDefaultSubobject<UY01CombatAbilitySystem>(TEXT("AbilitySystem"));
    Attributes = CreateDefaultSubobject<UY01AttributeSet>(TEXT("Attributes"));
    AbilitySystem->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);
    // 尚未登记也拒绝所有攻防，不能因地图多放一只而成为可攻击目标。
    AbilitySystem->TargetPermission = [](const auto*, const auto*) { return false; };
    HealthWidget = CreateDefaultSubobject<UWidgetComponent>(TEXT("CoreWorldHealth")); HealthWidget->SetupAttachment(RootComponent);
    HealthWidget->SetRelativeLocation(FVector(0,0,145)); HealthWidget->SetWidgetSpace(EWidgetSpace::Screen);
    HealthWidget->SetDrawSize(FVector2D(340,52)); HealthWidget->SetWidgetClass(UY01CoreHealthWidget::StaticClass());
    HealthWidget->SetCollisionEnabled(ECollisionEnabled::NoCollision); HealthWidget->SetGenerateOverlapEvents(false);
    HealthWidget->SetWindowFocusable(false); HealthWidget->SetVisibility(false);
}
bool AY01Core::TryInitializeCore(const FY01DataCatalog& Catalog, const FY01DataRef& Rules, FString& Error)
{
    Error.Reset();
    if (!HasAuthority() || Snapshot.bInitialized || Snapshot.bFrozen || IsActorBeingDestroyed())
    { Error = TEXT("核心必须为尚未初始化且未冻结的权威Pawn"); return false; }
    const auto Validation = Catalog.Validate(); FY01DataValidation Query;
    const auto* Row = Catalog.FindRow<FY01MatchRulesRow>(Rules, Query);
    if (!Validation.IsValid() || Rules.Type != EY01TableKind::MatchRules || !Row || !Row->Core.IsValid())
    { Error = TEXT("核心需要完整合法目录及MatchRules引用"); return false; }
    AbilitySystem->InitAbilityActorInfo(this, this);
    if (!AbilitySystem->TryInitializeCore(Row->Core, Error)) { return false; }
    Snapshot.Settings = Row->Core; Snapshot.bInitialized = true; Sample(); return true;
}
void AY01Core::BeginPlay()
{
    Super::BeginPlay(); AbilitySystem->InitAbilityActorInfo(this, this); OnRep_Snapshot();
}
void AY01Core::Sample()
{
    if (!HasAuthority() || !Snapshot.bInitialized) { return; }
    Snapshot.Health = AbilitySystem->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    Snapshot.MaxHealth = AbilitySystem->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute());
    Snapshot.ServerSampleTime = GetWorld()->GetTimeSeconds(); ForceNetUpdate(); OnRep_Snapshot();
}
void AY01Core::RecordHealthChange(bool bActualDamage, bool bDied)
{
    if (!HasAuthority() || !Snapshot.bInitialized || Snapshot.bFallen) { return; }
    const double Now = GetWorld()->GetTimeSeconds();
    if (bActualDamage)
    {
        ++Snapshot.HitSequence; Snapshot.LastHitTime = Now;
        if (Snapshot.AlertSequence == 0 || Now - Snapshot.LastAlertTime >= Snapshot.Settings.HitAlertInterval)
        { ++Snapshot.AlertSequence; Snapshot.LastAlertTime = Now; }
    }
    if (bDied) { Snapshot.bFallen = true; Snapshot.bFrozen = true; }
    Sample();
    if (bDied) { AbilitySystem->UpdateHealthRegen(false); OnFallen.Broadcast(); }
}
void AY01Core::Freeze()
{
    if (!HasAuthority() || Snapshot.bFrozen) { return; }
    Snapshot.bFrozen = true; AbilitySystem->UpdateHealthRegen(false); AbilitySystem->InvalidateCombatSession(); Sample();
}
void AY01Core::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority() && Snapshot.bInitialized)
    { AbilitySystem->UpdateHealthRegen(!Snapshot.bFrozen && !Snapshot.bFallen); Sample(); }
}
void AY01Core::OnRep_Snapshot()
{
    if (GetNetMode() == NM_DedicatedServer) { return; }
    HealthWidget->SetVisibility(Snapshot.bInitialized);
    if (!Snapshot.bInitialized) { return; }
    HealthWidget->InitWidget();
    if (auto* Widget = Cast<UY01CoreHealthWidget>(HealthWidget->GetUserWidgetObject())) { Widget->Present(Snapshot); }
}
void AY01Core::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(AY01Core, Snapshot);
}
