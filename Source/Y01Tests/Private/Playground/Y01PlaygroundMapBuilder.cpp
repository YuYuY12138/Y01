#include "Playground/Y01PlaygroundMapBuilder.h"
#if WITH_EDITOR
#include "Playground/Y01PlaygroundRuntime.h"
#include "Engine/World.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/SkyLight.h"
#include "Components/StaticMeshComponent.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/PlayerStart.h"
#include "EngineUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"

bool Y01PlaygroundEditor::BuildMap(const FString& Root, UY01PlaygroundProfile* Profile, bool bValidateOnly, FString& Error)
{
    Error.Reset();
    const FString Name = Root + TEXT("/Maps/L_Y01Playground");
    if (!Root.StartsWith(TEXT("/Game/")) || !FPackageName::IsValidLongPackageName(Name) || !Profile || !Profile->Validate().IsValid())
    { Error = TEXT("PlaygroundMap: invalid Root/Profile"); return false; }
    const FString Filename = FPackageName::LongPackageNameToFilename(Name, FPackageName::GetMapPackageExtension());
    if (FPackageName::DoesPackageExist(Name))
    {
        auto* Package = LoadPackage(nullptr, *Name, LOAD_None);
        UWorld* World = Package ? UWorld::FindWorldInPackage(Package) : nullptr;
        if (!World || World->GetWorldPartition() || World->GetWorldSettings()->DefaultGameMode != AY01PlaygroundGameMode::StaticClass())
        { Error = TEXT("PlaygroundMap: saved world missing or invalid ModeOverride: ") + Name; return false; }
        int32 Configs = 0, Starts = 0, Floors = 0;
        bool bProfileMatches = false;
        for (TActorIterator<AY01PlaygroundConfig> It(World, AY01PlaygroundConfig::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It) { ++Configs; bProfileMatches = It->Profile == Profile; }
        for (TActorIterator<APlayerStart> It(World, APlayerStart::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It) { ++Starts; }
        for (TActorIterator<AStaticMeshActor> It(World, AStaticMeshActor::StaticClass(), EActorIteratorFlags::SkipPendingKill); It; ++It)
        { if (It->Tags.Contains(TEXT("PlaygroundFloor")) && It->GetStaticMeshComponent()->GetStaticMesh() && It->GetActorEnableCollision()) { ++Floors; } }
        if (Configs != 1 || !bProfileMatches || Starts < 1 || Floors < 1)
        { Error = FString::Printf(TEXT("PlaygroundMap: saved Config/Profile/PlayerStart/Floor invalid: %s Configs=%d ProfileMatch=%d Starts=%d Floors=%d"), *Name, Configs, bProfileMatches, Starts, Floors); return false; }
        UE_LOG(LogTemp, Display, TEXT("PlaygroundMap VALID Preserved=%s"), *Name); return true;
    }
    if (bValidateOnly || FindPackage(nullptr, *Name))
    { Error = TEXT("PlaygroundMap: missing saved map; no overwrite: ") + Name; return false; }
    UPackage* Package = CreatePackage(*Name);
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .CreateNavigation(false).CreateAISystem(false).RequiresHitProxies(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Editor, false, TEXT("L_Y01Playground"), Package, false, ERHIFeatureLevel::Num, &IVS);
    if (!World) { Error = TEXT("PlaygroundMap: CreateWorld failed"); return false; }
    World->GetWorldSettings()->DefaultGameMode = AY01PlaygroundGameMode::StaticClass();
    auto* Config = World->SpawnActor<AY01PlaygroundConfig>(); Config->Profile = Profile; Config->SetActorLabel(TEXT("Playground - Persistent Profile"));
    auto* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
    auto Box = [&](const FVector& Location, const FVector& Scale, const TCHAR* Label)
    {
        auto* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
        Actor->GetStaticMeshComponent()->SetStaticMesh(Cube);
        Actor->SetActorScale3D(Scale); Actor->SetActorLabel(Label);
        Actor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
        return Actor;
    };
    Box(FVector(0, 0, -50), FVector(24, 24, 1), TEXT("Playground Floor"))->Tags.Add(TEXT("PlaygroundFloor"));
    Box(FVector(1200, 0, 50), FVector(0.3, 24, 2), TEXT("Boundary East"));
    Box(FVector(-1200, 0, 50), FVector(0.3, 24, 2), TEXT("Boundary West"));
    Box(FVector(0, 1200, 50), FVector(24, 0.3, 2), TEXT("Boundary North"));
    Box(FVector(0, -1200, 50), FVector(24, 0.3, 2), TEXT("Boundary South"));
    for (int32 I = 0; I < 4; ++I)
    {
        World->SpawnActor<APlayerStart>(FVector(-150, (I - 1.5) * 100, 100), FRotator::ZeroRotator);
    }
    auto* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 600), FRotator(-55, -30, 0));
    Sun->GetLightComponent()->SetMobility(EComponentMobility::Movable);
    Sun->GetLightComponent()->SetIntensity(3.f);
    auto* Sky = World->SpawnActor<ASkyLight>();
    Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
    Sky->GetLightComponent()->SetIntensity(0.8f);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true);
    World->SetFlags(RF_Public | RF_Standalone); Package->MarkPackageDirty();
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    const bool bSaved = !IFileManager::Get().FileExists(*Filename) && UPackage::SavePackage(Package, World, *Filename, Args);
    if (bSaved) { FAssetRegistryModule::AssetCreated(World); UE_LOG(LogTemp, Display, TEXT("PlaygroundMap CREATED %s"), *Name); }
    else { Error = TEXT("PlaygroundMap: SavePackage failed: ") + Filename; }
    World->CleanupWorld();
    return bSaved;
}

bool Y01PlaygroundEditor::RepairSeedEnemyBinding(UY01PlaygroundProfile* Profile, FString& Error)
{
    // 仅供本阶段显式修复交接遗漏；生成与启动绝不调用，不改任何数值。
    auto* Table = Profile ? Profile->Tables.FindRef(EY01TableKind::Monster).Get() : nullptr;
    if (!Table) { Error = TEXT("Enemy binding: missing Monster table"); return false; }
    TArray<FY01MonsterRow*> Rows;
    for (const FName RowName : { FName(TEXT("Default")), FName(TEXT("Boss")) })
    {
        auto* Row = Table->FindRow<FY01MonsterRow>(RowName, TEXT("ExplicitSeedRepair"));
        if (!Row || Row->Skills.Num() != 1 || Row->Skills[0].Type != EY01TableKind::Skill)
        { Error = TEXT("Enemy binding: user-modified row; refusing repair"); return false; }
        if (Row->Skills[0].RowName == TEXT("EnemyAttack")) { continue; }
        const bool bBoss = RowName == TEXT("Boss");
        if (Row->Skills[0].RowName != TEXT("BasicAttack") || Row->Health != (bBoss ? 150 : 60)
            || Row->AttackDamage != (bBoss ? 10 : 5) || Row->Range != (bBoss ? 180 : 140) || Row->MoveSpeed != (bBoss ? 90 : 120))
        { Error = TEXT("Enemy binding: not untouched seed values; refusing repair"); return false; }
        Rows.Add(Row);
    }
    if (Rows.IsEmpty()) { return true; }
    const FString File = FPackageName::LongPackageNameToFilename(Table->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
    const FString Backup = FPaths::ProjectSavedDir() / TEXT("Playground-Original-DT_Monster.uasset");
    if (!IFileManager::Get().FileExists(*Backup) && IFileManager::Get().Copy(*Backup, *File, false) != COPY_OK)
    { Error = TEXT("Enemy binding: backup failed"); return false; }
    Table->GetOutermost()->FullyLoad();
    ResetLoaders(Table->GetOutermost());
    for (auto* Row : Rows) { Row->Skills[0].RowName = TEXT("EnemyAttack"); }
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    const bool bSaved = UPackage::SavePackage(Table->GetOutermost(), Table, *File, Args);
    if (!bSaved) { Error = TEXT("Enemy binding: save failed"); }
    else { UE_LOG(LogTemp, Display, TEXT("Playground corrected only Monster.Skills[0] to EnemyAttack; Backup=%s"), *Backup); }
    return bSaved;
}
#endif
