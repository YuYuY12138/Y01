#pragma once

#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "Combat/Y01Combat.h"
#include "Y01Monster.generated.h"

UENUM()
enum class EY01MonsterPresentationKind : uint8 { Unknown, Normal, Boss };

// 完整结构复制；Health/MaxHealth仍以ASC为唯一来源。
USTRUCT()
struct Y01_API FY01MonsterPresentationState
{
    GENERATED_BODY()
    UPROPERTY() bool bRegistered = false;
    UPROPERTY() EY01MonsterPresentationKind Kind = EY01MonsterPresentationKind::Unknown;
    UPROPERTY() FGuid WorldEpoch;
    UPROPERTY() int32 RegionId = INDEX_NONE;
    UPROPERTY() int32 RegionSession = 0;
    UPROPERTY() bool bEverActuallyDamaged = false;
    UPROPERTY() bool bDead = false;
    UPROPERTY() uint64 Revision = 0;
    bool IsRegistered() const;
    bool HasLiveHealthBarEligibility() const
    { return IsRegistered() && Kind == EY01MonsterPresentationKind::Normal && bEverActuallyDamaged && !bDead; }
    bool NetSerialize(FArchive& Ar, UPackageMap* Map, bool& bOutSuccess);
};
template<> struct TStructOpsTypeTraits<FY01MonsterPresentationState> : TStructOpsTypeTraitsBase2<FY01MonsterPresentationState>
{ enum { WithNetSerializer = true }; };

UCLASS()
class Y01_API AY01Monster : public ACharacter, public IAbilitySystemInterface
{
    GENERATED_BODY()
public:
    AY01Monster();
    virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override { return AbilitySystem; }
    UY01CombatAbilitySystem* GetCombatASC() const { return AbilitySystem; }
    bool TryInitializeMonster(const FY01DataCatalog& Catalog, const FY01DataRef& Monster, FString& OutError);
    float GetDetectionRadius() const { return DetectionRadius; }
    // 候选由玩法区域提供；0.1秒重选，当前目标失效/越界每次调用立即裁定。
    AActor* RefreshDetectionTarget(TConstArrayView<AActor*> Candidates, class AY01Core* CoreFallback = nullptr);
    void RefreshMovementPeers(TConstArrayView<AY01Monster*> Candidates);
    FVector ResolveMovementVelocity(TConstArrayView<AY01Monster*> Candidates, const FVector& DesiredVelocity);
    FVector ResolveMovementVelocity(const FVector& DesiredVelocity);
    virtual void BeginPlay() override;
    virtual void EndPlay(EEndPlayReason::Type Reason) override;
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    // 服务器在成功初始化且加入真实玩法容器后登记；主线为(0,0)，私房必须有正会话。
    UFUNCTION() bool RegisterHealthBarPresentation(int32 RegionId, int32 RegionSession);
    void UnregisterHealthBarPresentation();
    FY01MonsterPresentationState GetHealthBarPresentationState() const;
    FSimpleMulticastDelegate OnHealthBarPresentationChanged;
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Y01") FY01DataRef MonsterDefinition;
private:
    friend class UY01AttributeSet;
    void RecordHealthBarHealthChange(bool bActualDamage, bool bDied);
    void EnsurePresentationWorld();
    void PublishPresentation();
    bool IsMovementPeerEligible() const;
    bool SharesMovementDomain(const AY01Monster* Other) const;
    void SetMovementPeer(AY01Monster* Other, bool bIgnore);
    void ClearMovementPeers();
    void RefreshMovementPeersFromWorld();
    void RefreshLocalSeparationCandidates();
    UFUNCTION() void OnRep_PresentationState();
    UPROPERTY(ReplicatedUsing=OnRep_PresentationState) FY01MonsterPresentationState PresentationState;
    FY01MonsterPresentationState AcceptedPresentationState;
    TWeakObjectPtr<UWorld> PresentationWorld;
    TSet<FGuid> RetiredPresentationEpochs;
    uint64 HighestPresentationRevision = 0;
    bool bPresentationEnded = false;
    TSet<TWeakObjectPtr<AY01Monster>> MovementPeers;
    TArray<TWeakObjectPtr<AY01Monster>> SeparationCandidates;
    uint64 MovementRegistrationOrder = 0;
    double NextSeparationRefreshTime = 0;
    UPROPERTY(Replicated) float DetectionRadius = 2000;
    double NextDetectionTime = 0;
    UPROPERTY(VisibleAnywhere) TObjectPtr<UY01CombatAbilitySystem> AbilitySystem;
    UPROPERTY() TObjectPtr<UY01AttributeSet> Attributes;
};
