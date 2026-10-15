#if WITH_EDITOR && WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
#include "Playground/Y01R36Assets.h"
#include "Playground/Y01R36Runtime.h"
#include "Playground/Y01R36UI.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01EconomyComponent.h"
#include "Editor.h"
#include "FileHelpers.h"
#include "PlayInEditorDataTypes.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "DataTableEditorUtils.h"
#include "Engine/GameViewportClient.h"
#include "Engine/GameInstance.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/MapBuildDataRegistry.h"
#include "EngineUtils.h"
#include "GameFramework/WorldSettings.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UnrealClient.h"
#include "DynamicRHI.h"

namespace Y01MainPIE
{
using K = EY01TableKind;
const FString MainMap = TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/S4_AddedRows_20261001_01/Maps/L_Y01R36");
FY01DataRef Ref(K Kind, FName Name) { FY01DataRef R; R.Type = Kind; R.RowName = Name; return R; }

bool CanPrepare(bool bIsMainMap, bool bMainDirty, bool bRelatedDirty)
{
    return bIsMainMap && !bMainDirty && !bRelatedDirty;
}
bool ShouldSkipEvidence(bool bExplicitEvidence)
{
    return !bExplicitEvidence;
}
bool CanUseEvidence(bool bPathIsFile, bool bHasEntries)
{
    return !bPathIsFile && !bHasEntries;
}

FString FindDirtyLevelPackage(const UWorld* World)
{
    if (!World) { return FString(); }
    // 仅查询，不调用可能标脏构建数据的通用保存候选收集接口。
    TArray<const ULevel*> Levels;
    Levels.Add(World->PersistentLevel);
    for (const auto* Level : World->GetLevels()) { Levels.AddUnique(Level); }
    for (const auto* Streaming : World->GetStreamingLevels())
    {
        if (Streaming) { Levels.AddUnique(Streaming->GetLoadedLevel()); }
    }
    for (const auto* Level : Levels)
    {
        if (!Level) { continue; }
        TArray<UPackage*> Packages = Level->GetLoadedExternalObjectPackages();
        Packages.Add(Level->GetOutermost());
        if (Level->MapBuildData) { Packages.Add(Level->MapBuildData->GetOutermost()); }
        for (const auto* Package : Packages)
        {
            if (Package && Package->IsDirty()) { return Package->GetName(); }
        }
    }
    return FString();
}

class FRepeatSession final : public IAutomationLatentCommand
{
public:
    FRepeatSession(FAutomationTestBase* InTest, const FString& InOutput)
        : Test(InTest), Output(InOutput), ProcessId(FPlatformProcess::GetCurrentProcessId()) {}
    virtual bool Update() override;
private:
    bool Prepare();
    bool SaveFixture(UObject* Asset);
    bool EditValues(int32 Round);
    bool StartPIE();
    bool Inspect();
    bool CaptureProfile(UY01R36Profile* Profile, const FString& Label);
    bool WriteJson(const FString& Name, const TSharedRef<FJsonObject>& Object);
    void RecordView(const FString& Label, AY01R36Controller* PC, const FY01CommittedView& View);
    bool Check(bool Condition, const FString& Message) { return Test->TestTrue(Message, Condition); }
    void Transition(int32 Next) { Stage = Next; StageStart = FPlatformTime::Seconds(); }
    bool NoPIE() const;
    FAutomationTestBase* Test;
    FString Output, FixtureRoot, Screenshot;
    uint32 ProcessId;
    int32 Stage = 0, Session = 1;
    double StageStart = FPlatformTime::Seconds();
    TStrongObjectPtr<UY01R36Profile> Fixture;
    TStrongObjectPtr<ULevelEditorPlaySettings> Settings;
    TMap<K, UDataTable*> OriginalTables;
    TWeakObjectPtr<UWorld> PreviousWorld;
    TWeakObjectPtr<UGameInstance> PreviousInstance;
    FGuid PreviousMatch, PreviousConnection, PreviousItem;
    FY01ItemRequest PreviousRequest;
};

bool FRepeatSession::WriteJson(const FString& Name, const TSharedRef<FJsonObject>& Object)
{
    Object->SetNumberField(TEXT("editorPid"), ProcessId);
    Object->SetNumberField(TEXT("session"), Session);
    Object->SetNumberField(TEXT("monotonicSeconds"), FPlatformTime::Seconds());
    FString Json;
    return Check(FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Json))
        && FFileHelper::SaveStringToFile(Json, *(Output / Name), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM), TEXT("保存证据：") + Name);
}

bool FRepeatSession::NoPIE() const
{
    if (GEditor->PlayWorld || GEditor->IsPlaySessionRequestQueued()) { return false; }
    for (const auto& Context : GEngine->GetWorldContexts())
    { if (Context.WorldType == EWorldType::PIE) { return false; } }
    return true;
}

bool FRepeatSession::SaveFixture(UObject* Asset)
{
    if (!Check(NoPIE(), TEXT("仅在停止PIE后保存夹具")) || !Asset
        || !Check(Asset->GetOutermost()->GetName().StartsWith(FixtureRoot + TEXT("/")), TEXT("禁止保存日常或外部资产"))) { return false; }
    Asset->MarkPackageDirty();
    FSavePackageArgs Args; Args.TopLevelFlags = RF_Public | RF_Standalone; Args.SaveFlags = SAVE_NoError;
    const auto Path = FPackageName::LongPackageNameToFilename(Asset->GetOutermost()->GetName(), FPackageName::GetAssetPackageExtension());
    return Check(UPackage::SavePackage(Asset->GetOutermost(), Asset, *Path, Args), TEXT("保存隔离夹具：") + Path);
}

bool FRepeatSession::EditValues(int32 Round)
{
    if (!Check(NoPIE(), TEXT("改值前PIE世界已结束"))) { return false; }
    const K Kinds[] = {K::Character, K::ShopOffer, K::SourceEffect};
    for (const K Kind : Kinds)
    {
        auto* Table = Fixture->Tables[Kind].Get();
        if (!Check(Table == OriginalTables[Kind], TEXT("复用同编辑器原DT对象，不重载"))) { return false; }
        FDataTableEditorUtils::BroadcastPreChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
        Table->Modify();
    }
    auto* Character = Fixture->Tables[K::Character]->FindRow<FY01CharacterRow>(TEXT("Default"), TEXT("MainPIE"));
    auto* Blessing = Fixture->Tables[K::ShopOffer]->FindRow<FY01ShopOfferRow>(TEXT("EvidenceBlessing"), TEXT("MainPIE"));
    auto* Equipment = Fixture->Tables[K::ShopOffer]->FindRow<FY01ShopOfferRow>(TEXT("EvidenceEquipment"), TEXT("MainPIE"));
    auto* Effect = Fixture->Tables[K::SourceEffect]->FindRow<FY01SourceEffectRow>(TEXT("Default"), TEXT("MainPIE"));
    if (!Check(Character && Blessing && Equipment && Effect, TEXT("夹具目标行齐全"))) { return false; }
    Character->Health = Round == 1 ? 401 : 733; Character->AttackDamage = Round == 1 ? 41 : 67;
    Character->Strength = 0;
    Blessing->Price = Round == 1 ? 23 : 47; Equipment->Price = Round == 1 ? 31 : 59;
    Effect->Magnitude = Round == 1 ? 7 : 19;
    for (const K Kind : Kinds)
    {
        auto* Table = Fixture->Tables[Kind].Get();
        FDataTableEditorUtils::BroadcastPostChange(Table, FDataTableEditorUtils::EDataTableChangeInfo::RowData);
        if (!SaveFixture(Table)) { return false; }
    }
    return CaptureProfile(Fixture.Get(), FString::Printf(TEXT("fixture-saved-%d"), Round));
}

bool FRepeatSession::CaptureProfile(UY01R36Profile* Profile, const FString& Label)
{
    if (!Check(Profile != nullptr, TEXT("捕获真实绑定Profile"))) { return false; }
    auto Root = MakeShared<FJsonObject>();
    Root->SetStringField(TEXT("profile"), Profile->GetPathName());
    auto Properties = MakeShared<FJsonObject>();
    for (TFieldIterator<FProperty> It(Profile->GetClass()); It; ++It)
    {
        FString Value; It->ExportText_InContainer(0, Value, Profile, Profile, Profile, PPF_None);
        Properties->SetStringField(It->GetName(), Value);
    }
    Root->SetObjectField(TEXT("propertyText"), Properties);
    TArray<TSharedPtr<FJsonValue>> Tables;
    for (const auto& Pair : Profile->Tables)
    {
        const auto* Table = Pair.Value.Get();
        if (!Check(Table != nullptr, TEXT("绑定表非空"))) { return false; }
        auto Entry = MakeShared<FJsonObject>();
        Entry->SetStringField(TEXT("kind"), StaticEnum<K>()->GetNameStringByValue(static_cast<int64>(Pair.Key)));
        Entry->SetStringField(TEXT("asset"), Table->GetPathName());
        Entry->SetStringField(TEXT("rowStruct"), Table->GetRowStruct()->GetPathName());
        Entry->SetBoolField(TEXT("dirty"), Table->GetOutermost()->IsDirty());
        TArray<TSharedPtr<FJsonValue>> Rows;
        // 本引擎的DT JSON访问器在结构体数组上断言；只读取字段原文，不调用该导出器。
        for (const auto& Row : Table->GetRowMap())
        {
            auto RowJson = MakeShared<FJsonObject>(); RowJson->SetStringField(TEXT("rowName"), Row.Key.ToString());
            auto Fields = MakeShared<FJsonObject>();
            for (TFieldIterator<FProperty> It(Table->GetRowStruct()); It; ++It)
            {
                FString Value; It->ExportText_InContainer(0, Value, Row.Value, Row.Value, nullptr, PPF_None);
                auto Field = MakeShared<FJsonObject>();
                Field->SetStringField(TEXT("cppType"), It->GetCPPType()); Field->SetStringField(TEXT("exportText"), Value);
                Fields->SetObjectField(It->GetName(), Field);
            }
            RowJson->SetObjectField(TEXT("fields"), Fields); Rows.Add(MakeShared<FJsonValueObject>(RowJson));
        }
        Entry->SetArrayField(TEXT("tableRows"), Rows); Tables.Add(MakeShared<FJsonValueObject>(Entry));
    }
    Root->SetArrayField(TEXT("tables"), Tables);
    return WriteJson(Label + TEXT(".json"), Root);
}

bool FRepeatSession::Prepare()
{
    if (!Check(NoPIE(), TEXT("验收不能接管已有PIE"))) { return false; }
    auto* EditorWorld = GEditor->GetEditorWorldContext().World();
    const FString DirtyLevelPackage = FindDirtyLevelPackage(EditorWorld);
    // 必须先拒绝未保存编辑，再生成任何夹具；不自动保存，也不清除用户的脏标记。
    if (!Check(CanPrepare(EditorWorld && EditorWorld->GetOutermost()->GetName() == MainMap,
        EditorWorld && EditorWorld->GetOutermost()->IsDirty(), !DirtyLevelPackage.IsEmpty()),
        TEXT("正式主关卡必须无未保存编辑，拒绝生成或切图；脏关卡包：") + DirtyLevelPackage)) { return false; }
    FixtureRoot = TEXT("/Game/Developers/Y01PlaygroundTests/R36/Runs/MainPIE_") + FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Y01R36Editor::FGeneration Generated;
    const bool GeneratedOK = Y01R36Editor::Generate(FixtureRoot, Generated, false, false, true);
    if (!Check(GeneratedOK, TEXT("复用旧生成器创建独立自动化夹具：") + Generated.Error)) { return false; }
    Fixture.Reset(Generated.Profiles[1].Get());
    Fixture->InitialGold = 1000; Fixture->BaseGoldPerSecond = 0;
    Fixture->ContentShopOffers = {Ref(K::ShopOffer, TEXT("EvidenceBlessing")), Ref(K::ShopOffer, TEXT("EvidenceEquipment"))};
    auto* Equipment = Fixture->Tables[K::Equipment]->FindRow<FY01EquipmentRow>(TEXT("EvidenceEquipment"), TEXT("MainPIE"));
    auto* Blessing = Fixture->Tables[K::ShopOffer]->FindRow<FY01ShopOfferRow>(TEXT("EvidenceBlessing"), TEXT("MainPIE"));
    if (!Check(Equipment && Blessing, TEXT("隔离经济夹具有效"))) { return false; }
    Equipment->EffectIds = {Ref(K::SourceEffect, TEXT("Default"))}; Equipment->GrowthRuleIds.Reset();
    Blessing->EffectIds = Equipment->EffectIds; Blessing->GrowthRuleIds.Reset();
    for (const auto& Pair : Fixture->Tables) { OriginalTables.Add(Pair.Key, Pair.Value.Get()); }
    if (!SaveFixture(Fixture.Get()) || !SaveFixture(Fixture->Tables[K::Equipment]) || !EditValues(1)) { return false; }
    // 旧生成器面向commandlet，保存后的临时世界仍为Standalone；切图前仅释放本次夹具的保活标记。
    // 不主动GC，也不在两轮PIE之间重载地图、DT或清缓存。
    auto* GeneratedWorld = FindObject<UWorld>(nullptr, *(FixtureRoot + TEXT("/Maps/L_Y01R36.L_Y01R36")));
    if (!Check(GeneratedWorld && GeneratedWorld != GEditor->GetEditorWorldContext().World(), TEXT("仅释放本次生成的非活动夹具世界"))) { return false; }
    GeneratedWorld->ClearFlags(RF_Standalone);
    if (!Check(UEditorLoadingAndSavingUtils::LoadMap(FixtureRoot + TEXT("/Maps/L_Y01R36")) != nullptr, TEXT("打开隔离夹具地图"))) { return false; }
    Settings.Reset(DuplicateObject<ULevelEditorPlaySettings>(GetDefault<ULevelEditorPlaySettings>(), GetTransientPackage()));
    Settings->SetPlayNumberOfClients(1); Settings->SetRunUnderOneProcess(true); Settings->SetPlayNetMode(PIE_Standalone);
    Settings->NewWindowWidth = 1280; Settings->NewWindowHeight = 720; Settings->CenterNewWindow = true;
    Settings->GameGetsMouseControl = false;
    auto Meta = MakeShared<FJsonObject>(); Meta->SetStringField(TEXT("fixtureRoot"), FixtureRoot);
    Meta->SetStringField(TEXT("mainMap"), MainMap); Meta->SetStringField(TEXT("rhi"), GDynamicRHI->GetName());
    Meta->SetStringField(TEXT("commandLine"), FCommandLine::Get());
    return WriteJson(TEXT("run.json"), Meta);
}

bool FRepeatSession::StartPIE()
{
    if (!Check(NoPIE(), TEXT("同进程下一轮开始前无PIE世界"))) { return false; }
    FRequestPlaySessionParams Params;
    Params.SessionDestination = EPlaySessionDestinationType::InProcess;
    Params.WorldType = EPlaySessionWorldType::PlayInEditor;
    Params.EditorPlaySettings = Settings.Get();
    GEditor->RequestPlaySession(Params);
    Test->AddInfo(FString::Printf(TEXT("MAIN_PIE REQUEST PID=%u Session=%d InProcess PlayInEditor"), ProcessId, Session));
    Transition(1); return true;
}

void FRepeatSession::RecordView(const FString& Label, AY01R36Controller* PC, const FY01CommittedView& View)
{
    auto Root = MakeShared<FJsonObject>();
    const auto* World = PC->GetWorld();
    Root->SetStringField(TEXT("world"), World->GetPathName());
    Root->SetStringField(TEXT("worldType"), TEXT("PIE"));
    Root->SetStringField(TEXT("gameInstance"), World->GetGameInstance()->GetPathName());
    Root->SetStringField(TEXT("phase"), StaticEnum<EY01MatchPhase>()->GetNameStringByValue(static_cast<int64>(World->GetGameState<AY01GameState>()->GetWaveSnapshot().Phase)));
    Root->SetStringField(TEXT("gameMode"), World->GetAuthGameMode()->GetClass()->GetPathName());
    Root->SetStringField(TEXT("matchId"), View.MatchId.ToString());
    Root->SetStringField(TEXT("connectionId"), View.ConnectionId.ToString());
    Root->SetNumberField(TEXT("joinSequence"), View.JoinSequence);
    Root->SetNumberField(TEXT("expectedSeq"), static_cast<double>(View.ExpectedSeq));
    Root->SetNumberField(TEXT("gold"), View.Gold); Root->SetNumberField(TEXT("health"), View.Health);
    Root->SetNumberField(TEXT("attackDamage"), View.Stats.AttackDamage); Root->SetNumberField(TEXT("itemCount"), View.Items.Num());
    Root->SetNumberField(TEXT("ascAttackDamage"), PC->GetPlayerState<AY01PlayerState>()->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()));
    TArray<TSharedPtr<FJsonValue>> Offers;
    for (const auto& Offer : View.ShopOffers)
    {
        auto Item = MakeShared<FJsonObject>(); Item->SetStringField(TEXT("id"), Offer.OfferId.ToString());
        Item->SetNumberField(TEXT("price"), Offer.Price); Item->SetBoolField(TEXT("purchased"), Offer.bPurchased);
        Offers.Add(MakeShared<FJsonValueObject>(Item));
    }
    Root->SetArrayField(TEXT("offers"), Offers);
    WriteJson(Label + TEXT(".json"), Root);
}

bool FRepeatSession::Inspect()
{
    auto* World = GEditor->PlayWorld.Get();
    if (!Check(World && World->WorldType == EWorldType::PIE, TEXT("实际PIE世界而非-game"))) { return false; }
    auto* Mode = World->GetAuthGameMode<AY01R36GameMode>();
    auto* PC = Cast<AY01R36Controller>(World->GetFirstPlayerController());
    auto* Player = PC ? PC->GetPlayerState<AY01PlayerState>() : nullptr;
    if (!Check(Mode && PC && Player, TEXT("实际GameMode/Controller/PlayerState"))) { return false; }
    if (!Check(Mode->IsMatchConfigured() && Mode->GetConfigurationError().IsEmpty() && Mode->GetLoadoutError().IsEmpty(), TEXT("运行时配置与经济初始化成功"))) { return false; }
    UY01R36Profile* Content = nullptr; int32 Count = 0;
    for (TActorIterator<AY01R36Config> It(World); It; ++It) { ++Count; Content = It->Content.Get(); }
    if (!Check(Count == 1 && Content && Mode->GetProfile() == Content, TEXT("无R36Profile参数正常Play取Content"))) { return false; }
    if (!Check(PC->GetPanel() && PC->GetPanel()->IsInViewport() && World->GetGameViewport(), TEXT("真实PIE视口与R36 UI"))) { return false; }
    Check(FPlatformProcess::GetCurrentProcessId() == ProcessId, TEXT("全部轮次同一编辑器PID"));
    Check(World != PreviousWorld.Get() && World->GetGameInstance() != PreviousInstance.Get(), TEXT("新PIE世界及GameInstance"));
    auto* Loadout = Player->GetLoadout(); FY01CommittedView Before;
    if (!Check(Loadout->ReadCommittedView(Before), TEXT("真实已提交运行快照"))) { return false; }
    RecordView(FString::Printf(TEXT("session-%d-before"), Session), PC, Before);
    if (Session == 3)
    {
        Check(Content->GetOutermost()->GetName().StartsWith(FPackageName::GetLongPackagePath(FPackageName::GetLongPackagePath(MainMap)) + TEXT("/Content/")), TEXT("日常地图实际Content绑定"));
        Check(Content->Tables.Num() == 22, TEXT("日常沿用现有22表"));
        Check(!Content->GetOutermost()->IsDirty(), TEXT("只读Profile未标脏"));
        for (const auto& Pair : Content->Tables) { Check(!Pair.Value->GetOutermost()->IsDirty(), TEXT("日常DT未标脏")); }
        Check(!GEditor->GetEditorWorldContext().World()->GetOutermost()->IsDirty(), TEXT("日常地图未标脏"));
        return CaptureProfile(Content, TEXT("daily-profile-tableRows"));
    }
    if (!Check(Content == Fixture.Get(), TEXT("两轮均复用原Content对象"))) { return false; }
    // 正常Play停在Waiting；购买必须通过既有开始入口进入Preparing，不能伪造阶段。
    if (!Check(Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Waiting, TEXT("每轮正常Play从Waiting开始"))
        || !Check(Mode->RequestAdvance(PC), TEXT("调用真实开始入口"))
        || !Check(Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Preparing, TEXT("真实进入允许购买的准备阶段"))) { return false; }
    const float HP = Session == 1 ? 401.f : 733.f, Attack = Session == 1 ? 41.f : 67.f, Magnitude = Session == 1 ? 7.f : 19.f;
    const int32 Price = Session == 1 ? 23 : 47, EquipmentPrice = Session == 1 ? 31 : 59;
    Check(FMath::IsNearlyEqual(Before.Health, HP), TEXT("保存后的角色HP生效"));
    Check(FMath::IsNearlyEqual(Before.Stats.AttackDamage, Attack), TEXT("保存后的角色攻击生效"));
    Check(Before.Gold == 1000 && Before.Items.IsEmpty() && Before.ExpectedSeq == 1, TEXT("金币/库存/请求序列每局重新初始化"));
    Check(Before.MatchId != PreviousMatch && Before.ConnectionId != PreviousConnection, TEXT("会话及连接身份不残留"));
    Check(Before.JoinSequence == 1, TEXT("每局首位玩家身份重新开始"));
    const auto* Offer = Before.ShopOffers.FindByPredicate([](const auto& Row) { return Row.OfferId == TEXT("EvidenceBlessing"); });
    if (!Check(Offer && Offer->Price == Price && !Offer->bPurchased, TEXT("新价格与限购状态刷新"))) { return false; }
    if (Session == 2)
    {
        const auto Stale = Loadout->SubmitRequest(PC, PreviousRequest);
        Check(Stale.Code == EY01ItemError::Session, TEXT("上一局请求身份被拒绝"));
        FY01ItemInstance Missing;
        Check(!Loadout->FindItem(PreviousItem, Missing).IsOk(), TEXT("上一局物品身份不存在"));
    }
    auto Request = [&](EY01ItemOperation Op)
    {
        FY01CommittedView Current; Loadout->ReadCommittedView(Current);
        FY01ItemRequest R; R.MatchId = Current.MatchId; R.JoinSequence = Current.JoinSequence;
        R.ConfigRevision = Current.ConfigRevision; R.Seq = Current.ExpectedSeq; R.Operation = Op; return R;
    };
    auto Purchase = Request(EY01ItemOperation::Purchase); Purchase.Offer = Ref(K::ShopOffer, TEXT("EvidenceBlessing"));
    const auto PurchaseResult = Loadout->SubmitRequest(PC, Purchase);
    if (!Check(PurchaseResult.IsOk(), FString::Printf(TEXT("真实经济事务购买公共效果 Code=%d %s"), static_cast<int32>(PurchaseResult.Code), *PurchaseResult.Message))) { return false; }
    FY01CommittedView After; Loadout->ReadCommittedView(After);
    Check(After.Gold == Before.Gold - Price, TEXT("真实扣款采用该轮DT价格"));
    Check(FMath::IsNearlyEqual(Player->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), Attack + Magnitude), TEXT("真实ASC已应用该轮公共效果Magnitude"));
    RecordView(FString::Printf(TEXT("session-%d-purchase"), Session), PC, After);
    auto Again = Request(EY01ItemOperation::Purchase); Again.Offer = Purchase.Offer;
    Check(Loadout->SubmitRequest(PC, Again).Code == EY01ItemError::PurchaseLimit, TEXT("本局第二次真实购买受限"));
    Loadout->ReadCommittedView(After); Check(After.Gold == Before.Gold - Price, TEXT("限购拒绝未重复扣款"));
    auto BuyItem = Request(EY01ItemOperation::Purchase); BuyItem.Offer = Ref(K::ShopOffer, TEXT("EvidenceEquipment"));
    const auto Bought = Loadout->SubmitRequest(PC, BuyItem);
    if (!Check(Bought.IsOk() && Bought.ProductItemId.IsValid(), TEXT("真实购买装备入库存"))) { return false; }
    Loadout->ReadCommittedView(After);
    Check(After.Gold == 1000 - Price - EquipmentPrice && After.Items.Num() == 1, TEXT("装备购买采用新价并生成唯一物品"));
    FY01ItemInstance Item;
    if (!Check(Loadout->FindItem(Bought.ProductItemId, Item).IsOk(), TEXT("购买实例真实存在"))) { return false; }
    Check(Item.Effects.Num() == 1 && Item.Effects[0].Magnitude == Magnitude, TEXT("装备快照也取本轮公共效果"));
    auto Equip = Request(EY01ItemOperation::Equip); Equip.ItemId = Item.ItemId; Equip.ExpectedEntityRevision = Item.EntityRevision; Equip.Slot = 0;
    if (!Check(Loadout->SubmitRequest(PC, Equip).IsOk(), TEXT("真实装备事务"))) { return false; }
    Check(FMath::IsNearlyEqual(Player->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), Attack + 2 * Magnitude), TEXT("装备与直购公共效果均实际生效"));
    Loadout->ReadCommittedView(After); RecordView(FString::Printf(TEXT("session-%d-equipped"), Session), PC, After);
    PreviousWorld = World; PreviousInstance = World->GetGameInstance(); PreviousMatch = Before.MatchId;
    PreviousConnection = Before.ConnectionId; PreviousRequest = Purchase; PreviousItem = Item.ItemId;
    return true;
}

bool FRepeatSession::Update()
{
    if (Stage == 0)
    {
        if (!Prepare() || !StartPIE()) { return true; }
        return false;
    }
    if (FPlatformTime::Seconds() - StageStart > 60)
    {
        Test->AddError(FString::Printf(TEXT("MAIN_PIE 超时 PID=%u Session=%d Stage=%d"), ProcessId, Session, Stage));
        if (GEditor->PlayWorld) { GEditor->RequestEndPlayMap(); }
        return true;
    }
    if (Stage == 1)
    {
        if (!GEditor->PlayWorld || !GEditor->PlayWorld->HasBegunPlay() || GEditor->PlayWorld->GetTimeSeconds() < 3.f) { return false; }
        if (!Inspect()) { GEditor->RequestEndPlayMap(); Transition(9); return false; }
        Screenshot = Output / FString::Printf(TEXT("session-%d-PIE.png"), Session);
        FScreenshotRequest::RequestScreenshot(Screenshot, true, false);
        Transition(2); return false;
    }
    if (Stage == 2)
    {
        if (IFileManager::Get().FileSize(*Screenshot) < 128) { return false; }
        Check(GEditor->PlayWorld && GEditor->PlayWorld->GetGameViewport()->Viewport->GetSizeXY().GetMin() > 0, TEXT("D3D12真实PIE截图已落盘"));
        GEditor->RequestEndPlayMap(); Transition(3); return false;
    }
    if (Stage == 9) { return NoPIE(); }
    if (!NoPIE()) { return false; }
    Test->AddInfo(FString::Printf(TEXT("MAIN_PIE STOPPED PID=%u Session=%d"), ProcessId, Session));
    if (Session == 3)
    {
        auto Result = MakeShared<FJsonObject>(); Result->SetBoolField(TEXT("completed"), !Test->HasAnyErrors());
        Result->SetBoolField(TEXT("dailyReadOnly"), true); Result->SetNumberField(TEXT("sessions"), 3);
        WriteJson(TEXT("pie-result.json"), Result); return true;
    }
    ++Session;
    if (Session == 2 && !EditValues(2)) { return true; }
    if (Session == 3 && !Check(UEditorLoadingAndSavingUtils::LoadMap(MainMap) != nullptr, TEXT("只读打开正式日常主关卡"))) { return true; }
    if (!StartPIE()) { return true; }
    return false;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01MainPIEDirtyMapGuardTest, "Y01.Framework.MainPIE.Guard.DirtyMap",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FY01MainPIEDirtyMapGuardTest::RunTest(const FString&)
{
    TestTrue(TEXT("干净正式地图允许准备"), Y01MainPIE::CanPrepare(true, false, false));
    TestFalse(TEXT("正式地图未保存时拒绝"), Y01MainPIE::CanPrepare(true, true, false));
    TestFalse(TEXT("主包干净但关卡相关包未保存时拒绝"), Y01MainPIE::CanPrepare(true, false, true));
    TestFalse(TEXT("缺少世界或非正式地图拒绝"), Y01MainPIE::CanPrepare(false, false, false));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01MainPIEOptionalEvidenceGuardTest, "Y01.Framework.MainPIE.Guard.OptionalEvidence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FY01MainPIEOptionalEvidenceGuardTest::RunTest(const FString&)
{
    FString Output;
    TestTrue(TEXT("普通批量测试无参数应跳过"), Y01MainPIE::ShouldSkipEvidence(
        FParse::Value(TEXT("-d3d12"), TEXT("MainPIEEvidence="), Output)));
    TestFalse(TEXT("显式参数不可跳过严格验证"), Y01MainPIE::ShouldSkipEvidence(
        FParse::Value(TEXT("-MainPIEEvidence=Saved/Fresh"), TEXT("MainPIEEvidence="), Output)));
    TestFalse(TEXT("显式空值也不可冒充未启用"), Y01MainPIE::ShouldSkipEvidence(
        FParse::Value(TEXT("-MainPIEEvidence=\"\""), TEXT("MainPIEEvidence="), Output)));
    TestTrue(TEXT("解析保留显式空值供入口拒绝"), Output.IsEmpty());
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01MainPIEFreshEvidenceGuardTest, "Y01.Framework.MainPIE.Guard.FreshEvidence",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter)
bool FY01MainPIEFreshEvidenceGuardTest::RunTest(const FString&)
{
    TestTrue(TEXT("全新或空证据目录允许"), Y01MainPIE::CanUseEvidence(false, false));
    TestFalse(TEXT("已有截图、结果或子目录时拒绝，不删除旧证据"), Y01MainPIE::CanUseEvidence(false, true));
    TestFalse(TEXT("证据路径是文件时拒绝"), Y01MainPIE::CanUseEvidence(true, false));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01MainRepeatPIETest, "Y01.Framework.MainPIE.RepeatAndDailyReadOnly",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ProductFilter | EAutomationTestFlags::NonNullRHI)
bool FY01MainRepeatPIETest::RunTest(const FString&)
{
    FString Output;
    const bool bExplicitEvidence = FParse::Value(FCommandLine::Get(), TEXT("MainPIEEvidence="), Output);
    if (Y01MainPIE::ShouldSkipEvidence(bExplicitEvidence))
    {
        AddInfo(TEXT("MAIN_PIE SKIP: MainPIEEvidence 未提供；跳过可视验收，不生成夹具、不切图。"));
        return true;
    }
    if (!TestTrue(TEXT("显式MainPIEEvidence不可为空"), !Output.TrimStartAndEnd().IsEmpty())) { return false; }
    Output = FPaths::ConvertRelativePathToFull(Output);
    if (!TestTrue(TEXT("证据限定项目Saved目录"), FPaths::IsUnderDirectory(Output, FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir())))
        || !TestTrue(TEXT("必须为真实D3D12编辑器"), GEditor && GDynamicRHI && FString(GDynamicRHI->GetName()) == TEXT("D3D12"))) { return false; }
    FString Selection;
    if (!TestFalse(TEXT("不得依赖R36Profile命令行选择"), FParse::Value(FCommandLine::Get(), TEXT("R36Profile="), Selection))) { return false; }
    FString Startup, Game;
    GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("EditorStartupMap"), Startup, GEngineIni);
    GConfig->GetString(TEXT("/Script/EngineSettings.GameMapsSettings"), TEXT("GameDefaultMap"), Game, GEngineIni);
    if (!TestEqual(TEXT("编辑器正式入口"), Startup, Y01MainPIE::MainMap) || !TestEqual(TEXT("游戏正式入口"), Game, Y01MainPIE::MainMap)) { return false; }
    bool bHasEntries = false;
    auto& Files = IFileManager::Get();
    const bool bReadable = !Files.DirectoryExists(*Output) || Files.IterateDirectory(*Output,
        [&bHasEntries](const TCHAR*, bool) { bHasEntries = true; return true; });
    if (!TestTrue(TEXT("证据目录可枚举"), bReadable)
        || !TestTrue(TEXT("证据目录必须为空，禁止覆盖或接受旧截图/结果"),
            Y01MainPIE::CanUseEvidence(Files.FileExists(*Output), bHasEntries))
        || !TestTrue(TEXT("创建证据目录成功"), Files.MakeDirectory(*Output, true))) { return false; }
    ADD_LATENT_AUTOMATION_COMMAND(Y01MainPIE::FRepeatSession(this, Output));
    return true;
}
#endif
