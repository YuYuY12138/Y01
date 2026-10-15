#include "Match/Y01SpawnPoint.h"
#include "Combat/Y01Monster.h"
#include "Components/SceneComponent.h"
#include "Components/ArrowComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"

AY01SpawnPoint::AY01SpawnPoint()
{
    PrimaryActorTick.bCanEverTick = false;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
#if WITH_EDITORONLY_DATA
    auto* Arrow = CreateEditorOnlyDefaultSubobject<UArrowComponent>(TEXT("SpawnDirection"));
    if (Arrow) { Arrow->SetupAttachment(RootComponent); Arrow->ArrowSize = 2; Arrow->SetHiddenInGame(true); }
#endif
}
bool AY01SpawnPoint::ReadPointTag(FName& OutTag, FString& OutError) const
{
    OutTag = NAME_None; OutError.Reset();
    if (Tags.Num() != 1 || !Y01Data::ValidSpawnPointTag(PointKind, Tags[0]))
    {
        FString Names; for (FName Tag : Tags) { Names += Tag.ToString() + TEXT(" "); }
        OutError = FString::Printf(TEXT("Object=%s Kind=%d Tags=[%s]: 点必须仅有一个Actor Tag；Normal为正int32规范数字，Boss为Boss Tag，禁止type mismatch"),
            *GetPathName(), static_cast<int32>(PointKind), *Names); return false;
    }
    if (GetActorTransform().ContainsNaN())
    { OutError = FString::Printf(TEXT("Object=%s: 出生点变换非有限"), *GetPathName()); return false; }
    OutTag = Tags[0]; return true;
}
bool AY01SpawnPoint::BuildRegistry(UWorld* World, TMap<FName, TWeakObjectPtr<AY01SpawnPoint>>& OutPoints, FString& OutError)
{
    OutPoints.Reset(); OutError.Reset();
    if (!World || World->GetNetMode() == NM_Client) { OutError = TEXT("出生点注册仅允许权威World"); return false; }
    TMap<FName, TWeakObjectPtr<AY01SpawnPoint>> Pending;
    for (TActorIterator<AY01SpawnPoint> It(World); It; ++It)
    {
        if (It->IsActorBeingDestroyed()) { continue; }
        FName Tag; FString Error;
        if (!It->ReadPointTag(Tag, Error)) { OutError += Error + TEXT(" | "); continue; }
        if (const auto* Existing = Pending.Find(Tag))
        {
            OutError += FString::Printf(TEXT("Point=%s duplicate Object=%s Object=%s | "),
                *Tag.ToString(), *GetPathNameSafe(Existing->Get()), *It->GetPathName());
        }
        else { Pending.Add(Tag, *It); }
    }
    if (!OutError.IsEmpty()) { return false; }
    OutPoints = MoveTemp(Pending); return true;
}
bool AY01SpawnPoint::TryFindMonsterTransform(int32& CandidateCursor, FTransform& OutTransform, FString& OutError) const
{
    OutError.Reset(); FName Tag;
    if (!HasAuthority() || IsActorBeingDestroyed() || !GetWorld() || !ReadPointTag(Tag, OutError))
    { if (OutError.IsEmpty()) { OutError = TEXT("出生点已失效或非权威"); } return false; }
    const auto* Default = GetDefault<AY01Monster>(); const auto* Capsule = Default->GetCapsuleComponent();
    const double Radius = Capsule->GetScaledCapsuleRadius(), HalfHeight = Capsule->GetScaledCapsuleHalfHeight();
    const double Spacing = Radius * 2 + 8;
    FCollisionQueryParams Query(SCENE_QUERY_STAT(Y01SpawnPlacement), false, this);
    FCollisionObjectQueryParams GroundObjects; GroundObjects.AddObjectTypesToQuery(ECC_WorldStatic);
    GroundObjects.AddObjectTypesToQuery(ECC_WorldDynamic);
    for (int32 Budget = 0; Budget < 128 && CandidateCursor >= 0 && CandidateCursor < 4096; ++Budget)
    {
        const int32 N = CandidateCursor++;
        int32 X = 0, Y = 0;
        if (N > 0)
        {
            const int32 Ring = FMath::CeilToInt((FMath::Sqrt(static_cast<double>(N + 1)) - 1) / 2);
            const int32 Side = Ring * 2, Offset = N - (Side - 1) * (Side - 1);
            const int32 Edge = Offset / Side, Along = Offset % Side;
            if (Edge == 0) { X = Ring; Y = -Ring + 1 + Along; }
            else if (Edge == 1) { X = Ring - 1 - Along; Y = Ring; }
            else if (Edge == 2) { X = -Ring; Y = Ring - 1 - Along; }
            else { X = -Ring + 1 + Along; Y = -Ring; }
        }
        const FVector Candidate = GetActorLocation() + FVector(X * Spacing, Y * Spacing, 0);
        FHitResult Ground;
        if (!GetWorld()->LineTraceSingleByObjectType(Ground, Candidate + FVector(0, 0, 500), Candidate - FVector(0, 0, 2000), GroundObjects, Query)
            || !Ground.bBlockingHit || Ground.bStartPenetrating || Ground.ImpactNormal.Z < Default->GetCharacterMovement()->GetWalkableFloorZ()) { continue; }
        const FVector Location = Ground.ImpactPoint + FVector(0, 0, HalfHeight + 2);
        if (Location.ContainsNaN() || GetWorld()->OverlapBlockingTestByProfile(Location, FQuat::Identity, Capsule->GetCollisionProfileName(),
            FCollisionShape::MakeCapsule(Radius + 1, HalfHeight + 1), Query)) { continue; }
        OutTransform = FTransform(FRotator(0, GetActorRotation().Yaw, 0), Location); return true;
    }
    OutError = FString::Printf(TEXT("Point=%s Object=%s 无合法地面/胶囊空位，候选预算耗尽 Cursor=%d PerMonster=128 PerBatch=4096"),
        *Tag.ToString(), *GetPathName(), CandidateCursor); return false;
}
