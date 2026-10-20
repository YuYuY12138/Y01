#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01EconomyComponent.h"
#include "Player/Y01GrowthComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "Engine/NetConnection.h"
#include "Net/UnrealNetwork.h"

FY01ItemResult UY01LoadoutComponent::SubmitRequest(AController* Requester, const FY01ItemRequest& Input)
{
    const auto* Owner = Cast<AY01PlayerState>(GetOwner());
    if (!IsInGameThread() || !IsValid(Owner) || !Owner->HasAuthority() || !IsValid(Requester)
        || Owner->GetOwner() != Requester || Requester->GetPlayerState<AY01PlayerState>() != Owner)
    { return Fail(EY01ItemError::Authority, TEXT("入口必须属于请求Controller的真实PlayerState")); }
    if (ResponseSerial == MAX_uint64)
    { ViewDiagnostic = TEXT("入口响应序号耗尽"); return Fail(EY01ItemError::Range, *ViewDiagnostic); }
    const FY01ItemRequest Request = Input;
    const double RequestStart = Y01ViewTiming::Now();
    Y01ViewTiming::Mark(TEXT("REQUEST_ENTER"), Y01ViewTiming::Scope(CommittedView), RequestStart, 0, Request.Seq, Items.Num());
    FY01ItemResult Result;
    {
        // 内层原业务结束后也暂不发布；完整回执与同次业务快照共同可见。
        TGuardValue<bool> Scope(bSubmittingRequest, true);
        switch (Request.Operation)
        {
        case EY01ItemOperation::Purchase: Result = Execute(Requester, Request, EY01ItemOperation::Purchase); break;
        case EY01ItemOperation::Synthesize: Result = Execute(Requester, Request, EY01ItemOperation::Synthesize); break;
        case EY01ItemOperation::ChooseTalent: Result = Execute(Requester, Request, EY01ItemOperation::ChooseTalent); break;
        case EY01ItemOperation::Devour: Result = Execute(Requester, Request, EY01ItemOperation::Devour); break;
        case EY01ItemOperation::Unequip: Result = TryUnequip(Requester, Request); break;
        case EY01ItemOperation::PrepareLearn: Result = PrepareLearn(Requester, Request); break;
        case EY01ItemOperation::ConfirmLearn: Result = ConfirmLearn(Requester, Request); break;
        case EY01ItemOperation::CancelLearn: Result = CancelLearn(Requester, Request); break;
        case EY01ItemOperation::RemoveSkill: Result = RemoveSkill(Requester, Request); break;
        default: Result = TryEquip(Requester, Request); break;
        }
        ++ResponseSerial; LatestResponse = {};
        LatestResponse.Request = Request; LatestResponse.Code = Result.Code; LatestResponse.Message = Result.Message;
        LatestResponse.Revision = Result.Revision; LatestResponse.TicketId = Result.TicketId;
        LatestResponse.LearnedId = Result.Learned.LearnedId; LatestResponse.RemovedLearnedId = Result.RemovedLearnedId;
        LatestResponse.ProductItemId = Result.ProductItemId; LatestResponse.TalentChoiceId = Result.TalentChoiceId; LatestResponse.SelectedTalent = Result.SelectedTalent;
    }
    PublishCommittedView();
    Y01ViewTiming::Mark(TEXT("REQUEST_DONE"), Y01ViewTiming::Scope(CommittedView), RequestStart, 0, Request.Seq, Items.Num());
    return Result;
}

bool UY01LoadoutComponent::ReadCommittedView(FY01CommittedView& Out) const
{
    Out = {};
    if (!IsInGameThread() || !GetOwner()) { return false; }
    Out = GetOwner()->HasAuthority() ? CommittedView : ViewReceiver.Get();
    return Out.ViewRevision != 0;
}
bool UY01LoadoutComponent::ReadCommittedScope(FY01ViewAck& Out) const
{
    Out = {};
    if (!IsInGameThread() || !GetOwner()) { return false; }
    Out = Y01ViewTiming::Scope(GetOwner()->HasAuthority() ? CommittedView : ViewReceiver.Get());
    return Out.ViewRevision != 0;
}
FY01ItemResult UY01LoadoutComponent::PublishCommittedView()
{
    const double TimingStart = Y01ViewTiming::Now();
    if (!IsInGameThread()) { return Fail(EY01ItemError::Authority, TEXT("仅服务器主线发布")); }
    if (bSubmittingRequest || Transactions.bBusy || (Player.IsValid() && (Player->GetSourceEffects()->bBusy || Player->GetSourceEffects()->bInCall)))
    { return Fail(EY01ItemError::Busy, TEXT("来源或库存尚未共同结束，保留上一完整视图")); }
    auto Result = CheckOwner(); if (!Result.IsOk()) { return Result; }
    auto* Sources = Player->GetSourceEffects();
    FY01ResolvedAttributes Attributes;
    // 未初始化来源的旧夹具可保留空明细；真实会话必须读取已提交缓存，不重算或发布部分状态。
    if (Sources->bInitialized)
    {
        Result = FromSource(Sources->ObserveAttributes(Attributes));
        if (!Result.IsOk()) { ViewDiagnostic = Result.Message; return Result; }
    }
    Result = FromSource(Sources->ValidateHandles());
    if (!Result.IsOk()) { ViewDiagnostic = Result.Message; return Result; }
    // 权威来源身份必须与当前生效的实例/已学身份一一对应；基础来源不属于库存。
    TMap<FString, TArray<FY01ItemEffect>> EquipmentEffects;
    for (const auto& Slot : EquipmentSlots)
    { if (Slot.IsValid()) { EquipmentEffects.Add(SourceId(Slot).Id, UY01GrowthComponent::EquipmentEffects(Items.FindChecked(Slot))); } }
    for (const auto& D : Devoured)
    { EquipmentEffects.Add(SourceId(D.ItemId, EY01RuntimeSourceKind::Devoured).Id, UY01GrowthComponent::EquipmentEffects(D)); }
    TMap<FString, const TArray<FY01ItemEffect>*> Expected;
    for (const auto& Pair : EquipmentEffects) { Expected.Add(Pair.Key, &Pair.Value); }
    for (const auto& S : Skills) { if (S.LearnedId.IsValid()) { Expected.Add(SourceId(S.LearnedId, EY01RuntimeSourceKind::Skill).Id, &S.Effects); } }
    const auto* Economy = Player->GetEconomy();
    for (const auto& Pair : Economy->OfferStates)
    { if (Pair.Value.bPurchased) { Expected.Add(Pair.Value.SourceId, &Pair.Value.Effects); } }
    if (!Economy->TalentState.SelectedTalent.IsNone())
    {
        const auto* Selected = Economy->TalentState.Candidates.FindByPredicate([&](const auto& C) { return C.TalentId == Economy->TalentState.SelectedTalent; });
        if (!Selected) { return Fail(EY01ItemError::SourceFailure, TEXT("已选天赋不属于服务器候选")); }
        Expected.Add(Economy->TalentState.SourceId, &Selected->Effects);
    }
    if (!Sources->bEnded)
    {
        for (const auto& R : Sources->Records)
        {
            if (R.Source.Kind == EY01RuntimeSourceKind::Equipment || R.Source.Kind == EY01RuntimeSourceKind::Skill
                || R.Source.Kind == EY01RuntimeSourceKind::Devoured
                || (R.Source.Kind == EY01RuntimeSourceKind::Shop && Economy->IsPurchasedSource(R.Source.Id))
                || (R.Source.Kind == EY01RuntimeSourceKind::Talent && Economy->IsSelectedTalentSource(R.Source.Id)))
            {
                const auto* Effects = Expected.Find(R.Source.Id);
                bool Matches = Effects && (*Effects)->Num() == R.Effects.Num();
                if (Matches)
                {
                    for (const auto& Effect : **Effects)
                    {
                        Matches &= R.Effects.ContainsByPredicate([&](const auto& Actual)
                        { return Actual.Source.Id == R.Source.Id && Actual.Source.Kind == R.Source.Kind && Actual.Source.LocalKey == R.Source.LocalKey
                            && Actual.EffectId == Effect.EffectId && Actual.Kind == Effect.Kind && Actual.Magnitude == Effect.Magnitude
                            && Actual.SourceAttribute == Effect.SourceAttribute && Actual.TargetAttribute == Effect.TargetAttribute && Actual.Amount == Effect.Amount
                            && Actual.Condition == Effect.Condition && Actual.ConditionThreshold == Effect.ConditionThreshold
                            && Actual.DurationSeconds == Effect.DurationSeconds && Actual.LayerCap == Effect.LayerCap
                            && Actual.IntervalSeconds == Effect.IntervalSeconds
                            && Actual.bGrowthContribution == Effect.bGrowthContribution; });
                    }
                }
                if (!Matches) { ViewDiagnostic = TEXT("来源与库存身份或效果快照不一致，拒绝发布"); return Fail(EY01ItemError::SourceFailure, *ViewDiagnostic); }
                Expected.Remove(R.Source.Id);
            }
        }
        if (!Expected.IsEmpty()) { ViewDiagnostic = TEXT("库存生效身份缺少来源，拒绝发布"); return Fail(EY01ItemError::SourceFailure, *ViewDiagnostic); }
    }
    auto* Controller = Cast<AController>(Player->GetOwner()); auto* Connection = Player->GetNetConnection();
    if (!ViewConnectionId.IsValid() || ViewOwner.Get() != Controller || ViewNetConnection.Get() != Connection)
    {
        ViewConnectionId = FGuid::NewGuid(); ViewOwner = Controller; ViewNetConnection = Connection;
        ViewSender.Reset(); ViewWire.Items.Reset(); ViewWire.MarkArrayDirty();
    }
    FY01CommittedView Next;
    Next.MatchId = MatchId; Next.JoinSequence = JoinSequence; Next.ConnectionId = ViewConnectionId;
    Next.Revision = Revision; Next.SourceRevision = Sources->Revision; Next.ConfigRevision = ConfigRevision;
    Next.ExpectedSeq = Transactions.ExpectedSeq; Next.EquipmentSlots = EquipmentSlots; Next.Tickets = Tickets;
    Next.ResponseSerial = ResponseSerial; Next.Response = LatestResponse;
    for (const auto& Id : Economy->OfferOrder) { Next.ShopOffers.Add(Economy->OfferStates.FindChecked(Id)); }
    for (const auto& Id : Economy->RecipeOrder)
    { auto& Recipe = Next.SynthesisRecipes.AddDefaulted_GetRef(); Recipe.RecipeId = Id; Recipe.Recipe = Economy->Recipes.FindChecked(Id); }
    Next.TalentChoice = Economy->TalentState;
    Next.Gold = Player->GetGold(); Next.Experience = Player->GetExperience();
    Next.Health = Player->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    Next.Stats = Player->GetCombatASC()->GetUnitStats(); Next.IncomeRate = Player->GetIncome()->Observe().State.Rate;
    Next.Growth = Player->GetGrowth()->GetLedger();
    Next.Delivery = Player->GetDropDelivery()->GetLedger();
    Next.AttributeBreakdowns = MoveTemp(Attributes.Breakdowns);
    Next.Items.Reserve(Order.Num());
    const auto ToView = [](const FY01ItemInstance& I, FY01ViewItem& V)
    {
        V.ItemId = I.ItemId; V.ArrivalDeliveryId = I.ArrivalDeliveryId;
        V.Definition = I.Definition; V.EntityRevision = I.EntityRevision; V.LastChangedRevision = I.LastChangedRevision;
        V.AcquisitionOrder = I.AcquisitionOrder; V.Location = I.Location; V.Slot = I.Slot; V.Count = I.Count;
        V.Effects = I.Effects; V.Growth = I.Growth; V.DamageEffects = I.DamageEffects;
        V.Display.Name = I.DisplayName; V.Display.Quality = I.Quality; V.Display.Icon = I.Icon.ToSoftObjectPath();
        V.Display.SkillType = I.Skill.Type; V.Display.CooldownSeconds = I.Skill.CooldownSeconds;
    };
    for (const auto& Id : Order) { ToView(Items.FindChecked(Id), Next.Items.AddDefaulted_GetRef()); }
    for (const auto& D : Devoured) { ToView(D, Next.Devoured.AddDefaulted_GetRef()); }
    for (const auto& S : Skills)
    {
        auto& V = Next.Skills.AddDefaulted_GetRef(); V.LearnedId = S.LearnedId; V.SkillId = S.SkillId; V.Slot = S.Slot;
        V.EntityRevision = S.EntityRevision; V.Deadline = S.Deadline; V.Effects = S.Effects; V.Growth = S.Growth; V.DamageEffects = S.DamageEffects;
        V.Display.Name = S.Skill.DisplayName; V.Display.Quality = S.Skill.Quality; V.Display.Icon = S.Skill.Icon.ToSoftObjectPath();
        V.Display.SkillType = S.Skill.Type; V.Display.CooldownSeconds = S.Skill.CooldownSeconds;
    }
    const uint64 First = Transactions.ExpectedSeq > 64 ? Transactions.ExpectedSeq - 64 : 1;
    for (uint64 Seq = First; Seq < Transactions.ExpectedSeq; ++Seq)
    {
        const auto& R = Transactions.Recent[(Seq - 1) % 64]; auto& V = Next.Receipts.AddDefaulted_GetRef();
        V.Request = R.Payload; V.Code = R.Result.Code; V.Message = R.Result.Message; V.Revision = R.Result.Revision;
        V.TicketId = R.Result.TicketId; V.LearnedId = R.Result.Learned.LearnedId; V.RemovedLearnedId = R.Result.RemovedLearnedId;
        V.ProductItemId = R.Result.ProductItemId; V.TalentChoiceId = R.Result.TalentChoiceId; V.SelectedTalent = R.Result.SelectedTalent;
    }
    Next.ViewRevision = CommittedView.ViewRevision; Next.SampleTime = CommittedView.SampleTime;
    if (FY01CommittedView::StaticStruct()->CompareScriptStruct(&Next, &CommittedView, 0))
    {
        // 未变的发布版本只由本组件整体替换；已在途/已确认时跳过重复全量校验（与Offer同样清诊断），其余情况仍重试Offer。
        if (Controller && Controller->GetPlayerState<AY01PlayerState>() == Player.Get())
        {
            if (ViewSender.IsCurrent(CommittedView)) { ViewDiagnostic.Reset(); }
            else { ViewSender.Offer(CommittedView, ViewWire, ViewDiagnostic); }
        }
        return Fail(EY01ItemError::None, TEXT(""));
    }
    if (Next.ViewRevision == MAX_uint64) { ViewDiagnostic = TEXT("业务视图版本耗尽"); return Fail(EY01ItemError::Range, *ViewDiagnostic); }
    ++Next.ViewRevision; Next.SampleTime = GetWorld()->GetTimeSeconds(); CommittedView = MoveTemp(Next);
    Y01ViewTiming::Mark(TEXT("COMMIT"), Y01ViewTiming::Scope(CommittedView), TimingStart, 0, LatestResponse.Request.Seq, Items.Num(), &CommittedView);
    // 网络排队失败不回滚已完成业务，保留明确诊断，后续复制调度可以重试。
    if (Controller && Controller->GetPlayerState<AY01PlayerState>() == Player.Get()) { ViewSender.Offer(CommittedView, ViewWire, ViewDiagnostic); }
    Player->ForceNetUpdate(); return Fail(EY01ItemError::None, TEXT(""));
}
void UY01LoadoutComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION(UY01LoadoutComponent, ViewWire, COND_OwnerOnly);
}
void UY01LoadoutComponent::PreReplication(IRepChangedPropertyTracker& Tracker)
{
    Super::PreReplication(Tracker);
    if (bInitialized) { PublishCommittedView(); }
}
void UY01LoadoutComponent::ReceiveViewWindow()
{
    if (!GetOwner() || GetOwner()->HasAuthority() || ViewWire.Items.IsEmpty()) { return; }
    FY01ViewAck Ack;
    if (ViewReceiver.Receive(ViewWire, Ack, ViewDiagnostic)) { ServerAckView(Ack); }
    else
    {
        // 当前窗口身份只用于请求重发；服务器重新核验，不接受客户端余额或实体内容。
        ServerRestartView(ViewWire.Items[0].Scope);
    }
}
bool UY01LoadoutComponent::CheckViewPeer()
{
    if (!CheckOwner().IsOk() || !ViewOwner.IsValid() || Player->GetOwner() != ViewOwner.Get()
        || ViewOwner->GetPlayerState<AY01PlayerState>() != Player.Get() || ViewOwner->IsActorBeingDestroyed()
        || ViewNetConnection.Get() != Player->GetNetConnection())
    { ViewDiagnostic = TEXT("复制确认不属于当前Owner/连接"); return false; }
    return true;
}
void UY01LoadoutComponent::ServerAckView_Implementation(FY01ViewAck Ack)
{
    // UE首先按拥有连接验证Server RPC；这里再次绑定真实PlayerState、Match/Join和在途序号。
    if (CheckViewPeer() && ViewSender.Acknowledge(Ack, ViewWire, ViewDiagnostic)) { Player->ForceNetUpdate(); }
}
void UY01LoadoutComponent::ServerRestartView_Implementation(FY01ViewAck Scope)
{
    if (CheckViewPeer() && ViewSender.Restart(Scope, ViewWire, ViewDiagnostic)) { Player->ForceNetUpdate(); }
}
