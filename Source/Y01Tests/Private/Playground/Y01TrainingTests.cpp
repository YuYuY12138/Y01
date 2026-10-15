#include "Misc/AutomationTest.h"
#include "Playground/Y01PlaygroundController.h"
#include "Playground/Y01PlaygroundSeed.h"
#include "Playground/Y01PlaygroundHUD.h"
#include "Playground/Y01TestSkill.h"
#include "Net/UnrealNetwork.h"
#include "UObject/UnrealType.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/Canvas.h"
#include "CanvasTypes.h"
#include "Engine/GameInstance.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Engine/StaticMeshActor.h"
#include "Components/StaticMeshComponent.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace
{
FY01DataRef TrainingRef(EY01TableKind Type, FName Name = TEXT("Default"))
{ FY01DataRef Ref; Ref.Type = Type; Ref.RowName = Name; return Ref; }
struct FTrainingWorld
{
    TStrongObjectPtr<UY01PlaygroundProfile> Profile{Y01PlaygroundEditor::CreateSeed(GetTransientPackage())};
    UWorld* World;
    AY01PlaygroundGameMode* Mode;
    AY01PlaygroundController* A;
    AY01PlaygroundController* B;
    FTrainingWorld()
    {
        Profile->bTrainingEnabled = true;
        auto* Table = Profile->Tables[EY01TableKind::TrainingStage].Get(); Table->EmptyTable();
        FY01TrainingStageRow Row; Row.SpawnInterval = 3; Row.MaxAlive = 6;
        FY01SpawnEntry Entry; Entry.Monster = TrainingRef(EY01TableKind::Monster); Entry.Count = 2;
        Row.Spawns = {Entry}; Row.Reward = TrainingRef(EY01TableKind::RewardPackage); Table->AddRow(TEXT("Default"), Row);
        Row.Stage = 2; Row.StartSeconds = 9; Row.Spawns[0].Monster.RowName = TEXT("Boss"); Row.Spawns[0].Count = 1;
        Table->AddRow(TEXT("Later"), Row);
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
        World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01PlaygroundGameMode::StaticClass();
        World->SetGameMode(FURL()); Mode = World->GetAuthGameMode<AY01PlaygroundGameMode>();
        auto* Config = World->SpawnActor<AY01PlaygroundConfig>(); Config->Profile = Profile.Get();
        A = AddPlayer(); B = AddPlayer();
        auto* Floor = World->SpawnActor<AStaticMeshActor>(FVector(0, 0, -50), FRotator::ZeroRotator);
        Floor->GetStaticMeshComponent()->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
        Floor->SetActorScale3D(FVector(30, 30, 1)); Floor->GetStaticMeshComponent()->SetCollisionProfileName(TEXT("BlockAll"));
    }
    AY01PlaygroundController* AddPlayer()
    {
        auto* PC = World->SpawnActor<AY01PlaygroundController>();
        auto* PS = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01PlaygroundCharacter>(FVector(0, 0, 90), FRotator::ZeroRotator);
        PC->PlayerState = PS; PC->Possess(Pawn); PS->GetCombatASC()->InitAbilityActorInfo(PS, Pawn);
        Mode->GetFlowState()->AddPlayerState(PS); return PC;
    }
    void Start() { Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay(); }
    ~FTrainingWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    AY01PlayerState* PS(AY01PlaygroundController* PC) const { return PC->GetPlayerState<AY01PlayerState>(); }
    UY01TrainingRooms* Rooms() const { return Mode->GetTraining(); }
    FY01TrainingStageRow* Row() { return Profile->Tables[EY01TableKind::TrainingStage]->FindRow<FY01TrainingStageRow>(TEXT("Default"), TEXT("TrainingTest")); }
    void Tick(float Seconds)
    { for (int32 I = 0; I < FMath::CeilToInt(Seconds / 0.05f); ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, 0.05f); } }
    void Damage(AY01PlaygroundController* PC, AY01Monster* Monster, float Amount)
    {
        auto* ASC = PS(PC)->GetCombatASC();
        auto Spec = ASC->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, ASC->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, Amount);
        ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data, Monster->GetCombatASC());
    }
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01TrainingTest, "Y01.Playground.Training",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
void FY01TrainingTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("EnterAndDuplicate"), TEXT("IndependentCycles"), TEXT("CapSkipsWithoutDebt"),
        TEXT("KillExactlyOnce"), TEXT("CleanupNoReward"), TEXT("DirectZeroNoReward"), TEXT("ExitStopsAndReenterResets"),
        TEXT("PreserveResourcesAndCooldown"), TEXT("CrossRoomFinalDamageRejected"), TEXT("MainlineIsolation"),
        TEXT("StageChangesOnlyNextBatch"), TEXT("DelayedSpawnCancelled"), TEXT("LogoutCleanup"), TEXT("WaitingDeath"),
        TEXT("BattleDeathEndsAll"), TEXT("EndedCleanup"), TEXT("EndPlayCleanup"), TEXT("DisabledCompatibility"),
        TEXT("InvalidIdentity"), TEXT("BadIntervalZero"), TEXT("BadIntervalNegative"), TEXT("BadIntervalNaN"),
        TEXT("BadIntervalInfinity"), TEXT("BadCapZero"), TEXT("BadCapNegative"), TEXT("BadCapHuge"),
        TEXT("UnsupportedRewardItems"), TEXT("WorldFloorAndWalls"), TEXT("CanvasButtons"), TEXT("TrainingOwnerOnly"),
        TEXT("CanvasSkillDefault"), TEXT("CanvasSkillToggle"), TEXT("CanvasSkillLayout"), TEXT("CanvasSkillDrawBounds"), TEXT("CanvasSkillScaled") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01TrainingTest::RunTest(const FString& Case)
{
    TGuardValue<uint64> FrameGuard(GFrameCounter, GFrameCounter);
    FTrainingWorld F;
    if (Case.StartsWith(TEXT("Bad")) || Case == TEXT("UnsupportedRewardItems"))
    {
        if (Case == TEXT("BadIntervalZero")) { F.Row()->SpawnInterval = 0; }
        if (Case == TEXT("BadIntervalNegative")) { F.Row()->SpawnInterval = -1; }
        if (Case == TEXT("BadIntervalNaN")) { F.Row()->SpawnInterval = std::numeric_limits<float>::quiet_NaN(); }
        if (Case == TEXT("BadIntervalInfinity")) { F.Row()->SpawnInterval = std::numeric_limits<float>::infinity(); }
        if (Case == TEXT("BadCapZero")) { F.Row()->MaxAlive = 0; }
        if (Case == TEXT("BadCapNegative")) { F.Row()->MaxAlive = -1; }
        if (Case == TEXT("BadCapHuge")) { F.Row()->MaxAlive = MAX_int32; }
        if (Case == TEXT("UnsupportedRewardItems"))
        {
            auto* Reward = F.Profile->Tables[EY01TableKind::RewardPackage]->FindRow<FY01RewardPackageRow>(TEXT("Default"), TEXT("TrainingTest"));
            FY01RewardEntry Item; Item.Item = TrainingRef(EY01TableKind::Equipment); Reward->Items.Add(Item);
        }
        TestFalse(TEXT("配置拒绝非法训练值，不静默丢弃物品"), F.Profile->Validate().IsValid()); return true;
    }
    // 长周期调度测试隔离真实怪物伤害，避免先死亡使调度断言失去前提。
    if (Case == TEXT("CapSkipsWithoutDebt") || Case == TEXT("StageChangesOnlyNextBatch"))
    { F.Profile->Tables[EY01TableKind::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("TrainingTest"))->Health = 20000; }
    if (Case == TEXT("DisabledCompatibility")) { F.Profile->bTrainingEnabled = false; }
    if (Case == TEXT("DelayedSpawnCancelled")) { F.Row()->Spawns[0].DelaySeconds = 2; }
    if (Case.StartsWith(TEXT("CanvasSkill")))
    {
        for (const TCHAR* Name : {TEXT("BasicAttack"), TEXT("HeavyAttack")})
        {
            auto* Row = F.Profile->Tables[EY01TableKind::Skill]->FindRow<FY01SkillRow>(Name, TEXT("CanvasSkill"));
            const bool Area = FString(Name) == TEXT("BasicAttack");
            Row->Ability = UY01TestSkillAbility::StaticClass();
            auto& C = Row->TestSkill; C.Mode = Area ? EY01TestSkillMode::Area : EY01TestSkillMode::Projectile;
            C.Radius = Area ? 250 : 12; C.HalfHeight = Area ? 120 : 0; C.ActiveSeconds = Area ? 0.35f : 0;
            C.TargetRange = Area ? 0 : 800; C.Speed = Area ? 0 : 400; C.Lifetime = Area ? 0 : 2.5f; C.HitFeedbackSeconds = Area ? 0 : 0.1f;
        }
    }
    F.Start();
    if (!TestTrue(TEXT("合法训练配置正常初始化"), F.Mode->IsMatchConfigured())) { return false; }
    auto* PS = F.PS(F.A); auto* ASC = PS->GetCombatASC(); APawn* Pawn = F.A->GetPawn();
    auto* Rooms = F.Rooms();
    if (!TestNotNull(TEXT("独立训练组件"), Rooms)) { return false; }
    if (Case == TEXT("DisabledCompatibility"))
    { TestFalse(TEXT("旧配置保持禁用"), Rooms->Request(F.A, true)); return true; }
    if (Case == TEXT("InvalidIdentity"))
    {
        TestFalse(TEXT("空身份拒绝"), Rooms->Request(nullptr, true));
        auto* Stranger = F.World->SpawnActor<AY01PlaygroundController>(); Stranger->PlayerState = PS;
        TestFalse(TEXT("借用他人PlayerState不能入房"), Rooms->Request(Stranger, true));
        TestTrue(TEXT("真实拥有者仍能进入"), Rooms->Request(F.A, true)); return true;
    }
    if (Case == TEXT("CanvasButtons"))
    {
        auto* HUD = F.World->SpawnActor<AY01PlaygroundHUD>(); HUD->PlayerOwner = F.A;
        HUD->NotifyHitBoxClick(TEXT("TrainingEnter"));
        TestTrue(TEXT("Canvas点击进入同一RPC入口"), Rooms->Observe(PS).bInside);
        HUD->NotifyHitBoxClick(TEXT("TrainingReturn")); TestFalse(TEXT("Canvas点击返回"), Rooms->Observe(PS).bInside);
        return true;
    }
    if (Case.StartsWith(TEXT("CanvasSkill")))
    {
        Rooms->Request(F.A, true); F.A->CaptureObserver(F.Mode);
        auto* HUD = F.World->SpawnActor<AY01PlaygroundHUD>(); HUD->PlayerOwner = F.A;
        struct FDraw { FBox2D Bounds; FString Text; };
        TArray<FDraw> Draws;
        HUD->ObserveDrawForTest = [&](const FBox2D& Bounds, const FString& Text) { Draws.Add({Bounds, Text}); };
        const float Scale = Case == TEXT("CanvasSkillScaled") ? 0.75f : 1.f;
        auto Render = [&]
        {
            Draws.Reset();
            FCanvas Batch(nullptr, nullptr, F.World, ERHIFeatureLevel::SM5);
            TStrongObjectPtr<UCanvas> Canvas(NewObject<UCanvas>());
            Canvas->Init(1280 * Scale, 800 * Scale, nullptr, &Batch); Canvas->Update();
            HUD->SetCanvas(Canvas.Get(), nullptr); HUD->DrawHUD(); HUD->SetCanvas(nullptr, nullptr);
        };
        auto HasText = [&](const TCHAR* Value) { return Draws.ContainsByPredicate([&](const FDraw& D) { return D.Text.Contains(Value); }); };
        auto HasPanel = [&](FVector2D Min, FVector2D Max)
        { return Draws.ContainsByPredicate([&](const FDraw& D) { return D.Text.IsEmpty() && D.Bounds.Min.Equals(Min * Scale) && D.Bounds.Max.Equals(Max * Scale); }); };
        Render();
        TestTrue(TEXT("测试采集实际Canvas绘制而不是空布局"), Draws.Num() > 10);
        if (Case == TEXT("CanvasSkillDefault"))
        {
            TestFalse(TEXT("详细说明默认不绘制"), HasText(TEXT("固定释放点")) || HasText(TEXT("无正式复活")));
            TestFalse(TEXT("默认不绘制说明背景"), HasPanel(FVector2D(458,350), FVector2D(1264,545)) || HasPanel(FVector2D(920,208), FVector2D(1264,558)));
            for (const TCHAR* Summary : {TEXT("测试范围"), TEXT("测试球")})
            { TestTrue(TEXT("默认底部仍有两槽简短形态"), Draws.ContainsByPredicate([&](const FDraw& D)
                { return D.Text == Summary && D.Bounds.Min.Y == 721 * Scale; })); }
            TestTrue(TEXT("自动普攻未接提示保留"), HasText(TEXT("自动普攻未接"))); return true;
        }
        if (Case == TEXT("CanvasSkillToggle"))
        {
            const auto Before = Rooms->Observe(PS);
            const auto* Box = HUD->GetHitBoxAtCoordinates(FVector2D(1092,183) * Scale, true);
            if (!TestNotNull(TEXT("右侧按钮必须真实注册hitbox"), Box)) { return false; }
            TestEqual(TEXT("点击命中说明按钮"), Box->GetName(), FName(TEXT("SkillDetails")));
            HUD->NotifyHitBoxClick(Box->GetName()); Render();
            TestTrue(TEXT("一次点击展开说明"), HasText(TEXT("固定释放点")) && HasText(TEXT("无正式复活")));
            HUD->NotifyHitBoxClick(TEXT("Unrelated")); Render(); TestTrue(TEXT("无关hitbox不折叠"), HasText(TEXT("固定释放点")));
            HUD->NotifyHitBoxClick(TEXT("SkillDetails")); Render(); TestFalse(TEXT("再次点击收起说明"), HasText(TEXT("固定释放点")));
            TestEqual(TEXT("说明点击不调用训练切区"), Rooms->Observe(PS).Room, Before.Room);
            TestTrue(TEXT("说明点击不退出训练"), Rooms->Observe(PS).bInside); return true;
        }
        HUD->NotifyHitBoxClick(TEXT("SkillDetails")); Render();
        if (Case == TEXT("CanvasSkillLayout") || Case == TEXT("CanvasSkillScaled"))
        {
            const auto* Box = HUD->GetHitBoxAtCoordinates(FVector2D(1092,183) * Scale, true);
            TestTrue(TEXT("右栏按钮绘制与命中同坐标"), Box && Box->GetName() == TEXT("SkillDetails"));
            TestTrue(TEXT("说明背景在目标面板下方且宽344"), HasPanel(FVector2D(920,208), FVector2D(1264,558)));
            const auto* Training = HUD->GetHitBoxAtCoordinates(FVector2D(150,446) * Scale, true);
            TestTrue(TEXT("训练按钮不移动"), Training && Training->GetName() == TEXT("TrainingReturn"));
            TestTrue(TEXT("底部技能槽背景不移动"), HasPanel(FVector2D(458,646), FVector2D(1264,784)));
            if (IsValid(F.A->GetObserver().Target))
            { TestTrue(TEXT("右上目标面板不移动"), HasPanel(FVector2D(920,16), FVector2D(1264,156))); }
        }
        const FBox2D Center(FVector2D(460,250) * Scale, FVector2D(900,550) * Scale);
        int32 DetailLines = 0; FString DetailText;
        for (const FDraw& D : Draws)
        {
            TestFalse(TEXT("实际背景和文字均不进入中央观察区：") + D.Text, D.Bounds.Intersect(Center));
            if (!D.Text.IsEmpty() && D.Bounds.Min.Y >= 208 * Scale && D.Bounds.Min.Y < 558 * Scale && D.Bounds.Min.X >= 900 * Scale)
            {
                ++DetailLines; DetailText += D.Text;
                TestTrue(TEXT("Canvas返回真实非零文字尺寸"), D.Bounds.GetSize().X > 2 && D.Bounds.GetSize().Y > 2);
                TestTrue(TEXT("折行文字含阴影完整位于右侧说明背景内"), D.Bounds.Min.X >= 932 * Scale && D.Bounds.Max.X <= 1254 * Scale
                    && D.Bounds.Max.Y <= 558 * Scale);
            }
        }
        TestTrue(TEXT("详细说明超过原始五行，确实折行而非隐藏说明"), DetailLines > 5);
        for (const auto& Slot : F.A->GetObserver().Slots)
        {
            FString Expected = Slot.Description.Replace(TEXT("\n"), TEXT(""));
            TestTrue(TEXT("右侧保留完整服务器技能说明"), DetailText.Contains(Expected));
        }
        TestTrue(TEXT("右侧保留清理及无正式复活说明"), DetailText.Contains(TEXT("无正式复活")));
        return true;
    }
    if (Case == TEXT("TrainingOwnerOnly"))
    {
        Rooms->Request(F.A, true); F.A->CaptureObserver(F.Mode); F.B->CaptureObserver(F.Mode);
        TestTrue(TEXT("本人观测训练区"), F.A->GetObserver().Training.bInside);
        TestFalse(TEXT("B观测不是A的房间"), F.B->GetObserver().Training.bInside);
        F.A->GetClass()->SetUpRuntimeReplicationData();
        const auto* Prop = FindFProperty<FStructProperty>(F.A->GetClass(), TEXT("Observer"));
        TArray<FLifetimeProperty> Props; F.A->GetLifetimeReplicatedProps(Props);
        TestTrue(TEXT("包含训练状态的观测仍仅OwnerOnly"), Prop && Props.ContainsByPredicate([&](const auto& P)
            { return P.RepIndex == Prop->RepIndex && P.Condition == COND_OwnerOnly; })); return true;
    }
    const FVector Origin = Pawn->GetActorLocation();
    if (!TestTrue(TEXT("A进入立即成功"), Rooms->Request(F.A, true))) { return false; }
    if (Case == TEXT("DelayedSpawnCancelled"))
    {
        TestEqual(TEXT("延迟前无怪"), Rooms->Observe(PS).Alive, 0); Rooms->Request(F.A, false); F.Tick(6);
        TestEqual(TEXT("离开清除待刷事件"), Rooms->Monsters(PS).Num(), 0); return true;
    }
    if (!TestEqual(TEXT("进入首批两只真实怪"), Rooms->Monsters(PS).Num(), 2)) { return false; }
    auto* Monster = Rooms->Monsters(PS)[0];
    if (Case == TEXT("EnterAndDuplicate"))
    {
        const auto View = Rooms->Observe(PS); TestTrue(TEXT("处于个人房"), View.bInside);
        TestTrue(TEXT("重复进入幂等确认"), Rooms->Request(F.A, true));
        TestEqual(TEXT("会话不重置"), Rooms->Observe(PS).Session, View.Session);
        TestEqual(TEXT("不额外刷怪"), Rooms->Observe(PS).Alive, 2);
        TestFalse(TEXT("B未进入"), Rooms->Observe(F.PS(F.B)).bInside);
        TestTrue(TEXT("同一Pawn"), Pawn == F.A->GetPawn()); TestTrue(TEXT("同一ASC"), ASC == PS->GetCombatASC());
        TestTrue(TEXT("独立空间非原点"), FVector::Dist(Origin, Pawn->GetActorLocation()) > 3000); return true;
    }
    if (Case == TEXT("IndependentCycles"))
    {
        F.Tick(1); TestTrue(TEXT("B独立进入"), Rooms->Request(F.B, true)); F.Tick(2.15f);
        TestEqual(TEXT("A到第二批"), Rooms->Observe(PS).Alive, 4);
        TestEqual(TEXT("B还未第二批"), Rooms->Observe(F.PS(F.B)).Alive, 2);
        TestTrue(TEXT("房间不相同"), Rooms->Observe(PS).Room != Rooms->Observe(F.PS(F.B)).Room);
        Rooms->Request(F.A, false); F.Tick(1);
        TestEqual(TEXT("B仍然刷怪"), Rooms->Observe(F.PS(F.B)).Alive, 4); return true;
    }
    if (Case == TEXT("CapSkipsWithoutDebt"))
    {
        F.Tick(12.1f); TestEqual(TEXT("始终不超过6"), Rooms->Observe(PS).Alive, 6);
        Monster->Destroy(); F.Tick(0.1f); TestEqual(TEXT("满额跳过不补欠批"), Rooms->Observe(PS).Alive, 5);
        F.Tick(3); TestEqual(TEXT("下一周期补到上限"), Rooms->Observe(PS).Alive, 6); return true;
    }
    if (Case == TEXT("KillExactlyOnce") || Case == TEXT("CrossRoomFinalDamageRejected") || Case == TEXT("PreserveResourcesAndCooldown"))
    {
        Monster->SetActorLocation(Pawn->GetActorLocation() + FVector(100, 0, 0));
        if (Case == TEXT("CrossRoomFinalDamageRejected"))
        {
            Rooms->Request(F.B, true); auto* Other = Rooms->Monsters(F.PS(F.B))[0];
            Other->SetActorLocation(Monster->GetActorLocation()); const float HP = Y01Playground::Health(Other->GetCombatASC());
            TestFalse(TEXT("距离内跨房仍禁止选中"), ASC->SetCombatTarget(Other)); F.Damage(F.A, Other, 9999);
            TestEqual(TEXT("最终GE仍拒绝跨房"), Y01Playground::Health(Other->GetCombatASC()), HP);
            TestEqual(TEXT("跨房无奖励"), PS->GetGold(), 0); return true;
        }
        if (Case == TEXT("PreserveResourcesAndCooldown"))
        {
            PS->TryAddMatchReward(17, 11); ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 123);
            TestTrue(TEXT("真实选目标"), ASC->SetCombatTarget(Monster)); TestTrue(TEXT("真实技能产生CD"), PS->TryActivateSkillSlot(1));
            const float CD = ASC->GetSkillCooldownRemaining(1); const float Attack = ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute());
            const auto Effects = ASC->GetActiveEffects(FGameplayEffectQuery());
            Rooms->Request(F.A, false); TestNull(TEXT("切区清目标"), ASC->GetCombatTarget()); F.Tick(0.25f); Rooms->Request(F.A, true);
            TestTrue(TEXT("ASC/Pawn身份保持"), ASC == PS->GetCombatASC() && Pawn == F.A->GetPawn());
            TestEqual(TEXT("不回血"), Y01Playground::Health(ASC), 123.f); TestEqual(TEXT("金币保持"), PS->GetGold(), 17);
            TestEqual(TEXT("经验保持"), PS->GetExperience(), 11);
            TestTrue(TEXT("CD世界时钟继续且不刷新"), ASC->GetSkillCooldownRemaining(1) > 0 && ASC->GetSkillCooldownRemaining(1) < CD);
            TestEqual(TEXT("Buff属性保持"), ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), Attack);
            TestTrue(TEXT("Buff实例保持"), Effects == ASC->GetActiveEffects(FGameplayEffectQuery())); return true;
        }
        F.Damage(F.A, Monster, 9999); F.Damage(F.A, Monster, 9999); F.Tick(0.1f);
        TestEqual(TEXT("有效击杀一次"), Rooms->Observe(PS).Kills, 1); TestEqual(TEXT("训练行金币一次"), PS->GetGold(), 10);
        TestEqual(TEXT("训练行经验一次"), PS->GetExperience(), 5); TestEqual(TEXT("B不分奖励"), F.PS(F.B)->GetGold(), 0); return true;
    }
    if (Case == TEXT("StageChangesOnlyNextBatch"))
    {
        F.Tick(9.1f); TestEqual(TEXT("进入经过时间决定阶段"), Rooms->Observe(PS).Stage, 2);
        TestTrue(TEXT("旧怪保留"), IsValid(Monster)); TestEqual(TEXT("旧怪定义不变"), Monster->GetCombatASC()->GetDefinition().RowName, FName(TEXT("Default")));
        Monster->Destroy(); F.Tick(3.1f);
        TestTrue(TEXT("下一批采用新阶段"), Rooms->Monsters(PS).ContainsByPredicate([](AY01Monster* M) { return M->GetCombatASC()->GetDefinition().RowName == TEXT("Boss"); }));
        Rooms->Request(F.A, false); Rooms->Request(F.A, true); TestEqual(TEXT("重入首阶段"), Rooms->Observe(PS).Stage, 1); return true;
    }
    if (Case == TEXT("MainlineIsolation"))
    {
        TestTrue(TEXT("主线原入口可用"), F.Mode->RequestAdvance(F.B)); F.Tick(6);
        if (!TestEqual(TEXT("主线波怪独立"), F.Mode->GetWaveMonsters().Num(), 3)) { return false; }
        auto* Wave = F.Mode->GetWaveMonsters()[0].Get(); Wave->SetActorLocation(Pawn->GetActorLocation() + FVector(100, 0, 0));
        TestFalse(TEXT("训练玩家不可攻击主线"), ASC->SetCombatTarget(Wave));
        F.Damage(F.A, Wave, 9999); TestTrue(TEXT("主线怪未被跨区GE伤害"), Y01Playground::Health(Wave->GetCombatASC()) > 0);
        Rooms->Request(F.A, false); TestFalse(TEXT("清房不清波"), F.Mode->GetFlowState()->GetWaveSnapshot().bWaveCompleted); return true;
    }
    if (Case == TEXT("WorldFloorAndWalls"))
    {
        F.Tick(0.5f); TestTrue(TEXT("房内真实地面不坠落"), Pawn->GetActorLocation().Z > 80);
        FHitResult Hit; const FVector P = Pawn->GetActorLocation();
        TestTrue(TEXT("围墙阻挡外出"), F.World->LineTraceSingleByChannel(Hit, P + FVector(0, 0, 200), P + FVector(2000, 0, 200), ECC_Visibility)); return true;
    }
    if (Case == TEXT("DirectZeroNoReward"))
    { Monster->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); F.Tick(0.1f); }
    if (Case == TEXT("WaitingDeath") || Case == TEXT("BattleDeathEndsAll"))
    {
        Rooms->Request(F.B, true);
        if (Case == TEXT("BattleDeathEndsAll")) { F.Mode->RequestAdvance(F.B); }
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); F.Tick(0.1f);
        TestFalse(TEXT("死者不能重入"), Rooms->Request(F.A, true));
        TestEqual(TEXT("沿用非Waiting全局失败"), F.Mode->GetFlowState()->GetWaveSnapshot().Phase,
            Case == TEXT("WaitingDeath") ? EY01MatchPhase::Waiting : EY01MatchPhase::Ended);
        TestEqual(TEXT("另一玩家按阶段清理"), Rooms->Observe(F.PS(F.B)).bInside, Case == TEXT("WaitingDeath"));
    }
    else if (Case == TEXT("EndedCleanup")) { F.Mode->TryStartNextWave(); F.Mode->TryEndMatch(false); F.Tick(0.1f); }
    else if (Case == TEXT("LogoutCleanup")) { F.Mode->Logout(F.A); }
    else if (Case == TEXT("EndPlayCleanup")) { Rooms->EndPlay(EEndPlayReason::Quit); }
    else { Rooms->Request(F.A, false); }
    TestEqual(TEXT("清房不发金币"), PS->GetGold(), 0); TestEqual(TEXT("清房不发经验"), PS->GetExperience(), 0);
    TestEqual(TEXT("清房不计击杀"), Rooms->Observe(PS).Kills, 0); F.Tick(6.2f);
    TestEqual(TEXT("离区和结束后没有残留怪或待刷"), Rooms->Monsters(PS).Num(), 0);
    if (Case == TEXT("ExitStopsAndReenterResets"))
    {
        TestTrue(TEXT("返回原地点"), FVector::Dist2D(Origin, Pawn->GetActorLocation()) < 1);
        TestTrue(TEXT("再次进入"), Rooms->Request(F.A, true)); TestEqual(TEXT("立即首批"), Rooms->Observe(PS).Alive, 2);
        TestEqual(TEXT("首阶段"), Rooms->Observe(PS).Stage, 1);
    }
    return true;
}
#endif
