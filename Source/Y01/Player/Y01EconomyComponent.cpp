#include "Player/Y01EconomyComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Match/Y01GameFlow.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include <cmath>

namespace
{
using K = EY01TableKind;
template<class T> const T* CopyRow(const FY01DataCatalog& Catalog, const FY01DataRef& Ref,
    TMap<K, TObjectPtr<UDataTable>>& Tables, TArray<TStrongObjectPtr<UDataTable>>& Owners)
{
    FY01DataValidation Errors;
    const auto* Row = Catalog.FindRow<T>(Ref, Errors); if (!Row) { return nullptr; }
    auto& Table = Tables.FindOrAdd(Ref.Type);
    if (!Table) { Table = NewObject<UDataTable>(); Table->RowStruct = T::StaticStruct(); Owners.Emplace(Table.Get()); }
    Table->AddRow(Ref.RowName, *Row); return Table->FindRow<T>(Ref.RowName, TEXT("ShopSnapshot"));
}
bool Empty(const FY01DataRef& Ref) { return Ref.Type == K::None && Ref.RowName.IsNone(); }
template<class T> EY01ItemError Weights(const TArray<T>& Entries)
{
    using E = EY01ItemError; double Total = 0;
    for (const auto& Entry : Entries)
    {
        const double W = Entry.Weight, Sum = Total + W;
        if (!FMath::IsFinite(W) || W < 0 || !FMath::IsFinite(Sum)) { return E::InvalidDefinition; }
        if (W > 0 && Total > 0 && (Sum == Total || Sum == W)) { return E::Precision; }
        Total = Sum;
    }
    if (Total <= 0) { return E::InvalidDefinition; }
    // GetFraction只有23位票域；每个正区间必须含至少一个可表示票值。
    double Prefix = 0;
    for (const auto& Entry : Entries)
    {
        const double End = Prefix + Entry.Weight;
        if (Entry.Weight > 0 && std::ceil(Prefix / Total * 8388608.) >= std::ceil(End / Total * 8388608.)) { return E::Precision; }
        Prefix = End;
    }
    return E::None;
}
}

UY01EconomyComponent::UY01EconomyComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
    bAutoActivate = false;
}
FY01ItemResult UY01EconomyComponent::Initialize(const FGuid& Match, int32 Join, uint64 Config,
    const FY01DataCatalog& Catalog, const TArray<FY01DataRef>& EnabledOffers,
    const TArray<FY01DataRef>& EnabledRecipes, const FY01DataRef& TalentChoice, const TArray<FY01DataRef>& DropMonsters)
{
    using E = EY01ItemError;
    auto* P = Cast<AY01PlayerState>(GetOwner());
    if (!IsInGameThread() || !IsValid(P) || !P->HasAuthority() || P->GetEconomy() != this)
    { return FY01ItemResult::Fail(E::Authority, 0, TEXT("经济只能属于唯一权威PlayerState")); }
    auto* L = P->GetLoadout();
    if (!L->Transactions.Enter()) { return L->Fail(E::Busy, TEXT("经济复用库存事务锁")); }
    ON_SCOPE_EXIT { L->Transactions.Leave(); L->PublishCommittedView(); };
    auto R = L->CheckOwner(); if (!R.IsOk()) { return R; }
    if (!Match.IsValid() || Join <= 0 || Match != L->MatchId || Join != L->JoinSequence
        || (bInitialized && (Match != MatchId || Join != JoinSequence)))
    { return L->Fail(E::Session, TEXT("经济与库存必须属于同一Match/Join")); }
    if (Config != L->ConfigRevision || (bInitialized && Config != ConfigRevision))
    { return L->Fail(E::StaleConfig, TEXT("经济配置纪元不一致")); }
    if (bEnded || L->bEnded || L->GameState->GetWaveSnapshot().Phase == EY01MatchPhase::Ended)
    { return L->Fail(E::Stopped, TEXT("已结束经济不能重启")); }
    if (bInitialized) { return R; }
    R = L->FromSource(P->GetSourceEffects()->Check()); if (!R.IsOk()) { return R; }
    R = L->FromSource(P->GetSourceEffects()->ValidateHandles()); if (!R.IsOk()) { return R; }
    if (L->CommittedView.ViewRevision == MAX_uint64) { return L->Fail(E::Range, TEXT("经济初始化需要可提交的完整视图版本")); }
    TMap<K, TObjectPtr<UDataTable>> Tables; TArray<TStrongObjectPtr<UDataTable>> Owners;
    TMap<FName, FY01ShopOfferRow> Published; TMap<FName, FY01ShopOfferState> States;
    TArray<TObjectPtr<UClass>> Classes;
    const auto RecipeRefs = EnabledRecipes; const auto ChoiceRef = TalentChoice;
    TMap<FName, FY01SynthesisRow> PublishedRecipes; FY01TalentChoiceState ChoiceState;
    auto CopyDependencies = [&](const TArray<FY01DataRef>& Effects, const TArray<FY01DataRef>& Growth,
        const TArray<FY01ItemDamageEffect>& Damage = TArray<FY01ItemDamageEffect>{})
    {
        for (const auto& Id : Effects)
        { if (!CopyRow<FY01SourceEffectRow>(Catalog, Id, Tables, Owners)) { return L->Fail(E::InvalidDefinition, TEXT("效果快照失败")); } }
        for (const auto& Id : Growth)
        { if (!CopyRow<FY01GrowthRuleRow>(Catalog, Id, Tables, Owners)) { return L->Fail(E::InvalidDefinition, TEXT("成长快照失败")); } }
        for (const auto& Effect : Damage)
        {
            FY01DataRef Id; Id.Type = K::DamageEffect; Id.RowName = Effect.EffectId;
            if (!CopyRow<FY01DamageEffectRow>(Catalog, Id, Tables, Owners)) { return L->Fail(E::InvalidDefinition, TEXT("伤害模板快照失败")); }
        }
        return L->Fail(E::None, TEXT(""));
    };
    auto CopyPool = [&](const FY01DataRef& Ref, K Type, EY01Quality Quality, bool RequireDisplay = false)
    {
        const auto* Found = CopyRow<FY01LocalItemPoolRow>(Catalog, Ref, Tables, Owners);
        if (Ref.Type != K::LocalItemPool || !Found || Found->Quality != Quality || (Type != K::None && Found->ItemType != Type)
            || (Found->ItemType != K::Equipment && Found->ItemType != K::Skill) || static_cast<uint8>(Quality) > static_cast<uint8>(EY01Quality::Red))
        { return L->Fail(E::InvalidDefinition, TEXT("商品池缺失、类型或品质不符")); }
        const auto Pool = *Found; auto Code = Weights(Pool.Entries);
        if (Code != E::None) { return L->Fail(Code, TEXT("商品池权重或23位票域精度无效")); }
        for (const auto& Entry : Pool.Entries)
        {
            FY01ItemInstance Definition; auto Check = L->Resolve(Catalog, Entry.Item, Definition); if (!Check.IsOk()) { return Check; }
            if (Entry.Item.Type != Pool.ItemType || Definition.Quality != Pool.Quality || (RequireDisplay && Definition.DisplayName.IsEmpty()))
            { return L->Fail(E::InvalidDefinition, TEXT("池内定义类型或品质不符")); }
            for (const auto& Effect : Definition.Effects)
            {
                if (Effect.Kind == EY01SourceEffectKind::AttackFlat
                    && (Effect.Magnitude > MAX_flt || (Effect.Magnitude > 0 && static_cast<float>(Effect.Magnitude) == 0)))
                { return L->Fail(E::Precision, TEXT("随机商品攻击无法表示为float")); }
            }
            TArray<FY01DataRef> Effects, Growth;
            if (Entry.Item.Type == K::Equipment)
            {
                const auto* Item = CopyRow<FY01EquipmentRow>(Catalog, Entry.Item, Tables, Owners);
                if (!Item) { return L->Fail(E::InvalidDefinition, TEXT("装备快照失败")); }
                Effects = Item->EffectIds; Growth = Item->GrowthRuleIds;
            }
            else
            {
                const auto* Item = CopyRow<FY01SkillRow>(Catalog, Entry.Item, Tables, Owners);
                if (!Item) { return L->Fail(E::InvalidDefinition, TEXT("技能书快照失败")); }
                Effects = Item->EffectIds; Growth = Item->GrowthRuleIds;
                TSubclassOf<UGameplayAbility> Class; Check = L->ResolveAbility(Definition, Class); if (!Check.IsOk()) { return Check; }
                if (Class) { Classes.AddUnique(Class.Get()); }
            }
            Check = CopyDependencies(Effects, Growth, Definition.DamageEffects); if (!Check.IsOk()) { return Check; }
        }
        return L->Fail(E::None, TEXT(""));
    };
    for (const auto& Ref : EnabledOffers)
    {
        FY01DataValidation Errors; const auto* Found = Catalog.FindRow<FY01ShopOfferRow>(Ref, Errors);
        if (Ref.Type != K::ShopOffer || !Found || Published.Contains(Ref.RowName) || Found->Price < 0 || Found->Quantity <= 0
            || static_cast<uint8>(Found->ShopKind) > static_cast<uint8>(EY01ShopKind::RandomTier)
            || static_cast<uint8>(Found->Quality) > static_cast<uint8>(EY01Quality::Red))
        { return L->Fail(E::InvalidDefinition, TEXT("公开商品身份、价格、数量或种类无效")); }
        const auto Row = *Found; Published.Add(Ref.RowName, Row);
        FY01ShopOfferState State; State.OfferId = Ref.RowName; State.DisplayName = Row.DisplayName;
        State.ShopKind = Row.ShopKind; State.Price = Row.Price; State.Quantity = Row.Quantity;
        if (Row.ShopKind == EY01ShopKind::DirectEffect)
        {
            if (!Empty(Row.Pool) || !Empty(Row.TierPool) || Row.Quantity != 1 || Row.Quality != EY01Quality::Green
                || (Row.EffectIds.IsEmpty() && Row.GrowthRuleIds.IsEmpty()))
            { return L->Fail(E::InvalidDefinition, TEXT("DirectEffect字段组合无效")); }
            FY01RuntimeSourceId Id;
            R = L->FromSource(P->GetSourceEffects()->MakeId(EY01RuntimeSourceKind::Shop, Ref.RowName.ToString().ToLower(), Id));
            if (!R.IsOk()) { return R; } State.SourceKey = Id.LocalKey; State.SourceId = Id.Id;
            TSet<FName> Seen;
            for (const auto& Effect : Row.EffectIds)
            {
                const auto* Value = Catalog.FindRow<FY01SourceEffectRow>(Effect, Errors);
                if (Effect.Type != K::SourceEffect || !Value || Seen.Contains(Effect.RowName)
                    || (Value->Kind != EY01SourceEffectKind::AttackFlat && Value->Kind != EY01SourceEffectKind::GoldPerSecond)
                    || !Y01Data::ValidSourceEffect(*Value))
                { return L->Fail(E::InvalidDefinition, TEXT("直接效果引用、身份或数值无效")); }
                if (Value->Kind == EY01SourceEffectKind::AttackFlat && (Value->Magnitude > MAX_flt || (Value->Magnitude > 0 && static_cast<float>(Value->Magnitude) == 0)))
                { return L->Fail(E::Precision, TEXT("直接攻击无法表示为float")); }
                Seen.Add(Effect.RowName); FY01ItemEffect Efx; Efx.EffectId = Effect.RowName; Efx.Kind = Value->Kind; Efx.Magnitude = Value->Magnitude; State.Effects.Add(Efx);
            }
            Seen.Reset();
            for (const auto& Rule : Row.GrowthRuleIds)
            {
                const auto* Value = Catalog.FindRow<FY01GrowthRuleRow>(Rule, Errors);
                if (Rule.Type != K::GrowthRule || !Value || Seen.Contains(Rule.RowName) || Value->Trigger != EY01GrowthTrigger::EffectiveKill
                    || Value->Target != EY01GrowthTarget::Player || Value->Attribute != EY01GrowthAttribute::Strength
                    || !FMath::IsFinite(Value->Delta) || Value->Delta < 0)
                { return L->Fail(E::InvalidDefinition, TEXT("直接商品仅支持玩家力量成长规则")); }
                Seen.Add(Rule.RowName); FY01ItemGrowth G; G.RuleId = Rule.RowName; G.Trigger = Value->Trigger;
                G.Target = Value->Target; G.Attribute = Value->Attribute; G.Delta = Value->Delta; State.Growth.Add(G);
            }
            State.Effects.Sort([](const auto& A, const auto& B) { return A.EffectId.LexicalLess(B.EffectId); });
            State.Growth.Sort([](const auto& A, const auto& B) { return A.RuleId.LexicalLess(B.RuleId); });
            R = CopyDependencies(Row.EffectIds, Row.GrowthRuleIds); if (!R.IsOk()) { return R; }
        }
        else
        {
            if (!Row.EffectIds.IsEmpty() || !Row.GrowthRuleIds.IsEmpty()) { return L->Fail(E::InvalidDefinition, TEXT("随机商品不能混入直接效果")); }
            if (Row.ShopKind == EY01ShopKind::RandomPool)
            {
                if (!Empty(Row.TierPool)) { return L->Fail(E::InvalidDefinition, TEXT("RandomPool不能混入TierPool")); }
                R = CopyPool(Row.Pool, K::None, Row.Quality); if (!R.IsOk()) { return R; }
            }
            else
            {
                const auto* Tier = CopyRow<FY01QualityRollPoolRow>(Catalog, Row.TierPool, Tables, Owners);
                if (!Empty(Row.Pool) || Row.Quality != EY01Quality::Green || Row.TierPool.Type != K::QualityRollPool || !Tier
                    || (Tier->ItemType != K::Equipment && Tier->ItemType != K::Skill))
                { return L->Fail(E::InvalidDefinition, TEXT("RandomTier字段或品质池类型无效")); }
                const auto Code = Weights(Tier->Entries); if (Code != E::None) { return L->Fail(Code, TEXT("品质权重或23位票域精度无效")); }
                TSet<EY01Quality> Seen;
                for (const auto& Entry : Tier->Entries)
                {
                    if (Seen.Contains(Entry.Quality)) { return L->Fail(E::InvalidDefinition, TEXT("品质池不允许重复品质")); }
                    Seen.Add(Entry.Quality); R = CopyPool(Entry.Pool, Tier->ItemType, Entry.Quality); if (!R.IsOk()) { return R; }
                }
            }
        }
        States.Add(Ref.RowName, MoveTemp(State));
    }
    // 怪物支持集及依赖与商店共享只读目录，但不共享池选择或随机流。
    TSet<FName> MonsterIds;
    for (const auto& Ref : DropMonsters)
    {
        const auto* Found = CopyRow<FY01MonsterRow>(Catalog, Ref, Tables, Owners);
        if (Ref.Type != K::Monster || !Found || MonsterIds.Contains(Ref.RowName)
            || !FMath::IsFinite(Found->ItemDropChance) || Found->ItemDropChance < 0 || Found->ItemDropChance > 1)
        { return L->Fail(E::InvalidDefinition, TEXT("掉落怪物支持集或概率无效")); }
        MonsterIds.Add(Ref.RowName); const auto Monster = *Found;
        if (Monster.ItemDropChance == 0) { continue; }
        const auto* Package = CopyRow<FY01RewardPackageRow>(Catalog, Monster.DropReward, Tables, Owners);
        if (Monster.DropReward.Type != K::RewardPackage || !Package || Package->Domain != EY01RewardDomain::MatchPersonal
            || Package->Gold != 0 || Package->Experience != 0 || Package->Items.IsEmpty())
        { return L->Fail(E::InvalidDefinition, TEXT("正概率掉落包必须为个人局内纯物品包")); }
        const auto Reward = *Package; int64 Quantity = 0;
        for (const auto& Entry : Reward.Items)
        {
            Quantity += Entry.Quantity;
            if (Entry.Quantity <= 0 || Quantity > MAX_int32)
            { return L->Fail(E::Range, TEXT("掉落数量超出可表示范围")); }
            if (Entry.Item.Type == K::LocalItemPool)
            {
                FY01DataValidation Errors; const auto* Pool = Catalog.FindRow<FY01LocalItemPoolRow>(Entry.Item, Errors);
                if (!Pool) { return L->Fail(E::InvalidDefinition, TEXT("掉落池不存在")); }
                R = CopyPool(Entry.Item, Pool->ItemType, Pool->Quality, true); if (!R.IsOk()) { return R; }
                continue;
            }
            FY01ItemInstance Definition; R = L->Resolve(Catalog, Entry.Item, Definition); if (!R.IsOk()) { return R; }
            if (Definition.DisplayName.IsEmpty()) { return L->Fail(E::InvalidDefinition, TEXT("掉落显示名为空")); }
            TArray<FY01DataRef> Effects, Growth;
            if (Entry.Item.Type == K::Equipment)
            {
                const auto* Item = CopyRow<FY01EquipmentRow>(Catalog, Entry.Item, Tables, Owners);
                if (!Item) { return L->Fail(E::InvalidDefinition, TEXT("掉落装备快照失败")); }
                Effects = Item->EffectIds; Growth = Item->GrowthRuleIds;
            }
            else
            {
                const auto* Item = CopyRow<FY01SkillRow>(Catalog, Entry.Item, Tables, Owners);
                if (!Item) { return L->Fail(E::InvalidDefinition, TEXT("掉落书快照失败")); }
                Effects = Item->EffectIds; Growth = Item->GrowthRuleIds;
                TSubclassOf<UGameplayAbility> Class; R = L->ResolveAbility(Definition, Class); if (!R.IsOk()) { return R; }
                if (Class) { Classes.AddUnique(Class.Get()); }
            }
            R = CopyDependencies(Effects, Growth, Definition.DamageEffects); if (!R.IsOk()) { return R; }
        }
    }
    TSet<uint16> RecipeKinds;
    for (const auto& Ref : RecipeRefs)
    {
        const auto* Found = CopyRow<FY01SynthesisRow>(Catalog, Ref, Tables, Owners);
        if (Ref.Type != K::Synthesis || !Found || Ref.RowName.IsNone() || PublishedRecipes.Contains(Ref.RowName)
            || (Found->InputType != K::Equipment && Found->InputType != K::Skill)
            || static_cast<uint8>(Found->InputQuality) >= static_cast<uint8>(EY01Quality::Red)
            || Found->InputCount != Y01Data::SynthesisInputCount)
        { return L->Fail(E::InvalidDefinition, TEXT("合成只允许同类同品质五件升级，红色无配方")); }
        const auto Row = *Found;
        const uint16 Key = uint16(static_cast<uint8>(Row.InputType)) * 256 + static_cast<uint8>(Row.InputQuality);
        if (RecipeKinds.Contains(Key)) { return L->Fail(E::InvalidDefinition, TEXT("启用配方的类型品质必须唯一")); }
        RecipeKinds.Add(Key);
        R = CopyPool(Row.OutputPool, Row.InputType, static_cast<EY01Quality>(static_cast<uint8>(Row.InputQuality) + 1), true);
        if (!R.IsOk()) { return R; } PublishedRecipes.Add(Ref.RowName, Row);
    }
    if (!Empty(ChoiceRef))
    {
        const auto* Found = CopyRow<FY01TalentChoiceRow>(Catalog, ChoiceRef, Tables, Owners);
        if (ChoiceRef.Type != K::TalentChoice || !Found || Found->Candidates.Num() != Y01Data::TalentCandidateCount)
        { return L->Fail(E::InvalidDefinition, TEXT("服务器三选一必须显式引用现有三个候选配置")); }
        const auto Choice = *Found; TSet<FName> SeenTalents;
        FY01RuntimeSourceId Source;
        R = L->FromSource(P->GetSourceEffects()->MakeId(EY01RuntimeSourceKind::Talent, ChoiceRef.RowName.ToString().ToLower(), Source));
        if (!R.IsOk()) { return R; }
        ChoiceState.Definition = ChoiceRef; ChoiceState.SourceKey = Source.LocalKey; ChoiceState.SourceId = Source.Id;
        for (const auto& Ref : Choice.Candidates)
        {
            const auto* Talent = CopyRow<FY01TalentRow>(Catalog, Ref, Tables, Owners);
            if (Ref.Type != K::Talent || !Talent || SeenTalents.Contains(Ref.RowName) || Talent->DisplayName.IsEmpty()
                || static_cast<uint8>(Talent->Quality) > static_cast<uint8>(EY01Quality::Red)
                || !Talent->Ability.IsNull() || !Talent->Effect.IsNull() || (Talent->EffectIds.IsEmpty() && Talent->GrowthRuleIds.IsEmpty()))
            { return L->Fail(E::InvalidDefinition, TEXT("天赋须三个不同有效显示身份，仅支持效果和玩家成长快照")); }
            const auto Row = *Talent; SeenTalents.Add(Ref.RowName);
            FY01TalentCandidate Candidate; Candidate.TalentId = Ref.RowName; Candidate.DisplayName = Row.DisplayName; Candidate.Quality = Row.Quality;
            TSet<FName> Seen; FY01DataValidation Errors;
            for (const auto& RefEffect : Row.EffectIds)
            {
                const auto* Efx = Catalog.FindRow<FY01SourceEffectRow>(RefEffect, Errors);
                if (RefEffect.Type != K::SourceEffect || !Efx || Seen.Contains(RefEffect.RowName)
                    || (Efx->Kind != EY01SourceEffectKind::AttackFlat && Efx->Kind != EY01SourceEffectKind::GoldPerSecond)
                    || !Y01Data::ValidSourceEffect(*Efx))
                { return L->Fail(E::InvalidDefinition, TEXT("天赋效果引用或数值无效")); }
                if (Efx->Kind == EY01SourceEffectKind::AttackFlat && (Efx->Magnitude > MAX_flt || (Efx->Magnitude > 0 && static_cast<float>(Efx->Magnitude) == 0)))
                { return L->Fail(E::Precision, TEXT("天赋攻击不能表示为float")); }
                Seen.Add(RefEffect.RowName); FY01ItemEffect Value; Value.EffectId = RefEffect.RowName; Value.Kind = Efx->Kind; Value.Magnitude = Efx->Magnitude;
                Candidate.Effects.Add(Value);
            }
            Seen.Reset();
            for (const auto& RefGrowth : Row.GrowthRuleIds)
            {
                const auto* Rule = Catalog.FindRow<FY01GrowthRuleRow>(RefGrowth, Errors);
                if (RefGrowth.Type != K::GrowthRule || !Rule || Seen.Contains(RefGrowth.RowName) || Rule->Trigger != EY01GrowthTrigger::EffectiveKill
                    || Rule->Target != EY01GrowthTarget::Player || Rule->Attribute != EY01GrowthAttribute::Strength
                    || !FMath::IsFinite(Rule->Delta) || Rule->Delta < 0)
                { return L->Fail(E::InvalidDefinition, TEXT("天赋仅支持玩家力量成长快照")); }
                Seen.Add(RefGrowth.RowName); FY01ItemGrowth Value; Value.RuleId = RefGrowth.RowName; Value.Trigger = Rule->Trigger;
                Value.Target = Rule->Target; Value.Attribute = Rule->Attribute; Value.Delta = Rule->Delta; Candidate.Growth.Add(Value);
            }
            Candidate.Effects.Sort([](const auto& A, const auto& B) { return A.EffectId.LexicalLess(B.EffectId); });
            Candidate.Growth.Sort([](const auto& A, const auto& B) { return A.RuleId.LexicalLess(B.RuleId); });
            R = CopyDependencies(Row.EffectIds, Row.GrowthRuleIds); if (!R.IsOk()) { return R; }
            ChoiceState.Candidates.Add(MoveTemp(Candidate));
        }
        // 固定Choice就是服务器选择，不需要抽样；旧刷新字段不参与新模式。
        ChoiceState.ChoiceId = FGuid::NewGuid();
    }
    R = L->CheckOwner(); if (!R.IsOk()) { return R; }
    if (L->Transactions.bInterrupted || bEnded || L->bEnded || L->ConfigRevision != Config || L->GameState->GetWaveSnapshot().Phase == EY01MatchPhase::Ended)
    { return L->Fail(E::Interrupted, TEXT("商品依赖加载期间会话改变")); }
    R = L->FromSource(P->GetSourceEffects()->Check()); if (!R.IsOk()) { return R; }
    R = L->FromSource(P->GetSourceEffects()->ValidateHandles()); if (!R.IsOk()) { return R; }
    SnapshotTables = MoveTemp(Tables); Offers = MoveTemp(Published); OfferStates = MoveTemp(States); SnapshotClasses = MoveTemp(Classes);
    Recipes = MoveTemp(PublishedRecipes); TalentState = MoveTemp(ChoiceState);
    for (const auto& Ref : EnabledOffers) { OfferOrder.Add(Ref.RowName); }
    for (const auto& Ref : RecipeRefs) { RecipeOrder.Add(Ref.RowName); }
    // 熵仅由服务器生成，三流各自持有状态；重复Initialize在此之前返回。
    ShoppingRandom.Initialize(static_cast<int32>(FGuid::NewGuid().A));
    SynthesisRandom.Initialize(static_cast<int32>(FGuid::NewGuid().B));
    LootRandom.Initialize(static_cast<int32>(FGuid::NewGuid().C));
    MatchId = Match; JoinSequence = Join; ConfigRevision = Config;
    bInitialized = true; SetActiveFlag(true); return R;
}
FY01DataCatalog UY01EconomyComponent::SnapshotCatalog() const
{
    FY01TableMap Tables; for (const auto& Pair : SnapshotTables) { Tables.Add(Pair.Key, Pair.Value.Get()); }
    return FY01DataCatalog(Tables);
}
FY01ItemResult UY01EconomyComponent::TryPurchase(AController* Requester, const FY01ItemRequest& Request)
{
    auto* P = Cast<AY01PlayerState>(GetOwner());
    if (!P || P->GetEconomy() != this) { return FY01ItemResult::Fail(EY01ItemError::Authority, 0, TEXT("非唯一经济组件")); }
    return P->GetLoadout()->SubmitRequest(Requester, Request);
}
FY01ItemResult UY01EconomyComponent::MutatePurchase(AController* Requester, const FY01ItemRequest& Request, double Now)
{
    using E = EY01ItemError;
    auto* P = CastChecked<AY01PlayerState>(GetOwner()); auto* L = P->GetLoadout(); auto* Income = P->GetIncome();
    auto CheckSession = [&]()
    {
        if (!bInitialized) { return L->Fail(E::Disabled, TEXT("经济尚未显式启用")); }
        if (bEnded) { return L->Fail(E::Stopped, TEXT("经济会话已结束")); }
        if (MatchId != L->MatchId || JoinSequence != L->JoinSequence)
        { return L->Fail(E::Session, TEXT("经济会话不一致")); }
        if (Request.ConfigRevision != ConfigRevision || ConfigRevision != L->ConfigRevision)
        { return L->Fail(E::StaleConfig, TEXT("商品配置已过期")); }
        return L->Fail(E::None, TEXT(""));
    };
    auto R = CheckSession(); if (!R.IsOk()) { return R; }
    if (!Empty(Request.Recipe) || !Empty(Request.Talent) || Request.TalentChoiceId.IsValid()
        || !Request.Materials.IsEmpty() || Request.ItemId.IsValid() || Request.ExpectedEntityRevision != 0
        || Request.Slot != INDEX_NONE || Request.ExpectedSlotItemId.IsValid() || Request.ExpectedSlotEntityRevision != 0
        || Request.ExpectedLearnedId.IsValid() || Request.ExpectedLearnedRevision != 0 || Request.TicketId.IsValid())
    { return L->Fail(E::InvalidOperation, TEXT("购买只接收商品意图，不接受材料或装配意图")); }
    const auto* Found = Request.Offer.Type == K::ShopOffer ? Offers.Find(Request.Offer.RowName) : nullptr;
    if (!Found) { return L->Fail(E::InvalidDefinition, TEXT("商品未由服务器显式公开")); }
    const auto Offer = *Found;
    const bool Direct = Offer.ShopKind == EY01ShopKind::DirectEffect;
    if (Direct)
    {
        const auto& State = OfferStates.FindChecked(Request.Offer.RowName);
        if (State.bPurchased) { return L->Fail(E::PurchaseLimit, TEXT("此商品本玩家本局已经购买")); }
        if (P->GetSourceEffects()->Records.ContainsByPredicate([&](const auto& Record) { return Record.Source.Id == State.SourceId; }))
        { return L->Fail(E::SourceFailure, TEXT("商品来源已被占用，不能以幂等来源代替购买交付")); }
    }
    UY01IncomeComponent::FPreparedSpending Spending;
    R = Income->PrepareSpending(Offer.Price, Now, Spending); R.Revision = L->Revision; if (!R.IsOk()) { return R; }
    if (L->Revision == MAX_uint64 || L->CommittedView.ViewRevision == MAX_uint64
        || (!Direct && (Offer.Quantity <= 0 || Offer.Quantity > MAX_int32 - L->Items.Num()
            || uint64(Offer.Quantity) > MAX_uint64 - L->AcquisitionSerial)))
    { return L->Fail(E::Range, TEXT("发货容器、版本或获取序号范围不足")); }
    const auto Catalog = SnapshotCatalog(); auto Random = ShoppingRandom;
    TArray<FY01ResolvedSource> Additions; auto NextStates = OfferStates;
    if (Direct)
    {
        auto& State = NextStates.FindChecked(Request.Offer.RowName); State.bPurchased = true;
        FY01ResolvedSource Source; Source.Kind = EY01RuntimeSourceKind::Shop; Source.LocalKey = State.SourceKey;
        for (const auto& Efx : State.Effects) { Source.Effects.Add({Efx.EffectId, Efx.Kind, Efx.Magnitude}); }
        Additions.Add(MoveTemp(Source));
    }
    TArray<FY01ItemGrant> Grants; if (!Direct) { Grants.Reserve(Offer.Quantity); }
    for (int32 Index = 0; !Direct && Index < Offer.Quantity; ++Index)
    {
        FY01ItemGrant Grant; FY01DataValidation Errors; auto Pool = Offer.Pool;
        if (Offer.ShopKind == EY01ShopKind::RandomTier)
        {
            const auto* Tier = Catalog.FindRow<FY01QualityRollPoolRow>(Offer.TierPool, Errors);
            if (!Tier || Weights(Tier->Entries) != E::None) { return L->Fail(E::InvalidDefinition, TEXT("品质快照无效")); }
            double Total = 0; for (const auto& Entry : Tier->Entries) { Total += Entry.Weight; }
            double Ticket = static_cast<double>(Random.GetFraction()) * Total;
            for (const auto& Entry : Tier->Entries)
            {
                if (Entry.Weight <= 0) { continue; }
                Pool = Entry.Pool; if (Ticket < Entry.Weight) { break; } Ticket -= Entry.Weight;
            }
        }
        if (!Catalog.SamplePool(Pool, Random, Grant.Definition, Errors))
        { return L->Fail(E::InvalidDefinition, TEXT("服务器商品池抽样失败")); }
        Grant.ItemId = FGuid::NewGuid(); Grants.Add(MoveTemp(Grant));
    }
    UY01LoadoutComponent::FPreparedBatch Batch;
    R = L->PrepareBatch(Catalog, Grants, {}, Batch); if (!R.IsOk()) { return R; }
    if (Grants.Num() == 1) { Batch.Result.ProductItemId = Grants[0].ItemId; }
    if (Direct) { Batch.Result.Revision = L->Revision + 1; }
    return L->CommitBatch(Requester, &Request, MoveTemp(Batch), Additions, {}, Now, [&]
    {
        auto Check = CheckSession(); if (!Check.IsOk()) { return Check; }
        Check = Income->ValidateSpending(Spending); Check.Revision = L->Revision; return Check;
    }, [&] { Income->CommitSpending(MoveTemp(Spending)); ShoppingRandom = Random; OfferStates = MoveTemp(NextStates); });
}
FY01ItemResult UY01EconomyComponent::TrySynthesize(AController* Requester, const FY01ItemRequest& Request)
{
    auto* P = Cast<AY01PlayerState>(GetOwner());
    if (!P || P->GetEconomy() != this) { return FY01ItemResult::Fail(EY01ItemError::Authority, 0, TEXT("非唯一经济组件")); }
    return P->GetLoadout()->SubmitRequest(Requester, Request);
}
FY01ItemResult UY01EconomyComponent::CheckSession(const FY01ItemRequest& Request) const
{
    using E = EY01ItemError; auto* L = CastChecked<AY01PlayerState>(GetOwner())->GetLoadout();
    if (!bInitialized) { return L->Fail(E::Disabled, TEXT("经济尚未显式启用")); }
    if (bEnded) { return L->Fail(E::Stopped, TEXT("经济已经结束")); }
    if (MatchId != L->MatchId || JoinSequence != L->JoinSequence) { return L->Fail(E::Session, TEXT("经济会话已变更")); }
    if (ConfigRevision != L->ConfigRevision || Request.ConfigRevision != ConfigRevision) { return L->Fail(E::StaleConfig, TEXT("经济配置过期")); }
    return L->Fail(E::None, TEXT(""));
}
FY01ItemResult UY01EconomyComponent::MutateSynthesis(AController* Requester, const FY01ItemRequest& Request, double Now)
{
    using E = EY01ItemError; auto* L = CastChecked<AY01PlayerState>(GetOwner())->GetLoadout();
    auto R = CheckSession(Request); if (!R.IsOk()) { return R; }
    if (!Empty(Request.Offer) || !Empty(Request.Talent) || Request.TalentChoiceId.IsValid()
        || Request.ItemId.IsValid() || Request.ExpectedEntityRevision || Request.Slot != INDEX_NONE
        || Request.ExpectedSlotItemId.IsValid() || Request.ExpectedSlotEntityRevision || Request.ExpectedLearnedId.IsValid()
        || Request.ExpectedLearnedRevision || Request.TicketId.IsValid())
    { return L->Fail(E::InvalidOperation, TEXT("合成只接收配方和五个材料身份版本")); }
    const auto* Recipe = Request.Recipe.Type == K::Synthesis ? Recipes.Find(Request.Recipe.RowName) : nullptr;
    if (!Recipe || Request.Materials.Num() != Y01Data::SynthesisInputCount)
    { return L->Fail(E::InvalidDefinition, TEXT("配方未启用或不是恰好五个材料")); }
    for (const auto& Material : Request.Materials)
    {
        const auto* Item = L->Items.Find(Material.ItemId);
        if (!Item) { return L->Fail(E::MissingItem, TEXT("材料不属于本玩家")); }
        if (Item->Definition.Type != Recipe->InputType || Item->Quality != Recipe->InputQuality)
        { return L->Fail(E::InvalidDefinition, TEXT("材料必须同类同品质，名称不参与合成资格")); }
    }
    const auto Catalog = SnapshotCatalog(); auto Random = SynthesisRandom;
    FY01ItemGrant Grant; FY01DataValidation Errors;
    if (!Catalog.SamplePool(Recipe->OutputPool, Random, Grant.Definition, Errors)) { return L->Fail(E::InvalidDefinition, TEXT("合成输出快照失效")); }
    Grant.ItemId = FGuid::NewGuid(); UY01LoadoutComponent::FPreparedBatch Batch;
    R = L->PrepareBatch(Catalog, {Grant}, Request.Materials, Batch); if (!R.IsOk()) { return R; }
    Batch.Result.ProductItemId = Grant.ItemId;
    // 免费路径只结算合法旧段，不调用任何支出预检或支出提交。
    return L->CommitBatch(Requester, &Request, MoveTemp(Batch), {}, {}, Now,
        [&] { return CheckSession(Request); }, [&] { SynthesisRandom = Random; });
}
FY01ItemResult UY01EconomyComponent::TryChooseTalent(AController* Requester, const FY01ItemRequest& Request)
{
    auto* P = Cast<AY01PlayerState>(GetOwner());
    if (!P || P->GetEconomy() != this) { return FY01ItemResult::Fail(EY01ItemError::Authority, 0, TEXT("非唯一经济组件")); }
    return P->GetLoadout()->SubmitRequest(Requester, Request);
}
FY01ItemResult UY01EconomyComponent::MutateChooseTalent(AController* Requester, const FY01ItemRequest& Request, double Now)
{
    using E = EY01ItemError; auto* P = CastChecked<AY01PlayerState>(GetOwner()); auto* L = P->GetLoadout();
    auto R = CheckSession(Request); if (!R.IsOk()) { return R; }
    if (!Empty(Request.Offer) || !Empty(Request.Recipe) || !Request.Materials.IsEmpty()
        || Request.ItemId.IsValid() || Request.ExpectedEntityRevision || Request.Slot != INDEX_NONE
        || Request.ExpectedSlotItemId.IsValid() || Request.ExpectedSlotEntityRevision || Request.ExpectedLearnedId.IsValid()
        || Request.ExpectedLearnedRevision || Request.TicketId.IsValid())
    { return L->Fail(E::InvalidOperation, TEXT("天赋选择只接收本玩家机会和候选身份")); }
    if (!TalentState.ChoiceId.IsValid()) { return L->Fail(E::Disabled, TEXT("未启用三选一天赋")); }
    if (Request.TalentChoiceId != TalentState.ChoiceId) { return L->Fail(E::InvalidDefinition, TEXT("不是本玩家的天赋机会")); }
    if (!TalentState.SelectedTalent.IsNone()) { return L->Fail(E::InvalidOperation, TEXT("本局天赋已经选择，不可刷新或替换")); }
    const auto* Candidate = Request.Talent.Type == K::Talent
        ? TalentState.Candidates.FindByPredicate([&](const auto& Value) { return Value.TalentId == Request.Talent.RowName; }) : nullptr;
    if (!Candidate) { return L->Fail(E::InvalidDefinition, TEXT("必须选择服务器保存的三个候选之一")); }
    if (L->Revision == MAX_uint64 || L->CommittedView.ViewRevision == MAX_uint64)
    { return L->Fail(E::Range, TEXT("天赋共同提交版本耗尽")); }
    if (P->GetSourceEffects()->Records.ContainsByPredicate([&](const auto& Record) { return Record.Source.Id == TalentState.SourceId; }))
    { return L->Fail(E::SourceFailure, TEXT("天赋来源已占用，不能用来源幂等冒充首次选择")); }
    auto NextState = TalentState; NextState.SelectedTalent = Candidate->TalentId;
    FY01ResolvedSource Source; Source.Kind = EY01RuntimeSourceKind::Talent; Source.LocalKey = NextState.SourceKey;
    for (const auto& Effect : Candidate->Effects) { Source.Effects.Add({Effect.EffectId, Effect.Kind, Effect.Magnitude}); }
    UY01LoadoutComponent::FPreparedBatch Batch;
    R = L->PrepareBatch(SnapshotCatalog(), {}, {}, Batch); if (!R.IsOk()) { return R; }
    Batch.Result.Revision = L->Revision + 1; Batch.Result.TalentChoiceId = NextState.ChoiceId;
    Batch.Result.SelectedTalent = NextState.SelectedTalent;
    return L->CommitBatch(Requester, &Request, MoveTemp(Batch), {Source}, {}, Now,
        [&] { return CheckSession(Request); }, [&] { TalentState = MoveTemp(NextState); });
}
bool UY01EconomyComponent::IsSelectedTalentSource(const FString& Id) const
{ return !TalentState.SelectedTalent.IsNone() && TalentState.SourceId == Id; }
bool UY01EconomyComponent::IsPurchasedSource(const FString& Id) const
{
    for (const auto& Pair : OfferStates) { if (Pair.Value.bPurchased && Pair.Value.SourceId == Id) { return true; } }
    return false;
}
void UY01EconomyComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    if (bEnded) { return; }
    bEnded = true; SetActiveFlag(false);
    if (bInitialized)
    { if (auto* P = Cast<AY01PlayerState>(GetOwner())) { P->GetIncome()->Stop(); } }
    if (HasBegunPlay()) { Super::EndPlay(Reason); }
}
