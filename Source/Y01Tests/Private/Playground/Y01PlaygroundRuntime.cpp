#include "Playground/Y01PlaygroundRuntime.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01PlaygroundHUD.h"
#include "Playground/Y01PlaygroundVerification.h"
#include "Combat/Y01Monster.h"
#include "Player/Y01PlayerState.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

float Y01Playground::Health(const UY01CombatAbilitySystem* ASC)
{ return ASC ? ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) : 0; }
void Y01Playground::ColorShape(UStaticMeshComponent* Shape, const FLinearColor& Color)
{
    if (!Shape || Shape->GetWorld()->GetNetMode() == NM_DedicatedServer) { return; }
    auto* Material = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
    auto* Dynamic = UMaterialInstanceDynamic::Create(Material, Shape);
    Dynamic->SetVectorParameterValue(TEXT("Color"), Color);
    Shape->SetMaterial(0, Dynamic);
}
AY01PlaygroundConfig::AY01PlaygroundConfig()
{ RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root")); }
AY01PlaygroundCharacter::AY01PlaygroundCharacter()
{
    Shape = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("PlaygroundShape"));
    Shape->SetupAttachment(RootComponent);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> ShapeAsset(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
    Shape->SetStaticMesh(ShapeAsset.Object); Shape->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Shape->SetRelativeScale3D(FVector(0.65, 0.65, 1.65));
    if (auto* Boom = FindComponentByClass<USpringArmComponent>())
    { Boom->TargetArmLength = 1700; Boom->bDoCollisionTest = false; Boom->SetRelativeRotation(FRotator(-65, 0, 0)); }
}
void AY01PlaygroundCharacter::BeginPlay()
{ Super::BeginPlay(); Y01Playground::ColorShape(Shape, FLinearColor(0.02f, 0.85f, 0.95f)); }
AY01PlaygroundVisual::AY01PlaygroundVisual()
{
    bReplicates = true; bAlwaysRelevant = true; PrimaryActorTick.bCanEverTick = true;
    Shape = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Shape")); RootComponent = Shape;
    static ConstructorHelpers::FObjectFinder<UStaticMesh> ShapeAsset(TEXT("/Engine/BasicShapes/Cube.Cube"));
    Shape->SetStaticMesh(ShapeAsset.Object); Shape->SetCollisionEnabled(ECollisionEnabled::NoCollision);
}
void AY01PlaygroundVisual::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{ Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(AY01PlaygroundVisual, Target); DOREPLIFETIME(AY01PlaygroundVisual, Definition); }
void AY01PlaygroundVisual::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!IsValid(Target)) { if (HasAuthority()) { Destroy(); } return; }
    const bool bDead = Y01Playground::Health(Target->GetCombatASC()) <= 0;
    const bool bBoss = Definition.RowName == TEXT("Boss");
    SetActorLocation(Target->GetActorLocation() + FVector(0, 0, bDead ? -60 : 0));
    if (!bStyled || bDead != bWasDead)
    {
        SetActorScale3D(FVector(bBoss ? 1.0 : 0.65, bBoss ? 1.0 : 0.65, bDead ? 0.2 : (bBoss ? 1.7 : 1.4)));
        Y01Playground::ColorShape(Shape, bDead ? FLinearColor(0.3f, 0.01f, 0.01f) :
            (bBoss ? FLinearColor(0.9f, 0.03f, 0.12f) : FLinearColor(1.f, 0.3f, 0.02f)));
        bStyled = true; bWasDead = bDead;
    }
}
AY01PlaygroundGameMode::AY01PlaygroundGameMode()
{
    PrimaryActorTick.bCanEverTick = true;
    DefaultPawnClass = AY01PlaygroundCharacter::StaticClass();
    PlayerControllerClass = AY01PlaygroundController::StaticClass();
    HUDClass = AY01PlaygroundHUD::StaticClass();
}
void AY01PlaygroundGameMode::BeginPlay()
{
    Super::BeginPlay(); bGameplay = IsMatchConfigured() && LoadedProfile != nullptr;
    if (bGameplay && AY01PlaygroundController::IsVerificationEnabled())
    { GetWorld()->SpawnActor<AY01PlaygroundVerification>(); }
}
bool AY01PlaygroundGameMode::LoadMatchConfiguration(FString& Error)
{
    int32 Count = 0;
    for (TActorIterator<AY01PlaygroundConfig> It(GetWorld()); It; ++It) { ++Count; LoadedProfile = It->Profile; }
    if (Count != 1 || !LoadedProfile)
    { Error = FString::Printf(TEXT("MissingConfiguration: Playground needs exactly 1 Config with Profile (found %d)"), Count); return false; }
    const auto Validation = LoadedProfile->Validate();
    if (!Validation.IsValid())
    {
        Error = TEXT("MissingConfiguration: ") + LoadedProfile->GetPathName();
        for (const auto& Item : Validation.Errors)
        { Error += FString::Printf(TEXT(" | %s.%s: %s"), *Item.Source.RowName.ToString(), *Item.Field, *Item.Message); }
        return false;
    }
    return TryConfigureMatch(LoadedProfile->BuildCatalog(), LoadedProfile->OrderedWaves, LoadedProfile->Character, Error);
}
bool AY01PlaygroundGameMode::IsCombatRunning() const
{
    const auto* State = GetFlowState();
    return bGameplay && State && State->GetWaveSnapshot().Phase == EY01MatchPhase::Battle && !State->GetWaveSnapshot().bWaveCompleted;
}
int32 AY01PlaygroundGameMode::AliveCount() const
{
    int32 Count = 0;
    for (const AY01Monster* Monster : GetWaveMonsters())
    { if (IsValid(Monster) && Y01Playground::Health(Monster->GetCombatASC()) > 0) { ++Count; } }
    return Count;
}
bool AY01PlaygroundGameMode::CanCompleteWave() const { return AliveCount() == 0; }
bool AY01PlaygroundGameMode::CanStartWave() const
{
    for (AY01Monster* Monster : GetWaveMonsters()) { if (IsValid(Monster)) { Monster->Destroy(); } }
    return true;
}
FTransform AY01PlaygroundGameMode::GetMonsterSpawnTransform(int32 Ordinal) const
{
    const double Angle = (Ordinal % 8) * UE_PI / 4;
    return FTransform(FVector(350 * FMath::Cos(Angle), 350 * FMath::Sin(Angle), 90));
}
void AY01PlaygroundGameMode::OnWaveMonsterSpawned(AY01Monster* Monster)
{
    Monster->GetCharacterMovement()->bRunPhysicsWithNoController = true;
    Monster->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
    auto* Visual = GetWorld()->SpawnActor<AY01PlaygroundVisual>();
    Visual->Target = Monster; Visual->Definition = Monster->GetCombatASC()->GetDefinition();
}
void AY01PlaygroundGameMode::RefreshTarget(AY01PlayerState* Player)
{
    if (!Player) { return; }
    auto* ASC = Player->GetCombatASC();
    AActor* Nearest = nullptr; double Distance = TNumericLimits<double>::Max();
    if (IsCombatRunning() && ASC->GetAvatarActor())
    {
        for (AY01Monster* Monster : GetWaveMonsters())
        {
            if (!ASC->IsValidCombatTarget(Monster)) { continue; }
            const double D = FVector::DistSquared(ASC->GetAvatarActor()->GetActorLocation(), Monster->GetActorLocation());
            if (D < Distance) { Distance = D; Nearest = Monster; }
        }
    }
    ASC->SetCombatTarget(Nearest);
}
void AY01PlaygroundGameMode::ClearTargets()
{
    if (!GetFlowState()) { return; }
    for (APlayerState* State : GetFlowState()->PlayerArray)
    { if (auto* Player = Cast<AY01PlayerState>(State)) { Player->GetCombatASC()->SetCombatTarget(nullptr); } }
    for (AY01Monster* Monster : GetWaveMonsters())
    {
        if (!IsValid(Monster)) { continue; }
        Monster->GetCombatASC()->SetCombatTarget(nullptr);
        Monster->GetCharacterMovement()->StopMovementImmediately();
    }
}
void AY01PlaygroundGameMode::OnWaveRewardCommitted()
{
    ClearTargets();
    bFinishNextTick = LoadedProfile && GetFlowState()->GetWaveSnapshot().CompletedWaves >= LoadedProfile->OrderedWaves.Num();
}
void AY01PlaygroundGameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || !bGameplay || !GetFlowState()) { return; }
    if (bFinishNextTick) { bFinishNextTick = false; TryEndMatch(true); }
    for (APlayerState* State : GetFlowState()->PlayerArray)
    {
        if (auto* Player = Cast<AY01PlayerState>(State))
        {
            if (Y01Playground::Health(Player->GetCombatASC()) <= 0 && GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Waiting)
            { TryEndMatch(false); }
            RefreshTarget(Player);
        }
    }
    if (!IsCombatRunning()) { ClearTargets(); return; }
    for (AY01Monster* Monster : GetWaveMonsters())
    {
        if (!IsValid(Monster)) { continue; }
        auto* ASC = Monster->GetCombatASC();
        if (Y01Playground::Health(ASC) <= 0)
        { Monster->SetActorEnableCollision(false); Monster->GetCharacterMovement()->DisableMovement(); ASC->SetCombatTarget(nullptr); continue; }
        AActor* Nearest = nullptr; double Distance = TNumericLimits<double>::Max();
        for (APlayerState* State : GetFlowState()->PlayerArray)
        {
            auto* Player = Cast<AY01PlayerState>(State);
            AActor* Avatar = Player ? Player->GetCombatASC()->GetAvatarActor() : nullptr;
            if (!IsValid(Avatar) || Y01Playground::Health(Player->GetCombatASC()) <= 0) { continue; }
            const double D = FVector::DistSquared(Monster->GetActorLocation(), Avatar->GetActorLocation());
            if (D < Distance) { Distance = D; Nearest = Avatar; }
        }
        ASC->SetCombatTarget(Nearest);
        if (Nearest && ASC->IsValidCombatTarget(Nearest))
        { Monster->GetCharacterMovement()->StopMovementImmediately(); ASC->TryActivateSkillSlot(0); }
        else if (Nearest)
        { Monster->AddMovementInput((Nearest->GetActorLocation() - Monster->GetActorLocation()).GetSafeNormal2D()); }
    }
    TryCompleteWave();
}
bool AY01PlaygroundGameMode::RequestAdvance(AY01PlaygroundController* Controller)
{
    if (!HasAuthority() || !bGameplay || !Controller || Controller->GetWorld() != GetWorld() || !Controller->PlayerState) { return false; }
    ClearTargets(); return TryStartNextWave();
}
bool AY01PlaygroundGameMode::RequestRestart(AY01PlaygroundController* Controller)
{
    if (!HasAuthority() || !Controller || Controller->GetWorld() != GetWorld()) { return false; }
    // 明确采用停止重开方案，避免ServerTravel复用内存中的旧DataTable。
    Controller->ShowRestartNotice(); return true;
}
