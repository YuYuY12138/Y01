#include "Player/Y01ShieldComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01ReviveComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"

UY01ShieldComponent::UY01ShieldComponent()
{ PrimaryComponentTick.bCanEverTick = true; PrimaryComponentTick.TickInterval = .1f; SetIsReplicatedByDefault(true); }
AY01PlayerState* UY01ShieldComponent::Player() const { return Cast<AY01PlayerState>(GetOwner()); }
void UY01ShieldComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{ Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UY01ShieldComponent, TotalShield); }
void UY01ShieldComponent::RecomputeTotal()
{
    double Total = 0;
    for (const auto& L : Layers) { Total += L.Value; }
    TotalShield = FMath::IsFinite(Total) && Total > 0 ? Total : 0;
}
void UY01ShieldComponent::ResetLayers()
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) { return; }
    Layers.Reset(); NextPeriodic.Reset(); RecomputeTotal();
}
void UY01ShieldComponent::GrantLayer(const FString& LayerKey, double Value, double LayerCap, double DurationSeconds)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !FMath::IsFinite(Value) || Value <= 0
        || !FMath::IsFinite(LayerCap) || LayerCap <= 0 || !FMath::IsFinite(DurationSeconds) || DurationSeconds <= 0) { return; }
    const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.;
    FLayer* Existing = Layers.FindByPredicate([&](const FLayer& L) { return L.Key == LayerKey; });
    if (Existing)
    {
        Existing->Value = FMath::Min(LayerCap, Existing->Value + Value);
        Existing->Expires = Now + DurationSeconds;
    }
    else { Layers.Add({LayerKey, FMath::Min(Value, LayerCap), LayerCap, Now + DurationSeconds}); }
    RecomputeTotal();
}
void UY01ShieldComponent::Consume(double Amount)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !FMath::IsFinite(Amount) || Amount <= 0 || Layers.IsEmpty()) { return; }
    double Left = Amount;
    while (Left > 0)
    {
        int32 Soonest = INDEX_NONE;
        for (int32 I = 0; I < Layers.Num(); ++I)
        { if (Layers[I].Value > 0 && (Soonest == INDEX_NONE || Layers[I].Expires < Layers[Soonest].Expires)) { Soonest = I; } }
        if (Soonest == INDEX_NONE) { break; }
        const double Take = FMath::Min(Left, Layers[Soonest].Value);
        Layers[Soonest].Value -= Take; Left -= Take;
    }
    Layers.RemoveAll([](const FLayer& L) { return L.Value <= 0; });
    RecomputeTotal();
}
void UY01ShieldComponent::NotifyCast(const FString& CastSourceId)
{
    auto* P = Player();
    if (!GetOwner() || !GetOwner()->HasAuthority() || !P || CastSourceId.IsEmpty()) { return; }
    for (const auto& Record : P->GetSourceEffects()->Records)
    {
        if (Record.Source.Id != CastSourceId) { continue; }
        for (const auto& E : Record.Effects)
        {
            if (E.Kind == EY01SourceEffectKind::ShieldOnCast)
            { GrantLayer(CastSourceId + TEXT("/") + E.EffectId.ToString(), E.Amount, E.LayerCap, E.DurationSeconds); }
        }
    }
}
void UY01ShieldComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    auto* P = Player();
    if (!P || !P->HasAuthority()) { return; }
    // 死亡等待/复活期间挂起（与 Y01Combat UpdateHealthRegen 的门一致）：不清理过期层、不结算周期获得，
    // 护盾不累积；挂起期间不做 NextPeriodic 清理——复活后首个 tick 重新发现周期来源时，
    // 按"Next 已过→立即获得一层并推进"路径继续，视为合法的"再次获得"。
    if (auto* Revive = P->GetRevive()) { if (Revive->BlocksActions()) { return; } }
    const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.;
    const int32 Expired = Layers.RemoveAll([&](const FLayer& L) { return L.Expires <= Now; });
    // 周期来源同步：新提交立即获得首层，之后按各自间隔。
    TSet<FString> Live;
    for (const auto& Record : P->GetSourceEffects()->Records)
    {
        for (const auto& E : Record.Effects)
        {
            if (E.Kind != EY01SourceEffectKind::ShieldPeriodic) { continue; }
            const FString Key = Record.Source.Id + TEXT("/") + E.EffectId.ToString();
            Live.Add(Key);
            const double* Next = NextPeriodic.Find(Key);
            if (!Next)
            {
                GrantLayer(Key, E.Amount, E.LayerCap, E.DurationSeconds);
                NextPeriodic.Add(Key, Now + E.IntervalSeconds);
            }
            else if (Now >= *Next)
            {
                GrantLayer(Key, E.Amount, E.LayerCap, E.DurationSeconds);
                NextPeriodic[Key] = *Next + E.IntervalSeconds * FMath::FloorToDouble((Now - *Next) / E.IntervalSeconds + 1.);
            }
        }
    }
    for (auto It = NextPeriodic.CreateIterator(); It; ++It) { if (!Live.Contains(It.Key())) { It.RemoveCurrent(); } }
    // 层自然过期不经过GrantLayer/ResetLayers，过期后需显式重算总量。
    if (Expired > 0) { RecomputeTotal(); }
}
