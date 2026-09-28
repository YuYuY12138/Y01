#include "Combat/Y01Combat.h"
#include "AbilitySystemGlobals.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

namespace Y01Tags { UE_DEFINE_GAMEPLAY_TAG(Data_Damage, "Data.Damage"); }

void UY01AttributeSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
    Super::GetLifetimeReplicatedProps(OutLifetimeProps);
    DOREPLIFETIME_CONDITION_NOTIFY(UY01AttributeSet, Health, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UY01AttributeSet, MaxHealth, COND_None, REPNOTIFY_Always);
    DOREPLIFETIME_CONDITION_NOTIFY(UY01AttributeSet, AttackDamage, COND_None, REPNOTIFY_Always);
}
void UY01AttributeSet::ClampAttribute(const FGameplayAttribute& Attribute, float& Value) const
{
    if (!FMath::IsFinite(Value)) { Value = 0; }
    if (Attribute == GetHealthAttribute()) { Value = FMath::Clamp(Value, 0.f, FMath::Max(0.f, GetMaxHealth())); }
    else if (Attribute == GetMaxHealthAttribute() || Attribute == GetAttackDamageAttribute()) { Value = FMath::Max(0.f, Value); }
}
void UY01AttributeSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) { ClampAttribute(Attribute, NewValue); }
void UY01AttributeSet::PreAttributeBaseChange(const FGameplayAttribute& Attribute, float& NewValue) const { ClampAttribute(Attribute, NewValue); }
bool UY01AttributeSet::PreGameplayEffectExecute(FGameplayEffectModCallbackData& Data)
{
    return GetOwningActor() && GetOwningActor()->HasAuthority() && FMath::IsFinite(Data.EvaluatedData.Magnitude);
}
void UY01AttributeSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
    const FGameplayAttribute& Attribute = Data.EvaluatedData.Attribute;
    float Value = GetOwningAbilitySystemComponent()->GetNumericAttributeBase(Attribute);
    float Clamped = Value;
    ClampAttribute(Attribute, Clamped);
    if (Clamped != Value) { GetOwningAbilitySystemComponent()->SetNumericAttributeBase(Attribute, Clamped); }
}
void UY01AttributeSet::PostAttributeChange(const FGameplayAttribute& Attribute, float OldValue, float NewValue)
{
    if (Attribute == GetMaxHealthAttribute() && GetHealth() > NewValue && GetOwningActor()->HasAuthority())
    { GetOwningAbilitySystemComponent()->SetNumericAttributeBase(GetHealthAttribute(), NewValue); }
}
void UY01AttributeSet::OnRep_Health(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UY01AttributeSet, Health, OldValue); }
void UY01AttributeSet::OnRep_MaxHealth(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UY01AttributeSet, MaxHealth, OldValue); }
void UY01AttributeSet::OnRep_AttackDamage(const FGameplayAttributeData& OldValue) { GAMEPLAYATTRIBUTE_REPNOTIFY(UY01AttributeSet, AttackDamage, OldValue); }
void UY01DamageExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters, FGameplayEffectCustomExecutionOutput& Output) const
{
    const auto* Source = Cast<UY01CombatAbilitySystem>(Parameters.GetSourceAbilitySystemComponent());
    const auto* Target = Parameters.GetTargetAbilitySystemComponent();
    if (!Source || !Target || !Target->GetOwner() || !Target->GetOwner()->HasAuthority()
        || !Source->IsValidCombatTarget(Target->GetAvatarActor())) { return; }
    const float Damage = Parameters.GetOwningSpec().GetSetByCallerMagnitude(Y01Tags::Data_Damage, false, -1.f);
    if (!FMath::IsFinite(Damage) || Damage < 0) { return; }
    const float Health = Target->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    if (Damage > 0 && FMath::IsFinite(Health) && Health > 0)
    {
        Output.AddOutputModifier(FGameplayModifierEvaluatedData(UY01AttributeSet::GetHealthAttribute(),
            EGameplayModOp::Additive, -FMath::Min(Damage, Health)));
    }
}
UY01DamageEffect::UY01DamageEffect()
{
    DurationPolicy = EGameplayEffectDurationType::Instant;
    Executions.AddDefaulted_GetRef().CalculationClass = UY01DamageExecution::StaticClass();
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
void UY01BasicAttackAbility::ActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
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

bool UY01CombatAbilitySystem::TryInitializeFromData(const FY01DataCatalog& Catalog, const FY01DataRef& Definition, FString& OutError)
{
    OutError.Reset();
    auto Reject = [&](const FString& Reason) { OutError = Reason; return false; };
    if (!GetOwner() || !GetOwner()->HasAuthority()) { return Reject(TEXT("初始化必须由服务器执行")); }
    if (bInitialized)
    {
        return DefinitionRef.Type == Definition.Type && DefinitionRef.RowName == Definition.RowName
            ? true : Reject(TEXT("实体已经初始化，不能替换定义"));
    }
    if (bInitializing) { return Reject(TEXT("初始化正在执行")); }
    TGuardValue<bool> Guard(bInitializing, true);
    float Health = 0, Damage = 0, Range = 0, Speed = 0;
    TArray<FY01DataRef> Equipment, Skills;
    FY01DataValidation Validation;
    if (Definition.Type == EY01TableKind::Character)
    {
        const auto* Row = Catalog.FindRow<FY01CharacterRow>(Definition, Validation);
        if (!Row) { return Reject(TEXT("角色定义引用无效")); }
        Health = Row->Health; Damage = Row->AttackDamage; Range = Row->Range; Speed = Row->MoveSpeed;
        Equipment = Row->InitialEquipment; Skills = Row->InitialSkills;
        if (Equipment.Num() > Y01Data::EquipmentSlotCount || Skills.Num() > Y01Data::SkillSlotCount)
        { return Reject(TEXT("角色初始装配超过六装备或四技能槽")); }
    }
    else if (Definition.Type == EY01TableKind::Monster)
    {
        const auto* Row = Catalog.FindRow<FY01MonsterRow>(Definition, Validation);
        if (!Row) { return Reject(TEXT("怪物定义引用无效")); }
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
    for (int32 I = 0; I < Equipment.Num(); ++I)
    {
        const auto* Row = Catalog.FindRow<FY01EquipmentRow>(Equipment[I], Validation);
        if (!Row) { return Reject(TEXT("初始装备引用无效")); }
        FPendingGrant& Grant = Pending.AddDefaulted_GetRef();
        Grant.Record.Source = Equipment[I]; Grant.Record.Slot = I;
        if (!LoadEffect(Row->Effect, Grant)) { return Reject(TEXT("初始装备GE类不可用")); }
    }
    for (int32 I = 0; I < Skills.Num(); ++I)
    {
        const auto* Row = Catalog.FindRow<FY01SkillRow>(Skills[I], Validation);
        if (!Row) { return Reject(TEXT("初始技能引用无效")); }
        if ((Row->Type != EY01SkillType::Active && Row->Type != EY01SkillType::Passive)
            || !FMath::IsFinite(Row->CooldownSeconds) || Row->CooldownSeconds < 0)
        { return Reject(TEXT("技能类型或CD非法")); }
        FPendingGrant& Grant = Pending.AddDefaulted_GetRef();
        Grant.Record.Source = Skills[I]; Grant.Record.Slot = I;
        Grant.Record.SkillType = Row->Type; Grant.Record.CooldownSeconds = Row->CooldownSeconds;
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
    SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(), Health);
    SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), Health);
    SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), Damage);
    auto Rollback = [&]()
    {
        for (auto& Grant : Pending)
        {
            if (Grant.Record.Ability.IsValid()) { ClearAbility(Grant.Record.Ability); }
            if (Grant.Record.Effect.IsValid()) { RemoveActiveGameplayEffect(Grant.Record.Effect); }
        }
        SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(), OldMax);
        SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), OldHealth);
        SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), OldDamage);
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
    BasicAttackHandle = Pending[0].Record.Ability;
    for (auto& Grant : Pending) { GrantedSources.Add(MoveTemp(Grant.Record)); }
    InitialEquipment = MoveTemp(Equipment); InitialSkills = MoveTemp(Skills);
    DefinitionRef = Definition; AttackRange = Range; InitialMoveSpeed = Speed; bInitialized = true;
    OnRep_InitialMoveSpeed();
    GetOwner()->ForceNetUpdate();
    return true;
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
bool UY01CombatAbilitySystem::IsValidCombatTarget(const AActor* Target) const
{
    const AActor* Avatar = GetAvatarActor();
    if (!bInitialized || !IsValid(GetOwner()) || !GetOwner()->HasAuthority() || !IsValid(Avatar) || !Avatar->HasAuthority()
        || !IsValid(Target) || Target->IsActorBeingDestroyed() || Avatar->IsActorBeingDestroyed()
        || Target == Avatar || Target == GetOwner() || Target->GetWorld() != GetWorld() || !Target->HasAuthority()) { return false; }
    const auto* OtherASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Target);
    if (!OtherASC || OtherASC == this || OtherASC->GetAvatarActor() != Target || !OtherASC->GetSet<UY01AttributeSet>()) { return false; }
    const float OwnHealth = GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    const float OtherHealth = OtherASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    const FVector A = Avatar->GetActorLocation(), B = Target->GetActorLocation();
    const double DistanceSquared = FVector::DistSquared(A, B);
    return FMath::IsFinite(OwnHealth) && OwnHealth > 0 && FMath::IsFinite(OtherHealth) && OtherHealth > 0
        && !A.ContainsNaN() && !B.ContainsNaN() && FMath::IsFinite(DistanceSquared)
        && DistanceSquared <= FMath::Square(static_cast<double>(AttackRange));
}
bool UY01CombatAbilitySystem::TryActivateBasicAttack()
{
    return GetOwner() && GetOwner()->HasAuthority() && bInitialized && IsValidCombatTarget(GetCombatTarget())
        && TryActivateAbility(BasicAttackHandle, false);
}
bool UY01CombatAbilitySystem::TryActivateSkillSlot(int32 Slot)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !bInitialized || bRequestInProgress || Slot < 0 || !GetWorld()) { return false; }
    for (auto& Grant : GrantedSources)
    {
        if (Grant.Source.Type != EY01TableKind::Skill || Grant.Slot != Slot) { continue; }
        if (Grant.SkillType != EY01SkillType::Active || !Grant.Ability.IsValid() || !IsAbilityReady(Grant.Ability)) { return false; }
        TGuardValue<bool> Guard(bRequestInProgress, true);
        if (!TryActivateAbility(Grant.Ability, false)) { return false; }
        // 首轮仅提供服务器槽位CD入口；完整技能自行实现GAS成本、取消和表现。
        Grant.NextActivationTime = GetWorld()->GetTimeSeconds() + static_cast<double>(Grant.CooldownSeconds);
        return true;
    }
    return false;
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
                break;
            }
        }
    }
    Super::NotifyAbilityCommit(Ability);
}
bool UY01CombatAbilitySystem::IsAbilityReady(FGameplayAbilitySpecHandle Handle) const
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !bInitialized || !GetWorld()) { return false; }
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
    if (!GetWorld()) { return 0; }
    for (const auto& Grant : GrantedSources)
    {
        if (Grant.Source.Type == EY01TableKind::Skill && Grant.Slot == Slot)
        { return static_cast<float>(FMath::Max(0.0, Grant.NextActivationTime - GetWorld()->GetTimeSeconds())); }
    }
    return 0;
}
