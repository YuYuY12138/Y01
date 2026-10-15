#include "Playground/Y01MonsterHealthBarVerification.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01PlaygroundProfile.h"
#include "Player/Y01PlayerState.h"
#include "Presentation/Y01MonsterHealthBarPoolComponent.h"
#include "Presentation/Y01MonsterHealthBarWidget.h"
#include "Combat/Y01Monster.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/ProgressBar.h"
#include "Components/StaticMeshComponent.h"
#include "Blueprint/WidgetTree.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/HUD.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMesh.h"
#include "EngineUtils.h"
#include "SceneView.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Dom/JsonObject.h"
#include "UObject/UnrealType.h"

namespace
{
using FObject = TSharedPtr<FJsonObject>;
FObject Object() { return MakeShared<FJsonObject>(); }
FString Encode(const FObject& O)
{
    FString Text; auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text);
    FJsonSerializer::Serialize(O.ToSharedRef(), Writer); return Text;
}
void Emit(const FObject& O)
{
    O->SetNumberField(TEXT("wall_ns"), (FDateTime::UtcNow() - FDateTime(1970,1,1)).GetTotalMicroseconds() * 1000.);
    UE_LOG(LogTemp, Display, TEXT("Y01R27 %s"), *Encode(O));
}
void Position(const FObject& O, const FVector& P)
{ O->SetNumberField(TEXT("x"),P.X); O->SetNumberField(TEXT("y"),P.Y); O->SetNumberField(TEXT("z"),P.Z); }
TArray<FY01MonsterHealthBarLease>* Entries(AY01PlayerController* PC)
{
    for (auto* Component : PC->GetComponents())
    {
        if (auto* Pool = Cast<UY01MonsterHealthBarPoolComponent>(Component); IsValid(Pool) && !Pool->IsBeingDestroyed())
        {
            auto* Property = FindFProperty<FArrayProperty>(Pool->GetClass(), TEXT("Entries"));
            return Property ? Property->ContainerPtrToValuePtr<TArray<FY01MonsterHealthBarLease>>(Pool) : nullptr;
        }
    }
    return nullptr;
}
}
AY01MonsterHealthBarVerification::AY01MonsterHealthBarVerification()
{ PrimaryActorTick.bCanEverTick = true; PrimaryActorTick.TickGroup = TG_PostUpdateWork; }
bool AY01MonsterHealthBarVerification::Controlled(const TCHAR* CommandLine)
{ return FParse::Param(CommandLine, TEXT("Y01R27Verify")); }
UY01PlaygroundProfile* AY01MonsterHealthBarVerification::PrepareProfile(UY01PlaygroundProfile* Source, bool bControlled)
{
    if (!Source || !bControlled) { return Source; }
    auto* P = DuplicateObject<UY01PlaygroundProfile>(Source, GetTransientPackage());
    for (auto& Pair : P->Tables) { Pair.Value = DuplicateObject<UDataTable>(Pair.Value, P); }
    P->bTrainingEnabled = true; P->bR34AutoAttackEnabled = false;
    for (auto& Pair : P->Tables[EY01TableKind::Character]->GetRowMap())
    {
        auto* R = reinterpret_cast<FY01CharacterRow*>(Pair.Value);
        R->Health = 1000; R->Armor = 0; R->HealthRegen = 0; R->MoveSpeed = 0; R->InitialEquipment.Reset(); R->R33TestGrowth.bEnabled = false;
    }
    for (auto& Pair : P->Tables[EY01TableKind::Monster]->GetRowMap())
    {
        auto* R = reinterpret_cast<FY01MonsterRow*>(Pair.Value);
        R->Health = 100; R->Armor = 0; R->HealthRegen = 0; R->MoveSpeed = 0; R->Skills.Reset();
    }
    for (auto& Pair : P->Tables[EY01TableKind::TrainingStage]->GetRowMap())
    {
        auto* R = reinterpret_cast<FY01TrainingStageRow*>(Pair.Value); R->MaxAlive = 2; R->SpawnInterval = 1000;
        for (auto& Spawn : R->Spawns) { Spawn.Count = 1; Spawn.DelaySeconds = 0; }
    }
    return P;
}
FVector AY01MonsterHealthBarVerification::GridPosition(int32 Index)
{ return FVector((Index / 13 - 4.5) * 170, (Index % 13 - 6) * 170, 100); }
bool AY01MonsterHealthBarVerification::Damage(UY01CombatAbilitySystem* Source, AY01Monster* Target, float Amount)
{
    if (!Source || !IsValid(Target) || !Source->GetAvatarActor() || !Source->GetOwner()->HasAuthority() || !FMath::IsFinite(Amount) || Amount <= 0) { return false; }
    auto* Avatar = Source->GetAvatarActor(); const FVector Old = Avatar->GetActorLocation();
    Avatar->SetActorLocation(Target->GetActorLocation() + FVector(100,0,0), false, nullptr, ETeleportType::TeleportPhysics);
    const float Before = Y01Playground::Health(Target->GetCombatASC());
    auto Spec = Source->MakeOutgoingSpec(UY01DamageEffect::StaticClass(),1,Source->MakeEffectContext());
    Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage,Amount); Source->ApplyGameplayEffectSpecToTarget(*Spec.Data,Target->GetCombatASC());
    Avatar->SetActorLocation(Old, false, nullptr, ETeleportType::TeleportPhysics); Target->ForceNetUpdate();
    const float After = Y01Playground::Health(Target->GetCombatASC());
    auto O = Object(); O->SetStringField(TEXT("kind"),TEXT("damage")); O->SetStringField(TEXT("target"),Target->GetName());
    O->SetNumberField(TEXT("before"),Before); O->SetNumberField(TEXT("health"),After); O->SetNumberField(TEXT("actual"),Before-After);
    O->SetStringField(TEXT("epoch"),Target->GetHealthBarPresentationState().WorldEpoch.ToString(EGuidFormats::Digits)); Emit(O);
    return After < Before && Target->GetHealthBarPresentationState().bEverActuallyDamaged;
}
FString AY01MonsterHealthBarVerification::Observe(AY01PlayerController* PC)
{
    if (!PC || !PC->GetWorld()) { return TEXT("{}"); }
    const auto Scope = PC->GetHealthBarAuthorization(); auto O = Object(); const auto Settings = GetDefault<UY01MonsterHealthBarSettings>()->Snapshot();
    O->SetStringField(TEXT("kind"),TEXT("snapshot")); O->SetStringField(TEXT("mode"),Controlled(FCommandLine::Get()) ? TEXT("controlled") : TEXT("natural"));
    O->SetNumberField(TEXT("net"),PC->GetNetMode()); O->SetBoolField(TEXT("null_rhi"),GUsingNullRHI);
    O->SetNumberField(TEXT("world"),PC->GetWorld()->GetTimeSeconds()); O->SetNumberField(TEXT("frame"),GFrameCounter);
    O->SetNumberField(TEXT("prewarm"),Settings.PrewarmCount); O->SetNumberField(TEXT("cap"),Settings.Capacity); O->SetNumberField(TEXT("hold"),Settings.DeathHoldSeconds);
    O->SetStringField(TEXT("epoch"),Scope.WorldEpoch.ToString(EGuidFormats::Digits)); O->SetNumberField(TEXT("region"),Scope.RegionId);
    O->SetNumberField(TEXT("session"),Scope.RegionSession); O->SetStringField(TEXT("generation"),LexToString(Scope.Generation));
    FVector Eye; FRotator Rotation; PC->GetPlayerViewPoint(Eye,Rotation); FSceneViewProjectionData Projection; bool View = false;
    const auto* Local = PC->GetLocalPlayer();
    if (Local && Local->ViewportClient && Local->ViewportClient->Viewport) { View = Local->GetProjectionData(Local->ViewportClient->Viewport, Projection); }
    const FMatrix Matrix = View ? Projection.ComputeViewProjectionMatrix() : FMatrix::Identity;
    TArray<TSharedPtr<FJsonValue>> Candidates, Leases;
    for (TActorIterator<AY01Monster> It(PC->GetWorld()); It; ++It)
    {
        if (It->IsActorBeingDestroyed()) { continue; }
        const auto S = It->GetHealthBarPresentationState(); const FVector Anchor = It->GetActorLocation() + FVector(0,0,It->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()+20);
        const auto Clip = Matrix.TransformFVector4(FVector4(Anchor,1));
        auto C = Object(); C->SetStringField(TEXT("name"),It->GetName()); C->SetNumberField(TEXT("distance"),FVector::DistSquared(View ? Projection.ViewOrigin : Eye,It->GetActorLocation()));
        C->SetBoolField(TEXT("eligible"),S.bEverActuallyDamaged && S.Kind == EY01MonsterPresentationKind::Normal && S.IsRegistered());
        C->SetBoolField(TEXT("in_view"),View && Clip.W > 0 && FMath::Abs(Clip.X) <= Clip.W && FMath::Abs(Clip.Y) <= Clip.W && Clip.Z >= 0 && Clip.Z <= Clip.W);
        C->SetBoolField(TEXT("dead"),S.bDead); C->SetNumberField(TEXT("kind"),static_cast<int32>(S.Kind));
        C->SetNumberField(TEXT("health"),Y01Playground::Health(It->GetCombatASC())); C->SetNumberField(TEXT("maximum"),It->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()));
        C->SetNumberField(TEXT("region"),S.RegionId); C->SetNumberField(TEXT("session"),S.RegionSession); C->SetStringField(TEXT("epoch"),S.WorldEpoch.ToString(EGuidFormats::Digits));
        C->SetNumberField(TEXT("anchor_z"),Anchor.Z); Position(C,It->GetActorLocation()); Candidates.Add(MakeShared<FJsonValueObject>(C));
    }
    int32 Count = 0, Active = 0, Visible = 0;
    if (const auto* E = Entries(PC))
    {
        Count = E->Num();
        for (int32 I=0; I<E->Num(); ++I)
        {
            const auto& L = (*E)[I]; if (!L.Version) { continue; } ++Active;
            auto J = Object(); J->SetNumberField(TEXT("slot"),I); J->SetStringField(TEXT("target"),GetNameSafe(L.Target.Get())); J->SetStringField(TEXT("version"),LexToString(L.Version));
            const bool bVisible = L.Display && L.Actor && L.Display->IsVisible() && !L.Actor->IsHidden(); Visible += bVisible;
            J->SetBoolField(TEXT("visible"),bVisible); J->SetBoolField(TEXT("render_target"),L.Display && L.Display->GetRenderTarget());
            J->SetBoolField(TEXT("holding"),L.bDeathHolding); J->SetBoolField(TEXT("owner_view"),L.Actor && L.Actor->GetOwner() == PC->GetViewTarget());
            J->SetNumberField(TEXT("fraction"),-1);
            if (L.Widget && L.Widget->WidgetTree) { L.Widget->WidgetTree->ForEachWidget([&](UWidget* W) { if (auto* B = Cast<UProgressBar>(W)) { J->SetNumberField(TEXT("fraction"),B->GetPercent()); } }); }
            if (L.Display) { Position(J,L.Display->GetComponentLocation()); }
            Leases.Add(MakeShared<FJsonValueObject>(J));
        }
    }
    int32 Components = 0;
    for (TActorIterator<AActor> It(PC->GetWorld()); It; ++It)
    { if (!It->IsActorBeingDestroyed() && It->FindComponentByClass<UY01MonsterHealthBarWorldWidgetComponent>()) { ++Components; } }
    O->SetNumberField(TEXT("objects"),Count); O->SetNumberField(TEXT("components"),Components); O->SetNumberField(TEXT("active"),Active); O->SetNumberField(TEXT("visible"),Visible);
    O->SetArrayField(TEXT("candidates"),Candidates); O->SetArrayField(TEXT("leases"),Leases); return Encode(O);
}
void AY01MonsterHealthBarVerification::Install(UWorld* World)
{
    if (!World || (!Controlled(FCommandLine::Get()) && !FParse::Param(FCommandLine::Get(),TEXT("Y01R27Trace")))) { return; }
    for (TActorIterator<AY01MonsterHealthBarVerification> It(World); It; ++It) { return; }
    World->SpawnActor<AY01MonsterHealthBarVerification>();
}
void AY01MonsterHealthBarVerification::Execute(const FString& Action)
{
    auto* Mode = GetWorld()->GetAuthGameMode<AY01PlaygroundGameMode>(); if (!Mode || !Mode->GetProfile()) { return; }
    TArray<AY01PlaygroundController*> PCs;
    for (TActorIterator<AY01PlaygroundController> It(GetWorld()); It; ++It) { if (It->GetPawn() && It->GetObserver().bReady) { PCs.Add(*It); } }
    PCs.Sort([](const auto& A,const auto& B) { return A.PlayerState->GetPlayerId() < B.PlayerState->GetPlayerId(); });
    if (PCs.IsEmpty()) { return; }
    auto Spawn = [&](FName Name, FVector Location, bool Boss = false)
    {
        FActorSpawnParameters P; P.Name = Name; P.ObjectFlags |= RF_Transient; P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* M = GetWorld()->SpawnActor<AY01Monster>(Location,FRotator::ZeroRotator,P);
        FY01DataRef Ref; Ref.Type=EY01TableKind::Monster; Ref.RowName=Boss ? TEXT("Boss") : TEXT("Wave1Minion"); FString Error;
        if (!M->TryInitializeMonster(Mode->GetProfile()->BuildCatalog(),Ref,Error)) { UE_LOG(LogTemp,Error,TEXT("Y01R27 初始化失败 %s"),*Error); M->Destroy(); return static_cast<AY01Monster*>(nullptr); }
        M->bAlwaysRelevant=true; M->SetActorEnableCollision(false); M->GetCharacterMovement()->DisableMovement();
        M->RegisterHealthBarPresentation(0,0);
        auto* Visual=GetWorld()->SpawnActor<AY01PlaygroundVisual>(); Visual->Target=M; Visual->Definition=Ref; return M;
    };
    AY01Monster* Source = nullptr;
    for (TActorIterator<AY01Monster> It(GetWorld()); It; ++It) { if (It->GetFName() == TEXT("R27_Source")) { Source=*It; } }
    if (!Source) { Source=Spawn(TEXT("R27_Source"),FVector(50000,0,100)); if (Source) { Source->UnregisterHealthBarPresentation(); } }
    if (!Source) { return; }
    bool OK = true;
    auto Hit = [&](AY01Monster* M,float Amount) { if (!Damage(Source->GetCombatASC(),M,Amount)) { OK=false; } };
    auto* A=PCs[0];
    if (Action == TEXT("create"))
    { Targets.Add(Spawn(TEXT("R27_Target_000"),FVector(0,0,100))); auto* Boss=Spawn(TEXT("R27_Boss"),FVector(0,300,100),true); Hit(Boss,25); }
    else if (Action == TEXT("damage") && Targets.Num()) { Hit(Targets[0].Get(),25); }
    else if (Action == TEXT("heal") && Targets.Num())
    {
        auto* M=Targets[0].Get(); M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),100); M->ForceNetUpdate();
        auto J=Object(); J->SetStringField(TEXT("kind"),TEXT("heal")); J->SetStringField(TEXT("target"),M->GetName()); J->SetNumberField(TEXT("health"),100); J->SetNumberField(TEXT("maximum"),100);
        J->SetStringField(TEXT("epoch"),M->GetHealthBarPresentationState().WorldEpoch.ToString(EGuidFormats::Digits)); Emit(J);
    }
    else if (Action == TEXT("follow") && Targets.Num()) { Targets[0]->SetActorLocation(FVector(250,0,100)); Targets[0]->ForceNetUpdate(); }
    else if (Action == TEXT("grid"))
    {
        for (int32 I=Targets.Num(); I<129; ++I) { Targets.Add(Spawn(*FString::Printf(TEXT("R27_Target_%03d"),I),GridPosition(I))); }
        for (int32 I=0; I<Targets.Num(); ++I)
        { auto* M=Targets[I].Get(); M->SetActorLocation(GridPosition(I)); M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),100); Hit(M,25); M->ForceNetUpdate(); }
    }
    else if (Action.StartsWith(TEXT("near:")))
    {
        const int32 I=FCString::Atoi(*Action.Mid(5));
        if (Targets.IsValidIndex(I) && Targets[I].IsValid()) { Targets[I]->SetActorLocation(FVector(0,0,800)); Targets[I]->ForceNetUpdate(); } else { OK=false; }
    }
    else if (Action.StartsWith(TEXT("destroy:")))
    {
        const int32 I=FCString::Atoi(*Action.Mid(8)); if (Targets.IsValidIndex(I) && Targets[I].IsValid()) { Targets[I]->Destroy(); } else { OK=false; }
    }
    else if (Action == TEXT("death") && Targets.Num()) { Hit(Targets[0].Get(),1000); }
    else if (Action == TEXT("occlude"))
    {
        auto* Wall=GetWorld()->SpawnActor<AStaticMeshActor>(FVector(-300,0,550),FRotator::ZeroRotator);
        Wall->SetMobility(EComponentMobility::Movable); Wall->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
        Wall->SetActorScale3D(FVector(4,5,8)); Wall->SetReplicates(true); Wall->GetStaticMeshComponent()->SetIsReplicated(true); Wall->SetReplicateMovement(true); Wall->bAlwaysRelevant=true; Occluder=Wall;
    }
    else if (Action == TEXT("unocclude")) { if (Occluder.IsValid()) { Occluder->Destroy(); } }
    else if (Action == TEXT("enter") || Action == TEXT("reenter")) { OK=Mode->GetTraining()->Request(A,true); }
    else if (Action == TEXT("leave")) { OK=Mode->GetTraining()->Request(A,false); }
    else if (Action == TEXT("roomdamage"))
    {
        const auto Monsters=Mode->GetTraining()->Monsters(A->GetPlayerState<AY01PlayerState>());
        for (auto* M : Monsters) { OK &= Damage(A->GetPlayerState<AY01PlayerState>()->GetCombatASC(),M,25); }
    }
    else if (Action == TEXT("clear"))
    {
        for (auto M : Targets) { if (M.IsValid()) { M->Destroy(); } }
        Targets.Reset();
        for (TActorIterator<AY01Monster> It(GetWorld()); It; ++It) { if (It->GetName().StartsWith(TEXT("R27_"))) { It->Destroy(); } }
    }
    auto J=Object(); J->SetStringField(TEXT("kind"),TEXT("action")); J->SetStringField(TEXT("action"),Action); J->SetBoolField(TEXT("ok"),OK); J->SetNumberField(TEXT("sequence"),LastSequence); Emit(J);
}
void AY01MonsterHealthBarVerification::SetCamera(const FString& Action)
{
#if !UE_SERVER
    AY01PlaygroundController* PC=nullptr;
    for (TActorIterator<AY01PlaygroundController> It(GetWorld()); It; ++It) { if (It->IsLocalController() && It->GetLocalPlayer()) { PC=*It; break; } }
    if (!PC) { return; }
    if (!Camera.IsValid()) { Camera=GetWorld()->SpawnActor<ACameraActor>(); }
    PC->bAutoManageActiveCameraTarget=false; PC->SetViewTarget(Camera.Get());
    if (PC->GetHUD()) { PC->GetHUD()->bShowHUD=false; }
    if (Action == TEXT("grid")) { bGridCamera=true; }
    if (Action == TEXT("enter") || Action == TEXT("reenter")) { bGridCamera=false; }
    const bool Grid = bGridCamera;
    FVector Center=FVector::ZeroVector;
    if (PC->GetObserver().Training.bInside) { Center=PC->GetPawn()->GetActorLocation(); }
    FVector Location=Grid ? FVector(-50,0,2600) : FVector(-650,0,1300);
    FRotator Rotation=Grid ? FRotator(-89,0,0) : FRotator(-65,0,0);
    if (Action == TEXT("offscreen")) { Rotation.Yaw=180; Rotation.Pitch=0; }
    Camera->SetActorLocationAndRotation(Center+Location,Rotation); Camera->GetCameraComponent()->SetFieldOfView(Grid ? 65 : 55);
#endif
}
void AY01MonsterHealthBarVerification::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    const bool Control=Controlled(FCommandLine::Get());
    if (Control)
    {
        FString Path, Text; FParse::Value(FCommandLine::Get(),TEXT("Y01R27CommandFile="),Path);
        FString Root=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()); FPaths::NormalizeDirectoryName(Root); FPaths::NormalizeFilename(Path);
        if (Path.StartsWith(Root+TEXT("/R27-U3-")) && FFileHelper::LoadFileToString(Text,*Path))
        {
            FString Number,Action; Text.TrimStartAndEndInline();
            if (Text.Split(TEXT("|"),&Number,&Action) && FCString::Atoi(*Number)>LastSequence)
            { LastSequence=FCString::Atoi(*Number); LastAction=Action; if (GetNetMode()==NM_DedicatedServer) { Execute(Action); } }
        }
        if (GetNetMode()!=NM_DedicatedServer) { SetCamera(LastAction); }
    }
    if (GetWorld()->GetTimeSeconds()<NextObserve) { return; } NextObserve=GetWorld()->GetTimeSeconds()+.05;
    for (TActorIterator<AY01PlayerController> It(GetWorld()); It; ++It)
    {
        if (GetNetMode()!=NM_DedicatedServer && !It->IsLocalController()) { continue; }
        FObject J; auto Reader=TJsonReaderFactory<>::Create(Observe(*It));
        if (FJsonSerializer::Deserialize(Reader,J)) { J->SetStringField(TEXT("action"),LastAction); Emit(J); }
    }
}
void AY01MonsterHealthBarVerification::EndPlay(EEndPlayReason::Type Reason)
{
    auto J=Object(); J->SetStringField(TEXT("kind"),TEXT("observer_end")); J->SetNumberField(TEXT("net"),GetNetMode()); Emit(J);
    Super::EndPlay(Reason);
}
