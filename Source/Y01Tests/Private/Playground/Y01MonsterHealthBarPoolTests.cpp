#include "Misc/AutomationTest.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01PlaygroundSeed.h"
#include "Playground/Y01MonsterHealthBarVerification.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Level.h"
#include "EngineUtils.h"
#include "UnrealClient.h"
#include "SceneView.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "Components/WidgetComponent.h"
#include "Components/ProgressBar.h"
#include "Components/TextBlock.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Materials/Material.h"
#include "GameplayEffectExtension.h"
#include "UObject/UnrealType.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
// 无窗口视口只提供尺寸，仍调用引擎LocalPlayer投影；不创建GPU证据或打开可见窗口。
class FBarViewport : public FViewport
{
public:
    FBarViewport() : FViewport(nullptr) {}
    virtual FIntPoint GetSizeXY() const override { return FIntPoint(1280, 720); }
    virtual void* GetWindow() override { return nullptr; }
    virtual void MoveWindow(int32, int32, int32, int32) override {}
    virtual void Destroy() override {}
    virtual void InvalidateDisplay() override {}
    virtual FViewportFrame* GetViewportFrame() override { return nullptr; }
    virtual bool SetUserFocus(bool) override { return false; }
    virtual bool KeyState(FKey) const override { return false; }
    virtual int32 GetMouseX() const override { return 0; }
    virtual int32 GetMouseY() const override { return 0; }
    virtual void GetMousePos(FIntPoint& P, bool) override { P = FIntPoint::ZeroValue; }
    virtual void SetMouse(int32, int32) override {}
    virtual void ProcessInput(float) override {}
    virtual FVector2D VirtualDesktopPixelToViewport(FIntPoint) const override { return FVector2D::ZeroVector; }
    virtual FIntPoint ViewportToVirtualDesktopPixel(FVector2D) const override { return FIntPoint::ZeroValue; }
};
struct FBarSettingsScope
{
    UObject* Object = nullptr;
    TMap<FString, FString> Saved;
    FBarSettingsScope()
    {
        if (auto* C = FindObject<UClass>(nullptr, TEXT("/Script/Y01.Y01MonsterHealthBarSettings")))
        {
            Object = C->GetDefaultObject();
            for (const TCHAR* N : {TEXT("bEnabled"), TEXT("PrewarmCount"), TEXT("Capacity"), TEXT("DeathHoldSeconds")})
            {
                if (auto* P = FindFProperty<FProperty>(C, N))
                { FString V; P->ExportText_InContainer(0, V, Object, Object, Object, PPF_None); Saved.Add(N, V); }
            }
        }
    }
    void Set(const TCHAR* N, const TCHAR* V)
    { if (Object) { if (auto* P = FindFProperty<FProperty>(Object->GetClass(), N)) { P->ImportText_InContainer(V, Object, Object, PPF_None); } } }
    ~FBarSettingsScope() { for (const auto& P : Saved) { Set(*P.Key, *P.Value); } }
};
struct FBarProbe
{
    static UActorComponent* Pool(AY01PlayerController* PC)
    {
        for (auto* C : PC->GetComponents())
        { if (IsValid(C) && !C->IsBeingDestroyed() && C->GetClass()->GetName() == TEXT("Y01MonsterHealthBarPoolComponent")) { return C; } }
        return nullptr;
    }
    static TArray<UWidgetComponent*> Entries(UWorld* W, AY01PlayerController* PC, bool Active = false)
    {
        TArray<UWidgetComponent*> Result;
        for (TActorIterator<AActor> It(W); It; ++It)
        {
            if (It->IsActorBeingDestroyed()) { continue; }
            auto* C = It->FindComponentByClass<UWidgetComponent>();
            if (C && C->GetClass()->GetName() == TEXT("Y01MonsterHealthBarWorldWidgetComponent") && C->GetOwnerPlayer() == PC->GetLocalPlayer()
                && (!Active || (C->IsVisible() && !It->IsHidden()))) { Result.Add(C); }
        }
        return Result;
    }
    static UProgressBar* Bar(UWidgetComponent* C)
    {
        auto* W = C ? C->GetUserWidgetObject() : nullptr; UProgressBar* B = nullptr;
        if (W && W->WidgetTree) { W->WidgetTree->ForEachWidget([&](UWidget* Child) { if (auto* P = Cast<UProgressBar>(Child)) { B = P; } }); }
        return B;
    }
    static bool At(UWidgetComponent* C, AY01Monster* M)
    { return C->GetComponentLocation().Equals(M->GetActorLocation() + FVector(0,0,M->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 20), .1); }
};
struct FBarWorld
{
    TStrongObjectPtr<UY01PlaygroundProfile> Profile{Y01PlaygroundEditor::CreateSeed(GetTransientPackage())};
    FBarViewport Viewport;
    UWorld* World;
    AY01PlaygroundGameMode* Mode;
    AY01PlaygroundController* PC;
    TStrongObjectPtr<UGameViewportClient> Client{NewObject<UGameViewportClient>(GEngine)};
    FAutomationTestBase& Test;
    explicit FBarWorld(FAutomationTestBase& InTest) : Test(InTest)
    {
        auto* C = Profile->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("R27Pool"));
        C->Health = 1000; C->MoveSpeed = 0; C->InitialEquipment.Reset(); C->R33TestGrowth.bEnabled = false;
        for (const TCHAR* N : {TEXT("Default"), TEXT("Boss")})
        { auto* M = Profile->Tables[EY01TableKind::Monster]->FindRow<FY01MonsterRow>(N, TEXT("R27Pool")); M->Health = 100; M->AttackDamage = 0; M->MoveSpeed = 0; M->Skills.Reset(); }
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true).RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01PlaygroundGameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01PlaygroundGameMode>(); World->SpawnActor<AY01PlaygroundConfig>()->Profile = Profile.Get();
        PC = World->SpawnActor<AY01PlaygroundController>(); auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01PlaygroundCharacter>(FVector(0,0,100), FRotator::ZeroRotator);
        PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(PS);
        PC->Player = NewObject<ULocalPlayer>(GEngine); PC->GetLocalPlayer()->PlayerController = PC;
        Client->Viewport = &Viewport; PC->GetLocalPlayer()->ViewportClient = Client.Get();
        PC->GetLocalPlayer()->Origin = FVector2D::ZeroVector; PC->GetLocalPlayer()->Size = FVector2D(1,1);
    }
    ~FBarWorld()
    { World->EndPlay(EEndPlayReason::Quit); Client->Viewport = nullptr; World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    void Start()
    { Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay(); PC->Tick(0); Camera(); }
    void Camera(FVector Eye = FVector(0,0,208), FRotator Rotation = FRotator::ZeroRotator)
    {
        FMinimalViewInfo V; V.Location = Eye; V.Rotation = Rotation; V.FOV = 90; V.AspectRatio = 1280.f/720;
        // 引擎仅在缓存时间大于零时采用相机POV，夹具不推进整个战斗World。
        World->TimeSeconds = FMath::Max(World->TimeSeconds, .001);
        PC->PlayerCameraManager->FillCameraCache(V);
        FVector ActualEye; FRotator ActualRotation; PC->GetPlayerViewPoint(ActualEye, ActualRotation);
        Test.TestTrue(TEXT("夹具实际相机位置生效"), ActualEye.Equals(Eye));
        Test.TestTrue(TEXT("夹具实际相机旋转生效"), ActualRotation.Equals(Rotation));
    }
    void Step(float Time = .11f)
    { if (auto* P = FBarProbe::Pool(PC)) { P->TickComponent(Time, LEVELTICK_All, &P->PrimaryComponentTick); } }
    AY01Monster* Monster(float X = 500, FName Name = TEXT("Default"), bool Register = true)
    {
        FActorSpawnParameters P; P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* M = World->SpawnActor<AY01Monster>(FVector(X,0,100), FRotator::ZeroRotator, P);
        FY01DataRef Ref; Ref.Type = EY01TableKind::Monster; Ref.RowName = Name; FString Error;
        Test.TestTrue(TEXT("夹具怪物初始化成功"), M->TryInitializeMonster(Profile->BuildCatalog(), Ref, Error)); M->GetCharacterMovement()->DisableMovement();
        if (Register) { Test.TestTrue(TEXT("夹具普通怪登记成功"), M->RegisterHealthBarPresentation(0,0)); } return M;
    }
    void Damage(AY01Monster* M, float Amount = 20)
    {
        auto* S = PC->GetPlayerState<AY01PlayerState>()->GetCombatASC();
        auto* Avatar = S->GetAvatarActor(); const FVector Previous = Avatar->GetActorLocation();
        // 先在合法攻击距离内造成真实伤害，再恢复施法者；血条距离测试不绕过战斗规则。
        Avatar->SetActorLocation(M->GetActorLocation() - FVector(100,0,0));
        const float Before = M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
        auto Spec = S->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, S->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, Amount); S->ApplyGameplayEffectSpecToTarget(*Spec.Data, M->GetCombatASC());
        Avatar->SetActorLocation(Previous);
        Test.TestEqual(TEXT("夹具GE真实扣血"), M->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), FMath::Max(0.f, Before - Amount));
        Test.TestTrue(TEXT("夹具真实伤害已锁存资格"), M->GetHealthBarPresentationState().bEverActuallyDamaged);
    }
    TArray<UWidgetComponent*> Active() { return FBarProbe::Entries(World, PC, true); }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01BarPoolTest, "Y01.Playground.R27.Pool", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01BarPoolTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("Prewarm"), TEXT("Config.Defaults"), TEXT("Config.CapMin"), TEXT("Config.CapMax"), TEXT("Config.PrewarmZero"),
        TEXT("Config.Disabled"), TEXT("Config.Snapshot"), TEXT("Config.HoldZero"), TEXT("Config.HoldNaN"), TEXT("Config.HoldInfinity"),
        TEXT("Registration.Spawn"), TEXT("Registration.InitialScan"), TEXT("Registration.Delayed"), TEXT("Registration.BossUnknown"),
        TEXT("Selection.Closest129"), TEXT("Selection.StableTie"), TEXT("Selection.NoDistanceCutoff"), TEXT("Selection.OffscreenReturn"), TEXT("Selection.Rate"),
        TEXT("ASC.Health"), TEXT("ASC.MaxHealth"), TEXT("ASC.FullHeal"), TEXT("Lease.OldCallback"), TEXT("Lease.OldVersion"), TEXT("Lease.OldAuthorization"), TEXT("Lease.Follow"),
        TEXT("Scope.Region"), TEXT("Scope.Session"), TEXT("Scope.Revoke"), TEXT("Scope.Unregister"), TEXT("Scope.Epoch"), TEXT("Scope.Controllers"),
        TEXT("Death.Hold"), TEXT("Death.NoNewLease"), TEXT("Death.Destroy"), TEXT("Death.Offscreen"), TEXT("Death.Revoke"),
        TEXT("Lifecycle.Travel"), TEXT("Lifecycle.SeamlessWorld"), TEXT("Lifecycle.SeamlessNoPre"), TEXT("Lifecycle.TargetWorld"), TEXT("Lifecycle.EndPlay"),
        TEXT("Lifecycle.WorldCleanup"), TEXT("Lifecycle.Idempotent"), TEXT("Lifecycle.DedicatedServer"), TEXT("Lifecycle.Remote"), TEXT("Lifecycle.BaseController")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01BarPoolTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FBarSettingsScope Settings;
    if (Case == TEXT("Config.CapMin")) { Settings.Set(TEXT("Capacity"), TEXT("-9")); Settings.Set(TEXT("PrewarmCount"), TEXT("100")); }
    if (Case == TEXT("Config.CapMax")) { Settings.Set(TEXT("Capacity"), TEXT("900")); Settings.Set(TEXT("PrewarmCount"), TEXT("999")); }
    if (Case == TEXT("Config.PrewarmZero")) { Settings.Set(TEXT("PrewarmCount"), TEXT("-8")); }
    if (Case == TEXT("Config.Disabled")) { Settings.Set(TEXT("bEnabled"), TEXT("False")); }
    if (Case == TEXT("Config.HoldZero")) { Settings.Set(TEXT("DeathHoldSeconds"), TEXT("-5")); }
    if (Case == TEXT("Config.HoldNaN") || Case == TEXT("Config.HoldInfinity"))
    {
        if (Settings.Object)
        { if (auto* P = FindFProperty<FFloatProperty>(Settings.Object->GetClass(), TEXT("DeathHoldSeconds")))
            { P->SetPropertyValue_InContainer(Settings.Object, Case.EndsWith(TEXT("NaN")) ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity()); } }
    }
    FBarWorld F(*this);
    if (Case == TEXT("Lifecycle.DedicatedServer")) { F.World->WorldType = EWorldType::PIE; F.World->SetPlayInEditorInitialNetMode(NM_DedicatedServer); }
    if (Case == TEXT("Lifecycle.Remote") || Case == TEXT("Registration.InitialScan")) { F.PC->Player = nullptr; }
    F.Start(); if (!TestTrue(TEXT("夹具真实对局配置成功"), F.Mode->IsMatchConfigured())) { return false; }
    if (Case == TEXT("Registration.InitialScan"))
    {
        auto* M = F.Monster(); F.Damage(M);
        F.PC->Player = NewObject<ULocalPlayer>(GEngine); F.PC->GetLocalPlayer()->PlayerController = F.PC;
        F.PC->GetLocalPlayer()->ViewportClient = F.Client.Get();
        F.PC->GetLocalPlayer()->Origin = FVector2D::ZeroVector; F.PC->GetLocalPlayer()->Size = FVector2D(1,1);
        F.PC->Tick(0); F.Camera(); F.Step();
        return TestEqual(TEXT("已有怪物在池首次就绪时补扫显示"), F.Active().Num(), 1);
    }
    if (Case == TEXT("Lifecycle.DedicatedServer") || Case == TEXT("Lifecycle.Remote") || Case == TEXT("Lifecycle.BaseController") || Case == TEXT("Config.Disabled"))
    {
        auto* PC = static_cast<AY01PlayerController*>(F.PC);
        if (Case == TEXT("Lifecycle.BaseController")) { PC = F.World->SpawnActor<AY01PlayerController>(); PC->Player = NewObject<ULocalPlayer>(GEngine); PC->Tick(0); }
        if (Case == TEXT("Lifecycle.DedicatedServer")) { TestEqual(TEXT("真实World DS模式"), F.World->GetNetMode(), NM_DedicatedServer); }
        TestNull(TEXT("无资格Controller不建血条池"), FBarProbe::Pool(PC));
        TestEqual(TEXT("无资格Controller零血条组件"), FBarProbe::Entries(F.World, PC).Num(), 0); return true;
    }
    const int32 ExpectedPrewarm = Case == TEXT("Config.CapMin") ? 1 : Case == TEXT("Config.CapMax") ? 128 : Case == TEXT("Config.PrewarmZero") ? 0 : 32;
    auto Entries = FBarProbe::Entries(F.World, F.PC);
    if (!TestEqual(TEXT("获授权本地Controller预热真实World血条条目"), Entries.Num(), ExpectedPrewarm)) { return false; }
    auto* Pool = FBarProbe::Pool(F.PC); if (!TestNotNull(TEXT("独立本地血条池组件"), Pool)) { return false; }
    TestFalse(TEXT("池不复制"), Pool->GetIsReplicated());
    for (auto* W : Entries)
    {
        TestEqual(TEXT("世界空间而非固定像素投影"), W->GetWidgetSpace(), EWidgetSpace::World);
        TestEqual(TEXT("临时世界尺寸"), W->GetDrawSize(), FVector2D(128,16));
        TestTrue(TEXT("闲置隐藏"), !W->IsVisible() && W->GetOwner()->IsHidden());
        TestFalse(TEXT("条目不独立Tick"), W->PrimaryComponentTick.bCanEverTick);
        TestFalse(TEXT("条目不复制"), W->GetOwner()->GetIsReplicated()); TestTrue(TEXT("只对所属视图"), W->bOnlyOwnerSee);
        TestEqual(TEXT("无碰撞"), W->GetCollisionEnabled(), ECollisionEnabled::NoCollision);
        if (auto* Mat = W->GetMaterial(0)) { TestFalse(TEXT("正常深度遮挡"), Mat->GetMaterial()->bDisableDepthTest); }
        if (TestNotNull(TEXT("真实原生进度条"), FBarProbe::Bar(W))) { TestEqual(TEXT("归还清空"), FBarProbe::Bar(W)->GetPercent(), 0.f); }
        if (auto* U = W->GetUserWidgetObject())
        { U->WidgetTree->ForEachWidget([&](UWidget* Child) { TestFalse(TEXT("无文字名字图标"), Child->IsA<UTextBlock>()); }); TestEqual(TEXT("闲置透明"), U->GetRenderOpacity(), 0.f); }
    }
    if (Case == TEXT("Prewarm") || Case == TEXT("Config.Defaults")) { TestNotNull(TEXT("配置类存在"), Settings.Object); return true; }
    FSceneViewProjectionData Projection;
    if (!TestTrue(TEXT("引擎本地视口投影可用"), F.PC->GetLocalPlayer()->GetProjectionData(&F.Viewport, Projection))) { return false; }
    if (Case == TEXT("Config.Snapshot")) { Settings.Set(TEXT("Capacity"), TEXT("1")); Settings.Set(TEXT("bEnabled"), TEXT("False")); }
    if (Case == TEXT("Registration.BossUnknown"))
    {
        auto* Boss = F.Monster(600, TEXT("Boss")); auto* Unknown = F.Monster(700, TEXT("Default"), false); F.Damage(Boss); F.Damage(Unknown); F.Step();
        TestEqual(TEXT("Boss与未登记目标绝不借出"), F.Active().Num(), 0);
    }
    auto* M = F.Monster(Case == TEXT("Selection.NoDistanceCutoff") ? 1000000 : 500, TEXT("Default"), Case != TEXT("Registration.Delayed"));
    F.Damage(M);
    if (Case == TEXT("Registration.Delayed")) { F.Step(); TestEqual(TEXT("先受伤未登记仍不显示"), F.Active().Num(), 0); M->RegisterHealthBarPresentation(0,0); }
    if (Case == TEXT("Death.NoNewLease")) { F.Damage(M, 1000); F.Step(); return TestEqual(TEXT("从未持有租约的尸体不借出"), F.Active().Num(), 0); }
    F.Step(); auto Active = F.Active();
    if (!TestEqual(TEXT("真实扣血已登记普通怪借出"), Active.Num(), 1)) { return false; }
    auto* First = Active[0]; auto* Bar = FBarProbe::Bar(First); if (!TestNotNull(TEXT("活动进度条存在"), Bar)) { return false; }
    TestEqual(TEXT("首帧直接取ASC比例无插值"), Bar->GetPercent(), .8f); TestTrue(TEXT("胶囊顶加20cm"), FBarProbe::At(First, M));
    TestTrue(TEXT("世界缩放0.5"), First->GetComponentScale().Equals(FVector(.5)));
    if (Case == TEXT("ASC.Health")) { F.Damage(M,10); TestEqual(TEXT("ASC委托立即更新"), Bar->GetPercent(), .7f); }
    if (Case == TEXT("ASC.MaxHealth")) { M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(), 200); TestEqual(TEXT("MaxHealth同样订阅"), Bar->GetPercent(), .4f); }
    if (Case == TEXT("ASC.FullHeal")) { M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(),100); F.Step(); TestEqual(TEXT("回血到满仍显示"), F.Active().Num(),1); TestEqual(TEXT("满血立即更新"),Bar->GetPercent(),1.f); }
    if (Case.StartsWith(TEXT("Selection.Closest")) || Case == TEXT("Selection.StableTie") || Case == TEXT("Config.CapMin") || Case == TEXT("Config.CapMax") || Case == TEXT("Config.Snapshot"))
    {
        TArray<AY01Monster*> Targets{M};
        for (int32 I=1; I<129; ++I) { auto* T = F.Monster(Case == TEXT("Selection.StableTie") ? 500 : 500 + I*10); F.Damage(T); Targets.Add(T); }
        F.Step(); TestEqual(TEXT("硬容量钳制且初始化快照不热改"), F.Active().Num(), Case == TEXT("Config.CapMin") ? 1 : 128);
        bool LastShown = false; for (auto* W : F.Active()) { if (FBarProbe::At(W, Targets.Last())) { LastShown = true; } }
        if (Case != TEXT("Selection.StableTie")) { TestFalse(TEXT("第129较远目标未获租约"), LastShown); Targets.Last()->SetActorLocation(FVector(400,0,100)); F.Step();
            bool NearShown = false; for (auto* W : F.Active()) { NearShown |= FBarProbe::At(W,Targets.Last()); } TestTrue(TEXT("第129靠近后自动竞争成功"),NearShown);
            auto* Evicted = Targets[Case == TEXT("Config.CapMin") ? 0 : 127];
            TestFalse(TEXT("被挤出目标立即退订"), Evicted->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).IsBoundToObject(Pool));
            TestTrue(TEXT("满池归还不抹资格"), Evicted->GetHealthBarPresentationState().HasLiveHealthBarEligibility());
            Targets.Last()->SetActorLocation(FVector(-500,0,100)); F.Step();
            TestTrue(TEXT("容量恢复后被挤出目标自动重新借出"), Evicted->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).IsBoundToObject(Pool));
        }
        else
        {
            TestTrue(TEXT("等距首个目标原租约保持"), First->IsVisible());
            TestTrue(TEXT("同距第128个有ASC租约"), Targets[127]->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).IsBoundToObject(Pool));
            TestFalse(TEXT("同距第129个按稳定登记顺序落选"), Targets[128]->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).IsBoundToObject(Pool));
        }
    }
    if (Case == TEXT("Selection.Rate"))
    { auto* T = F.Monster(600); F.Damage(T); F.Step(.01f); TestEqual(TEXT("非每帧重新筛选"), F.Active().Num(),1); F.Step(.1f); TestEqual(TEXT("10Hz筛选新增"),F.Active().Num(),2); }
    if (Case == TEXT("Selection.OffscreenReturn"))
    { F.Camera(FVector(0,0,208),FRotator(0,180,0)); F.Step(.001f); TestEqual(TEXT("活动离屏立即归还"),F.Active().Num(),0); F.Camera(); F.Step(); TestEqual(TEXT("回来自动恢复资格"),F.Active().Num(),1); }
    if (Case == TEXT("Lease.Follow"))
    { M->SetActorLocation(FVector(600,50,100)); F.Step(.001f); TestTrue(TEXT("每帧跟随无插值"),FBarProbe::At(First,M)); TestTrue(TEXT("始终朝向相机"),First->GetForwardVector().Equals((Projection.ViewOrigin-First->GetComponentLocation()).GetSafeNormal(),.001)); }
    if (Case == TEXT("Scope.Epoch"))
    {
        // 注入完整状态只验证池边界，不冒充网络投递证据。
        auto* Property = FindFProperty<FStructProperty>(M->GetClass(), TEXT("PresentationState"));
        if (!TestNotNull(TEXT("真实持久状态字段"), Property)) { return false; }
        auto* State = Property->ContainerPtrToValuePtr<FY01MonsterPresentationState>(M); const auto Saved = *State;
        State->WorldEpoch = FGuid::NewGuid(); M->OnHealthBarPresentationChanged.Broadcast();
        TestEqual(TEXT("同区域会话但异Epoch立即归还"), F.Active().Num(), 0); F.Step();
        TestEqual(TEXT("异Epoch不重新借出"), F.Active().Num(), 0);
        *State = Saved; M->OnHealthBarPresentationChanged.Broadcast(); F.Step();
        return TestEqual(TEXT("有效World身份恢复后显示"), F.Active().Num(), 1);
    }
    if (Case == TEXT("Scope.Controllers"))
    {
        auto* Other = F.World->SpawnActor<AY01PlaygroundController>(); Other->Player = NewObject<ULocalPlayer>(GEngine);
        auto* Local = Other->GetLocalPlayer(); Local->PlayerController = Other; Local->ViewportClient = F.Client.Get();
        Local->Origin = FVector2D::ZeroVector; Local->Size = FVector2D(1,1);
        // 此例只验证池消费显式授权；不调用会因缺少玩法PlayerState而撤权的Playground观察器。
        Other->SetHealthBarRegion(3,1); Other->AY01PlayerController::Tick(0);
        TestEqual(TEXT("夹具私有授权有效"), Other->GetHealthBarAuthorization().RegionId, 3);
        Other->PlayerCameraManager->FillCameraCache(F.PC->PlayerCameraManager->GetCameraCacheView());
        auto* Private = F.Monster(700); Private->RegisterHealthBarPresentation(3,1); F.Damage(Private);
        auto* OtherPool = FBarProbe::Pool(Other); if (!TestNotNull(TEXT("第二本地Controller独立池"), OtherPool)) { return false; }
        F.Step(); OtherPool->TickComponent(.11f, LEVELTICK_All, &OtherPool->PrimaryComponentTick);
        const auto OtherActive = FBarProbe::Entries(F.World, Other, true);
        TestEqual(TEXT("主线池只显示主线目标"), F.Active().Num(), 1);
        if (TestEqual(TEXT("私有区域池只显示同会话目标"), OtherActive.Num(), 1))
        { TestTrue(TEXT("私有租约绑定正确目标"), FBarProbe::At(OtherActive[0], Private)); TestNotEqual(TEXT("两Controller不共享组件"), OtherActive[0], First); }
        TestFalse(TEXT("主线池不订阅私有目标"), Private->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).IsBoundToObject(Pool));
        TestFalse(TEXT("私有池不订阅主线目标"), M->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).IsBoundToObject(OtherPool));
        return true;
    }
    if (Case.StartsWith(TEXT("Scope.")) || Case == TEXT("Lease.OldAuthorization"))
    {
        auto Old = M->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute());
        if (Case == TEXT("Scope.Unregister")) { M->UnregisterHealthBarPresentation(); }
        else if (Case == TEXT("Scope.Revoke")) { F.PC->RevokeHealthBarAuthorization(); }
        else { F.PC->SetHealthBarRegion(3, Case == TEXT("Scope.Session") ? 2 : 1); if (Case == TEXT("Scope.Session")) { M->RegisterHealthBarPresentation(3,1); } }
        TestEqual(TEXT("离区撤权旧会话立即清场"),F.Active().Num(),0);
        F.PC->SetHealthBarRegion(0,0); M->RegisterHealthBarPresentation(0,0); F.Step(); TestEqual(TEXT("恢复授权后重新绑定"),F.Active().Num(),1);
        auto* Current = F.Active()[0]; const FVector BeforeCallback = Current->GetComponentLocation();
        M->SetActorLocation(M->GetActorLocation() + FVector(50,0,0));
        FOnAttributeChangeData D; D.NewValue = 1; D.OldValue = 80; Old.Broadcast(D);
        TestEqual(TEXT("旧Generation回调不得触发新租约更新"), Current->GetComponentLocation(), BeforeCallback);
        TestEqual(TEXT("旧授权回调不能串条"), FBarProbe::Bar(Current)->GetPercent(), .8f);
        F.Step(.001f); TestTrue(TEXT("有效租约仍能正常跟随"), FBarProbe::At(Current,M));
    }
    if (Case == TEXT("Lease.OldCallback") || Case == TEXT("Lease.OldVersion"))
    {
        auto Old = M->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute());
        M->UnregisterHealthBarPresentation();
        TestFalse(TEXT("归还退订Health"), M->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).IsBoundToObject(Pool));
        TestFalse(TEXT("归还退订MaxHealth"), M->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetMaxHealthAttribute()).IsBoundToObject(Pool));
        TestEqual(TEXT("复用前清空旧显示"), Bar->GetPercent(), 0.f); TestFalse(TEXT("复用前隐藏"), First->IsVisible());
        auto* N = Case == TEXT("Lease.OldVersion") ? M : F.Monster(700);
        if (N == M) { M->RegisterHealthBarPresentation(0,0); F.Damage(M,30); } else { F.Damage(N,50); }
        F.Step();
        if (TestEqual(TEXT("归还后复用一个槽"), F.Active().Num(), 1))
        {
            TestEqual(TEXT("复用真实组件身份"), F.Active()[0], First);
            const FVector BeforeCallback = First->GetComponentLocation(); N->SetActorLocation(N->GetActorLocation() + FVector(50,0,0));
            FOnAttributeChangeData D; D.NewValue = 1; Old.Broadcast(D);
            TestEqual(TEXT("旧目标或旧Version回调不得触发复用租约更新"), First->GetComponentLocation(), BeforeCallback);
            TestEqual(TEXT("旧目标委托与快照不能覆盖新条"), FBarProbe::Bar(First)->GetPercent(), .5f);
            F.Step(.001f); TestTrue(TEXT("新Version正常跟随"), FBarProbe::At(First,N));
            if (N != M) { F.Damage(M,10); TestEqual(TEXT("旧目标退订后不影响新条"), FBarProbe::Bar(First)->GetPercent(), .5f); }
        }
    }
    if (Case.StartsWith(TEXT("Death.")) || Case.StartsWith(TEXT("Config.Hold")))
    {
        F.Damage(M,1000);
        if (Case == TEXT("Config.HoldZero")) { return TestEqual(TEXT("负hold钳零立即归还"),F.Active().Num(),0); }
        TestEqual(TEXT("仅已有租约保留死亡条"),F.Active().Num(),1); TestEqual(TEXT("死亡显示空条"),Bar->GetPercent(),0.f);
        if (Case == TEXT("Death.Destroy")) { M->Destroy(); TestEqual(TEXT("销毁不等待hold"),F.Active().Num(),0); }
        else if (Case == TEXT("Death.Revoke")) { F.PC->RevokeHealthBarAuthorization(); TestEqual(TEXT("死亡条撤权立即归还"),F.Active().Num(),0); }
        else if (Case == TEXT("Death.Offscreen")) { F.Camera(FVector(0,0,208),FRotator(0,180,0)); F.Step(.001f); TestEqual(TEXT("死亡条离屏立即归还"),F.Active().Num(),0); }
        else { F.Step(.29f); TestEqual(TEXT("0.3秒前保持"),F.Active().Num(),1); F.Step(.02f); TestEqual(TEXT("到期归还不再借尸体"),F.Active().Num(),0); }
    }
    if (Case == TEXT("Lifecycle.TargetWorld"))
    {
        FBarWorld Next(*this); Next.Start(); M->Rename(nullptr, Next.World->PersistentLevel); F.Step(.001f);
        TestEqual(TEXT("目标迁移World立即归还"), F.Active().Num(), 0);
        return TestFalse(TEXT("不继续订阅异World目标"), M->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).IsBoundToObject(Pool));
    }
    if (Case == TEXT("Lifecycle.EndPlay"))
    {
        F.PC->RouteEndPlay(EEndPlayReason::LevelTransition); F.PC->RouteEndPlay(EEndPlayReason::LevelTransition);
        TestEqual(TEXT("Controller结束立即清显示"), F.Active().Num(), 0); TestNull(TEXT("结束释放Widget资源"), First->GetUserWidgetObject());
        return TestFalse(TEXT("结束退订ASC"), M->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute()).IsBoundToObject(Pool));
    }
    if (Case == TEXT("Lifecycle.SeamlessWorld") || Case == TEXT("Lifecycle.SeamlessNoPre"))
    {
        auto OldCallback = M->GetCombatASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetHealthAttribute());
        const auto OldScope = F.PC->GetHealthBarAuthorization(); FBarWorld Next(*this); Next.Start(); auto* Kept = F.PC;
        if (Case == TEXT("Lifecycle.SeamlessWorld")) { Kept->PreClientTravel(TEXT("/Game/Next"), TRAVEL_Relative, true); }
        Kept->UnPossess(); Kept->PlayerState = nullptr; F.World->RemoveController(Kept);
        Kept->Rename(nullptr, Next.World->PersistentLevel); Next.World->AddController(Kept); Kept->PostSeamlessTravel();
        TestTrue(TEXT("迁移销毁旧World租约Actor"), First->GetOwner()->IsActorBeingDestroyed());
        TestNull(TEXT("迁移释放旧纹理Widget引用"), First->GetUserWidgetObject());
        Kept->SetHealthBarRegion(0,0); Kept->AY01PlayerController::Tick(0); F.Camera();
        TestTrue(TEXT("夹具迁移后显式授权保持有效"), Kept->GetHealthBarAuthorization().IsValidScope());
        auto* NewPool = FBarProbe::Pool(Kept); if (!TestNotNull(TEXT("新World独立重建池"), NewPool)) { return false; }
        TestNotEqual(TEXT("旧池不可复用到新World"), NewPool, Pool);
        TestNotEqual(TEXT("新World授权Epoch不同"), Kept->GetHealthBarAuthorization().WorldEpoch, OldScope.WorldEpoch);
        TestEqual(TEXT("新World预热32"), FBarProbe::Entries(Next.World, Kept).Num(), 32);
        auto* NewMonster = Next.Monster(); Next.Damage(NewMonster,50);
        NewPool->TickComponent(.11f, LEVELTICK_All, &NewPool->PrimaryComponentTick);
        auto NewActive = FBarProbe::Entries(Next.World, Kept, true);
        if (TestEqual(TEXT("迁移后只显示新World目标"), NewActive.Num(), 1))
        {
            FOnAttributeChangeData D; D.NewValue = 1; OldCallback.Broadcast(D);
            FWorldDelegates::OnWorldCleanup.Broadcast(F.World, true, true);
            TestFalse(TEXT("旧World清理不销毁新池"), NewPool->IsBeingDestroyed());
            TestEqual(TEXT("旧World回调不能串新条"), FBarProbe::Bar(NewActive[0])->GetPercent(), .5f);
        }
        return true;
    }
    if (Case == TEXT("Lifecycle.Travel")) { F.PC->PreClientTravel(TEXT("/Game/Next"),TRAVEL_Relative,true); F.PC->Tick(0); TestNull(TEXT("旅行销毁池"),FBarProbe::Pool(F.PC)); TestEqual(TEXT("旅行立即销毁条目"),FBarProbe::Entries(F.World,F.PC).Num(),0); }
    if (Case == TEXT("Lifecycle.WorldCleanup")) { FWorldDelegates::OnWorldCleanup.Broadcast(F.World,true,true); TestTrue(TEXT("World清理立即释放"),Pool->IsBeingDestroyed()); TestEqual(TEXT("无残余显示"),F.Active().Num(),0); }
    if (Case == TEXT("Lifecycle.Idempotent")) { F.PC->Tick(0); F.PC->Tick(0); TestEqual(TEXT("重复Ensure不重复预热"),FBarProbe::Entries(F.World,F.PC).Num(),32); Pool->DestroyComponent(); Pool->DestroyComponent(); TestEqual(TEXT("重复销毁安全"),F.Active().Num(),0); F.PC->Tick(0); TestEqual(TEXT("可重新建立独立池"),FBarProbe::Entries(F.World,F.PC).Num(),32); }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01BarVerificationTest, "Y01.Playground.R27.Verification", EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01BarVerificationTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* N : {TEXT("ExplicitMode"), TEXT("TransientProfile"), TEXT("RealDamage"), TEXT("ReadOnlyObservation"), TEXT("Grid129")})
    { Names.Add(N); Commands.Add(N); }
}
bool FY01BarVerificationTest::RunTest(const FString& Case)
{
    using V = AY01MonsterHealthBarVerification;
    if (Case == TEXT("ExplicitMode"))
    {
        TestFalse(TEXT("自然模式不得控制玩法"), V::Controlled(TEXT("-Y01R27Trace")));
        TestTrue(TEXT("显式隔离开关生效"), V::Controlled(TEXT("-Y01R27Verify")));
        TestFalse(TEXT("相似前缀不是授权"), V::Controlled(TEXT("-Y01R27VerifyOther"))); return true;
    }
    FBarWorld F(*this);
    if (Case == TEXT("TransientProfile"))
    {
        TestTrue(TEXT("自然场保留原配置对象"), V::PrepareProfile(F.Profile.Get(), false) == F.Profile.Get());
        auto* P = V::PrepareProfile(F.Profile.Get(), true);
        if (!TestTrue(TEXT("隔离配置必须深拷贝"), P != F.Profile.Get())) { return false; }
        TestTrue(TEXT("隔离副本仅瞬态"), P->GetOutermost() == GetTransientPackage());
        TestFalse(TEXT("仅隔离停自动普攻"), P->bR34AutoAttackEnabled);
        for (const auto& Pair : P->Tables) { TestTrue(TEXT("数据表不共享写入"), Pair.Value != F.Profile->Tables[Pair.Key]); }
        return true;
    }
    F.Start(); auto* M = F.Monster();
    if (Case == TEXT("RealDamage"))
    {
        auto* S = F.PC->GetPlayerState<AY01PlayerState>()->GetCombatASC();
        TestTrue(TEXT("验证工具走真实GAS伤害"), V::Damage(S, M, 25));
        TestEqual(TEXT("ASC实际扣血"), Y01Playground::Health(M->GetCombatASC()), 75.f);
        TestTrue(TEXT("实际伤害锁存资格"), M->GetHealthBarPresentationState().bEverActuallyDamaged);
        TestFalse(TEXT("拒绝零伤"), V::Damage(S,M,0)); return true;
    }
    if (Case == TEXT("Grid129"))
    {
        TSet<FVector> Positions;
        for (int32 I=0; I<129; ++I) { Positions.Add(V::GridPosition(I)); }
        return TestEqual(TEXT("129目标必须不同位置，不能重叠冒充压力"), Positions.Num(), 129);
    }
    F.Damage(M); F.Step(); const auto Before = M->GetHealthBarPresentationState();
    const FString JSON = V::Observe(F.PC);
    TestTrue(TEXT("只读实数对象数"), JSON.Contains(TEXT("\"objects\":32")));
    TestTrue(TEXT("只读真实活动租约"), JSON.Contains(TEXT("\"active\":1")));
    TestTrue(TEXT("包含候选及租约而非仅计数"), JSON.Contains(TEXT("\"candidates\"")) && JSON.Contains(TEXT("\"leases\"")));
    TestEqual(TEXT("观察不改资格版本"), M->GetHealthBarPresentationState().Revision, Before.Revision);
    TestEqual(TEXT("观察不改ASC"), Y01Playground::Health(M->GetCombatASC()), 80.f); return true;
}
#endif
