#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01EconomyComponent.h"
#include "Player/Y01GrowthComponent.h"
#include "Player/Y01DamageEffectsComponent.h"
#include "Player/Y01ReviveComponent.h"
#include "Match/Y01GameFlow.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

namespace { using E = EY01ItemError; }
UY01LoadoutComponent::UY01LoadoutComponent()
{
    PrimaryComponentTick.bCanEverTick = false; bAutoActivate = false;
    SetIsReplicatedByDefault(true);
    EquipmentSlots.SetNum(Y01Data::EquipmentSlotCount);
    Skills.SetNum(Y01Data::SkillSlotCount); Tickets.SetNum(Y01Data::SkillSlotCount);
}
FY01ItemResult UY01LoadoutComponent::Fail(E Code, const TCHAR* Message) const
{ return FY01ItemResult::Fail(Code, Revision, Message); }
FY01ItemResult UY01LoadoutComponent::FromSource(const FY01RuntimeResult& R) const
{
    E Code = E::SourceFailure;
    switch (R.Code)
    {
    case EY01RuntimeError::None: Code = E::None; break;
    case EY01RuntimeError::Authority: Code = E::Authority; break;
    case EY01RuntimeError::Session: Code = E::Session; break;
    case EY01RuntimeError::Disabled: Code = E::Disabled; break;
    case EY01RuntimeError::Stopped: Code = E::Stopped; break;
    case EY01RuntimeError::Busy: Code = E::Busy; break;
    case EY01RuntimeError::Precision: Code = E::Precision; break;
    case EY01RuntimeError::EffectRejected: Code = E::EffectRejected; break;
    case EY01RuntimeError::HandleLost: Code = E::HandleLost; break;
    case EY01RuntimeError::Interrupted: Code = E::Interrupted; break;
    case EY01RuntimeError::IntegerRange: Code = E::Range; break;
    default: break;
    }
    return Fail(Code, *R.Message);
}
FY01ItemResult UY01LoadoutComponent::CheckOwner() const
{
    if (!bInitialized) { return Fail(E::Disabled, TEXT("物品注册表尚未显式初始化")); }
    const auto* P = Player.Get();
    const auto* State = GetWorld() ? GetWorld()->GetGameState<AY01GameState>() : nullptr;
    const auto* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AY01GameMode>() : nullptr;
    if (!IsInGameThread() || !IsValid(P) || P != GetOwner() || P->GetLoadout() != this || !P->HasAuthority()
        || P->IsActorBeingDestroyed() || !State || !Mode || Mode->GetFlowState() != State || GameState.Get() != State
        || !State->PlayerArray.Contains(P) || !P->IsCharacterInitialized()
        || P->GetCombatASC()->GetOwnerActor() != P)
    { return Fail(E::Authority, TEXT("注册表必须属于真实权威PlayerState/GameState/ASC会话")); }
    const auto* S = P->GetSourceEffects();
    if (!S || S->Player.Get() != P || S->MatchId != MatchId || S->JoinSequence != JoinSequence)
    { return Fail(E::Session, TEXT("物品会话与来源会话不一致")); }
    return Fail(E::None, TEXT(""));
}
FY01ItemResult UY01LoadoutComponent::CheckRequester(const AController* C) const
{
    auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    const auto* Pawn = Cast<APawn>(Player->GetCombatASC()->GetAvatarActor());
    if (!IsValid(C) || !C->HasAuthority() || C->IsActorBeingDestroyed() || C->GetWorld() != GetWorld()
        || C->GetPlayerState<AY01PlayerState>() != Player.Get() || !IsValid(Pawn) || !Pawn->HasAuthority()
        || Pawn->IsActorBeingDestroyed() || Pawn->GetPlayerState() != Player.Get() || C->GetPawn() != Pawn || Pawn->GetController() != C)
    { return Fail(E::Authority, TEXT("请求控制器必须真实拥有本PlayerState及ASC的Pawn")); }
    return R;
}
FY01ItemResult UY01LoadoutComponent::CheckActive() const
{
    auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    if (bEnded || GetWorld()->GetAuthGameMode<AY01GameMode>()->IsGameplayFrozen())
    { return Fail(E::Stopped, TEXT("物品会话已经结束或玩法冻结")); }
    if (Player->GetRevive()->BlocksActions()) { return Fail(E::Dead, TEXT("复活等待期间禁止主动状态变更")); }
    const auto Phase = GameState->GetWaveSnapshot().Phase;
    if (Phase != EY01MatchPhase::Preparing && Phase != EY01MatchPhase::Battle)
    { return Fail(E::Phase, TEXT("主动装卸仅允许Preparing或Battle")); }
    const float Health = Player->GetCombatASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    if (!FMath::IsFinite(Health) || Health <= 0) { return Fail(E::Dead, TEXT("死亡玩家不可主动装卸")); }
    return FromSource(Player->GetSourceEffects()->Check(true));
}
FY01ItemResult UY01LoadoutComponent::Initialize(const FGuid& Match, int32 Join)
{
    if (!IsInGameThread()) { return Fail(E::Authority, TEXT("仅主线程序列可初始化")); }
    if (!Transactions.Enter()) { return Fail(E::Busy, TEXT("事务期间不可初始化")); }
    ON_SCOPE_EXIT { Transactions.Leave(); PublishCommittedView(); };
    auto* P = Cast<AY01PlayerState>(GetOwner());
    auto* State = GetWorld() ? GetWorld()->GetGameState<AY01GameState>() : nullptr;
    auto* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AY01GameMode>() : nullptr;
    if (!IsValid(P) || P->GetLoadout() != this || !P->HasAuthority() || P->IsActorBeingDestroyed()
        || !State || !Mode || Mode->GetFlowState() != State || !State->PlayerArray.Contains(P) || !P->IsCharacterInitialized()
        || P->GetCombatASC()->GetOwnerActor() != P)
    { return Fail(E::Authority, TEXT("初始化必须绑定真实权威玩家、Mode、GameState和ASC")); }
    if (!Match.IsValid() || Join <= 0 || (bInitialized && (Match != MatchId || Join != JoinSequence)))
    { return Fail(E::Session, TEXT("Match/Join无效或试图覆盖本局身份")); }
    if (bEnded) { return Fail(E::Stopped, TEXT("结束后不得重置注册表")); }
    if (bInitialized) { return CheckOwner(); }
    for (const auto& Grant : P->GetCombatASC()->GetGrantedSources())
    { if (Grant.Source.Type == EY01TableKind::Equipment || Grant.Source.Type == EY01TableKind::Skill)
        { return Fail(E::InvalidDefinition, TEXT("旧装备或技能授予不能混入新物品注册表")); } }
    const auto R = P->GetSourceEffects()->Initialize(Match, Join); if (!R.IsOk()) { return FromSource(R); }
    Player = P; GameState = State; MatchId = Match; JoinSequence = Join; bInitialized = true; Revision = 1;
    SetActiveFlag(true); return Fail(E::None, TEXT(""));
}
FY01ItemResult UY01LoadoutComponent::Resolve(const FY01DataCatalog& Catalog, const FY01DataRef& Ref, FY01ItemInstance& Out) const
{
    Out = FY01ItemInstance(); Out.Definition = Ref;
    FY01DataValidation Errors; TArray<FY01DataRef> Effects, Growth, Damage;
    if (Ref.Type == EY01TableKind::Equipment)
    {
        const auto* Row = Catalog.FindRow<FY01EquipmentRow>(Ref, Errors);
        if (!Row || !Row->IsValid() || Row->AttackBonus != 0 || !Row->Effect.IsNull()
            || (Row->EffectIds.IsEmpty() && Row->GrowthRuleIds.IsEmpty() && Row->DamageEffectIds.IsEmpty()))
        { return Fail(E::InvalidDefinition, TEXT("装备必须配置EffectIds/GrowthRuleIds/DamageEffectIds之一，不接受旧AttackBonus或Effect")); }
        Out.DisplayName = Row->DisplayName; Out.Quality = Row->Quality; Out.Icon = Row->Icon;
        Effects = Row->EffectIds; Growth = Row->GrowthRuleIds; Damage = Row->DamageEffectIds;
    }
    else if (Ref.Type == EY01TableKind::Skill)
    {
        const auto* Row = Catalog.FindRow<FY01SkillRow>(Ref, Errors);
        if (!Row || static_cast<uint8>(Row->Type) > static_cast<uint8>(EY01SkillType::Passive)
            || static_cast<uint8>(Row->Quality) > static_cast<uint8>(EY01Quality::Red)
            || !FMath::IsFinite(Row->CooldownSeconds) || Row->CooldownSeconds < 0 || !Row->TestSkill.IsValid()
            || (Row->TestSkill.Mode != EY01TestSkillMode::None && Row->Type != EY01SkillType::Active))
        { return Fail(E::InvalidDefinition, TEXT("技能书静态定义无效")); }
        if (!Row->Effect.IsNull()
            || (!Row->DamageEffectIds.IsEmpty() && Row->TestSkill.Mode != EY01TestSkillMode::None)
            || (Row->Type == EY01SkillType::Passive && (!Row->Ability.IsNull() || Row->CooldownSeconds != 0)))
        { return Fail(E::InvalidDefinition, TEXT("技能书新旧效果配置或主动被动字段冲突")); }
        // 只复制书的静态效果/成长，不加载或授予模板GA；模板适配由后续Profile负责。
        Out.Skill.Type = Row->Type; Out.Skill.Quality = Row->Quality; Out.Skill.DisplayName = Row->DisplayName;
        Out.Skill.Icon = Row->Icon; Out.Skill.Ability = Row->Ability; Out.Skill.TestSkill = Row->TestSkill;
        Out.Skill.CooldownSeconds = Row->CooldownSeconds;
        Out.DisplayName = Row->DisplayName; Out.Quality = Row->Quality; Out.Icon = Row->Icon;
        Effects = Row->EffectIds; Growth = Row->GrowthRuleIds; Damage = Row->DamageEffectIds;
    }
    else { return Fail(E::InvalidDefinition, TEXT("本局包仅接收装备或技能书")); }
    TSet<FName> Seen;
    for (const auto& Id : Effects)
    {
        const auto* Row = Catalog.FindRow<FY01SourceEffectRow>(Id, Errors);
        if (Id.Type != EY01TableKind::SourceEffect || !Row || Seen.Contains(Id.RowName)
            || !Y01Data::ValidSourceEffect(*Row)
            || (Ref.Type != EY01TableKind::Equipment && Row->Kind != EY01SourceEffectKind::AttackFlat && Row->Kind != EY01SourceEffectKind::GoldPerSecond))
        { return Fail(E::InvalidDefinition, TEXT("效果引用、重复身份或有限非负数值不合法")); }
        Seen.Add(Id.RowName); FY01ItemEffect Efx; Efx.EffectId = Id.RowName; Efx.Kind = Row->Kind; Efx.Magnitude = Row->Magnitude;
        Efx.SourceAttribute = Row->SourceAttribute; Efx.TargetAttribute = Row->TargetAttribute; Efx.Amount = Row->Amount;
        Out.Effects.Add(Efx);
    }
    Seen.Reset();
    for (const auto& Id : Growth)
    {
        const auto* Row = Catalog.FindRow<FY01GrowthRuleRow>(Id, Errors);
        if (Id.Type != EY01TableKind::GrowthRule || !Row || Seen.Contains(Id.RowName)
            || Row->Trigger != EY01GrowthTrigger::EffectiveKill || !FMath::IsFinite(Row->Delta) || Row->Delta < 0
            || !((Row->Target == EY01GrowthTarget::EquipmentInstance && Ref.Type == EY01TableKind::Equipment && Row->Attribute == EY01GrowthAttribute::AttackDamage)
                || (Row->Target == EY01GrowthTarget::Player && Row->Attribute == EY01GrowthAttribute::Strength)))
        { return Fail(E::InvalidDefinition, TEXT("成长引用、种类、目标或数值不合法")); }
        Seen.Add(Id.RowName); FY01ItemGrowth G;
        G.RuleId = Id.RowName; G.Trigger = Row->Trigger; G.Target = Row->Target; G.Attribute = Row->Attribute; G.Delta = Row->Delta;
        Out.Growth.Add(G);
    }
    Seen.Reset();
    for (const auto& Id : Damage)
    {
        const auto* Row = Catalog.FindRow<FY01DamageEffectRow>(Id, Errors);
        if (Id.Type != EY01TableKind::DamageEffect || !Row || Seen.Contains(Id.RowName) || !Row->IsValid())
        { return Fail(E::InvalidDefinition, TEXT("伤害效果引用、身份或模板配置无效")); }
        Seen.Add(Id.RowName); auto& Efx = Out.DamageEffects.AddDefaulted_GetRef(); Efx.EffectId = Id.RowName; Efx.Config = *Row;
    }
    if (!Y01Damage::ValidateSource(Out.DamageEffects, Ref.Type, Out.Skill.Type)
        || (Ref.Type == EY01TableKind::Skill && Out.Skill.Type == EY01SkillType::Active
            && ((!Out.DamageEffects.IsEmpty()) != (Out.Skill.Ability.ToSoftObjectPath() == FSoftObjectPath(UY01DamageAbility::StaticClass())))))
    { return Fail(E::InvalidDefinition, TEXT("伤害来源发动方式与原生主动GA不匹配")); }
    Out.DamageEffects.Sort([](const auto& A, const auto& B) { return A.EffectId.LexicalLess(B.EffectId); });
    Out.Effects.Sort([](const auto& A, const auto& B) { return A.EffectId.LexicalLess(B.EffectId); });
    Out.Growth.Sort([](const auto& A, const auto& B) { return A.RuleId.LexicalLess(B.RuleId); });
    return Fail(E::None, TEXT(""));
}
FY01ItemResult UY01LoadoutComponent::AddItems(const FY01DataCatalog& Catalog, const TArray<FY01ItemGrant>& Grants)
{
    if (!IsInGameThread()) { return Fail(E::Authority, TEXT("内部入包仅允许主线程")); }
    if (!Transactions.Enter()) { return Fail(E::Busy, TEXT("事务期间不能入包")); }
    ON_SCOPE_EXIT { Transactions.Leave(); PublishCommittedView(); };
    auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    if (bEnded || GameState->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { return Fail(E::Stopped, TEXT("会话已结束，不再入包")); }
    // 时钟也可能执行服务器回调，先拥有输入及所有静态行，再兑现旧段收入。
    FPreparedBatch Batch; const auto Prepared = PrepareBatch(Catalog, Grants, {}, Batch);
    double Now = 0; R = FromSource(Player->GetSourceEffects()->SettleMutationTime(Now)); if (!R.IsOk()) { return R; }
    if (!Prepared.IsOk()) { return Prepared; }
    R = CheckOwner(); if (!R.IsOk()) { return R; }
    if (Transactions.bInterrupted || bEnded || Revision != Batch.BeforeRevision) { return Fail(E::Interrupted, TEXT("入包准备期间会话中断")); }
    const auto Result = Batch.Result; ApplyBatch(MoveTemp(Batch)); return Result;
}
FY01ItemResult UY01LoadoutComponent::PrepareBatch(const FY01DataCatalog& Catalog, const TArray<FY01ItemGrant>& Input,
    const TArray<FY01ItemConsume>& Consumes, FPreparedBatch& Out)
{
    Out = {}; auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    if (!Transactions.bBusy) { return Fail(E::Busy, TEXT("批次准备必须持有库存事务")); }
    const auto Grants = Input; const auto Materials = Consumes;
    const bool Changed = !Grants.IsEmpty() || !Materials.IsEmpty();
    if ((Changed && (Revision == MAX_uint64 || CommittedView.ViewRevision == MAX_uint64))
        || Grants.Num() > MAX_int32 - Items.Num() || Grants.Num() > MAX_int32 - Materials.Num()
        || uint64(Grants.Num()) > MAX_uint64 - AcquisitionSerial)
    { return Fail(E::Range, TEXT("批次或完整视图版本、容器、获取序列范围耗尽")); }
    FPreparedBatch B; B.BeforeRevision = Revision; B.Result.Revision = Revision + (Changed ? 1 : 0);
    B.AcquisitionSerial = AcquisitionSerial; B.Result.Changes.Reserve(Grants.Num() + Materials.Num());
    TSet<FGuid> Removed;
    for (const auto& Material : Materials)
    {
        if (Removed.Contains(Material.ItemId)) { return Fail(E::DuplicateItem, TEXT("同一材料不可重复消耗")); }
        const auto* Item = Items.Find(Material.ItemId);
        if (!Item) { return Fail(E::MissingItem, TEXT("材料不属于本玩家背包")); }
        if (Item->EntityRevision != Material.ExpectedEntityRevision) { return Fail(E::StaleEntity, TEXT("材料实体版本过期")); }
        if (Item->Location != EY01ItemLocation::Backpack || Item->Slot != INDEX_NONE)
        { return Fail(E::InvalidLocation, TEXT("不能消耗已穿装备或已学技能")); }
        if (Item->EntityRevision == MAX_uint64) { return Fail(E::Range, TEXT("材料墓碑版本耗尽")); }
        Removed.Add(Material.ItemId);
        auto& Change = B.Result.Changes.AddDefaulted_GetRef(); Change.Kind = EY01ItemChangeKind::Removed; Change.Item = *Item;
        ++Change.Item.EntityRevision; Change.Item.LastChangedRevision = B.Result.Revision;
    }
    B.Items = Items; B.Order.Reserve(Order.Num() + Grants.Num());
    for (const auto& Id : Order) { if (Removed.Contains(Id)) { B.Items.Remove(Id); } else { B.Order.Add(Id); } }
    B.Items.Reserve(Items.Num() + Grants.Num());
    TMap<FName, FY01ItemInstance> EquipmentCache, SkillCache;
    for (const auto& Grant : Grants)
    {
        if (!Grant.ItemId.IsValid()) { return Fail(E::InvalidDefinition, TEXT("服务器物品Id必须有效")); }
        if (Items.Contains(Grant.ItemId) || B.Items.Contains(Grant.ItemId)) { return Fail(E::DuplicateItem, TEXT("新增物品身份不得重复或复用材料")); }
        auto& Cache = Grant.Definition.Type == EY01TableKind::Equipment ? EquipmentCache : SkillCache;
        auto* Definition = Cache.Find(Grant.Definition.RowName);
        if (!Definition || Definition->Definition.Type != Grant.Definition.Type)
        {
            FY01ItemInstance Value; R = Resolve(Catalog, Grant.Definition, Value); if (!R.IsOk()) { return R; }
            Definition = &Cache.Add(Grant.Definition.RowName, MoveTemp(Value));
        }
        auto Item = *Definition; Item.ItemId = Grant.ItemId; Item.AcquisitionOrder = ++B.AcquisitionSerial;
        Item.LastChangedRevision = B.Result.Revision;
        auto& Change = B.Result.Changes.AddDefaulted_GetRef(); Change.Kind = EY01ItemChangeKind::Added; Change.Item = Item;
        B.Order.Add(Item.ItemId); B.Items.Add(Item.ItemId, MoveTemp(Item));
    }
    Out = MoveTemp(B); return Fail(E::None, TEXT(""));
}
void UY01LoadoutComponent::ApplyBatch(FPreparedBatch&& B)
{
    Items = MoveTemp(B.Items); Order = MoveTemp(B.Order);
    AcquisitionSerial = B.AcquisitionSerial; Revision = B.Result.Revision;
}
FY01ItemResult UY01LoadoutComponent::CommitBatch(AController* C, const FY01ItemRequest* Input, FPreparedBatch&& Prepared,
    const TArray<FY01ResolvedSource>& Additions, const TArray<FY01RuntimeSourceId>& Removals, double Now,
    TFunctionRef<FY01ItemResult()> CheckCommit, TFunctionRef<void()> CommitOwner, bool bPassiveDelivery)
{
    // 只有可信协调者可调用；输入、注册表、结果和原回执均在暂存前拥有独立内存。
    auto B = MoveTemp(Prepared); const bool HasRequest = Input != nullptr;
    const FY01ItemRequest Request = HasRequest ? *Input : FY01ItemRequest(); auto ReceiptRequest = Request;
    auto Result = B.Result; auto Receipt = Result;
    auto Validate = [&]()
    {
        auto R = bPassiveDelivery ? CheckOwner() : CheckRequester(C); if (!R.IsOk()) { return R; }
        if (bPassiveDelivery)
        {
            if (HasRequest || C || !Additions.IsEmpty() || !Removals.IsEmpty())
            { return Fail(E::InvalidOperation, TEXT("被动交付不接受玩家意图或来源变动")); }
            if (bEnded || GameState->GetWaveSnapshot().Phase == EY01MatchPhase::Ended)
            { return Fail(E::Stopped, TEXT("被动交付不能跨局")); }
            R = FromSource(Player->GetSourceEffects()->Check(true));
        }
        else { R = CheckActive(); }
        if (!R.IsOk()) { return R; }
        if (!Transactions.bBusy || Transactions.bInterrupted || Revision != B.BeforeRevision)
        { return Fail(E::Interrupted, TEXT("批次期间重入或库存改变")); }
        return CheckCommit();
    };
    auto R = Validate(); if (!R.IsOk()) { return R; }
    auto* Sources = Player->GetSourceEffects(); FY01PreparedMutation Plan;
    R = FromSource(Sources->PrepareMutation(Additions, Removals, Plan)); if (!R.IsOk()) { return R; }
    FY01SourceTransaction Tx; R = FromSource(Sources->StageMutationAt(Plan, Tx, Now)); if (!R.IsOk()) { return R; }
    FY01ScopedSourceMutation Guard(MoveTemp(Tx)); R = Validate(); if (!R.IsOk()) { return R; }
    R = FromSource(Guard.CommitAt([&]
    {
        ApplyBatch(MoveTemp(B)); CommitOwner();
        if (HasRequest) { Transactions.Record(MoveTemp(ReceiptRequest), MoveTemp(Receipt)); }
    }));
    return R.IsOk() ? Result : R;
}
FY01RuntimeSourceId UY01LoadoutComponent::SourceId(const FGuid& Id, EY01RuntimeSourceKind Kind) const
{
    FY01RuntimeSourceId Source; Source.Kind = Kind;
    Source.LocalKey = Id.ToString(EGuidFormats::DigitsWithHyphens);
    Source.Id = FString::Printf(TEXT("R36/%s/P%d/%s/%s"), *MatchId.ToString(EGuidFormats::DigitsWithHyphens), JoinSequence,
        Kind == EY01RuntimeSourceKind::Skill ? TEXT("Skill") : TEXT("Equipment"), *Source.LocalKey);
    return Source;
}
FY01ItemResult UY01LoadoutComponent::CheckEntity(const FY01ItemRequest& R) const
{
    if (!EquipmentSlots.IsValidIndex(R.Slot)) { return Fail(E::InvalidSlot, TEXT("装备槽必须在六槽范围内")); }
    const auto* Item = Items.Find(R.ItemId);
    if (!Item) { return Fail(E::MissingItem, TEXT("本玩家注册表不存在此物品")); }
    if (Item->EntityRevision != R.ExpectedEntityRevision) { return Fail(E::StaleEntity, TEXT("请求实体版本已过期")); }
    const FGuid Occupant = EquipmentSlots[R.Slot];
    const auto* Old = Items.Find(Occupant);
    if (R.ExpectedSlotItemId != Occupant || R.ExpectedSlotEntityRevision != (Old ? Old->EntityRevision : 0))
    { return Fail(E::StaleSlot, TEXT("目标槽占用实例或版本不符合预期")); }
    if (Item->Definition.Type != EY01TableKind::Equipment) { return Fail(E::InvalidDefinition, TEXT("技能书不能装备")); }
    if ((R.Operation == EY01ItemOperation::Equip && Item->Location != EY01ItemLocation::Backpack)
        || (R.Operation == EY01ItemOperation::Unequip && (Item->Location != EY01ItemLocation::EquipmentSlot || Item->Slot != R.Slot || Occupant != R.ItemId)))
    { return Fail(E::InvalidLocation, TEXT("不能重复穿戴、双槽、身上直接换槽或从包卸下")); }
    if (Revision == MAX_uint64 || Item->EntityRevision == MAX_uint64 || (Old && Old->EntityRevision == MAX_uint64))
    { return Fail(E::Range, TEXT("库存或实体版本已耗尽")); }
    return Fail(E::None, TEXT(""));
}
FY01ItemResult UY01LoadoutComponent::TryEquip(AController* C, const FY01ItemRequest& R)
{ return Execute(C, R, EY01ItemOperation::Equip); }
FY01ItemResult UY01LoadoutComponent::TryUnequip(AController* C, const FY01ItemRequest& R)
{ return Execute(C, R, EY01ItemOperation::Unequip); }
FY01ItemResult UY01LoadoutComponent::Execute(AController* C, const FY01ItemRequest& Input, EY01ItemOperation Operation)
{
    if (!IsInGameThread()) { return Fail(E::Authority, TEXT("请求仅允许服务器主线程")); }
    // GAS委托可修改调用方变量；身份、复验、提交和防重必须共用入口自有快照。
    const FY01ItemRequest Request = Input;
    if (!Transactions.Enter()) { return Fail(E::Busy, TEXT("物品事务正在执行，拒绝重入")); }
    ON_SCOPE_EXIT { Transactions.Leave(); PublishCommittedView(); };
    auto R = CheckRequester(C); if (!R.IsOk()) { return R; }
    if (Request.MatchId != MatchId || Request.JoinSequence != JoinSequence)
    { return Fail(E::Session, TEXT("请求不属于本Match/Join，不污染防重账本")); }
    if (!Transactions.Admit(Request, Revision, R)) { return R; }
    // 后续所有业务错误都消费并缓存；成功在共同提交点写入原结果。
    R = Mutate(C, Request, Operation);
    if (Transactions.ExpectedSeq == Request.Seq && R.Code != E::Busy)
    { auto Receipt = R; Transactions.Record(Request, MoveTemp(Receipt)); }
    return R;
}
FY01ItemResult UY01LoadoutComponent::Mutate(AController* C, const FY01ItemRequest& Request, EY01ItemOperation Operation)
{
    auto R = CheckActive(); if (!R.IsOk()) { return R; }
    auto* Sources = Player->GetSourceEffects();
    double Now = 0; R = FromSource(Sources->SettleMutationTime(Now)); if (!R.IsOk()) { return R; }
    if (Request.Operation != Operation) { return Fail(E::InvalidOperation, TEXT("请求Operation与入口不符")); }
    if (Operation == EY01ItemOperation::Purchase) { return Player->GetEconomy()->MutatePurchase(C, Request, Now); }
    if (Operation == EY01ItemOperation::Synthesize) { return Player->GetEconomy()->MutateSynthesis(C, Request, Now); }
    if (Operation == EY01ItemOperation::ChooseTalent) { return Player->GetEconomy()->MutateChooseTalent(C, Request, Now); }
    if (Request.Offer.Type != EY01TableKind::None || !Request.Offer.RowName.IsNone() || !Request.Materials.IsEmpty()
        || Request.Recipe.Type != EY01TableKind::None || !Request.Recipe.RowName.IsNone()
        || Request.Talent.Type != EY01TableKind::None || !Request.Talent.RowName.IsNone() || Request.TalentChoiceId.IsValid())
    { return Fail(E::InvalidOperation, TEXT("装配或学习不接受经济意图")); }
    if (Operation != EY01ItemOperation::Equip && Operation != EY01ItemOperation::Unequip) { return MutateSkill(C, Request); }
    R = CheckEntity(Request); if (!R.IsOk()) { return R; }
    const uint64 BeforeRevision = Revision;
    FY01ItemResult Result; Result.Revision = Revision + 1;
    TArray<FY01ResolvedSource> Additions; TArray<FY01RuntimeSourceId> Removals;
    const auto Item = Items.FindChecked(Request.ItemId);
    auto Changed = [&](FY01ItemInstance Value, EY01ItemLocation Location, int32 Slot)
    {
        Value.Location = Location; Value.Slot = Slot; ++Value.EntityRevision; Value.LastChangedRevision = Result.Revision;
        FY01ItemChange Change; Change.Item = MoveTemp(Value); Result.Changes.Add(MoveTemp(Change));
    };
    if (Operation == EY01ItemOperation::Equip)
    {
        if (EquipmentSlots[Request.Slot].IsValid())
        {
            const auto Old = Items.FindChecked(EquipmentSlots[Request.Slot]);
            Removals.Add(SourceId(Old.ItemId)); Changed(Old, EY01ItemLocation::Backpack, INDEX_NONE);
        }
        FY01ResolvedSource Source; Source.Kind = EY01RuntimeSourceKind::Equipment; Source.LocalKey = SourceId(Item.ItemId).LocalKey;
        for (const auto& Effect : UY01GrowthComponent::EquipmentEffects(Item)) { Source.Effects.Add({Effect.EffectId, Effect.Kind, Effect.Magnitude, Effect.SourceAttribute, Effect.TargetAttribute, Effect.Amount}); }
        Additions.Add(MoveTemp(Source)); Changed(Item, EY01ItemLocation::EquipmentSlot, Request.Slot);
    }
    else { Removals.Add(SourceId(Item.ItemId)); Changed(Item, EY01ItemLocation::Backpack, INDEX_NONE); }
    // 保留三份预先准备的内存：注册表移动、原返回值、防重原结果；回调内不分配副本。
    auto Changes = Result.Changes; auto Receipt = Result;
    FY01PreparedMutation Plan;
    R = FromSource(Sources->PrepareMutation(Additions, Removals, Plan)); if (!R.IsOk()) { return R; }
    FY01SourceTransaction Tx;
    R = FromSource(Sources->StageMutationAt(Plan, Tx, Now)); if (!R.IsOk()) { return R; }
    FY01ScopedSourceMutation Guard(MoveTemp(Tx));
    R = CheckRequester(C); if (!R.IsOk()) { return R; }
    R = CheckActive(); if (!R.IsOk()) { return R; }
    R = CheckEntity(Request); if (!R.IsOk()) { return R; }
    if (Transactions.bInterrupted || Revision != BeforeRevision) { return Fail(E::Interrupted, TEXT("暂存期间发生业务重入或库存改变")); }
    R = FromSource(Guard.CommitAt([&]
    {
        for (auto& Change : Changes) { Items.FindChecked(Change.Item.ItemId) = MoveTemp(Change.Item); }
        EquipmentSlots[Request.Slot] = Operation == EY01ItemOperation::Equip ? Request.ItemId : FGuid();
        Revision = Result.Revision;
        Transactions.Record(Request, MoveTemp(Receipt));
    }));
    // 提交后的GE删除可能重入死亡或EndPlay；已线性化的结果不能被事后权限变化改成失败。
    return R.IsOk() ? Result : R;
}
FY01ItemResult UY01LoadoutComponent::Observe(FY01LoadoutSnapshot& Out) const
{
    Out = FY01LoadoutSnapshot();
    if (!IsInGameThread()) { return Fail(E::Authority, TEXT("观察仅允许服务器主线程")); }
    if (Transactions.bBusy || (Player.IsValid() && Player->GetSourceEffects()->bBusy)) { return Fail(E::Busy, TEXT("事务期间不发布半份库存")); }
    auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    Out.Revision = Revision; Out.ExpectedSeq = Transactions.ExpectedSeq; Out.EquipmentSlots = EquipmentSlots; Out.Items.Reserve(Order.Num());
    Out.Skills = Skills; Out.ConfigRevision = ConfigRevision;
    for (const auto& Id : Order) { Out.Items.Add(Items.FindChecked(Id)); }
    return R;
}
FY01ItemResult UY01LoadoutComponent::FindItem(const FGuid& Id, FY01ItemInstance& Out) const
{
    Out = FY01ItemInstance();
    if (!IsInGameThread()) { return Fail(E::Authority, TEXT("寻址仅允许服务器主线程")); }
    if (Transactions.bBusy || (Player.IsValid() && Player->GetSourceEffects()->bBusy)) { return Fail(E::Busy, TEXT("事务期间不发布半份实体")); }
    auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    const auto* Item = Items.Find(Id); if (!Item) { return Fail(E::MissingItem, TEXT("本玩家没有此物品")); }
    Out = *Item; return R;
}
FY01ItemResult UY01LoadoutComponent::PrepareLearn(AController* C, const FY01ItemRequest& R)
{ return Execute(C, R, EY01ItemOperation::PrepareLearn); }
FY01ItemResult UY01LoadoutComponent::ConfirmLearn(AController* C, const FY01ItemRequest& R)
{ return Execute(C, R, EY01ItemOperation::ConfirmLearn); }
FY01ItemResult UY01LoadoutComponent::CancelLearn(AController* C, const FY01ItemRequest& R)
{ return Execute(C, R, EY01ItemOperation::CancelLearn); }
FY01ItemResult UY01LoadoutComponent::RemoveSkill(AController* C, const FY01ItemRequest& R)
{ return Execute(C, R, EY01ItemOperation::RemoveSkill); }
FY01ItemResult UY01LoadoutComponent::SetConfigRevision(uint64 Next)
{
    if (!IsInGameThread()) { return Fail(E::Authority, TEXT("配置发布仅服务器主线程")); }
    if (!Transactions.Enter()) { return Fail(E::Busy, TEXT("事务期间不可发布配置")); }
    ON_SCOPE_EXIT { Transactions.Leave(); PublishCommittedView(); };
    auto R = CheckOwner(); if (!R.IsOk()) { return R; }
    if (bEnded || GameState->GetWaveSnapshot().Phase == EY01MatchPhase::Ended) { return Fail(E::Stopped, TEXT("会话已结束")); }
    if (Next <= ConfigRevision) { return Fail(E::StaleConfig, TEXT("配置纪元必须严格前进")); }
    ConfigRevision = Next; return Fail(E::None, TEXT(""));
}
bool UY01LoadoutComponent::CanActivate(FGameplayAbilitySpecHandle Handle) const
{
    if (Transactions.bBusy && Handle != ActivatingHandle) { return false; }
    if (!CheckActive().IsOk() || Player->GetSourceEffects()->bBusy) { return false; }
    const auto* Pawn = Cast<APawn>(Player->GetCombatASC()->GetAvatarActor());
    return Pawn && CheckRequester(Pawn->GetController()).IsOk();
}
bool UY01LoadoutComponent::BeginActivation(FGameplayAbilitySpecHandle Handle)
{
    if (!CanActivate(Handle) || !Handle.IsValid() || Transactions.bBusy || !Transactions.Enter()) { return false; }
    ActivatingHandle = Handle; return true;
}
void UY01LoadoutComponent::FinishActivation(FGameplayAbilitySpecHandle Handle)
{
    if (Handle == ActivatingHandle) { ActivatingHandle = FGameplayAbilitySpecHandle(); Transactions.Leave(); PublishCommittedView(); }
}
FY01ItemResult UY01LoadoutComponent::ResolveAbility(const FY01ItemInstance& Book, TSubclassOf<UGameplayAbility>& OutClass) const
{
    OutClass = nullptr;
    if (Book.Skill.Type == EY01SkillType::Passive) { return Fail(E::None, TEXT("")); }
    OutClass = Book.Skill.Ability.LoadSynchronous();
    if (!OutClass || OutClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists | CLASS_CompiledFromBlueprint))
    { return Fail(E::AbilityRejected, TEXT("主动技能必须具有可执行的原生GA类")); }
    const auto* Ability = Cast<UY01LoadoutAbility>(OutClass->GetDefaultObject());
    if (!Ability || !Ability->SupportsLoadoutSkill(Book.Skill.TestSkill)
        || ((!Book.DamageEffects.IsEmpty()) != (OutClass == UY01DamageAbility::StaticClass()))
        || Ability->GetInstancingPolicy() != EGameplayAbilityInstancingPolicy::InstancedPerActor
        || Ability->GetNetExecutionPolicy() != EGameplayAbilityNetExecutionPolicy::ServerOnly
        || Ability->GetNetSecurityPolicy() != EGameplayAbilityNetSecurityPolicy::ServerOnly)
    { return Fail(E::AbilityRejected, TEXT("GA未实现本服务器动态技能适配契约")); }
    return Fail(E::None, TEXT(""));
}
FY01ItemResult UY01LoadoutComponent::CheckSkillEntity(const FY01ItemRequest& R) const
{
    using O = EY01ItemOperation;
    if (!Skills.IsValidIndex(R.Slot)) { return Fail(E::InvalidSlot, TEXT("主动被动共用四槽")); }
    if (R.ConfigRevision != ConfigRevision) { return Fail(E::StaleConfig, TEXT("配置纪元已变化")); }
    const auto& Old = Skills[R.Slot];
    if (R.ExpectedLearnedId != Old.LearnedId || R.ExpectedLearnedRevision != Old.EntityRevision)
    { return Fail(E::StaleSlot, TEXT("已学实体或版本已经变化")); }
    if (R.ExpectedSlotItemId.IsValid() || R.ExpectedSlotEntityRevision != 0)
    { return Fail(E::InvalidOperation, TEXT("技能意图不得夹带装备槽身份")); }
    if (R.Operation == O::RemoveSkill)
    {
        if (R.ItemId.IsValid() || R.ExpectedEntityRevision != 0 || R.TicketId.IsValid())
        { return Fail(E::InvalidOperation, TEXT("移除不得夹带书或票据")); }
        if (!Old.LearnedId.IsValid()) { return Fail(E::InvalidLocation, TEXT("空槽不能移除")); }
    }
    else
    {
        const auto* Book = Items.Find(R.ItemId);
        if (!Book) { return Fail(E::MissingItem, TEXT("本玩家书已不存在")); }
        if (Book->EntityRevision != R.ExpectedEntityRevision) { return Fail(E::StaleEntity, TEXT("书实体版本已变化")); }
        if (Book->Definition.Type != EY01TableKind::Skill || Book->Location != EY01ItemLocation::Backpack)
        { return Fail(E::InvalidDefinition, TEXT("只能学习本包技能书")); }
        for (const auto& S : Skills)
        { if (S.LearnedId.IsValid() && S.SkillId == Book->Definition.RowName) { return Fail(E::DuplicateSkill, TEXT("任意槽含同槽均不得重复技能ID")); } }
        if (Book->EntityRevision == MAX_uint64) { return Fail(E::Range, TEXT("书版本耗尽")); }
    }
    if (Revision == MAX_uint64) { return Fail(E::Range, TEXT("库存版本耗尽")); }
    return Fail(E::None, TEXT(""));
}
FY01ItemResult UY01LoadoutComponent::MutateSkill(AController* C, const FY01ItemRequest& Request)
{
    using O = EY01ItemOperation;
    if (!Skills.IsValidIndex(Request.Slot)) { return Fail(E::InvalidSlot, TEXT("技能槽必须在四槽范围内")); }
    if (Request.Operation == O::ConfirmLearn || Request.Operation == O::CancelLearn)
    {
        auto Expected = Tickets[Request.Slot];
        Expected.Seq = Request.Seq; Expected.Operation = Request.Operation;
        if (!Request.TicketId.IsValid() || !FY01ItemTransactions::SamePayload(Expected, Request))
        { return Fail(E::StaleTicket, TEXT("票据不存在或与完整意图不符")); }
        if (Request.Operation == O::CancelLearn)
        { Tickets[Request.Slot] = FY01ItemRequest(); return Fail(E::None, TEXT("")); }
    }
    else if (Request.TicketId.IsValid()) { return Fail(E::InvalidOperation, TEXT("准备或移除不可夹带票据")); }
    auto R = CheckSkillEntity(Request); if (!R.IsOk()) { return R; }
    const bool Remove = Request.Operation == O::RemoveSkill;
    const auto Old = Skills[Request.Slot];
    FY01ItemInstance Book; TSubclassOf<UGameplayAbility> AbilityClass;
    if (!Remove)
    {
        Book = Items.FindChecked(Request.ItemId);
        R = ResolveAbility(Book, AbilityClass); if (!R.IsOk()) { return R; }
        R = CheckRequester(C); if (!R.IsOk()) { return R; }
        R = CheckActive(); if (!R.IsOk()) { return R; }
        R = CheckSkillEntity(Request); if (!R.IsOk()) { return R; }
        if (Transactions.bInterrupted) { return Fail(E::Interrupted, TEXT("加载期间事务重入")); }
        if (Request.Operation == O::PrepareLearn && Old.LearnedId.IsValid())
        {
            auto Ticket = Request; Ticket.TicketId = FGuid::NewGuid(); Tickets[Request.Slot] = Ticket;
            auto Result = Fail(E::None, TEXT("")); Result.TicketId = Ticket.TicketId; return Result;
        }
    }
    auto* ASC = Player->GetCombatASC(); auto* Sources = Player->GetSourceEffects();
    if (ASC->AbilityScopeLockCount != 0) { return Fail(E::Busy, TEXT("GAS授予列表正在使用，不能暂存")); }
    const auto* OldGrant = ASC->GrantedSources.FindByPredicate([&](const auto& G) { return Old.LearnedId.IsValid() && G.LearnedId == Old.LearnedId; });
    if (Old.LearnedId.IsValid() && (!OldGrant || OldGrant->Ability != Old.GAHandle
        || (Old.GAHandle.IsValid() && !ASC->FindAbilitySpecFromHandle(Old.GAHandle))))
    { return Fail(E::HandleLost, TEXT("旧GA身份丢失，拒绝伪造恢复")); }
    FY01ItemResult Result; Result.Revision = Revision + 1; Result.RemovedLearnedId = Old.LearnedId;
    FY01GrantedSource Grant;
    TArray<FY01ResolvedSource> Additions; TArray<FY01RuntimeSourceId> Removals;
    if (Old.LearnedId.IsValid()) { Removals.Add(SourceId(Old.LearnedId, EY01RuntimeSourceKind::Skill)); }
    if (!Remove)
    {
        auto& Next = Result.Learned; Next.LearnedId = FGuid::NewGuid(); Next.SkillId = Book.Definition.RowName;
        Next.Slot = Request.Slot; Next.EntityRevision = 1; Next.Skill = Book.Skill; Next.Effects = Book.Effects; Next.Growth = Book.Growth; Next.DamageEffects = Book.DamageEffects;
        Grant.Source = Book.Definition; Grant.LearnedId = Next.LearnedId; Grant.Slot = Request.Slot;
        Grant.SkillType = Book.Skill.Type; Grant.CooldownSeconds = Book.Skill.CooldownSeconds; Grant.TestSkill = Book.Skill.TestSkill;
        FY01ResolvedSource Source; Source.Kind = EY01RuntimeSourceKind::Skill; Source.LocalKey = SourceId(Next.LearnedId, Source.Kind).LocalKey;
        for (const auto& Effect : Book.Effects) { Source.Effects.Add({Effect.EffectId, Effect.Kind, Effect.Magnitude, Effect.SourceAttribute, Effect.TargetAttribute, Effect.Amount}); }
        Additions.Add(MoveTemp(Source));
        auto& Change = Result.Changes.AddDefaulted_GetRef(); Change.Kind = EY01ItemChangeKind::Removed; Change.Item = Book;
        ++Change.Item.EntityRevision; Change.Item.LastChangedRevision = Result.Revision;
    }
    FY01PreparedMutation Plan; R = FromSource(Sources->PrepareMutation(Additions, Removals, Plan)); if (!R.IsOk()) { return R; }
    double Now = 0; R = FromSource(Sources->SettleMutationTime(Now)); if (!R.IsOk()) { return R; }
    FY01SourceTransaction Tx; R = FromSource(Sources->StageMutationAt(Plan, Tx, Now)); if (!R.IsOk()) { return R; }
    FY01ScopedSourceMutation Guard(MoveTemp(Tx));
    bool Committed = false;
    ON_SCOPE_EXIT { if (!Committed && Grant.Ability.IsValid()) { ASC->ClearAbility(Grant.Ability); } };
    if (AbilityClass)
    {
        FGameplayAbilitySpec Spec(AbilityClass, 1, INDEX_NONE, this); Grant.Ability = Spec.Handle;
        const auto Given = ASC->GiveAbility(Spec);
        const auto* Live = ASC->FindAbilitySpecFromHandle(Grant.Ability);
        if (Given != Grant.Ability || !Live || Live->PendingRemove || Live->Ability->GetClass() != AbilityClass)
        { return Fail(E::AbilityRejected, TEXT("真实GA授予未保留可执行句柄")); }
        Result.Learned.GAHandle = Grant.Ability;
    }
    R = CheckRequester(C); if (!R.IsOk()) { return R; }
    R = CheckActive(); if (!R.IsOk()) { return R; }
    R = CheckSkillEntity(Request); if (!R.IsOk()) { return R; }
    if (Transactions.bInterrupted) { return Fail(E::Interrupted, TEXT("GA/GE暂存期间重入")); }
    if (Old.GAHandle.IsValid() && !ASC->FindAbilitySpecFromHandle(Old.GAHandle))
    { return Fail(E::HandleLost, TEXT("暂存期间旧GA丢失")); }
    auto Next = Result.Learned; auto Receipt = Result;
    ASC->GrantedSources.Reserve(ASC->GrantedSources.Num() + (Remove ? 0 : 1));
    R = FromSource(Guard.CommitAt([&]
    {
        // 同一无委托区间提交来源、动态身份、书和回执；退休GA在此区间之后处理。
        ASC->GrantedSources.RemoveAll([&](const auto& G) { return Old.LearnedId.IsValid() && G.LearnedId == Old.LearnedId; });
        if (!Remove)
        { ASC->GrantedSources.Add(MoveTemp(Grant)); Items.Remove(Book.ItemId); Order.RemoveSingle(Book.ItemId); }
        Skills[Request.Slot] = MoveTemp(Next); Tickets[Request.Slot] = FY01ItemRequest();
        Revision = Result.Revision; Transactions.Record(Request, MoveTemp(Receipt)); Committed = true;
    }));
    if (!R.IsOk()) { return R; }
    if (Old.GAHandle.IsValid()) { ASC->ClearAbility(Old.GAHandle); }
    return Result;
}
void UY01LoadoutComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    if (bEnded) { return; }
    bEnded = true; Transactions.bInterrupted = true; SetActiveFlag(false);
    if (Player.IsValid()) { Player->GetDamageEffects()->ResetCombat(); }
    // 留存最终账本供已提交结果和收尾观察；不迁移至下一会话。
    if (HasBegunPlay()) { Super::EndPlay(Reason); }
}
