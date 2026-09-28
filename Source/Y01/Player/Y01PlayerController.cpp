#include "Player/Y01PlayerController.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/LocalPlayer.h"
#include "Player/Y01Character.h"
#include "Player/Y01PlayerState.h"

void AY01PlayerController::RequestSkillSlot(int32 Slot)
{
    if (Slot < 0 || Slot >= Y01Data::SkillSlotCount) { return; }
    if (HasAuthority()) { ServerRequestSkillSlot_Implementation(Slot); }
    else if (IsLocalController()) { ServerRequestSkillSlot(Slot); }
}
void AY01PlayerController::SetupInputComponent()
{
    Super::SetupInputComponent();
    BuildKeyboardMapping();
    if (auto* Input = Cast<UEnhancedInputComponent>(InputComponent))
    {
        Input->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ThisClass::Move);
        for (int32 Slot = 0; Slot < Y01Data::SkillSlotCount; ++Slot)
        { Input->BindAction(SkillActions[Slot], ETriggerEvent::Started, this, &ThisClass::Skill, Slot); }
    }
    InstallMapping();
}
void AY01PlayerController::BeginPlay()
{
    Super::BeginPlay();
    if (IsLocalController()) { BuildKeyboardMapping(); InstallMapping(); }
}
void AY01PlayerController::EndPlay(EEndPlayReason::Type Reason)
{
    if (auto* Local = GetLocalPlayer())
    {
        if (auto* Subsystem = Local->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(); Subsystem && KeyboardMapping)
        { Subsystem->RemoveMappingContext(KeyboardMapping); }
    }
    Super::EndPlay(Reason);
}
void AY01PlayerController::BuildKeyboardMapping()
{
    if (KeyboardMapping) { return; }
    KeyboardMapping = NewObject<UInputMappingContext>(this, TEXT("KeyboardMapping"));
    MoveAction = NewObject<UInputAction>(this, TEXT("Move"));
    MoveAction->ValueType = EInputActionValueType::Axis2D;
    auto MapMove = [&](FKey Key, bool bNegate, bool bSwap)
    {
        auto& Mapping = KeyboardMapping->MapKey(MoveAction, Key);
        if (bNegate) { Mapping.Modifiers.Add(NewObject<UInputModifierNegate>(KeyboardMapping)); }
        if (bSwap)
        {
            auto* Swizzle = NewObject<UInputModifierSwizzleAxis>(KeyboardMapping);
            Swizzle->Order = EInputAxisSwizzle::YXZ; Mapping.Modifiers.Add(Swizzle);
        }
    };
    MapMove(EKeys::W, false, true); MapMove(EKeys::S, true, true);
    MapMove(EKeys::A, true, false); MapMove(EKeys::D, false, false);
    const FKey Keys[] = { EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four };
    for (int32 Slot = 0; Slot < Y01Data::SkillSlotCount; ++Slot)
    {
        auto* Action = NewObject<UInputAction>(this, FName(*FString::Printf(TEXT("SkillSlot%d"), Slot + 1)));
        Action->ValueType = EInputActionValueType::Boolean;
        SkillActions.Add(Action); KeyboardMapping->MapKey(Action, Keys[Slot]);
    }
}
void AY01PlayerController::InstallMapping()
{
    if (auto* Local = GetLocalPlayer())
    {
        if (auto* Subsystem = Local->GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(); Subsystem && KeyboardMapping)
        { Subsystem->AddMappingContext(KeyboardMapping, 0); }
    }
}
void AY01PlayerController::Move(const FInputActionValue& Value)
{
    auto* Avatar = Cast<AY01Character>(GetPawn());
    if (!Avatar || IsMoveInputIgnored()) { return; }
    const FVector2D Axis = Value.Get<FVector2D>();
    if (!FMath::IsFinite(Axis.X) || !FMath::IsFinite(Axis.Y)) { return; }
    const FVector Direction = FVector(Axis.Y, Axis.X, 0).GetClampedToMaxSize(1.f);
    Avatar->AddMovementInput(FVector::ForwardVector, Direction.X);
    Avatar->AddMovementInput(FVector::RightVector, Direction.Y);
}
void AY01PlayerController::Skill(const FInputActionValue& Value, int32 Slot) { RequestSkillSlot(Slot); }
void AY01PlayerController::ServerRequestSkillSlot_Implementation(int32 Slot)
{
    if (!HasAuthority() || Slot < 0 || Slot >= Y01Data::SkillSlotCount) { return; }
    auto* State = GetPlayerState<AY01PlayerState>();
    auto* Avatar = Cast<AY01Character>(GetPawn());
    if (!State || !Avatar || Avatar->GetController() != this || Avatar->GetPlayerState() != State
        || State->GetCombatASC()->GetAvatarActor() != Avatar) { return; }
    State->TryActivateSkillSlot(Slot);
}
