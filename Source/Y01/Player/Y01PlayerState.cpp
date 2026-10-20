#include "Player/Y01PlayerState.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01EconomyComponent.h"
#include "Player/Y01GrowthComponent.h"
#include "Player/Y01DropDeliveryComponent.h"
#include "Player/Y01DamageEffectsComponent.h"
#include "Player/Y01Character.h"
#include "Player/Y01ReviveComponent.h"
#include "Player/Y01ShieldComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"
#include "Match/Y01GameFlow.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"

AY01PlayerState::AY01PlayerState()
{
    AbilitySystem = CreateDefaultSubobject<UY01CombatAbilitySystem>(TEXT("AbilitySystem"));
    Attributes = CreateDefaultSubobject<UY01AttributeSet>(TEXT("Attributes"));
    SourceEffects = CreateDefaultSubobject<UY01SourceEffectsComponent>(TEXT("SourceEffects"));
    Income = CreateDefaultSubobject<UY01IncomeComponent>(TEXT("Income"));
    Loadout = CreateDefaultSubobject<UY01LoadoutComponent>(TEXT("Loadout"));
    Economy = CreateDefaultSubobject<UY01EconomyComponent>(TEXT("Economy"));
    Growth = CreateDefaultSubobject<UY01GrowthComponent>(TEXT("Growth"));
    DropDelivery = CreateDefaultSubobject<UY01DropDeliveryComponent>(TEXT("DropDelivery"));
    DamageEffects = CreateDefaultSubobject<UY01DamageEffectsComponent>(TEXT("DamageEffects"));
    Revive = CreateDefaultSubobject<UY01ReviveComponent>(TEXT("Revive"));
    Shield = CreateDefaultSubobject<UY01ShieldComponent>(TEXT("Shield"));
    AbilitySystem->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);
    EquipmentSlots.SetNum(Y01Data::EquipmentSlotCount);
    SkillSlots.SetNum(Y01Data::SkillSlotCount);
    SetNetUpdateFrequency(30.f);
}
void AY01PlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME(AY01PlayerState, Gold);
    DOREPLIFETIME(AY01PlayerState, Experience);
    DOREPLIFETIME(AY01PlayerState, EquipmentSlots);
    DOREPLIFETIME(AY01PlayerState, SkillSlots);
}
bool AY01PlayerState::TryInitializeCharacter(const FY01DataCatalog& Catalog, const FY01DataRef& Character, FString& OutError)
{
    OutError.Reset();
    if (!HasAuthority() || Character.Type != EY01TableKind::Character)
    { OutError = TEXT("角色初始化需要服务器及Character引用"); return false; }
    if (Loadout->IsInitialized()) { OutError = TEXT("新物品会话启用后不得通过旧入口重置角色或槽位"); return false; }
    if (bEquipmentChanging) { OutError = TEXT("装备事务期间不能重入初始化"); return false; }
    if (!AbilitySystem->TryInitializeFromData(Catalog, Character, OutError)) { return false; }
    EquipmentSlots.Reset(); EquipmentSlots.SetNum(Y01Data::EquipmentSlotCount);
    for (const auto& Grant : AbilitySystem->GetGrantedSources())
    {
        if (Grant.Source.Type == EY01TableKind::Equipment && EquipmentSlots.IsValidIndex(Grant.Slot))
        { EquipmentSlots[Grant.Slot] = Grant.Source; }
    }
    SkillSlots = AbilitySystem->GetInitialSkills(); SkillSlots.SetNum(Y01Data::SkillSlotCount);
    // 属性初始化成功后清空护盾层：初入局/重开均经此路径回到无盾状态；
    // 对局内死亡清空见 Y01Combat 死亡分支（ResetLayers）；复活后护盾保持空，由周期护盾来源按周期重新获得。
    Shield->ResetLayers();
    if (auto* Avatar = Cast<AY01Character>(GetPawn())) { Avatar->RefreshAbilityActorInfo(); }
    ForceNetUpdate();
    return true;
}
bool AY01PlayerState::TryActivateSkillSlot(int32 Slot)
{
    return HasAuthority() && !Revive->BlocksActions() && Slot >= 0 && Slot < Y01Data::SkillSlotCount && AbilitySystem->TryActivateSkillSlot(Slot);
}
bool AY01PlayerState::TrySetEquipmentSlot(const FY01DataCatalog& Catalog, AController* Requester, int32 Slot, const FY01DataRef& Equipment, FString& OutError)
{
    OutError.Reset();
    const auto* State = GetWorld() ? GetWorld()->GetGameState<AY01GameState>() : nullptr;
    const auto* Avatar = Cast<APawn>(AbilitySystem->GetAvatarActor());
    if (!HasAuthority() || !IsCharacterInitialized() || bEquipmentChanging || Loadout->IsInitialized() || IsActorBeingDestroyed()
        || !IsValid(Requester) || !Requester->HasAuthority() || Requester->IsActorBeingDestroyed() || Requester->GetWorld() != GetWorld()
        || Requester->GetPlayerState<AY01PlayerState>() != this || !IsValid(Avatar) || !Avatar->HasAuthority()
        || Avatar->IsActorBeingDestroyed() || Avatar->GetPlayerState() != this || Requester->GetPawn() != Avatar || Avatar->GetController() != Requester
        || !State || !State->PlayerArray.Contains(this) || State->GetWaveSnapshot().Phase == EY01MatchPhase::Ended
        || Revive->BlocksActions() || (GetWorld()->GetAuthGameMode<AY01GameMode>() && GetWorld()->GetAuthGameMode<AY01GameMode>()->IsGameplayFrozen())
        || AbilitySystem->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) <= 0 || !EquipmentSlots.IsValidIndex(Slot))
    { OutError = TEXT("装备操作拒绝：权限/拥有者/初始化/存活/阶段/槽位不合法"); return false; }
    TGuardValue<bool> Guard(bEquipmentChanging, true);
    if (!AbilitySystem->TrySetEquipmentSlot(Catalog, Slot, Equipment, OutError)) { return false; }
    EquipmentSlots[Slot] = Equipment;
    ForceNetUpdate(); return true;
}
bool AY01PlayerState::CanAddMatchReward(int32 GoldDelta, int32 ExperienceDelta) const
{
    return HasAuthority() && GoldDelta >= 0 && ExperienceDelta >= 0
        && GoldDelta <= MAX_int32 - Gold && ExperienceDelta <= MAX_int32 - Experience;
}
bool AY01PlayerState::TryAddMatchReward(int32 GoldDelta, int32 ExperienceDelta)
{
    if (!CanAddMatchReward(GoldDelta, ExperienceDelta)) { return false; }
    Gold += GoldDelta; Experience += ExperienceDelta;
    ForceNetUpdate();
    return true;
}
bool AY01PlayerState::TryAddFractionalGold(double Amount)
{
    if (!HasAuthority() || !FMath::IsFinite(Amount) || Amount < 0) { return false; }
    const double Next = RewardRemainder + Amount;
    if (!FMath::IsFinite(Next) || Next < 0 || (Amount > 0 && Next == RewardRemainder)) { return false; }
    if (Next < 1.) { RewardRemainder = Next; return true; }
    const double Whole = FMath::FloorToDouble(Next);
    if (Whole >= 2147483648.0 || static_cast<int64>(Whole) > static_cast<int64>(MAX_int32) - Gold) { return false; }
    if (!TryAddMatchReward(static_cast<int32>(Whole), 0)) { return false; }
    RewardRemainder = Next - Whole;
    return true;
}
