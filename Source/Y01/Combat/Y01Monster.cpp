#include "Combat/Y01Monster.h"
#include "Data/Y01DataSubsystem.h"
#include "Engine/GameInstance.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"

AY01Monster::AY01Monster()
{
    AbilitySystem = CreateDefaultSubobject<UY01CombatAbilitySystem>(TEXT("AbilitySystem"));
    Attributes = CreateDefaultSubobject<UY01AttributeSet>(TEXT("Attributes"));
    AbilitySystem->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);
    bReplicates = true;
    AutoPossessAI = EAutoPossessAI::Disabled;
    GetCapsuleComponent()->InitCapsuleSize(34.f, 88.f);
}
bool AY01Monster::TryInitializeMonster(const FY01DataCatalog& Catalog, const FY01DataRef& Monster, FString& OutError)
{
    OutError.Reset();
    if (!HasAuthority() || Monster.Type != EY01TableKind::Monster)
    { OutError = TEXT("怪物初始化需要服务器及Monster引用"); return false; }
    AbilitySystem->InitAbilityActorInfo(this, this);
    if (!AbilitySystem->TryInitializeFromData(Catalog, Monster, OutError)) { return false; }
    GetCharacterMovement()->MaxWalkSpeed = AbilitySystem->GetInitialMoveSpeed();
    return true;
}
void AY01Monster::BeginPlay()
{
    Super::BeginPlay();
    AbilitySystem->InitAbilityActorInfo(this, this);
    if (HasAuthority() && !MonsterDefinition.RowName.IsNone())
    {
        const auto* Data = GetGameInstance() ? GetGameInstance()->GetSubsystem<UY01DataSubsystem>() : nullptr;
        if (Data && Data->GetCatalog())
        {
            FString Error;
            if (!TryInitializeMonster(*Data->GetCatalog(), MonsterDefinition, Error))
            { UE_LOG(LogTemp, Warning, TEXT("怪物初始化失败：%s"), *Error); }
        }
    }
}
