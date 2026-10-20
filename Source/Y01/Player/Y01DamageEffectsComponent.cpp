#include "Player/Y01DamageEffectsComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01LoadoutComponent.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Player/Y01GrowthComponent.h"
#include "Player/Y01ReviveComponent.h"
#include "Combat/Y01Monster.h"
#include "Combat/Y01DamagePipeline.h"
#include "Match/Y01GameFlow.h"
#include "AbilitySystemGlobals.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Controller.h"
#include "Misc/ScopeExit.h"

UY01DamageEffectsComponent::UY01DamageEffectsComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
    PrimaryComponentTick.TickGroup = TG_PostPhysics;
    PrimaryComponentTick.TickInterval = .05f;
}
AY01PlayerState* UY01DamageEffectsComponent::Player() const { return Cast<AY01PlayerState>(GetOwner()); }
double UY01DamageEffectsComponent::Now() const { return FMath::Max(LastTime,GetWorld() ? double(GetWorld()->GetTimeSeconds()) : 0.); }
bool UY01DamageEffectsComponent::Eligible() const
{
    const auto* P=Player(); const auto* A=P ? P->GetCombatASC() : nullptr;
    const auto* L=P ? P->GetLoadout() : nullptr;
    const auto* State=GetWorld() ? GetWorld()->GetGameState<AY01GameState>() : nullptr;
    const auto* Pawn=A ? Cast<APawn>(A->GetAvatarActor()) : nullptr;
    const auto* Mode=GetWorld() ? GetWorld()->GetAuthGameMode<AY01GameMode>() : nullptr;
    return IsInGameThread() && !bEnded && IsValid(P) && P->HasAuthority() && !P->IsActorBeingDestroyed()
        && !P->GetRevive()->BlocksActions() && (!Mode || !Mode->IsGameplayFrozen())
        && L && L->bInitialized && !L->bEnded && State && State->PlayerArray.Contains(P)
        && (State->GetWaveSnapshot().Phase==EY01MatchPhase::Preparing || State->GetWaveSnapshot().Phase==EY01MatchPhase::Battle)
        && !P->GetSourceEffects()->bEnded && IsValid(Pawn) && !Pawn->IsActorBeingDestroyed()
        && Pawn->GetPlayerState()==P && Pawn->GetController() && Pawn->GetController()->GetPlayerState<AY01PlayerState>()==P
        && A->IsCombatInitialized() && A->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute())>0;
}
bool UY01DamageEffectsComponent::CanRun(FGameplayAbilitySpecHandle Active) const
{
    if (!Eligible()) { return false; }
    const auto* S=Player()->GetSourceEffects(); const auto* L=Player()->GetLoadout();
    if (S->bBusy || S->bInCall || !S->Check(bUpdating).IsOk()) { return false; }
    return !L->IsBusy() || (Active.IsValid() && L->ActivatingHandle==Active) || (bUpdating && !ActiveHandle.IsValid());
}
TArray<AActor*> UY01DamageEffectsComponent::Candidates() const
{
    TArray<AActor*> Result;
    for (TActorIterator<AY01Monster> It(GetWorld()); It; ++It)
    { if (Player()->GetCombatASC()->IsLegalCombatTarget(*It)) { Result.Add(*It); } }
    return Result;
}
void UY01DamageEffectsComponent::ResetCombat()
{
    ++Epoch; Sources.Reset(); bAuthorizationAvailable=false;
    auto Old=MoveTemp(Products); Products.Reset();
    for (const auto& Pair : Old) { if (Pair.Value.Visual.IsValid()) { Pair.Value.Visual->Destroy(); } }
}
void UY01DamageEffectsComponent::SynchronizeCommitted()
{
    auto* P=Player(); if (!P || !P->HasAuthority() || bSynchronizing) { return; }
    TGuardValue<bool> Sync(bSynchronizing, true); const uint64 StartEpoch = Epoch;
    if (!bBoundSession)
    {
        P->GetCombatASC()->CombatSessionInvalidated.AddUObject(this,&UY01DamageEffectsComponent::ResetCombat);
        Random.Initialize(static_cast<int32>(GetTypeHash(FGuid::NewGuid()))); bBoundSession=true;
    }
    if (!Eligible()) { ResetCombat(); return; }
    const auto* L=P->GetLoadout(); const auto* S=P->GetSourceEffects();
    const double Time=Now(); TSet<FString> Current;
    struct FCommitted { FString Id; FGuid Learned; TArray<FY01ItemDamageEffect> Effects; };
    TArray<FCommitted> Committed;
    for (const auto& Id : L->EquipmentSlots)
    { if (const auto* Item=L->Items.Find(Id)) { Committed.Add({L->SourceId(Id).Id, {}, Item->DamageEffects}); } }
    for (const auto& D : L->Devoured)
    { Committed.Add({L->SourceId(D.ItemId,EY01RuntimeSourceKind::Devoured).Id, {}, D.DamageEffects}); }
    for (const auto& Skill : L->Skills)
    { if (Skill.LearnedId.IsValid()) { Committed.Add({L->SourceId(Skill.LearnedId,EY01RuntimeSourceKind::Skill).Id, Skill.LearnedId, Skill.DamageEffects}); } }
    auto Add=[&](const FString& Source, const FGuid& Learned, const TArray<FY01ItemDamageEffect>& Effects)
    {
        // 即使空属性，也要求真实提交来源记录存在；从不读取Pending。
        if (!S->Records.ContainsByPredicate([&](const auto& R){return R.Source.Id==Source;})) { return; }
        for (const auto& E : Effects)
        {
            if (Epoch != StartEpoch || !Eligible()) { return; }
            const FString Key=Source+TEXT("/")+E.EffectId.ToString(); Current.Add(Key);
            if (Sources.Contains(Key)) { continue; }
            FSource New; New.Id=Key; New.LearnedId=Learned; New.Effect=E; New.Generation=++Generation; New.Next=Time+E.Config.PeriodSeconds;
            Sources.Add(Key,New);
            if (E.Config.Trigger==EY01DamageTrigger::Bound)
            {
                FProduct Product; Product.Id=++Serial; Product.SourceId=Key; Product.Generation=New.Generation; Product.Effect=E;
                // 重进房/重同步视为重新放置：绑定区域在创建时判一次暴击。
                Product.bCritical=Y01DamagePipeline::RollCritical(P->GetCombatASC());
                Product.Position=P->GetCombatASC()->GetAvatarActor()->GetActorLocation(); Product.Expires=TNumericLimits<double>::Max(); Product.LastMove=Time;
                if (!MakeVisual(Product)) { return; }
                const uint64 Id = Product.Id; Products.Add(Id,MoveTemp(Product));
            }
        }
    };
    for (const auto& C : Committed)
    { if (Epoch != StartEpoch || !Eligible()) { return; } Add(C.Id, C.Learned, C.Effects); }
    if (Epoch != StartEpoch || !Eligible()) { return; }
    for (auto It=Sources.CreateIterator(); It; ++It) { if (!Current.Contains(It.Key())) { It.RemoveCurrent(); } }
    TArray<uint64> Removed;
    for (const auto& Pair : Products)
    { if (Pair.Value.Effect.Config.Product==EY01DamageProduct::BoundArea && !Current.Contains(Pair.Value.SourceId)) { Removed.Add(Pair.Key); } }
    for (uint64 Id : Removed) { RemoveProduct(Id); }
}
void UY01DamageEffectsComponent::TickComponent(float Delta, ELevelTick Type, FActorComponentTickFunction* Tick)
{ Super::TickComponent(Delta,Type,Tick); if (GetOwner()->HasAuthority()) { Update(Now()); } }
void UY01DamageEffectsComponent::Update(double Time)
{
    if (bUpdating || bSynchronizing || !FMath::IsFinite(Time) || Time<LastTime || !Player() || !Player()->HasAuthority()) { return; }
    LastTime=Time;
    if (!Eligible()) { ResetCombat(); return; }
    if (!CanRun()) { return; }
    auto* L=Player()->GetLoadout();
    if (!L->BeginDamageExecution()) { return; }
    ON_SCOPE_EXIT { L->FinishDamageExecution(); };
    TGuardValue<bool> Guard(bUpdating,true); const uint64 StartEpoch=Epoch;
    Executed.Reset(); SynchronizeCommitted();
    // 先处理已有产物：到期先于刷新与同刻跳伤；不补偿服务器卡帧的欠跳。
    TArray<uint64> Ids; Products.GenerateKeyArray(Ids); Ids.Sort();
    for (uint64 Id : Ids)
    { if (Epoch!=StartEpoch || !CanRun()) { return; } Advance(Id,Time); }
    TArray<FString> Keys; Sources.GenerateKeyArray(Keys); Keys.Sort();
    for (const auto& Key : Keys)
    {
        if (Epoch!=StartEpoch || !CanRun()) { return; }
        const auto* Live=Sources.Find(Key);
        if (!Live || Live->Effect.Config.Trigger!=EY01DamageTrigger::Periodic || Time<Live->Next) { continue; }
        const FSource Source=*Live; FY01DamageSelection Selected;
        const float Snapshot=Y01Damage::Evaluate(Source.Effect.Config,Player()->GetCombatASC()->GetUnitStats());
        if (Snapshot<0 || !Y01Damage::Select(Player()->GetCombatASC(),Source.Effect.Config,Candidates(),Random,Selected)) { continue; }
        // 先登记发动，再允许伤害委托重入；落空同样已发动。
        Sources.FindChecked(Key).Next=Time+Source.Effect.Config.PeriodSeconds;
        // 周期效果每次发射视为一次放置：现掷一次暴击，直击与产物沿用。
        Launch(Source,Selected,Snapshot,Time,Y01DamagePipeline::RollCritical(Player()->GetCombatASC()));
    }
}
bool UY01DamageEffectsComponent::CanActivate(FGameplayAbilitySpecHandle Handle) const
{
    if (bUpdating || !CanRun(Handle)) { return false; }
    const auto* Grant=Player()->GetCombatASC()->FindGrantedAbility(Handle);
    if (!Grant || !Grant->LearnedId.IsValid()) { return false; }
    auto ProbeRandom=Random; const auto Actors=Candidates();
    for (const auto& Pair : Sources)
    {
        const auto& S=Pair.Value; FY01DamageSelection Selected;
        if (S.LearnedId==Grant->LearnedId && S.Effect.Config.Trigger==EY01DamageTrigger::Active
            && Y01Damage::Evaluate(S.Effect.Config,Player()->GetCombatASC()->GetUnitStats())>=0
            && Y01Damage::Select(Player()->GetCombatASC(),S.Effect.Config,Actors,ProbeRandom,Selected)) { return true; }
    }
    return false;
}
bool UY01DamageEffectsComponent::PrepareActive(FGameplayAbilitySpecHandle Handle,TArray<FActiveCast>& Out)
{
    Out.Reset(); if (!CanActivate(Handle)) { return false; }
    const FGuid Learned=Player()->GetCombatASC()->FindGrantedAbility(Handle)->LearnedId;
    TArray<FString> Keys; Sources.GenerateKeyArray(Keys); Keys.Sort(); const auto Actors=Candidates();
    for (const auto& Key : Keys)
    {
        const auto& S=Sources.FindChecked(Key); if (S.LearnedId!=Learned || S.Effect.Config.Trigger!=EY01DamageTrigger::Active) { continue; }
        FActiveCast C; C.Source=S; C.Snapshot=Y01Damage::Evaluate(S.Effect.Config,Player()->GetCombatASC()->GetUnitStats());
        if (C.Snapshot>=0 && Y01Damage::Select(Player()->GetCombatASC(),S.Effect.Config,Actors,Random,C.Selection))
        {
            // 主动施放在快照时刻判定一次暴击，随施放数据流传入Launch；被丢弃的施放不消耗掷点。
            C.bCritical=Y01DamagePipeline::RollCritical(Player()->GetCombatASC());
            Out.Add(MoveTemp(C));
        }
    }
    return !Out.IsEmpty();
}
void UY01DamageEffectsComponent::ExecuteActive(const TArray<FActiveCast>& Casts,uint64 StartEpoch,FGameplayAbilitySpecHandle Handle)
{
    if (bUpdating || Epoch!=StartEpoch || !CanRun(Handle)) { return; }
    TGuardValue<bool> Guard(bUpdating,true); TGuardValue<FGameplayAbilitySpecHandle> Active(ActiveHandle,Handle);
    Executed.Reset();
    for (const auto& C : Casts)
    {
        if (Epoch!=StartEpoch || !CanRun(Handle)) { return; }
        const auto* Current=Sources.Find(C.Source.Id);
        if (!Current || Current->Generation!=C.Source.Generation) { continue; }
        Launch(C.Source,C.Selection,C.Snapshot,Now(),C.bCritical);
    }
}
bool UY01DamageEffectsComponent::MakeVisual(FProduct& P)
{
    const uint64 StartEpoch = Epoch;
    FActorSpawnParameters Params; Params.Owner=GetOwner(); Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    auto* V=GetWorld()->SpawnActor<AY01DamageVisual>(P.Position,FRotator::ZeroRotator,Params);
    if (Epoch != StartEpoch || !Eligible()) { if (IsValid(V)) { V->Destroy(); } return false; }
    if (IsValid(V)) { V->Configure(P.Effect.Config.Product,P.Effect.Config.Radius); P.Visual=V; }
    return true;
}
void UY01DamageEffectsComponent::RemoveProduct(uint64 Id)
{
    FProduct Old; if (Products.RemoveAndCopyValue(Id,Old) && Old.Visual.IsValid()) { Old.Visual->Destroy(); }
}
void UY01DamageEffectsComponent::Launch(const FSource& Source,const FY01DamageSelection& Selected,float Snapshot,double Time,bool bCritical)
{
    const uint64 StartEpoch=Epoch; const auto& R=Source.Effect.Config;
    UE_LOG(LogTemp,Display,TEXT("Y01DamageNext LAUNCH Source=%s Generation=%llu Time=%.6f Product=%d Base=%g Crit=%d Targets=%d"),
        *Source.Id,Source.Generation,Time,int32(R.Product),Snapshot,bCritical?1:0,Selected.Targets.Num());
    if (R.Product==EY01DamageProduct::IndependentArea)
    {
        FProduct P; P.Id=++Serial; P.SourceId=Source.Id; P.Generation=Source.Generation; P.Effect=Source.Effect; P.bCritical=bCritical;
        P.Position=R.Position==EY01DamagePosition::FollowPlayer ? Player()->GetCombatASC()->GetAvatarActor()->GetActorLocation() : Selected.Center;
        P.Expires=Time+R.Lifetime; P.LastMove=Time;
        if (!MakeVisual(P)) { return; }
        const uint64 Id=P.Id; Products.Add(Id,MoveTemp(P)); Advance(Id,Time); return;
    }
    const uint64 Execution=++Serial;
    for (const auto& Target : Selected.Targets)
    {
        if (StartEpoch!=Epoch || !CanRun(ActiveHandle)) { return; }
        auto* ASC=Player()->GetCombatASC(); if (!ASC->IsLegalCombatTarget(Target.Get())) { continue; }
        // 直击沿用发射时判定的暴击；可吸血随来源效果配置。
        if (R.Product==EY01DamageProduct::Instant) { Hit(Execution,0,Target.Get(),Snapshot,bCritical,R.bLifesteal); continue; }
        if (R.Product==EY01DamageProduct::AttachedDot)
        {
            int32 Count=0; uint64 Shortest=0; double Deadline=TNumericLimits<double>::Max();
            for (const auto& Pair : Products)
            {
                const auto& P=Pair.Value;
                if (P.SourceId!=Source.Id || P.Target!=Target || P.Expires<=Time) { continue; }
                ++Count;
                if (P.Expires<Deadline || (P.Expires==Deadline && (!Shortest || P.Id<Shortest))) { Deadline=P.Expires; Shortest=P.Id; }
            }
            if (Count>=R.MaxStacks)
            { Products.FindChecked(Shortest).Expires=Time+R.Lifetime; continue; }
        }
        FProduct P; P.Id=++Serial; P.SourceId=Source.Id; P.Generation=Source.Generation; P.Effect=Source.Effect; P.Target=Target; P.bCritical=bCritical;
        auto* TargetASC=Cast<UY01CombatAbilitySystem>(UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Target.Get()));
        P.TargetSession=TargetASC->GetCombatSession(); P.Snapshot=Snapshot; P.Expires=Time+R.Lifetime; P.LastMove=Time;
        P.Position=R.Product==EY01DamageProduct::Projectile ? ASC->GetAvatarActor()->GetActorLocation() : Target->GetActorLocation();
        if (!MakeVisual(P)) { return; }
        const uint64 Id=P.Id; Products.Add(Id,MoveTemp(P));
        if (R.Product==EY01DamageProduct::AttachedDot) { Advance(Id,Time); }
    }
    // 主动区域即时技能在结算完成后补一个只读范围圈；不进产物账本，不影响命中、施放或冷却。
    if (R.Product==EY01DamageProduct::Instant && R.Trigger==EY01DamageTrigger::Active
        && R.Targeting!=EY01DamageTargeting::NearbyCount && StartEpoch==Epoch && CanRun(ActiveHandle))
    {
        FActorSpawnParameters Params; Params.Owner=GetOwner(); Params.SpawnCollisionHandlingOverride=ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        auto* V=GetWorld()->SpawnActor<AY01DamageVisual>(Selected.Center,FRotator::ZeroRotator,Params);
        if (IsValid(V)) { V->Configure(EY01DamageProduct::Instant,R.Radius); V->SetLifeSpan(.5f); }
    }
}
void UY01DamageEffectsComponent::Advance(uint64 Id,double Time)
{
    auto* Live=Products.Find(Id); if (!Live) { return; }
    const FProduct Copy=*Live; const auto& R=Copy.Effect.Config;
    if (Time>=Copy.Expires) { RemoveProduct(Id); return; }
    auto* ASC=Player()->GetCombatASC(); const uint64 StartEpoch=Epoch;
    const bool Attached=R.Product==EY01DamageProduct::AttachedDot || R.Product==EY01DamageProduct::Projectile;
    if (Attached)
    {
        const auto* TargetASC=Cast<UY01CombatAbilitySystem>(UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Copy.Target.Get()));
        if (!ASC->IsLegalCombatTarget(Copy.Target.Get()) || !TargetASC || TargetASC->GetCombatSession()!=Copy.TargetSession)
        { RemoveProduct(Id); return; }
    }
    if (R.Product==EY01DamageProduct::Projectile)
    {
        const FVector TargetPosition=Copy.Target->GetActorLocation(); const double Distance=FVector::Distance(Copy.Position,TargetPosition);
        const double Step=FMath::Max(0.,Time-Copy.LastMove)*R.Speed;
        if (Distance<=Step)
        {
            // 在应用GE之前退休产物，重复调度或目标死亡回调不能再次命中。
            RemoveProduct(Id);
            // 飞行物命中沿用发射时产物携带的暴击；可吸血随来源效果配置。
            if (Epoch == StartEpoch) { Hit(Id,1,Copy.Target.Get(),Copy.Snapshot,Copy.bCritical,Copy.Effect.Config.bLifesteal); }
            return;
        }
        Live->Position=Copy.Position+(TargetPosition-Copy.Position).GetSafeNormal()*Step; Live->LastMove=Time;
        if (Live->Visual.IsValid()) { Live->Visual->SetActorLocation(Live->Position); } return;
    }
    if (Attached) { Live->Position=Copy.Target->GetActorLocation(); }
    else if (R.Position==EY01DamagePosition::FollowPlayer) { Live->Position=ASC->GetAvatarActor()->GetActorLocation(); }
    if (Live->Visual.IsValid()) { Live->Visual->SetActorLocation(Live->Position); }
    const FVector Center=Live->Position;
    TArray<AActor*> Targets=Attached ? TArray<AActor*>{Copy.Target.Get()} : Candidates();
    // 离开区域仍保留原节拍，越过的时点只前进而不补跳；首次进入建立独立目标账本。
    for (auto& Pair : Live->Beats)
    {
        if (Attached || (Pair.Key.IsValid() && FVector::DistSquared2D(Center,Pair.Key->GetActorLocation())<=FMath::Square(R.Radius))) { continue; }
        Pair.Value.bInside = false;
        if (Time>=Pair.Value.Next) { Pair.Value.Next+=(FMath::FloorToDouble((Time-Pair.Value.Next)/R.TickSeconds)+1)*R.TickSeconds; }
    }
    for (auto* Target : Targets)
    {
        if (Epoch!=StartEpoch || !CanRun(ActiveHandle)) { return; }
        Live=Products.Find(Id); if (!Live) { return; }
        if (!ASC->IsLegalCombatTarget(Target) || (!Attached && FVector::DistSquared2D(Center,Target->GetActorLocation())>FMath::Square(R.Radius))) { continue; }
        auto* Beat=Live->Beats.Find(Target);
        if (!Beat) { FBeat New; New.Next=Time; Beat=&Live->Beats.Add(Target,New); }
        if (!Beat->bInside && Time > Beat->Next)
        { Beat->Next += (FMath::FloorToDouble((Time-Beat->Next)/R.TickSeconds)+1)*R.TickSeconds; }
        Beat->bInside = true;
        if (Time<Beat->Next) { continue; }
        // 身份、下一跳先于GE；容器引用绝不越过伤害回调。
        Beat->Next+=(FMath::FloorToDouble((Time-Beat->Next)/R.TickSeconds)+1)*R.TickSeconds;
        const uint64 BeatSerial=++Beat->Serial;
        const float Damage=Y01Damage::Evaluate(R,ASC->GetUnitStats());
        // 每跳沿用放置时判定的产物暴击；可吸血随来源效果配置。
        if (Damage>=0) { Hit(Id,BeatSerial,Target,Damage,Copy.bCritical,Copy.Effect.Config.bLifesteal); }
    }
}
void UY01DamageEffectsComponent::Hit(uint64 Execution,uint64 Beat,AActor* Target,float Damage,bool bCritical,bool bLifesteal)
{
    if (!bUpdating || !CanRun(ActiveHandle) || !FMath::IsFinite(Damage) || Damage<0) { return; }
    auto* ASC=Player()->GetCombatASC();
    if (!ASC->IsLegalCombatTarget(Target)) { return; }
    auto* TargetASC=Cast<UY01CombatAbilitySystem>(UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(Target));
    auto& Targets = Executed.FindOrAdd(Execution);
    if (const uint64* Previous = Targets.Find(Target); Previous && *Previous >= Beat) { return; }
    Targets.Add(Target, Beat);
    auto Context=ASC->MakeEffectContext(); Context.AddSourceObject(this);
    auto Spec=ASC->MakeOutgoingSpec(UY01TemplateDamageEffect::StaticClass(),1,Context); if (!Spec.IsValid()) { return; }
    // 暴击/可吸血开关与伤害同一授权生命周期：此处记录，仅本刻执行器经ConsumeAuthorization消费。
    AuthorizedContext=Context.Get(); AuthorizedTarget=TargetASC; AuthorizedDamage=Damage;
    AuthorizedCritical=bCritical; AuthorizedLifesteal=bLifesteal; bAuthorizationAvailable=true;
    ON_SCOPE_EXIT { AuthorizedContext=nullptr; AuthorizedTarget.Reset(); AuthorizedDamage=0; AuthorizedCritical=false; AuthorizedLifesteal=false; bAuthorizationAvailable=false; };
    UE_LOG(LogTemp,Display,TEXT("Y01DamageNext HIT Execution=%llu Beat=%llu Target=%s Base=%g Crit=%d Steal=%d"),Execution,Beat,*Target->GetName(),Damage,bCritical?1:0,bLifesteal?1:0);
    ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data,TargetASC);
}
bool UY01DamageEffectsComponent::ConsumeAuthorization(const FGameplayEffectSpec& Spec,const UY01CombatAbilitySystem* Source,
    const UY01CombatAbilitySystem* Target,float& Damage,bool& bCritical,bool& bLifesteal)
{
    if (!bAuthorizationAvailable || !bUpdating || !Spec.Def || Spec.Def->GetClass() != UY01TemplateDamageEffect::StaticClass()
        || Spec.GetContext().Get()!=AuthorizedContext || !CanRun(ActiveHandle)
        || Source!=Player()->GetCombatASC() || !Target || Target!=AuthorizedTarget.Get() || !Source->IsLegalCombatTarget(Target->GetAvatarActor())) { return false; }
    bAuthorizationAvailable=false; Damage=AuthorizedDamage; bCritical=AuthorizedCritical; bLifesteal=AuthorizedLifesteal; return true;
}
void UY01DamageEffectsComponent::EndPlay(const EEndPlayReason::Type Reason)
{
    bEnded=true; ResetCombat();
    if (Player()) { Player()->GetCombatASC()->CombatSessionInvalidated.RemoveAll(this); }
    Super::EndPlay(Reason);
}
