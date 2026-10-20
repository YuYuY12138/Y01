#include "Playground/Y01TestSkill.h"
#include "AbilitySystemGlobals.h"
#include "Combat/Y01DamagePipeline.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "Playground/Y01PlaygroundRuntime.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01GrowthComponent.h"
#include "Player/Y01ShieldComponent.h"
#include "Combat/Y01Monster.h"
#include "Combat/Y01Core.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Net/UnrealNetwork.h"
#include "DrawDebugHelpers.h"
#include "UObject/ConstructorHelpers.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
bool CanCast(const UY01CombatAbilitySystem* ASC)
{
    const auto* Player = ASC ? Cast<AY01PlayerState>(ASC->GetOwner()) : nullptr;
    const auto* Pawn = ASC ? Cast<APawn>(ASC->GetAvatarActor()) : nullptr;
    const auto* Mode = ASC && ASC->GetWorld() ? ASC->GetWorld()->GetAuthGameMode<AY01PlaygroundGameMode>() : nullptr;
    return Player && Player->HasAuthority() && !Player->IsActorBeingDestroyed() && ASC->IsCombatInitialized()
        && IsValid(Pawn) && !Pawn->IsActorBeingDestroyed() && Pawn->HasAuthority() && !Pawn->GetActorLocation().ContainsNaN()
        && Pawn->GetController() && Pawn->GetPlayerState() == Player && Y01Playground::Health(ASC) > 0
        && Mode && !Mode->IsGameplayFrozen() && Mode->IsMatchConfigured() && Mode->GetFlowState() && Mode->GetFlowState()->PlayerArray.Contains(Player)
        && Mode->GetFlowState()->GetWaveSnapshot().Phase != EY01MatchPhase::Ended
        && (Mode->IsCombatRunning() || Mode->GetTraining()->Observe(Player).bInside);
}
UY01CombatAbilitySystem* CombatOf(const AActor* Actor)
{ return Cast<UY01CombatAbilitySystem>(UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Actor)); }
bool CanAutoAttack(const UY01CombatAbilitySystem* ASC)
{
    const auto* Mode = ASC && ASC->GetWorld() ? ASC->GetWorld()->GetAuthGameMode<AY01PlaygroundGameMode>() : nullptr;
    if (!Mode || Mode->IsGameplayFrozen() || !Mode->GetProfile() || !Mode->IsMatchConfigured()
        || !Mode->GetFlowState() || Mode->GetFlowState()->GetWaveSnapshot().Phase == EY01MatchPhase::Ended
        || !ASC->IsCombatInitialized() || !IsValid(ASC->GetAvatarActor()) || !ASC->GetAvatarActor()->HasAuthority()
        || ASC->GetAvatarActor()->IsActorBeingDestroyed() || Y01Playground::Health(ASC) <= 0) { return false; }
    const auto* Player = Cast<AY01PlayerState>(ASC->GetOwner());
    const auto* Monster = Cast<AY01Monster>(ASC->GetAvatarActor());
    return Player ? Mode->GetProfile()->bR34AutoAttackEnabled && Mode->IsPlayerPresent(Player) && CanCast(ASC)
        : Monster && ASC == Monster->GetCombatASC() && ASC->GetOwner() == Monster
            && (Mode->GetProfile()->bR34AutoAttackEnabled || (Mode->IsCoreDefenseEnabled()
                && Mode->IsCombatRunning() && Mode->GetWaveMonsters().Contains(Monster)));
}
AY01Monster* Nearest(const UY01CombatAbilitySystem* ASC, const FY01TestSkillConfig& Config)
{
    AY01Monster* Found = nullptr; double Best = FMath::Square(static_cast<double>(Config.TargetRange));
    for (TActorIterator<AY01Monster> It(ASC->GetWorld()); It; ++It)
    {
        if (!ASC->IsLegalCombatTarget(*It)) { continue; }
        const double D = FVector::DistSquared(ASC->GetAvatarActor()->GetActorLocation(), It->GetActorLocation());
        if (D <= Best && (!Found || D < Best || It->GetUniqueID() < Found->GetUniqueID())) { Found = *It; Best = D; }
    }
    return Found;
}
}
UY01TestSkillAbility::UY01TestSkillAbility()
{
    InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
    NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
    NetSecurityPolicy = EGameplayAbilityNetSecurityPolicy::ServerOnly;
}
bool UY01TestSkillAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
    const FGameplayTagContainer* SourceTags, const FGameplayTagContainer* TargetTags, FGameplayTagContainer* OptionalRelevantTags) const
{
    const auto* ASC = ActorInfo ? Cast<UY01CombatAbilitySystem>(ActorInfo->AbilitySystemComponent.Get()) : nullptr;
    const auto* Grant = ASC ? ASC->FindGrantedAbility(Handle) : nullptr;
    if (!CanCast(ASC) || !Grant || Grant->Source.Type != EY01TableKind::Skill || !ASC->IsAbilityReady(Handle)
        || !Grant->TestSkill.IsValid() || Grant->TestSkill.Mode == EY01TestSkillMode::None) { return false; }
    const float Damage = ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()) * Grant->TestSkill.DamageCoefficient;
    return FMath::IsFinite(Damage) && Damage >= 0
        && (Grant->TestSkill.Mode != EY01TestSkillMode::Projectile || Nearest(ASC, Grant->TestSkill))
        && Super::CanActivateAbility(Handle, ActorInfo, SourceTags, TargetTags, OptionalRelevantTags);
}
void UY01TestSkillAbility::ExecuteSkill(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* ActorInfo,
    FGameplayAbilityActivationInfo ActivationInfo, const FGameplayEventData* TriggerEventData)
{
    auto* ASC = ActorInfo ? Cast<UY01CombatAbilitySystem>(ActorInfo->AbilitySystemComponent.Get()) : nullptr;
    const auto* Grant = ASC ? ASC->FindGrantedAbility(Handle) : nullptr;
    if (!CanActivateAbility(Handle, ActorInfo)) { EndAbility(Handle, ActorInfo, ActivationInfo, false, true); return; }
    FVector Direction = FVector::ZeroVector;
    if (Grant->TestSkill.Mode == EY01TestSkillMode::Projectile)
    {
        const auto* Target = Nearest(ASC, Grant->TestSkill);
        if (!Target) { EndAbility(Handle, ActorInfo, ActivationInfo, false, true); return; }
        Direction = (Target->GetActorLocation() - ASC->GetAvatarActor()->GetActorLocation()).GetSafeNormal();
        if (Direction.IsNearlyZero()) { Direction = ASC->GetAvatarActor()->GetActorForwardVector(); }
    }
    const uint64 Session = ASC->GetCombatSession();
    const uint64 Serial = Grant->CommitSerial;
    if (!CommitAbility(Handle, ActorInfo, ActivationInfo) || Session != ASC->GetCombatSession() || !CanCast(ASC))
    { EndAbility(Handle, ActorInfo, ActivationInfo, false, true); return; }
    // 提交广播可能重入；重新按句柄取授予，使用成本执行后、广播前记录的服务器快照。
    Grant = ASC->FindGrantedAbility(Handle);
    if (!Grant || Grant->CommitSerial != Serial + 1)
    { EndAbility(Handle, ActorInfo, ActivationInfo, false, true); return; }
    const float Damage = Grant->CommittedAttackDamage * Grant->TestSkill.DamageCoefficient;
    if (!FMath::IsFinite(Damage) || Damage < 0)
    { EndAbility(Handle, ActorInfo, ActivationInfo, false, true); return; }
    // 施放护盾钩子：提交成功后按技能来源Id通知护盾组件记一次施放（ShieldOnCast）。
    if (auto* PlayerState = Cast<AY01PlayerState>(ASC->GetOwner()))
    {
        // 已授予技能实例自带LearnedId；经Loadout解析为运行时来源Id。
        if (auto* Shield = PlayerState->GetShield(); Shield && Grant->LearnedId.IsValid())
        { Shield->NotifyCast(PlayerState->GetLoadout()->SourceId(Grant->LearnedId, EY01RuntimeSourceKind::Skill).Id); }
    }
    FActorSpawnParameters Params; Params.Owner = ASC->GetOwner(); Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* Attack = ASC->GetWorld()->SpawnActor<AY01TestAttack>(ASC->GetAvatarActor()->GetActorLocation(), FRotator::ZeroRotator, Params);
    if (Attack) { Attack->Initialize(ASC, *Grant, Direction, Damage); }
    EndAbility(Handle, ActorInfo, ActivationInfo, false, Attack == nullptr);
}
TFunction<void(UAbilitySystemComponent*, FGameplayAbilitySpecHandle)> UY01LearnProbeAbility::Given;
TFunction<void(UAbilitySystemComponent*, FGameplayAbilitySpecHandle)> UY01LearnProbeAbility::Removed;
void UY01LearnProbeAbility::OnGiveAbility(const FGameplayAbilityActorInfo* Info, const FGameplayAbilitySpec& Spec)
{
    Super::OnGiveAbility(Info, Spec);
    if (Given) { auto Callback = Given; Callback(Info->AbilitySystemComponent.Get(), Spec.Handle); }
}
void UY01LearnProbeAbility::OnRemoveAbility(const FGameplayAbilityActorInfo* Info, const FGameplayAbilitySpec& Spec)
{
    if (Removed) { auto Callback = Removed; Callback(Info->AbilitySystemComponent.Get(), Spec.Handle); }
    Super::OnRemoveAbility(Info, Spec);
}
UY01TestSkillDamageEffect::UY01TestSkillDamageEffect()
{
    auto* Tags = CreateDefaultSubobject<UAssetTagsGameplayEffectComponent>(TEXT("DamageAssetTags"));
    GEComponents.Add(Tags); FInheritedTagContainer Changes; Changes.AddTag(Y01Tags::Effect_Damage);
    Tags->SetAndApplyAssetTagChanges(Changes);
    DurationPolicy = EGameplayEffectDurationType::Instant;
    Executions.AddDefaulted_GetRef().CalculationClass = UY01TestSkillDamageExecution::StaticClass();
}
void UY01TestSkillDamageExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters,
    FGameplayEffectCustomExecutionOutput& Output) const
{
    const auto* Source = Cast<UY01CombatAbilitySystem>(Parameters.GetSourceAbilitySystemComponent());
    const auto* Target = Cast<UY01CombatAbilitySystem>(Parameters.GetTargetAbilitySystemComponent());
    const auto* Attack = Cast<AY01TestAttack>(Parameters.GetOwningSpec().GetContext().GetSourceObject());
    if (!Attack || !Attack->AuthorizesDamage(Source, Target)) { return; }
    // 可吸血随技能配置（普攻/主动测试技能默认可吸血）；暴击沿用发射时判定的状态。
    const bool bLifesteal = Attack->GetState().Config.bLifesteal;
    // 共享伤害管线：基础 → ×暴击倍率 → 减甲(下限0) → 护盾吸收(仅玩家) → 扣血(不超当前)。
    const auto Result = Y01DamagePipeline::Resolve(Source, Target, Attack->GetState().DamageSnapshot, Attack->GetState().bCritical);
    if (Result.bValid && Result.HealthLoss > 0)
    {
        if (auto* Spec = Parameters.GetOwningSpecForPreExecuteMod())
        {
            Spec->SetSetByCallerMagnitude(Y01Tags::Data_DamageDisplay, Result.AfterArmor);
            // 暴击标记与可吸血开关写入spec，供事件标记与吸血判定消费。
            Spec->SetSetByCallerMagnitude(Y01Tags::Data_Crit, Result.bCritical ? 1.f : 0.f);
            Spec->SetSetByCallerMagnitude(Y01Tags::Data_LifestealAllowed, bLifesteal ? 1.f : 0.f);
        }
        Output.AddOutputModifier(FGameplayModifierEvaluatedData(UY01AttributeSet::GetHealthAttribute(), EGameplayModOp::Additive, -Result.HealthLoss));
    }
}
AY01TestAttack::AY01TestAttack()
{
    bReplicates = true; bAlwaysRelevant = true; bOnlyRelevantToOwner = false;
    SetNetUpdateFrequency(60); SetMinNetUpdateFrequency(30); PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.TickGroup = TG_PostPhysics;
    Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision")); RootComponent = Collision;
    Collision->SetCollisionEnabled(ECollisionEnabled::NoCollision); Collision->SetGenerateOverlapEvents(false);
    Shape = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Shape")); Shape->SetupAttachment(RootComponent);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    Shape->SetStaticMesh(Sphere.Object); Shape->SetCollisionEnabled(ECollisionEnabled::NoCollision); Shape->SetVisibility(false);
}
AActor* AY01TestAttack::FindAutoTarget(const UY01CombatAbilitySystem* ASC)
{
    if (!CanAutoAttack(ASC)) { return nullptr; }
    if (const auto* Monster = Cast<AY01Monster>(ASC->GetAvatarActor()))
    {
        AActor* Target = ASC->GetCombatTarget();
        if (!ASC->IsAutoAttackInRange(Target)) { return nullptr; }
        const auto* Mode = ASC->GetWorld()->GetAuthGameMode<AY01PlaygroundGameMode>();
        const bool bCore = Mode->IsCoreDefenseEnabled() && Target == Mode->GetMatchCore()
            && Mode->GetWaveMonsters().Contains(Monster);
        return bCore || (Monster->GetDetectionRadius() > 0
            && FVector::DistSquared2D(Monster->GetActorLocation(), Target->GetActorLocation()) <= FMath::Square(static_cast<double>(Monster->GetDetectionRadius())))
            ? Target : nullptr;
    }
    AActor* Target = nullptr; double Best = TNumericLimits<double>::Max();
    for (TActorIterator<APawn> It(ASC->GetWorld()); It; ++It)
    {
        if (!ASC->IsAutoAttackInRange(*It)) { continue; }
        const double D = FVector::DistSquared2D(ASC->GetAvatarActor()->GetActorLocation(), It->GetActorLocation());
        if (!Target || D < Best || (D == Best && It->GetUniqueID() < Target->GetUniqueID())) { Target = *It; Best = D; }
    }
    return Target;
}
bool AY01TestAttack::TryAutoAttack(UY01CombatAbilitySystem* ASC)
{
    AActor* Target = FindAutoTarget(ASC);
    if (!Target) { return false; }
    return ASC->TryAutoAttack(Target, [&](float Damage)
    {
        FActorSpawnParameters Params; Params.Owner = ASC->GetOwner(); Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* Attack = ASC->GetWorld()->SpawnActor<AY01TestAttack>(ASC->GetAvatarActor()->GetActorLocation(), FRotator::ZeroRotator, Params);
        if (!Attack) { return false; }
        const auto Stats = ASC->GetUnitStats(); auto& S = Attack->State;
        S.bAutoAttack = true; S.Skill = ASC->GetDefinition(); S.SelectedTarget = Target; S.MeleeAngle = Stats.MeleeAngle;
        const bool bMelee = Stats.AttackType == EY01AttackType::Melee;
        S.Config.Mode = bMelee ? EY01TestSkillMode::Area : EY01TestSkillMode::Projectile;
        S.Config.Radius = bMelee ? Stats.Range : 12.f; S.Config.HalfHeight = 120;
        S.Config.ActiveSeconds = FMath::Min(.35f, Stats.AttackInterval);
        S.Config.TargetRange = Stats.Range; S.Config.Speed = Stats.ProjectileSpeed;
        S.Config.Lifetime = Stats.Range / Stats.ProjectileSpeed; S.Config.HitFeedbackSeconds = .1f;
        const FVector Delta = Target->GetActorLocation() - ASC->GetAvatarActor()->GetActorLocation();
        S.Direction = bMelee ? Delta.GetSafeNormal2D() : Delta.GetSafeNormal();
        if (S.Direction.IsNearlyZero()) { S.Direction = FVector::ForwardVector; }
        S.DamageSnapshot = Damage; Attack->ActivateState(ASC); return true;
    });
}
void AY01TestAttack::Initialize(UY01CombatAbilitySystem* ASC, const FY01GrantedSource& Grant, FVector Direction, float Damage)
{
    if (!HasAuthority() || !CanCast(ASC) || Grant.CommitSerial == 0) { Cancel(); return; }
    State.Config = Grant.TestSkill; State.Skill = Grant.Source;
    State.Direction = Direction; State.DamageSnapshot = Damage; ActivateState(ASC);
}
void AY01TestAttack::ActivateState(UY01CombatAbilitySystem* ASC)
{
    Source = ASC; SourceSession = ASC->GetCombatSession();
    // 暴击在发射/放置时判定一次（DamageSnapshot就绪后随状态激活），命中沿用。
    State.bCritical = Y01DamagePipeline::RollCritical(ASC);
    State.Caster = ASC->GetAvatarActor(); State.Origin = State.Caster->GetActorLocation(); State.Position = State.Origin;
    State.StartedAt = GetWorld()->GetTimeSeconds();
    const double Lifetime = State.bAutoAttack && State.Config.Mode == EY01TestSkillMode::Projectile
        ? static_cast<double>(State.Config.TargetRange) / State.Config.Speed : State.Config.Lifetime;
    State.EndsAt = State.StartedAt + (State.Config.Mode == EY01TestSkillMode::Area ? State.Config.ActiveSeconds : Lifetime);
    LastStepAt = State.StartedAt; bInitialized = true;
    Collision->SetSphereRadius(State.Config.Radius); SessionHandle = ASC->CombatSessionInvalidated.AddUObject(this, &ThisClass::Cancel);
    State.Caster->OnEndPlay.AddDynamic(this, &ThisClass::SourceEnded);
    if (ASC->GetOwner() != State.Caster) { ASC->GetOwner()->OnEndPlay.AddDynamic(this, &ThisClass::SourceEnded); }
    RememberTargets(); OnRep_State(); ForceNetUpdate();
    if (State.bAutoAttack && State.Config.Mode == EY01TestSkillMode::Projectile)
    {
        SelectedASC = CombatOf(State.SelectedTarget);
        TargetSessionHandle = SelectedASC->CombatSessionInvalidated.AddUObject(this, &ThisClass::TargetInvalidated);
        State.SelectedTarget->OnEndPlay.AddDynamic(this, &ThisClass::TargetEnded);
    }
    UE_LOG(LogTemp, Display, TEXT("Y01Skill COMMIT Actor=%s Caster=%s Row=%s Mode=%d Session=%llu Damage=%g Origin=%s Radius=%g Height=%g Speed=%g End=%.3f World=%.6f StartedAt=%.6f EndsAt=%.6f Source=%s"),
        *GetName(), *GetNameSafe(State.Caster), *State.Skill.RowName.ToString(), static_cast<int32>(State.Config.Mode), SourceSession,
        State.DamageSnapshot, *State.Origin.ToString(), State.Config.Radius, State.Config.HalfHeight, State.Config.Speed, State.EndsAt,
        GetWorld()->GetTimeSeconds(), State.StartedAt, State.EndsAt, *GetNameSafe(ASC->GetOwner()));
    if (State.bAutoAttack)
    {
        const auto* TargetASC = CombatOf(State.SelectedTarget);
        UE_LOG(LogTemp, Display, TEXT("Y01R34 ATTACK Actor=%s Source=%s Caster=%s Target=%s DS=%.17g Snapshot=%g SourceSession=%llu TargetSession=%llu Type=%d Origin=%s Direction=%s Range=%g Angle=%g Radius=%g Height=%g Speed=%g EndsAt=%.9f"),
            *GetName(), *GetNameSafe(ASC->GetOwner()), *GetNameSafe(State.Caster), *GetNameSafe(State.SelectedTarget), State.StartedAt, State.DamageSnapshot,
            SourceSession, TargetASC ? TargetASC->GetCombatSession() : 0, static_cast<int32>(State.Config.Mode), *State.Origin.ToString(), *State.Direction.ToString(),
            State.Config.TargetRange, State.MeleeAngle, State.Config.Radius, State.Config.HalfHeight, State.Config.Speed, State.EndsAt);
    }
    if (State.Config.Mode == EY01TestSkillMode::Area) { CheckArea(); }
    else { SweepBall(State.Position); }
    // 普攻在守卫窗口内真正发动点：只计数不结算（计数点=发动，不按命中人数），由成长Tick统一结算。
    if (State.bAutoAttack)
    {
        if (auto* P = Cast<AY01PlayerState>(ASC->GetOwner()); P && P->GetGrowth()) { P->GetGrowth()->NotifyBasicAttack(); }
    }
}
bool AY01TestAttack::SourceValid() const
{
    return bInitialized && HasAuthority() && !IsActorBeingDestroyed() && Source.IsValid()
        && (State.bAutoAttack ? CanAutoAttack(Source.Get()) : CanCast(Source.Get()))
        && Source->GetCombatSession() == SourceSession && Source->GetAvatarActor() == State.Caster;
}
void AY01TestAttack::RememberTargets()
{
    for (TActorIterator<APawn> It(GetWorld()); It; ++It)
    {
        auto* ASC = CombatOf(*It);
        if (ASC && !TargetSessions.Contains(ASC)) { TargetSessions.Add(ASC, ASC->GetCombatSession()); }
    }
}
bool AY01TestAttack::TargetValid(AActor* Target) const
{
    if (!SourceValid() || !IsValid(Target) || (!State.bAutoAttack && !Cast<AY01Monster>(Target))
        || !Source->IsLegalCombatTarget(Target)) { return false; }
    if (State.bAutoAttack && State.Config.Mode == EY01TestSkillMode::Projectile && Target != State.SelectedTarget) { return false; }
    auto* ASC = CombatOf(Target);
    const auto* Session = TargetSessions.Find(ASC);
    return ASC && Session && *Session == ASC->GetCombatSession();
}
bool AY01TestAttack::AuthorizesDamage(const UY01CombatAbilitySystem* InstigatorASC, const UY01CombatAbilitySystem* Target) const
{
    return SourceValid() && Source.Get() == InstigatorASC && Target && ApplyingTarget.Get() == Target
        && State.Phase == EY01TestAttackPhase::Active && TargetValid(Target->GetAvatarActor());
}
void AY01TestAttack::ApplyHit(AActor* Monster)
{
    if (!TargetValid(Monster) || State.Phase != EY01TestAttackPhase::Active || HitTargets.Contains(Monster)) { return; }
    // 伤害回调可重入销毁/清理；诊断身份及命中位置必须在回调前从真实对象留样。
    const FString SourceName = GetNameSafe(Source->GetOwner()), CasterName = GetNameSafe(State.Caster), TargetName = GetNameSafe(Monster);
    const FVector SourcePosition = State.Caster->GetActorLocation(), TargetPosition = Monster->GetActorLocation();
    auto* TargetASC = CombatOf(Monster);
    const float BeforeHP = Y01Playground::Health(TargetASC), Armor = TargetASC->GetNumericAttribute(UY01AttributeSet::GetArmorAttribute());
    const uint64 TargetSession = TargetASC->GetCombatSession();
    HitTargets.Add(Monster); ApplyingTarget = TargetASC;
    auto Context = Source->MakeEffectContext(); Context.AddSourceObject(this);
    auto Spec = Source->MakeOutgoingSpec(UY01TestSkillDamageEffect::StaticClass(), 1, Context);
    Source->ApplyGameplayEffectSpecToTarget(*Spec.Data, TargetASC); ApplyingTarget.Reset();
    if (State.bAutoAttack)
    {
        const float AfterHP = Y01Playground::Health(TargetASC);
        UE_LOG(LogTemp, Display, TEXT("Y01R34 HIT Actor=%s Source=%s Caster=%s Target=%s DS=%.17g Snapshot=%g Armor=%g BeforeHP=%g AfterHP=%g Actual=%g SourceSession=%llu TargetSession=%llu"),
            *GetName(), *SourceName, *CasterName, *TargetName, GetWorld()->GetTimeSeconds(), State.DamageSnapshot, Armor, BeforeHP, AfterHP, BeforeHP-AfterHP, SourceSession, TargetSession);
    }
    UE_LOG(LogTemp, Display, TEXT("Y01Skill HIT Actor=%s Target=%s Damage=%g HP=%g Position=%s World=%.6f Source=%s Caster=%s SourceX=%.6f SourceY=%.6f SourceZ=%.6f TargetX=%.6f TargetY=%.6f TargetZ=%.6f"),
        *GetName(), *TargetName, State.DamageSnapshot, Y01Playground::Health(TargetASC), *State.Position.ToString(),
        GetWorld()->GetTimeSeconds(), *SourceName, *CasterName, SourcePosition.X, SourcePosition.Y, SourcePosition.Z,
        TargetPosition.X, TargetPosition.Y, TargetPosition.Z);
}
void AY01TestAttack::CheckArea()
{
    TArray<TWeakObjectPtr<AActor>> Candidates;
    for (TActorIterator<APawn> It(GetWorld()); It; ++It) { Candidates.Add(*It); }
    for (const auto& Candidate : Candidates)
    {
        auto* Monster = Candidate.Get(); if (!TargetValid(Monster)) { continue; }
        const FVector Delta = Monster->GetActorLocation() - State.Origin;
        const bool bAngle = !State.bAutoAttack || Delta.SizeSquared2D() == 0 || State.MeleeAngle >= 360
            || FVector::DotProduct(Delta.GetSafeNormal2D(), State.Direction) + 1.e-8 >= FMath::Cos(FMath::DegreesToRadians(State.MeleeAngle * .5));
        if (bAngle && Delta.SizeSquared2D() <= FMath::Square(static_cast<double>(State.Config.Radius)) && FMath::Abs(Delta.Z) <= State.Config.HalfHeight)
        { ApplyHit(Monster); }
    }
}
void AY01TestAttack::SweepBall(FVector End)
{
    if (!SourceValid() || State.Phase != EY01TestAttackPhase::Active) { return; }
    FCollisionQueryParams Params(SCENE_QUERY_STAT(Y01TestBall), false, this);
    Params.AddIgnoredActor(State.Caster); Params.AddIgnoredActor(Source->GetOwner());
    // 非法玩家/怪物不遮挡球；墙仍然真实阻挡。不能先撞非法怪再直接穿过整段。
    for (TActorIterator<APawn> It(GetWorld()); It; ++It)
    { if (!TargetValid(*It)) { Params.AddIgnoredActor(*It); } }
    FCollisionObjectQueryParams Objects;
    Objects.AddObjectTypesToQuery(ECC_WorldStatic); Objects.AddObjectTypesToQuery(ECC_WorldDynamic); Objects.AddObjectTypesToQuery(ECC_Pawn);
    TArray<FHitResult> Hits;
    const FVector Start = State.Position;
    GetWorld()->SweepMultiByObjectType(Hits, Start, End, FQuat::Identity, Objects, FCollisionShape::MakeSphere(Collision->GetUnscaledSphereRadius()), Params);
    Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Time < B.Time; });
    for (const auto& Hit : Hits)
    {
        AActor* Monster = CombatOf(Hit.GetActor()) ? Hit.GetActor() : nullptr;
        if (Monster && !TargetValid(Monster)) { continue; }
        if (!Monster && (!Hit.GetComponent() || Hit.GetComponent()->GetCollisionResponseToChannel(ECC_WorldDynamic) != ECR_Block)) { continue; }
        State.Position = FMath::Lerp(Start, End, Hit.Time); SetActorLocation(State.Position);
        if (!Monster) { UE_LOG(LogTemp, Display, TEXT("Y01Skill WALL Actor=%s Wall=%s"), *GetName(), *GetNameSafe(Hit.GetActor())); CancelWithReason(TEXT("Wall")); return; }
        ApplyHit(Monster);
        if (!SourceValid()) { Cancel(); return; }
        State.Phase = EY01TestAttackPhase::HitFeedback; State.EndsAt = GetWorld()->GetTimeSeconds() + State.Config.HitFeedbackSeconds;
        OnRep_State(); ForceNetUpdate(); return;
    }
    State.Position = End; SetActorLocation(End);
}
void AY01TestAttack::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (HasAuthority())
    {
        if (!SourceValid()) { CancelWithReason(TEXT("SourceInvalid")); return; }
        const double Now = GetWorld()->GetTimeSeconds();
        // 只在原有自然截止分支记录；END前有EXPIRE即自然到期，其余销毁不冒充超时。
        auto LogExpiry = [&]
        {
            if (IsActorBeingDestroyed()) { return; }
            UE_LOG(LogTemp, Display, TEXT("Y01Skill EXPIRE Actor=%s World=%.6f StartedAt=%.6f EndsAt=%.6f Mode=%d Phase=%d"),
                *GetName(), Now, State.StartedAt, State.EndsAt, static_cast<int32>(State.Config.Mode), static_cast<int32>(State.Phase));
        };
        if (State.Phase == EY01TestAttackPhase::HitFeedback)
        { if (Now >= State.EndsAt) { LogExpiry(); CancelWithReason(TEXT("Expired")); return; } }
        else
        {
            RememberTargets();
            if (State.bAutoAttack && State.Config.Mode == EY01TestSkillMode::Projectile && !TargetValid(State.SelectedTarget)) { CancelWithReason(TEXT("TargetInvalid")); return; }
            if (State.Config.Mode == EY01TestSkillMode::Area)
            { if (Now >= State.EndsAt) { LogExpiry(); CancelWithReason(TEXT("Expired")); return; } CheckArea(); }
            else
            {
                const double StepTime = FMath::Min(Now, State.EndsAt);
                const double PreviousWorld = LastStepAt;
                const FVector PreviousPosition = State.Position;
                const FVector End = State.bAutoAttack
                    ? State.Origin + State.Direction * FMath::Clamp((StepTime - State.StartedAt) * State.Config.Speed, 0., static_cast<double>(State.Config.TargetRange))
                    : State.Position + State.Direction * State.Config.Speed * FMath::Max(0., StepTime - LastStepAt);
                SweepBall(End); LastStepAt = StepTime;
                if (FParse::Param(FCommandLine::Get(), TEXT("Y01SkillTrace")))
                {
                    UE_LOG(LogTemp, Display, TEXT("Y01Skill STEP Actor=%s World=%.6f PrevWorld=%.6f SampleWorld=%.6f PrevX=%.6f PrevY=%.6f PrevZ=%.6f Position=X=%.6f Y=%.6f Z=%.6f Mode=%d Phase=%d Terminal=%d"),
                        *GetName(), Now, PreviousWorld, StepTime, PreviousPosition.X, PreviousPosition.Y, PreviousPosition.Z,
                        State.Position.X, State.Position.Y, State.Position.Z, static_cast<int32>(State.Config.Mode), static_cast<int32>(State.Phase),
                        IsActorBeingDestroyed() || State.Phase != EY01TestAttackPhase::Active || Now >= State.EndsAt ? 1 : 0);
                }
                if (State.Phase == EY01TestAttackPhase::Active && Now >= State.EndsAt) { LogExpiry(); CancelWithReason(TEXT("Expired")); return; }
            }
        }
    }
    DrawState();
}
void AY01TestAttack::DrawState()
{
    if (IsActorBeingDestroyed() || GetNetMode() == NM_DedicatedServer) { return; }
    if (State.Config.Mode == EY01TestSkillMode::Projectile && State.Phase == EY01TestAttackPhase::HitFeedback)
    {
        // 单帧前景反馈，12为屏幕标记大小而非碰撞范围；严格使用复制球心。
        DrawDebugPoint(GetWorld(), State.Position, 12.f, FColor::Green, false, 0.f, SDPG_Foreground);
    }
    if (State.Config.Mode != EY01TestSkillMode::Area) { return; }
    if (State.bAutoAttack)
    {
        const double Half = State.MeleeAngle * .5;
        for (double Z : {-static_cast<double>(State.Config.HalfHeight), static_cast<double>(State.Config.HalfHeight)})
        {
            const FVector Center = State.Origin + FVector(0,0,Z);
            FVector Previous = Center + State.Direction.RotateAngleAxis(-Half, FVector::UpVector) * State.Config.Radius;
            DrawDebugLine(GetWorld(), Center, Previous, FColor::Red, false, 0, 0, 1.5f);
            for (int32 I = 1; I <= 64; ++I)
            {
                const FVector Next = Center + State.Direction.RotateAngleAxis(-Half + State.MeleeAngle * I / 64., FVector::UpVector) * State.Config.Radius;
                DrawDebugLine(GetWorld(), Previous, Next, FColor::Red, false, 0, 0, 1.5f); Previous = Next;
            }
            DrawDebugLine(GetWorld(), Center, Previous, FColor::Red, false, 0, 0, 1.5f);
        }
        return;
    }
    // 单帧线，不留持久调试线；圆心、半径、上下界与Actor中心判定共用复制配置。
    DrawDebugCylinder(GetWorld(), State.Origin - FVector(0,0,State.Config.HalfHeight), State.Origin + FVector(0,0,State.Config.HalfHeight),
        State.Config.Radius, 64, FColor::Red, false, 0, 0, 1.5f);
}
void AY01TestAttack::OnRep_State()
{
    SetActorLocation(State.Position); Collision->SetSphereRadius(State.Config.Radius);
    Shape->SetVisibility(State.Config.Mode == EY01TestSkillMode::Projectile);
    Shape->SetRelativeScale3D(FVector(State.Config.Radius / 50.f));
    Y01Playground::ColorShape(Shape, State.Phase == EY01TestAttackPhase::HitFeedback ? FLinearColor::Green : FLinearColor::Red);
    UE_LOG(LogTemp, Display, TEXT("Y01Skill STATE NetMode=%d Actor=%s Caster=%s Row=%s Mode=%d Phase=%d Position=%s Radius=%g End=%.3f"),
        static_cast<int32>(GetNetMode()), *GetName(), *GetNameSafe(State.Caster), *State.Skill.RowName.ToString(),
        static_cast<int32>(State.Config.Mode), static_cast<int32>(State.Phase), *State.Position.ToString(), State.Config.Radius, State.EndsAt);
}
void AY01TestAttack::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{ Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(AY01TestAttack, State); }
void AY01TestAttack::Cancel() { CancelWithReason(TEXT("SourceSession")); }
void AY01TestAttack::CancelWithReason(const TCHAR* Reason)
{
    if (!HasAuthority() || IsActorBeingDestroyed()) { return; }
    if (State.bAutoAttack)
    {
        UE_LOG(LogTemp, Display, TEXT("Y01R34 CANCEL Actor=%s Source=%s Target=%s DS=%.17g Reason=%s SourceSession=%llu Phase=%d Position=%s"),
            *GetName(), *GetNameSafe(GetOwner()), *GetNameSafe(State.SelectedTarget), GetWorld()->GetTimeSeconds(), Reason, SourceSession, static_cast<int32>(State.Phase), *State.Position.ToString());
    }
    Destroy();
}
void AY01TestAttack::SourceEnded(AActor*, EEndPlayReason::Type) { CancelWithReason(TEXT("SourceEnded")); }
void AY01TestAttack::TargetInvalidated()
{ if (State.Phase == EY01TestAttackPhase::Active && !ApplyingTarget.IsValid()) { CancelWithReason(TEXT("TargetSessionOrEnded")); } }
void AY01TestAttack::TargetEnded(AActor*, EEndPlayReason::Type) { TargetInvalidated(); }
void AY01TestAttack::EndPlay(const EEndPlayReason::Type Reason)
{
    if (Source.IsValid())
    {
        Source->CombatSessionInvalidated.Remove(SessionHandle);
        if (Source->GetOwner()) { Source->GetOwner()->OnEndPlay.RemoveDynamic(this, &ThisClass::SourceEnded); }
    }
    if (IsValid(State.Caster)) { State.Caster->OnEndPlay.RemoveDynamic(this, &ThisClass::SourceEnded); }
    if (SelectedASC.IsValid()) { SelectedASC->CombatSessionInvalidated.Remove(TargetSessionHandle); }
    if (IsValid(State.SelectedTarget)) { State.SelectedTarget->OnEndPlay.RemoveDynamic(this, &ThisClass::TargetEnded); }
    Shape->SetVisibility(false);
    UE_LOG(LogTemp, Display, TEXT("Y01Skill END NetMode=%d Actor=%s Phase=%d Reason=%d World=%.6f StartedAt=%.6f EndsAt=%.6f Mode=%d"),
        static_cast<int32>(GetNetMode()), *GetName(), static_cast<int32>(State.Phase), static_cast<int32>(Reason),
        GetWorld()->GetTimeSeconds(), State.StartedAt, State.EndsAt, static_cast<int32>(State.Config.Mode));
    Super::EndPlay(Reason);
}
