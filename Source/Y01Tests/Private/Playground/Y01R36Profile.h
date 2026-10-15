#pragma once
#include "Playground/Y01PlaygroundProfile.h"
#include "Y01R36Profile.generated.h"

// 独立开发配置；公开引用用于内容发现，不是按钮或客户端领物白名单。
UCLASS(BlueprintType)
class UY01R36Profile : public UY01PlaygroundProfile
{
    GENERATED_BODY()
public:
    virtual void Serialize(FArchive& Ar) override;
    virtual void PostLoad() override;
    UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="R36") int32 ContentVersion = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36") TArray<FY01DataRef> ContentEquipment;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36") TArray<FY01DataRef> ContentSkills;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36|Lifecycle") bool bEvidenceMode = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36|Core") bool bCoreDefenseEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36|Core") FY01DataRef CoreRules = {EY01TableKind::MatchRules, TEXT("Default")};
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36|Economy") bool bEconomyEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36|Economy") TArray<FY01DataRef> ContentShopOffers;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36|Economy") TArray<FY01DataRef> ContentRecipes;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36|Economy") FY01DataRef TalentChoice;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36|Economy") int32 InitialGold = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36|Economy") double BaseGoldPerSecond = 0;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36|Attributes") TArray<FY01DataRef> BaseAttributeMappings;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="R36|Delivery", meta=(ClampMin="0")) double DropFlightSeconds = 1;
protected:
    virtual void ValidateExtension(FY01DataValidation& Result) const override;
};
