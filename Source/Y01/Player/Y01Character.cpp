#include "Player/Y01Character.h"
#include "Player/Y01PlayerState.h"
#include "Data/Y01DataSubsystem.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/GameInstance.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"

AY01Character::AY01Character()
{
    bReplicates = true;
    GetCapsuleComponent()->InitCapsuleSize(34.f, 88.f);
    GetCharacterMovement()->bOrientRotationToMovement = true;
    bUseControllerRotationYaw = false;
    bUseControllerRotationPitch = false;
    bUseControllerRotationRoll = false;
    CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
    CameraBoom->SetupAttachment(RootComponent);
    CameraBoom->SetUsingAbsoluteRotation(true);
    CameraBoom->SetRelativeRotation(FRotator(-60.f, 0.f, 0.f));
    CameraBoom->TargetArmLength = 850.f;
    CameraBoom->bUsePawnControlRotation = false;
    CameraBoom->bInheritPitch = false;
    CameraBoom->bInheritYaw = false;
    CameraBoom->bInheritRoll = false;
    FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
    FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
    FollowCamera->bUsePawnControlRotation = false;
}
UAbilitySystemComponent* AY01Character::GetAbilitySystemComponent() const
{
    const auto* State = GetPlayerState<AY01PlayerState>();
    return State ? State->GetAbilitySystemComponent() : nullptr;
}
void AY01Character::PossessedBy(AController* NewController)
{
    Super::PossessedBy(NewController);
    RefreshAbilityActorInfo();
    if (auto* State = GetPlayerState<AY01PlayerState>(); State && !State->IsCharacterInitialized() && !CharacterDefinition.RowName.IsNone())
    {
        const auto* Data = GetGameInstance() ? GetGameInstance()->GetSubsystem<UY01DataSubsystem>() : nullptr;
        if (Data && Data->GetCatalog())
        {
            FString Error;
            if (!State->TryInitializeCharacter(*Data->GetCatalog(), CharacterDefinition, Error))
            { UE_LOG(LogTemp, Warning, TEXT("角色初始化失败：%s"), *Error); }
        }
    }
}
void AY01Character::OnRep_PlayerState()
{
    Super::OnRep_PlayerState();
    RefreshAbilityActorInfo();
}
void AY01Character::RefreshAbilityActorInfo()
{
    if (auto* State = GetPlayerState<AY01PlayerState>())
    {
        auto* ASC = State->GetCombatASC();
        ASC->InitAbilityActorInfo(State, this);
        if (ASC->IsCombatInitialized()) { GetCharacterMovement()->MaxWalkSpeed = ASC->GetInitialMoveSpeed(); }
    }
}
