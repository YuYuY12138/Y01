#include "Playground/Y01PlaygroundRuntime.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01PlaygroundHUD.h"
#include "Playground/Y01TestSkill.h"
#include "Playground/Y01PlaygroundVerification.h"
#include "Playground/Y01MonsterHealthBarVerification.h"
#include "Misc/CommandLine.h"
#include "Combat/Y01Monster.h"
#include "Combat/Y01Core.h"
#include "Playground/Y01R36Profile.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01ReviveComponent.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01DropDeliveryComponent.h"
#include "Match/Y01KillRewards.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"
#include "HAL/PlatformMisc.h"

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
void AY01PlaygroundVisual::OnRep_Target()
{
    RefreshMeshFollow();
}
void AY01PlaygroundVisual::RefreshMeshFollow()
{
    USkeletalMeshComponent* Mesh = IsValid(Target) ? Target->GetMesh() : nullptr;
    if (FollowedMesh.Get() == Mesh) { return; }
    if (USkeletalMeshComponent* Previous = FollowedMesh.Get()) { RemoveTickPrerequisiteComponent(Previous); }
    FollowedMesh = Mesh;
    if (!Mesh) { MeshLocationOffset = FVector::ZeroVector; MeshRotationOffset = FQuat::Identity; return; }
    AddTickPrerequisiteComponent(Mesh);
    const FTransform ActorTransform = Target->GetActorTransform();
    const FVector BaseMeshLocation = ActorTransform.TransformPosition(Target->GetBaseTranslationOffset());
    const FQuat BaseMeshRotation = Target->GetActorQuat() * Target->GetBaseRotationOffset();
    MeshLocationOffset = Target->GetActorLocation() - BaseMeshLocation;
    MeshRotationOffset = Target->GetActorQuat() * BaseMeshRotation.Inverse();
}
void AY01PlaygroundVisual::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!IsValid(Target)) { if (HasAuthority()) { Destroy(); } return; }
    RefreshMeshFollow();
    USkeletalMeshComponent* Mesh = FollowedMesh.Get();
    if (!Mesh) { return; }
    const bool bDead = Y01Playground::Health(Target->GetCombatASC()) <= 0;
    const bool bBoss = Definition.RowName == TEXT("Boss");
    SetActorLocationAndRotation(Mesh->GetComponentLocation() + MeshLocationOffset + FVector(0, 0, bDead ? -60 : 0),
        MeshRotationOffset * Mesh->GetComponentQuat());
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
    Training = CreateDefaultSubobject<UY01TrainingRooms>(TEXT("TrainingRooms"));
    Growth = CreateDefaultSubobject<UY01PlaygroundGrowth>(TEXT("R33TestGrowth"));
}
void AY01PlaygroundGameMode::BeginPlay()
{
    Super::BeginPlay(); bGameplay = IsMatchConfigured() && LoadedProfile != nullptr;
    GetWorld()->GetSubsystem<UY01DamageEvents>()->AudiencePolicy.BindUObject(this, &ThisClass::CaptureDamageAudience);
    Growth->Update();
    AY01MonsterHealthBarVerification::Install(GetWorld());
    for (auto It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    { RefreshHealthBarAuthorization(Cast<AY01PlaygroundController>(It->Get())); }
    if (bGameplay && AY01DamageVerification::Enabled() && GetNetMode() == NM_DedicatedServer)
    { GetWorld()->SpawnActor<AY01DamageVerification>(); }
    if (bGameplay && AY01PlaygroundController::IsVerificationEnabled())
    {
        FY01DataValidation Validation;
        const auto Catalog = LoadedProfile->BuildCatalog();
        const auto* Character = Catalog.FindRow<FY01CharacterRow>(LoadedProfile->Character, Validation);
        int32 ImmediateSkills = 0;
        bool bLegacy = Character != nullptr;
        if (Character)
        {
            for (const auto& Ref : Character->InitialSkills)
            {
                const auto* Skill = Catalog.FindRow<FY01SkillRow>(Ref, Validation);
                if (!Skill) { bLegacy = false; break; }
                if (Skill->Type != EY01SkillType::Active) { continue; }
                ++ImmediateSkills;
                bLegacy &= Skill->TestSkill.Mode == EY01TestSkillMode::None
                    && Skill->Ability.ToSoftObjectPath() == FSoftObjectPath(UY01BasicAttackAbility::StaticClass());
            }
        }
        if (!bLegacy || ImmediateSkills < 2)
        {
            bGameplay = false;
            UE_LOG(LogTemp, Error, TEXT("Y01Playground LEGACY_VERIFY_REJECT Profile=%s: 旧即时Verify仅接受legacy基础GA配置；新范围/飞行物须独立验收。"), *LoadedProfile->GetPathName());
            // Windows在初始化期间的非强制退出只投递WM_QUIT，主循环未启动会丢失失败码。
            FPlatformMisc::RequestExitWithStatus(true, 1);
            return;
        }
        UE_LOG(LogTemp, Display, TEXT("Y01Playground LEGACY_VERIFY_ONLY Profile=%s: 仅旧即时回归，不代表新技能验收。"), *LoadedProfile->GetPathName());
        GetWorld()->SpawnActor<AY01PlaygroundVerification>();
    }
}
UY01PlaygroundProfile* AY01PlaygroundGameMode::SelectProfile(FString& Error)
{
    int32 Count = 0; UY01PlaygroundProfile* Profile = nullptr;
    for (TActorIterator<AY01PlaygroundConfig> It(GetWorld()); It; ++It) { ++Count; Profile = It->Profile; }
    if (Count != 1 || !Profile)
    { Error = FString::Printf(TEXT("MissingConfiguration: Playground needs exactly 1 Config with Profile (found %d)"), Count); return nullptr; }
    return Profile;
}
bool AY01PlaygroundGameMode::LoadMatchConfiguration(FString& Error)
{
    LoadedProfile = SelectProfile(Error);
    if (!LoadedProfile) { return false; }
    const auto* CoreProfile = Cast<UY01R36Profile>(LoadedProfile);
    bCoreDefenseEnabled = CoreProfile && CoreProfile->bCoreDefenseEnabled;
    LoadedProfile = AY01DamageVerification::PrepareProfile(LoadedProfile);
    LoadedProfile = AY01MonsterHealthBarVerification::PrepareProfile(LoadedProfile, AY01MonsterHealthBarVerification::Controlled(FCommandLine::Get()));
    const auto Validation = LoadedProfile->Validate();
    if (!Validation.IsValid())
    {
        Error = TEXT("MissingConfiguration: ") + LoadedProfile->GetPathName();
        for (const auto& Item : Validation.Errors)
        { Error += FString::Printf(TEXT(" | %s.%s: %s"), *Item.Source.RowName.ToString(), *Item.Field, *Item.Message); }
        return false;
    }
    if (bCoreDefenseEnabled && !InitializeMatchCore(Error)) { return false; }
    const bool Configured = TryConfigureMatch(LoadedProfile->BuildCatalog(), LoadedProfile->OrderedWaves, LoadedProfile->Character, Error);
    if (!Configured && bCoreDefenseEnabled) { FreezeCoreDefense(); }
    for (auto It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    { RefreshHealthBarAuthorization(Cast<AY01PlaygroundController>(It->Get())); }
    return Configured;
}
bool AY01PlaygroundGameMode::InitializeMatchCore(FString& Error)
{
    const auto* Profile = Cast<UY01R36Profile>(LoadedProfile);
    AY01Core* Core = nullptr; int32 Count = 0;
    for (TActorIterator<AY01Core> It(GetWorld()); It; ++It)
    { if (!It->IsActorBeingDestroyed()) { Core = *It; ++Count; } }
    if (!Profile || Count != 1)
    { Error = FString::Printf(TEXT("CoreDefense需要唯一地图核心，数量=%d；禁止回退玩家死亡失败策略"), Count); return false; }
    if (!Core->TryInitializeCore(Profile->BuildCatalog(), Profile->CoreRules, Error) || !RegisterMatchCore(Core))
    { if (Error.IsEmpty()) { Error = TEXT("CoreDefense核心登记失败"); } Core->Freeze(); return false; }
    Training->BindCombat(Core->GetCombatASC());
    Core->OnFallen.AddUObject(this, &ThisClass::HandleCoreFallen);
    Core->OnEndPlay.AddDynamic(this, &ThisClass::CoreEnded);
    CoreSpawnHandle = GetWorld()->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateUObject(this, &ThisClass::CoreActorSpawned));
    UE_LOG(LogTemp, Display, TEXT("Y01Core REGISTER Core=%s Rules=%s"), *Core->GetPathName(), *Profile->CoreRules.RowName.ToString());
    return true;
}
bool AY01PlaygroundGameMode::IsGameplayFrozen() const
{
    const auto* Core = GetMatchCore();
    return bCoreDefenseEnabled && (bCoreDefenseFrozen || !IsValid(Core) || Core->IsActorBeingDestroyed()
        || !Core->GetSnapshot().bInitialized || Core->GetSnapshot().bFrozen || Core->GetSnapshot().bFallen);
}
void AY01PlaygroundGameMode::FreezeCoreDefense()
{
    if (!HasAuthority() || !bCoreDefenseEnabled || bCoreDefenseFrozen) { return; }
    bCoreDefenseFrozen = true; bFinishNextTick = false;
    if (auto* Core = GetMatchCore(); IsValid(Core)) { Core->Freeze(); }
    auto Stop = [](UY01CombatAbilitySystem* ASC)
    {
        ASC->UpdateHealthRegen(false); ASC->InvalidateCombatSession();
        if (auto* Character = Cast<ACharacter>(ASC->GetAvatarActor()))
        { Character->GetCharacterMovement()->StopMovementImmediately(); Character->GetCharacterMovement()->DisableMovement(); Character->ConsumeMovementInputVector(); }
    };
    if (GetFlowState())
    {
        for (APlayerState* State : GetFlowState()->PlayerArray)
        {
            if (auto* Player = Cast<AY01PlayerState>(State))
            {
                Player->GetRevive()->Cancel(); Player->GetIncome()->Pause();
                Stop(Player->GetCombatASC());
                for (auto* Monster : Training->Monsters(Player)) { if (IsValid(Monster)) { Stop(Monster->GetCombatASC()); } }
            }
        }
    }
    for (AY01Monster* Monster : GetWaveMonsters()) { if (IsValid(Monster)) { Stop(Monster->GetCombatASC()); } }
    Training->StopAll();
    // 返回传送可能恢复移动模式；训练清理完成后再次锁定。
    if (GetFlowState())
    { for (APlayerState* State : GetFlowState()->PlayerArray)
        { if (auto* P = Cast<AY01PlayerState>(State))
            { if (auto* Pawn = Cast<ACharacter>(P->GetCombatASC()->GetAvatarActor())) { Pawn->GetCharacterMovement()->DisableMovement(); } } } }
}
void AY01PlaygroundGameMode::HandleCoreFallen()
{
    if (!HasAuthority() || bCoreFallen || !GetFlowState() || GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { return; }
    bCoreFallen = true;
    FreezeCoreDefense(); OnCoreFallen();
}
void AY01PlaygroundGameMode::SettleCoreFallen()
{
    if (!bCoreFallen || bSettlingCore || GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { return; }
    TGuardValue<bool> Guard(bSettlingCore, true);
    auto* Kills = GetWorld()->GetSubsystem<UY01KillRewards>();
    Kills->Drain();
    bool bComplete = true;
    const auto Players = GetFlowState()->PlayerArray;
    for (APlayerState* State : Players)
    {
        auto* P = Cast<AY01PlayerState>(State); if (!IsValid(P)) { continue; }
        // 致死调用栈内已冻结但尚未发布的击杀也属于结清边界。
        if (Kills->HasPending(P) || P->GetLoadout()->IsBusy() || P->GetSourceEffects()->IsBusy()) { bComplete = false; continue; }
        if (!P->GetDropDelivery()->SettleOwnedOnFallen()) { bComplete = false; }
    }
    if (!bComplete) { return; }
    bCoreSettlementComplete = true;
    TryEndMatch(false);
}
void AY01PlaygroundGameMode::OnPlayerDeath(AY01PlayerState* Player)
{
    if (HasAuthority() && IsCoreDefenseEnabled()) { Training->ReturnPlayer(Player); }
}
void AY01PlaygroundGameMode::OnCoreFallen()
{
    UE_LOG(LogTemp, Display, TEXT("Y01Core FALLEN Frozen=1；等待已归属奖励结清"));
}
void AY01PlaygroundGameMode::CoreActorSpawned(AActor* Actor)
{
    if (auto* Core = Cast<AY01Core>(Actor); Core && Core != GetMatchCore())
    {
        FreezeCoreDefense(); Core->Freeze();
        UE_LOG(LogTemp, Error, TEXT("Y01Core DUPLICATE Core=%s；已冻结，禁止切换核心"), *Core->GetPathName());
    }
}
void AY01PlaygroundGameMode::CoreEnded(AActor* Actor, EEndPlayReason::Type Reason)
{
    if (GetWorld()->bIsTearingDown || Reason == EEndPlayReason::Quit || Reason == EEndPlayReason::EndPlayInEditor) { return; }
    FreezeCoreDefense();
    UE_LOG(LogTemp, Error, TEXT("Y01Core MISSING Core=%s；已冻结，禁止回退玩家失败策略"), *GetPathNameSafe(Actor));
}
void AY01PlaygroundGameMode::PostLogin(APlayerController* NewPlayer)
{
    Super::PostLogin(NewPlayer);
    RefreshHealthBarAuthorization(Cast<AY01PlaygroundController>(NewPlayer));
}
void AY01PlaygroundGameMode::RefreshHealthBarAuthorization(AY01PlaygroundController* PC) const
{
    if (!HasAuthority() || !PC || PC->GetWorld() != GetWorld()) { return; }
    const auto* Player = PC->GetPlayerState<AY01PlayerState>();
    if (bHealthBarShutdown || !IsMatchConfigured() || !LoadedProfile || !IsPlayerPresent(Player)
        || !Player->IsCharacterInitialized() || GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended)
    { PC->RevokeHealthBarAuthorization(); return; }
    const auto View = Training->Observe(Player);
    PC->SetHealthBarRegion(View.bInside ? View.Room : 0, View.bInside ? View.Session : 0);
}
bool AY01PlaygroundGameMode::IsPlayerPresent(const AY01PlayerState* Player) const
{
    const auto* Pawn = IsValid(Player) ? Cast<APawn>(Player->GetCombatASC()->GetAvatarActor()) : nullptr;
    return IsValid(Player) && !Player->IsActorBeingDestroyed() && !DepartedPlayers.Contains(Player)
        && GetFlowState() && GetFlowState()->PlayerArray.Contains(Player) && IsValid(Pawn) && !Pawn->IsActorBeingDestroyed()
        && Pawn->GetPlayerState() == Player && Pawn->GetController() && Pawn->GetController()->GetPlayerState<AY01PlayerState>() == Player;
}
bool AY01PlaygroundGameMode::CaptureDamageAudience(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target, FY01PendingDamage& Pending)
{
    if (!bGameplay || !Source || !Target || !GetFlowState() || GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { return false; }
    auto AddRecipient = [&](const AY01PlayerState* Recipient, int32 Region)
    {
        if (!IsPlayerPresent(Recipient)) { return; }
        auto* Pawn = Cast<APawn>(Recipient->GetCombatASC()->GetAvatarActor());
        auto* PC = Pawn ? Cast<AY01PlaygroundController>(Pawn->GetController()) : nullptr;
        if (!PC) { return; }
        PC->SetDamageRegion(Region); Pending.Recipients.Add({PC, PC->GetDamageAudience()});
    };
    if (Source == Target)
    {
        if (const auto* Self = Cast<AY01PlayerState>(Source->GetOwner()))
        {
            if (!IsPlayerPresent(Self)) { return false; }
            const auto View = Training->Observe(Self); Pending.Event.RegionId = View.bInside ? View.Room : 0;
            if (View.bInside) { AddRecipient(Self, View.Room); }
            else { for (APlayerState* State : GetFlowState()->PlayerArray) { const auto* R = Cast<AY01PlayerState>(State); if (!Training->Observe(R).bInside) { AddRecipient(R, 0); } } }
            return true;
        }
        if (const auto* SelfMonster = Cast<AY01Monster>(Source->GetAvatarActor()))
        {
            for (APlayerState* State : GetFlowState()->PlayerArray)
            {
                const auto* RoomPlayer = Cast<AY01PlayerState>(State); const auto View = Training->Observe(RoomPlayer);
                if (View.bInside && Training->Monsters(RoomPlayer).Contains(SelfMonster))
                { Pending.Event.RegionId = View.Room; AddRecipient(RoomPlayer, View.Room); return true; }
            }
            if (!GetWaveMonsters().Contains(SelfMonster)) { return false; }
        }
        else if (Source->GetAvatarActor() != GetMatchCore()) { return false; }
        Pending.Event.RegionId = 0;
        for (APlayerState* State : GetFlowState()->PlayerArray) { const auto* R = Cast<AY01PlayerState>(State); if (!Training->Observe(R).bInside) { AddRecipient(R, 0); } }
        return true;
    }
    if (Target->GetAvatarActor() == GetMatchCore())
    {
        const auto* Attacker = Cast<AY01Monster>(Source->GetAvatarActor());
        if (!Attacker || !GetWaveMonsters().Contains(Attacker)) { return false; }
        Pending.Event.RegionId = 0;
        for (APlayerState* State : GetFlowState()->PlayerArray) { const auto* R = Cast<AY01PlayerState>(State); if (!Training->Observe(R).bInside) { AddRecipient(R, 0); } }
        return true;
    }
    const auto* Player = Cast<AY01PlayerState>(Source->GetOwner());
    const auto* Monster = Cast<AY01Monster>(Target->GetAvatarActor());
    const auto* PlayerASC = Source; const auto* MonsterASC = Target;
    if (!Player)
    { Player = Cast<AY01PlayerState>(Target->GetOwner()); Monster = Cast<AY01Monster>(Source->GetAvatarActor()); PlayerASC = Target; MonsterASC = Source; }
    if (!IsPlayerPresent(Player) || Player->GetCombatASC() != PlayerASC || !IsValid(Monster)
        || Monster->GetCombatASC() != MonsterASC || MonsterASC->GetOwner() != Monster) { return false; }
    const auto TrainingView = Training->Observe(Player);
    if (TrainingView.bInside)
    {
        if (!Training->Monsters(Player).Contains(Monster)) { return false; }
        Pending.Event.RegionId = TrainingView.Room;
    }
    else
    {
        if (!IsCombatRunning() || !GetWaveMonsters().Contains(Monster)) { return false; }
        Pending.Event.RegionId = 0;
    }
    // 这里仍处于执行前。提交时不再查询Pawn、死亡状态或CanAttack。
    for (APlayerState* State : GetFlowState()->PlayerArray)
    {
        const auto* Recipient = Cast<AY01PlayerState>(State);
        if (!IsPlayerPresent(Recipient)) { continue; }
        const auto View = Training->Observe(Recipient);
        if (TrainingView.bInside ? Recipient != Player : View.bInside) { continue; }
        auto* Pawn = Cast<APawn>(Recipient->GetCombatASC()->GetAvatarActor());
        auto* PC = Cast<AY01PlaygroundController>(Pawn->GetController());
        if (!PC) { continue; }
        PC->SetDamageRegion(View.bInside ? View.Room : 0);
        Pending.Recipients.Add({PC, PC->GetDamageAudience()});
    }
    return true;
}
bool AY01PlaygroundGameMode::IsCombatRunning() const
{
    const auto* State = GetFlowState();
    return !IsGameplayFrozen() && bGameplay && State && State->GetWaveSnapshot().Phase == EY01MatchPhase::Battle
        && (IsAutomaticWaves() || !State->GetWaveSnapshot().bWaveCompleted);
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
    if (IsAutomaticWaves()) { return true; }
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
    if (GetWaveMonsters().Contains(Monster)) { Monster->RegisterHealthBarPresentation(0, 0); }
    Growth->RegisterMonster(Monster);
    if (LoadedProfile && (LoadedProfile->bTrainingEnabled || LoadedProfile->bR34AutoAttackEnabled || bCoreDefenseEnabled)) { Training->BindCombat(Monster->GetCombatASC()); }
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
    const bool bTraining = Training->Observe(Player).bInside;
    TArray<AY01Monster*> Candidates;
    if (bTraining) { Candidates = Training->Monsters(Player); }
    else { for (AY01Monster* Monster : GetWaveMonsters()) { Candidates.Add(Monster); } }
    if (!IsGameplayFrozen() && (IsCombatRunning() || bTraining) && ASC->GetAvatarActor())
    {
        for (AY01Monster* Monster : Candidates)
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
    { if (auto* Player = Cast<AY01PlayerState>(State); Player && !Training->Observe(Player).bInside) { Player->GetCombatASC()->SetCombatTarget(nullptr); } }
    for (AY01Monster* Monster : GetWaveMonsters())
    {
        if (!IsValid(Monster)) { continue; }
        Monster->GetCombatASC()->SetCombatTarget(nullptr);
        Monster->GetCharacterMovement()->StopMovementImmediately();
    }
}
void AY01PlaygroundGameMode::OnWaveRewardCommitted()
{
    if (!IsAutomaticWaves()) { ClearTargets(); }
    bFinishNextTick = ShouldEndOnFinalWave() && LoadedProfile && GetFlowState()->GetWaveSnapshot().CompletedWaves >= LoadedProfile->OrderedWaves.Num();
}
void AY01PlaygroundGameMode::Tick(float DeltaSeconds)
{
    // 自动调度前先裁定死亡；同帧胜利候选不能覆盖失败，也不能继续生成到期批次。
    if (HasAuthority() && bGameplay && GetFlowState() && ShouldEndOnPlayerDeath()
        && GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Waiting)
    {
        for (APlayerState* State : GetFlowState()->PlayerArray)
        {
            if (const auto* Player = Cast<AY01PlayerState>(State); Player && Player->IsCharacterInitialized()
                && (!IsAutomaticWaves() || IsPlayerPresent(Player)) && Y01Playground::Health(Player->GetCombatASC()) <= 0)
            { TryEndMatch(false); break; }
        }
    }
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || !bGameplay || !GetFlowState()) { return; }
    if (IsGameplayFrozen()) { FreezeCoreDefense(); SettleCoreFallen(); return; }
    if (IsCoreDefenseEnabled())
    { for (APlayerState* State : GetFlowState()->PlayerArray)
        { if (auto* P = Cast<AY01PlayerState>(State)) { P->GetRevive()->Update(); } } }
    Growth->Update();
    if (bFinishNextTick) { bFinishNextTick = false; TryEndMatch(true); }
    for (APlayerState* State : GetFlowState()->PlayerArray)
    {
        if (auto* Player = Cast<AY01PlayerState>(State))
        {
            if (ShouldEndOnPlayerDeath() && Player->IsCharacterInitialized()
                && (!IsAutomaticWaves() || IsPlayerPresent(Player)) && Y01Playground::Health(Player->GetCombatASC()) <= 0
                && GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Waiting)
            { TryEndMatch(false); }
            if (LoadedProfile->bTrainingEnabled || LoadedProfile->bR34AutoAttackEnabled || bCoreDefenseEnabled) { Training->BindCombat(Player->GetCombatASC()); }
            if (LoadedProfile->bR34AutoAttackEnabled)
            { Player->GetCombatASC()->UpdateHealthRegen(IsPlayerPresent(Player) && GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Ended); }
        }
    }
    if (LoadedProfile->bR34AutoAttackEnabled)
    {
        for (AY01Monster* Monster : GetWaveMonsters())
        { if (IsValid(Monster)) { Monster->GetCombatASC()->UpdateHealthRegen(GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Ended); } }
    }
    Training->Update();
    for (APlayerState* State : GetFlowState()->PlayerArray)
    {
        auto* Player = Cast<AY01PlayerState>(State); RefreshTarget(Player);
        if (LoadedProfile->bR34AutoAttackEnabled && IsPlayerPresent(Player)) { AY01TestAttack::TryAutoAttack(Player->GetCombatASC()); }
    }
    if (!IsCombatRunning()) { ClearTargets(); return; }
    TArray<AActor*> DetectionCandidates;
    for (APlayerState* State : GetFlowState()->PlayerArray)
    {
        auto* Player = Cast<AY01PlayerState>(State);
        if (IsPlayerPresent(Player) && !Training->Observe(Player).bInside && Y01Playground::Health(Player->GetCombatASC()) > 0)
        { DetectionCandidates.Add(Player->GetCombatASC()->GetAvatarActor()); }
    }
    for (AY01Monster* Monster : GetWaveMonsters())
    {
        if (!IsValid(Monster)) { continue; }
        auto* ASC = Monster->GetCombatASC();
        if (Y01Playground::Health(ASC) <= 0)
        { Monster->SetActorEnableCollision(false); Monster->GetCharacterMovement()->DisableMovement(); ASC->SetCombatTarget(nullptr); continue; }
        if (IsGameplayFrozen()) { break; }
        AActor* Nearest = Monster->RefreshDetectionTarget(DetectionCandidates, bCoreDefenseEnabled ? GetMatchCore() : nullptr);
        const bool bAuto = LoadedProfile->bR34AutoAttackEnabled || bCoreDefenseEnabled;
        if (Nearest && (bAuto ? ASC->IsAutoAttackInRange(Nearest) : ASC->IsValidCombatTarget(Nearest)))
        {
            Monster->GetCharacterMovement()->StopMovementImmediately();
            if (bAuto) { AY01TestAttack::TryAutoAttack(ASC); }
            else { ASC->TryActivateSkillSlot(0); }
        }
        else if (Nearest)
        {
            const float MaxSpeed = Monster->GetCharacterMovement()->MaxWalkSpeed;
            const FVector Desired = (Nearest->GetActorLocation() - Monster->GetActorLocation()).GetSafeNormal2D() * MaxSpeed;
            if (MaxSpeed > KINDA_SMALL_NUMBER) { Monster->AddMovementInput(Monster->ResolveMovementVelocity(Desired) / MaxSpeed); }
        }
    }
    TryCompleteWave();
}
bool AY01PlaygroundGameMode::RequestAdvance(AY01PlaygroundController* Controller)
{
    if (!HasAuthority() || !bGameplay || IsAutomaticWaves() || !Controller || Controller->GetWorld() != GetWorld() || !Controller->PlayerState) { return false; }
    ClearTargets(); return TryStartNextWave();
}
bool AY01PlaygroundGameMode::TryEndMatch(bool bSuccess)
{
    if (IsCoreDefenseEnabled() && IsGameplayFrozen() && (!bCoreSettlementComplete || bSuccess)) { return false; }
    if (bSuccess && ShouldEndOnPlayerDeath() && GetFlowState())
    {
        for (APlayerState* State : GetFlowState()->PlayerArray)
        {
            if (const auto* Player = Cast<AY01PlayerState>(State); Player && Player->IsCharacterInitialized()
                && (!IsAutomaticWaves() || IsPlayerPresent(Player)) && Y01Playground::Health(Player->GetCombatASC()) <= 0)
            { bSuccess = false; break; }
        }
    }
    if (!Super::TryEndMatch(bSuccess)) { return false; }
    for (auto It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    { if (auto* PC = Cast<AY01PlayerController>(It->Get())) { PC->RevokeHealthBarAuthorization(); } }
    if (auto* Core = GetMatchCore(); IsValid(Core)) { Core->Freeze(); }
    Growth->StopAll(TEXT("Ended"));
    for (APlayerState* State : GetFlowState()->PlayerArray)
    { if (auto* Player = Cast<AY01PlayerState>(State))
        { if (IsCoreDefenseEnabled()) { Player->GetRevive()->Cancel(); } Player->GetCombatASC()->InvalidateCombatSession(); } }
    for (AY01Monster* Monster : GetWaveMonsters())
    { if (IsValid(Monster)) { Monster->GetCombatASC()->InvalidateCombatSession(); } }
    Training->StopAll(); return true;
}
void AY01PlaygroundGameMode::Logout(AController* Exiting)
{
    if (Exiting)
    {
        if (auto* PC = Cast<AY01PlayerController>(Exiting)) { PC->RevokeHealthBarAuthorization(); PC->SetDamageRegion(INDEX_NONE); }
        auto* Player = Exiting->GetPlayerState<AY01PlayerState>();
        if (Player) { DepartedPlayers.Add(Player); Player->GetCombatASC()->UpdateHealthRegen(false); Player->GetCombatASC()->InvalidateCombatSession(); }
        Growth->StopPlayer(Player, TEXT("登出"));
        Training->Remove(Player);
    }
    Super::Logout(Exiting);
}
void AY01PlaygroundGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
    bHealthBarShutdown = true;
    GetWorld()->RemoveOnActorSpawnedHandler(CoreSpawnHandle);
    if (auto* Core = GetMatchCore(); IsValid(Core))
    { Core->OnFallen.RemoveAll(this); Core->OnEndPlay.RemoveDynamic(this, &ThisClass::CoreEnded); Core->Freeze(); }
    for (auto It = GetWorld()->GetPlayerControllerIterator(); It; ++It)
    { if (auto* PC = Cast<AY01PlayerController>(It->Get())) { PC->RevokeHealthBarAuthorization(); } }
    GetWorld()->GetSubsystem<UY01DamageEvents>()->AudiencePolicy.Unbind();
    Growth->StopAll(TEXT("EndPlay")); Training->StopAll(); Super::EndPlay(Reason);
}
bool AY01PlaygroundGameMode::RequestRestart(AY01PlaygroundController* Controller)
{
    if (!HasAuthority() || !Controller || Controller->GetWorld() != GetWorld()) { return false; }
    // 明确采用停止重开方案，避免ServerTravel复用内存中的旧DataTable。
    Controller->ShowRestartNotice(); return true;
}
