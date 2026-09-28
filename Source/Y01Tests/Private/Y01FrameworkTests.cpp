#include "Misc/AutomationTest.h"
#include "Y01FrameworkTestTypes.h"
#include "Combat/Y01Combat.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01Character.h"
#include "Player/Y01PlayerController.h"
#include "Combat/Y01Monster.h"
#include "Data/Y01DataCatalog.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "InputMappingContext.h"
#include "InputModifiers.h"
#include "EnhancedInputComponent.h"
#include "EnhancedPlayerInput.h"
#include "Net/UnrealNetwork.h"
#include "UObject/StrongObjectPtr.h"
#include <limits>

UY01TestInitialEffect::UY01TestInitialEffect()
{
    DurationPolicy = EGameplayEffectDurationType::Infinite;
    FGameplayModifierInfo& Mod = Modifiers.AddDefaulted_GetRef();
    Mod.Attribute = UY01AttributeSet::GetAttackDamageAttribute();
    Mod.ModifierOp = EGameplayModOp::Additive;
    Mod.ModifierMagnitude = FScalableFloat(3.f);
}
UY01TestInstantEffect::UY01TestInstantEffect()
{
    DurationPolicy = EGameplayEffectDurationType::Instant;
}
void AY01TestController::PrepareInput()
{
    PlayerInput = NewObject<UEnhancedPlayerInput>(this);
    SetupInputComponent();
}
void AY01TestController::Inject(const UInputAction* Action, const FInputActionValue& Value)
{
    auto* Enhanced = CastChecked<UEnhancedPlayerInput>(PlayerInput);
    Enhanced->InjectInputForAction(Action, Value);
    TArray<UInputComponent*> Stack { InputComponent };
    Enhanced->ProcessInputStack(Stack, 0.016f, false);
}

#if WITH_DEV_AUTOMATION_TESTS
namespace
{
FY01DataRef Ref(EY01TableKind Kind, FName Name = TEXT("Default"))
{
    FY01DataRef R; R.Type = Kind; R.RowName = Name; return R;
}
struct FFrameworkFixture
{
    UWorld* World;
    TArray<TStrongObjectPtr<UDataTable>> OwnedTables;
    FY01TableMap Tables;
    AY01PlayerState* Player;
    AY01Character* Character;
    AY01Monster* Monster;
    AY01TestController* Controller;

    FFrameworkFixture()
    {
        const auto IVS = UWorld::InitializationValues().AllowAudioPlayback(false)
            .CreatePhysicsScene(true).RequiresHitProxies(false).CreateNavigation(false).CreateAISystem(false);
        World = UWorld::CreateWorld(EWorldType::Game, false, NAME_None, nullptr, true, ERHIFeatureLevel::Num, &IVS);
        GEngine->CreateNewWorldContext(EWorldType::Game).SetCurrentWorld(World);
        World->InitializeActorsForPlay(FURL());
        Player = Spawn<AY01PlayerState>();
        Character = Spawn<AY01Character>();
        Monster = Spawn<AY01Monster>();
        Controller = Spawn<AY01TestController>();
        Controller->PlayerState = Player;
        Controller->Possess(Character);
        Character->SetActorLocation(FVector(0, 0, 100));
        Monster->SetActorLocation(FVector(100, 0, 100));
        FY01EquipmentRow Equipment; Equipment.Effect = UY01TestInitialEffect::StaticClass();
        Add(EY01TableKind::Equipment, Equipment);
        for (int I = 1; I < 6; ++I) { Add(EY01TableKind::Equipment, Equipment, FName(*FString::FromInt(I))); }
        FY01SkillRow Active; Active.Ability = UY01BasicAttackAbility::StaticClass(); Active.CooldownSeconds = 2;
        Add(EY01TableKind::Skill, Active);
        FY01SkillRow Passive; Passive.Type = EY01SkillType::Passive; Passive.Effect = UY01TestInitialEffect::StaticClass();
        Add(EY01TableKind::Skill, Passive, TEXT("Passive"));
        FY01CharacterRow Row; Row.Health = 120; Row.AttackDamage = 17; Row.MoveSpeed = 420; Row.Range = 250;
        Row.InitialEquipment.Add(Ref(EY01TableKind::Equipment));
        Row.InitialSkills = { Ref(EY01TableKind::Skill), Ref(EY01TableKind::Skill, TEXT("Passive")) };
        Add(EY01TableKind::Character, Row);
        FY01MonsterRow Mob; Mob.Health = 80; Mob.AttackDamage = 9; Mob.Range = 250; Mob.MoveSpeed = 180;
        Add(EY01TableKind::Monster, Mob);
    }
    ~FFrameworkFixture()
    {
        World->DestroyWorld(false);
        GEngine->DestroyWorldContext(World);
    }
    template<class T> T* Spawn()
    {
        FActorSpawnParameters P; P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        return World->SpawnActor<T>(FVector::ZeroVector, FRotator::ZeroRotator, P);
    }
    template<class T> void Add(EY01TableKind Kind, const T& Row, FName Name = TEXT("Default"))
    {
        if (!Tables.Contains(Kind))
        {
            auto* Table = NewObject<UDataTable>(); Table->RowStruct = T::StaticStruct();
            OwnedTables.Emplace(Table); Tables.Add(Kind, Table);
        }
        const_cast<UDataTable*>(Tables[Kind])->AddRow(Name, Row);
    }
    template<class T> T& Row(EY01TableKind Kind, FName Name = TEXT("Default"))
    {
        return *const_cast<UDataTable*>(Tables[Kind])->FindRow<T>(Name, TEXT("FrameworkTest"));
    }
    bool Initialize(FAutomationTestBase& Test)
    {
        FString Error;
        bool A = Player->TryInitializeCharacter(FY01DataCatalog(Tables), Ref(EY01TableKind::Character), Error);
        Test.TestTrue(*FString::Printf(TEXT("有效角色行须初始化成功：%s"), *Error), A);
        bool B = Monster->TryInitializeMonster(FY01DataCatalog(Tables), Ref(EY01TableKind::Monster), Error);
        Test.TestTrue(*FString::Printf(TEXT("有效怪物行须初始化成功：%s"), *Error), B);
        return A && B;
    }
    UY01CombatAbilitySystem* ASC() const { return Player->GetCombatASC(); }
    UY01CombatAbilitySystem* MobASC() const { return Monster->GetCombatASC(); }
};
void SetTestRole(AActor* Actor, ENetRole Role)
{
    // 只在夹具模拟网络端角色；生产接口不能更改此属性。
    FindFieldChecked<FByteProperty>(AActor::StaticClass(), TEXT("Role"))->SetPropertyValue_InContainer(Actor, Role);
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01CombatTest, "Y01.Framework.Combat",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01CombatTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("RealAbilityDamage"), TEXT("MonsterAttack"), TEXT("LethalFloor"), TEXT("NoTarget"),
        TEXT("SelfTarget"), TEXT("FarTarget"), TEXT("OtherWorld"), TEXT("DeadTarget"), TEXT("DeadAttacker"),
        TEXT("MovedTarget"), TEXT("DestroyedTarget"), TEXT("ClientCannotAttack"), TEXT("NegativeDamage"),
        TEXT("NaNDamage"), TEXT("InfiniteDamage"), TEXT("MissingDamage"), TEXT("ZeroDamage"), TEXT("Replication") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01CombatTest::RunTest(const FString& Case)
{
    FFrameworkFixture F;
    if (!F.Initialize(*this)) { return false; }
    auto* ASC = F.ASC(); auto* Target = F.MobASC();
    if (Case == TEXT("Replication"))
    {
        UY01AttributeSet::StaticClass()->SetUpRuntimeReplicationData();
        TArray<FLifetimeProperty> Props;
        const auto* Attributes = ASC->GetSet<UY01AttributeSet>();
        Attributes->GetLifetimeReplicatedProps(Props);
        for (const TCHAR* Name : { TEXT("Health"), TEXT("MaxHealth"), TEXT("AttackDamage") })
        {
            const auto* P = FindFieldChecked<FProperty>(UY01AttributeSet::StaticClass(), Name);
            TestTrue(TEXT("属性须复制并有RepNotify"), P->HasAllPropertyFlags(CPF_Net | CPF_RepNotify));
            TestTrue(TEXT("属性须实际注册终生复制"), Props.ContainsByPredicate([&](const FLifetimeProperty& L) { return L.RepIndex == P->RepIndex; }));
        }
        TestTrue(TEXT("玩家ASC复制"), ASC->GetIsReplicated()); TestTrue(TEXT("怪物ASC独立且复制"), ASC != Target && Target->GetIsReplicated());
        TestTrue(TEXT("原生伤害标签"), Y01Tags::Data_Damage.GetTag().IsValid());
        const auto* Ability = GetDefault<UY01BasicAttackAbility>();
        TestEqual(TEXT("只在服务端执行"), Ability->GetNetExecutionPolicy(), EGameplayAbilityNetExecutionPolicy::ServerOnly);
        TestEqual(TEXT("禁止客户端发起GAS激活"), Ability->GetNetSecurityPolicy(), EGameplayAbilityNetSecurityPolicy::ServerOnly);
        return true;
    }
    if (Case == TEXT("MonsterAttack"))
    {
        TestTrue(TEXT("怪物由权威指定目标"), Target->SetCombatTarget(F.Character));
        TestTrue(TEXT("怪物真实GA激活"), Target->TryActivateBasicAttack());
        TestEqual(TEXT("怪物从行读取攻击力"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 111.f);
        return true;
    }
    if (Case == TEXT("NoTarget")) { TestFalse(TEXT("无目标不得激活"), ASC->TryActivateBasicAttack()); return true; }
    if (Case == TEXT("SelfTarget")) { TestFalse(TEXT("拒绝自身"), ASC->SetCombatTarget(F.Character)); return true; }
    if (Case == TEXT("FarTarget"))
    {
        F.Monster->SetActorLocation(FVector(1000, 0, 100));
        TestFalse(TEXT("拒绝超距目标"), ASC->SetCombatTarget(F.Monster)); return true;
    }
    if (Case == TEXT("OtherWorld"))
    {
        FFrameworkFixture Other; if (!Other.Initialize(*this)) { return false; }
        TestFalse(TEXT("拒绝其他世界"), ASC->SetCombatTarget(Other.Monster)); return true;
    }
    TestTrue(TEXT("服务端指定有效目标"), ASC->SetCombatTarget(F.Monster));
    if (Case.Contains(TEXT("Damage")) && Case != TEXT("RealAbilityDamage"))
    {
        auto Spec = ASC->MakeOutgoingSpec(UY01DamageEffect::StaticClass(), 1, ASC->MakeEffectContext());
        TestTrue(TEXT("伤害GE为Instant"), Spec.Data->Def->DurationPolicy == EGameplayEffectDurationType::Instant);
        float Damage = 0;
        if (Case == TEXT("NegativeDamage")) { Damage = -10; }
        if (Case == TEXT("NaNDamage")) { Damage = std::numeric_limits<float>::quiet_NaN(); }
        if (Case == TEXT("InfiniteDamage")) { Damage = std::numeric_limits<float>::infinity(); }
        if (Case != TEXT("MissingDamage")) { Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Damage, Damage); }
        ASC->ApplyGameplayEffectSpecToTarget(*Spec.Data, Target);
        TestEqual(TEXT("非法或零伤害不改变生命"), Target->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 80.f);
        return true;
    }
    if (Case == TEXT("ClientCannotAttack"))
    {
        SetTestRole(F.Player, ROLE_AutonomousProxy);
        TestFalse(TEXT("客户端不能指定权威目标"), ASC->SetCombatTarget(F.Monster));
        TestFalse(TEXT("客户端不能发基础攻击"), ASC->TryActivateBasicAttack());
        TestFalse(TEXT("客户端无法绕过入口直接激活"), ASC->TryActivateAbility(ASC->GetBasicAttackHandle(), false));
        SetTestRole(F.Player, ROLE_Authority);
        TestEqual(TEXT("客户端未造成伤害"), Target->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 80.f);
        return true;
    }
    if (Case == TEXT("MovedTarget")) { F.Monster->SetActorLocation(FVector(1000, 0, 100)); }
    if (Case == TEXT("DestroyedTarget")) { F.Monster->Destroy(); }
    if (Case == TEXT("DeadAttacker")) { ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 0); }
    if (Case == TEXT("DeadTarget"))
    {
        for (int I = 0; I < 4; ++I) { ASC->TryActivateBasicAttack(); }
        TestEqual(TEXT("真实攻击杀死目标"), Target->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 0.f);
    }
    if (Case == TEXT("MovedTarget") || Case == TEXT("DestroyedTarget") || Case.StartsWith(TEXT("Dead")))
    {
        TestFalse(TEXT("激活时必须再次验证目标与攻击者"), ASC->TryActivateBasicAttack()); return true;
    }
    int32 Activated = 0, Effects = 0;
    ASC->AbilityActivatedCallbacks.AddLambda([&](UGameplayAbility*) { ++Activated; });
    Target->OnGameplayEffectAppliedDelegateToSelf.AddLambda([&](UAbilitySystemComponent*, const FGameplayEffectSpec& Spec, FActiveGameplayEffectHandle)
    {
        if (Spec.Def->IsA<UY01DamageEffect>()) { ++Effects; TestEqual(TEXT("SetByCaller来自服务器攻击属性"), Spec.GetSetByCallerMagnitude(Y01Tags::Data_Damage), 23.f); }
    });
    const int Count = Case == TEXT("LethalFloor") ? 4 : 1;
    for (int I = 0; I < Count; ++I) { TestTrue(TEXT("必须经真实GA激活"), ASC->TryActivateBasicAttack()); }
    TestEqual(TEXT("真实激活回调次数"), Activated, Count); TestEqual(TEXT("真实GE应用次数"), Effects, Count);
    TestEqual(TEXT("GE修改生命且死亡截断"), Target->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), Case == TEXT("LethalFloor") ? 0.f : 57.f);
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01PlayerStateTest, "Y01.Framework.State",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01PlayerStateTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("Initialize"), TEXT("SixEquipmentFourSkills"), TEXT("BadEquipmentRef"), TEXT("BadSkillRef"),
        TEXT("SevenEquipment"), TEXT("FiveSkills"), TEXT("BadStats"), TEXT("BadCooldown"), TEXT("BadSkillType"), TEXT("BadAbilityClass"),
        TEXT("DifferentDefinition"), TEXT("RepeatPossess"), TEXT("ClientActorInfo"), TEXT("Sources"), TEXT("ActiveCooldown"),
        TEXT("PassiveSlot"), TEXT("InvalidSlot"), TEXT("ClientInitialize"), TEXT("Reward"), TEXT("NegativeReward"),
        TEXT("GoldOverflow"), TEXT("ExperienceOverflow"), TEXT("ClientReward"), TEXT("Replication"), TEXT("MonsterIdempotence") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01PlayerStateTest::RunTest(const FString& Case)
{
    FFrameworkFixture F;
    auto& Row = F.Row<FY01CharacterRow>(EY01TableKind::Character);
    FString Error;
    if (Case == TEXT("SixEquipmentFourSkills"))
    {
        for (int I = 1; I < 6; ++I) { Row.InitialEquipment.Add(Ref(EY01TableKind::Equipment, FName(*FString::FromInt(I)))); }
        Row.InitialSkills.Add(Ref(EY01TableKind::Skill)); Row.InitialSkills.Add(Ref(EY01TableKind::Skill, TEXT("Passive")));
    }
    if (Case.StartsWith(TEXT("Bad")) || Case == TEXT("SevenEquipment") || Case == TEXT("FiveSkills"))
    {
        // 先验证同一目录中的合法配置，避免拒绝路径测试因永远返回false而假绿。
        auto* Baseline = F.Spawn<AY01PlayerState>();
        if (!TestTrue(TEXT("合法基准必须能初始化"), Baseline->TryInitializeCharacter(FY01DataCatalog(F.Tables), Ref(EY01TableKind::Character), Error))) { return false; }
        if (Case == TEXT("BadEquipmentRef")) { Row.InitialEquipment.Add(Ref(EY01TableKind::Skill)); }
        if (Case == TEXT("BadSkillRef")) { Row.InitialSkills.Add(Ref(EY01TableKind::Skill, TEXT("Missing"))); }
        if (Case == TEXT("SevenEquipment")) { Row.InitialEquipment.Init(Ref(EY01TableKind::Equipment), 7); }
        if (Case == TEXT("FiveSkills")) { Row.InitialSkills.Init(Ref(EY01TableKind::Skill), 5); }
        if (Case == TEXT("BadStats")) { Row.Health = std::numeric_limits<float>::quiet_NaN(); }
        if (Case == TEXT("BadCooldown")) { F.Row<FY01SkillRow>(EY01TableKind::Skill).CooldownSeconds = -1; }
        if (Case == TEXT("BadSkillType")) { F.Row<FY01SkillRow>(EY01TableKind::Skill).Type = static_cast<EY01SkillType>(255); }
        if (Case == TEXT("BadAbilityClass")) { F.Row<FY01SkillRow>(EY01TableKind::Skill).Ability = UGameplayAbility::StaticClass(); }
        TestFalse(TEXT("非法初始装配必须拒绝"), F.Player->TryInitializeCharacter(FY01DataCatalog(F.Tables), Ref(EY01TableKind::Character), Error));
        TestFalse(TEXT("提供拒绝原因"), Error.IsEmpty()); TestFalse(TEXT("失败未提交状态"), F.Player->IsCharacterInitialized());
        TestEqual(TEXT("无部分能力"), F.ASC()->GetActivatableAbilities().Num(), 0);
        TestEqual(TEXT("无部分效果"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 0);
        TestEqual(TEXT("无部分属性"), F.ASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 0.f);
        for (const auto& Slot : F.Player->GetEquipmentSlots()) { TestTrue(TEXT("失败槽位为空"), Slot.RowName.IsNone()); }
        return true;
    }
    if (!F.Initialize(*this)) { return false; }
    auto* ASC = F.ASC();
    if (Case == TEXT("Initialize") || Case == TEXT("SixEquipmentFourSkills"))
    {
        TestEqual(TEXT("固定六装备槽"), F.Player->GetEquipmentSlots().Num(), 6); TestEqual(TEXT("固定四共用技能槽"), F.Player->GetSkillSlots().Num(), 4);
        TestEqual(TEXT("生命来自角色表"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 120.f);
        TestEqual(TEXT("最大生命来自角色表"), ASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()), 120.f);
        TestEqual(TEXT("初始装备和技能效果真实应用"), ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), Case == TEXT("Initialize") ? 23.f : 41.f);
        TestEqual(TEXT("角色移动速度"), F.Character->GetCharacterMovement()->MaxWalkSpeed, 420.f);
        TestEqual(TEXT("怪物移动速度"), F.Monster->GetCharacterMovement()->MaxWalkSpeed, 180.f);
        TestEqual(TEXT("怪物生命"), F.MobASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 80.f);
        TestTrue(TEXT("角色可碰撞"), F.Character->GetCapsuleComponent()->GetCollisionEnabled() != ECollisionEnabled::NoCollision);
        TestTrue(TEXT("怪物可碰撞"), F.Monster->GetCapsuleComponent()->GetCollisionEnabled() != ECollisionEnabled::NoCollision);
        return true;
    }
    if (Case == TEXT("DifferentDefinition"))
    {
        F.Add(EY01TableKind::Character, Row, TEXT("Other"));
        TestFalse(TEXT("一次性初始化不能替换角色"), F.Player->TryInitializeCharacter(FY01DataCatalog(F.Tables), Ref(EY01TableKind::Character, TEXT("Other")), Error)); return true;
    }
    if (Case == TEXT("RepeatPossess") || Case == TEXT("MonsterIdempotence"))
    {
        auto* Victim = Case == TEXT("RepeatPossess") ? ASC : F.MobASC();
        auto* Attacker = Case == TEXT("RepeatPossess") ? F.MobASC() : ASC;
        Attacker->SetCombatTarget(Case == TEXT("RepeatPossess") ? static_cast<AActor*>(F.Character) : F.Monster);
        TestTrue(TEXT("先真实攻击"), Attacker->TryActivateBasicAttack());
        float Health = Victim->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
        int32 Abilities = Victim->GetActivatableAbilities().Num(), Effects = Victim->GetActiveEffects(FGameplayEffectQuery()).Num();
        if (Case == TEXT("RepeatPossess"))
        {
            ASC->SetCombatTarget(F.Monster); TestTrue(TEXT("先进入技能冷却"), F.Player->TryActivateSkillSlot(0));
            F.Controller->UnPossess(); F.Controller->Possess(F.Character);
            TestTrue(TEXT("重复初始化同角色成功但无副作用"), F.Player->TryInitializeCharacter(FY01DataCatalog(F.Tables), Ref(EY01TableKind::Character), Error));
            TestFalse(TEXT("重复占有不重置CD"), F.Player->TryActivateSkillSlot(0));
            TestTrue(TEXT("ASC归PlayerState且Avatar为角色"), ASC->GetOwnerActor() == F.Player && ASC->GetAvatarActor() == F.Character);
        }
        else { TestTrue(TEXT("怪物初始化幂等"), F.Monster->TryInitializeMonster(FY01DataCatalog(F.Tables), Ref(EY01TableKind::Monster), Error)); }
        TestEqual(TEXT("不重置生命"), Victim->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), Health);
        TestEqual(TEXT("不重复GA"), Victim->GetActivatableAbilities().Num(), Abilities); TestEqual(TEXT("不重复GE"), Victim->GetActiveEffects(FGameplayEffectQuery()).Num(), Effects); return true;
    }
    if (Case == TEXT("ClientActorInfo"))
    {
        SetTestRole(F.Character, ROLE_SimulatedProxy); F.Character->OnRep_PlayerState();
        TestTrue(TEXT("OnRep设置ActorInfo"), ASC->GetOwnerActor() == F.Player && ASC->GetAvatarActor() == F.Character);
        SetTestRole(F.Character, ROLE_Authority); return true;
    }
    if (Case == TEXT("Sources"))
    {
        const auto& Grants = ASC->GetGrantedSources();
        TestEqual(TEXT("基础攻击、装备、主动和被动各有来源"), Grants.Num(), 4);
        int32 AbilityCount = 0, EffectCount = 0;
        for (const auto& Grant : Grants)
        {
            TestFalse(TEXT("来源保留行身份"), Grant.Source.RowName.IsNone());
            if (Grant.Ability.IsValid()) { ++AbilityCount; TestNotNull(TEXT("能力句柄可用于后续清理"), ASC->FindAbilitySpecFromHandle(Grant.Ability)); }
            if (Grant.Effect.IsValid()) { ++EffectCount; TestNotNull(TEXT("持续效果句柄可用于后续清理"), ASC->GetActiveGameplayEffect(Grant.Effect)); }
        }
        TestEqual(TEXT("记录GA"), AbilityCount, 2); TestEqual(TEXT("记录GE"), EffectCount, 2); return true;
    }
    if (Case == TEXT("ActiveCooldown"))
    {
        ASC->SetCombatTarget(F.Monster);
        TestTrue(TEXT("主动槽激活真实能力"), F.Player->TryActivateSkillSlot(0));
        TestEqual(TEXT("技能实际伤害"), F.MobASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 57.f);
        TestFalse(TEXT("服务器CD阻挡重复请求"), F.Player->TryActivateSkillSlot(0));
        TestTrue(TEXT("CD取自表"), ASC->GetSkillCooldownRemaining(0) > 1.9f);
        for (int32 I = 0; I < 21; ++I) { F.World->Tick(LEVELTICK_All, 0.1f); }
        TestTrue(TEXT("真实世界时间推进超过CD"), F.World->GetTimeSeconds() > 2.0);
        TestTrue(TEXT("CD到期可再激活"), F.Player->TryActivateSkillSlot(0)); return true;
    }
    if (Case == TEXT("PassiveSlot")) { TestFalse(TEXT("被动槽不能主动触发"), F.Player->TryActivateSkillSlot(1)); return true; }
    if (Case == TEXT("InvalidSlot"))
    {
        for (int32 Slot : { -1, 2, 3, 4, MAX_int32 }) { TestFalse(TEXT("非法或空槽拒绝"), F.Player->TryActivateSkillSlot(Slot)); } return true;
    }
    if (Case == TEXT("ClientInitialize"))
    {
        auto* Client = F.Spawn<AY01PlayerState>(); SetTestRole(Client, ROLE_AutonomousProxy);
        TestFalse(TEXT("客户端不得初始化属性与能力"), Client->TryInitializeCharacter(FY01DataCatalog(F.Tables), Ref(EY01TableKind::Character), Error));
        TestFalse(TEXT("客户端无提交"), Client->IsCharacterInitialized()); SetTestRole(Client, ROLE_Authority); return true;
    }
    if (Case == TEXT("Replication"))
    {
        AY01PlayerState::StaticClass()->SetUpRuntimeReplicationData();
        TArray<FLifetimeProperty> Props; F.Player->GetLifetimeReplicatedProps(Props);
        for (const TCHAR* Name : { TEXT("Gold"), TEXT("Experience"), TEXT("EquipmentSlots"), TEXT("SkillSlots") })
        {
            const auto* P = FindFieldChecked<FProperty>(AY01PlayerState::StaticClass(), Name);
            TestTrue(TEXT("玩家状态字段复制"), P->HasAnyPropertyFlags(CPF_Net));
            TestTrue(TEXT("玩家字段注册复制"), Props.ContainsByPredicate([&](const FLifetimeProperty& L) { return L.RepIndex == P->RepIndex; }));
        }
        return true;
    }
    TestTrue(TEXT("权威奖励入口可用"), F.Player->TryAddMatchReward(7, 11));
    if (Case == TEXT("NegativeReward")) { TestFalse(TEXT("负金币原子拒绝"), F.Player->TryAddMatchReward(-1, 10)); TestFalse(TEXT("负经验原子拒绝"), F.Player->TryAddMatchReward(10, -1)); }
    if (Case == TEXT("GoldOverflow")) { TestFalse(TEXT("金币溢出原子拒绝"), F.Player->TryAddMatchReward(MAX_int32, 10)); }
    if (Case == TEXT("ExperienceOverflow")) { TestFalse(TEXT("经验溢出原子拒绝"), F.Player->TryAddMatchReward(10, MAX_int32)); }
    if (Case == TEXT("ClientReward"))
    {
        SetTestRole(F.Player, ROLE_AutonomousProxy); TestFalse(TEXT("客户端奖励拒绝"), F.Player->TryAddMatchReward(100, 100));
        SetTestRole(F.Player, ROLE_Authority);
    }
    TestEqual(TEXT("金币单次结果"), F.Player->GetGold(), 7); TestEqual(TEXT("经验单次结果"), F.Player->GetExperience(), 11);
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01InputTest, "Y01.Framework.Input",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01InputTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("KeyboardMapping"), TEXT("FixedCamera"), TEXT("MovementAction"), TEXT("SkillAction"), TEXT("ServerSlotValidation") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01InputTest::RunTest(const FString& Case)
{
    FFrameworkFixture F;
    if (Case == TEXT("FixedCamera"))
    {
        auto* Arm = F.Character->FindComponentByClass<USpringArmComponent>();
        auto* Camera = F.Character->FindComponentByClass<UCameraComponent>();
        if (!TestNotNull(TEXT("固定跟随臂"), Arm) || !TestNotNull(TEXT("相机"), Camera)) { return false; }
        TestFalse(TEXT("相机不跟控制旋转"), Arm->bUsePawnControlRotation || Camera->bUsePawnControlRotation);
        TestTrue(TEXT("跟随臂世界旋转固定"), Arm->IsUsingAbsoluteRotation());
        TestFalse(TEXT("角色不接受控制器自由旋转"), F.Character->bUseControllerRotationYaw || F.Character->bUseControllerRotationPitch || F.Character->bUseControllerRotationRoll);
        const FRotator Before = Arm->GetComponentRotation();
        F.Controller->SetControlRotation(FRotator(80, 90, 40)); F.Character->SetActorRotation(FRotator(0, 120, 0));
        TestTrue(TEXT("旋转控制器和角色均不改变相机方向"), Before.Equals(Arm->GetComponentRotation())); return true;
    }
    F.Controller->PrepareInput();
    const auto* Mapping = F.Controller->GetKeyboardMapping();
    if (!TestNotNull(TEXT("无资产键盘Mapping"), Mapping)) { return false; }
    if (Case == TEXT("KeyboardMapping"))
    {
        TestEqual(TEXT("仅WASD和1至4八项映射"), Mapping->GetMappings().Num(), 8);
        for (const FKey Key : { EKeys::W, EKeys::A, EKeys::S, EKeys::D, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four })
        { TestTrue(TEXT("必需按键绑定"), Mapping->GetMappings().ContainsByPredicate([&](const auto& M) { return M.Key == Key; })); }
        return true;
    }
    if (Case == TEXT("MovementAction"))
    {
        F.Controller->SetControlRotation(FRotator(0, 90, 0));
        F.Controller->Inject(F.Controller->GetMoveAction(), FInputActionValue(FVector2D(1, 1)));
        const FVector Move = F.Character->ConsumeMovementInputVector();
        TestTrue(TEXT("真实EnhancedInput回调产生平面移动"), Move.X > 0 && Move.Y > 0 && FMath::IsNearlyZero(Move.Z)); return true;
    }
    if (!F.Initialize(*this)) { return false; }
    F.ASC()->SetCombatTarget(F.Monster);
    if (Case == TEXT("SkillAction"))
    {
        F.Controller->Inject(F.Controller->GetSkillAction(0), FInputActionValue(true));
        TestEqual(TEXT("真实输入经过Controller和服务端槽入口造成伤害"), F.MobASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 57.f);
    }
    else
    {
        F.Controller->RequestSkillSlot(-1); F.Controller->RequestSkillSlot(4); F.Controller->RequestSkillSlot(1);
        TestEqual(TEXT("服务端请求拒绝非法槽与被动槽"), F.MobASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 80.f);
        F.Controller->RequestSkillSlot(0);
        TestEqual(TEXT("有效请求到达能力"), F.MobASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 57.f);
    }
    return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FY01FrameworkEdgeTest, "Y01.Framework.Edge",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::ProductFilter)
void FY01FrameworkEdgeTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    for (const TCHAR* Name : { TEXT("SpeedAfterAvatar"), TEXT("SpeedBeforeAvatar"), TEXT("ZeroSpeed"), TEXT("MonsterSpeed"),
        TEXT("DirectAbilityCooldown"), TEXT("EffectRejectionRollback"), TEXT("InstantEffectSource"),
        TEXT("InitializationReentry"), TEXT("InitializeBeforePossess") })
    { Names.Add(Name); Commands.Add(Name); }
}
bool FY01FrameworkEdgeTest::RunTest(const FString& Case)
{
    FFrameworkFixture F;
    auto* ASC = F.ASC();
    FString Error;
    if (Case.Contains(TEXT("Speed")))
    {
        if (!F.Initialize(*this)) { return false; }
        const float ServerSpeed = Case == TEXT("ZeroSpeed") ? 0.f
            : (Case == TEXT("MonsterSpeed") ? F.MobASC()->GetInitialMoveSpeed() : ASC->GetInitialMoveSpeed());
        FFrameworkFixture Client;
        auto* ClientASC = Case == TEXT("MonsterSpeed") ? Client.MobASC() : Client.ASC();
        ACharacter* Avatar = Case == TEXT("MonsterSpeed") ? static_cast<ACharacter*>(Client.Monster) : Client.Character;
        AActor* Owner = Case == TEXT("MonsterSpeed") ? static_cast<AActor*>(Client.Monster) : Client.Player;
        SetTestRole(Owner, ROLE_SimulatedProxy); SetTestRole(Avatar, ROLE_SimulatedProxy);
        ClientASC->ClearActorInfo();
        UY01CombatAbilitySystem::StaticClass()->SetUpRuntimeReplicationData();
        auto* Property = FindFProperty<FFloatProperty>(UY01CombatAbilitySystem::StaticClass(), TEXT("InitialMoveSpeed"));
        auto* Notify = ClientASC->FindFunction(TEXT("OnRep_InitialMoveSpeed"));
        if (!TestNotNull(TEXT("速度复制字段存在"), Property) || !TestNotNull(TEXT("速度复制通知存在"), Notify)) { return false; }
        TArray<FLifetimeProperty> Props; static_cast<const UObject*>(ClientASC)->GetLifetimeReplicatedProps(Props);
        TestTrue(TEXT("速度需要复制及通知"), Property->HasAllPropertyFlags(CPF_Net | CPF_RepNotify));
        TestTrue(TEXT("速度实际注册复制"), Props.ContainsByPredicate([&](const FLifetimeProperty& P) { return P.RepIndex == Property->RepIndex; }));
        auto AttachAvatar = [&]()
        {
            if (Case == TEXT("MonsterSpeed")) { ClientASC->InitAbilityActorInfo(Owner, Avatar); }
            else { Client.Character->OnRep_PlayerState(); }
        };
        if (Case != TEXT("SpeedBeforeAvatar")) { AttachAvatar(); }
        // 本地模拟复制投递顺序，不将此测试当作真实网络收包验证。
        Property->SetPropertyValue_InContainer(ClientASC, ServerSpeed);
        ClientASC->ProcessEvent(Notify, nullptr);
        if (Case == TEXT("SpeedBeforeAvatar")) { AttachAvatar(); }
        TestEqual(TEXT("客户端使用服务器表速度而不是默认600"), Avatar->GetCharacterMovement()->MaxWalkSpeed, ServerSpeed);
        TestFalse(TEXT("客户端无需执行权威初始化"), ClientASC->IsCombatInitialized());
        TestEqual(TEXT("复制速度不授予能力"), ClientASC->GetActivatableAbilities().Num(), 0);
        TestEqual(TEXT("复制速度不初始化生命"), ClientASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 0.f);
        SetTestRole(Owner, ROLE_Authority); SetTestRole(Avatar, ROLE_Authority);
        return true;
    }
    if (Case == TEXT("EffectRejectionRollback"))
    {
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetMaxHealthAttribute(), 50);
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 30);
        ASC->SetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute(), 5);
        int32 Queries = 0, Applied = 0;
        ASC->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda(
            [&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { return ++Queries != 2; }));
        const auto AppliedHandle = ASC->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
            [&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle) { ++Applied; });
        TestFalse(TEXT("第二个真实GE拒绝时初始化失败"), F.Player->TryInitializeCharacter(FY01DataCatalog(F.Tables), Ref(EY01TableKind::Character), Error));
        TestEqual(TEXT("确实走过两次GAS应用检查"), Queries, 2);
        TestEqual(TEXT("首个GE实际应用过"), Applied, 1);
        TestFalse(TEXT("失败有原因且未提交"), Error.IsEmpty() || F.Player->IsCharacterInitialized());
        TestEqual(TEXT("回滚实际授予的能力"), ASC->GetActivatableAbilities().Num(), 0);
        TestEqual(TEXT("回滚实际应用的持续效果"), ASC->GetActiveEffects(FGameplayEffectQuery()).Num(), 0);
        TestEqual(TEXT("恢复最大生命"), ASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()), 50.f);
        TestEqual(TEXT("恢复生命"), ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 30.f);
        TestEqual(TEXT("恢复攻击"), ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), 5.f);
        TestEqual(TEXT("不留来源"), ASC->GetGrantedSources().Num(), 0);
        for (const auto& Slot : F.Player->GetEquipmentSlots()) { TestTrue(TEXT("不留装备槽"), Slot.RowName.IsNone()); }
        for (const auto& Slot : F.Player->GetSkillSlots()) { TestTrue(TEXT("不留技能槽"), Slot.RowName.IsNone()); }
        ASC->GameplayEffectApplicationQueries.Reset(); ASC->OnGameplayEffectAppliedDelegateToSelf.Remove(AppliedHandle);
        TestTrue(TEXT("失败回滚后可重新初始化"), F.Initialize(*this));
        TestEqual(TEXT("重试效果不叠加残留"), ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), 23.f);
        return true;
    }
    if (Case == TEXT("InitializeBeforePossess"))
    {
        F.Controller->UnPossess(); ASC->ClearActorInfo();
        if (!F.Initialize(*this)) { return false; }
        F.Controller->Possess(F.Character);
        TestTrue(TEXT("先初始化再占有建立ActorInfo"), ASC->GetOwnerActor() == F.Player && ASC->GetAvatarActor() == F.Character);
        TestEqual(TEXT("先初始化再占有应用速度"), F.Character->GetCharacterMovement()->MaxWalkSpeed, 420.f);
        TestTrue(TEXT("随后能指定目标"), ASC->SetCombatTarget(F.Monster));
        TestTrue(TEXT("随后能实际攻击"), ASC->TryActivateBasicAttack());
        TestEqual(TEXT("真实伤害结果"), F.MobASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 57.f);
        return true;
    }
    if (Case == TEXT("InstantEffectSource"))
    { F.Row<FY01EquipmentRow>(EY01TableKind::Equipment).Effect = UY01TestInstantEffect::StaticClass(); }
    if (Case == TEXT("InitializationReentry"))
    {
        int32 Calls = 0;
        const auto Handle = ASC->OnGameplayEffectAppliedDelegateToSelf.AddLambda(
            [&](UAbilitySystemComponent*, const FGameplayEffectSpec&, FActiveGameplayEffectHandle)
            {
                ++Calls; FString ReentryError;
                TestFalse(TEXT("真实GE回调不能重入初始化"), F.Player->TryInitializeCharacter(FY01DataCatalog(F.Tables), Ref(EY01TableKind::Character), ReentryError));
            });
        const bool Result = F.Initialize(*this);
        ASC->OnGameplayEffectAppliedDelegateToSelf.Remove(Handle);
        TestTrue(TEXT("外层初始化成功"), Result); TestEqual(TEXT("真实GE回调次数"), Calls, 2);
        TestEqual(TEXT("重入没有重复能力"), ASC->GetActivatableAbilities().Num(), 2);
        TestEqual(TEXT("重入没有重复效果"), ASC->GetActiveEffects(FGameplayEffectQuery()).Num(), 2);
        return true;
    }
    if (!F.Initialize(*this)) { return false; }
    if (Case == TEXT("InstantEffectSource"))
    {
        const auto* Source = ASC->GetGrantedSources().FindByPredicate([](const FY01GrantedSource& Grant)
            { return Grant.Source.Type == EY01TableKind::Equipment && Grant.Slot == 0; });
        if (!TestNotNull(TEXT("瞬时效果仍有行及槽来源"), Source)) { return false; }
        TestTrue(TEXT("瞬时GE记录应用结果与类型"), Source->bEffectApplied && Source->EffectClass == UY01TestInstantEffect::StaticClass());
        TestFalse(TEXT("瞬时GE不伪造可移除持续句柄"), Source->Effect.IsValid());
        TestEqual(TEXT("瞬时GE真实改变基础攻击属性"), ASC->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute()), 20.f);
        return true;
    }
    const auto* Source = ASC->GetGrantedSources().FindByPredicate([](const FY01GrantedSource& Grant)
        { return Grant.Source.Type == EY01TableKind::Skill && Grant.Slot == 0; });
    if (!TestNotNull(TEXT("主动能力来源"), Source)) { return false; }
    const auto Handle = Source->Ability;
    TestTrue(TEXT("指定服务端目标"), ASC->SetCombatTarget(F.Monster));
    TestTrue(TEXT("直接GAS激活真实主动GA"), ASC->TryActivateAbility(Handle, false));
    TestEqual(TEXT("直接激活真实GE扣血"), F.MobASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 57.f);
    TestTrue(TEXT("GAS提交同样启动表CD"), ASC->GetSkillCooldownRemaining(0) > 1.9f);
    TestFalse(TEXT("直接GAS入口不能重复绕过CD"), ASC->TryActivateAbility(Handle, false));
    TestFalse(TEXT("不能改走槽入口绕过CD"), F.Player->TryActivateSkillSlot(0));
    TestEqual(TEXT("CD中没有二次伤害"), F.MobASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 57.f);
    for (int32 I = 0; I < 21; ++I) { F.World->Tick(LEVELTICK_All, 0.1f); }
    TestTrue(TEXT("直接GAS入口在CD到期后可用"), ASC->TryActivateAbility(Handle, false));
    TestEqual(TEXT("到期后真实伤害"), F.MobASC()->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute()), 34.f);
    return true;
}
#endif
