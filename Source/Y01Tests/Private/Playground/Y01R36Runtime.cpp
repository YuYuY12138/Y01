#include "Playground/Y01R36Runtime.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01EconomyComponent.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01GrowthComponent.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "GameFramework/HUD.h"
#include "Playground/Y01R36UI.h"
#include "Playground/Y01R36Verification.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "Match/Y01GameFlow.h"
#include "Combat/Y01Combat.h"
#include "Combat/Y01Core.h"

UY01R36Profile* AY01R36Config::Resolve(const FString& Selection) const
{
    if (Selection.IsEmpty() || Selection == TEXT("profile") || Selection == TEXT("content")) { return Content.LoadSynchronous(); }
    if (Selection == TEXT("evidence")) { return Baseline.LoadSynchronous(); }
    if (Selection == TEXT("compare")) { return Compare.LoadSynchronous(); }
    return nullptr;
}
AY01R36GameMode::AY01R36GameMode()
{ PlayerControllerClass = AY01R36Controller::StaticClass(); HUDClass = AHUD::StaticClass(); }
void AY01R36GameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!HasAuthority() || bControlledExitRequested) { return; }
    const auto Players = EconomyPlayers.Array();
    for (const auto& Entry : Players)
    {
        auto* Player = Entry.Get();
        if (!IsValid(Player)) { EconomyPlayers.Remove(Entry); continue; }
        Player->GetDropDelivery()->Schedule();
        const int32 Before = Player->GetGold();
        const auto Settled = Player->GetIncome()->Settle();
        if (!Settled.IsOk() && LoadoutError.IsEmpty()) { LoadoutError = Settled.Message; }
        if (Before != Player->GetGold()) { Player->GetLoadout()->PublishCommittedView(); }
    }
    StopPollSeconds += DeltaSeconds;
    if (StopPollSeconds < .5f) { return; }
    StopPollSeconds = 0;
    FString Fixture, Scenario, Label, Token; FGuid ParsedToken;
    FParse::Value(FCommandLine::Get(), TEXT("R36Fixture="), Fixture);
    FParse::Value(FCommandLine::Get(), TEXT("R36Scenario="), Scenario);
    if (!FParse::Value(FCommandLine::Get(), TEXT("R36Run="), Label)) { return; }
    const bool S2 = Label.StartsWith(TEXT("R36-S2-Visible-")) || Label.StartsWith(TEXT("R36-S2-Runtime-"));
    const bool S3 = Label.StartsWith(TEXT("R36-S3-Visible-")) || Label.StartsWith(TEXT("R36-S3-Presentation-"));
    const bool S4 = Label.StartsWith(TEXT("R36-S4-")) && Label.Len() > 7;
    const bool Controlled = Fixture == TEXT("controlled") && (!S4 || Scenario.IsEmpty());
    const bool Natural = (S2 || S3 || S4) && Fixture.IsEmpty() && Scenario == TEXT("natural");
    if ((!Label.StartsWith(TEXT("R36-S1-UI-")) && !S2 && !S3 && !S4) || (!Controlled && !Natural)
        || !FParse::Value(FCommandLine::Get(), TEXT("R36StopToken="), Token)
        || !FGuid::ParseExact(Token, EGuidFormats::Digits, ParsedToken) || !ParsedToken.IsValid()) { return; }
    for (const TCHAR C : Label)
    {
        if (!((C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z'))
            || (C >= TEXT('0') && C <= TEXT('9')) || C == TEXT('-') || C == TEXT('_'))) { return; }
    }
    FString Lifecycle;
    const auto* Flow = GetFlowState();
    const auto Phase = Flow ? Flow->GetWaveSnapshot().Phase : EY01MatchPhase::Waiting;
    if ((S2 || S3) && Controlled && Scenario.IsEmpty()
        && FParse::Value(FCommandLine::Get(), TEXT("R36Lifecycle="), Lifecycle) && Lifecycle == TEXT("controlled")
        && (Phase == EY01MatchPhase::Preparing || Phase == EY01MatchPhase::Battle))
    {
        auto Authorized = [&](const TCHAR* Name)
        {
            const FString File = FPaths::ProjectSavedDir() / Label / Name; FString Value;
            return IFileManager::Get().FileSize(*File) == 32 && FFileHelper::LoadFileToString(Value, *File) && Value == Token;
        };
        if (!bLifecycleDeathInjected && Authorized(TEXT("death.request")))
        {
            AY01PlayerState* First = nullptr; int32 Join = MAX_int32;
            for (const auto& Entry : EconomyPlayers)
            {
                auto* Player = Entry.Get(); FY01CommittedView View;
                if (IsValid(Player) && IsPlayerPresent(Player) && Player->GetLoadout()->ReadCommittedView(View) && View.JoinSequence < Join)
                { First = Player; Join = View.JoinSequence; }
            }
            if (First && First->GetCombatASC())
            {
                bLifecycleDeathInjected = true;
                First->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0);
                First->GetLoadout()->PublishCommittedView();
                UE_LOG(LogTemp, Display, TEXT("Y01R36 LIFECYCLE_DEATH Join=%d Controlled=1 NaturalPurchase=0"), Join);
            }
        }
        if (Authorized(TEXT("ended.request")) && TryEndMatch(false))
        { UE_LOG(LogTemp, Display, TEXT("Y01R36 LIFECYCLE_ENDED Controlled=1 NaturalPurchase=0")); }
    }
    // 路径固定在Saved本次目录，不接收任意路径；严格32字节令牌，不删除停止证据。
    const FString Path = FPaths::ProjectSavedDir() / Label / TEXT("stop.request");
    FString Requested;
    if (IFileManager::Get().FileSize(*Path) == 32 && FFileHelper::LoadFileToString(Requested, *Path) && Requested == Token)
    {
        bControlledExitRequested = true;
        RequestControlledExit();
    }
}
void AY01R36GameMode::RequestControlledExit()
{
    UE_LOG(LogTemp, Display, TEXT("Y01R36 CONTROLLED_NORMAL_EXIT"));
    RequestEngineExit(TEXT("R36自有受控运行请求正常退出"));
}
UY01R36Profile* AY01R36GameMode::BuildFormalProfile(FString& Error)
{
    static const TCHAR* BasePath = TEXT("/Game/Y01Playground/Data/DT_");
    auto* Profile = NewObject<UY01R36Profile>(this);
    const UEnum* KindEnum = StaticEnum<EY01TableKind>();
    for (int32 I = 1; I <= static_cast<int32>(EY01TableKind::Consumable); ++I)
    {
        const EY01TableKind Kind = static_cast<EY01TableKind>(I);
        const FString Name = KindEnum->GetNameStringByValue(I);
        const FString Path = FString::Printf(TEXT("%s%s"), BasePath, *Name);
        auto* Table = LoadObject<UDataTable>(nullptr, *Path);
        if (!Table)
        {
            Error = FString::Printf(TEXT("正式数据表缺失：%s（%s）"), *Path, *Name);
            return nullptr;
        }
        Profile->Tables.Add(Kind, Table);
    }
    const auto Catalog = Profile->BuildCatalog();
    FY01DataValidation ScanErrors;
    const auto* Rules = Catalog.FindRow<FY01MatchRulesRow>({EY01TableKind::MatchRules, TEXT("Default")}, ScanErrors);
    if (!Rules)
    {
        Error = TEXT("正式DT_MatchRules缺少Default行");
        for (const auto& E : ScanErrors.Errors) { Error += FString::Printf(TEXT(" | %s%s.%s.%s: %s"), BasePath,
                    *KindEnum->GetNameStringByValue(static_cast<int64>(E.Source.Type)), *E.Source.RowName.ToString(), *E.Field, *E.Message); }
        return nullptr;
    }
    // 从Startup填充Profile
    Profile->Character = Rules->Startup.Character;
    Profile->bTrainingEnabled = Rules->Startup.bTrainingEnabled;
    Profile->bR34AutoAttackEnabled = Rules->Startup.bAutoAttackEnabled;
    Profile->bCoreDefenseEnabled = Rules->Startup.bCoreDefenseEnabled;
    Profile->bEconomyEnabled = Rules->Startup.bEconomyEnabled;
    Profile->CoreRules = {EY01TableKind::MatchRules, TEXT("Default")};
    Profile->TalentChoice = Rules->Startup.TalentChoice;
    Profile->InitialGold = Rules->Startup.InitialGold;
    Profile->BaseGoldPerSecond = Rules->Startup.BaseGoldPerSecond;
    Profile->DropFlightSeconds = Rules->Startup.DropFlightSeconds;
    Profile->bEvidenceMode = false;
    // 从正式表自动发现
    Profile->OrderedWaves = Catalog.CollectEnabled(EY01TableKind::Wave, ScanErrors);
    Profile->ContentEquipment = Catalog.CollectEnabled(EY01TableKind::Equipment, ScanErrors);
    Profile->ContentSkills = Catalog.CollectEnabled(EY01TableKind::Skill, ScanErrors);
    Profile->ContentShopOffers = Catalog.CollectEnabled(EY01TableKind::ShopOffer, ScanErrors);
    Profile->ContentRecipes = Catalog.CollectEnabled(EY01TableKind::Synthesis, ScanErrors);
    Profile->BaseAttributeMappings = Catalog.CollectEnabled(EY01TableKind::AttributeMapping, ScanErrors);
    if (!ScanErrors.IsValid())
    {
        Error = TEXT("正式表扫描错误");
        for (const auto& E : ScanErrors.Errors) { Error += FString::Printf(TEXT(" | %s%s.%s.%s: %s"), BasePath,
                    *KindEnum->GetNameStringByValue(static_cast<int64>(E.Source.Type)), *E.Source.RowName.ToString(), *E.Field, *E.Message); }
        return nullptr;
    }
    UE_LOG(LogTemp, Display, TEXT("Y01R36 FORMAL_PROFILE Tables=%d Waves=%d Equipment=%d Skills=%d Offers=%d Recipes=%d Mappings=%d"),
        Profile->Tables.Num(), Profile->OrderedWaves.Num(), Profile->ContentEquipment.Num(),
        Profile->ContentSkills.Num(), Profile->ContentShopOffers.Num(), Profile->ContentRecipes.Num(),
        Profile->BaseAttributeMappings.Num());
    return Profile;
}
UY01PlaygroundProfile* AY01R36GameMode::SelectProfile(FString& Error)
{
    FString Selection; FParse::Value(FCommandLine::Get(), TEXT("R36Profile="), Selection);
    // 显式profile/evidence/compare需要地图Config
    if (Selection == TEXT("profile") || Selection == TEXT("evidence") || Selection == TEXT("compare"))
    {
        int32 Count = 0; UY01R36Profile* Profile = nullptr;
        for (TActorIterator<AY01R36Config> It(GetWorld()); It; ++It) { ++Count; Profile = It->Resolve(Selection); }
        if (Count != 1 || !Profile)
        {
            Error = FString::Printf(TEXT("显式R36Profile=%s需要地图Config中的有效Profile（config数量=%d）"), *Selection, Count);
            return nullptr;
        }
        return Profile;
    }
    if (!Selection.IsEmpty() && Selection != TEXT("content"))
    { Error = TEXT("未知R36Profile选择：") + Selection; return nullptr; }
    // 仅保留无磁盘身份的程序夹具；已加载的真实Profile绝不能接管日常入口。
#if WITH_DEV_AUTOMATION_TESTS
    {
        int32 Count = 0; UY01R36Profile* InMemory = nullptr;
        for (TActorIterator<AY01R36Config> It(GetWorld()); It; ++It) { ++Count; InMemory = It->Content.Get(); }
        if (Count == 1 && InMemory && InMemory->GetOutermost() == GetTransientPackage()) { return InMemory; }
    }
#endif
    // 无参数/content始终从正式25表构建，不依赖地图Config或磁盘Profile的加载状态。
    FString FormalError;
    auto* Formal = BuildFormalProfile(FormalError);
    if (Formal) { return Formal; }
    Error = FormalError;
    return nullptr;
}
void AY01R36GameMode::OnPlayerInitialized(AY01PlayerState* Player)
{
    Super::OnPlayerInitialized(Player);
    if (!HasAuthority() || !IsValid(Player) || !Player->IsCharacterInitialized()) { return; }
    auto* Loadout = Player->GetLoadout();
    if (Loadout->IsInitialized()) { return; }
    if (NextJoin == MAX_int32) { LoadoutError = TEXT("R36连接序号已耗尽"); return; }
    if (!MatchId.IsValid()) { MatchId = FGuid::NewGuid(); }
    const auto Result = Loadout->Initialize(MatchId, ++NextJoin);
    if (!Result.IsOk()) { LoadoutError = Result.Message; return; }
    if (!Player->GetGrowth()->Initialize()) { LoadoutError = TEXT("R36成长会话初始化失败"); return; }
    const auto* Profile = Cast<UY01R36Profile>(GetProfile());
    if (Profile && Profile->bEconomyEnabled)
    {
        FY01CommittedView View;
        if (!Loadout->ReadCommittedView(View)) { LoadoutError = TEXT("经济初始化缺少完整库存会话"); return; }
        TArray<FY01DataRef> DropMonsters;
        if (const auto* Table = Profile->Tables.Find(EY01TableKind::Monster); Table && *Table)
        { for (const auto& Pair : (*Table)->GetRowMap())
            { FY01DataRef Ref; Ref.Type = EY01TableKind::Monster; Ref.RowName = Pair.Key; DropMonsters.Add(Ref); } }
        const auto Economy = Player->GetEconomy()->Initialize(View.MatchId, View.JoinSequence, View.ConfigRevision,
            Profile->BuildCatalog(), Profile->ContentShopOffers, Profile->ContentRecipes, Profile->TalentChoice, DropMonsters);
        if (!Economy.IsOk()) { LoadoutError = Economy.Message; return; }
        const auto Delivery = Player->GetDropDelivery()->Initialize(Profile->DropFlightSeconds);
        if (!Delivery.IsOk()) { LoadoutError = Delivery.Message; return; }
    }
    if (Profile)
    {
        const auto Configured = Player->GetSourceEffects()->InitializeProfile(Profile->BuildCatalog(), Profile->BaseAttributeMappings,
            Profile->bEconomyEnabled ? Profile->InitialGold : 0, Profile->bEconomyEnabled ? Profile->BaseGoldPerSecond : 0);
        if (!Configured.IsOk()) { LoadoutError = Configured.Message; return; }
    }
    if (Profile && Profile->bEconomyEnabled)
    {
        EconomyPlayers.Add(Player);
        const auto Phase = GetFlowState()->GetWaveSnapshot().Phase;
        if (Phase == EY01MatchPhase::Preparing || Phase == EY01MatchPhase::Battle)
        {
            const auto Started = Player->GetIncome()->Start();
            if (!Started.IsOk()) { LoadoutError = Started.Message; }
        }
        Loadout->PublishCommittedView();
    }
    FString Fixture;
    if (!FParse::Value(FCommandLine::Get(), TEXT("R36Fixture="), Fixture)) { return; }
    if (Fixture != TEXT("controlled")) { LoadoutError = TEXT("R36Fixture仅接受显式controlled；不是自然购买"); return; }
    FString CountText; int32 Count = 0;
    if (!FParse::Value(FCommandLine::Get(), TEXT("R36Items="), CountText) || CountText.IsEmpty())
    { LoadoutError = TEXT("controlled需要显式R36Items正整数"); return; }
    for (const TCHAR C : CountText)
    {
        if (C < TEXT('0') || C > TEXT('9') || Count > (MAX_int32 - (C - TEXT('0'))) / 10)
        { LoadoutError = TEXT("R36Items必须为有效int32正整数；不截断或设置背包容量"); return; }
        Count = Count * 10 + C - TEXT('0');
    }
    FString Label; FParse::Value(FCommandLine::Get(), TEXT("R36Run="), Label);
    const bool S3 = Label.StartsWith(TEXT("R36-S3-Visible-")) || Label.StartsWith(TEXT("R36-S3-Presentation-"));
    const bool S4 = Label.StartsWith(TEXT("R36-S4-")) && Label.Len() > 7;
    if (Count == 0 && (S3 || S4) && Profile) { return; }
    if (Count <= 0 || !Profile) { LoadoutError = TEXT("controlled数量或R36Profile无效"); return; }
    TArray<FY01DataRef> Definitions = Profile->ContentEquipment; Definitions.Append(Profile->ContentSkills);
    if (Definitions.IsEmpty()) { LoadoutError = TEXT("controlled没有公开内容引用"); return; }
    // 隔离服务器夹具仅预置包内实例，不增加余额、不装配、不冒充购买。
    TArray<FY01ItemGrant> Grants; Grants.Reserve(Count);
    for (int32 I = 0; I < Count; ++I)
    {
        auto& Grant = Grants.AddDefaulted_GetRef(); Grant.ItemId = FGuid::NewGuid();
        Grant.Definition = Definitions[I % Definitions.Num()];
    }
    const auto Granted = Loadout->AddItems(Profile->BuildCatalog(), Grants);
    if (!Granted.IsOk()) { LoadoutError = Granted.Message; }
    UE_LOG(LogTemp, Display, TEXT("Y01R36 CONTROLLED Player=%s Join=%d Items=%d Result=%d NaturalPurchase=0"),
        *Player->GetName(), NextJoin, Count, static_cast<int32>(Granted.Code));
}
bool AY01R36GameMode::UseAutomaticWaves() const
{
    const auto* Profile = Cast<UY01R36Profile>(GetProfile());
    if (!Profile || Profile->bEvidenceMode) { return false; }
    FString Selection, Fixture, Scenario, Lifecycle, Burst;
    FParse::Value(FCommandLine::Get(), TEXT("R36Profile="), Selection);
    FParse::Value(FCommandLine::Get(), TEXT("R36Fixture="), Fixture);
    FParse::Value(FCommandLine::Get(), TEXT("R36Scenario="), Scenario);
    FParse::Value(FCommandLine::Get(), TEXT("R36Lifecycle="), Lifecycle);
    FParse::Value(FCommandLine::Get(), TEXT("R36Burst="), Burst);
    return (Selection.IsEmpty() || Selection == TEXT("content") || Selection == TEXT("profile")) && Fixture.IsEmpty()
        && (Scenario.IsEmpty() || Scenario == TEXT("natural")) && Lifecycle.IsEmpty() && Burst.IsEmpty()
        && !FParse::Param(FCommandLine::Get(), TEXT("Y01R27Verify"))
        && !FParse::Param(FCommandLine::Get(), TEXT("Y01DamageNetVerify"))
        && !AY01PlaygroundController::IsVerificationEnabled();
}
bool AY01R36GameMode::IsAutomaticPlayerReady(const AY01PlayerState* Player) const
{
    const auto* Profile = Cast<UY01R36Profile>(GetProfile()); FY01CommittedView View;
    return LoadoutError.IsEmpty() && Profile && IsPlayerPresent(Player) && Super::IsAutomaticPlayerReady(Player)
        && Player->GetLoadout()->ReadCommittedView(View) && Player->GetGrowth()->IsEnabled()
        && (!Profile->bEconomyEnabled || (EconomyPlayers.Contains(Player) && Player->GetEconomy()->IsInitialized()
            && Player->GetDropDelivery()->IsEnabled()));
}
bool AY01R36GameMode::CanStartWave() const
{ return LoadoutError.IsEmpty() && Super::CanStartWave(); }
bool AY01R36GameMode::ShouldEndOnPlayerDeath() const
{
    const auto* Profile = Cast<UY01R36Profile>(GetProfile());
    return !IsCoreDefenseEnabled() && (!Profile || !Profile->bEvidenceMode);
}
bool AY01R36GameMode::ShouldEndOnFinalWave() const
{
    const auto* Profile = Cast<UY01R36Profile>(GetProfile());
    return !Profile || !Profile->bEvidenceMode;
}
void AY01R36GameMode::OnMatchPhaseChanged(EY01MatchPhase Previous, EY01MatchPhase Current)
{
    Super::OnMatchPhaseChanged(Previous, Current);
    if (!HasAuthority()) { return; }
    if (Current == EY01MatchPhase::Ended)
    {
        GetWorld()->GetSubsystem<UY01KillRewards>()->EndSession();
        for (APlayerState* State : GetFlowState()->PlayerArray)
        { if (auto* Player = Cast<AY01PlayerState>(State))
            { Player->GetDropDelivery()->EndSession(EY01DeliveryCancel::Ended); Player->GetGrowth()->EndSession(); } }
    }
    const auto Players = EconomyPlayers.Array();
    for (const auto& Entry : Players)
    {
        auto* Player = Entry.Get(); if (!IsValid(Player)) { continue; }
        FY01RuntimeResult Result;
        if (Current == EY01MatchPhase::Preparing) { Result = Player->GetIncome()->Start(); }
        else if (Current == EY01MatchPhase::Ended)
        {
            // 失守只关闭业务；停止收入已使来源Check拒绝变更，已得贡献留到EndPlay清理。
            const auto* Core = GetMatchCore();
            if (IsCoreDefenseEnabled() && IsValid(Core) && Core->GetSnapshot().bFallen) { Result = Player->GetIncome()->Stop(); }
            else { Player->GetSourceEffects()->EndSession(); }
        }
        if (!Result.IsOk() && LoadoutError.IsEmpty()) { LoadoutError = Result.Message; }
        Player->GetLoadout()->PublishCommittedView();
    }
}
void AY01R36GameMode::Logout(AController* Exiting)
{
    auto* Player = Exiting ? Exiting->GetPlayerState<AY01PlayerState>() : nullptr;
    if (HasAuthority() && IsValid(Player))
    { Player->GetDropDelivery()->EndSession(EY01DeliveryCancel::Logout); Player->GetGrowth()->EndSession(); }
    if (HasAuthority() && IsValid(Player) && EconomyPlayers.Remove(Player))
    {
        const auto Stopped = Player->GetIncome()->Stop();
        if (!Stopped.IsOk() && LoadoutError.IsEmpty()) { LoadoutError = Stopped.Message; }
        Player->GetLoadout()->PublishCommittedView();
    }
    Super::Logout(Exiting);
}
void AY01R36Controller::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (!bPanelEnded) { Y01R36Trace::TickCoreVerification(this); }
    if (!bPanelEnded && FParse::Param(FCommandLine::Get(), TEXT("R36Trace")) && FPlatformTime::Seconds() >= NextTraceTime)
    {
        FString Label; FParse::Value(FCommandLine::Get(), TEXT("R36Run="), Label);
        const bool S3 = Label.StartsWith(TEXT("R36-S3-Visible-")) || Label.StartsWith(TEXT("R36-S3-Presentation-"));
        const bool S4 = Label.StartsWith(TEXT("R36-S4-")) && Label.Len() > 7;
        NextTraceTime = FPlatformTime::Seconds() + ((S3 || S4) ? .1 : .5);
        Trace = Y01R36Trace::Capture(this, true);
        for (const auto& Row : Trace) { UE_LOG(LogTemp, Display, TEXT("Y01R36_TRACE %s"), *Row); }
    }
#if !UE_SERVER
    FString Burst;
    if (!bPanelEnded && !bBurstSent && !HasAuthority() && IsLocalController() && GetLocalPlayer()
        && GetWorld() && !GetWorld()->bIsTearingDown
        && FParse::Value(FCommandLine::Get(), TEXT("R36Burst="), Burst) && Burst == TEXT("controlled"))
    {
        const auto* State = GetPlayerState<AY01PlayerState>();
        const auto* Flow = GetWorld()->GetGameState<AY01GameState>(); FY01CommittedView View;
        if (IsValid(State) && State->GetOwner() == this && IsValid(Flow) && State->GetLoadout()->ReadCommittedView(View))
        {
            const FString Record = Y01R36Trace::TryBurst(View, Flow->GetWaveSnapshot().Phase, true, bBurstSent,
                [this](const FY01ItemRequest& Request) { ServerItemRequest(Request); });
            if (!Record.IsEmpty())
            {
                Trace.Add(Record);
                UE_LOG(LogTemp, Display, TEXT("Y01R36_TRACE %s"), *Record);
            }
        }
    }
    if (bPanelEnded || !IsLocalController() || !GetLocalPlayer() || GetNetMode() == NM_DedicatedServer
        || !GetWorld() || GetWorld()->bIsTearingDown || !GetWorld()->GetGameViewport())
    {
        if (Panel) { Panel->RemoveFromParent(); Panel = nullptr; }
        return;
    }
    if (!Panel) { Panel = CreateWidget<UY01R36Panel>(this); }
    if (Panel && !Panel->IsInViewport()) { Panel->AddToViewport(5); }
#endif
}
void AY01R36Controller::EndPlay(const EEndPlayReason::Type Reason)
{
    bPanelEnded = true;
    if (Panel) { Panel->RemoveFromParent(); Panel = nullptr; }
    Super::EndPlay(Reason);
}
void AY01R36Controller::ServerItemRequest_Implementation(FY01ItemRequest Request)
{
    auto* State = GetPlayerState<AY01PlayerState>();
    if (!HasAuthority() || !GetWorld()->GetAuthGameMode<AY01R36GameMode>() || !IsValid(State) || State->GetOwner() != this) { return; }
    State->GetLoadout()->SubmitRequest(this, Request);
    if (FParse::Param(FCommandLine::Get(), TEXT("R36Trace")))
    {
        const FString Record = Y01R36Trace::RequestRecord(this);
        if (!Record.IsEmpty())
        {
            // 内存只保留当前观测；逐请求证据写日志，不累积无限历史。
            Trace = {Record};
            UE_LOG(LogTemp, Display, TEXT("Y01R36_TRACE %s"), *Record);
        }
    }
}
void AY01R36Controller::ServerPlaygroundAction_Implementation(int32 Action)
{
    if (Action == 8 || Action == 9) { return; }
    Super::ServerPlaygroundAction_Implementation(Action);
}
