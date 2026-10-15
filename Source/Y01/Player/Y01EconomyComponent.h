#pragma once

#include "Components/ActorComponent.h"
#include "Data/Y01DataCatalog.h"
#include "Player/Y01ItemRuntimeTypes.h"
#include "Y01EconomyComponent.generated.h"

// 经济会话只由可信服务器显式启用，旧模式不自动接入。
UCLASS()
class Y01_API UY01EconomyComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01EconomyComponent();
    bool IsInitialized() const { return bInitialized; }
    FY01ItemResult Initialize(const FGuid& Match, int32 Join, uint64 Config,
        const FY01DataCatalog& Catalog, const TArray<FY01DataRef>& EnabledOffers,
        const TArray<FY01DataRef>& EnabledRecipes = {}, const FY01DataRef& TalentChoice = {},
        const TArray<FY01DataRef>& DropMonsters = {});
    FY01ItemResult TryPurchase(AController* Requester, const FY01ItemRequest& Request);
    FY01ItemResult TrySynthesize(AController* Requester, const FY01ItemRequest& Request);
    FY01ItemResult TryChooseTalent(AController* Requester, const FY01ItemRequest& Request);
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
private:
    friend class UY01LoadoutComponent;
    friend class UY01SourceEffectsComponent;
    friend class UY01GrowthComponent;
    friend class UY01DropDeliveryComponent;
    bool IsPurchasedSource(const FString& SourceId) const;
    bool IsSelectedTalentSource(const FString& SourceId) const;
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FY01R36EconomyTestAccess;
    friend struct FY01R36DeliveryTestAccess;
#endif
    FY01ItemResult MutatePurchase(AController* Requester, const FY01ItemRequest& Request, double Now);
    FY01ItemResult MutateSynthesis(AController* Requester, const FY01ItemRequest& Request, double Now);
    FY01ItemResult MutateChooseTalent(AController* Requester, const FY01ItemRequest& Request, double Now);
    FY01ItemResult CheckSession(const FY01ItemRequest& Request) const;
    FY01DataCatalog SnapshotCatalog() const;
    UPROPERTY() TMap<EY01TableKind, TObjectPtr<UDataTable>> SnapshotTables;
    UPROPERTY() TMap<FName, FY01ShopOfferRow> Offers;
    UPROPERTY() TMap<FName, FY01ShopOfferState> OfferStates;
    UPROPERTY() TArray<TObjectPtr<UClass>> SnapshotClasses;
    UPROPERTY() TMap<FName, FY01SynthesisRow> Recipes;
    UPROPERTY() TArray<FName> OfferOrder;
    UPROPERTY() TArray<FName> RecipeOrder;
    UPROPERTY() FY01TalentChoiceState TalentState;
    FRandomStream ShoppingRandom;
    FRandomStream SynthesisRandom;
    FRandomStream LootRandom;
    FGuid MatchId;
    int32 JoinSequence = 0;
    uint64 ConfigRevision = 0;
    bool bInitialized = false;
    bool bEnded = false;
};
