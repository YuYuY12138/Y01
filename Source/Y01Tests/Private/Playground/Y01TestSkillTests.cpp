#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "HAL/PlatformProcess.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01PlaygroundSeed.h"
#include "Playground/Y01TestSkill.h"
#include "Playground/Y01PlaygroundHUD.h"
#include "Engine/Canvas.h"
#include "CanvasTypes.h"
#include "Components/LineBatchComponent.h"
#include "Components/SphereComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Playground/Y01PlaygroundPassiveDamageEffect.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Y01MatchFixture.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMeshActor.h"
#include "EngineUtils.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
// 反射接线使测试在功能尚不存在时也可编译，红灯来自真实行为断言而不是编译错误。
void SetTestNumber(FY01SkillRow& Row, const TCHAR* Field, double Value)
{
    auto* Config = FindFProperty<FStructProperty>(FY01SkillRow::StaticStruct(), TEXT("TestSkill"));
    if (!Config) { return; }
    void* Data = Config->ContainerPtrToValuePtr<void>(&Row);
    if (auto* P = FindFProperty<FFloatProperty>(Config->Struct, Field)) { P->SetPropertyValue_InContainer(Data, Value); }
    if (auto* P = FindFProperty<FEnumProperty>(Config->Struct, Field))
    { P->GetUnderlyingProperty()->SetIntPropertyValue(P->ContainerPtrToValuePtr<void>(Data), static_cast<int64>(Value)); }
}
void ConfigureTestSkill(FY01SkillRow& Row, int32 Mode)
{
    if (auto* Class = FindObject<UClass>(nullptr, TEXT("/Script/Y01Tests.Y01TestSkillAbility"))) { Row.Ability = Class; }
    SetTestNumber(Row, TEXT("Mode"), Mode);
    SetTestNumber(Row, TEXT("DamageCoefficient"), 1);
    SetTestNumber(Row, TEXT("Radius"), Mode == 1 ? 250 : 12);
    SetTestNumber(Row, TEXT("HalfHeight"), Mode == 1 ? 120 : 0);
    SetTestNumber(Row, TEXT("ActiveSeconds"), Mode == 1 ? 0.35 : 0);
    SetTestNumber(Row, TEXT("TargetRange"), Mode == 2 ? 800 : 0);
    SetTestNumber(Row, TEXT("Speed"), Mode == 2 ? 400 : 0);
    SetTestNumber(Row, TEXT("Lifetime"), Mode == 2 ? 2.5 : 0);
    SetTestNumber(Row, TEXT("HitFeedbackSeconds"), Mode == 2 ? 0.1 : 0);
    Row.CooldownSeconds = Mode == 1 ? 1.5f : 2.f;
}
FY01DataRef SkillRef(EY01TableKind Type, FName Name = TEXT("Default"))
{ FY01DataRef R; R.Type = Type; R.RowName = Name; return R; }
struct FSkillLog : FOutputDevice
{
    struct FEvent { FString Line; double World; };
    UWorld* World;
    FString CommandLine = FCommandLine::Get();
    TArray<FEvent> Events;
    explicit FSkillLog(UWorld* InWorld, bool Trace) : World(InWorld)
    {
        FCommandLine::Set(*(CommandLine.Replace(TEXT("-Y01SkillTrace"), TEXT("")) + (Trace ? TEXT(" -Y01SkillTrace") : TEXT(""))));
        GLog->AddOutputDevice(this);
    }
    ~FSkillLog() { GLog->RemoveOutputDevice(this); FCommandLine::Set(*CommandLine); }
    // 同步接收业务线程日志，避免默认异步日志队列在后续tick读取到另一个World时间。
    bool CanBeUsedOnMultipleThreads() const override { return true; }
    void Serialize(const TCHAR* Text, ELogVerbosity::Type, const FName&) override
    { if (IsInGameThread() && FString(Text).StartsWith(TEXT("Y01Skill "))) { Events.Add({Text, World->GetTimeSeconds()}); } }
    TArray<FEvent> Find(const TCHAR* Kind) const
    { return Events.FilterByPredicate([&](const FEvent& E) { return E.Line.StartsWith(FString(TEXT("Y01Skill ")) + Kind + TEXT(" ")); }); }
    static FString Field(const FString& Line, const TCHAR* Key)
    {
        const FString Token = FString(TEXT(" ")) + Key + TEXT("="); const int32 At = Line.Find(Token);
        if (At == INDEX_NONE) { return {}; }
        FString Value = Line.Mid(At + Token.Len()); int32 End;
        if (Value.FindChar(' ', End)) { Value.LeftInline(End); } return Value;
    }
    static double Number(const FString& Line, const TCHAR* Key)
    { const FString Value = Field(Line, Key); return Value.IsEmpty() ? -1.e30 : FCString::Atod(*Value); }
    static FVector Position(const FString& Line)
    { FVector Value; const int32 At = Line.Find(TEXT(" Position=")); return At != INDEX_NONE && Value.InitFromString(Line.Mid(At + 10)) ? Value : FVector(MAX_flt); }
};
struct FSkillWorld
{
    TStrongObjectPtr<UY01PlaygroundProfile> Profile{Y01PlaygroundEditor::CreateSeed(GetTransientPackage())};
    UWorld* World;
    AY01PlaygroundGameMode* Mode;
    AY01PlaygroundController* A;
    AY01PlaygroundController* B;
    AY01Monster* M = nullptr;
    AY01Monster* N = nullptr;
    FSkillWorld()
    {
        Profile->bTrainingEnabled = true;
        ConfigureTestSkill(*Row(0), 1); ConfigureTestSkill(*Row(1), 2);
        auto* Table = Profile->Tables[EY01TableKind::TrainingStage].Get(); Table->EmptyTable();
        FY01TrainingStageRow R; R.SpawnInterval = 30; R.MaxAlive = 6; R.Reward = SkillRef(EY01TableKind::RewardPackage);
        FY01SpawnEntry E; E.Monster = SkillRef(EY01TableKind::Monster); E.Count = 2; R.Spawns = {E}; Table->AddRow(TEXT("Default"), R);
        Profile->Tables[EY01TableKind::Monster]->FindRow<FY01MonsterRow>(TEXT("Default"), TEXT("TestSkill"))->Health = 1000;
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01PlaygroundGameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01PlaygroundGameMode>();
        World->SpawnActor<AY01PlaygroundConfig>()->Profile = Profile.Get(); A = Player(); B = Player();
    }
    ~FSkillWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    FY01SkillRow* Row(int32 Slot) const
    { return Profile->Tables[EY01TableKind::Skill]->FindRow<FY01SkillRow>(Slot == 0 ? TEXT("BasicAttack") : TEXT("HeavyAttack"), TEXT("TestSkill")); }
    AY01PlaygroundController* Player()
    {
        auto* PC = World->SpawnActor<AY01PlaygroundController>(); auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01PlaygroundCharacter>(FVector(0,0,100), FRotator::ZeroRotator);
        PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement(); Mode->GetFlowState()->AddPlayerState(PS); return PC;
    }
    AY01PlayerState* PS(AY01PlaygroundController* PC) const { return PC->GetPlayerState<AY01PlayerState>(); }
    UY01CombatAbilitySystem* ASC() const { return PS(A)->GetCombatASC(); }
    UY01TrainingRooms* Rooms() const { return Mode->GetTraining(); }
    FVector Origin() const { return A->GetPawn()->GetActorLocation(); }
    void Freeze(AY01Monster* Monster) { Monster->GetCharacterMovement()->DisableMovement(); }
    void Start()
    {
        Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay();
        Rooms()->Request(A, true); auto Monsters = Rooms()->Monsters(PS(A));
        if (Monsters.Num() == 2) { M = Monsters[0]; N = Monsters[1]; Freeze(M); Freeze(N); Place(M, FVector(400,0,0)); Place(N, FVector(0,700,0)); }
    }
    void Place(AY01Monster* Monster, FVector Delta) { Monster->SetActorLocation(Origin() + Delta); }
    void Tick(float Seconds)
    { for (int32 I = 0; I < FMath::CeilToInt(Seconds / 0.01f); ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, 0.01f); } }
    bool CastSkill(int32 Slot) { Mode->RefreshTarget(PS(A)); return PS(A)->TryActivateSkillSlot(Slot); }
    TArray<AActor*> Attacks() const
    {
        TArray<AActor*> Result;
        for (TActorIterator<AActor> It(World); It; ++It)
        { if (It->GetClass()->GetName() == TEXT("Y01TestAttack") && !It->IsActorBeingDestroyed()) { Result.Add(*It); } }
        return Result;
    }
    AStaticMeshActor* Wall(FVector Delta)
    {
        auto* W = World->SpawnActor<AStaticMeshActor>(Origin() + Delta, FRotator::ZeroRotator);
        W->GetStaticMeshComponent()->SetMobility(EComponentMobility::Movable);
        W->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
        W->SetActorScale3D(FVector(0.1,3,3)); W->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll")); return W;
    }
    float HP(AY01Monster* Monster) const { return Y01Playground::Health(Monster->GetCombatASC()); }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01TestSkills, "Y01.Playground.TestSkills",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01TestSkills::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("AreaEmpty"), TEXT("AreaInside"), TEXT("AreaOutside"), TEXT("AreaBoundary"),
        TEXT("AreaHeightBoundary"), TEXT("AreaHeightOutside"), TEXT("AreaHeightBelow"), TEXT("AreaFixedCenter"),
        TEXT("AreaNewEntryOnce"), TEXT("AreaIgnoresWall"), TEXT("AreaCooldown"), TEXT("ParallelMovement"),
        TEXT("BallNoEarlyDamage"), TEXT("BallMovingTarget"), TEXT("BallSingleHit"), TEXT("BallWall"), TEXT("BallTimeout"),
        TEXT("BallOriginalDead"), TEXT("BallOverlap"), TEXT("BallNoTarget"), TEXT("BallRangeBoundary"), TEXT("BallRangeOutside"),
        TEXT("BallNearest"), TEXT("BallSweep"), TEXT("BallFeedback"), TEXT("Snapshot"), TEXT("Coefficient"), TEXT("GrantIdentity"),
        TEXT("CrossRoom"), TEXT("SourcePermission"), TEXT("TargetPermission"), TEXT("TargetSession"), TEXT("Reenter"),
        TEXT("CancelExit"), TEXT("CancelDeath"), TEXT("CancelEnded"), TEXT("CancelLogout"), TEXT("CancelFeedback"),
        TEXT("MainLogout"), TEXT("RewardOnce"), TEXT("BaseRangePreserved"), TEXT("ReplicationLifecycle"), TEXT("RejectClient"),
        TEXT("CommitSnapshot"), TEXT("HudConfig"), TEXT("ForgedEffect"),
        TEXT("Log.AreaTimeout"), TEXT("Log.Cancel"), TEXT("Log.HitFeedback"), TEXT("Log.TraceDefault"),
        TEXT("Log.Trace400"), TEXT("Log.Trace200"), TEXT("Log.BallTimeout"), TEXT("Log.Wall"), TEXT("Log.WallAtDeadline"),
        TEXT("R33.Clock.Initial"), TEXT("R33.Clock.NotReady"), TEXT("R33.Clock.Skew"), TEXT("R33.Clock.Expired"),
        TEXT("R33.Clock.Slots"), TEXT("R33.Clock.TrainingPreparation"), TEXT("R33.Clock.Converges"),
        TEXT("R33.Clock.Frozen"), TEXT("R33.Clock.Capture"), TEXT("R33.FeedbackDraw"), TEXT("R33.FeedbackHUD") })
    { Names.Add(Name); Commands.Add(Name); }
    for (const TCHAR* Field : { TEXT("DamageCoefficient"), TEXT("Radius"), TEXT("HalfHeight"), TEXT("ActiveSeconds"), TEXT("TargetRange"), TEXT("Speed"), TEXT("Lifetime"), TEXT("HitFeedbackSeconds") })
    {
        for (const TCHAR* Bad : { TEXT("Negative"), TEXT("NaN"), TEXT("Infinity") })
        { FString Name = FString(TEXT("Invalid.")) + Field + TEXT(".") + Bad; Names.Add(Name); Commands.Add(Name); }
    }
    for (const TCHAR* Field : { TEXT("Radius"), TEXT("ActiveSeconds"), TEXT("TargetRange"), TEXT("Speed"), TEXT("Lifetime") })
    { FString Name = FString(TEXT("Invalid.")) + Field + TEXT(".Zero"); Names.Add(Name); Commands.Add(Name); }
}
bool FY01TestSkills::RunTest(const FString& Case)
{
    TGuardValue<uint64> FrameGuard(GFrameCounter, GFrameCounter); FSkillWorld F;
    if (Case.StartsWith(TEXT("Invalid.")))
    {
        TArray<FString> Parts; Case.ParseIntoArray(Parts, TEXT("."));
        const float Bad = Parts[2] == TEXT("NaN") ? std::numeric_limits<float>::quiet_NaN() :
            Parts[2] == TEXT("Infinity") ? std::numeric_limits<float>::infinity() : Parts[2] == TEXT("Zero") ? 0 : -1;
        const int32 Slot = Parts[1] == TEXT("ActiveSeconds") || Parts[1] == TEXT("HalfHeight") ? 0 : 1;
        SetTestNumber(*F.Row(Slot), *Parts[1], Bad);
        TestFalse(TEXT("非法测试配置不能通过目录校验"), F.Profile->Validate().IsValid());
        FString Error; TestFalse(TEXT("直接ASC初始化也拒绝非法配置"), F.ASC()->TryInitializeFromData(F.Profile->BuildCatalog(), F.Profile->Character, Error));
        TestTrue(TEXT("拒绝不产生半套授予"), F.ASC()->GetGrantedSources().IsEmpty()); return true;
    }
    if (Case == TEXT("Coefficient")) { SetTestNumber(*F.Row(1), TEXT("DamageCoefficient"), 2); }
    if (Case == TEXT("BallSweep")) { SetTestNumber(*F.Row(1), TEXT("Speed"), 40000); }
    if (Case == TEXT("HudConfig") || Case == TEXT("Log.Trace200") || Case == TEXT("Log.BallTimeout"))
    { SetTestNumber(*F.Row(1), TEXT("Speed"), 200); }
    if (Case == TEXT("Log.WallAtDeadline"))
    { SetTestNumber(*F.Row(1), TEXT("Speed"), 40000); SetTestNumber(*F.Row(1), TEXT("Lifetime"), 0.005); }
    if (Case == TEXT("GrantIdentity"))
    {
        auto* Table = F.Profile->Tables[EY01TableKind::Skill].Get(); Table->AddRow(TEXT("RenamedBall"), *F.Row(1));
        auto* C = F.Profile->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("TestSkill"));
        C->InitialSkills[0] = SkillRef(EY01TableKind::Skill, TEXT("RenamedBall"));
    }
    F.Start(); if (!TestTrue(TEXT("真实训练夹具初始化"), F.Mode->IsMatchConfigured() && F.M && F.N)) { return false; }
    if (Case.StartsWith(TEXT("R33.Clock.")) || Case == TEXT("R33.FeedbackHUD"))
    {
        TestTrue(TEXT("准备期与个人训练可同时观测"), F.Mode->TryStartNextWave());
        TestTrue(TEXT("真实范围提交"), F.CastSkill(0)); TestTrue(TEXT("真实球提交"), F.CastSkill(1));
        F.Tick(0.2f); F.A->CaptureObserver(F.Mode);
        auto* Sample = FindFProperty<FDoubleProperty>(FY01PlaygroundObserver::StaticStruct(), TEXT("ServerSampleTime"));
        auto* Preparation = FindFProperty<FDoubleProperty>(FY01PlaygroundObserver::StaticStruct(), TEXT("PreparationDeadline"));
        if (Case == TEXT("R33.Clock.Capture"))
        {
            if (!TestNotNull(TEXT("Observer必须复制明确的服务器游戏时间样本"), Sample)
                || !TestNotNull(TEXT("准备截止必须与训练和技能同一快照"), Preparation)) { return false; }
            const auto& O = F.A->GetObserver();
            TestEqual(TEXT("采样准确读取服务器World，不是GS估值"), Sample->GetPropertyValue_InContainer(&O), F.World->GetTimeSeconds());
            TestEqual(TEXT("准备截止取同次观测"), Preparation->GetPropertyValue_InContainer(&O), F.Mode->GetFlowState()->GetWaveSnapshot().PreparationDeadline);
            TestEqual(TEXT("训练截止取同次观测"), O.Training.NextBatchAt, F.Rooms()->Observe(F.PS(F.A)).NextBatchAt);
            for (const auto& Grant : F.ASC()->GetGrantedSources())
            { if (Grant.Source.Type == EY01TableKind::Skill && Grant.Slot >= 0 && Grant.Slot < 2)
                { TestEqual(TEXT("权威Deadline原样复制"), O.Slots[Grant.Slot].Deadline, Grant.NextActivationTime); } }
            F.Tick(0.2f); F.A->CaptureObserver(F.Mode);
            TestEqual(TEXT("新采样推进至当前World"), Sample->GetPropertyValue_InContainer(&F.A->GetObserver()), F.World->GetTimeSeconds());
            return true;
        }
        // 只向观测端复制测试快照，不改ASC权威Deadline；停掉自动采样模拟不再收到复制。
        F.B->SetActorTickEnabled(false);
        auto* ObserverProperty = FindFProperty<FStructProperty>(F.B->GetClass(), TEXT("Observer"));
        if (!TestNotNull(TEXT("已有OwnerOnly观测属性"), ObserverProperty)) { return false; }
        auto* O = ObserverProperty->ContainerPtrToValuePtr<FY01PlaygroundObserver>(F.B);
        *O = F.A->GetObserver();
        auto SetSample = [&](double Time) { if (Sample) { Sample->SetPropertyValue_InContainer(O, Time); } };
        SetSample(20); if (Preparation) { Preparation->SetPropertyValue_InContainer(O, 24); }
        O->Training.NextBatchAt = 26; O->Slots[0].Deadline = 21.5; O->Slots[1].Deadline = 23.25;
        auto* GS = F.Mode->GetFlowState();
        auto* Offset = FindFProperty<FFloatProperty>(GS->GetClass(), TEXT("ServerWorldTimeSecondsDelta"));
        if (!TestNotNull(TEXT("真实GS时钟偏差可设置"), Offset)) { return false; }
        Offset->SetPropertyValue_InContainer(GS, -1000.f);
        auto* HUD = F.World->SpawnActor<AY01PlaygroundHUD>(); HUD->PlayerOwner = F.B;
        struct FDraw { FBox2D Bounds; FString Text; }; TArray<FDraw> Draws;
        HUD->ObserveDrawForTest = [&](const FBox2D& Bounds, const FString& Text) { Draws.Add({Bounds, Text}); };
        auto Render = [&]
        {
            Draws.Reset(); FCanvas Batch(nullptr, nullptr, F.World, ERHIFeatureLevel::SM5);
            TStrongObjectPtr<UCanvas> Canvas(NewObject<UCanvas>()); Canvas->Init(1280, 800, nullptr, &Batch); Canvas->Update();
            HUD->SetCanvas(Canvas.Get(), nullptr); HUD->DrawHUD(); HUD->SetCanvas(nullptr, nullptr);
        };
        auto HasText = [&](const TCHAR* Text) { return Draws.ContainsByPredicate([&](const FDraw& D) { return D.Text.Contains(Text); }); };
        auto SlotText = [&](int32 Slot)
        {
            const auto* D = Draws.FindByPredicate([&](const FDraw& Item) { return Item.Bounds.Min.Equals(FVector2D(472 + Slot * 195, 690)); });
            return D ? D->Text : FString(TEXT("缺失实际绘制"));
        };
        if (Case == TEXT("R33.Clock.Initial"))
        {
            *O = FY01PlaygroundObserver(); Render();
            TestTrue(TEXT("初始未采样明确等待服务器快照"), HasText(TEXT("等待服务器快照")));
            TestFalse(TEXT("初始不能假Ready"), HasText(TEXT("Ready"))); return true;
        }
        if (Case == TEXT("R33.Clock.NotReady"))
        {
            O->bReady = false; Render();
            TestTrue(TEXT("配置未就绪仍等待服务器快照"), HasText(TEXT("等待服务器快照")));
            TestFalse(TEXT("未就绪不显示数值下批或准备"), HasText(TEXT("下批 6.0s")) || HasText(TEXT("准备：4.0 秒")));
            return true;
        }
        if (Case == TEXT("R33.Clock.Expired"))
        {
            SetSample(30); Render(); TestEqual(TEXT("样本超过deadline归零Ready"), SlotText(0), FString(TEXT("Ready")));
            TestEqual(TEXT("第二槽同样归零"), SlotText(1), FString(TEXT("Ready")));
            TestTrue(TEXT("训练和准备也归零不显示负数"), HasText(TEXT("下批 0.0s")) && HasText(TEXT("准备：0.0 秒"))); return true;
        }
        Render();
        TestTrue(TEXT("倒计时必须明确标注服务器快照"), HasText(TEXT("服务器快照")));
        TestEqual(TEXT("极端落后GS不能污染快照槽1CD"), SlotText(0), FString(TEXT("CD 1.50 s")));
        if (Case == TEXT("R33.Clock.Skew"))
        {
            Offset->SetPropertyValue_InContainer(GS, 1000.f); Render();
            TestTrue(TEXT("实际GS getter已极端领先"), GS->GetServerWorldTimeSeconds() > 999);
            TestEqual(TEXT("极端领先GS也不能提前Ready"), SlotText(0), FString(TEXT("CD 1.50 s")));
        }
        if (Case == TEXT("R33.Clock.Slots"))
        { TestEqual(TEXT("第二槽独立且不夹到配置2秒"), SlotText(1), FString(TEXT("CD 3.25 s"))); }
        if (Case == TEXT("R33.Clock.TrainingPreparation"))
        { TestTrue(TEXT("训练同样本相减"), HasText(TEXT("下批 6.0s"))); TestTrue(TEXT("准备不拿另一份GS截止做差"), HasText(TEXT("准备：4.0 秒"))); }
        if (Case == TEXT("R33.Clock.Converges"))
        {
            SetSample(21); Render(); TestEqual(TEXT("新样本离散收敛"), SlotText(0), FString(TEXT("CD 0.50 s")));
            SetSample(24); Render(); TestEqual(TEXT("收到已过期样本才Ready"), SlotText(0), FString(TEXT("Ready")));
        }
        if (Case == TEXT("R33.Clock.Frozen"))
        {
            const auto Before = SlotText(0); const double AuthorityDeadline = F.A->GetObserver().Slots[0].Deadline;
            FPlatformProcess::Sleep(0.03f); F.Tick(2.f); Render();
            TestEqual(TEXT("世界与墙钟推进但没新样本不自行跑到Ready"), SlotText(0), Before);
            TestTrue(TEXT("训练准备同时静态保留"), HasText(TEXT("下批 6.0s")) && HasText(TEXT("准备：4.0 秒")));
            TestTrue(TEXT("必须提示无新样本保持"), HasText(TEXT("无新样本保持")));
            TestEqual(TEXT("读取HUD不修改权威截止"), F.A->GetObserver().Slots[0].Deadline, AuthorityDeadline);
        }
        if (Case == TEXT("R33.FeedbackHUD"))
        {
            HUD->NotifyHitBoxClick(TEXT("SkillDetails")); Render(); FString Details;
            for (const auto& D : Draws) { if (D.Bounds.Min.X >= 932 && D.Bounds.Min.Y >= 218 && D.Bounds.Min.Y < 558) { Details += D.Text; } }
            TestTrue(TEXT("实际展开Canvas说明区标注非碰撞范围"), Details.Contains(TEXT("绿色标记=命中反馈，非碰撞范围")));
        }
        return true;
    }
    if (Case == TEXT("R33.FeedbackDraw"))
    {
        TestEqual(TEXT("真实standalone世界非DS"), F.World->GetNetMode(), NM_Standalone);
        auto* Foreground = F.World->GetLineBatcher(UWorld::ELineBatcherType::Foreground);
        auto* Persistent = F.World->GetLineBatcher(UWorld::ELineBatcherType::ForegroundPersistent);
        if (!TestNotNull(TEXT("前景linebatcher"), Foreground) || !TestNotNull(TEXT("持久linebatcher"), Persistent)) { return false; }
        TestTrue(TEXT("真实提交球"), F.CastSkill(1));
        auto* Attack = F.Attacks().IsEmpty() ? nullptr : Cast<AY01TestAttack>(F.Attacks()[0]);
        if (!TestNotNull(TEXT("真实攻击Actor"), Attack)) { return false; }
        Attack->Tick(0); TestEqual(TEXT("飞行阶段没有绿色标记"), Foreground->BatchedPoints.Num(), 0);
        F.Tick(0.9f);
        if (!TestEqual(TEXT("真实扫掠命中后处于反馈"), Attack->GetState().Phase, EY01TestAttackPhase::HitFeedback)) { return false; }
        TestTrue(TEXT("实际目标已经扣血"), F.HP(F.M) < 1000);
        // nullrhi无Viewport；执行与GameViewportClient帧尾相同的临时batch清理，再采集一次真实Tick。
        const UWorld::ELineBatcherType Temporary[] = { UWorld::ELineBatcherType::World, UWorld::ELineBatcherType::Foreground };
        F.World->FlushLineBatchers(Temporary); Attack->Tick(0);
        const auto Position = Attack->GetState().Position; const auto EndsAt = Attack->GetState().EndsAt;
        TestEqual(TEXT("反馈Tick必须调用真实前景点绘制"), Foreground->BatchedPoints.Num(), 1);
        if (Foreground->BatchedPoints.Num() == 1)
        {
            const auto& Point = Foreground->BatchedPoints[0];
            TestTrue(TEXT("点中心严格等于复制State.Position，不抬高挪动"), Point.Position.Equals(Position, 0));
            TestEqual(TEXT("点为前景深度"), Point.DepthPriority, static_cast<uint8>(SDPG_Foreground));
            TestEqual(TEXT("点为纯绿色"), Point.Color, FLinearColor::Green);
            TestEqual(TEXT("标记固定12屏幕像素，不是碰撞半径"), Point.PointSize, 12.f);
        }
        TestEqual(TEXT("未使用持久前景batch"), Persistent->BatchedPoints.Num(), 0);
        auto* Shape = Attack->FindComponentByClass<UStaticMeshComponent>();
        auto* MID = Shape ? Cast<UMaterialInstanceDynamic>(Shape->GetMaterial(0)) : nullptr;
        TestTrue(TEXT("原Shape仍为绿色MID"), MID && MID->K2_GetVectorParameterValue(TEXT("Color")) == FLinearColor::Green);
        TestEqual(TEXT("真实Sphere半径保持12"), Attack->FindComponentByClass<USphereComponent>()->GetUnscaledSphereRadius(), 12.f);
        TestEqual(TEXT("反馈窗仍为0.1秒"), Attack->GetState().Config.HitFeedbackSeconds, 0.1f);
        TestEqual(TEXT("绘制不改EndsAt"), Attack->GetState().EndsAt, EndsAt);
        TestTrue(TEXT("绘制不改Actor位置"), Attack->GetActorLocation().Equals(Position, 0));
        F.Tick(0.12f); TestTrue(TEXT("自然到期真实Destroy"), Attack->IsActorBeingDestroyed());
        F.World->FlushLineBatchers(Temporary); Attack->Tick(0);
        TestEqual(TEXT("Destroy后不再绘制，无下一帧残留"), Foreground->BatchedPoints.Num(), 0);
        TestEqual(TEXT("Destroy没有持久残留"), Persistent->BatchedPoints.Num(), 0);
        TestTrue(TEXT("范围仍可释放"), F.CastSkill(0)); F.Tick(0.01f);
        TestEqual(TEXT("范围不产生绿色点"), Foreground->BatchedPoints.Num(), 0);
        AddInfo(TEXT("nullrhi仅验证真实Tick的linebatcher数据及Viewport同样的帧尾清理；可见性需另跑D3D12截图。"));
        return true;
    }
    if (Case.StartsWith(TEXT("Log.")))
    {
        // 世界已推进后暂停真实线程；日志必须读取业务时钟，不能用墙钟或日志前缀推断寿命。
        F.Tick(0.2f); FPlatformProcess::Sleep(0.04f);
        const bool Trace = Case == TEXT("Log.Trace400") || Case == TEXT("Log.Trace200");
        const bool Area = Case == TEXT("Log.AreaTimeout") || Case == TEXT("Log.Cancel");
        if (Case.StartsWith(TEXT("Log.Wall"))) { F.Wall(FVector(200,0,0)); }
        FSkillLog Log(F.World, Trace);
        TestTrue(TEXT("日志来自真实成功提交"), F.CastSkill(Area ? 0 : 1));
        if (!TestEqual(TEXT("日志夹具实例存在"), F.Attacks().Num(), 1)) { return false; }
        auto* Attack = Cast<AY01TestAttack>(F.Attacks()[0]);
        const auto Initial = Attack->GetState(); const FString ActorName = Attack->GetName();
        const FString SourceName = F.PS(F.A)->GetName(), CasterName = F.A->GetPawn()->GetName(), TargetName = F.M->GetName();
        if (Case == TEXT("Log.Cancel")) { F.Tick(0.08f); F.Rooms()->Request(F.A, false); }
        else if (Case == TEXT("Log.BallTimeout")) { F.Place(F.M, FVector(400,250,0)); F.Tick(2.6f); }
        else { F.Tick(Area ? 0.4f : Trace ? 2.1f : 1.3f); }
        auto CheckClock = [&](const FSkillLog::FEvent& Event)
        {
            TestTrue(TEXT("日志World等于输出瞬间业务世界时间：") + Event.Line,
                FMath::IsNearlyEqual(FSkillLog::Number(Event.Line, TEXT("World")), Event.World, 0.000002));
            TestEqual(TEXT("日志对应同一真实攻击实例"), FSkillLog::Field(Event.Line, TEXT("Actor")), ActorName);
        };
        const auto Commits = Log.Find(TEXT("COMMIT")), Ends = Log.Find(TEXT("END"));
        if (!TestEqual(TEXT("唯一提交日志"), Commits.Num(), 1) || !TestEqual(TEXT("唯一结束日志"), Ends.Num(), 1)) { return false; }
        for (const auto& E : {Commits[0], Ends[0]})
        {
            CheckClock(E);
            TestTrue(TEXT("StartedAt取业务状态不是墙钟"), FMath::IsNearlyEqual(FSkillLog::Number(E.Line, TEXT("StartedAt")), Initial.StartedAt, 0.000002));
            TestEqual(TEXT("模式来自业务配置"), FSkillLog::Number(E.Line, TEXT("Mode")), Area ? 1. : 2.);
        }
        TestTrue(TEXT("提交EndsAt为业务配置截止"), FMath::IsNearlyEqual(FSkillLog::Number(Commits[0].Line, TEXT("EndsAt")), Initial.EndsAt, 0.000002));
        const bool Cancelled = Case == TEXT("Log.Cancel") || Case.StartsWith(TEXT("Log.Wall"));
        const auto Expiries = Log.Find(TEXT("EXPIRE"));
        TestEqual(TEXT("仅自然到期发明确EXPIRE，取消不伪装超时"), Expiries.Num(), Cancelled ? 0 : 1);
        if (!Cancelled && Expiries.Num() == 1)
        {
            CheckClock(Expiries[0]);
            TestTrue(TEXT("自然结束实际世界时间达到业务截止"), FSkillLog::Number(Ends[0].Line, TEXT("World")) >= FSkillLog::Number(Ends[0].Line, TEXT("EndsAt")));
        }
        if (Case == TEXT("Log.Cancel"))
        {
            TestTrue(TEXT("取消发生在自然截止之前"), FSkillLog::Number(Ends[0].Line, TEXT("World")) < Initial.EndsAt);
            TestEqual(TEXT("取消仍为Active"), FSkillLog::Number(Ends[0].Line, TEXT("Phase")), 0.);
        }
        const auto Hits = Log.Find(TEXT("HIT"));
        if (!Area && !Cancelled && Case != TEXT("Log.BallTimeout"))
        {
            if (!TestEqual(TEXT("真实单次命中日志"), Hits.Num(), 1)) { return false; }
            CheckClock(Hits[0]);
            TestEqual(TEXT("可靠施法PlayerState身份"), FSkillLog::Field(Hits[0].Line, TEXT("Source")), SourceName);
            TestEqual(TEXT("可靠施法Pawn身份"), FSkillLog::Field(Hits[0].Line, TEXT("Caster")), CasterName);
            TestEqual(TEXT("可靠目标Actor身份"), FSkillLog::Field(Hits[0].Line, TEXT("Target")), TargetName);
            TestTrue(TEXT("HIT记录实际目标位置"), FMath::IsNearlyEqual(FSkillLog::Number(Hits[0].Line, TEXT("TargetX")), F.M->GetActorLocation().X, 0.000002));
            TestTrue(TEXT("HIT记录实际施法者位置"), FMath::IsNearlyEqual(FSkillLog::Number(Hits[0].Line, TEXT("SourceX")), F.Origin().X, 0.000002));
            TestTrue(TEXT("END的EndsAt是实际命中后反馈截止，不是旧寿命"), FMath::IsNearlyEqual(FSkillLog::Number(Ends[0].Line, TEXT("EndsAt")), Hits[0].World + Initial.Config.HitFeedbackSeconds, 0.000002));
            TestEqual(TEXT("反馈结束阶段准确"), FSkillLog::Number(Ends[0].Line, TEXT("Phase")), 1.);
        }
        const auto Steps = Log.Find(TEXT("STEP"));
        if (!Trace) { TestEqual(TEXT("普通运行不刷STEP"), Steps.Num(), 0); }
        else
        {
            TestTrue(TEXT("显式开关逐tick记录实际球位置"), Steps.Num() > 10);
            int32 Samples = 0; bool Terminal = false;
            for (const auto& E : Steps)
            {
                CheckClock(E);
                if (FSkillLog::Number(E.Line, TEXT("Terminal")) == 1) { Terminal = true; continue; }
                const double Dt = FSkillLog::Number(E.Line, TEXT("SampleWorld")) - FSkillLog::Number(E.Line, TEXT("PrevWorld"));
                const FVector Prev(FSkillLog::Number(E.Line, TEXT("PrevX")), FSkillLog::Number(E.Line, TEXT("PrevY")), FSkillLog::Number(E.Line, TEXT("PrevZ")));
                TestTrue(TEXT("非终止样本以真实位置/世界时间验证速度"), Dt > 0 && FMath::IsNearlyEqual(FVector::Dist(Prev, FSkillLog::Position(E.Line)) / Dt, static_cast<double>(Initial.Config.Speed), 0.15));
                ++Samples;
            }
            TestTrue(TEXT("包含足够非碰撞样本并明确标记最后截断样本"), Samples > 10 && Terminal);
        }
        return true;
    }
    if (Case == TEXT("MainLogout"))
    {
        TestTrue(TEXT("主线夹具先真实离开训练"), F.Rooms()->Request(F.A, false));
        TestTrue(TEXT("主线开始"), F.Mode->TryStartNextWave()); F.Tick(5.6f);
        if (!TestTrue(TEXT("主线波怪已生成，非训练怪"), F.Mode->IsCombatRunning() && F.Mode->GetWaveMonsters().Num() >= 2)) { return false; }
        F.M = F.Mode->GetWaveMonsters()[0]; F.N = F.Mode->GetWaveMonsters()[1];
        for (AY01Monster* Monster : F.Mode->GetWaveMonsters()) { F.Freeze(Monster); F.Place(Monster, FVector(0,900,0)); }
        F.Place(F.M, FVector(400,0,0)); F.Place(F.N, FVector(0,700,0));
        TestFalse(TEXT("主线攻击者确实不在训练"), F.Rooms()->Observe(F.PS(F.A)).bInside);
    }
    if (Case == TEXT("HudConfig"))
    {
        F.A->CaptureObserver(F.Mode);
        auto* Property = FindFProperty<FStrProperty>(FY01PlaygroundSlotView::StaticStruct(), TEXT("Description"));
        if (!TestNotNull(TEXT("HUD须有服务器授予来源的真实说明"), Property)) { return false; }
        const auto& Slots = F.A->GetObserver().Slots;
        const auto Area = Property->GetPropertyValue_InContainer(&Slots[0]);
        const auto Ball = Property->GetPropertyValue_InContainer(&Slots[1]);
        TestTrue(TEXT("范围说明包含固定中心/Actor中心/几何与测试标识"), Area.Contains(TEXT("固定")) && Area.Contains(TEXT("Actor中心"))
            && Area.Contains(TEXT("250")) && Area.Contains(TEXT("120")) && Area.Contains(TEXT("0.35")) && Area.Contains(TEXT("测试")));
        TestTrue(TEXT("球说明读取授予配置200而非硬编码400"), Ball.Contains(TEXT("200")) && Ball.Contains(TEXT("800"))
            && Ball.Contains(TEXT("12")) && Ball.Contains(TEXT("2.5")) && Ball.Contains(TEXT("测试")));
        return true;
    }
    if (Case == TEXT("CommitSnapshot"))
    {
        const FY01GrantedSource* Grant = nullptr;
        for (const auto& Item : F.ASC()->GetGrantedSources()) { if (Item.Slot == 1) { Grant = &Item; } }
        if (!TestNotNull(TEXT("实际球授予句柄"), Grant)) { return false; }
        auto* Ability = F.ASC()->FindAbilitySpecFromHandle(Grant->Ability)->GetPrimaryInstance();
        auto* Cost = FindFProperty<FClassProperty>(UGameplayAbility::StaticClass(), TEXT("CostGameplayEffectClass"));
        if (!TestNotNull(TEXT("真实GAS提交成本字段"), Cost) || !TestNotNull(TEXT("实例GA"), Ability)) { return false; }
        Cost->SetObjectPropertyValue_InContainer(Ability, UY01PlaygroundPassiveDamageEffect::StaticClass());
        const float CommittedDamage = F.ASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()) + 5;
        const auto Handle = F.ASC()->AbilityCommittedCallbacks.AddLambda([&](UGameplayAbility*)
        { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 111); });
        const float HP = F.HP(F.M); TestTrue(TEXT("真实GAS成本后成功提交"), F.CastSkill(1));
        F.ASC()->AbilityCommittedCallbacks.Remove(Handle);
        if (!TestEqual(TEXT("提交生成一个实例"), F.Attacks().Num(), 1)) { return false; }
        TestEqual(TEXT("快照在成功提交时，成本之后、提交广播之前"), Cast<AY01TestAttack>(F.Attacks()[0])->GetState().DamageSnapshot, CommittedDamage);
        F.Tick(1.2f); TestEqual(TEXT("命中使用提交瞬间快照"), F.HP(F.M), HP - CommittedDamage); return true;
    }
    if (Case == TEXT("ForgedEffect"))
    {
        const float HP = F.HP(F.M); TestTrue(TEXT("先生成合法球"), F.CastSkill(1));
        auto Context = F.ASC()->MakeEffectContext(); Context.AddSourceObject(F.Attacks()[0]);
        auto Spec = F.ASC()->MakeOutgoingSpec(UY01TestSkillDamageEffect::StaticClass(), 1, Context);
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, 9999);
        F.ASC()->ApplyGameplayEffectSpecToTarget(*Spec.Data, F.M->GetCombatASC());
        TestEqual(TEXT("即使伪造GE来源实例和伤害也不能绕过真实碰撞"), F.HP(F.M), HP); return true;
    }
    const float Damage = F.ASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute());
    const float Before = F.HP(F.M); const FVector Center = F.Origin();
    if (Case == TEXT("BaseRangePreserved"))
    {
        F.Place(F.M, FVector(700,0,0)); TestFalse(TEXT("旧基础选择仍保留500距离"), F.ASC()->SetCombatTarget(F.M));
        auto S = F.ASC()->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, F.ASC()->MakeEffectContext());
        S.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, 999); F.ASC()->ApplyGameplayEffectSpecToTarget(*S.Data, F.M->GetCombatASC());
        TestEqual(TEXT("旧基础GE不能远程越权伤害"), F.HP(F.M), Before); return true;
    }
    if (Case.StartsWith(TEXT("Area")))
    {
        FVector Offset(100,0,0); bool bHit = true;
        if (Case == TEXT("AreaEmpty")) { Offset = FVector(700,0,0); bHit = false; }
        if (Case == TEXT("AreaOutside")) { Offset.X = 250.01; bHit = false; }
        if (Case == TEXT("AreaBoundary")) { Offset.X = 250; }
        if (Case == TEXT("AreaHeightBoundary")) { Offset.Z = 120; }
        if (Case == TEXT("AreaHeightOutside")) { Offset.Z = 120.01; bHit = false; }
        if (Case == TEXT("AreaHeightBelow")) { Offset.Z = -120.01; bHit = false; }
        if (Case == TEXT("AreaFixedCenter") || Case == TEXT("AreaNewEntryOnce")) { Offset.X = 400; bHit = false; }
        F.Place(F.M, Offset); if (Case == TEXT("AreaIgnoresWall")) { F.Wall(FVector(50,0,0)); }
        if (Case == TEXT("AreaEmpty")) { F.ASC()->SetCombatTarget(nullptr); }
        TestTrue(TEXT("范围可以空放，成功Commit"), F.CastSkill(0));
        TestEqual(TEXT("产生且仅产生一个真实攻击实例"), F.Attacks().Num(), 1);
        TestEqual(TEXT("立即严格按Actor中心几何判定"), F.HP(F.M), bHit ? Before - Damage : Before);
        if (Case == TEXT("AreaFixedCenter")) { F.A->GetPawn()->SetActorLocation(Center + FVector(400,0,0)); }
        if (Case == TEXT("AreaNewEntryOnce")) { F.Place(F.M, FVector(100,0,0)); bHit = true; }
        if (Case == TEXT("AreaCooldown")) { TestFalse(TEXT("CD拒绝不产生假表现"), F.CastSkill(0)); TestEqual(TEXT("CD准确为1.5"), F.ASC()->GetSkillCooldownRemaining(0), 1.5f); }
        F.Tick(0.3f); TestEqual(TEXT("窗口内一次命中且不跟随施法者"), F.HP(F.M), bHit ? Before - Damage : Before);
        F.Tick(0.06f); TestEqual(TEXT("0.35秒后范围实例立即消失"), F.Attacks().Num(), 0);
        TestEqual(TEXT("整个窗口最多一次"), F.HP(F.M), bHit ? Before - Damage : Before); return true;
    }
    if (Case == TEXT("BallNoTarget") || Case == TEXT("BallRangeOutside"))
    {
        F.Place(F.M, FVector(800.01,0,0)); F.Place(F.N, FVector(0,900,0));
        TestFalse(TEXT("800外或无目标拒绝"), F.CastSkill(1)); TestEqual(TEXT("失败无表现"), F.Attacks().Num(), 0);
        TestEqual(TEXT("失败不进入CD"), F.ASC()->GetSkillCooldownRemaining(1), 0.f); return true;
    }
    if (Case == TEXT("BallRangeBoundary")) { F.Place(F.M, FVector(800,0,0)); F.Place(F.N, FVector(0,900,0)); }
    if (Case == TEXT("BallOverlap")) { F.Place(F.M, FVector(10,0,0)); }
    if (Case == TEXT("BallNearest")) { F.Place(F.N, FVector(200,0,0)); }
    if (Case == TEXT("BallSingleHit")) { F.Place(F.N, FVector(600,0,0)); }
    if (Case == TEXT("BallWall"))
    {
        F.Wall(FVector(200,0,0)); FHitResult Hit;
        TestTrue(TEXT("夹具存在真实阻挡墙"), F.World->LineTraceSingleByChannel(Hit, Center, Center + FVector(300,0,0), ECC_Visibility));
    }
    if (Case == TEXT("CrossRoom"))
    {
        F.Rooms()->Request(F.B, true); auto* Other = F.Rooms()->Monsters(F.PS(F.B))[0]; F.Freeze(Other); F.Place(Other, FVector(150,0,0));
        TestTrue(TEXT("同位置其他房间不能成为范围伤害目标"), F.CastSkill(0)); TestEqual(TEXT("B怪无伤"), F.HP(Other), 1000.f);
        TestTrue(TEXT("球允许穿过非法分区对象"), F.CastSkill(1)); F.Tick(1.2f);
        TestEqual(TEXT("A合法目标真正命中"), F.HP(F.M), Before - Damage); TestEqual(TEXT("跨房球不误伤"), F.HP(Other), 1000.f); return true;
    }
    if (Case == TEXT("RejectClient"))
    {
        F.PS(F.A)->SetRole(ROLE_SimulatedProxy); TestFalse(TEXT("客户端不能提交"), F.CastSkill(1));
        TestEqual(TEXT("拒绝客户端假实例"), F.Attacks().Num(), 0); F.PS(F.A)->SetRole(ROLE_Authority); return true;
    }
    if (Case == TEXT("RewardOnce")) { F.M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), Damage); }
    const int32 Slot = Case == TEXT("GrantIdentity") ? 0 : 1;
    if (!TestTrue(TEXT("服务器成功释放真实小球"), F.CastSkill(Slot))) { return false; }
    if (!TestEqual(TEXT("成功Commit产生复制攻击实例"), F.Attacks().Num(), 1)) { return false; }
    TWeakObjectPtr<AActor> Attack = F.Attacks()[0];
    TestEqual(TEXT("球CD取授予表2秒"), F.ASC()->GetSkillCooldownRemaining(Slot), 2.f);
    TestFalse(TEXT("球CD拒绝"), F.CastSkill(Slot));
    if (Case != TEXT("BallOverlap") && Case != TEXT("RewardOnce")) { TestEqual(TEXT("远目标释放时不得提前扣血"), F.HP(F.M), Before); }
    if (Case == TEXT("ParallelMovement"))
    {
        TestTrue(TEXT("范围与球独立并行"), F.CastSkill(0)); TestEqual(TEXT("并行两个实例"), F.Attacks().Num(), 2);
        F.A->GetPawn()->SetActorLocation(Center + FVector(0,300,0)); F.Tick(0.2f); TestEqual(TEXT("移动不取消"), F.Attacks().Num(), 2);
    }
    if (Case == TEXT("BallMovingTarget") || Case == TEXT("BallTimeout")) { F.Place(F.M, FVector(400,250,0)); }
    if (Case == TEXT("BallOriginalDead"))
    { F.M->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); F.Place(F.N, FVector(600,0,0)); }
    if (Case == TEXT("Snapshot")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 111); }
    if (Case == TEXT("TargetSession")) { F.M->GetCombatASC()->InvalidateCombatSession(); }
    if (Case == TEXT("SourcePermission")) { F.ASC()->TargetPermission = [](auto*, auto*) { return false; }; F.Mode->SetActorTickEnabled(false); }
    if (Case == TEXT("TargetPermission")) { F.M->GetCombatASC()->TargetPermission = [](auto*, auto*) { return false; }; F.Mode->SetActorTickEnabled(false); }
    if (Case.StartsWith(TEXT("Cancel")) || Case == TEXT("Reenter") || Case == TEXT("MainLogout"))
    {
        if (Case == TEXT("CancelFeedback")) { F.Tick(0.91f); TestEqual(TEXT("真实命中后仍在反馈期"), F.HP(F.M), Before - Damage); }
        if (Case == TEXT("CancelDeath")) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        else if (Case == TEXT("CancelEnded")) { F.Mode->TryStartNextWave(); F.Mode->TryEndMatch(false); }
        else if (Case == TEXT("CancelLogout") || Case == TEXT("MainLogout")) { F.Mode->Logout(F.A); }
        else { F.Rooms()->Request(F.A, false); }
        TestTrue(TEXT("失效同步清理攻击及命中反馈，不等待Tick"), !Attack.IsValid() || Attack->IsActorBeingDestroyed());
        TestTrue(TEXT("失效不退CD"), F.ASC()->GetSkillCooldownRemaining(Slot) > 0);
        if (Case == TEXT("Reenter")) { F.Rooms()->Request(F.A, true); F.Tick(1.2f); TestEqual(TEXT("旧攻击不能伤害重入会话"), F.PS(F.A)->GetGold(), 0); }
        TestEqual(TEXT("清理不产生训练奖励"), F.PS(F.A)->GetGold(), 0); return true;
    }
    if (Case == TEXT("ReplicationLifecycle"))
    {
        TestTrue(TEXT("实例复制且不是OwnerOnly"), Attack->GetIsReplicated() && !Attack->bOnlyRelevantToOwner);
        Attack->GetClass()->SetUpRuntimeReplicationData(); TArray<FLifetimeProperty> Props; Attack->GetLifetimeReplicatedProps(Props);
        auto* P = FindFProperty<FStructProperty>(Attack->GetClass(), TEXT("State"));
        TestTrue(TEXT("权威完整状态向相关客户端复制"), P && Props.ContainsByPredicate([&](const auto& R) { return R.RepIndex == P->RepIndex && R.Condition == COND_None; }));
    }
    if (Case == TEXT("BallSweep")) { F.Tick(0.01f); TestEqual(TEXT("一帧越过目标仍扫掠命中"), F.HP(F.M), Before - Damage); return true; }
    if (Case == TEXT("BallOverlap")) { TestEqual(TEXT("生成即真实重叠可立即伤害"), F.HP(F.M), Before - Damage); F.Tick(0.11f); TestEqual(TEXT("重叠仅一次且反馈结束"), F.Attacks().Num(), 0); return true; }
    if (Case == TEXT("BallFeedback"))
    {
        F.Tick(0.91f); TestEqual(TEXT("真正碰撞伤害"), F.HP(F.M), Before - Damage);
        if (!TestTrue(TEXT("命中后保留0.1反馈"), Attack.IsValid() && !Attack->IsActorBeingDestroyed())) { return false; }
        const FVector Stop = Attack->GetActorLocation(); F.Tick(0.03f); TestTrue(TEXT("反馈小球停住"), Attack->GetActorLocation().Equals(Stop));
        F.Tick(0.1f); TestEqual(TEXT("反馈结束消失"), F.Attacks().Num(), 0); TestEqual(TEXT("反馈不重复判伤"), F.HP(F.M), Before - Damage); return true;
    }
    F.Tick(Case == TEXT("BallRangeBoundary") ? 2.f : 1.7f);
    if (Case == TEXT("BallOriginalDead")) { TestEqual(TEXT("原目标死仍直线命中其他合法怪"), F.HP(F.N), Before - Damage); }
    else if (Case == TEXT("BallNearest")) { TestEqual(TEXT("最近目标先中"), F.HP(F.N), Before - Damage); TestEqual(TEXT("后方不穿透"), F.HP(F.M), Before); }
    else if (Case == TEXT("RewardOnce")) { TestEqual(TEXT("GAS来源结算一次金币"), F.PS(F.A)->GetGold(), 10); TestEqual(TEXT("GAS来源结算一次击杀"), F.Rooms()->Observe(F.PS(F.A)).Kills, 1); }
    else
    {
        const bool Miss = Case == TEXT("BallMovingTarget") || Case == TEXT("BallTimeout") || Case == TEXT("BallWall") || Case == TEXT("TargetSession") || Case == TEXT("SourcePermission") || Case == TEXT("TargetPermission");
        TestEqual(TEXT("只在权威几何及双向许可有效时命中"), F.HP(F.M), Miss ? Before : Before - Damage * (Case == TEXT("Coefficient") ? 2 : 1));
        TestEqual(TEXT("其他目标未被多重伤害"), F.HP(F.N), Before);
    }
    F.Tick(1); TestEqual(TEXT("寿命结束不存在攻击残留"), F.Attacks().Num(), 0); return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01R36LearnAttackTest, "Y01.Playground.R36.LearnedAttacks",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01R36LearnAttackTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Mode : {TEXT("Area"), TEXT("Projectile")})
    {
        for (const TCHAR* Action : {TEXT("RemoveSnapshot"), TEXT("ReplaceSnapshot"), TEXT("Death"), TEXT("Exit"), TEXT("Ended"),
            TEXT("CommitReentry"), TEXT("ReplacementRejected"), TEXT("RemoveDuringCommit")})
        { const FString N = FString(Mode) + TEXT(".") + Action; Names.Add(N); Commands.Add(N); }
    }
}
bool FY01R36LearnAttackTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter); FSkillWorld F; FY01MatchFixture Books;
    using K = EY01TableKind; using O = EY01ItemOperation;
    const bool Area = Case.StartsWith(TEXT("Area.")); const int32 Template = Area ? 0 : 1;
    auto* Character = F.Profile->Tables[K::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("R36Learn"));
    Character->InitialSkills.Reset(); Character->InitialEquipment.Reset(); Character->AttackDamage = 35;
    Character->R33TestGrowth.bEnabled = false; F.Profile->bR34AutoAttackEnabled = false;
    FY01SourceEffectRow Effect; Effect.Magnitude = 5; Books.Add(K::SourceEffect, Effect, TEXT("Five"));
    auto Skill = *F.Row(Template); Skill.EffectIds = {SkillRef(K::SourceEffect, TEXT("Five"))}; Skill.CooldownSeconds = 8;
    Books.Add(K::Skill, Skill, TEXT("BookA"));
    FY01SkillRow Passive; Passive.Type = EY01SkillType::Passive;
    FY01GrowthRuleRow Growth; Growth.Target = EY01GrowthTarget::Player; Growth.Attribute = EY01GrowthAttribute::Strength; Growth.Delta = 3;
    Books.Add(K::GrowthRule, Growth, TEXT("BookGrowth")); Passive.GrowthRuleIds = {SkillRef(K::GrowthRule, TEXT("BookGrowth"))};
    Books.Add(K::Skill, Passive, TEXT("BookB"));
    F.Start(); if (!TestTrue(TEXT("真实个人房怪物和Mode"), F.Mode->IsMatchConfigured() && F.M && F.N)) { return false; }
    TestTrue(TEXT("个人房同时Preparing"), F.Mode->TryStartNextWave());
    auto* L = F.PS(F.A)->GetLoadout(); const FGuid Match = FGuid::NewGuid();
    auto Ok = [&](const TCHAR* M, const FY01ItemResult& R) { if (!R.IsOk()) { AddInfo(R.Message); } return TestTrue(M, R.IsOk()); };
    if (!Ok(TEXT("真实空技能注册表"), L->Initialize(Match, 1))) { return false; }
    FY01ItemGrant A, B; A.ItemId = FGuid::NewGuid(); A.Definition = SkillRef(K::Skill, TEXT("BookA"));
    B.ItemId = FGuid::NewGuid(); B.Definition = SkillRef(K::Skill, TEXT("BookB"));
    if (!Ok(TEXT("真实模板书入包"), L->AddItems(FY01DataCatalog(Books.Tables), {A, B}))) { return false; }
    auto View = [&] { FY01LoadoutSnapshot V; L->Observe(V); return V; };
    auto Request = [&](FGuid Item, O Op)
    {
        const auto V = View(); FY01ItemRequest R; R.MatchId = Match; R.JoinSequence = 1; R.Seq = V.ExpectedSeq;
        R.Operation = Op; R.ItemId = Item; R.ExpectedEntityRevision = Item.IsValid() ? 1 : 0; R.Slot = 3;
        R.ExpectedLearnedId = V.Skills[3].LearnedId; R.ExpectedLearnedRevision = V.Skills[3].EntityRevision;
        R.ConfigRevision = V.ConfigRevision; return R;
    };
    if (!Ok(TEXT("真实模板学习到第四槽"), L->PrepareLearn(F.A, Request(A.ItemId, O::PrepareLearn)))) { return false; }
    const auto Old = View().Skills[3]; TestEqual(TEXT("发动前攻击40"), F.ASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), 40.f);
    const float BeforeHP = F.HP(F.M); const uint64 Session = F.ASC()->GetCombatSession();
    auto Replace = Request(B.ItemId, O::PrepareLearn); auto Remove = Request({}, O::RemoveSkill); int32 Calls = 0;
    FDelegateHandle Callback;
    if (Case.EndsWith(TEXT("CommitReentry")) || Case.EndsWith(TEXT("RemoveDuringCommit")))
    {
        Callback = F.ASC()->AbilityCommittedCallbacks.AddLambda([&](UGameplayAbility*)
        {
            ++Calls;
            const auto R = Case.EndsWith(TEXT("RemoveDuringCommit")) ? L->RemoveSkill(F.A, Remove) : L->PrepareLearn(F.A, Replace);
            TestEqual(TEXT("真实模板Commit广播中的主动修改Busy"), R.Code, EY01ItemError::Busy);
        });
    }
    if (!TestTrue(TEXT("真实模板GA发动而不是mock"), F.CastSkill(3))) { F.ASC()->AbilityCommittedCallbacks.Remove(Callback); return false; }
    F.ASC()->AbilityCommittedCallbacks.Remove(Callback);
    const auto Attacks = F.Attacks(); if (!TestEqual(TEXT("生成真实攻击Actor"), Attacks.Num(), 1)) { return false; }
    TWeakObjectPtr<AY01TestAttack> Attack = Cast<AY01TestAttack>(Attacks[0]);
    TestEqual(TEXT("原攻击快照40"), Attack->GetState().DamageSnapshot, 40.f);
    TestEqual(TEXT("原模板模式"), Attack->GetState().Config.Mode, Area ? EY01TestSkillMode::Area : EY01TestSkillMode::Projectile);
    if (Case.EndsWith(TEXT("CommitReentry")) || Case.EndsWith(TEXT("RemoveDuringCommit")))
    { TestEqual(TEXT("Commit确实广播"), Calls, 1); TestEqual(TEXT("仍为原技能"), View().Skills[3].LearnedId, Old.LearnedId); }
    if (Case.EndsWith(TEXT("Death")) || Case.EndsWith(TEXT("Exit")) || Case.EndsWith(TEXT("Ended")))
    {
        if (Case.EndsWith(TEXT("Death"))) { F.ASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
        if (Case.EndsWith(TEXT("Exit"))) { TestTrue(TEXT("真实离区"), F.Rooms()->Request(F.A, false)); }
        if (Case.EndsWith(TEXT("Ended"))) { TestTrue(TEXT("真实结束"), F.Mode->TryEndMatch(false)); }
        TestTrue(TEXT("死亡离区Ended同步清原攻击"), !Attack.IsValid() || Attack->IsActorBeingDestroyed());
        TestEqual(TEXT("清理不返CD"), View().Skills[3].Deadline, Old.Deadline + 8);
        return true;
    }
    if (Case.EndsWith(TEXT("ReplaceSnapshot")) || Case.EndsWith(TEXT("ReplacementRejected")))
    {
        const auto Prepared = L->PrepareLearn(F.A, Replace); if (!Ok(TEXT("准备覆盖不杀原攻击"), Prepared)) { return false; }
        Replace.Operation = O::ConfirmLearn; Replace.Seq = View().ExpectedSeq; Replace.TicketId = Prepared.TicketId;
        FDelegateHandle Reject;
        if (Case.EndsWith(TEXT("ReplacementRejected")))
        {
            Reject = F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).AddLambda([&](const FOnAttributeChangeData&)
            { if (++Calls == 1) { TestEqual(TEXT("覆盖可逆阶段真实重入"), L->RemoveSkill(F.A, Remove).Code, EY01ItemError::Busy); } });
        }
        const auto R = L->ConfirmLearn(F.A, Replace);
        F.ASC()->GetGameplayAttributeValueChangeDelegate(UY01AttributeSet::GetAttackDamageAttribute()).Remove(Reject);
        if (Case.EndsWith(TEXT("ReplacementRejected")))
        {
            TestFalse(TEXT("覆盖失败"), R.IsOk()); TestEqual(TEXT("保留原GA"), View().Skills[3].GAHandle, Old.GAHandle);
            TestEqual(TEXT("保留原CD"), View().Skills[3].Deadline, 8.); TestEqual(TEXT("未耗B"), View().Items.Num(), 1);
        }
        else { Ok(TEXT("覆盖成功"), R); TestEqual(TEXT("被动保留成长快照"), View().Skills[3].Growth.Num(), 1); }
    }
    else if (Case.EndsWith(TEXT("RemoveSnapshot"))) { Ok(TEXT("已发动后移除"), L->RemoveSkill(F.A, Remove)); }
    const bool Retired = Case.EndsWith(TEXT("RemoveSnapshot")) || Case.EndsWith(TEXT("ReplaceSnapshot"));
    TestEqual(TEXT("技能修改不使攻击会话失效"), F.ASC()->GetCombatSession(), Session);
    TestTrue(TEXT("已发攻击仍存活"), Attack.IsValid() && !Attack->IsActorBeingDestroyed());
    TestEqual(TEXT("当前攻击与原快照分离"), F.ASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), Retired ? 35.f : 40.f);
    if (Retired)
    {
        TestNull(TEXT("旧handle已撤销"), F.ASC()->FindAbilitySpecFromHandle(Old.GAHandle));
        AddExpectedMessagePlain(TEXT("TryActivateAbility called with invalid Handle"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
        TestFalse(TEXT("旧handle不能再激活"), F.ASC()->TryActivateAbility(Old.GAHandle, false));
    }
    if (Area) { F.Place(F.M, FVector(100, 0, 0)); F.Tick(.02f); }
    else { F.Tick(1.1f); }
    TestEqual(TEXT("原范围或飞行物真实命中仍40"), F.HP(F.M), BeforeHP - 40);
    F.Tick(2.6f); TestEqual(TEXT("攻击正常寿命清理"), F.Attacks().Num(), 0); return true;
}
#endif
