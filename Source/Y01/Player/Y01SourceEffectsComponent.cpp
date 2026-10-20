#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01IncomeComponent.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01EconomyComponent.h"
#include "Player/Y01GrowthComponent.h"
#include "Player/Y01DamageEffectsComponent.h"
#include "Misc/ScopeExit.h"
#include "GameplayEffectAggregator.h"
#include "UObject/UnrealType.h"
#include <limits>

namespace
{
using EError = EY01RuntimeError;
using EKind = EY01SourceEffectKind;
FY01RuntimeResult Fail(EError Code, const TCHAR* Message) { return FY01RuntimeResult::Fail(Code, Message); }
bool Before(const FY01SourceEffectSnapshot& A, const FY01SourceEffectSnapshot& B)
{
    if (A.Source.Kind != B.Source.Kind) { return A.Source.Kind < B.Source.Kind; }
    const int32 Order = A.Source.LocalKey.Compare(B.Source.LocalKey, ESearchCase::CaseSensitive);
    return Order != 0 ? Order < 0 : A.EffectId.LexicalLess(B.EffectId);
}
bool Same(const TArray<FY01SourceEffectSnapshot>& A, const TArray<FY01SourceEffectSnapshot>& B)
{
    if (A.Num() != B.Num()) { return false; }
    for (int32 I = 0; I < A.Num(); ++I)
    { if (A[I].EffectId != B[I].EffectId || A[I].Kind != B[I].Kind || A[I].Magnitude != B[I].Magnitude
        || A[I].SourceAttribute != B[I].SourceAttribute || A[I].TargetAttribute != B[I].TargetAttribute || A[I].Amount != B[I].Amount
        || A[I].Condition != B[I].Condition || A[I].ConditionThreshold != B[I].ConditionThreshold
        || A[I].DurationSeconds != B[I].DurationSeconds || A[I].LayerCap != B[I].LayerCap || A[I].IntervalSeconds != B[I].IntervalSeconds
        || A[I].bGrowthContribution != B[I].bGrowthContribution) { return false; } }
    return true;
}
bool AddFloat(float& Sum, double Value)
{
    if (Value > MAX_flt || static_cast<double>(Sum) + Value > MAX_flt) { return false; }
    const float V = static_cast<float>(Value), Next = Sum + V;
    if (!FMath::IsFinite(Next) || (Value > 0 && (V == 0 || Next == Sum)) || (Sum > 0 && Next == V)) { return false; }
    Sum = Next; return true;
}
bool ReadHealthRatio(const UY01CombatAbilitySystem& System, double& Out)
{
    const double Health = System.GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    const double Max = System.GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute());
    if (!FMath::IsFinite(Health) || Health < 0 || !FMath::IsFinite(Max) || Max < 0) { return false; }
    Out = Max == 0 ? 0 : Health / Max;
    return FMath::IsFinite(Out) && Out >= 0 && Out <= 1;
}
bool ReadCandidateHealthRatio(const UY01CombatAbilitySystem& System, double Max, double& Out)
{
    const double Health = System.GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    if (!FMath::IsFinite(Health) || Health < 0 || !FMath::IsFinite(Max) || Max < 0) { return false; }
    Out = Max == 0 ? 0 : FMath::Min(Health, Max) / Max;
    return FMath::IsFinite(Out) && Out >= 0 && Out <= 1;
}
bool SameConditions(const TArray<FY01SourceEffectSnapshot>& Effects, double A, double B)
{
    for (const auto& E : Effects)
    {
        if (FY01AttributeResolver::ConditionActive(E.Condition, E.ConditionThreshold, A)
            != FY01AttributeResolver::ConditionActive(E.Condition, E.ConditionThreshold, B)) { return false; }
    }
    return true;
}
bool SameBreakdowns(const TArray<FY01AttributeBreakdown>& A, const TArray<FY01AttributeBreakdown>& B)
{
    if (A.Num() != B.Num()) { return false; }
    for (int32 I = 0; I < A.Num(); ++I)
    { if (!FY01AttributeBreakdown::StaticStruct()->CompareScriptStruct(&A[I], &B[I], 0)) { return false; } }
    return true;
}
}
UY01SourceEffectsComponent::UY01SourceEffectsComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.bStartWithTickEnabled = false;
    PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
    bAutoActivate = false;
}
void UY01SourceEffectsComponent::ReevaluateConditions()
{
    if (!IsInGameThread() || !bInitialized || bEnded) { return; }
    const auto R = Check(true);
    if (!R.IsOk() && R.Code != EError::Busy) { return; }
    bAttributeRefreshDirty = true;
    SetComponentTickEnabled(true);
    DrainAttributeRefresh();
}
void UY01SourceEffectsComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    DrainAttributeRefresh();
}
void UY01SourceEffectsComponent::DrainAttributeRefresh()
{
    if (!IsInGameThread()) { return; }
    auto Clear = [&] { bAttributeRefreshDirty = false; SetComponentTickEnabled(false); };
    if (!bAttributeRefreshDirty || bEnded) { Clear(); return; }
    if (bDrainingAttributeRefresh || bBusy || bInCall || (Player.IsValid() && Player->GetLoadout()->IsBusy())) { return; }
    auto R = Check();
    if (R.Code == EError::Busy) { return; }
    bRefreshConditionsChanged = false;
    if (R.Code == EError::Stopped && Income.IsValid() && Income->State.Phase == EY01IncomePhase::Stopped)
    { Clear(); return; }
    TGuardValue<bool> Refresh(bDrainingAttributeRefresh, true);
    auto Unchanged = [&]
    {
        double Ratio = 0;
        if (!ReadHealthRatio(*ASC.Get(), Ratio)
            || ASC->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute()) != CommittedBaseAttack) { return false; }
        for (const auto& Record : Records)
        { if (!SameConditions(Record.Effects, Ratio, CommittedHealthRatio)) { return false; } }
        return true;
    };
    if (R.IsOk() && Unchanged()) { Clear(); return; }
    if (R.IsOk())
    {
        FY01PreparedMutation Plan; FY01SourceTransaction Tx;
        R = PrepareMutation({}, {}, Plan);
        if (R.IsOk()) { R = StageMutation(Plan, Tx); }
        if (R.IsOk()) { R = CommitMutation(Tx); }
    }
    if (R.IsOk())
    {
        // 提交委托可再次改生命；只检查一次，不在当前GAS广播栈内循环提交。
        if (!bEnded && Unchanged()) { Clear(); }
    }
    else if (R.Code == EError::Interrupted && bRefreshConditionsChanged && !bEnded)
    {
        // 候选条件再次跨界已精确回滚；保留dirty/tick，下一入口再有限重算。
        return;
    }
    else if (R.Code != EError::Busy)
    {
        UE_LOG(LogTemp, Error, TEXT("属性条件刷新停止 Owner=%s Code=%d: %s"),
            *GetNameSafe(GetOwner()), static_cast<int32>(R.Code), *R.Message);
        Clear();
    }
}
FY01RuntimeResult UY01SourceEffectsComponent::Interrupt()
{
    bInterrupted = true;
    return Fail(EError::Busy, TEXT("来源事务期间拒绝重入，外层将回滚"));
}
FY01RuntimeResult UY01SourceEffectsComponent::Check(bool bAllowBusy) const
{
    if (!IsInGameThread() || !Player.IsValid() || Player.Get() != GetOwner() || !Player->HasAuthority()
        || Player->IsActorBeingDestroyed() || !ASC.IsValid() || ASC->GetOwnerActor() != Player.Get()
        || !Income.IsValid() || Income->Player != Player || Income->MatchId != MatchId || Income->JoinSequence != JoinSequence)
    { return Fail(EError::Authority, TEXT("来源拥有者、ASC或收入会话失效")); }
    if (!bInitialized) { return Fail(EError::Disabled, TEXT("来源组件尚未显式初始化")); }
    if (ASC->bAutoAttackStarting) { return Fail(EError::Busy, TEXT("普攻发动提交期间不可改变来源")); }
    if (!bAllowBusy && bBusy) { return Fail(EError::Busy, TEXT("来源事务尚未完成")); }
    if (!bAllowBusy && Player->GetGrowth()->BlocksMutation())
    { return Fail(EError::Busy, TEXT("已提交致死事件须先消费再改变来源")); }
    if (bEnded || Income->bEnded || Income->State.Phase == EY01IncomePhase::Stopped || Income->State.Phase == EY01IncomePhase::Fault)
    { return Fail(EError::Stopped, TEXT("会话收入已停止或故障，拒绝新增变动")); }
    return {};
}
FY01RuntimeResult UY01SourceEffectsComponent::Initialize(const FGuid& Match, int32 JoinSeq, double InitialRemainder)
{
    if (bBusy) { return Interrupt(); }
    auto* P = Cast<AY01PlayerState>(GetOwner());
    if (!IsInGameThread() || !IsValid(P) || !P->HasAuthority() || P->IsActorBeingDestroyed())
    { return Fail(EError::Authority, TEXT("仅有效服务器PlayerState可以初始化来源")); }
    if (bEnded) { return Fail(EError::Stopped, TEXT("EndPlay之后不可重新初始化")); }
    if (bInitialized && (Match != MatchId || JoinSeq != JoinSequence))
    { return Fail(EError::Session, TEXT("不能覆盖已授予的Match和JoinSeq")); }
    auto* A = P->GetCombatASC(); auto* I = P->GetIncome();
    if (!A || !I || !A->GetSet<UY01AttributeSet>() || (A->GetOwnerActor() && A->GetOwnerActor() != P))
    { return Fail(EError::Session, TEXT("缺少同owner战斗属性或收入组件")); }
    auto R = I->Initialize(Match, JoinSeq, InitialRemainder); if (!R.IsOk()) { return R; }
    if (bInitialized) { return Check(); }
    if (!A->GetOwnerActor()) { A->InitAbilityActorInfo(P, P); }
    Player = P; ASC = A; Income = I; MatchId = Match; JoinSequence = JoinSeq;
    BaseAttributes = A->UnitStats;
    BaseAttributes.AttackDamage = A->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute());
    BaseAttributes.Health = A->GetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute());
    BaseAttributes.Armor = A->GetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute());
    BaseAttributes.HealthRegen = A->GetNumericAttributeBase(UY01AttributeSet::GetHealthRegenAttribute());
    double HealthRatio = 0;
    if (!ReadHealthRatio(*A, HealthRatio)) { return Fail(EError::InvalidNumber, TEXT("实际生命比例无效")); }
    FY01ResolvedAttributes Attributes;
    R = FY01AttributeResolver::Resolve(BaseAttributes, PlayerGrowth, {}, Attributes, HealthRatio);
    if (!R.IsOk()) { return R; }
    CommittedAttributes = MoveTemp(Attributes); CommittedHealthRatio = HealthRatio;
    CommittedBaseAttack = BaseAttributes.AttackDamage;
    bInitialized = true; Revision = 1; SetActiveFlag(true);
    return {};
}
FY01RuntimeResult UY01SourceEffectsComponent::InitializeProfile(const FY01DataCatalog& Catalog,
    const TArray<FY01DataRef>& Mappings, int32 InitialGold, double BaseGoldPerSecond)
{
    if (bBusy || bInCall) { return Interrupt(); }
    auto R = Check(); if (!R.IsOk()) { return R; }
    auto* L = Player->GetLoadout();
    if (!L || !L->CheckOwner().IsOk() || L->bEnded)
    { return Fail(EError::Session, TEXT("基础配置必须绑定有效共同库存会话")); }
    if (!L->Transactions.Enter()) { return Fail(EError::Busy, TEXT("配置初始化必须持有共同库存事务")); }
    ON_SCOPE_EXIT { L->Transactions.Leave(); L->PublishCommittedView(); };
    if (InitialGold < 0 || !FMath::IsFinite(BaseGoldPerSecond) || BaseGoldPerSecond < 0
        || (bProfileInitialized && InitialGold != ProfileInitialGold))
    { return Fail(EError::PayloadConflict, TEXT("配置初始化数值非法或重复载荷不同")); }
    FY01ResolvedSource Base; Base.LocalKey = TEXT("ProfileIncome");
    Base.Effects.Add({TEXT("BaseGoldPerSecond"), EKind::GoldPerSecond, BaseGoldPerSecond});
    TSet<FName> Seen;
    for (const auto& Ref : Mappings)
    {
        FY01DataValidation Q; const auto* Row = Catalog.FindRow<FY01AttributeMappingRow>(Ref, Q);
        if (Ref.Type != EY01TableKind::AttributeMapping || !Row || !Row->IsValid() || Seen.Contains(Ref.RowName))
        { return Fail(EError::InvalidReference, TEXT("基础映射引用缺失、重复或非法")); }
        Seen.Add(Ref.RowName);
        Base.Effects.Add({FName(*(TEXT("Mapping_") + Ref.RowName.ToString())), EKind::MappingCoefficientAdd, 0,
            Row->SourceAttribute, Row->TargetAttribute, Row->Coefficient});
    }
    FY01PreparedMutation Plan; R = PrepareMutation({Base}, {}, Plan); if (!R.IsOk()) { return R; }
    if (bProfileInitialized) { return {}; }
    FY01SourceTransaction Tx; R = StageMutation(Plan, Tx); if (!R.IsOk()) { return R; }
    if (L->Transactions.bInterrupted || !Player->CanAddMatchReward(InitialGold, 0))
    { RollbackMutation(Tx); return Fail(EError::BalanceRange, TEXT("配置初始金币无法完整入账")); }
    return CommitMutationAt(Tx, [&]
    {
        Player->TryAddMatchReward(InitialGold, 0);
        ProfileInitialGold = InitialGold; bProfileInitialized = true;
    });
}
FY01RuntimeResult UY01SourceEffectsComponent::MakeId(EY01RuntimeSourceKind Kind, const FString& LocalKey, FY01RuntimeSourceId& Out) const
{
    static const TCHAR* Kinds[] = { TEXT("Base"), TEXT("Equipment"), TEXT("Skill"), TEXT("Talent"), TEXT("Shop"), TEXT("Devoured") };
    const uint8 Index = static_cast<uint8>(Kind);
    if (Index >= UE_ARRAY_COUNT(Kinds) || LocalKey.IsEmpty() || LocalKey.Len() > 128)
    { return Fail(EError::InvalidIdentity, TEXT("来源种类或LocalKey无效")); }
    for (TCHAR C : LocalKey)
    { if (!FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-')) { return Fail(EError::InvalidIdentity, TEXT("LocalKey只允许字母数字、下划线和连字符")); } }
    Out.Kind = Kind; Out.LocalKey = LocalKey;
    Out.Id = FString::Printf(TEXT("R36/%s/P%d/%s/%s"), *MatchId.ToString(EGuidFormats::DigitsWithHyphens), JoinSequence, Kinds[Index], *LocalKey);
    return {};
}
FY01RuntimeResult UY01SourceEffectsComponent::ValidateHandles(bool bStaged) const
{
    if (!ASC.IsValid()) { return Fail(EError::Session, TEXT("ASC已失效")); }
    if (DerivedAttackHandle.IsValid())
    {
        const auto* GE = ASC->GetActiveGameplayEffect(DerivedAttackHandle);
        if (!GE || GE->IsPendingRemove || GE->bIsInhibited != (bStaged && bDerivedAttackChanged)
            || GE->Spec.GetStackCount() != 1 || GE->Spec.GetSetByCallerMagnitude(Y01Tags::Data_EquipmentAttack, false, -1) != static_cast<float>(DerivedAttackBonus))
        { return Fail(EError::HandleLost, TEXT("属性换算攻击GE被外部改变")); }
    }
    return {};
}
FY01RuntimeResult UY01SourceEffectsComponent::ValidateTotals(const TArray<FY01SourceEffectSnapshot>& Extra, const TSet<FString>& Exclude,
    double& OutRate, double& OutCurrent, FY01ResolvedAttributes* OutAttributes, TOptional<FY01PlayerGrowthTotals> CandidateStrength, double* OutHealthRatio) const
{
    TArray<FY01SourceEffectSnapshot> All;
    for (const auto& R : Records) { if (!Exclude.Contains(R.Source.Id)) { All.Append(R.Effects); } }
    All.Append(Extra); All.Sort(Before);
    const auto Attribute = UY01AttributeSet::GetAttackDamageAttribute();
    float Attack = ASC->GetNumericAttributeBase(Attribute);
    if (!FMath::IsFinite(Attack) || Attack < 0) { return Fail(EError::InvalidNumber, TEXT("基础攻击无效")); }
    auto Base = BaseAttributes; Base.AttackDamage = Attack;
    double HealthRatio = 0;
    if (!ReadHealthRatio(*ASC.Get(), HealthRatio)) { return Fail(EError::InvalidNumber, TEXT("实际生命比例无效")); }
    // 第一遍只解析合法无条件的上限依赖，避免旧比例激活无关效果并提前拒绝精度。
    TArray<FY01SourceEffectSnapshot> MaxDependencies;
    for (const auto& E : All)
    {
        if (E.Condition == EY01EffectCondition::None && Y01Data::ValidSourceEffect(E)
            && (((E.Kind == EKind::AttributeFlat || E.Kind == EKind::AttributePercent)
                    && (Y01Data::IsPrimaryAttribute(E.TargetAttribute) || E.TargetAttribute == EY01Attribute::MaxHealth))
                || (E.Kind == EKind::MappingCoefficientAdd && E.TargetAttribute == EY01Attribute::MaxHealth)))
        { MaxDependencies.Add(E); }
    }
    FY01ResolvedAttributes Resolved;
    auto ResolveResult = FY01AttributeResolver::Resolve(Base, CandidateStrength.Get(PlayerGrowth), MaxDependencies, Resolved, HealthRatio);
    if (!ResolveResult.IsOk()) { return ResolveResult; }
    // 条件不可改变三维或生命上限；第二遍以成功提交后会钳制到的候选上限裁定。
    if (!ReadCandidateHealthRatio(*ASC.Get(), Resolved.Stats.Health, HealthRatio))
    { return Fail(EError::InvalidNumber, TEXT("候选生命比例无效")); }
    ResolveResult = FY01AttributeResolver::Resolve(Base, CandidateStrength.Get(PlayerGrowth), All, Resolved, HealthRatio);
    if (!ResolveResult.IsOk()) { return ResolveResult; }
    // R37第四期：金币获取%只放大自然收入速率，不进属性明细。
    const double Gained = Resolved.GoldPerSecond * (1. + Resolved.GoldGainPercent);
    if (!FMath::IsFinite(Gained) || Gained < 0 || (Gained > 0 && Resolved.GoldPerSecond == 0))
    { return Fail(EError::InvalidNumber, TEXT("金币获取%放大后的收入速率无效")); }
    OutRate = Gained;
    if (!AddFloat(Attack, Resolved.AttackBonus)) { return Fail(EError::Precision, TEXT("换算攻击无法表示为有效float")); }
    // ASC还可能包含兼容旧GE；新增时也验证其实际current，不能只看自建sum。
    float Current = ASC->GetNumericAttribute(Attribute);
    if (!FMath::IsFinite(Current) || Current < 0) { return Fail(EError::InvalidNumber, TEXT("实际攻击无效")); }
    if (DerivedAttackHandle.IsValid())
    {
        FGameplayEffectAttributeCaptureSpec Capture(FGameplayEffectAttributeCaptureDefinition(Attribute, EGameplayEffectAttributeCaptureSource::Target, false));
        ASC->CaptureAttributeForGameplayEffect(Capture);
        FAggregator Snapshot;
        if (!Capture.AttemptGetAttributeAggregatorSnapshot(Snapshot))
        { return Fail(EError::InvalidNumber, TEXT("不能复制实际攻击聚合器")); }
        TArray<bool> Qualifications;
        Snapshot.ForEachMod([&](const FAggregatorModInfo& Info)
        { Qualifications.Add(Info.Mod->Qualifies() && Info.Mod->ActiveHandle != DerivedAttackHandle); });
        FAggregatorEvaluateMetaData FrozenQualifications([&](const FAggregatorEvaluateParameters&, const FAggregator* Local)
        {
            int32 Index = 0;
            Local->ForEachMod([&](const FAggregatorModInfo& Info) { Info.Mod->SetExplicitQualifies(Qualifications[Index++]); });
        });
        Snapshot.EvaluationMetaData = &FrozenQualifications;
        // 只在局部快照求值，保留已裁定资格；不调用真实聚合器或其自定义资格委托。
        // 提交前仍验证真实GAS结果，不模拟任意外部效果的资格规则。
        Current = Snapshot.Evaluate(FAggregatorEvaluateParameters());
        Snapshot.EvaluationMetaData = nullptr;
        if (!FMath::IsFinite(Current) || Current < 0)
        { return Fail(EError::InvalidNumber, TEXT("最终保留集合实际攻击无效")); }
    }
    if (!AddFloat(Current, Resolved.AttackBonus)) { return Fail(EError::Precision, TEXT("实际GAS无法表示换算攻击")); }
    OutCurrent = Current;
    if (OutHealthRatio) { *OutHealthRatio = HealthRatio; }
    if (OutAttributes) { *OutAttributes = MoveTemp(Resolved); }
    return {};
}
FY01RuntimeResult UY01SourceEffectsComponent::PrepareSource(const FY01DataCatalog& Catalog, EY01RuntimeSourceKind Kind,
    const FString& LocalKey, const TArray<FY01DataRef>& EffectIds, FY01PreparedSource& OutPlan)
{
    OutPlan = FY01PreparedSource();
    if (bBusy) { return Interrupt(); }
    auto R = Check(); if (!R.IsOk()) { return R; }
    FY01RuntimeSourceId Id; R = MakeId(Kind, LocalKey, Id); if (!R.IsOk()) { return R; }
    FY01ResolvedSource Source; Source.Kind = Kind; Source.LocalKey = LocalKey;
    TSet<FName> Seen;
    for (const auto& Ref : EffectIds)
    {
        FY01DataValidation Q;
        const auto* Row = Catalog.FindRow<FY01SourceEffectRow>(Ref, Q);
        if (Ref.Type != EY01TableKind::SourceEffect || !Row) { return Fail(EError::InvalidReference, TEXT("EffectIds类型、表结构或行无效")); }
        if (Seen.Contains(Ref.RowName)) { return Fail(EError::DuplicateEffect, TEXT("同一来源重复EffectId")); }
        Seen.Add(Ref.RowName);
        if (!Y01Data::ValidSourceEffect(*Row))
        { return Fail(EError::InvalidNumber, TEXT("效果种类或有限非负幅度无效")); }
        if (Row->Kind == EKind::AttackFlat && (Row->Magnitude > MAX_flt || (Row->Magnitude > 0 && static_cast<float>(Row->Magnitude) == 0)))
        { return Fail(EError::Precision, TEXT("AttackFlat不能转换为非零有限float")); }
        Source.Effects.Add({Ref.RowName, Row->Kind, Row->Magnitude, Row->SourceAttribute, Row->TargetAttribute, Row->Amount,
            Row->Condition, Row->ConditionThreshold, false, Row->DurationSeconds, Row->LayerCap, Row->IntervalSeconds});
    }
    FY01PreparedMutation Mutation;
    R = PrepareMutation({Source}, {}, Mutation);
    if (R.IsOk()) { OutPlan = Mutation.Additions[0]; }
    return R;
}
bool UY01SourceEffectsComponent::Owns(const FY01SourceTransaction& Transaction) const
{
    return bBusy && Transaction.Owner.Get() == this && Transaction.Token.IsValid() && Transaction.Token == PendingToken;
}
FY01RuntimeResult UY01SourceEffectsComponent::ValidatePending()
{
    auto R = Check(true); if (!R.IsOk()) { return R; }
    if (bInterrupted) { return Fail(EError::Interrupted, TEXT("GAS委托重入使暂存事务失效")); }
    R = ValidateHandles(true); if (!R.IsOk()) { return R; }
    double ActualRatio = 0, CandidateRatio = 0;
    if (!ReadHealthRatio(*ASC.Get(), ActualRatio)
        || !ReadCandidateHealthRatio(*ASC.Get(), PendingAttributes.Stats.Health, CandidateRatio))
    { return Fail(EError::InvalidNumber, TEXT("暂存期间实际或候选生命比例无效")); }
    // Stage不写Max；实际GAS须仍对应已提交权威上限，不可覆盖外部变动伪装成功。
    if (ASC->GetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute()) != CommittedAttributes.Stats.Health
        || ASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()) != CommittedAttributes.Stats.Health)
    { return Fail(EError::Interrupted, TEXT("实际生命上限与已提交权威属性不一致")); }
    bool bConditionsMatch = true;
    for (const auto& Record : Records)
    {
        if (!PendingRemoved.Contains(Record.Source.Id))
        { bConditionsMatch &= SameConditions(Record.Effects, CandidateRatio, PendingHealthRatio); }
    }
    for (const auto& Record : Pending)
    { bConditionsMatch &= SameConditions(Record.Effects, CandidateRatio, PendingHealthRatio); }
    if (!bConditionsMatch)
    {
        if (bDrainingAttributeRefresh) { bRefreshConditionsChanged = true; }
        bAttributeRefreshDirty = true; SetComponentTickEnabled(true);
        return Fail(EError::Interrupted, TEXT("暂存期间生命变化使候选条件失效"));
    }
    const auto Attribute = UY01AttributeSet::GetAttackDamageAttribute();
    if (ASC->GetNumericAttributeBase(Attribute) != BeforeBase) { return Fail(EError::Interrupted, TEXT("暂存期间基础攻击被外部改变")); }
    if (bDerivedAttackChanged && PendingAttributes.AttackBonus > 0)
    {
        const auto* GE = ASC->GetActiveGameplayEffect(PendingDerivedAttackHandle);
        if (!GE || GE->IsPendingRemove || GE->bIsInhibited || GE->Spec.GetStackCount() != 1
            || GE->Spec.GetSetByCallerMagnitude(Y01Tags::Data_EquipmentAttack, false, -1) != static_cast<float>(PendingAttributes.AttackBonus))
        { return Fail(EError::HandleLost, TEXT("暂存换算GE失效")); }
    }
    const float Actual = ASC->GetNumericAttribute(Attribute);
    const double Tolerance = FMath::Max(1e-6, FMath::Abs(PendingCurrent) * std::numeric_limits<float>::epsilon() * 2);
    if (!FMath::IsFinite(Actual) || FMath::Abs(static_cast<double>(Actual) - PendingCurrent) > Tolerance)
    { return Fail(EError::Interrupted, TEXT("实际ASC攻击与暂存效果不一致")); }
    return {};
}
FY01RuntimeResult UY01SourceEffectsComponent::RequestStop()
{
    // 首次请求确定停止边界，后续重入或回滚耗时不能延长收入。
    if (!bStopTimeCaptured && Income.IsValid()) { StopTime = Income->ReadNow(); bStopTimeCaptured = true; }
    bStopRequested = true;
    return Interrupt();
}
void UY01SourceEffectsComponent::FinishTransaction()
{
    Pending.Reset(); PendingRemoved.Reset(); PendingToken.Invalidate();
    PendingDerivedAttackHandle.Invalidate(); bDerivedAttackChanged = false; bDerivedAttackInhibited = false;
    PendingAttributes = {}; PendingHealthRatio = 1; PendingGrowth = {};
    PendingRate = 0; PendingTime = 0; PendingCurrent = 0; BeforeBase = 0; BeforeCurrent = 0;
    bResolvedStateChanged = false;
    if (bStopRequested && Income.IsValid())
    { bStopRequested = false; Income->StopAt(StopTime, true); }
    if (bEnded && !bEndCleanupComplete)
    {
        bEndCleanupComplete = true;
        bBusy = true;
        Records.Reset();
        const auto EndedDerived = DerivedAttackHandle; DerivedAttackHandle.Invalidate();
        if (ASC.IsValid() && ASC->GetActiveGameplayEffect(EndedDerived)) { ASC->RemoveActiveGameplayEffect(EndedDerived); }
    }
    if (bStopRequested && Income.IsValid())
    { bStopRequested = false; Income->StopAt(StopTime, true); }
    bBusy = false;
}
FY01RuntimeResult UY01SourceEffectsComponent::AbortPending()
{
    bool Restored = true;
    if (ASC.IsValid() && ASC->GetActiveGameplayEffect(PendingDerivedAttackHandle))
    { Restored &= ASC->RemoveActiveGameplayEffect(PendingDerivedAttackHandle); }
    if (bDerivedAttackInhibited && !bEnded)
    {
        auto Handle = DerivedAttackHandle;
        const auto* GE = ASC.IsValid() ? ASC->GetActiveGameplayEffect(Handle) : nullptr;
        if (!GE || GE->IsPendingRemove) { Restored = false; }
        else { Restored &= ASC->SetActiveGameplayEffectInhibit(MoveTemp(Handle), false, false).IsValid(); }
    }
    if (!bEnded)
    {
        Restored &= ValidateHandles().IsOk();
        Restored &= ASC.IsValid() && ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()) == BeforeCurrent
            && ASC->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute()) == BeforeBase;
    }
    FinishTransaction();
    return Restored ? FY01RuntimeResult() : Fail(EError::HandleLost, TEXT("来源回滚遇到外部GAS变动，不能宣称精确恢复"));
}
FY01RuntimeResult UY01SourceEffectsComponent::StageSource(const FY01PreparedSource& Plan, FY01SourceTransaction& OutTransaction)
{
    FY01PreparedMutation Mutation;
    Mutation.Owner = Plan.Owner; Mutation.Revision = Plan.Revision;
    Mutation.Match = MatchId; Mutation.JoinSequence = JoinSequence; Mutation.Additions.Add(Plan);
    return StageMutation(Mutation, OutTransaction);
}
FY01RuntimeResult UY01SourceEffectsComponent::CommitSource(const FY01SourceTransaction& Transaction) { return CommitMutation(Transaction); }
FY01RuntimeResult UY01SourceEffectsComponent::CommitMutation(const FY01SourceTransaction& Transaction)
{
    return CommitMutationAt(Transaction, [] {});
}
FY01RuntimeResult UY01SourceEffectsComponent::CommitMutationAt(const FY01SourceTransaction& Transaction, TFunctionRef<void()> CommitOwner)
{
    if (!IsInGameThread() || !Player.IsValid() || Player.Get() != GetOwner() || !Player->HasAuthority())
    { return Fail(EError::Authority, TEXT("无权提交，保留服务器事务与回滚句柄")); }
    if (bInCall) { return Interrupt(); }
    if (!Owns(Transaction)) { return Fail(EError::Stale, TEXT("提交事务不属于此owner或已经结束")); }
    ON_SCOPE_EXIT { if (Player.IsValid()) { Player->GetGrowth()->Drain(); } };
    TGuardValue<bool> Call(bInCall, true);
    auto R = ValidatePending();
    if (!R.IsOk()) { const auto Undo = AbortPending(); return Undo.IsOk() ? R : Undo; }
    const auto RetiredDerived = bDerivedAttackChanged ? DerivedAttackHandle : FActiveGameplayEffectHandle();
    if (!Pending.IsEmpty() || !PendingRemoved.IsEmpty() || PendingGrowth != PlayerGrowth || bResolvedStateChanged)
    {
        // 线性化点：所有可能失败/广播委托的操作已完成，以下只提交本模块状态。
        // 删除通知在此点之后发生，不能再把已提交事务报告成可回滚失败。
        Income->ChangeRate(PendingRate, PendingTime);
        for (int32 I = Records.Num() - 1; I >= 0; --I)
        { if (PendingRemoved.Contains(Records[I].Source.Id)) { Records.RemoveAt(I); } }
        Records.Append(MoveTemp(Pending)); ++Revision;
    }
    // 受信任外层仅移动已准备内存和结果，不得验证、失败或广播委托。
    // 与来源记录/收入速率处于同一不可观察区间，删除GE重入不能撤销任何一侧。
    PlayerGrowth = PendingGrowth;
    if (bDerivedAttackChanged) { DerivedAttackHandle = PendingDerivedAttackHandle; }
    DerivedAttackBonus = PendingAttributes.AttackBonus;
    CommittedAttributes = MoveTemp(PendingAttributes); CommittedHealthRatio = PendingHealthRatio;
    CommittedBaseAttack = BeforeBase;
    CommitOwner();
    // 所有失败点已过；生命上限、护甲及恢复从未在Stage被改动。
    // 无聚合器时GAS可立即通知；此时双方状态已提交且仍Busy，EndPlay仅挂起清理。
    // 先写三维与间隔，绝不改正在等待的NextAutoAttackTime或动态AttackDamage基础值。
    auto* CommittedASC = ASC.Get();
    {
        FScopedAggregatorOnDirtyBatch Batch;
        CommittedASC->UnitStats.Strength = CommittedAttributes.Stats.Strength; CommittedASC->UnitStats.Agility = CommittedAttributes.Stats.Agility;
        CommittedASC->UnitStats.Wisdom = CommittedAttributes.Stats.Wisdom; CommittedASC->UnitStats.AttackInterval = CommittedAttributes.Stats.AttackInterval;
        CommittedASC->SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), CommittedAttributes.Stats.Armor);
        CommittedASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthRegenAttribute(), CommittedAttributes.Stats.HealthRegen);
        CommittedASC->SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(), CommittedAttributes.Stats.Health);
        // 暴击/倍率/吸血为解析器直写基础值；护盾种类不进属性集，由护盾组件任务消费。
        CommittedASC->SetNumericAttributeBase(UY01AttributeSet::GetCritChanceAttribute(), CommittedAttributes.CritChance);
        CommittedASC->SetNumericAttributeBase(UY01AttributeSet::GetCritMultiplierBonusAttribute(), CommittedAttributes.CritMultiplierBonus);
        CommittedASC->SetNumericAttributeBase(UY01AttributeSet::GetLifestealAttribute(), CommittedAttributes.Lifesteal);
    }
    Player->GetDamageEffects()->SynchronizeCommitted();
    Pending.Reset(); PendingRemoved.Reset();
    if (ASC.IsValid() && ASC->GetActiveGameplayEffect(RetiredDerived)) { ASC->RemoveActiveGameplayEffect(RetiredDerived); }
    FinishTransaction(); return {};
}
FY01RuntimeResult UY01SourceEffectsComponent::RollbackSource(const FY01SourceTransaction& Transaction) { return RollbackMutation(Transaction); }
FY01RuntimeResult UY01SourceEffectsComponent::RollbackMutation(const FY01SourceTransaction& Transaction)
{
    if (!IsInGameThread() || !Player.IsValid() || Player.Get() != GetOwner() || !Player->HasAuthority())
    { return Fail(EError::Authority, TEXT("无权回滚，保留服务器事务与回滚句柄")); }
    if (bInCall) { return Interrupt(); }
    if (!Owns(Transaction)) { return Fail(EError::Stale, TEXT("回滚事务不属于此owner或已经结束")); }
    ON_SCOPE_EXIT { if (Player.IsValid()) { Player->GetGrowth()->Drain(); } };
    TGuardValue<bool> Call(bInCall, true); return AbortPending();
}
FY01RuntimeResult UY01SourceEffectsComponent::RemoveSource(EY01RuntimeSourceKind Kind, const FString& LocalKey)
{
    if (bBusy || bInCall) { return Interrupt(); }
    auto R = Check(); if (!R.IsOk()) { return R; }
    FY01RuntimeSourceId Id; R = MakeId(Kind, LocalKey, Id); if (!R.IsOk()) { return R; }
    const double Now = Income->ReadNow(); R = Income->SettleAt(Now); if (!R.IsOk()) { return R; }
    if (!FMath::IsFinite(Now) || Now < 0) { return Fail(EError::Time, TEXT("移除请求时间无效")); }
    const int32 Index = Records.IndexOfByPredicate([&](const FRecord& V) { return V.Source.Id == Id.Id; });
    if (Index == INDEX_NONE) { return {}; }
    if (Revision == MAX_uint64) { return Fail(EError::IntegerRange, TEXT("来源版本已耗尽")); }
    R = ValidateHandles(); if (!R.IsOk()) { return R; }
    FY01PreparedMutation Plan; R = PrepareMutation({}, {Id}, Plan); if (!R.IsOk()) { return R; }
    // 旧入口已在上述同一时刻结算；批量暂存复用该时刻，不再读取时钟。
    FY01SourceTransaction Transaction;
    R = StageMutationAt(Plan, Transaction, Now);
    return R.IsOk() ? CommitMutation(Transaction) : R;
}
FY01RuntimeResult UY01SourceEffectsComponent::PrepareMutation(const TArray<FY01ResolvedSource>& Additions,
    const TArray<FY01RuntimeSourceId>& Removals, FY01PreparedMutation& OutPlan)
{
    OutPlan = FY01PreparedMutation();
    if (bBusy || bInCall) { return Interrupt(); }
    auto R = Check(); if (!R.IsOk()) { return R; }
    R = ValidateHandles(); if (!R.IsOk()) { return R; }
    FY01PreparedMutation Plan;
    TSet<FString> Removed, Added;
    for (const auto& Id : Removals)
    {
        FY01RuntimeSourceId Expected; R = MakeId(Id.Kind, Id.LocalKey, Expected); if (!R.IsOk()) { return R; }
        if (Id.Id != Expected.Id || Removed.Contains(Id.Id))
        { return Fail(EError::InvalidIdentity, TEXT("移除身份跨会话、不完整或重复")); }
        if (bProfileInitialized && Id.Kind == EY01RuntimeSourceKind::Base && Id.LocalKey == TEXT("ProfileIncome"))
        { return Fail(EError::InvalidIdentity, TEXT("已提交基础配置本局不可移除或替换")); }
        if (Id.Kind == EY01RuntimeSourceKind::Shop && Player->GetEconomy()->IsPurchasedSource(Id.Id))
        { return Fail(EError::InvalidIdentity, TEXT("已购买的商店来源本局不可撤销或替换")); }
        if (Id.Kind == EY01RuntimeSourceKind::Talent && Player->GetEconomy()->IsSelectedTalentSource(Id.Id))
        { return Fail(EError::InvalidIdentity, TEXT("已选择的天赋来源本局不可撤销或替换")); }
        if (Id.Kind == EY01RuntimeSourceKind::Devoured && !Additions.ContainsByPredicate([&](const FY01ResolvedSource& A)
            { return A.Kind == EY01RuntimeSourceKind::Devoured && A.LocalKey == Id.LocalKey; }))
        { return Fail(EError::InvalidIdentity, TEXT("已吞噬来源本局不可撤销，只能同批替换成长快照")); }
        Removed.Add(Id.Id);
        if (!Records.ContainsByPredicate([&](const FRecord& V) { return V.Source.Id == Id.Id; }))
        { return Fail(EError::InvalidIdentity, TEXT("批量移除必须引用当前来源")); }
        Plan.Removals.Add(Id);
    }
    TArray<FY01SourceEffectSnapshot> Extra;
    for (const auto& Source : Additions)
    {
        FY01PreparedSource Item; R = MakeId(Source.Kind, Source.LocalKey, Item.Source); if (!R.IsOk()) { return R; }
        if (Added.Contains(Item.Source.Id)) { return Fail(EError::InvalidIdentity, TEXT("批量新增身份重复")); }
        Added.Add(Item.Source.Id);
        TSet<FName> Seen;
        for (const auto& E : Source.Effects)
        {
            if (E.EffectId.IsNone()) { return Fail(EError::InvalidReference, TEXT("已解析效果仍须非空身份")); }
            if (Seen.Contains(E.EffectId)) { return Fail(EError::DuplicateEffect, TEXT("同一来源重复EffectId")); }
            Seen.Add(E.EffectId);
            if (!Y01Data::ValidSourceEffect(E)
                || ((E.Kind == EKind::AttributeFlat || E.Kind == EKind::MappingCoefficientAdd || E.Kind == EKind::AttributePercent)
                    && Source.Kind != EY01RuntimeSourceKind::Equipment && Source.Kind != EY01RuntimeSourceKind::Devoured && Source.Kind != EY01RuntimeSourceKind::Base))
            { return Fail(EError::InvalidNumber, TEXT("已解析效果种类或幅度无效")); }
            // 运行时门与目录层一致：暴击/倍率/吸血/周期护盾仅接通装备（含已吞噬装备）来源，施放护盾仅接通技能来源。
            if (((E.Kind == EKind::CritChance || E.Kind == EKind::CritMultiplierAdd || E.Kind == EKind::Lifesteal
                    || E.Kind == EKind::ShieldPeriodic) && Source.Kind != EY01RuntimeSourceKind::Equipment && Source.Kind != EY01RuntimeSourceKind::Devoured)
                || (E.Kind == EKind::ShieldOnCast && Source.Kind != EY01RuntimeSourceKind::Skill))
            { return Fail(EError::InvalidNumber, TEXT("暴击、倍率、吸血与周期护盾仅接通装备来源；施放护盾仅接通技能来源")); }
            if (E.Kind == EKind::AttackFlat && (E.Magnitude > MAX_flt || (E.Magnitude > 0 && static_cast<float>(E.Magnitude) == 0)))
            { return Fail(EError::Precision, TEXT("已解析攻击不能表示为非零有限float")); }
            Item.Effects.Add({Item.Source, E.EffectId, E.Kind, E.Magnitude, E.SourceAttribute, E.TargetAttribute, E.Amount,
                E.Condition, E.ConditionThreshold, E.bGrowthContribution, E.DurationSeconds, E.LayerCap, E.IntervalSeconds});
        }
        Item.Effects.Sort(Before);
        if (!Removed.Contains(Item.Source.Id))
        {
            if (const auto* Old = Records.FindByPredicate([&](const FRecord& V) { return V.Source.Id == Item.Source.Id; }))
            {
                if (!Same(Old->Effects, Item.Effects)) { return Fail(EError::PayloadConflict, TEXT("同source的不同payload不能静默替换")); }
                Item.bRepeat = true;
            }
        }
        if (!Item.bRepeat) { Extra.Append(Item.Effects); }
        Item.Owner = this; Item.Revision = Revision; Plan.Additions.Add(MoveTemp(Item));
    }
    double Rate = 0, Current = 0; R = ValidateTotals(Extra, Removed, Rate, Current); if (!R.IsOk()) { return R; }
    Plan.Owner = this; Plan.Match = MatchId; Plan.JoinSequence = JoinSequence; Plan.Revision = Revision;
    OutPlan = MoveTemp(Plan); return {};
}
FY01RuntimeResult UY01SourceEffectsComponent::SettleMutationTime(double& OutNow)
{
    if (bBusy || bInCall) { return Interrupt(); }
    auto R = Check(); if (!R.IsOk()) { return R; }
    OutNow = Income->ReadNow();
    return Income->SettleAt(OutNow);
}
FY01RuntimeResult UY01SourceEffectsComponent::StageMutation(const FY01PreparedMutation& Plan, FY01SourceTransaction& OutTransaction)
{
    OutTransaction = FY01SourceTransaction();
    if (bBusy || bInCall) { return Interrupt(); }
    auto R = Check(); if (!R.IsOk()) { return R; }
    return StageMutationAt(Plan, OutTransaction, Income->ReadNow());
}
FY01RuntimeResult UY01SourceEffectsComponent::StageMutationAt(const FY01PreparedMutation& Plan,
    FY01SourceTransaction& OutTransaction, double Now)
{
    if (Plan.Owner.Get() != this || Plan.Match != MatchId || Plan.JoinSequence != JoinSequence
        || Plan.Revision != Revision || Revision == MAX_uint64)
    { return Fail(EError::Stale, TEXT("计划不属于此owner/session或已过期")); }
    // 先兑现本次请求时刻之前的合法旧段。后续GE失败也不回滚已赚收入。
    auto R = Income->SettleAt(Now); if (!R.IsOk()) { return R; }
    if (!FMath::IsFinite(Now) || Now < 0) { return Fail(EError::Time, TEXT("来源请求时间无效")); }
    R = ValidateHandles(); if (!R.IsOk()) { return R; }
    TSet<FString> Removed; for (const auto& Id : Plan.Removals) { Removed.Add(Id.Id); }
    TArray<FY01SourceEffectSnapshot> Extra;
    for (const auto& Item : Plan.Additions) { if (!Item.bRepeat) { Extra.Append(Item.Effects); } }
    FY01ResolvedAttributes Attributes;
    double Rate = 0, Current = 0, HealthRatio = 0;
    R = ValidateTotals(Extra, Removed, Rate, Current, &Attributes, Plan.PlayerGrowth, &HealthRatio); if (!R.IsOk()) { return R; }
    ON_SCOPE_EXIT { if (Player.IsValid()) { Player->GetGrowth()->Drain(); } };
    TGuardValue<bool> Call(bInCall, true);
    bBusy = true; bInterrupted = false; PendingRemoved = MoveTemp(Removed);
    for (const auto& Item : Plan.Additions)
    {
        if (Item.bRepeat) { continue; }
        FRecord Record; Record.Source = Item.Source; Record.Effects = Item.Effects;
        Pending.Add(MoveTemp(Record));
    }
    Pending.Sort([](const FRecord& A, const FRecord& B)
    {
        return A.Source.Kind != B.Source.Kind ? A.Source.Kind < B.Source.Kind
            : A.Source.LocalKey.Compare(B.Source.LocalKey, ESearchCase::CaseSensitive) < 0;
    });
    PendingToken = FGuid::NewGuid(); PendingRate = Rate; PendingTime = Now; PendingCurrent = Current;
    PendingAttributes = MoveTemp(Attributes); PendingGrowth = Plan.PlayerGrowth.Get(PlayerGrowth);
    PendingHealthRatio = HealthRatio;
    bDerivedAttackChanged = PendingAttributes.AttackBonus != DerivedAttackBonus;
    const auto Attribute = UY01AttributeSet::GetAttackDamageAttribute();
    BeforeBase = ASC->GetNumericAttributeBase(Attribute); BeforeCurrent = ASC->GetNumericAttribute(Attribute);
    // 暴击/吸血/倍率合计只随Pending/PendingRemoved变化：它们是效果集合的纯合计，与成长/血量无关，
    // 非空时CommitMutationAt的提交块必然执行，故栅极无需逐字段比较三个新属性。
    bResolvedStateChanged = BeforeBase != CommittedBaseAttack
        || PendingAttributes.GoldGainPercent != CommittedAttributes.GoldGainPercent
        || !SameBreakdowns(PendingAttributes.Breakdowns, CommittedAttributes.Breakdowns);
    // 先可逆停用而非删handle：属性/停用委托重入时可精确恢复原handle。
    if (bDerivedAttackChanged && DerivedAttackHandle.IsValid())
    {
        auto Handle = DerivedAttackHandle;
        bDerivedAttackInhibited = true;
        const auto Returned = ASC->SetActiveGameplayEffectInhibit(MoveTemp(Handle), true, false);
        R = Check(true);
        if (!R.IsOk() || bInterrupted) { R = Fail(EError::Interrupted, TEXT("来源停用期间重入或会话失效")); }
        else if (!Returned.IsValid()) { R = Fail(EError::HandleLost, TEXT("停用期间旧handle丢失")); }
    }
    if (R.IsOk() && bDerivedAttackChanged && PendingAttributes.AttackBonus > 0)
    {
        auto Context = ASC->MakeEffectContext(); Context.AddSourceObject(this);
        auto Spec = ASC->MakeOutgoingSpec(UY01EquipmentAttackEffect::StaticClass(), 1, Context);
        if (!Spec.IsValid()) { R = Fail(EError::EffectRejected, TEXT("无法构造换算攻击GE")); }
        else
        {
            Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_EquipmentAttack, static_cast<float>(PendingAttributes.AttackBonus));
            const float BeforeValue = ASC->GetNumericAttribute(Attribute);
            PendingDerivedAttackHandle = ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data);
            R = Check(true);
            if (!R.IsOk() || bInterrupted) { R = Fail(EError::Interrupted, TEXT("应用GE后会话失效或委托重入")); }
            else if (!PendingDerivedAttackHandle.IsValid() || !ASC->GetActiveGameplayEffect(PendingDerivedAttackHandle))
            { R = Fail(EError::EffectRejected, TEXT("GAS拒绝换算攻击GE")); }
            else if (ASC->GetNumericAttribute(Attribute) <= BeforeValue)
            { R = Fail(EError::Precision, TEXT("真实ASC完全吞掉新增正值")); }
        }
    }
    if (R.IsOk()) { R = ValidatePending(); }
    if (!R.IsOk()) { const auto Undo = AbortPending(); return Undo.IsOk() ? R : Undo; }
    OutTransaction.Owner = this; OutTransaction.Token = PendingToken;
    return {};
}
FY01ScopedSourceMutation::FY01ScopedSourceMutation(FY01SourceTransaction&& InTransaction) : Transaction(MoveTemp(InTransaction))
{
    InTransaction = FY01SourceTransaction();
}
FY01ScopedSourceMutation::~FY01ScopedSourceMutation()
{
    if (auto* Owner = Transaction.Owner.Get()) { Owner->RollbackMutation(Transaction); }
}
FY01RuntimeResult FY01ScopedSourceMutation::Commit()
{
    return CommitAt([] {});
}
FY01RuntimeResult FY01ScopedSourceMutation::CommitAt(TFunctionRef<void()> CommitOwner)
{
    auto* Owner = Transaction.Owner.Get();
    if (!Owner) { return Fail(EError::Stale, TEXT("作用域事务owner已终止")); }
    const auto R = Owner->CommitMutationAt(Transaction, CommitOwner);
    if (R.IsOk()) { Transaction = FY01SourceTransaction(); }
    return R;
}
FY01RuntimeResult UY01SourceEffectsComponent::Observe(FY01SourceSnapshot& OutSnapshot) const
{
    OutSnapshot = FY01SourceSnapshot();
    if (bBusy || bInCall || (Player.IsValid() && Player->GetLoadout()->IsBusy()))
    { return Fail(EError::Busy, TEXT("最终来源快照在共同事务结束后可读")); }
    if (!ASC.IsValid()) { return Fail(EError::Disabled, TEXT("尚无来源会话")); }
    for (const auto& Record : Records) { OutSnapshot.Effects.Append(Record.Effects); }
    OutSnapshot.Effects.Sort(Before); OutSnapshot.Revision = Revision;
    OutSnapshot.BaseAttack = ASC->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute());
    OutSnapshot.CurrentAttack = ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute());
    return {};
}
FY01RuntimeResult UY01SourceEffectsComponent::ObserveAttributes(FY01ResolvedAttributes& Out) const
{
    Out = {};
    if (bBusy || bInCall || (Player.IsValid() && Player->GetLoadout()->IsBusy()))
    { return Fail(EError::Busy, TEXT("最终属性快照在共同事务结束后可读")); }
    if (!bInitialized || !ASC.IsValid()) { return Fail(EError::Disabled, TEXT("尚无来源会话")); }
    Out = CommittedAttributes;
    return {};
}
void UY01SourceEffectsComponent::EndSession()
{
    if (!IsInGameThread() || !GetOwner() || !GetOwner()->HasAuthority() || bEnded) { return; }
    // 必须在回滚、移除GE及其外部委托之前冻结结束时刻。
    bEnded = true;
    bAttributeRefreshDirty = false; SetComponentTickEnabled(false);
    if (Player.IsValid()) { Player->GetDamageEffects()->ResetCombat(); }
    RequestStop();
    SetActiveFlag(false);
    if (!bInCall)
    {
        ON_SCOPE_EXIT { if (Player.IsValid()) { Player->GetGrowth()->Drain(); } };
        TGuardValue<bool> Call(bInCall, true);
        if (bBusy) { AbortPending(); }
        else { FinishTransaction(); }
    }
}
void UY01SourceEffectsComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    EndSession();
    if (HasBegunPlay()) { Super::EndPlay(Reason); }
}
