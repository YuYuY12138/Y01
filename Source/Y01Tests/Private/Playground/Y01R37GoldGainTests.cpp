// R37第四期定向测试：金币获取%乘算（小数钱包/自然收入/主线怪均分/清波奖励）。
// 夹具来源（全部沿用既有绿色模式，不新建基础设施）：
// 1. FractionalWallet：Y01WaveTests::FWaveWorld 的最小真实Game世界 + SpawnActor<AY01PlayerState>
//    直连服务器权威PlayerState；客户端态用引擎公开 SetRole(ROLE_SimulatedProxy) 注入（同Y01WaveTests::Role语义）。
// 2. NaturalIncome：Y01CoreReviveTests::FCoreWorld 的真实 AY01R36GameMode + UY01R36Profile
//    （内存19表 + BaseGoldPerSecond入口 + Income TestClock 确定性时钟 + 真实Loadout穿戴装备）。
// 3. MainlineSplit：FCoreWorld 骨架 + AllDead 用例的第二玩家 PostLogin 登记模式；
//    主线怪硬约束满足：怪物由真实 SpawnEntry→RegisterWaveMonster 生成（WaveMonsters 非空后击杀），
//    击杀走真实 UY01DamageEffect 伤害管线（Capture→FreezeDeath→Complete→OnEffectiveKill→OnMainlineMonsterKill）。
// 4. WaveReward：FR37GrowthWorld 的真实权威 AY01GameMode 骨架（TryConfigureMatch + 真实TryStartNextWave/
//    TickUntilBattle + 0.01秒步进），金币获取%经真实 Loadout 入包穿戴提交为装备来源。
// 断言语义披露：
// a. “金币增量”一律断言 GetGold()+GetRewardRemainder() 总增量（小数钱包满1才入账，整数入账存在节拍差）；
// b. NaturalIncome 推进时间用 TestClock 确定性推进（增量≈2×2×5=20，IsNearlyEqual 容差容忍结算节拍），
//    账本 Rate 断言精确等于 4（2×(1+1) 在 double 内精确）；
// c. MainlineSplit 双玩家均为真实初始化+有效PC的“在局”玩家，击杀者(+100%)得30、旁观者(0%)得15，
//    两者基础份额同为30/2=15，金额本身即证明击杀者无额外份；波次奖励注入NoReward包清零，
//    隔离“杀唯一怪自动完成波次发放清波奖励”与均分的叠加（清波乘算由WaveReward用例覆盖）；
// d. 无绕行：未直刷WaveMonsters、未伪造KillEvent、未跳过任何预检路径。
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "GameFramework/WorldSettings.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Y01MatchFixture.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01PlayerController.h"
#include "Player/Y01Character.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01GrowthComponent.h"
#include "Player/Y01AttributeResolver.h"
#include "Player/Y01ItemRuntimeTypes.h"
#include "Combat/Y01Monster.h"
#include "Combat/Y01Combat.h"
#include "Match/Y01GameFlow.h"
#include "Match/Y01KillRewards.h"
#include "Playground/Y01PlaygroundRuntime.h"
#include "Playground/Y01R36Runtime.h"
#include <limits>

#if WITH_DEV_AUTOMATION_TESTS
// TestClock 为收入组件的自动化测试专用好友入口；沿用Y01CoreReviveTests/Y01R36GrowthDropTests同名访问器。
// 必须位于全局命名空间，与Y01IncomeComponent.h里的friend声明匹配。
struct FY01R36DeliveryTestAccess
{
    static void Clock(UY01IncomeComponent* I, TFunction<double()> F) { I->TestClock = MoveTemp(F); }
};
namespace
{
using K = EY01TableKind;

// 内存表注入：金币获取+100%（AttributePercent Target=GoldGain Amount=100）。
void AddGoldGainEffect(FY01MatchFixture& Data, const TCHAR* Name = TEXT("Gain100"))
{
    FY01SourceEffectRow E; E.Kind = EY01SourceEffectKind::AttributePercent;
    E.TargetAttribute = EY01Attribute::GoldGain; E.Amount = 100;
    Data.Add(K::SourceEffect, E, Name);
}

// R36开发配置（独立UY01R36Profile + 内存19表），不触碰全局Settings。
UY01R36Profile* MakeR36Profile(FY01MatchFixture& Data, double BaseGoldPerSecond, int32 InitialGold)
{
    auto* Profile = NewObject<UY01R36Profile>();
    Profile->bEvidenceMode = true;
    Profile->bTrainingEnabled = true;
    Profile->bEconomyEnabled = true;
    Profile->InitialGold = InitialGold;
    Profile->BaseGoldPerSecond = BaseGoldPerSecond;
    Profile->DropFlightSeconds = 10;
    Profile->Character = Data.Ref(K::Character);
    Profile->OrderedWaves = Data.Waves();
    for (const auto& Pair : Data.Tables) { Profile->Tables.Add(Pair.Key, const_cast<UDataTable*>(Pair.Value)); }
    return Profile;
}
}

// ── 用例1：服务器权威小数钱包（TryAddFractionalGold/GetRewardRemainder） ─────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GoldFractionalWalletTest, "Y01.R37.Gold.FractionalWallet",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GoldFractionalWalletTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    // 最小真实Game世界：权威PlayerState直连，不需要完整对局链（钱包入口只做权威与数值校验）。
    const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
        .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
    GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
    World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
    AY01PlayerState* P = World->SpawnActor<AY01PlayerState>();
    ON_SCOPE_EXIT { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); };

    // 0.4+0.4不足1：只累计钱包，金币不变。
    TestTrue(TEXT("0.4入包成功"), P->TryAddFractionalGold(0.4));
    TestTrue(TEXT("再0.4入包成功"), P->TryAddFractionalGold(0.4));
    TestEqual(TEXT("不足1不自动入账金币"), P->GetGold(), 0);
    TestTrue(TEXT("钱包累计0.8"), FMath::IsNearlyEqual(P->GetRewardRemainder(), 0.8, 1.e-9));
    // 再0.3跨过1：入账1金币，钱包余0.1。
    TestTrue(TEXT("0.3触发满1入账"), P->TryAddFractionalGold(0.3));
    TestEqual(TEXT("满1入账1金币"), P->GetGold(), 1);
    TestTrue(TEXT("钱包余0.1"), FMath::IsNearlyEqual(P->GetRewardRemainder(), 0.1, 1.e-9));
    // 非法输入整笔拒绝且状态不变。
    TestFalse(TEXT("负数拒绝"), P->TryAddFractionalGold(-1.));
    TestFalse(TEXT("NaN拒绝"), P->TryAddFractionalGold(std::numeric_limits<double>::quiet_NaN()));
    TestEqual(TEXT("拒绝不动金币"), P->GetGold(), 1);
    TestTrue(TEXT("拒绝不动钱包"), FMath::IsNearlyEqual(P->GetRewardRemainder(), 0.1, 1.e-9));
    // 无权威（客户端态）拒绝。
    P->SetRole(ROLE_SimulatedProxy);
    TestFalse(TEXT("非权威拒绝"), P->TryAddFractionalGold(0.5));
    P->SetRole(ROLE_Authority);
    TestEqual(TEXT("非权威尝试不写金币"), P->GetGold(), 1);
    TestTrue(TEXT("非权威尝试不写钱包"), FMath::IsNearlyEqual(P->GetRewardRemainder(), 0.1, 1.e-9));
    return true;
}

namespace
{
// 自然收入世界：真实R36经济会话 + TestClock确定性时钟；波次怪物无害木桩，无击杀干扰。
struct FGoldIncomeWorld
{
    FY01MatchFixture Data;
    UWorld* World = nullptr;
    AY01R36GameMode* Mode = nullptr;
    AY01R36Controller* PC = nullptr;
    AY01PlayerState* P = nullptr;
    double Time = 1000;
    FGoldIncomeWorld()
    {
        auto& C = Data.Row<FY01CharacterRow>(K::Character);
        C.InitialEquipment.Reset(); C.InitialSkills.Reset();
        C.Health = 300; C.HealthRegen = 0; C.AttackDamage = 30; C.Range = 1000;
        auto& M = Data.Row<FY01MonsterRow>(K::Monster);
        M.Health = 10; M.AttackDamage = 0; M.Range = 0; M.MoveSpeed = 0; M.DetectionRadius = 0; M.Skills.Reset();
        AddGoldGainEffect(Data);
        for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Blue"))})
        {
            auto& Item = Data.Row<FY01EquipmentRow>(K::Equipment, Name);
            Item.DisplayName = FText::FromString(TEXT("金币获取装备"));
            Item.EffectIds = {Data.Ref(K::SourceEffect, TEXT("Gain100"))};
        }
        auto& W = Data.Row<FY01WaveRow>(K::Wave);
        W.PreparationSeconds = .02f; W.Spawns = {{Data.Ref(K::Monster), 1, 0}};
        Data.Row<FY01WaveRow>(K::Wave, TEXT("Boss")).PreparationSeconds = 1000001.f;
        auto* Profile = MakeR36Profile(Data, 2., 0);
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01R36GameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01R36GameMode>(); World->SpawnActor<AY01R36Config>()->Content = Profile;
        PC = World->SpawnActor<AY01R36Controller>(); P = PC->GetPlayerState<AY01PlayerState>();
        if (!P) { P = World->SpawnActor<AY01PlayerState>(); }
        P->SetOwner(PC);
        FY01R36DeliveryTestAccess::Clock(P->GetIncome(), [this] { return Time; });
        auto* Pawn = World->SpawnActor<AY01PlaygroundCharacter>(FVector(0, 0, 90), FRotator::ZeroRotator);
        PC->PlayerState = P; PC->Possess(Pawn); P->GetCombatASC()->InitAbilityActorInfo(P, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement();
        Mode->GetFlowState()->AddPlayerState(P);
        Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay();
        Mode->TryStartNextWave();
        Tick(.15);
    }
    ~FGoldIncomeWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    void Tick(double Seconds)
    { for (int32 I = 0; I < FMath::CeilToInt(Seconds / .025); ++I) { ++GFrameCounter; Time += .025; World->Tick(LEVELTICK_All, .025f); } }
    FY01CommittedView View() const { P->GetLoadout()->PublishCommittedView(); FY01CommittedView V; P->GetLoadout()->ReadCommittedView(V); return V; }
    FY01ItemInstance Item(FGuid Id) const { FY01ItemInstance I; P->GetLoadout()->FindItem(Id, I); return I; }
    bool EquipGain()
    {
        auto* L = P->GetLoadout();
        FY01ItemGrant GrantItem{FGuid::NewGuid(), Data.Ref(K::Equipment)};
        if (!L->AddItems(FY01DataCatalog(Data.Tables), {GrantItem}).IsOk()) { return false; }
        const auto V = View(); FY01ItemRequest R;
        R.MatchId = V.MatchId; R.JoinSequence = V.JoinSequence; R.Seq = V.ExpectedSeq;
        R.Operation = EY01ItemOperation::Equip; R.ItemId = GrantItem.ItemId; R.Slot = 0; R.ConfigRevision = V.ConfigRevision;
        R.ExpectedEntityRevision = Item(GrantItem.ItemId).EntityRevision;
        if (!V.EquipmentSlots.IsValidIndex(0)) { return false; }
        R.ExpectedSlotItemId = V.EquipmentSlots[0];
        if (R.ExpectedSlotItemId.IsValid()) { R.ExpectedSlotEntityRevision = Item(R.ExpectedSlotItemId).EntityRevision; }
        return L->SubmitRequest(PC, R).IsOk();
    }
};
}

// ── 用例2：自然收入按 Rate=金币/秒×(1+金币获取%) 放大 ───────────────────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GoldNaturalIncomeTest, "Y01.R37.Gold.NaturalIncome",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GoldNaturalIncomeTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FGoldIncomeWorld F;
    if (!TestTrue(TEXT("真实R36经济会话配置"), F.Mode->IsMatchConfigured())) { return false; }
    // 装备前基础速率=2（BaseGoldPerSecond入口，Gain=0）。
    TestEqual(TEXT("装备前自然收入速率=2"), F.P->GetIncome()->Observe().State.Rate, 2.);
    if (!TestTrue(TEXT("金币获取装备真实入包并穿戴"), F.EquipGain())) { return false; }
    FY01ResolvedAttributes Cached;
    if (!TestTrue(TEXT("读取来源已提交解析缓存"), F.P->GetSourceEffects()->ObserveAttributes(Cached).IsOk())) { return false; }
    TestEqual(TEXT("归一化金币获取%=1"), Cached.GoldGainPercent, 1.);
    TestEqual(TEXT("装备后收入账本Rate=2×(1+1)=4"), F.P->GetIncome()->Observe().State.Rate, 4.);
    // 推进约5秒：金币+可见钱包总增量≈2×2×5=20（整数入账节拍抖动用容差）。
    const int32 GoldBefore = F.P->GetGold();
    const double RemainderBefore = F.P->GetRewardRemainder();
    F.Tick(5.15);
    const double TotalDelta = double(F.P->GetGold() - GoldBefore) + (F.P->GetRewardRemainder() - RemainderBefore);
    TestTrue(TEXT("金币+钱包总增量≈2×2×5=20"), FMath::IsNearlyEqual(TotalDelta, 20., 1.5));
    return true;
}

namespace
{
// 主线均分世界：真实波次生成主线怪（Gold=30），两名真实玩家（PostLogin登记）。
struct FGoldSplitWorld
{
    FY01MatchFixture Data;
    UWorld* World = nullptr;
    AY01R36GameMode* Mode = nullptr;
    AY01R36Controller* PC1 = nullptr; AY01PlayerState* P1 = nullptr;
    AY01R36Controller* PC2 = nullptr; AY01PlayerState* P2 = nullptr;
    double Time = 2000;
    FString Error;
    FGoldSplitWorld()
    {
        auto& C = Data.Row<FY01CharacterRow>(K::Character);
        C.InitialEquipment.Reset(); C.InitialSkills.Reset();
        C.Health = 300; C.HealthRegen = 0; C.AttackDamage = 30; C.Range = 1000;
        auto& M = Data.Row<FY01MonsterRow>(K::Monster);
        M.Health = 20; M.AttackDamage = 0; M.Range = 0; M.MoveSpeed = 0; M.DetectionRadius = 0;
        M.Skills.Reset(); M.Gold = 30; M.DropReward = Data.Ref(K::RewardPackage);
        AddGoldGainEffect(Data);
        for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Blue"))})
        {
            auto& Item = Data.Row<FY01EquipmentRow>(K::Equipment, Name);
            Item.DisplayName = FText::FromString(TEXT("金币获取装备"));
            Item.EffectIds = {Data.Ref(K::SourceEffect, TEXT("Gain100"))};
        }
        auto& W = Data.Row<FY01WaveRow>(K::Wave);
        W.PreparationSeconds = .02f; W.Spawns = {{Data.Ref(K::Monster), 1, 0}};
        // 清零波次奖励隔离清波路径：契约默认波次奖励包为Gold=10，击杀唯一怪会自动完成波次并把
        // 清波奖励（逐人×(1+%)）叠加在均分上（曾致50/25假象）；清波乘算由WaveReward用例单独覆盖。
        FY01RewardPackageRow NoReward; Data.Add(K::RewardPackage, NoReward, TEXT("NoReward"));
        W.Reward = Data.Ref(K::RewardPackage, TEXT("NoReward"));
        Data.Row<FY01WaveRow>(K::Wave, TEXT("Boss")).PreparationSeconds = 1000001.f;
        auto* Profile = MakeR36Profile(Data, 0., 0);
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01R36GameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01R36GameMode>(); World->SpawnActor<AY01R36Config>()->Content = Profile;
        // 第一位玩家（Y01CoreReviveTests::FCoreWorld模式）。
        PC1 = World->SpawnActor<AY01R36Controller>(); P1 = PC1->GetPlayerState<AY01PlayerState>();
        if (!P1) { P1 = World->SpawnActor<AY01PlayerState>(); }
        P1->SetOwner(PC1);
        FY01R36DeliveryTestAccess::Clock(P1->GetIncome(), [this] { return Time; });
        auto* Pawn1 = World->SpawnActor<AY01PlaygroundCharacter>(FVector(-700, 0, 90), FRotator::ZeroRotator);
        PC1->PlayerState = P1; PC1->Possess(Pawn1); P1->GetCombatASC()->InitAbilityActorInfo(P1, Pawn1);
        Pawn1->GetCharacterMovement()->DisableMovement();
        Mode->GetFlowState()->AddPlayerState(P1);
        Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay();
        // 第二位玩家：AllDead用例的真实PostLogin登记（配置后登录走InitializePlayer全链）。
        PC2 = World->SpawnActor<AY01R36Controller>(); P2 = PC2->GetPlayerState<AY01PlayerState>();
        if (!P2) { P2 = World->SpawnActor<AY01PlayerState>(); }
        P2->SetOwner(PC2); PC2->PlayerState = P2;
        auto* Pawn2 = World->SpawnActor<AY01PlaygroundCharacter>(FVector(-900, 0, 90), FRotator::ZeroRotator);
        PC2->Possess(Pawn2); P2->GetCombatASC()->InitAbilityActorInfo(P2, Pawn2);
        Pawn2->GetCharacterMovement()->DisableMovement();
        FY01R36DeliveryTestAccess::Clock(P2->GetIncome(), [this] { return Time; });
        Mode->GetFlowState()->AddPlayerState(P2); PC2->Player = NewObject<ULocalPlayer>(GEngine); Mode->PostLogin(PC2);
        Mode->TryStartNextWave();
        Tick(1.);
    }
    ~FGoldSplitWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    void Tick(double Seconds)
    { for (int32 I = 0; I < FMath::CeilToInt(Seconds / .01); ++I) { ++GFrameCounter; Time += .01; World->Tick(LEVELTICK_All, .01f); } }
    FY01CommittedView View(AY01PlayerState* Player) const
    { Player->GetLoadout()->PublishCommittedView(); FY01CommittedView V; Player->GetLoadout()->ReadCommittedView(V); return V; }
    FY01ItemInstance Item(AY01PlayerState* Player, FGuid Id) const { FY01ItemInstance I; Player->GetLoadout()->FindItem(Id, I); return I; }
    // 击杀者P1真实入包穿戴+100%装备（经济开启但0金币/0速率，不干扰均分断言）。
    bool EquipGainOnKiller()
    {
        auto* L = P1->GetLoadout();
        FY01ItemGrant GrantItem{FGuid::NewGuid(), Data.Ref(K::Equipment)};
        if (!L->AddItems(FY01DataCatalog(Data.Tables), {GrantItem}).IsOk()) { return false; }
        const auto V = View(P1); FY01ItemRequest R;
        R.MatchId = V.MatchId; R.JoinSequence = V.JoinSequence; R.Seq = V.ExpectedSeq;
        R.Operation = EY01ItemOperation::Equip; R.ItemId = GrantItem.ItemId; R.Slot = 0; R.ConfigRevision = V.ConfigRevision;
        R.ExpectedEntityRevision = Item(P1, GrantItem.ItemId).EntityRevision;
        if (!V.EquipmentSlots.IsValidIndex(0)) { return false; }
        R.ExpectedSlotItemId = V.EquipmentSlots[0];
        if (R.ExpectedSlotItemId.IsValid()) { R.ExpectedSlotEntityRevision = Item(P1, R.ExpectedSlotItemId).EntityRevision; }
        return L->SubmitRequest(PC1, R).IsOk();
    }
    // 击杀者P1以真实伤害GE致死主线怪（Capture要求击杀者成长会话已启用，两者均经配置链初始化）。
    // 与绿灯用例同款前置：本世界无地板，波次怪会被重力坠离出生点，致死前先钉住怪并把玩家移到目标旁
    // （Y01CoreReviveTests::Drop/Y01R36GrowthDropTests同款）；逐项记录伤害管线前置，红灯可定位到具体关卡。
    struct FKillProbe { bool bLegal = false; bool bApplied = false; float Before = 0; float After = 0;
        float Distance = 0; float Range = 0; bool Killed() const { return After <= 0.f; } };
    FKillProbe KillMainlineMonster()
    {
        FKillProbe K;
        if (Mode->GetWaveMonsters().IsEmpty()) { return K; }
        auto* Target = Mode->GetWaveMonsters()[0].Get();
        auto* A = P1->GetCombatASC();
        K.Before = Target->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
        if (auto* Move = Target->GetCharacterMovement()) { Move->DisableMovement(); }
        Target->SetActorLocation(FVector(0, 0, 90));
        A->GetAvatarActor()->SetActorLocation(FVector(-100, 0, 90));
        K.Distance = FVector::Dist(A->GetAvatarActor()->GetActorLocation(), Target->GetActorLocation());
        K.Range = A->GetUnitStats().Range;
        K.bLegal = A->IsValidCombatTarget(Target);
        auto Spec = A->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, A->MakeEffectContext());
        Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, 1000.f);
        K.bApplied = A->ApplyGameplayEffectSpecToTarget(*Spec.Data, Target->GetCombatASC()).WasSuccessfullyApplied();
        K.After = Target->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
        return K;
    }
};
}

// ── 用例3：主线怪死亡金币在局均分，逐人按(1+各自%)放大，击杀者无额外份 ──────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GoldMainlineSplitTest, "Y01.R37.Gold.MainlineSplit",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GoldMainlineSplitTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FGoldSplitWorld F;
    if (!TestTrue(TEXT("真实R36会话配置"), F.Mode->IsMatchConfigured() && F.Error.IsEmpty())) { return false; }
    if (!TestTrue(TEXT("第二位真实玩家初始化"), F.P2->IsCharacterInitialized())) { return false; }
    if (!TestTrue(TEXT("两位玩家均在局（有效PC）"),
        F.Mode->IsPlayerPresent(F.P1) && F.Mode->IsPlayerPresent(F.P2))) { return false; }
    if (!TestTrue(TEXT("真实波次生成主线怪"), !F.Mode->GetWaveMonsters().IsEmpty())) { return false; }
    if (!TestTrue(TEXT("击杀者穿戴+100%金币获取装备"), F.EquipGainOnKiller())) { return false; }
    FY01ResolvedAttributes Cached;
    if (!TestTrue(TEXT("击杀者金币获取%=1"), F.P1->GetSourceEffects()->ObserveAttributes(Cached).IsOk()
        && Cached.GoldGainPercent == 1.)) { return false; }

    const int32 Gold1 = F.P1->GetGold(); const double Rem1 = F.P1->GetRewardRemainder();
    const int32 Gold2 = F.P2->GetGold(); const double Rem2 = F.P2->GetRewardRemainder();
    const auto Kill = F.KillMainlineMonster();
    if (!TestTrue(TEXT("击杀者可合法锁定主线怪（初始化/距离/权限/在局）"), Kill.bLegal)) { return false; }
    if (!TestTrue(FString::Printf(TEXT("伤害GE应用成功 Dist=%.1f Range=%.1f Before=%.1f"), Kill.Distance, Kill.Range, Kill.Before), Kill.bApplied)) { return false; }
    if (!TestTrue(TEXT("击杀者致死主线怪"), Kill.Killed())) { return false; }
    F.Tick(.1);
    const int32 D1 = F.P1->GetGold() - Gold1; const double DR1 = F.P1->GetRewardRemainder() - Rem1;
    const int32 D2 = F.P2->GetGold() - Gold2; const double DR2 = F.P2->GetRewardRemainder() - Rem2;
    auto* Kills = F.World->GetSubsystem<UY01KillRewards>();
    // 均分：30/2=15基础份额；击杀者(+100%)得30，旁观者(0%)得15——金额本身证明击杀者无额外份。
    // 失败时报告实际增量与击杀事件链状态（成长会话/待处理事件/成长账本），可直接定位断链环节。
    TestTrue(FString::Printf(TEXT("击杀者增量=30 实际=%d+%.4f 旁观者实际=%d+%.4f 成长启用=%d 事件待队列=%d 已消费击杀=%llu"),
        D1, DR1, D2, DR2, F.P1->GetGrowth()->IsEnabled() ? 1 : 0,
        Kills && Kills->HasPending(F.P1) ? 1 : 0, F.P1->GetGrowth()->GetLedger().AppliedKills),
        FMath::IsNearlyEqual(double(D1) + DR1, 30., 1.e-9));
    TestTrue(FString::Printf(TEXT("旁观者增量=15 实际=%d+%.4f"), D2, DR2),
        FMath::IsNearlyEqual(double(D2) + DR2, 15., 1.e-9));
    TestTrue(TEXT("整额入账无小数残留"),
        FMath::IsNearlyEqual(F.P1->GetRewardRemainder(), 0., 1.e-9) && FMath::IsNearlyEqual(F.P2->GetRewardRemainder(), 0., 1.e-9));
    return true;
}

namespace
{
// 清波奖励世界：真实权威AY01GameMode骨架（Y01R37GrowthTriggerTests模式），波次奖励Gold=10/Exp=5。
struct FGoldWaveWorld : FY01MatchFixture
{
    UWorld* World = nullptr;
    AY01GameMode* Mode = nullptr;
    AY01PlayerController* PC = nullptr;
    AY01PlayerState* P = nullptr;
    FString Error;
    bool bConfigured = false;
    bool bWaveStarted = false;
    FY01ItemResult InitResult;
    FGuid Match = FGuid::NewGuid();
    FGoldWaveWorld()
    {
        auto& C = Row<FY01CharacterRow>(K::Character);
        C.InitialEquipment.Reset(); C.InitialSkills.Reset();
        C.Health = 100; C.AttackDamage = 20; C.Range = 1000; C.Armor = 0; C.HealthRegen = 0;
        C.Strength = 0; C.Agility = 0; C.Wisdom = 0;
        auto& M = Row<FY01MonsterRow>(K::Monster);
        M.Health = 20; M.AttackDamage = 0; M.Range = 0; M.MoveSpeed = 0; M.DetectionRadius = 0;
        M.Skills.Reset(); M.Gold = 0;   // 怪物金币=0，隔离主线均分路径，只观察清波奖励。
        AddGoldGainEffect(*this);
        for (const FName Name : {FName(TEXT("Default")), FName(TEXT("Blue"))})
        {
            auto& Item = Row<FY01EquipmentRow>(K::Equipment, Name);
            Item.DisplayName = FText::FromString(TEXT("金币获取装备"));
            Item.EffectIds = {Ref(K::SourceEffect, TEXT("Gain100"))};
        }
        FY01RewardPackageRow WaveReward; WaveReward.Gold = 10; WaveReward.Experience = 5;
        Add(K::RewardPackage, WaveReward, TEXT("WaveReward"));
        auto& W = Row<FY01WaveRow>(K::Wave);
        W.PreparationSeconds = .02f; W.Spawns = {{Ref(K::Monster), 1, 0}};
        W.Reward = Ref(K::RewardPackage, TEXT("WaveReward"));
        Row<FY01WaveRow>(K::Wave, TEXT("Boss")).PreparationSeconds = 1000001.f;
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false).CreatePhysicsScene(true)
            .RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL()); World->SetGameInstance(NewObject<UGameInstance>(GEngine));
        World->GetWorldSettings()->DefaultGameMode = AY01GameMode::StaticClass(); World->SetGameMode(FURL());
        Mode = World->GetAuthGameMode<AY01GameMode>();
        PC = World->SpawnActor<AY01PlayerController>();
        P = World->SpawnActor<AY01PlayerState>();
        auto* Pawn = World->SpawnActor<AY01Character>(FVector(0, 0, 100), FRotator::ZeroRotator);
        PC->PlayerState = P; PC->Possess(Pawn); P->SetOwner(PC); P->GetCombatASC()->InitAbilityActorInfo(P, Pawn);
        Pawn->GetCharacterMovement()->DisableMovement();
        Mode->GetFlowState()->AddPlayerState(P);
        bConfigured = Mode->TryConfigureMatch(FY01DataCatalog(Tables), Waves(), Ref(K::Character), Error);
        InitResult = P->GetLoadout()->Initialize(Match, 1);
        bWaveStarted = Mode->TryStartNextWave();
        Mode->DispatchBeginPlay(); World->GetWorldSettings()->NotifyBeginPlay(); World->BeginPlay();
    }
    ~FGoldWaveWorld() { World->EndPlay(EEndPlayReason::Quit); World->DestroyWorld(false); GEngine->DestroyWorldContext(World); }
    void Tick(float Seconds)
    { const int32 Steps = FMath::RoundToInt(Seconds * 100); for (int32 I = 0; I < Steps; ++I) { ++GFrameCounter; World->Tick(LEVELTICK_All, .01f); } }
    bool TickUntilBattleAndSpawned()
    {
        for (int32 I = 0; I < 60 && (Phase() != EY01MatchPhase::Battle || Mode->GetWaveMonsters().Num() == 0); ++I)
        { ++GFrameCounter; World->Tick(LEVELTICK_All, .01f); }
        return Phase() == EY01MatchPhase::Battle && Mode->GetWaveMonsters().Num() == 1;
    }
    EY01MatchPhase Phase() const { return Mode->GetFlowState()->GetWaveSnapshot().Phase; }
    FY01LoadoutSnapshot View() const { FY01LoadoutSnapshot V; P->GetLoadout()->Observe(V); return V; }
    FY01ItemInstance Item(FGuid Id) const { FY01ItemInstance I; P->GetLoadout()->FindItem(Id, I); return I; }
    bool EquipGain()
    {
        auto* L = P->GetLoadout();
        FY01ItemGrant GrantItem{FGuid::NewGuid(), Ref(K::Equipment)};
        if (!L->AddItems(FY01DataCatalog(Tables), {GrantItem}).IsOk()) { return false; }
        const auto V = View(); FY01ItemRequest R;
        R.MatchId = Match; R.JoinSequence = 1; R.Seq = V.ExpectedSeq;
        R.Operation = EY01ItemOperation::Equip; R.ItemId = GrantItem.ItemId; R.Slot = 0;
        R.ExpectedEntityRevision = Item(GrantItem.ItemId).EntityRevision;
        if (V.EquipmentSlots.IsValidIndex(0))
        {
            R.ExpectedSlotItemId = V.EquipmentSlots[0];
            if (R.ExpectedSlotItemId.IsValid()) { R.ExpectedSlotEntityRevision = Item(R.ExpectedSlotItemId).EntityRevision; }
        }
        return L->TryEquip(PC, R).IsOk();
    }
};
}

// ── 用例4：清波奖励金币按(1+各自%)乘算经小数钱包发放，经验保持原值 ──────────────
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37GoldWaveRewardTest, "Y01.R37.Gold.WaveReward",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
bool FY01R37GoldWaveRewardTest::RunTest(const FString&)
{
    TGuardValue<uint64> Frames(GFrameCounter, GFrameCounter);
    FGoldWaveWorld F;
    if (!TestTrue(FString::Printf(TEXT("真实权威Mode目录配置成功 Error=%s"), *F.Error),
        F.bConfigured && F.Error.IsEmpty() && F.Mode->IsMatchConfigured())) { return false; }
    if (!TestTrue(TEXT("库存初始化成功"), F.InitResult.IsOk())) { return false; }
    if (!TestTrue(TEXT("玩家角色初始化"), F.P->IsCharacterInitialized())) { return false; }
    if (!TestTrue(TEXT("金币获取装备真实入包并穿戴"), F.EquipGain())) { return false; }
    FY01ResolvedAttributes Cached;
    if (!TestTrue(TEXT("归一化金币获取%=1"), F.P->GetSourceEffects()->ObserveAttributes(Cached).IsOk()
        && Cached.GoldGainPercent == 1.)) { return false; }
    if (!TestTrue(TEXT("波次进入准备阶段"), F.bWaveStarted)) { return false; }
    if (!TestTrue(TEXT("真实进入Battle且主线怪已生成"), F.TickUntilBattleAndSpawned())) { return false; }

    const int32 GoldBefore = F.P->GetGold();
    const int32 ExpBefore = F.P->GetExperience();
    const double RemainderBefore = F.P->GetRewardRemainder();
    if (!TestTrue(TEXT("真实完成波次"), F.Mode->TryCompleteWave())) { return false; }
    const double TotalDelta = double(F.P->GetGold() - GoldBefore) + (F.P->GetRewardRemainder() - RemainderBefore);
    TestTrue(TEXT("金币+钱包增量=10×(1+100%)=20"), FMath::IsNearlyEqual(TotalDelta, 20., 1.e-6));
    TestEqual(TEXT("经验为波次经验原值5不变"), F.P->GetExperience() - ExpBefore, 5);
    TestEqual(TEXT("完成计数一次"), F.Mode->GetFlowState()->GetWaveSnapshot().CompletedWaves, 1);
    TestFalse(TEXT("幂等：重复完成不重复发奖"), F.Mode->TryCompleteWave());
    TestTrue(TEXT("重复完成不改金币"), FMath::IsNearlyEqual(double(F.P->GetGold() - GoldBefore) + (F.P->GetRewardRemainder() - RemainderBefore), 20., 1.e-6));
    return true;
}
#endif
