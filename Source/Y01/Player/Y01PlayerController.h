#pragma once

#include "GameFramework/PlayerController.h"
#include "InputActionValue.h"
#include "Y01PlayerController.generated.h"

class UInputAction;
class UInputMappingContext;

UCLASS()
class Y01_API AY01PlayerController : public APlayerController
{
    GENERATED_BODY()
public:
    void RequestSkillSlot(int32 Slot);
    const UInputMappingContext* GetKeyboardMapping() const { return KeyboardMapping; }
    const UInputAction* GetMoveAction() const { return MoveAction; }
    const UInputAction* GetSkillAction(int32 Slot) const { return SkillActions.IsValidIndex(Slot) ? SkillActions[Slot] : nullptr; }
protected:
    virtual void SetupInputComponent() override;
    virtual void BeginPlay() override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
private:
    void BuildKeyboardMapping();
    void InstallMapping();
    void Move(const FInputActionValue& Value);
    void Skill(const FInputActionValue& Value, int32 Slot);
    UFUNCTION(Server, Reliable) void ServerRequestSkillSlot(int32 Slot);
    UPROPERTY(Transient) TObjectPtr<UInputMappingContext> KeyboardMapping;
    UPROPERTY(Transient) TObjectPtr<UInputAction> MoveAction;
    UPROPERTY(Transient) TArray<TObjectPtr<UInputAction>> SkillActions;
};
