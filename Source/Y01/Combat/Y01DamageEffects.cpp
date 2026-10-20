#include "Combat/Y01DamageEffects.h"
#include "Combat/Y01Targeting.h"
#include "Combat/Y01DamagePipeline.h"
#include "Player/Y01DamageEffectsComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01ShieldComponent.h"
#include "Player/Y01AttributeResolver.h"
#include "GameplayEffectComponents/AssetTagsGameplayEffectComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "DrawDebugHelpers.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

bool FY01DamageEffectRow::IsValid() const
{
    if (uint8(Trigger)>uint8(EY01DamageTrigger::Bound) || uint8(Targeting)>uint8(EY01DamageTargeting::RemoteCircle)
        || uint8(Order)>uint8(EY01DamageOrder::Random) || uint8(Center)>uint8(EY01DamageCenter::RandomGround)
        || uint8(Product)>uint8(EY01DamageProduct::BoundArea) || uint8(Position)>uint8(EY01DamagePosition::FollowPlayer)
        || uint8(Stacking)>uint8(EY01DamageStacking::Independent)) { return false; }
    for (double V : {SearchRadius, Radius, FixedDamage, PeriodSeconds, Lifetime, TickSeconds, Speed})
    { if (!FMath::IsFinite(V) || V < 0 || V > MAX_flt) { return false; } }
    if (TargetCount < 1 || TargetCount > 256 || MaxStacks < 1 || MaxStacks > 256 || Terms.Num() > 4) { return false; }
    TSet<EY01Attribute> Seen;
    for (const auto& T : Terms)
    {
        if ((!Y01Data::IsPrimaryAttribute(T.Attribute) && T.Attribute != EY01Attribute::AttackDamage)
            || Seen.Contains(T.Attribute) || !FMath::IsFinite(T.Coefficient) || T.Coefficient < 0 || T.Coefficient > MAX_flt) { return false; }
        Seen.Add(T.Attribute);
    }
    const bool Area = Product == EY01DamageProduct::IndependentArea || Product == EY01DamageProduct::BoundArea;
    if ((Trigger == EY01DamageTrigger::Bound) != (Product == EY01DamageProduct::BoundArea)) { return false; }
    if (Trigger == EY01DamageTrigger::Periodic && PeriodSeconds < .001) { return false; }
    if (Product == EY01DamageProduct::BoundArea && (Lifetime != 0 || Targeting != EY01DamageTargeting::SelfCircle)) { return false; }
    if (Product != EY01DamageProduct::Instant && Product != EY01DamageProduct::BoundArea && Lifetime < .001) { return false; }
    if ((Area || Product == EY01DamageProduct::AttachedDot) && TickSeconds < .001) { return false; }
    if (Product == EY01DamageProduct::Projectile && Speed <= 0) { return false; }
    if (Targeting == EY01DamageTargeting::SelfCircle ? Radius <= 0 : SearchRadius <= 0) { return false; }
    if (Targeting == EY01DamageTargeting::RemoteCircle && Radius <= 0) { return false; }
    if (Area && Targeting == EY01DamageTargeting::NearbyCount) { return false; }
    if (!Area && Position != EY01DamagePosition::Fixed) { return false; }
    if (Stacking == EY01DamageStacking::Refresh && MaxStacks != 1) { return false; }
    if (Product != EY01DamageProduct::AttachedDot && (Stacking != EY01DamageStacking::Refresh || MaxStacks != 1)) { return false; }
    return true;
}
float Y01Damage::Evaluate(const FY01DamageEffectRow& Row, const FY01UnitCombatStats& Stats)
{
    double Value = Row.FixedDamage;
    for (const auto& Term : Row.Terms)
    {
        double Attribute = 0;
        switch (Term.Attribute)
        {
        case EY01Attribute::AttackDamage: Attribute = Stats.AttackDamage; break;
        case EY01Attribute::Strength: Attribute = Stats.Strength; break;
        case EY01Attribute::Agility: Attribute = Stats.Agility; break;
        case EY01Attribute::Wisdom: Attribute = Stats.Wisdom; break;
        default: return -1;
        }
        if (!FMath::IsFinite(Attribute) || Attribute < 0 || !FMath::IsFinite(Term.Coefficient) || Term.Coefficient < 0) { return -1; }
        Value += Attribute * Term.Coefficient;
    }
    return FMath::IsFinite(Value) && Value >= 0 && Value <= MAX_flt ? static_cast<float>(Value) : -1.f;
}
bool Y01Damage::Select(const UY01CombatAbilitySystem* Source, const FY01DamageEffectRow& Row,
    TConstArrayView<AActor*> Candidates, FRandomStream& Random, FY01DamageSelection& Out)
{
    Out = {};
    if (!Source || !Source->GetAvatarActor() || !Row.IsValid()) { return false; }
    const FVector Origin = Source->GetAvatarActor()->GetActorLocation();
    TArray<AActor*> Nearby; TSet<AActor*> Seen;
    const double Search = Row.Targeting == EY01DamageTargeting::SelfCircle ? Row.Radius : Row.SearchRadius;
    for (auto* Actor : Candidates)
    {
        if (!Seen.Contains(Actor) && Y01Targeting::InRadiusXY(Source, Actor, Search))
        { Seen.Add(Actor); Nearby.Add(Actor); }
    }
    if (Nearby.IsEmpty()) { return false; }
    Nearby.Sort([&](const AActor& A, const AActor& B)
    {
        const double DA = FVector::DistSquared2D(Origin,A.GetActorLocation()), DB = FVector::DistSquared2D(Origin,B.GetActorLocation());
        return DA != DB ? DA < DB : A.GetPathName().Compare(B.GetPathName(),ESearchCase::CaseSensitive) < 0;
    });
    Out.Center = Origin;
    if (Row.Targeting == EY01DamageTargeting::NearbyCount)
    {
        if (Row.Order == EY01DamageOrder::Random)
        { for (int32 I=0; I<Nearby.Num()-1; ++I) { Nearby.Swap(I,Random.RandRange(I,Nearby.Num()-1)); } }
        for (int32 I=0; I<FMath::Min(Row.TargetCount,Nearby.Num()); ++I) { Out.Targets.Add(Nearby[I]); }
        return true;
    }
    if (Row.Targeting == EY01DamageTargeting::RemoteCircle)
    {
        if (Row.Center == EY01DamageCenter::RandomGround)
        {
            const double Angle = Random.FRand()*2*PI, Distance = FMath::Sqrt(Random.FRand())*Row.SearchRadius;
            Out.Center += FVector(FMath::Cos(Angle)*Distance,FMath::Sin(Angle)*Distance,0);
        }
        else { Out.Center = Nearby[Row.Center == EY01DamageCenter::NearestEnemy ? 0 : Random.RandRange(0,Nearby.Num()-1)]->GetActorLocation(); }
    }
    Seen.Reset();
    for (auto* Actor : Candidates)
    {
        if (Seen.Contains(Actor) || !Source->IsLegalCombatTarget(Actor)) { continue; }
        Seen.Add(Actor);
        if (FVector::DistSquared2D(Out.Center,Actor->GetActorLocation()) <= FMath::Square(Row.Radius)) { Out.Targets.Add(Actor); }
    }
    Out.Targets.Sort([](const auto& A,const auto& B) { return A->GetPathName().Compare(B->GetPathName(),ESearchCase::CaseSensitive)<0; });
    return true;
}
bool Y01Damage::ValidateSource(const TArray<FY01ItemDamageEffect>& Effects, EY01TableKind Kind, EY01SkillType SkillType)
{
    if (!Effects.IsEmpty() && Kind == EY01TableKind::Skill && uint8(SkillType) > uint8(EY01SkillType::Passive)) { return false; }
    TSet<FName> Seen;
    for (const auto& E : Effects)
    {
        if (E.EffectId.IsNone() || Seen.Contains(E.EffectId) || !E.Config.IsValid()
            || (Kind != EY01TableKind::Equipment && Kind != EY01TableKind::Skill)) { return false; }
        Seen.Add(E.EffectId);
        if (E.Config.Trigger == EY01DamageTrigger::Active && (Kind != EY01TableKind::Skill || SkillType != EY01SkillType::Active)) { return false; }
        if (Kind == EY01TableKind::Skill && SkillType == EY01SkillType::Active && E.Config.Trigger != EY01DamageTrigger::Active) { return false; }
    }
    return true;
}
FString Y01Damage::Describe(const TArray<FY01ItemDamageEffect>& Effects)
{
    FString Text;
    const TCHAR* Products[] = {TEXT("即时伤害"),TEXT("飞行攻击"),TEXT("挂身持续"),TEXT("独立区域"),TEXT("绑定火圈")};
    const TCHAR* Attributes[] = {TEXT(""),TEXT("力量"),TEXT("敏捷"),TEXT("智力"),TEXT("攻击")};
    for (const auto& E : Effects)
    {
        const auto& R = E.Config; if (!R.IsValid()) { continue; }
        Text += FString::Printf(TEXT("%s：%s"),Products[uint8(R.Product)],*FY01AttributeResolver::WholeText(R.FixedDamage));
        for (const auto& T : R.Terms) { Text += FString::Printf(TEXT(" + %s×%g%%"),Attributes[uint8(T.Attribute)],T.Coefficient*100); }
        if (R.Trigger == EY01DamageTrigger::Periodic) { Text += FString::Printf(TEXT("；每%g秒"),R.PeriodSeconds); }
        if (R.Product == EY01DamageProduct::AttachedDot || R.Product == EY01DamageProduct::IndependentArea || R.Product == EY01DamageProduct::BoundArea)
        { Text += FString::Printf(TEXT("；每跳%g秒"),R.TickSeconds); }
        Text += TEXT("\n");
    }
    return Text;
}
UY01DamageAbility::UY01DamageAbility()
{
    InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;
    NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;
    NetSecurityPolicy = EGameplayAbilityNetSecurityPolicy::ServerOnly;
}
bool UY01DamageAbility::SupportsLoadoutSkill(const FY01TestSkillConfig& Config) const
{ return Config.IsValid() && Config.Mode == EY01TestSkillMode::None; }
bool UY01DamageAbility::CanActivateAbility(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
    const FGameplayTagContainer* S, const FGameplayTagContainer* T, FGameplayTagContainer* Tags) const
{
    auto* P = Info ? Cast<AY01PlayerState>(Info->OwnerActor.Get()) : nullptr;
    return P && P->GetCombatASC()->IsAbilityReady(Handle) && P->GetDamageEffects()->CanActivate(Handle)
        && Super::CanActivateAbility(Handle,Info,S,T,Tags);
}
void UY01DamageAbility::ExecuteSkill(FGameplayAbilitySpecHandle Handle, const FGameplayAbilityActorInfo* Info,
    FGameplayAbilityActivationInfo Activation, const FGameplayEventData*)
{
    auto* P = Info ? Cast<AY01PlayerState>(Info->OwnerActor.Get()) : nullptr;
    if (!P) { EndAbility(Handle,Info,Activation,false,true); return; }
    auto* D = P->GetDamageEffects(); TArray<UY01DamageEffectsComponent::FActiveCast> Casts;
    const uint64 Epoch = D->Epoch;
    if (!D->PrepareActive(Handle,Casts) || !CommitAbility(Handle,Info,Activation))
    { EndAbility(Handle,Info,Activation,false,true); return; }
    // 施放护盾钩子：提交成功后按技能来源Id通知护盾组件记一次施放（ShieldOnCast）。
    if (auto* Shield = P->GetShield())
    {
        // 已授予技能实例自带LearnedId；经Loadout解析为运行时来源Id。
        if (const auto* Grant = P->GetCombatASC()->FindGrantedAbility(Handle); Grant && Grant->LearnedId.IsValid())
        { Shield->NotifyCast(P->GetLoadout()->SourceId(Grant->LearnedId,EY01RuntimeSourceKind::Skill).Id); }
    }
    D->ExecuteActive(Casts,Epoch,Handle);
    EndAbility(Handle,Info,Activation,false,false);
}
UY01TemplateDamageEffect::UY01TemplateDamageEffect()
{
    auto* Tags = CreateDefaultSubobject<UAssetTagsGameplayEffectComponent>(TEXT("DamageAssetTags"));
    GEComponents.Add(Tags); FInheritedTagContainer Changes; Changes.AddTag(Y01Tags::Effect_Damage); Tags->SetAndApplyAssetTagChanges(Changes);
    DurationPolicy = EGameplayEffectDurationType::Instant;
    Executions.AddDefaulted_GetRef().CalculationClass = UY01TemplateDamageExecution::StaticClass();
}
void UY01TemplateDamageExecution::Execute_Implementation(const FGameplayEffectCustomExecutionParameters& Parameters,
    FGameplayEffectCustomExecutionOutput& Output) const
{
    const auto* Source = Cast<UY01CombatAbilitySystem>(Parameters.GetSourceAbilitySystemComponent());
    const auto* Target = Cast<UY01CombatAbilitySystem>(Parameters.GetTargetAbilitySystemComponent());
    auto* Manager = const_cast<UY01DamageEffectsComponent*>(Cast<UY01DamageEffectsComponent>(Parameters.GetOwningSpec().GetContext().GetSourceObject()));
    float Base = 0; bool bCritical = false; bool bLifesteal = false;
    if (!Manager || !Manager->ConsumeAuthorization(Parameters.GetOwningSpec(),Source,Target,Base,bCritical,bLifesteal)) { return; }
    // 共享伤害管线：基础 → ×暴击倍率 → 减甲(下限0) → 护盾吸收(仅玩家) → 扣血(不超当前)。
    const auto Result = Y01DamagePipeline::Resolve(Source,Target,Base,bCritical);
    if (Result.bValid && Result.HealthLoss > 0)
    {
        if (auto* Spec = Parameters.GetOwningSpecForPreExecuteMod())
        {
            Spec->SetSetByCallerMagnitude(Y01Tags::Data_DamageDisplay,Result.AfterArmor);
            // 暴击标记与可吸血开关（随来源效果配置）写入spec，供事件标记与吸血判定消费。
            Spec->SetSetByCallerMagnitude(Y01Tags::Data_Crit,Result.bCritical ? 1.f : 0.f);
            Spec->SetSetByCallerMagnitude(Y01Tags::Data_LifestealAllowed,bLifesteal ? 1.f : 0.f);
        }
        Output.AddOutputModifier(FGameplayModifierEvaluatedData(UY01AttributeSet::GetHealthAttribute(),EGameplayModOp::Additive,-Result.HealthLoss));
    }
}
AY01DamageVisual::AY01DamageVisual()
{
    bReplicates = true; SetReplicateMovement(true); SetNetUpdateFrequency(30);
    PrimaryActorTick.bCanEverTick = true;
    RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Shape = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Shape")); Shape->SetupAttachment(RootComponent);
    Rings = CreateDefaultSubobject<UInstancedStaticMeshComponent>(TEXT("Rings")); Rings->SetupAttachment(RootComponent);
    Rings->SetCollisionEnabled(ECollisionEnabled::NoCollision); Rings->SetGenerateOverlapEvents(false); Rings->SetCastShadow(false);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
    Rings->SetStaticMesh(Cube.Object);
    Shape->SetCollisionEnabled(ECollisionEnabled::NoCollision); Shape->SetGenerateOverlapEvents(false); Shape->SetCastShadow(false);
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
    Shape->SetStaticMesh(Sphere.Object);
}
void AY01DamageVisual::Configure(EY01DamageProduct InProduct, double InRadius)
{ Product=InProduct; Radius=static_cast<float>(InRadius); OnRep_Visual(); ForceNetUpdate(); }
void AY01DamageVisual::OnRep_Visual()
{
    const bool Flash = Product==EY01DamageProduct::Instant;
    const bool Area = Flash || Product==EY01DamageProduct::IndependentArea || Product==EY01DamageProduct::BoundArea;
    Shape->SetVisibility(!Area);
    Shape->SetRelativeScale3D(FVector(Product==EY01DamageProduct::AttachedDot ? .24 : .4));
    Shape->SetRelativeLocation(Product==EY01DamageProduct::AttachedDot ? FVector(0,0,55) : FVector::ZeroVector);
    Rings->ClearInstances();
    if (GetNetMode() == NM_DedicatedServer) { return; }
    const FLinearColor Color = Flash ? FLinearColor(1,.8f,.08f)
        : Product==EY01DamageProduct::BoundArea ? FLinearColor(1,.22f,.015f) : FLinearColor(.05f,1,.12f);
    if (auto* Material = Shape->CreateAndSetMaterialInstanceDynamic(0)) { Material->SetVectorParameterValue(TEXT("Color"), Color); }
    if (!Area) { return; }
    if (auto* Material = Rings->CreateAndSetMaterialInstanceDynamic(0)) { Material->SetVectorParameterValue(TEXT("Color"), Color); }
    const int32 Count = Product==EY01DamageProduct::IndependentArea ? 2 : 1;
    for (int32 Ring = 0; Ring < Count; ++Ring)
    {
        const double R = Radius * (Ring ? .65 : 1.);
        for (int32 I = 0; I < 48; ++I)
        {
            const double A = I * 2. * PI / 48., B = (I+1) * 2. * PI / 48.;
            const FVector Start(R*FMath::Cos(A),R*FMath::Sin(A),-75), End(R*FMath::Cos(B),R*FMath::Sin(B),-75);
            Rings->AddInstance(FTransform((End-Start).Rotation(), (Start+End)*.5, FVector((End-Start).Size()/100.,.045,.025)));
        }
    }
}
void AY01DamageVisual::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (GetNetMode()==NM_DedicatedServer) { return; }
    const FVector P=GetActorLocation();
    if (Product==EY01DamageProduct::Instant)
    { DrawDebugCircle(GetWorld(),P+FVector(0,0,-75),Radius,48,FColor(255,204,20),false,-1,0,6,FVector::ForwardVector,FVector::RightVector,false); }
    else if (Product==EY01DamageProduct::IndependentArea || Product==EY01DamageProduct::BoundArea)
    {
        const FColor Color=Product==EY01DamageProduct::BoundArea ? FColor::Orange : FColor::Green;
        DrawDebugCircle(GetWorld(),P+FVector(0,0,-75),Radius,48,Color,false,-1,0,5,FVector::ForwardVector,FVector::RightVector,false);
        if (Product==EY01DamageProduct::IndependentArea)
        { DrawDebugCircle(GetWorld(),P+FVector(0,0,-73),Radius*.65f,32,Color,false,-1,0,2,FVector::ForwardVector,FVector::RightVector,false); }
    }
    else if (Product==EY01DamageProduct::AttachedDot)
    { DrawDebugSphere(GetWorld(),P+FVector(0,0,55),20,12,FColor::Green,false,-1,0,3); }
}
void AY01DamageVisual::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{ Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(AY01DamageVisual,Product); DOREPLIFETIME(AY01DamageVisual,Radius); }
