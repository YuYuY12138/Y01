#include "Combat/Y01Combat.h"
#include "Player/Y01ReviveComponent.h"
#include "Player/Y01PlayerState.h"
#include "Match/Y01GameFlow.h"
#include "Combat/Y01Monster.h"
#include "Combat/Y01Core.h"
#include "AbilitySystemGlobals.h"
#include "GameplayEffectExtension.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameplayEffectAggregator.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01LoadoutComponent.h"
#include "Misc/ScopeExit.h"

namespace Y01Tags
{
    UE_DEFINE_GAMEPLAY_TAG(Data_Damage, "Data.Damage");
    UE_DEFINE_GAMEPLAY_TAG(Data_DamageDisplay, "Data.Damage.Display");
    UE_DEFINE_GAMEPLAY_TAG(Effect_Damage, "Effect.Y01.Damage");
    UE_DEFINE_GAMEPLAY_TAG(Effect_Healing, "Effect.Y01.Healing");
    UE_DEFINE_GAMEPLAY_TAG(Data_Healing, "Data.Healing");
    UE_DEFINE_GAMEPLAY_TAG(Data_EquipmentAttack, "Data.Equipment.AttackBonus");
}
UY01EquipmentAttackEffect::UY01EquipmentAttackEffect()
{
    DurationPolicy = EGameplayEffectDurationType::Infinite;
    FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
    Modifier.Attribute = UY01AttributeSet::GetAttackDamageAttribute();
    Modifier.ModifierOp = EGameplayModOp::Additive;
    FSetByCallerFloat Amount; Amount.DataTag = Y01Tags::Data_EquipmentAttack;
    Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Amount);
}

void UY01AttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION_NOTIFY(UY01AttributeSet, Health, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UY01AttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UY01AttributeSet, AttackDamage, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UY01AttributeSet, Armor, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UY01AttributeSet, HealthRegen, COND_None, REPNOTIFY_Always);
}
void UY01AttributeSet::OnAttributeAggregatorCreated(const FGameplayAttribute& Attribute, FAggregator* NewAggregator) const
{
    Super::OnAttributeAggregatorCreated(Attribute, NewAggregator);
    auto* OwnerASC = GetOwningAbilitySystemComponent();
    if (Attribute != GetAttackDamageAttribute() || !NewAggregator || !OwnerASC) { return; }
    // 包装原dirty链，确保GAS自身求值前已稳定排序；不覆盖属性值或基础值。
    // 未启用R36时来源组件直接返回，原有dirty/recursive及复制路径照常执行。
    auto Wrap = [OwnerASC](FAggregator::FOnAggregatorDirty& Event)
    {
        auto Original = Event;
        Event.Clear();
        Event.AddWeakLambda(OwnerASC, [OwnerASC, Original](FAggregator* Aggregator)
        {
            if (AActor* Owner = OwnerASC->GetOwner())
            {
                if (const auto* Sources = Owner->FindComponentByClass<UY01SourceEffectsComponent>())
                { Sources->StabilizeAttackAggregator(*Aggregator); }
            }
            Original.Broadcast(Aggregator);
        });
    };
    Wrap(NewAggregator->OnDirty);
    Wrap(NewAggregator->OnDirtyRecursive);
}
void UY01AttributeSet::ClampAttribute(const FGameplayAttribute& Attribute, float& Value) const
{
    if (!FMath::IsFinite(Value)) { Value = 0; }
    if (Attribute == GetHealthAttribute())
    {
        Value = FMath::Clamp(Value, 0.f, FMath::Max(0.f, GetMaxHealth()));
        if (const auto* Player = Cast<AY01PlayerState>(GetOwningActor()); Player && Player->HasAuthority())
        {
            const auto* Mode = GetWorld()->GetAuthGameMode<AY01GameMode>();
            if (Player->GetRevive()->LocksDeadHealth()) { Value = 0; }
            else if (Mode && Mode->IsGameplayFrozen()) { Value = GetHealth(); }
        }
        if (const auto* Core = Cast<AY01Core>(GetOwningActor()); Core && Core->HasAuthority())
        {
            if (Core->GetSnapshot().bFallen) { Value = 0; }
            else if (Core->GetSnapshot().bFrozen) { Value = GetHealth(); }
        }
    }
    else if (Attribute == GetMaxHealthAttribute() || Attribute == GetAttackDamageAttribute()
        || Attribute == GetArmorAttribute() || Attribute == GetHealthRegenAttribute()) { Value = FMath::Max(0.f, Value); }
}
void UY01AttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) { ClampAttribute(Attribute, NewValue); }
void UY01AttributeSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const { ClampAttribute(Attribute, NewValue); }
bool UY01AttributeSet::PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data)
{
    if (!GetOwningActor() || !GetOwningActor()->HasAuthority() || !FMath::IsFinite(Data.EvaluatedData.Magnitude)) { return false; }
    if (Data.EvaluatedData.Attribute == GetHealthAttribute() && Data.EvaluatedData.Magnitude < 0)
    {
        const auto* Target = Cast<UY01CombatAbilitySystem>(GetOwningAbilitySystemComponent());
        const auto* Source = Cast<UY01CombatAbilitySystem>(Data.EffectSpec.GetContext().GetOriginalInstigatorAbilitySystemComponent());
        if (Target && Target->TargetPermission && (!Source || !Source->IsCombatPairAllowed(Target))) { return false; }
    }
    if (Data.EvaluatedData.Attribute == GetHealthAttribute())
    {
        // 每个获准的Health执行都入栈，包括回血/消耗，防止内层借用外层上下文。
        FHealthExecution Execution; Execution.Callback = &Data;
        FGameplayTagContainer Tags; Data.EffectSpec.GetAllAssetTags(Tags);
        const bool bDamage = Data.EvaluatedData.ModifierOp == EGameplayModOp::Additive && Data.EvaluatedData.Magnitude < 0
            && Tags.HasTagExact(Y01Tags::Effect_Damage);
        const bool bHealing = Data.EvaluatedData.ModifierOp == EGameplayModOp::Additive && Data.EvaluatedData.Magnitude > 0
            && Tags.HasTagExact(Y01Tags::Effect_Healing);
        if (bDamage || bHealing)
        {
            Execution.bDamageTagged = bDamage; Execution.bHealingTagged = bHealing;
            if (bDamage)
            {
                const float Display = Data.EffectSpec.GetSetByCallerMagnitude(Y01Tags::Data_DamageDisplay, false, -Data.EvaluatedData.Magnitude);
                if (FMath::IsFinite(Display) && Display > 0) { Execution.Pending.Event.DisplayAmount = Display; }
                if (auto* Kills = GetWorld()->GetSubsystem<UY01KillRewards>())
                {
                    Execution.KillAuthorization = Kills->Capture(
                        Cast<UY01CombatAbilitySystem>(Data.EffectSpec.GetContext().GetOriginalInstigatorAbilitySystemComponent()),
                        Cast<UY01CombatAbilitySystem>(GetOwningAbilitySystemComponent()));
                }
            }
            auto* Service = GetWorld()->GetSubsystem<UY01DamageEvents>();
            if (Service)
            {
                Execution.Service = Service;
                Execution.Pending = Service->Capture(
                    Cast<UY01CombatAbilitySystem>(Data.EffectSpec.GetContext().GetOriginalInstigatorAbilitySystemComponent()),
                    Cast<UY01CombatAbilitySystem>(GetOwningAbilitySystemComponent()));
                Execution.Pending.Event.Type = bHealing ? EY01DamageEventType::Healing : EY01DamageEventType::Damage;
                if (bDamage)
                {
                    const float Display = Data.EffectSpec.GetSetByCallerMagnitude(Y01Tags::Data_DamageDisplay, false, -Data.EvaluatedData.Magnitude);
                    if (FMath::IsFinite(Display) && Display > 0) { Execution.Pending.Event.DisplayAmount = Display; }
                }
            }
        }
        HealthExecutions.Add(MoveTemp(Execution));
    }
    return true;
}
void UY01AttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
    const FGameplayAttribute& Attribute = Data.EvaluatedData.Attribute;
    FHealthExecution Completed;
    if (Attribute == GetHealthAttribute() && !HealthExecutions.IsEmpty() && HealthExecutions.Last().Callback == &Data)
    { Completed = HealthExecutions.Pop(EAllowShrinking::No); }
    float Value = GetOwningAbilitySystemComponent()->GetNumericAttributeBase(Attribute);
    float Clamped = Value;
    ClampAttribute(Attribute, Clamped);
    if (Clamped != Value) { GetOwningAbilitySystemComponent()->SetNumericAttributeBase(Attribute, Clamped); }
    // 原钳制和死亡/奖励链不变；发布前已出栈，允许消费端再次施加GE。
    if (Completed.KillEventId.IsValid()) { GetWorld()->GetSubsystem<UY01KillRewards>()->Complete(Completed.KillEventId); }
    if (Completed.bFrozen && Completed.Service.IsValid()) { Completed.Service->Commit(Completed.Pending); }
}
void UY01AttributeSet::PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
    // UE5.7先写CurrentValue，再到这里，然后才广播ASC属性/死亡委托。
    bool bActualTaggedDamage = false;
    if (Attribute == GetHealthAttribute() && !HealthExecutions.IsEmpty() && !HealthExecutions.Last().bFrozen)
    {
        auto& Execution = HealthExecutions.Last(); Execution.bFrozen = true;
        const float Actual = OldValue - NewValue;
        const float Restored = NewValue - OldValue;
        if (FMath::IsFinite(OldValue) && FMath::IsFinite(NewValue) && FMath::IsFinite(Actual) && Actual > 0)
        {
            bActualTaggedDamage = Execution.bDamageTagged;
            if (Execution.bDamageTagged && OldValue > 0 && NewValue == 0)
            { Execution.KillEventId = GetWorld()->GetSubsystem<UY01KillRewards>()->FreezeDeath(Execution.KillAuthorization, OldValue, NewValue); }
            Execution.Pending.Event.ActualDamage = Actual;
            Execution.Pending.Event.DisplayAmount = FMath::Max(Execution.Pending.Event.DisplayAmount, Actual);
            // 只冻结交接身份；仍等PostGameplayEffectExecute提交，不在死亡回调前发事件。
            if (NewValue <= 0 && Execution.Service.IsValid())
            { Execution.Service->StageFinalDamage(Execution.Pending, Cast<UY01CombatAbilitySystem>(GetOwningAbilitySystemComponent())); }
        }
        else if (Execution.bHealingTagged && FMath::IsFinite(Restored) && Restored > 0)
        {
            Execution.Pending.Event.ActualDamage = 0;
            Execution.Pending.Event.DisplayAmount = Restored;
            Execution.Pending.Event.Type = EY01DamageEventType::Healing;
        }
    }
    if (Attribute == GetHealthAttribute())
    {
        if (auto* Monster = Cast<AY01Monster>(GetOwningActor()))
        { Monster->RecordHealthBarHealthChange(bActualTaggedDamage, OldValue > 0 && NewValue <= 0); }
        if (auto* Core = Cast<AY01Core>(GetOwningActor()))
        { Core->RecordHealthChange(bActualTaggedDamage, OldValue > 0 && NewValue <= 0); }
    }
    if (Attribute == GetHealthAttribute() && OldValue > 0 && NewValue <= 0)
    {
        if (auto* Player = Cast<AY01PlayerState>(GetOwningActor())) { Player->GetRevive()->RecordDeath(); }
        if (auto* ASC = Cast<UY01CombatAbilitySystem>(GetOwningAbilitySystemComponent())) { ASC->InvalidateCombatSession(); }
    }
    if (Attribute == GetMaxHealthAttribute() && GetHealth() > NewValue && GetOwningActor()->HasAuthority())
    { GetOwningAbilitySystemComponent()->SetNumericAttributeBase(GetHealthAttribute(), NewValue); }
}
void UY01AttributeSet::OnRep_Health(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UY01AttributeSet, Health, OldValue); }
void UY01AttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UY01AttributeSet, MaxHealth, OldValue); }
void UY01AttributeSet::OnRep_AttackDamage(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UY01AttributeSet, AttackDamage, OldValue); }
void UY01AttributeSet::OnRep_Armor(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UY01AttributeSet, Armor, OldValue); }
void UY01AttributeSet::OnRep_HealthRegen(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UY01AttributeSet, HealthRegen, OldValue); }
void UY01DamageExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters, FGameplayEffectCustomExecutionOutput& Output) const
{
    const auto* Source = Cast<UY01CombatAbilitySystem>(Parameters.GetSourceAbilitySystemComponent());
    const auto* Target = Parameters.GetTargetAbilitySystemComponent();
    if (!Source || !Target || !Target->GetOwner() || !Target->GetOwner()->HasAuthority()
        || !Source->IsValidCombatTarget(Target->GetAvatarActor())) { return; }
    const float Snapshot = Parameters.GetOwningSpec().GetSetByCallerMagnitude(Y01Tags::Data_Damage, false, -1.f);
    if (!FMath::IsFinite(Snapshot) || Snapshot < 0) { return; }
    const float Damage = FMath::Max(0.f, Snapshot - Target->GetNumericAttribute(UY01AttributeSet::GetArmorAttribute()));
    const float Health = Target->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    if (Damage > 0 && FMath::IsFinite(Health) && Health > 0)
    {
        if (auto* Spec = Parameters.GetOwningSpecForPreExecuteMod()) { Spec->SetSetByCallerMagnitude(Y01Tags::Data_DamageDisplay, Damage); }
        Output.AddOutputModifier(FGameplayModifierEvaluatedData(UY01AttributeSet::GetHealthAttribute(),
            EGameplayModOp::Additive, -FMath::Min(Damage, Health)));
    }
}
UY01DamageEffect::UY01DamageEffect()
{
    auto* Tags = CreateDefaultSubobject<UAssetTagsGameplayEffectComponent>(TEXT("DamageAssetTags"));
    GEComponents.Add(Tags); FInheritedTagContainer Changes; Changes.AddTag(Y01Tags::Effect_Damage);
    Tags->SetAndApplyAssetTagChanges(Changes);
    DurationPolicy = EGameplayEffectDurationType::Instant;
    Executions.AddDefaulted_GetRef().CalculationClass = UY01DamageExecution::StaticClass();
}
UY01HealingEffect::UY01HealingEffect()
{
    auto* Tags = CreateDefaultSubobject<UAssetTagsGameplayEffectComponent>(TEXT("HealingAssetTags"));
    GEComponents.Add(Tags); FInheritedTagContainer Changes; Changes.AddTag(Y01Tags::Effect_Healing);
    Tags->SetAndApplyAssetTagChanges(Changes);
    DurationPolicy = EGameplayEffectDurationType::Instant;
    FGameplayModifierInfo& Modifier = Modifiers.AddDefaulted_GetRef();
    Modifier.Attribute = UY01AttributeSet::GetHealthAttribute(); Modifier.ModifierOp = EGameplayModOp::Additive;
    FSetByCallerFloat Amount; Amount.DataTag = Y01Tags::Data_Healing;
    Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Amount);
}
void UY01LoadoutAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
    FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
    auto* ASC = ActorInfo ? Cast<UY01CombatAbilitySystem>(ActorInfo->AbilitySystemComponent.Get()) : nullptr;
    auto* Loadout = ASC ? ASC->ActiveLoadout() : nullptr;
    const bool OwnsActivation = Loadout && Loadout->ActivatingHandle == Handle;
    ON_SCOPE_EXIT { if (OwnsActivation) { Loadout->FinishActivation(Handle); } };
    if (!ASC || !ASC->IsAbilityReady(Handle) || (Loadout && (!OwnsActivation || !Loadout->CanActivate(Handle))))
    { EndAbility(Handle, ActorInfo, ActivationInfo, false, true); return; }
    ExecuteSkill(Handle, ActorInfo, ActivationInfo, TriggerEventData);
}
UY01BasicAttackAbility::UY01BasicAttackAbility()
{
    InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
    NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
    NetSecurityPolicy = EGameplayAbilityNetSecurityPolicy::ServerOnly;
}
bool UY01BasicAttackAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
    const auto* ASC = ActorInfo ? Cast<UY01CombatAbilitySystem>(ActorInfo->AbilitySystemComponent.Get()) : nullptr;
    if (!ASC || !ActorInfo->OwnerActor.IsValid() || !ActorInfo->OwnerActor->HasAuthority()
        || !ASC->IsAbilityReady(Handle) || !ASC->IsValidCombatTarget(ASC->GetCombatTarget())) { return false; }
    const float Damage = ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute());
    return FMath::IsFinite(Damage) && Damage >= 0 && Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
}
void UY01BasicAttackAbility::ExecuteSkill(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
    FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
    auto* ASC = ActorInfo ? Cast<UY01CombatAbilitySystem>(ActorInfo->AbilitySystemComponent.Get()) : nullptr;
    if (!ASC || !ASC->IsValidCombatTarget(ASC->GetCombatTarget()) || !CommitAbility(Handle, ActorInfo, ActivationInfo))
    { EndAbility(Handle, ActorInfo, ActivationInfo, false, true); return; }
    auto* Target = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(ASC->GetCombatTarget());
    auto Context = ASC->MakeEffectContext();
    Context.SetAbility(this); Context.AddSourceObject(ASC->GetOwner());
    auto Spec = ASC->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, Context);
    // 伤害完全从服务器属性读取，不使用RPC参数或客户端事件载荷。
    Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()));
    ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data, Target);
    EndAbility(Handle, ActorInfo, ActivationInfo, false, false);
}
UY01CombatAbilitySystem::UY01CombatAbilitySystem() { SetIsReplicatedByDefault(true); }
void UY01CombatAbilitySystem::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION_NOTIFY(UY01CombatAbilitySystem, InitialMoveSpeed, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME(UY01CombatAbilitySystem, UnitStats);
    DOREPLIFETIME(UY01CombatAbilitySystem, NextAutoAttackTime);
}
void UY01CombatAbilitySystem::InitAbilityActorInfo(AActor* InOwnerActor, AActor* InAvatarActor)
{
    Super::InitAbilityActorInfo(InOwnerActor, InAvatarActor);
    OnRep_InitialMoveSpeed();
}
void UY01CombatAbilitySystem::OnRep_InitialMoveSpeed()
{
    if (!FMath::IsFinite(InitialMoveSpeed) || InitialMoveSpeed < 0) { return; }
    if (auto* Avatar = Cast<ACharacter>(GetAvatarActor()))
    { Avatar->GetCharacterMovement()->MaxWalkSpeed = InitialMoveSpeed; }
}

bool UY01CombatAbilitySystem::TryInitializeCore(const FY01CoreSettings& Settings, FString& Error)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !Cast<AY01Core>(GetOwner()) || GetAvatarActor() != GetOwner()
        || bInitialized || bInitializing || !Settings.IsValid() || !GetSet<UY01AttributeSet>())
    { Error = TEXT("核心ASC初始化身份、配置或属性集非法"); return false; }
    TGuardValue<bool> Guard(bInitializing, true);
    SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(), Settings.MaxHealth);
    SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), Settings.MaxHealth);
    SetNumericAttributeBase(UY01AttributeSet::GetHealthRegenAttribute(), Settings.HealthRegen);
    SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 0);
    SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), 0);
    UnitStats = {}; UnitStats.Health = Settings.MaxHealth; UnitStats.HealthRegen = Settings.HealthRegen;
    InitialMoveSpeed = 0; AttackRange = 0; LastRegenTime = GetWorld()->GetTimeSeconds(); bInitialized = true;
    GetOwner()->ForceNetUpdate(); return true;
}
bool UY01CombatAbilitySystem::TryInitializeFromData(const FY01DataCatalog& Catalog, const FY01DataRef& Definition, FString& OutError)
{
    OutError.Reset();
    auto Reject = [&](const FString& Reason) { OutError = Reason; return false; };
    if (!GetOwner() || !GetOwner()->HasAuthority()) { return Reject(TEXT("初始化必须由服务器执行")); }
    if (Cast<AY01Core>(GetOwner())) { return Reject(TEXT("核心只能使用专用初始化")); }
    if (ActiveLoadout()) { return Reject(TEXT("本局注册表启用后禁止按定义授予或重复初始化")); }
    if (bInitialized)
    {
        return DefinitionRef.Type == Definition.Type && DefinitionRef.RowName == Definition.RowName
            ? true : Reject(TEXT("实体已经初始化，不能替换定义"));
    }
    if (bInitializing) { return Reject(TEXT("初始化正在执行")); }
    TGuardValue<bool> Guard(bInitializing, true);
    float Health = 0, Damage = 0, Range = 0, Speed = 0, Armor = 0, Regen = 0;
    FY01UnitCombatStats PendingStats;
    TArray<FY01DataRef> Equipment, Skills;
    FY01DataValidation Validation;
    if (Definition.Type == EY01TableKind::Character)
    {
        const auto* Row = Catalog.FindRow<FY01CharacterRow>(Definition, Validation);
        if (!Row) { return Reject(TEXT("角色定义引用无效")); }
        if (!Y01Data::ValidUnitStats(*Row)) { return Reject(TEXT("统一基础字段非法")); }
        for (float V : {Row->Strength, Row->Agility, Row->Wisdom})
        { if (!FMath::IsFinite(V) || V < 0) { return Reject(TEXT("显示三维必须有限且非负")); } }
        Armor = Row->Armor; Regen = Row->HealthRegen;
        PendingStats.ReadRow(*Row); PendingStats.Strength = Row->Strength; PendingStats.Agility = Row->Agility; PendingStats.Wisdom = Row->Wisdom;
        if (!Row->R33TestGrowth.IsValid()) { return Reject(TEXT("R33测试成长必须有限且非负")); }
        Health = Row->Health; Damage = Row->AttackDamage; Range = Row->Range; Speed = Row->MoveSpeed;
        Equipment = Row->InitialEquipment; Skills = Row->InitialSkills;
        if (Equipment.Num() > Y01Data::EquipmentSlotCount || Skills.Num() > Y01Data::SkillSlotCount)
        { return Reject(TEXT("角色初始装配超过六装备或四技能槽")); }
    }
    else if (Definition.Type == EY01TableKind::Monster)
    {
        const auto* Row = Catalog.FindRow<FY01MonsterRow>(Definition, Validation);
        if (!Row) { return Reject(TEXT("怪物定义引用无效")); }
        if (!Y01Data::ValidUnitStats(*Row)) { return Reject(TEXT("统一基础字段非法")); }
        if (!FMath::IsFinite(Row->DetectionRadius) || Row->DetectionRadius < 0)
        { return Reject(TEXT("Monster.DetectionRadius必须有限且非负")); }
        Armor = Row->Armor; Regen = Row->HealthRegen; PendingStats.ReadRow(*Row);
        Health = Row->Health; Damage = Row->AttackDamage; Range = Row->Range; Speed = Row->MoveSpeed; Skills = Row->Skills;
    }
    else { return Reject(TEXT("初始化仅接受角色或怪物定义")); }
    if (!FMath::IsFinite(Health) || Health <= 0 || !FMath::IsFinite(Damage) || Damage < 0
        || !FMath::IsFinite(Range) || Range < 0 || !FMath::IsFinite(Speed) || Speed < 0)
    { return Reject(TEXT("初始属性必须有限，生命为正，其余非负")); }

    struct FPendingGrant
    {
        FY01GrantedSource Record;
        TSubclassOf<UGameplayAbility> AbilityClass;
    };
    TArray<FPendingGrant> Pending;
    FPendingGrant& Basic = Pending.AddDefaulted_GetRef();
    Basic.Record.Source = Definition; Basic.AbilityClass = UY01BasicAttackAbility::StaticClass();
    auto LoadEffect = [&](const TSoftClassPtr<UGameplayEffect>& Path, FPendingGrant& Grant)
    {
        if (Path.IsNull()) { return true; }
        Grant.Record.EffectClass = Path.LoadSynchronous();
        return Grant.Record.EffectClass && !Grant.Record.EffectClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists);
    };
    double EquipmentTotal = Damage;
    for (int32 I = 0; I < Equipment.Num(); ++I)
    {
        const auto* Row = Catalog.FindRow<FY01EquipmentRow>(Equipment[I], Validation);
        if (!Row) { return Reject(TEXT("初始装备引用无效")); }
        if (!Row->EffectIds.IsEmpty() || !Row->DamageEffectIds.IsEmpty()) { return Reject(TEXT("实例装备必须通过来源运行时授予，旧初始化不支持")); }
        if (!Row->IsValid()) { return Reject(TEXT("初始装备品质或AttackBonus非法")); }
        EquipmentTotal += Row->AttackBonus;
        if (EquipmentTotal > MAX_flt) { return Reject(TEXT("初始装备攻击总和溢出")); }
        FPendingGrant& Grant = Pending.AddDefaulted_GetRef();
        Grant.Record.Source = Equipment[I]; Grant.Record.Slot = I;
        Grant.Record.AttackBonus = Row->AttackBonus; Grant.Record.EquipmentQuality = Row->Quality;
        if (!LoadEffect(Row->Effect, Grant)) { return Reject(TEXT("初始装备GE类不可用")); }
    }
    for (int32 I = 0; I < Skills.Num(); ++I)
    {
        const auto* Row = Catalog.FindRow<FY01SkillRow>(Skills[I], Validation);
        if (!Row) { return Reject(TEXT("初始技能引用无效")); }
        if (!Row->EffectIds.IsEmpty() || !Row->DamageEffectIds.IsEmpty()) { return Reject(TEXT("实例技能必须通过来源运行时授予，旧初始化不支持")); }
        if ((Row->Type != EY01SkillType::Active && Row->Type != EY01SkillType::Passive)
            || !FMath::IsFinite(Row->CooldownSeconds) || Row->CooldownSeconds < 0
            || !Row->TestSkill.IsValid() || (Row->TestSkill.Mode != EY01TestSkillMode::None && Row->Type != EY01SkillType::Active))
        { return Reject(TEXT("技能类型、CD或程序测试配置非法")); }
        FPendingGrant& Grant = Pending.AddDefaulted_GetRef();
        Grant.Record.Source = Skills[I]; Grant.Record.Slot = I;
        Grant.Record.SkillType = Row->Type; Grant.Record.CooldownSeconds = Row->CooldownSeconds;
        Grant.Record.TestSkill = Row->TestSkill;
        if (!LoadEffect(Row->Effect, Grant)) { return Reject(TEXT("初始技能GE类不可用")); }
        if (!Row->Ability.IsNull())
        {
            Grant.AbilityClass = Row->Ability.LoadSynchronous();
            if (!Grant.AbilityClass || Grant.AbilityClass->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
            { return Reject(TEXT("初始技能GA类不可用")); }
            const auto* Ability = Grant.AbilityClass->GetDefaultObject<UGameplayAbility>();
            if (Ability->GetNetExecutionPolicy() != EGameplayAbilityNetExecutionPolicy::ServerOnly
                || Ability->GetNetSecurityPolicy() != EGameplayAbilityNetSecurityPolicy::ServerOnly)
            { return Reject(TEXT("首轮技能GA必须限制为服务器执行和服务器发起")); }
        }
    }
    if (!GetSet<UY01AttributeSet>()) { return Reject(TEXT("缺少战斗属性集")); }
    // 引用、容量、数值和软类全部验证完成之后才允许改动GAS。
    if (!AbilityActorInfo.IsValid() || !AbilityActorInfo->OwnerActor.IsValid()) { InitAbilityActorInfo(GetOwner(), GetOwner()); }
    const float OldMax = GetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute());
    const float OldHealth = GetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute());
    const float OldDamage = GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute());
    const float OldArmor = GetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute());
    const float OldRegen = GetNumericAttributeBase(UY01AttributeSet::GetHealthRegenAttribute());
    SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(), Health);
    SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), Health);
    SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), Damage);
    SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), Armor);
    SetNumericAttributeBase(UY01AttributeSet::GetHealthRegenAttribute(), Regen);
    auto Rollback = [&]()
    {
        for (auto& Grant : Pending)
        {
            if (Grant.Record.Ability.IsValid()) { ClearAbility(Grant.Record.Ability); }
            if (Grant.Record.Effect.IsValid()) { RemoveActiveGameplayEffect(Grant.Record.Effect); }
            if (Grant.Record.EquipmentAttackEffect.IsValid()) { RemoveActiveGameplayEffect(Grant.Record.EquipmentAttackEffect); }
        }
        SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(), OldMax);
        SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), OldHealth);
        SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), OldDamage);
        SetNumericAttributeBase(UY01AttributeSet::GetArmorAttribute(), OldArmor);
        SetNumericAttributeBase(UY01AttributeSet::GetHealthRegenAttribute(), OldRegen);
    };
    for (auto& Grant : Pending)
    {
        if (Grant.Record.EffectClass)
        {
            auto Context = MakeEffectContext(); Context.AddSourceObject(GetOwner());
            auto Spec = MakeOutgoingSpec(Grant.Record.EffectClass, 1, Context);
            Grant.Record.Effect = ApplyGameplayEffectSpecToSelf(*Spec.Data);
            Grant.Record.bEffectApplied = Grant.Record.Effect.WasSuccessfullyApplied();
            if (!Grant.Record.bEffectApplied) { Rollback(); return Reject(TEXT("初始GE被拒绝，已回滚")); }
        }
        if (Grant.AbilityClass)
        {
            Grant.Record.Ability = GiveAbility(FGameplayAbilitySpec(Grant.AbilityClass, 1, INDEX_NONE, GetOwner()));
            if (!Grant.Record.Ability.IsValid()) { Rollback(); return Reject(TEXT("初始GA授予失败，已回滚")); }
        }
    }
    for (auto& Grant : Pending)
    {
        if (Grant.Record.Source.Type == EY01TableKind::Equipment && !ApplyEquipmentAttack(Grant.Record))
        { Rollback(); return Reject(TEXT("初始装备攻击GE被拒绝或溢出，已回滚")); }
    }
    BasicAttackHandle = Pending[0].Record.Ability;
    for (auto& Grant : Pending) { GrantedSources.Add(MoveTemp(Grant.Record)); }
    InitialEquipment = MoveTemp(Equipment); InitialSkills = MoveTemp(Skills);
    DefinitionRef = Definition; AttackRange = Range; InitialMoveSpeed = Speed; bInitialized = true;
    UnitStats = PendingStats; LastRegenTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0;
    const auto S = GetUnitStats();
    UE_LOG(LogTemp, Display, TEXT("Y01R34 CONFIG Source=%s Caster=%s Row=%s DS=%.17g Health=%g AttackDamage=%g AttackInterval=%g AttackType=%d Range=%g MeleeAngle=%g ProjectileSpeed=%g Armor=%g MoveSpeed=%g HealthRegen=%g Strength=%g Agility=%g Wisdom=%g"),
        *GetNameSafe(GetOwner()), *GetNameSafe(GetAvatarActor()), *DefinitionRef.RowName.ToString(), LastRegenTime, S.Health, S.AttackDamage, S.AttackInterval,
        static_cast<int32>(S.AttackType), S.Range, S.MeleeAngle, S.ProjectileSpeed, S.Armor, Speed, S.HealthRegen, S.Strength, S.Agility, S.Wisdom);
    for (const auto& Grant : GrantedSources)
    { if (Grant.Source.Type == EY01TableKind::Equipment) { LogEquipment(TEXT("CONFIG"), Grant); } }
    OnRep_InitialMoveSpeed();
    GetOwner()->ForceNetUpdate();
    return true;
}
bool UY01CombatAbilitySystem::ApplyEquipmentAttack(FY01GrantedSource& Grant)
{
    if (Grant.AttackBonus == 0) { return true; }
    const float Current = GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute());
    if (!FMath::IsFinite(Current) || !FMath::IsFinite(Grant.AttackBonus) || Grant.AttackBonus < 0
        || static_cast<double>(Current) + Grant.AttackBonus > MAX_flt) { return false; }
    auto Context = MakeEffectContext(); Context.AddSourceObject(GetOwner());
    auto Spec = MakeOutgoingSpec(UY01EquipmentAttackEffect::StaticClass(), 1, Context);
    if (!Spec.IsValid()) { return false; }
    Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_EquipmentAttack, Grant.AttackBonus);
    Grant.EquipmentAttackEffect = ApplyGameplayEffectSpecToSelf(*Spec.Data);
    return Grant.EquipmentAttackEffect.IsValid() && GetActiveGameplayEffect(Grant.EquipmentAttackEffect) != nullptr;
}
void UY01CombatAbilitySystem::LogEquipment(const TCHAR* Action, const FY01GrantedSource& Grant) const
{
    UE_LOG(LogTemp, Display, TEXT("Y01R34 EQ_%s Source=%s DS=%.17g Slot=%d Row=%s Quality=%s AttackBonus=%g Base=%g Current=%g Effect=%s"),
        Action, *GetNameSafe(GetOwner()), GetWorld() ? static_cast<double>(GetWorld()->GetTimeSeconds()) : 0., Grant.Slot,
        *Grant.Source.RowName.ToString(), *StaticEnum<EY01Quality>()->GetNameStringByValue(static_cast<int64>(Grant.EquipmentQuality)),
        Grant.AttackBonus, GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute()),
        GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), *GetNameSafe(Grant.EffectClass));
}
bool UY01CombatAbilitySystem::TrySetEquipmentSlot(const FY01DataCatalog& Catalog, int32 Slot, const FY01DataRef& Equipment, FString& OutError)
{
    OutError.Reset();
    auto Reject = [&](const TCHAR* Reason) { OutError = Reason; return false; };
    if (!GetOwner() || !GetOwner()->HasAuthority() || !bInitialized || Slot < 0 || Slot >= Y01Data::EquipmentSlotCount)
    { return Reject(TEXT("装备槽操作需要已初始化权威实体和有效槽")); }
    const bool bEmpty = Equipment.Type == EY01TableKind::None && Equipment.RowName.IsNone();
    FY01DataValidation Query; const FY01EquipmentRow* Row = nullptr;
    if (!bEmpty)
    {
        if (Equipment.Type != EY01TableKind::Equipment || Equipment.RowName.IsNone()
            || !(Row = Catalog.FindRow<FY01EquipmentRow>(Equipment, Query))) { return Reject(TEXT("装备引用无效")); }
        if (!Row->IsValid()) { return Reject(TEXT("装备品质或AttackBonus非法")); }
        if (!Row->EffectIds.IsEmpty() || !Row->DamageEffectIds.IsEmpty()) { return Reject(TEXT("旧装备槽不能静默忽略实例效果")); }
    }
    const int32 OldIndex = GrantedSources.IndexOfByPredicate([&](const auto& G) { return G.Source.Type == EY01TableKind::Equipment && G.Slot == Slot; });
    FY01GrantedSource Old;
    if (OldIndex != INDEX_NONE)
    {
        Old = GrantedSources[OldIndex];
        if (Old.AttackBonus > 0 && !GetActiveGameplayEffect(Old.EquipmentAttackEffect))
        { return Reject(TEXT("原装备攻击句柄已失效，拒绝继续调试")); }
        if (Old.Source.Type == Equipment.Type && Old.Source.RowName == Equipment.RowName) { return true; }
        // 任意旧GE可能有瞬时/周期/连锁副作用；不假装靠减base能撤销。整槽保留，不开放反复获利。
        if (Old.EffectClass) { return Reject(TEXT("旧Effect装备仅兼容初始化，调试不允许卸换")); }
    }
    else if (bEmpty) { return true; }
    if (Row && !Row->Effect.IsNull()) { return Reject(TEXT("调试新装仅支持空Effect的AttackBonus装备")); }
    FY01GrantedSource Next;
    Next.Source = Equipment; Next.Slot = Slot;
    if (Row)
    {
        Next.AttackBonus = Row->AttackBonus; Next.EquipmentQuality = Row->Quality;
        LogEquipment(TEXT("CONFIG"), Next);
        // 先尝试新GE；拒绝时旧GE和槽都未改变。为临时并存也做保守溢出检查。
        if (!ApplyEquipmentAttack(Next)) { return Reject(TEXT("装备攻击GE被拒绝或溢出")); }
    }
    if (Old.EquipmentAttackEffect.IsValid() && !RemoveActiveGameplayEffect(Old.EquipmentAttackEffect))
    {
        if (Next.EquipmentAttackEffect.IsValid()) { RemoveActiveGameplayEffect(Next.EquipmentAttackEffect); }
        return Reject(TEXT("旧装备移除失败，已撤销新GE"));
    }
    if (OldIndex != INDEX_NONE) { GrantedSources.RemoveAt(OldIndex); }
    if (Row) { GrantedSources.Add(Next); }
    if (OldIndex != INDEX_NONE) { LogEquipment(TEXT("UNEQUIP"), Old); }
    if (Row) { LogEquipment(TEXT("EQUIP"), Next); }
    GetOwner()->ForceNetUpdate(); return true;
}
FY01UnitCombatStats UY01CombatAbilitySystem::GetUnitStats() const
{
    auto Result = UnitStats;
    Result.Health = GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute());
    Result.AttackDamage = GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute());
    Result.Armor = GetNumericAttribute(UY01AttributeSet::GetArmorAttribute());
    Result.HealthRegen = GetNumericAttribute(UY01AttributeSet::GetHealthRegenAttribute());
    if (const auto* Character = Cast<ACharacter>(GetAvatarActor())) { Result.MoveSpeed = Character->GetCharacterMovement()->MaxWalkSpeed; }
    return Result;
}
bool UY01CombatAbilitySystem::IsAutoAttackInRange(const AActor* Target) const
{
    return IsLegalCombatTarget(Target) && FVector::DistSquared2D(GetAvatarActor()->GetActorLocation(), Target->GetActorLocation())
        <= FMath::Square(static_cast<double>(UnitStats.Range));
}
bool UY01CombatAbilitySystem::ApplyHealingTo(UY01CombatAbilitySystem* Target, float Amount)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !GetWorld() || !bInitialized || !Target || !Target->IsCombatInitialized()
        || Target->GetWorld() != GetWorld() || !FMath::IsFinite(Amount) || Amount <= 0
        || Target->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()) <= 0) { return false; }
    auto Spec = MakeOutgoingSpec(UY01HealingEffect::StaticClass(), 1, MakeEffectContext());
    if (!Spec.IsValid()) { return false; }
    Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Healing, Amount);
    ApplyGameplayEffectSpecToTarget(*Spec.Data, Target);
    return true;
}
bool UY01CombatAbilitySystem::TryAutoAttack(AActor* Target, TFunctionRef<bool(float)> StartAttack)
{
    if (bAutoAttackStarting || !GetWorld() || !IsAutoAttackInRange(Target)) { return false; }
    auto* Loadout = ActiveLoadout();
    const auto* Sources = GetOwner()->FindComponentByClass<UY01SourceEffectsComponent>();
    if (Sources && Sources->bInitialized && (Sources->bInCall || !Sources->Check().IsOk())) { return false; }
    // 非GA普攻共用物品事务锁，但不借用GA句柄，避免嵌套GA误认并释放外层锁。
    if (Loadout && !Loadout->BeginAutoAttack()) { return false; }
    ON_SCOPE_EXIT { if (Loadout) { Loadout->FinishAutoAttack(); } };
    const double Now = GetWorld()->GetTimeSeconds();
    const float Damage = GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute());
    if (Now < NextAutoAttackTime || !FMath::IsFinite(Damage) || Damage < 0) { return false; }
    TGuardValue<bool> Guard(bAutoAttackStarting, true);
    if (!StartAttack(Damage)) { return false; }
    NextAutoAttackTime = Now + static_cast<double>(UnitStats.AttackInterval);
    GetOwner()->ForceNetUpdate(); return true;
}
void UY01CombatAbilitySystem::UpdateRegenDisplay(float Gained, double Now, double Elapsed, bool bKeep)
{
    auto Reset = [&]
    {
        RegenDisplayAmount = 0; RegenDisplayStartedAt = -1; RegenDisplaySession = CombatSession;
        RegenDisplayEpoch.Invalidate(); RegenDisplayRegion = INDEX_NONE;
    };
    auto* Service = GetWorld() ? GetWorld()->GetSubsystem<UY01DamageEvents>() : nullptr;
    FY01PendingDamage Probe;
    if (!bKeep || !Service || !Service->ResolveAudience(this, this, Probe) || Probe.Event.RegionId < 0)
    { Reset(); return; }
    if (RegenDisplaySession != CombatSession || RegenDisplayEpoch != Probe.Event.WorldEpoch || RegenDisplayRegion != Probe.Event.RegionId)
    {
        Reset(); RegenDisplaySession = CombatSession; RegenDisplayEpoch = Probe.Event.WorldEpoch; RegenDisplayRegion = Probe.Event.RegionId;
    }
    if (FMath::IsFinite(Gained) && Gained > 0)
    {
        if (RegenDisplayStartedAt < 0) { RegenDisplayStartedAt = FMath::Max(0., Now - Elapsed); }
        RegenDisplayAmount += Gained;
    }
    double MergeSeconds = 1;
    if (const auto* State = GetWorld()->GetGameState<AY01GameState>(); State && State->GetPresentation().Revision > 0)
    { MergeSeconds = State->GetPresentation().Values.RegenMergeSeconds; }
    if (RegenDisplayStartedAt < 0 || Now - RegenDisplayStartedAt + 1.e-6 < MergeSeconds) { return; }
    const float Shown = static_cast<float>(FMath::FloorToDouble(RegenDisplayAmount + 1.e-4));
    if (Shown >= 1)
    {
        auto Pending = Service->Capture(this, this); Pending.Event.ActualDamage = 0; Pending.Event.DisplayAmount = Shown;
        Pending.Event.Type = EY01DamageEventType::Healing; Service->Commit(Pending);
        RegenDisplayAmount = FMath::Max(0., RegenDisplayAmount - Shown);
    }
    RegenDisplayStartedAt = RegenDisplayAmount > 1.e-6 ? Now : -1;
}
void UY01CombatAbilitySystem::UpdateHealthRegen(bool bEffective)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !GetWorld() || !bInitialized) { return; }
    const double Now = GetWorld()->GetTimeSeconds(), Elapsed = FMath::Max(0., Now - LastRegenTime);
    LastRegenTime = Now;
    const bool bApply = bEffective && bRegenEffective;
    bRegenEffective = bEffective;
    const float HP = GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    if (const auto* Mode = GetWorld()->GetAuthGameMode<AY01GameMode>(); Mode && Mode->IsGameplayFrozen())
    { bRegenEffective = false; UpdateRegenDisplay(0, Now, Elapsed, false); return; }
    if (const auto* P = Cast<AY01PlayerState>(GetOwner()); P && P->GetRevive()->BlocksActions())
    { bRegenEffective = false; UpdateRegenDisplay(0, Now, Elapsed, false); return; }
    const auto Stats = GetUnitStats();
    if (!bApply || HP <= 0 || Stats.HealthRegen <= 0)
    { RegenRemainder = 0; UpdateRegenDisplay(0, Now, Elapsed, false); return; }
    if (HP >= Stats.Health)
    { RegenRemainder = 0; UpdateRegenDisplay(0, Now, Elapsed, true); return; }
    RegenRemainder += Elapsed * Stats.HealthRegen;
    const float Next = static_cast<float>(FMath::Min(static_cast<double>(Stats.Health), HP + RegenRemainder));
    if (Next <= HP) { UpdateRegenDisplay(0, Now, Elapsed, true); return; }
    ApplyModToAttribute(UY01AttributeSet::GetHealthAttribute(), EGameplayModOp::Additive, Next - HP);
    const float After = GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    RegenRemainder = After >= Stats.Health ? 0 : RegenRemainder - (After - HP);
    UpdateRegenDisplay(After - HP, Now, Elapsed, true);
    UE_LOG(LogTemp, Display, TEXT("Y01R34 REGEN Source=%s DS=%.17g HealthRegen=%g BeforeHP=%g AfterHP=%g Actual=%g"),
        *GetNameSafe(GetOwner()), Now, Stats.HealthRegen, HP, After, After-HP);
}
bool UY01CombatAbilitySystem::SetCombatTarget(AActor* Target)
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) { return false; }
    // 无目标可以显式清除；无效的新目标也不会留下上一个目标供误触发。
    CombatTarget.Reset();
    if (!Target) { return true; }
    if (!IsValidCombatTarget(Target)) { return false; }
    CombatTarget = Target;
    return true;
}
bool UY01CombatAbilitySystem::SetDiscoveredTarget(AActor* Target)
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) { return false; }
    CombatTarget.Reset();
    if (!Target) { return true; }
    if (!IsLegalCombatTarget(Target)) { return false; }
    CombatTarget = Target; return true;
}
bool UY01CombatAbilitySystem::IsValidCombatTarget(const AActor* Target) const
{
    if (!IsLegalCombatTarget(Target)) { return false; }
    const double DistanceSquared = FVector::DistSquared(GetAvatarActor()->GetActorLocation(), Target->GetActorLocation());
    return FMath::IsFinite(DistanceSquared) && DistanceSquared <= FMath::Square(static_cast<double>(AttackRange));
}
bool UY01CombatAbilitySystem::IsLegalCombatTarget(const AActor* Target) const
{
    const AActor* Avatar = GetAvatarActor();
    if (const auto* Mode = GetWorld() ? GetWorld()->GetAuthGameMode<AY01GameMode>() : nullptr; Mode && Mode->IsGameplayFrozen()) { return false; }
    if (const auto* P = Cast<AY01PlayerState>(GetOwner()); P && P->GetRevive()->BlocksActions()) { return false; }
    if (!bInitialized || !IsValid(GetOwner()) || !GetOwner()->HasAuthority() || !IsValid(Avatar) || !Avatar->HasAuthority()
        || !IsValid(Target) || Target->IsActorBeingDestroyed() || Avatar->IsActorBeingDestroyed()
        || Target == Avatar || Target == GetOwner() || Target->GetWorld() != GetWorld() || !Target->HasAuthority()) { return false; }
    const auto* OtherASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Target);
    if (!OtherASC || OtherASC == this || OtherASC->GetAvatarActor() != Target || !OtherASC->GetSet<UY01AttributeSet>()) { return false; }
    if (const auto* Other = Cast<UY01CombatAbilitySystem>(OtherASC); Other && !IsCombatPairAllowed(Other)) { return false; }
    const float OwnHealth = GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    const float OtherHealth = OtherASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    const FVector A = Avatar->GetActorLocation(), B = Target->GetActorLocation();
    return FMath::IsFinite(OwnHealth) && OwnHealth > 0 && FMath::IsFinite(OtherHealth) && OtherHealth > 0
        && !A.ContainsNaN() && !B.ContainsNaN();
}
bool UY01CombatAbilitySystem::IsCombatPairAllowed(const UY01CombatAbilitySystem* Other) const
{
    return Other && (!TargetPermission || TargetPermission(this, Other))
        && (!Other->TargetPermission || Other->TargetPermission(this, Other));
}
void UY01CombatAbilitySystem::InvalidateCombatSession()
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) { return; }
    ++CombatSession; SetCombatTarget(nullptr); CancelAllAbilities(); CombatSessionInvalidated.Broadcast();
}
bool UY01CombatAbilitySystem::TryActivateBasicAttack()
{
    return GetOwner() && GetOwner()->HasAuthority() && bInitialized && IsValidCombatTarget(GetCombatTarget())
        && TryActivateAbility(BasicAttackHandle, false);
}
bool UY01CombatAbilitySystem::TryActivateSkillSlot(int32 Slot)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !bInitialized || bRequestInProgress || Slot < 0 || !GetWorld()) { return false; }
    if (const auto* Mode = GetWorld()->GetAuthGameMode<AY01GameMode>(); Mode && Mode->IsGameplayFrozen()) { return false; }
    if (const auto* P = Cast<AY01PlayerState>(GetOwner()); P && P->GetRevive()->BlocksActions()) { return false; }
    for (auto& Grant : GrantedSources)
    {
        if (Grant.Source.Type != EY01TableKind::Skill || Grant.Slot != Slot) { continue; }
        if (Grant.SkillType != EY01SkillType::Active || !Grant.Ability.IsValid() || !IsAbilityReady(Grant.Ability)) { return false; }
        TGuardValue<bool> Guard(bRequestInProgress, true);
        const uint64 Before = Grant.CommitSerial;
        if (!TryActivateAbility(Grant.Ability, false)) { return false; }
        // 成功提交才产生CD；被GA拒绝的激活不能伪造成功。
        return Grant.CommitSerial != Before;
    }
    return false;
}
UY01LoadoutComponent* UY01CombatAbilitySystem::ActiveLoadout() const
{
    auto* L = GetOwner() ? GetOwner()->FindComponentByClass<UY01LoadoutComponent>() : nullptr;
    return L && L->IsInitialized() ? L : nullptr;
}
const TArray<FY01GrantedSource>& UY01CombatAbilitySystem::GetGrantedSources() const
{
    static const TArray<FY01GrantedSource> Hidden;
    const auto* L = ActiveLoadout(); return L && L->IsBusy() ? Hidden : GrantedSources;
}
void UY01CombatAbilitySystem::NotifyAbilityActivated(FGameplayAbilitySpecHandle Handle, UGameplayAbility* Ability)
{
    if (auto* L = ActiveLoadout(); L && Cast<UY01LoadoutAbility>(Ability)) { L->BeginActivation(Handle); }
    // 锁覆盖真实GAS激活广播，并由固定的GA执行外层在返回时释放。
    Super::NotifyAbilityActivated(Handle, Ability);
}
void UY01CombatAbilitySystem::NotifyAbilityCommit(UGameplayAbility* Ability)
{
    if (Ability && GetOwner() && GetOwner()->HasAuthority() && bInitialized && GetWorld())
    {
        const auto Handle = Ability->GetCurrentAbilitySpecHandle();
        for (auto& Grant : GrantedSources)
        {
            if (Grant.Ability == Handle && Grant.Source.Type == EY01TableKind::Skill && Grant.SkillType == EY01SkillType::Active)
            {
                // 在GAS提交广播前记录，直接激活同一技能句柄也不能跳过表CD。
                Grant.NextActivationTime = GetWorld()->GetTimeSeconds() + static_cast<double>(Grant.CooldownSeconds);
                ++Grant.CommitSerial;
                Grant.CommittedAttackDamage = GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute());
                if (auto* L = ActiveLoadout(); L && Grant.LearnedId.IsValid() && L->Skills.IsValidIndex(Grant.Slot))
                {
                    auto& Learned = L->Skills[Grant.Slot];
                    if (Learned.LearnedId == Grant.LearnedId && Learned.GAHandle == Handle) { Learned.Deadline = Grant.NextActivationTime; }
                }
                break;
            }
        }
    }
    Super::NotifyAbilityCommit(Ability);
}
const FY01GrantedSource* UY01CombatAbilitySystem::FindGrantedAbility(FGameplayAbilitySpecHandle Handle) const
{
    if (const auto* L = ActiveLoadout(); L && L->IsBusy() && L->ActivatingHandle != Handle) { return nullptr; }
    return GrantedSources.FindByPredicate([Handle](const auto& Grant) { return Grant.Ability == Handle; });
}
bool UY01CombatAbilitySystem::IsAbilityReady(FGameplayAbilitySpecHandle Handle) const
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !bInitialized || !GetWorld()) { return false; }
    const auto* Sources = GetOwner()->FindComponentByClass<UY01SourceEffectsComponent>();
    if (Sources && Sources->bInitialized && (Sources->bInCall || !Sources->Check().IsOk())) { return false; }
    if (const auto* L = ActiveLoadout(); L && !L->CanActivate(Handle)) { return false; }
    if (Handle == BasicAttackHandle) { return true; }
    for (const auto& Grant : GrantedSources)
    {
        if (Grant.Ability == Handle)
        {
            return Grant.Source.Type == EY01TableKind::Skill && Grant.SkillType == EY01SkillType::Active
                && GetWorld()->GetTimeSeconds() >= Grant.NextActivationTime;
        }
    }
    return false;
}
float UY01CombatAbilitySystem::GetSkillCooldownRemaining(int32 Slot) const
{
    if (const auto* L = ActiveLoadout(); L && L->IsBusy()) { return 0; }
    if (!GetWorld()) { return 0; }
    for (const auto& Grant : GrantedSources)
    {
        if (Grant.Source.Type == EY01TableKind::Skill && Grant.Slot == Slot)
        { return static_cast<float>(FMath::Max(0.0, Grant.NextActivationTime - GetWorld()->GetTimeSeconds())); }
    }
    return 0;
}
