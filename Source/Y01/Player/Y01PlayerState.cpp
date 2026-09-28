#include "Player/Y01PlayerState.h"
#include "Player/Y01Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Net/UnrealNetwork.h"

AY01PlayerState::AY01PlayerState()
{
    AbilitySystem = CreateDefaultSubobject<UY01CombatAbilitySystem>(TEXT("AbilitySystem"));
    Attributes = CreateDefaultSubobject<UY01AttributeSet>(TEXT("Attributes"));
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
    if (!AbilitySystem->TryInitializeFromData(Catalog, Character, OutError)) { return false; }
    EquipmentSlots = AbilitySystem->GetInitialEquipment(); EquipmentSlots.SetNum(Y01Data::EquipmentSlotCount);
    SkillSlots = AbilitySystem->GetInitialSkills(); SkillSlots.SetNum(Y01Data::SkillSlotCount);
    if (auto* Avatar = Cast<AY01Character>(GetPawn())) { Avatar->RefreshAbilityActorInfo(); }
    ForceNetUpdate();
    return true;
}
bool AY01PlayerState::TryActivateSkillSlot(int32 Slot)
{
    return HasAuthority() && Slot >= 0 && Slot < Y01Data::SkillSlotCount && AbilitySystem->TryActivateSkillSlot(Slot);
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
