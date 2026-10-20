#include "Playground/Y01R36Verification.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01IncomeComponent.h"
#include "EngineUtils.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/SecureHash.h"
#include "UObject/UnrealType.h"
#include "Playground/Y01R36UI.h"
#include "Blueprint/WidgetTree.h"
#include "Blueprint/SlateBlueprintLibrary.h"
#include "Engine/World.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Framework/Application/SlateApplication.h"
#include "Fonts/FontMeasure.h"
#include "Rendering/SlateRenderer.h"
#include "Components/CanvasPanelSlot.h"
#include "Combat/Y01Monster.h"
#include "Combat/Y01Core.h"
#include "Player/Y01ReviveComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/WidgetComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
void EffectFields(FJsonObject& Object, const TArray<FY01ItemEffect>& Effects)
{
    double Attack = 0, Income = 0; TArray<TSharedPtr<FJsonValue>> Details;
    for (const auto& E : Effects)
    {
        auto Detail = MakeShared<FJsonObject>(); Detail->SetStringField(TEXT("id"), E.EffectId.ToString());
        Detail->SetNumberField(TEXT("kind"), static_cast<int32>(E.Kind)); Detail->SetNumberField(TEXT("magnitude"), E.Magnitude);
        Detail->SetNumberField(TEXT("sourceAttribute"), static_cast<int32>(E.SourceAttribute));
        Detail->SetNumberField(TEXT("targetAttribute"), static_cast<int32>(E.TargetAttribute)); Detail->SetNumberField(TEXT("amount"), E.Amount);
        Details.Add(MakeShared<FJsonValueObject>(Detail));
        switch (E.Kind)
        {
        case EY01SourceEffectKind::AttackFlat: Attack += E.Magnitude; break;
        case EY01SourceEffectKind::GoldPerSecond: Income += E.Magnitude; break;
        case EY01SourceEffectKind::AttributeFlat:
            if (E.TargetAttribute == EY01Attribute::AttackDamage) { Attack += E.Amount; }
            else if (E.TargetAttribute == EY01Attribute::GoldPerSecond) { Income += E.Amount; }
            break;
        case EY01SourceEffectKind::MappingCoefficientAdd: break;
        default: break;
        }
    }
    // 平面字段仅表示直接贡献；映射保留完整明细，不能冒充固定收入。
    Object.SetNumberField(TEXT("attackEffect"), Attack); Object.SetNumberField(TEXT("incomeEffect"), Income);
    Object.SetArrayField(TEXT("effects"), Details);
}
FString Encode(const TSharedRef<FJsonObject>& Object)
{
    FString Text;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
    return Text;
}
TSharedPtr<FJsonObject> EconomyRecord(const AY01PlayerState* Player, const FY01CommittedView& View)
{
    if (!IsValid(Player) || !Player->HasAuthority()) { return nullptr; }
    const auto Income = Player->GetIncome()->Observe(); const auto& State = Income.State;
    if (State.Fault.Code == EY01RuntimeError::Busy || State.Phase == EY01IncomePhase::Disabled
        || Income.Balance != View.Gold || State.Rate != View.IncomeRate) { return nullptr; }
    auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("kind"), TEXT("ECONOMY"));
    J->SetStringField(TEXT("match"), View.MatchId.ToString()); J->SetStringField(TEXT("connection"), View.ConnectionId.ToString());
    J->SetNumberField(TEXT("join"), View.JoinSequence); J->SetNumberField(TEXT("revision"), View.ViewRevision);
    // int64累计与流水用十进制字符串，避免JSON数字消费端丢失整数精度；这是服务器只读审计，不拼入客户端UI。
    J->SetStringField(TEXT("sequence"), LexToString(State.Sequence));
    J->SetStringField(TEXT("totalIncome"), LexToString(State.TotalIncome));
    J->SetStringField(TEXT("totalSpending"), LexToString(State.TotalSpending));
    J->SetNumberField(TEXT("balance"), Income.Balance); J->SetNumberField(TEXT("remainder"), State.Remainder);
    J->SetNumberField(TEXT("rate"), State.Rate); J->SetNumberField(TEXT("lastTime"), State.LastTime);
    J->SetNumberField(TEXT("phase"), static_cast<int32>(State.Phase));
    J->SetNumberField(TEXT("fault"), static_cast<int32>(State.Fault.Code));
    return J;
}
TSharedPtr<FJsonObject> WaveRecord(const AY01R36Controller* Controller)
{
    const auto* World = Controller->GetWorld();
    const auto* Mode = World->GetAuthGameMode<AY01R36GameMode>();
    const auto* Flow = Mode ? Mode->GetFlowState() : nullptr;
    const auto* Profile = Mode ? Cast<UY01R36Profile>(Mode->GetProfile()) : nullptr;
    if (!Controller->HasAuthority() || !Flow || !Profile) { return nullptr; }
    const auto& S = Flow->GetWaveSnapshot(); const auto Catalog = Profile->BuildCatalog(); FY01DataValidation Errors;
    auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("kind"), TEXT("WAVE"));
    J->SetNumberField(TEXT("time"), World->GetTimeSeconds()); J->SetNumberField(TEXT("phaseTime"), S.ServerPhaseTime);
    J->SetNumberField(TEXT("prepareDeadline"), S.PreparationDeadline);
    J->SetNumberField(TEXT("phase"), static_cast<int32>(S.Phase)); J->SetNumberField(TEXT("result"), static_cast<int32>(S.Result));
    J->SetNumberField(TEXT("index"), S.WaveIndex); J->SetNumberField(TEXT("pending"), S.PendingSpawns);
    J->SetNumberField(TEXT("completed"), S.CompletedWaves); J->SetBoolField(TEXT("cleared"), S.bWaveCompleted);
    J->SetBoolField(TEXT("automaticWaves"), S.bAutomaticWaves);
    J->SetNumberField(TEXT("nextWaveIndex"), S.NextWaveIndex); J->SetNumberField(TEXT("nextWaveDeadline"), S.NextWaveDeadline);
    J->SetNumberField(TEXT("totalWaves"), S.TotalWaves); J->SetNumberField(TEXT("totalMonsters"), S.TotalMonsters);
    J->SetNumberField(TEXT("totalStartedMonsters"), S.TotalStartedMonsters);
    J->SetNumberField(TEXT("totalSpawnedMonsters"), S.TotalSpawnedMonsters);
    J->SetNumberField(TEXT("totalAliveMonsters"), S.TotalAliveMonsters);
    J->SetNumberField(TEXT("totalPendingSpawns"), S.TotalPendingSpawns);
    TArray<TSharedPtr<FJsonValue>> Progress;
    for (const auto& P : S.WaveStates)
    {
        auto W = MakeShared<FJsonObject>(); W->SetNumberField(TEXT("key"), P.WaveKey);
        W->SetStringField(TEXT("id"), P.Wave.RowName.ToString()); W->SetBoolField(TEXT("started"), P.bStarted);
        W->SetNumberField(TEXT("startTime"), P.StartTime); W->SetNumberField(TEXT("total"), P.TotalMonsters);
        W->SetNumberField(TEXT("spawned"), P.SpawnedMonsters); W->SetNumberField(TEXT("alive"), P.AliveMonsters);
        W->SetNumberField(TEXT("pending"), P.PendingSpawns); W->SetBoolField(TEXT("rewarded"), P.bRewarded);
        Progress.Add(MakeShared<FJsonValueObject>(W));
    }
    J->SetArrayField(TEXT("waveStates"), Progress);
    J->SetStringField(TEXT("profile"), Profile->GetPathName()); J->SetNumberField(TEXT("initialGold"), Profile->InitialGold);
    auto Tables = MakeShared<FJsonObject>();
    for (const auto& Pair : Profile->Tables)
    { Tables->SetStringField(StaticEnum<EY01TableKind>()->GetNameStringByValue(static_cast<int64>(Pair.Key)), GetPathNameSafe(Pair.Value)); }
    J->SetObjectField(TEXT("tables"), Tables);
    TArray<TSharedPtr<FJsonValue>> Waves, Monsters, Offers;
    for (const auto& Ref : Profile->OrderedWaves)
    {
        const auto* Row = Catalog.FindRow<FY01WaveRow>(Ref, Errors); if (!Row) { continue; }
        auto W = MakeShared<FJsonObject>(); W->SetStringField(TEXT("id"), Ref.RowName.ToString());
        W->SetNumberField(TEXT("prepare"), Row->PreparationSeconds);
        const auto* Reward = Catalog.FindRow<FY01RewardPackageRow>(Row->Reward, Errors);
        W->SetNumberField(TEXT("rewardGold"), Reward ? Reward->Gold : 0);
        W->SetNumberField(TEXT("rewardXP"), Reward ? Reward->Experience : 0);
        TArray<TSharedPtr<FJsonValue>> Spawns;
        for (const auto& Spawn : Row->Spawns)
        {
            auto E = MakeShared<FJsonObject>(); E->SetStringField(TEXT("definition"), Spawn.Monster.RowName.ToString());
            E->SetNumberField(TEXT("count"), Spawn.Count); E->SetNumberField(TEXT("delay"), Spawn.DelaySeconds);
            if (const auto* M = Catalog.FindRow<FY01MonsterRow>(Spawn.Monster, Errors))
            {
                E->SetNumberField(TEXT("maxHealth"), M->Health); E->SetNumberField(TEXT("attack"), M->AttackDamage);
                E->SetNumberField(TEXT("interval"), M->AttackInterval); E->SetNumberField(TEXT("speed"), M->MoveSpeed);
                E->SetNumberField(TEXT("attackType"), static_cast<int32>(M->AttackType));
                E->SetNumberField(TEXT("dropChance"), M->ItemDropChance); E->SetStringField(TEXT("dropReward"), M->DropReward.RowName.ToString());
            }
            Spawns.Add(MakeShared<FJsonValueObject>(E));
        }
        W->SetArrayField(TEXT("spawns"), Spawns); Waves.Add(MakeShared<FJsonValueObject>(W));
    }
    for (const AY01Monster* Monster : Mode->GetWaveMonsters())
    {
        if (!IsValid(Monster)) { continue; }
        const auto* ASC = Monster->GetCombatASC(); const auto Stats = ASC->GetUnitStats();
        auto M = MakeShared<FJsonObject>(); M->SetStringField(TEXT("actor"), Monster->GetName());
        M->SetStringField(TEXT("definition"), ASC->GetDefinition().RowName.ToString());
        M->SetNumberField(TEXT("health"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()));
        M->SetNumberField(TEXT("maxHealth"), Stats.Health); M->SetNumberField(TEXT("attack"), Stats.AttackDamage);
        M->SetNumberField(TEXT("x"), Monster->GetActorLocation().X); M->SetNumberField(TEXT("y"), Monster->GetActorLocation().Y);
        Monsters.Add(MakeShared<FJsonValueObject>(M));
    }
    for (const auto& Ref : Profile->ContentShopOffers)
    {
        const auto* O = Catalog.FindRow<FY01ShopOfferRow>(Ref, Errors); if (!O) { continue; }
        auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("id"), Ref.RowName.ToString());
        R->SetNumberField(TEXT("price"), O->Price); R->SetNumberField(TEXT("kind"), static_cast<int32>(O->ShopKind));
        if (O->ShopKind == EY01ShopKind::RandomPool)
        {
            if (const auto* Pool = Catalog.FindRow<FY01LocalItemPoolRow>(O->Pool, Errors))
            { R->SetNumberField(TEXT("itemType"), static_cast<int32>(Pool->ItemType)); }
        }
        else if (O->ShopKind == EY01ShopKind::RandomTier)
        {
            if (const auto* Pool = Catalog.FindRow<FY01QualityRollPoolRow>(O->TierPool, Errors))
            { R->SetNumberField(TEXT("itemType"), static_cast<int32>(Pool->ItemType)); }
        }
        Offers.Add(MakeShared<FJsonValueObject>(R));
    }
    J->SetArrayField(TEXT("orderedWaves"), Waves); J->SetArrayField(TEXT("monsters"), Monsters);
    J->SetArrayField(TEXT("shopConfig"), Offers); return J;
}
}
FString Y01R36Trace::ViewRecord(const FY01CommittedView& V)
{
    auto RuleRecord = [](const FY01ItemGrowth& Rule)
    {
        auto R = MakeShared<FJsonObject>(); R->SetStringField(TEXT("rule"), Rule.RuleId.ToString());
        R->SetNumberField(TEXT("trigger"), static_cast<int32>(Rule.Trigger));
        R->SetNumberField(TEXT("target"), static_cast<int32>(Rule.Target)); R->SetNumberField(TEXT("attribute"), static_cast<int32>(Rule.Attribute));
        R->SetNumberField(TEXT("delta"), Rule.Delta); R->SetNumberField(TEXT("accumulated"), Rule.Accumulated);
        R->SetStringField(TEXT("appliedCount"), LexToString(Rule.AppliedCount)); return R;
    };
    auto J = MakeShared<FJsonObject>();
    auto Growth = MakeShared<FJsonObject>(); Growth->SetBoolField(TEXT("enabled"), V.Growth.bEnabled);
    Growth->SetBoolField(TEXT("ended"), V.Growth.bEnded); Growth->SetStringField(TEXT("appliedKills"), LexToString(V.Growth.AppliedKills));
    Growth->SetNumberField(TEXT("strength"), V.Growth.Strength); Growth->SetNumberField(TEXT("fault"), static_cast<int32>(V.Growth.GrowthFault));
    Growth->SetStringField(TEXT("lastKill"), V.Growth.LastKillEventId.ToString());
    Growth->SetStringField(TEXT("faultKill"), V.Growth.FaultKillEventId.ToString()); Growth->SetStringField(TEXT("faultMessage"), V.Growth.FaultMessage);
    TArray<TSharedPtr<FJsonValue>> PlayerRules, ItemDetails;
    for (const auto& Entry : V.Growth.PlayerRules)
    { auto R = RuleRecord(Entry.Rule); R->SetStringField(TEXT("source"), Entry.SourceId); PlayerRules.Add(MakeShared<FJsonValueObject>(R)); }
    Growth->SetArrayField(TEXT("playerRules"), PlayerRules); J->SetObjectField(TEXT("growth"), Growth);
    for (const auto& Item : V.Items)
    {
        auto I = MakeShared<FJsonObject>(); I->SetStringField(TEXT("item"), Item.ItemId.ToString());
        I->SetStringField(TEXT("definition"), Item.Definition.RowName.ToString()); I->SetNumberField(TEXT("location"), static_cast<int32>(Item.Location));
        I->SetStringField(TEXT("name"), Item.Display.Name.ToString());
        I->SetNumberField(TEXT("type"), static_cast<int32>(Item.Definition.Type));
        I->SetNumberField(TEXT("skillType"), static_cast<int32>(Item.Display.SkillType));
        I->SetNumberField(TEXT("slot"), Item.Slot); TArray<TSharedPtr<FJsonValue>> Rules;
        for (const auto& Rule : Item.Growth) { Rules.Add(MakeShared<FJsonValueObject>(RuleRecord(Rule))); }
        I->SetArrayField(TEXT("growth"), Rules); EffectFields(*I, Item.Effects); ItemDetails.Add(MakeShared<FJsonValueObject>(I));
    }
    J->SetArrayField(TEXT("itemDetails"), ItemDetails);
    J->SetStringField(TEXT("kind"), TEXT("VIEW")); J->SetStringField(TEXT("match"), V.MatchId.ToString());
    J->SetStringField(TEXT("connection"), V.ConnectionId.ToString()); J->SetNumberField(TEXT("join"), V.JoinSequence);
    J->SetNumberField(TEXT("revision"), V.ViewRevision); J->SetNumberField(TEXT("items"), V.Items.Num());
    J->SetNumberField(TEXT("response"), V.ResponseSerial); J->SetNumberField(TEXT("seq"), V.Response.Request.Seq);
    J->SetNumberField(TEXT("code"), static_cast<int32>(V.Response.Code));
    // 使用完整已提交结构的稳定文本，而非进程内FName索引；只读摘要不参与业务或复制。
    FString Export; FY01CommittedView::StaticStruct()->ExportText(Export, &V, nullptr, nullptr, PPF_None, nullptr);
    FTCHARToUTF8 Bytes(*Export); FSHAHash Digest; FSHA1::HashBuffer(Bytes.Get(), Bytes.Length(), Digest.Hash);
    J->SetStringField(TEXT("digest"), Digest.ToString());
    int32 Bag = 0; for (const auto& I : V.Items) { Bag += I.Location == EY01ItemLocation::Backpack; }
    J->SetNumberField(TEXT("bag"), Bag); J->SetNumberField(TEXT("health"), V.Health);
    J->SetNumberField(TEXT("attack"), V.Stats.AttackDamage); J->SetNumberField(TEXT("expectedSeq"), V.ExpectedSeq);
    J->SetStringField(TEXT("requestItem"), V.Response.Request.ItemId.ToString());
    J->SetStringField(TEXT("ticket"), V.Response.TicketId.ToString());
    J->SetNumberField(TEXT("gold"), V.Gold); J->SetNumberField(TEXT("incomeRate"), V.IncomeRate);
    J->SetNumberField(TEXT("experience"), V.Experience);
    J->SetNumberField(TEXT("strength"), V.Stats.Strength); J->SetNumberField(TEXT("sampleTime"), V.SampleTime);
    J->SetStringField(TEXT("selectedTalent"), V.TalentChoice.SelectedTalent.ToString());
    J->SetStringField(TEXT("choice"), V.TalentChoice.ChoiceId.ToString());
    J->SetStringField(TEXT("productItem"), V.Response.ProductItemId.ToString());
    J->SetStringField(TEXT("requestOffer"), V.Response.Request.Offer.RowName.ToString());
    if (const auto* Product = V.Items.FindByPredicate([&](const auto& I) { return I.ItemId == V.Response.ProductItemId; }))
    {
        J->SetStringField(TEXT("productDefinition"), Product->Definition.RowName.ToString());
        J->SetNumberField(TEXT("productQuality"), static_cast<int32>(Product->Display.Quality));
    }
    TArray<TSharedPtr<FJsonValue>> Offers;
    for (const auto& O : V.ShopOffers)
    {
        auto Offer = MakeShared<FJsonObject>(); Offer->SetStringField(TEXT("id"), O.OfferId.ToString());
        Offer->SetStringField(TEXT("name"), O.DisplayName.ToString()); Offer->SetNumberField(TEXT("price"), O.Price);
        Offer->SetBoolField(TEXT("purchased"), O.bPurchased); Offer->SetNumberField(TEXT("kind"), static_cast<int32>(O.ShopKind));
        EffectFields(*Offer, O.Effects);
        Offers.Add(MakeShared<FJsonValueObject>(Offer));
    }
    J->SetArrayField(TEXT("offers"), Offers);
    TArray<TSharedPtr<FJsonValue>> Equipment, Skills;
    for (const auto& Id : V.EquipmentSlots) { Equipment.Add(MakeShared<FJsonValueString>(Id.ToString())); }
    for (const auto& S : V.Skills)
    {
        auto Skill = MakeShared<FJsonObject>(); Skill->SetStringField(TEXT("id"), S.LearnedId.ToString());
        Skill->SetStringField(TEXT("skill"), S.SkillId.ToString()); Skill->SetNumberField(TEXT("deadline"), S.Deadline);
        Skills.Add(MakeShared<FJsonValueObject>(Skill));
    }
    J->SetArrayField(TEXT("equipment"), Equipment); J->SetArrayField(TEXT("skills"), Skills);
    J->SetBoolField(TEXT("deliveryEnabled"), V.Delivery.bEnabled); J->SetBoolField(TEXT("deliveryEnded"), V.Delivery.bEnded);
    J->SetNumberField(TEXT("deliveryFault"), static_cast<int32>(V.Delivery.Fault));
    J->SetStringField(TEXT("deliveryFaultKill"), V.Delivery.FaultKillEventId.ToString());
    TArray<TSharedPtr<FJsonValue>> Deliveries, Arrivals;
    for (const auto& R : V.Delivery.Records)
    {
        auto D = MakeShared<FJsonObject>(); D->SetStringField(TEXT("id"), R.DeliveryId.ToString());
        D->SetStringField(TEXT("kill"), R.KillEventId.ToString()); D->SetStringField(TEXT("item"), R.ItemId.ToString());
        D->SetStringField(TEXT("match"), R.MatchId.ToString()); D->SetNumberField(TEXT("join"), R.JoinSequence);
        D->SetStringField(TEXT("definition"), R.Definition.RowName.ToString()); D->SetNumberField(TEXT("type"), static_cast<int32>(R.Definition.Type));
        D->SetNumberField(TEXT("quality"), static_cast<int32>(R.Quality)); D->SetNumberField(TEXT("start"), R.StartTime);
        D->SetNumberField(TEXT("eta"), R.ETA); D->SetNumberField(TEXT("state"), static_cast<int32>(R.State));
        D->SetNumberField(TEXT("cancel"), static_cast<int32>(R.CancelReason)); D->SetNumberField(TEXT("fault"), static_cast<int32>(R.Fault));
        D->SetNumberField(TEXT("retry"), R.RetryCount); D->SetNumberField(TEXT("delay"), R.DelaySeconds);
        D->SetNumberField(TEXT("deliveredTime"), R.DeliveredTime);
        TArray<TSharedPtr<FJsonValue>> Origin;
        for (double Value : {R.Origin.X, R.Origin.Y, R.Origin.Z}) { Origin.Add(MakeShared<FJsonValueNumber>(Value)); }
        D->SetArrayField(TEXT("origin"), Origin); Deliveries.Add(MakeShared<FJsonValueObject>(D));
    }
    for (const auto& I : V.Items)
    {
        if (!I.ArrivalDeliveryId.IsValid()) { continue; }
        auto A = MakeShared<FJsonObject>(); A->SetStringField(TEXT("item"), I.ItemId.ToString());
        A->SetStringField(TEXT("delivery"), I.ArrivalDeliveryId.ToString()); Arrivals.Add(MakeShared<FJsonValueObject>(A));
    }
    J->SetArrayField(TEXT("deliveries"), Deliveries); J->SetArrayField(TEXT("arrivals"), Arrivals);
    return Encode(J);
}
FString Y01R36Trace::PanelRecord(const UY01R36Panel* Panel)
{
    auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("kind"), TEXT("UI"));
    if (!Panel || !Panel->GetBackpack()) { return Encode(J); }
    const auto& M = Panel->GetModel(); auto* Bag = Panel->GetBackpack();
    J->SetNumberField(TEXT("time"), FPlatformTime::Seconds()); J->SetNumberField(TEXT("total"), Bag->GetListItems().Num());
    J->SetNumberField(TEXT("revision"), M.View().ViewRevision); J->SetBoolField(TEXT("pending"), M.IsPending());
    J->SetStringField(TEXT("connection"), M.View().ConnectionId.ToString());
    J->SetNumberField(TEXT("response"), M.View().ResponseSerial);
    J->SetBoolField(TEXT("confirmation"), M.NeedsConfirmation()); J->SetNumberField(TEXT("serverTime"), Panel->GetServerTime());
    J->SetNumberField(TEXT("offset"), Bag->GetScrollOffset());
    J->SetBoolField(TEXT("canDrag"), Panel->CanDrag());
    J->SetNumberField(TEXT("unseenArrivals"), M.UnseenArrivals());
    J->SetNumberField(TEXT("logRevision"), M.GetLogRevision());
    TArray<TSharedPtr<FJsonValue>> LogLines;
    for (const auto& Entry : M.GameLog()) { LogLines.Add(MakeShared<FJsonValueString>(Entry.ToString())); }
    J->SetArrayField(TEXT("gameLog"), LogLines);
    J->SetBoolField(TEXT("characterExpanded"), Panel->IsCharacterExpanded());
    J->SetBoolField(TEXT("backpackExpanded"), Panel->IsBackpackExpanded());
    J->SetBoolField(TEXT("shopExpanded"), Panel->IsShopExpanded());
    J->SetBoolField(TEXT("panelsExpanded"), Panel->IsBackpackExpanded());
    J->SetStringField(TEXT("panelsExpandedMeaning"), TEXT("backpack-only-compatibility"));
    J->SetBoolField(TEXT("canDragBackpack"), Panel->CanDragFrom(EY01R36Zone::Backpack));
    J->SetBoolField(TEXT("canToggleCharacter"), Panel->CanTogglePanel(EY01R36PanelKind::Character));
    J->SetBoolField(TEXT("canToggleBackpack"), Panel->CanTogglePanel(EY01R36PanelKind::Backpack));
    J->SetBoolField(TEXT("canToggleShop"), Panel->CanTogglePanel(EY01R36PanelKind::Shop));
    const auto& Waves = Panel->GetDisplayedWaves();
    J->SetStringField(TEXT("waveClockSource"), Panel->GetWaveClockSource().ToString());
    J->SetBoolField(TEXT("waveTimeHeld"), Panel->GetWaveClockSource() == TEXT("ObserverHeld"));
    J->SetNumberField(TEXT("waveServerTime"), Panel->GetWaveServerTime());
    J->SetBoolField(TEXT("automaticWaves"), Waves.bAutomaticWaves);
    J->SetNumberField(TEXT("nextWaveIndex"), Waves.NextWaveIndex);
    J->SetNumberField(TEXT("nextWaveDeadline"), Waves.NextWaveDeadline);
    J->SetNumberField(TEXT("totalAliveMonsters"), Waves.TotalAliveMonsters);
    J->SetNumberField(TEXT("totalPendingSpawns"), Waves.TotalPendingSpawns);
    J->SetNumberField(TEXT("waveRevision"), Waves.Revision);
    auto Geometry = [&](const UWidget* W)
    {
        auto G = MakeShared<FJsonObject>(); const auto& Cached = W->GetCachedGeometry();
        const auto* CellWidget = Cast<UY01R36Cell>(W);
        bool Visible = Panel->IsVisible() && (!CellWidget || Panel->CanUseZone(CellWidget->GetZone()));
        for (const UWidget* Parent = W; Parent; Parent = Parent->GetParent()) { Visible &= Parent->IsVisible(); }
        bool LaidOut = Visible && Cached.GetLocalSize().X > 0 && Cached.GetLocalSize().Y > 0 && W->GetWorld() && W->GetWorld()->GetGameViewport();
        FVector2D Min = FVector2D::ZeroVector, Max = FVector2D::ZeroVector;
        if (LaidOut)
        {
            FVector2D Viewport;
            USlateBlueprintLibrary::LocalToViewport(W, Cached, FVector2D::ZeroVector, Min, Viewport);
            USlateBlueprintLibrary::LocalToViewport(W, Cached, Cached.GetLocalSize(), Max, Viewport);
            LaidOut = FMath::IsFinite(Min.X) && FMath::IsFinite(Min.Y) && FMath::IsFinite(Max.X) && FMath::IsFinite(Max.Y)
                && Max.X > Min.X && Max.Y > Min.Y;
        }
        if (!LaidOut) { Min = Max = FVector2D::ZeroVector; }
        TArray<TSharedPtr<FJsonValue>> Rect;
        for (const double Value : { Min.X, Min.Y, Max.X, Max.Y }) { Rect.Add(MakeShared<FJsonValueNumber>(Value)); }
        G->SetArrayField(TEXT("rect"), Rect);
        G->SetBoolField(TEXT("visible"), Visible);
        G->SetBoolField(TEXT("laidOut"), LaidOut); G->SetStringField(TEXT("widget"), W->GetName());
        if (const auto* Text = Cast<UTextBlock>(W)) { G->SetStringField(TEXT("text"), Text->GetText().ToString()); }
        G->SetBoolField(TEXT("enabled"), W->GetIsEnabled());
        if (const auto* Button = Cast<UButton>(W))
        {
            if (const auto* Text = Cast<UTextBlock>(Button->GetContent()))
            {
                G->SetStringField(TEXT("label"), Text->GetText().ToString());
                G->SetNumberField(TEXT("fontSize"), Text->GetFont().Size);
                const auto& TG = Text->GetCachedGeometry();
                G->SetNumberField(TEXT("textAllocatedWidth"), TG.GetLocalSize().X);
                G->SetNumberField(TEXT("textAllocatedHeight"), TG.GetLocalSize().Y);
                if (FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer() != nullptr)
                {
                    const auto Natural = FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text->GetText(), Text->GetFont());
                    G->SetNumberField(TEXT("textNaturalWidth"), Natural.X);
                    G->SetNumberField(TEXT("textNaturalHeight"), Natural.Y);
                }
                if (LaidOut)
                {
                    FVector2D A, B, Viewport;
                    USlateBlueprintLibrary::LocalToViewport(Text, TG, FVector2D::ZeroVector, A, Viewport);
                    USlateBlueprintLibrary::LocalToViewport(Text, TG, TG.GetLocalSize(), B, Viewport);
                    TArray<TSharedPtr<FJsonValue>> TextRect;
                    for (const double Value : {A.X, A.Y, B.X, B.Y}) { TextRect.Add(MakeShared<FJsonValueNumber>(Value)); }
                    G->SetArrayField(TEXT("textRect"), TextRect);
                }
            }
        }
        if (LaidOut)
        {
            const FVector2D Pixel = (Min + Max) * .5;
            G->SetNumberField(TEXT("x"), Pixel.X); G->SetNumberField(TEXT("y"), Pixel.Y);
        }
        return G;
    };
    TArray<TSharedPtr<FJsonValue>> Cells;
    auto Cell = [&](UY01R36Cell* C)
    {
        if (!C) { return; } auto Row = Geometry(C);
        Row->SetNumberField(TEXT("zone"), static_cast<int32>(C->GetZone())); Row->SetNumberField(TEXT("slot"), C->GetSlotIndex());
        Row->SetStringField(TEXT("item"), C->GetItemId().ToString());
        Row->SetNumberField(TEXT("flashRemaining"), C->GetZone() == EY01R36Zone::Backpack
            ? M.FlashRemaining(C->GetItemId(), Panel->GetServerTime()) : 0.);
        if (const auto* I = M.Find(C->GetItemId()))
        {
            Row->SetNumberField(TEXT("type"), static_cast<int32>(I->Definition.Type));
            Row->SetStringField(TEXT("definition"), I->Definition.RowName.ToString()); Row->SetNumberField(TEXT("order"), I->AcquisitionOrder);
            Row->SetNumberField(TEXT("quality"), static_cast<int32>(I->Display.Quality));
            Row->SetNumberField(TEXT("skillType"), static_cast<int32>(I->Display.SkillType));
            EffectFields(*Row, I->Effects);
        }
        if (C->GetZone() == EY01R36Zone::Skill && M.View().Skills.IsValidIndex(C->GetSlotIndex()))
        {
            const auto& Skill = M.View().Skills[C->GetSlotIndex()];
            Row->SetNumberField(TEXT("deadline"), Skill.Deadline);
            Row->SetNumberField(TEXT("remaining"), M.Remaining(Skill, Panel->GetServerTime()));
            Row->SetStringField(TEXT("label"), M.SkillLabel(C->GetSlotIndex(), Panel->GetServerTime()));
        }
        Cells.Add(MakeShared<FJsonValueObject>(Row));
    };
    for (int32 I = 0; I < 6; ++I) { Cell(Panel->GetCell(EY01R36Zone::Equipment, I)); }
    for (int32 I = 0; I < 4; ++I) { Cell(Panel->GetCell(EY01R36Zone::Skill, I)); }
    for (auto* W : Bag->GetDisplayedEntryWidgets()) { Cell(Cast<UY01R36Cell>(W)); }
    J->SetArrayField(TEXT("cells"), Cells); J->SetObjectField(TEXT("bagGeometry"), Geometry(Bag));
    TArray<TSharedPtr<FJsonValue>> Glyphs;
    if (const auto* Layer = Cast<UY01R36FlightLayer>(Panel->WidgetTree->FindWidget(TEXT("DeliveryFlightLayer"))))
    {
        for (auto* Child : Layer->GetAllChildren())
        {
            const auto* Image = Cast<UImage>(Child); const auto* Slot = Image ? Cast<UCanvasPanelSlot>(Image->Slot) : nullptr;
            if (!Image || !Slot || !Image->IsVisible()) { continue; }
            auto G = Geometry(Image); const auto Position = Slot->GetPosition(), Size = Slot->GetSize();
            G->SetNumberField(TEXT("localX"), Position.X); G->SetNumberField(TEXT("localY"), Position.Y);
            G->SetNumberField(TEXT("width"), Size.X); G->SetNumberField(TEXT("height"), Size.Y);
            const auto Color = Image->GetColorAndOpacity(); TArray<TSharedPtr<FJsonValue>> RGBA;
            for (const double Value : {double(Color.R), double(Color.G), double(Color.B), double(Color.A)}) { RGBA.Add(MakeShared<FJsonValueNumber>(Value)); }
            G->SetArrayField(TEXT("color"), RGBA); Glyphs.Add(MakeShared<FJsonValueObject>(G));
        }
    }
    J->SetArrayField(TEXT("flightGlyphs"), Glyphs);
    for (const auto* Name : { TEXT("ToggleCharacter"), TEXT("ToggleBackpack"), TEXT("ToggleShop"), TEXT("CharacterPanel"), TEXT("BackpackPanel"), TEXT("ShopPanel"),
        TEXT("TopVitals"), TEXT("HealthBar"), TEXT("ShieldBar"), TEXT("HealthText"), TEXT("WaveCountdown"), TEXT("BackpackHint"), TEXT("AttributeList"),
        TEXT("GrowthSummary"), TEXT("GameLog"), TEXT("Confirmation"), TEXT("Status"),
        TEXT("Confirm"), TEXT("Cancel"), TEXT("Training"), TEXT("Synthesize"), TEXT("ShopList"), TEXT("TalentList") })
    { if (auto* W = Panel->WidgetTree->FindWidget(FName(Name))) { J->SetObjectField(Name, Geometry(W)); } }
    Panel->WidgetTree->ForEachWidget([&](UWidget* W)
    { if (W->GetName().StartsWith(TEXT("StatRow_")) || W->GetName().StartsWith(TEXT("StatLabel_")) || W->GetName().StartsWith(TEXT("StatValue_")))
        { J->SetObjectField(W->GetName(), Geometry(W)); } });
    for (const auto* Prefix : {TEXT("Purchase"), TEXT("Talent")})
    {
        const int32 Count = FCString::Strcmp(Prefix, TEXT("Purchase")) == 0 ? M.View().ShopOffers.Num() : M.View().TalentChoice.Candidates.Num();
        for (int32 I = 0; I < Count; ++I)
        {
            const FString Name = FString::Printf(TEXT("%s_%d"), Prefix, I);
            if (auto* W = Panel->WidgetTree->FindWidget(FName(Name))) { J->SetObjectField(Name, Geometry(W)); }
        }
    }
    return Encode(J);
}
TArray<FString> Y01R36Trace::Capture(const AY01R36Controller* Controller, bool bIncludeView)
{
    TArray<FString> Rows;
    if (!IsValid(Controller) || !Controller->GetWorld()) { return Rows; }
    int32 PrivateViews = 0, States = 0;
    for (TActorIterator<AY01PlayerState> It(Controller->GetWorld()); It; ++It)
    {
        ++States; FY01CommittedView V; const auto* Loadout = It->GetLoadout();
        const auto* Property = FindFProperty<FStructProperty>(Loadout->GetClass(), TEXT("ViewWire"));
        const auto* Wire = Property ? Property->ContainerPtrToValuePtr<FY01ViewWire>(Loadout) : nullptr;
        if (Wire && !Wire->Items.IsEmpty() && (!Controller->HasAuthority() || It->GetOwner() == Controller))
        {
            const auto& First = Wire->Items[0]; auto W = MakeShared<FJsonObject>();
            W->SetStringField(TEXT("kind"), TEXT("WIRE")); W->SetStringField(TEXT("transfer"), First.Scope.TransferId.ToString());
            W->SetStringField(TEXT("connection"), First.Scope.ConnectionId.ToString()); W->SetNumberField(TEXT("join"), First.Scope.JoinSequence);
            W->SetNumberField(TEXT("time"), FPlatformTime::Seconds()); W->SetNumberField(TEXT("window"), First.Scope.WindowSequence);
            W->SetNumberField(TEXT("bytes"), First.TotalBytes); W->SetNumberField(TEXT("revision"), First.Scope.ViewRevision);
            int32 End = 0; for (const auto& Chunk : Wire->Items) { End = FMath::Max(End, Chunk.Offset + Chunk.Bytes.Num()); }
            W->SetNumberField(TEXT("end"), End); W->SetNumberField(TEXT("chunks"), Wire->Items.Num());
            W->SetStringField(TEXT("diagnostic"), Loadout->GetViewDiagnostic()); Rows.Add(Encode(W));
        }
        if (Loadout->ReadCommittedView(V))
        {
            ++PrivateViews;
            if (bIncludeView && (!Controller->HasAuthority() || It->GetOwner() == Controller))
            {
                Rows.Add(ViewRecord(V));
                if (Controller->HasAuthority()) { if (const auto Ledger = EconomyRecord(*It, V)) { Rows.Add(Encode(Ledger.ToSharedRef())); } }
            }
        }
    }
    auto Scope = MakeShared<FJsonObject>(); Scope->SetStringField(TEXT("kind"), TEXT("SCOPE"));
    Scope->SetNumberField(TEXT("states"), States); Scope->SetNumberField(TEXT("privateViews"), PrivateViews);
    Scope->SetBoolField(TEXT("authority"), Controller->HasAuthority());
    Scope->SetStringField(TEXT("controller"), Controller->GetName());
    Scope->SetStringField(TEXT("pawn"), GetNameSafe(Controller->GetPawn()));
    const APawn* Pawn = Controller->GetPawn();
    const auto* Player = Controller->GetPlayerState<AY01PlayerState>();
    const auto* ASC = Player ? Player->GetCombatASC() : nullptr;
    const auto* Target = ASC ? ASC->GetCombatTarget() : nullptr;
    Scope->SetNumberField(TEXT("pawnX"), Pawn ? Pawn->GetActorLocation().X : 0.);
    Scope->SetNumberField(TEXT("pawnY"), Pawn ? Pawn->GetActorLocation().Y : 0.);
    Scope->SetNumberField(TEXT("targetDistance"), Pawn && IsValid(Target) ? FVector::Distance(Pawn->GetActorLocation(), Target->GetActorLocation()) : -1.);
    Scope->SetBoolField(TEXT("targetValid"), ASC && ASC->IsValidCombatTarget(Target));
    Scope->SetStringField(TEXT("ownerState"), GetNameSafe(Controller->PlayerState));
    Scope->SetBoolField(TEXT("ownerLinked"), Controller->PlayerState && Controller->PlayerState->GetOwner() == Controller);
    Scope->SetNumberField(TEXT("time"), FPlatformTime::Seconds());
    const auto* Flow = Controller->GetWorld()->GetGameState<AY01GameState>();
    Scope->SetNumberField(TEXT("phase"), Flow ? static_cast<int32>(Flow->GetWaveSnapshot().Phase) : -1);
    const auto& Training = Controller->GetObserver().Training;
    Scope->SetBoolField(TEXT("inside"), Training.bInside); Scope->SetNumberField(TEXT("room"), Training.Room);
    Scope->SetNumberField(TEXT("trainingKills"), Training.Kills); Scope->SetNumberField(TEXT("trainingAlive"), Training.Alive);
    Rows.Add(Encode(Scope));
    if (const auto Wave = WaveRecord(Controller)) { Rows.Add(Encode(Wave.ToSharedRef())); }
    if (Controller->GetPanel()) { Rows.Add(PanelRecord(Controller->GetPanel())); }
    return Rows;
}

TArray<FY01ItemRequest> Y01R36Trace::BuildBurst(const FY01CommittedView& View, EY01MatchPhase Phase)
{
    FY01R36UIModel Model;
    if (!Model.Sync(View) || !Model.CanInteract(Phase) || !View.ExpectedSeq || View.ExpectedSeq > MAX_uint64 - 8
        || View.EquipmentSlots.ContainsByPredicate([](const auto& Id) { return Id.IsValid(); })
        || View.Skills.ContainsByPredicate([](const auto& Skill) { return Skill.LearnedId.IsValid(); })) { return {}; }
    TArray<FGuid> Equipment, Books; TSet<FName> SkillIds;
    for (const auto& Item : View.Items)
    {
        if (Item.Location != EY01ItemLocation::Backpack) { return {}; }
        if (Item.Definition.Type == EY01TableKind::Equipment && Equipment.Num() < 6) { Equipment.Add(Item.ItemId); }
        if (Item.Definition.Type == EY01TableKind::Skill && Books.Num() < 2 && !SkillIds.Contains(Item.Definition.RowName))
        { Books.Add(Item.ItemId); SkillIds.Add(Item.Definition.RowName); }
    }
    if (Equipment.Num() != 6 || Books.Num() != 2) { return {}; }
    TArray<FY01ItemRequest> Requests;
    for (int32 I = 0; I < 8; ++I)
    {
        FY01R36Drag Identity; FY01ItemRequest Request;
        if (!Model.Drag(EY01R36Zone::Backpack, I < 6 ? Equipment[I] : Books[I - 6], INDEX_NONE, Identity)
            || !Model.Drop(Identity, I < 6 ? EY01R36Zone::Equipment : EY01R36Zone::Skill,
                I < 6 ? I : I - 6, Phase, Request)) { return {}; }
        Request.Seq += I; Requests.Add(Request);
    }
    return Requests;
}
FString Y01R36Trace::TryBurst(const FY01CommittedView& View, EY01MatchPhase Phase, bool bEnabled, bool& bSent,
    TFunctionRef<void(const FY01ItemRequest&)> Send)
{
    if (!bEnabled || bSent) { return {}; }
    const auto Requests = BuildBurst(View, Phase);
    if (Requests.IsEmpty()) { return {}; }
    TSharedPtr<FJsonObject> Base;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ViewRecord(View)), Base) || !Base) { return {}; }
    auto Record = MakeShared<FJsonObject>(); Record->SetStringField(TEXT("kind"), TEXT("BURST_SENT"));
    Record->SetStringField(TEXT("connection"), View.ConnectionId.ToString()); Record->SetObjectField(TEXT("base"), Base);
    TArray<TSharedPtr<FJsonValue>> Intents;
    for (const auto& Request : Requests)
    {
        auto Intent = MakeShared<FJsonObject>(); Intent->SetNumberField(TEXT("seq"), Request.Seq);
        Intent->SetNumberField(TEXT("operation"), static_cast<int32>(Request.Operation)); Intent->SetNumberField(TEXT("slot"), Request.Slot);
        Intent->SetStringField(TEXT("item"), Request.ItemId.ToString());
        const auto* Item = View.Items.FindByPredicate([&](const auto& I) { return I.ItemId == Request.ItemId; });
        Intent->SetStringField(TEXT("definition"), Item->Definition.RowName.ToString()); Intents.Add(MakeShared<FJsonValueObject>(Intent));
    }
    Record->SetArrayField(TEXT("requests"), Intents);
    bSent = true;
    for (const auto& Request : Requests) { Send(Request); }
    return Encode(Record);
}
FString Y01R36Trace::RequestRecord(const AY01R36Controller* Controller)
{
    if (!IsValid(Controller) || !Controller->HasAuthority()) { return {}; }
    const auto* State = Controller->GetPlayerState<AY01PlayerState>(); FY01CommittedView View;
    if (!IsValid(State) || State->GetOwner() != Controller || !State->GetLoadout()->ReadCommittedView(View)) { return {}; }
    TSharedPtr<FJsonObject> Summary;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ViewRecord(View)), Summary) || !Summary) { return {}; }
    const auto* Loadout = State->GetLoadout();
    const auto* Property = FindFProperty<FStructProperty>(Loadout->GetClass(), TEXT("ViewWire"));
    const auto* Wire = Property ? Property->ContainerPtrToValuePtr<FY01ViewWire>(Loadout) : nullptr;
    auto Record = MakeShared<FJsonObject>(); Record->SetStringField(TEXT("kind"), TEXT("REQUEST_COMMIT"));
    Record->SetObjectField(TEXT("view"), Summary); Record->SetNumberField(TEXT("frame"), GFrameCounter);
    Record->SetNumberField(TEXT("inFlightRevision"), Wire && !Wire->Items.IsEmpty() ? Wire->Items[0].Scope.ViewRevision : 0);
    Record->SetBoolField(TEXT("requestPresent"), View.Items.ContainsByPredicate([&](const auto& I) { return I.ItemId == View.Response.Request.ItemId; }));
    if (const auto Ledger = EconomyRecord(State, View)) { Record->SetObjectField(TEXT("economy"), Ledger); }
    return Encode(Record);
}

void AY01R36BurstControllerProbe::ProcessEvent(UFunction* Function, void* Parameters)
{
    if (Function && Function->GetFName() == TEXT("ServerItemRequest"))
    {
        const auto* Property = FindFProperty<FStructProperty>(Function, TEXT("Request"));
        if (Property && Parameters) { CapturedRequests.Add(*Property->ContainerPtrToValuePtr<FY01ItemRequest>(Parameters)); }
        return;
    }
    if (Function && Function->GetFName() == TEXT("ServerPlaygroundAction"))
    {
        const auto* Property = FindFProperty<FIntProperty>(Function, TEXT("Action"));
        if (Property && Parameters) { CapturedActions.Add(Property->GetPropertyValue_InContainer(Parameters)); }
        return;
    }
    Super::ProcessEvent(Function, Parameters);
}

namespace
{
// 显式短场只构造真实UIModel意图；不改DT、报价、事务或网络协议。
void TickTransactionTiming(AY01R36Controller* PC)
{
    FString Token, Label; FGuid Id;
    if (!IsValid(PC) || !PC->GetWorld() || !Y01ViewTiming::Now()
        || !FParse::Value(FCommandLine::Get(), TEXT("R36TransactionTiming="), Token)
        || !FGuid::ParseExact(Token, EGuidFormats::Digits, Id) || !Id.IsValid()
        || !FParse::Value(FCommandLine::Get(), TEXT("R36Run="), Label) || !Label.StartsWith(TEXT("R36-S4-Early13-"))) { return; }
    static const double Start = FPlatformTime::Seconds();
    if (FPlatformTime::Seconds() - Start > 200) { RequestEngineExit(TEXT("交易短场有界退出")); return; }
    auto* Player = PC->GetPlayerState<AY01PlayerState>();
    auto* GS = PC->GetWorld()->GetGameState<AY01GameState>();
    if (!Player || !GS) { return; }
    if (PC->HasAuthority())
    {
        FString Fixture;
        if (!FParse::Value(FCommandLine::Get(), TEXT("R36Fixture="), Fixture) || Fixture != TEXT("controlled")) { return; }
        auto* Mode = PC->GetWorld()->GetAuthGameMode<AY01R36GameMode>();
        if (Mode && GS->GetWaveSnapshot().Phase == EY01MatchPhase::Waiting) { Mode->TryStartNextWave(); }
        return;
    }
    if (!PC->IsLocalController() || Player->GetOwner() != PC) { return; }
    static FY01R36UIModel Model; static FY01ItemRequest Pending;
    static int32 Step = 0, BeforeItems = 0; static double Sent = 0, Next = 0;
    static bool Failed = false;
    if (Failed || Step >= 80) { return; }
    FY01CommittedView View;
    if (!Player->GetLoadout()->ReadCommittedView(View)) { return; }
    Model.Sync(View);
    const auto Phase = GS->GetWaveSnapshot().Phase;
    if (Sent)
    {
        if (Model.IsPending()) { return; }
        const auto& Response = View.Response;
        const bool Match = FY01ItemRequest::StaticStruct()->CompareScriptStruct(&Response.Request, &Pending, 0);
        const int32 Delta = Pending.Operation == EY01ItemOperation::Purchase ? 1 : Pending.Operation == EY01ItemOperation::Synthesize ? -4 : 0;
        bool Valid = Match && Response.Code == EY01ItemError::None && View.Items.Num() == BeforeItems + Delta;
        if (Pending.Operation == EY01ItemOperation::Equip) { Valid &= View.EquipmentSlots[0] == Pending.ItemId; }
        if (Pending.Operation == EY01ItemOperation::Unequip) { Valid &= !View.EquipmentSlots[0].IsValid(); }
        if (Pending.Operation == EY01ItemOperation::Synthesize)
        { for (const auto& M : Pending.Materials) { Valid &= !Model.Find(M.ItemId); } Valid &= Model.Find(Response.ProductItemId) != nullptr; }
        UE_LOG(LogTemp, Display, TEXT("R36Short SAMPLE {\"step\":%d,\"operation\":%d,\"seq\":%llu,\"revision\":%llu,\"milliseconds\":%.3f,\"items\":%d,\"code\":%d}"),
            Step, int32(Pending.Operation), Pending.Seq, View.ViewRevision, (FPlatformTime::Seconds() - Sent) * 1000, View.Items.Num(), Valid ? 0 : -1);
        if (!Valid) { Failed = true; UE_LOG(LogTemp, Display, TEXT("R36Short FAIL ReceiptOrAtomicity")); return; }
        Sent = 0; ++Step; Next = FPlatformTime::Seconds() + .12;
        if (Step == 80) { UE_LOG(LogTemp, Display, TEXT("R36Short DONE")); return; }
    }
    if (FPlatformTime::Seconds() < Next || !Model.CanInteract(Phase) || (Step == 0 && View.Items.Num() != 96)) { return; }
    FY01ItemRequest Request; bool Ready = false;
    if (Step % 4 == 0)
    {
        for (const auto& Offer : View.ShopOffers)
        { if (Offer.ShopKind != EY01ShopKind::DirectEffect && Offer.Quantity == 1 && !Offer.bPurchased && Model.Purchase(Offer.OfferId, Phase, Request)) { Ready = true; break; } }
    }
    else if (Step % 4 == 1 || Step % 4 == 2)
    {
        const bool Equip = Step % 4 == 1;
        const auto* Item = Equip ? View.Items.FindByPredicate([](const auto& I)
            { return I.Definition.Type == EY01TableKind::Equipment && I.Location == EY01ItemLocation::Backpack; }) : Model.Find(View.EquipmentSlots[0]);
        FY01R36Drag Drag;
        Ready = Item && Model.Drag(Equip ? EY01R36Zone::Backpack : EY01R36Zone::Equipment, Item->ItemId, 0, Drag)
            && Model.Drop(Drag, Equip ? EY01R36Zone::Equipment : EY01R36Zone::Backpack, 0, Phase, Request);
    }
    else { Ready = Model.Synthesize(Phase, Request); }
    if (!Ready) { Failed = true; UE_LOG(LogTemp, Display, TEXT("R36Short FAIL NoIntent Step=%d Items=%d Gold=%d"), Step, View.Items.Num(), View.Gold); return; }
    BeforeItems = View.Items.Num(); Pending = Request; Sent = FPlatformTime::Seconds();
    Model.Sent(Request); Y01ViewTiming::Mark(TEXT("REQUEST_SENT"), Y01ViewTiming::Scope(View), 0, 0, Request.Seq, View.Items.Num());
    PC->ServerItemRequest(Request);
}
}

void Y01R36Trace::TickCoreVerification(AY01R36Controller* PC)
{
    TickTransactionTiming(PC);
    if (!FParse::Param(FCommandLine::Get(), TEXT("Core33Net")) || !IsValid(PC) || !PC->GetWorld()) { return; }
    FString Label, Token; FGuid Id;
    if (!FParse::Value(FCommandLine::Get(), TEXT("R36Run="), Label) || !Label.StartsWith(TEXT("R36-S4-Core33Final-"))
        || !FParse::Value(FCommandLine::Get(), TEXT("Core33Token="), Token)
        || !FGuid::ParseExact(Token, EGuidFormats::Digits, Id) || !Id.IsValid()) { return; }
    for (const TCHAR C : Label) { if (!FChar::IsAlnum(C) && C != TEXT('-') && C != TEXT('_')) { return; } }
    static double Started = FPlatformTime::Seconds();
    FString Stop;
    if ((FFileHelper::LoadFileToString(Stop, *(FPaths::ProjectSavedDir() / Label / TEXT("stop.request"))) && Stop == Token)
        || FPlatformTime::Seconds() - Started > 100)
    { UE_LOG(LogTemp, Display, TEXT("Core33Net NORMAL_EXIT")); RequestEngineExit(TEXT("Core33自有短场正常退出")); return; }
    auto* World = PC->GetWorld(); auto* GS = World->GetGameState<AY01GameState>();
    auto* Core = GS ? GS->GetMatchCore() : nullptr; auto* Player = PC->GetPlayerState<AY01PlayerState>();
    if (!IsValid(Core) || !IsValid(Player) || !PC->GetPawn()) { return; }
    auto* Mode = World->GetAuthGameMode<AY01R36GameMode>();
    static TWeakObjectPtr<UWorld> ActiveWorld;
    static TWeakObjectPtr<AY01R36Controller> Subject;
    static TWeakObjectPtr<APawn> OriginalPawn;
    static int32 Stage = 0; static double Boundary = 0;
    static TMap<TWeakObjectPtr<AY01R36Controller>, double> Samples;
    if (ActiveWorld.Get() != World) { ActiveWorld = World; Subject.Reset(); OriginalPawn.Reset(); Stage = 0; Samples.Reset(); }
    FString Event;
    if (Mode && PC->HasAuthority())
    {
        if (Stage == 0)
        {
            TArray<AY01R36Controller*> Clients;
            for (auto It = World->GetPlayerControllerIterator(); It; ++It)
            {
                auto* C = Cast<AY01R36Controller>(It->Get()); auto* P = C ? C->GetPlayerState<AY01PlayerState>() : nullptr;
                if (C && C->GetNetConnection() && C->GetPawn() && P && P->IsCharacterInitialized()) { Clients.Add(C); }
            }
            if (Clients.Num() == 2)
            {
                // 两人暂入各自真实练功房，让正常主线怪自然选择核心；不改任何DT或怪物属性。
                Subject = Clients[0]; OriginalPawn = Clients[0]->GetPawn();
                const bool A = Mode->GetTraining()->Request(Clients[0], true), B = Mode->GetTraining()->Request(Clients[1], true);
                Stage = A && B ? 1 : -1; Event = A && B ? TEXT("SETUP") : TEXT("FAIL_TRAINING");
            }
        }
        if (Subject.Get() == PC)
        {
            auto* ASC = Player->GetCombatASC(); const auto& R = Player->GetRevive()->GetSnapshot();
            if (Stage == 1 && Core->GetSnapshot().HitSequence >= 4)
            { ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); Stage = 2; Event = TEXT("DEATH"); }
            else if (Stage == 2 && !R.bWaiting && !R.bCancelled && Y01Playground::Health(ASC) > 0)
            { Stage = 3; Boundary = World->GetTimeSeconds(); Event = TEXT("REVIVED"); }
            else if (Stage == 3 && World->GetTimeSeconds() - Boundary > 2)
            {
                ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0);
                UE_LOG(LogTemp, Display, TEXT("Core33Net SECOND_DEATH Waiting=%d"), Player->GetRevive()->GetSnapshot().bWaiting);
                Core->GetCombatASC()->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0);
                Stage = 4; Boundary = World->GetTimeSeconds(); Event = TEXT("FALLEN");
            }
            else if (Stage == 4 && World->GetTimeSeconds() - Boundary > 3)
            { Stage = 5; Event = TEXT("DONE"); }
        }
    }
    double& Next = Samples.FindOrAdd(PC);
    if (Event.IsEmpty() && World->GetTimeSeconds() < Next) { return; }
    Next = World->GetTimeSeconds() + .1;
    auto J = MakeShared<FJsonObject>(); J->SetStringField(TEXT("event"), Event);
    J->SetNumberField(TEXT("netMode"), int32(World->GetNetMode())); J->SetBoolField(TEXT("authority"), PC->HasAuthority());
    J->SetStringField(TEXT("player"), Player->GetName()); J->SetStringField(TEXT("pawn"), PC->GetPawn()->GetName());
    J->SetBoolField(TEXT("samePawn"), !Mode || Subject.Get() != PC || OriginalPawn.Get() == PC->GetPawn());
    J->SetNumberField(TEXT("time"), World->GetTimeSeconds()); J->SetNumberField(TEXT("stage"), Stage);
    J->SetStringField(TEXT("map"), World->GetOutermost()->GetName());
    J->SetStringField(TEXT("core"), Core->GetPathName()); J->SetNumberField(TEXT("phase"), int32(GS->GetWaveSnapshot().Phase));
    const auto& S = Core->GetSnapshot(); auto C = MakeShared<FJsonObject>();
    C->SetNumberField(TEXT("sample"), S.ServerSampleTime); C->SetNumberField(TEXT("hp"), S.Health); C->SetNumberField(TEXT("max"), S.MaxHealth);
    C->SetNumberField(TEXT("hit"), S.HitSequence); C->SetNumberField(TEXT("alert"), S.AlertSequence);
    C->SetNumberField(TEXT("lastHit"), S.LastHitTime); C->SetNumberField(TEXT("lastAlert"), S.LastAlertTime);
    C->SetBoolField(TEXT("fallen"), S.bFallen); C->SetBoolField(TEXT("frozen"), S.bFrozen); J->SetObjectField(TEXT("coreState"), C);
    const auto& R = Player->GetRevive()->GetSnapshot(); auto V = MakeShared<FJsonObject>();
    V->SetNumberField(TEXT("death"), R.DeathSequence); V->SetNumberField(TEXT("start"), R.DeathTime);
    V->SetNumberField(TEXT("deadline"), R.Deadline); V->SetNumberField(TEXT("sample"), R.ServerSampleTime);
    V->SetNumberField(TEXT("wave"), R.WaveIndex); V->SetBoolField(TEXT("waiting"), R.bWaiting); V->SetBoolField(TEXT("cancelled"), R.bCancelled);
    J->SetObjectField(TEXT("revive"), V); J->SetNumberField(TEXT("hp"), Y01Playground::Health(Player->GetCombatASC()));
    J->SetNumberField(TEXT("maxHP"), Player->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()));
    const auto T = Mode ? Mode->GetTraining()->Observe(Player) : PC->GetObserver().Training;
    J->SetBoolField(TEXT("inside"), T.bInside); J->SetNumberField(TEXT("room"), T.Room);
    const FVector P = PC->GetPawn()->GetActorLocation(); J->SetStringField(TEXT("location"), P.ToString());
    const auto* Capsule = PC->GetPawn()->FindComponentByClass<UCapsuleComponent>();
    const auto* Shape = Core->FindComponentByClass<UStaticMeshComponent>();
    J->SetBoolField(TEXT("outsideCore"), Capsule && Shape && FVector::DistSquared2D(P, Shape->Bounds.GetBox().GetClosestPointTo(P)) > FMath::Square(double(Capsule->GetScaledCapsuleRadius())));
    auto Text = [&](const UUserWidget* W, const TCHAR* Name)
    {
        const auto* B = W && W->WidgetTree ? Cast<UTextBlock>(W->WidgetTree->FindWidget(FName(Name))) : nullptr;
        auto O = MakeShared<FJsonObject>(); O->SetBoolField(TEXT("visible"), B && B->IsVisible());
        O->SetStringField(TEXT("text"), B ? B->GetText().ToString() : FString()); return O;
    };
    J->SetObjectField(TEXT("alertUI"), Text(PC->GetPanel(), TEXT("CoreAlert")));
    J->SetObjectField(TEXT("reviveUI"), Text(PC->GetPanel(), TEXT("ReviveCountdown")));
    if (const auto* W = Core->FindComponentByClass<UWidgetComponent>())
    {
        FString HealthText; const auto* User = W->GetUserWidgetObject();
        if (User && User->WidgetTree) { User->WidgetTree->ForEachWidget([&](UWidget* Child)
            { if (const auto* B = Cast<UTextBlock>(Child)) { HealthText += B->GetText().ToString(); } }); }
        J->SetStringField(TEXT("coreWorldText"), HealthText); J->SetBoolField(TEXT("coreWorldVisible"), W->IsVisible());
    }
    if (Mode)
    {
        J->SetBoolField(TEXT("combatRunning"), Mode->IsCombatRunning()); J->SetBoolField(TEXT("frozen"), Mode->IsGameplayFrozen());
        J->SetNumberField(TEXT("incomePhase"), int32(Player->GetIncome()->Observe().State.Phase));
        TArray<TSharedPtr<FJsonValue>> Monsters;
        for (const AY01Monster* M : Mode->GetWaveMonsters())
        {
            if (!IsValid(M)) { continue; } auto O = MakeShared<FJsonObject>();
            O->SetStringField(TEXT("id"), M->GetName()); O->SetNumberField(TEXT("hp"), Y01Playground::Health(M->GetCombatASC()));
            O->SetNumberField(TEXT("nextAttack"), M->GetCombatASC()->GetNextAutoAttackTime());
            O->SetStringField(TEXT("target"), GetNameSafe(M->GetCombatASC()->GetCombatTarget())); Monsters.Add(MakeShared<FJsonValueObject>(O));
        }
        J->SetArrayField(TEXT("monsters"), Monsters);
    }
    UE_LOG(LogTemp, Display, TEXT("Core33Net SAMPLE %s"), *Encode(J));
}

void AY01R36FlowProbe::OnPlayerInitialized(AY01PlayerState* Player)
{
    Super::OnPlayerInitialized(Player);
    ++InitializedCalls; bSawInitializedCharacter = Player && Player->IsCharacterInitialized();
}
void AY01R36FlowProbe::OnMatchPhaseChanged(EY01MatchPhase Previous, EY01MatchPhase Current)
{
    Super::OnMatchPhaseChanged(Previous, Current);
    PreviousPhases.Add(Previous); Phases.Add(Current);
}
