#pragma once
#include "GameFramework/HUD.h"
#include "Y01PlaygroundHUD.generated.h"

UCLASS()
class AY01PlaygroundHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
};
