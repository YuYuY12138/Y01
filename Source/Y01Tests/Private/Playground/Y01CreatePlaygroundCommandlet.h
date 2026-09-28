#pragma once

#include "Commandlets/Commandlet.h"
#include "Y01CreatePlaygroundCommandlet.generated.h"

UCLASS()
class UY01CreatePlaygroundCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UY01CreatePlaygroundCommandlet();
    virtual int32 Main(const FString& Params) override;
};
