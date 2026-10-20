# Y01 R37 第二期实施计划：暴击 / 吸血 / 护盾 / 共享伤害管线 / 左上角血条

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现暴击率/暴击倍率/吸血%三类属性、按来源分层护盾组件、三执行器统一共享伤害管线（基础→×暴击→减甲→护盾→扣血→吸血）、暴击飘字色，并把玩家左上角"金币+生命"文本换成金币文本+生命血条+护盾覆盖条。

**Architecture:** 暴击/吸血进 `UY01AttributeSet` 新属性（结算器写 base，沿用护甲/生命回复模式）；伤害结算抽成纯函数 `Y01DamagePipeline`，三个执行器（`UY01DamageExecution`、`UY01TemplateDamageExecution`、`UY01TestSkillDamageExecution`）统一调用；护盾为 PlayerState 新服务器组件，按来源分层、只复制总量；`FY01DamageEvent` 增加暴击标记经既有 RPC 传输。

**Tech Stack:** UE5.7.4 源码引擎（G:\UE_Engine\g01_engine）、GAS、UMG（纯代码 R36 测试 HUD）。

---

## 纪律（每任务开始前重读）

- 原地 dirty main 修改：**不提交、不推送、不重置、不 stash、不清理、不建 worktree、不终止其他窗口进程**。
- **不覆盖其他窗口的新调值**：改正式 `.uasset` 前先读最新内容；本期**禁止**自动改写任何正式 DataTable 资产，测试全部用内存构造表（`FY01DataCatalog` 测试夹具模式）。
- 设计依据：`Docs/superpowers/specs/2026-10-15-y01-r37-extended-affixes-design.md` 第2节 + 用户确认规则（百分比 Amount 填 20=+20%；护盾首版只配固定值；死亡/复活/离局清空护盾；吸血按实际生命损失、逐次绿色飘字、不回复护盾；DOT 按放置时判暴击一次）。
- 构建前检查外部编辑器/构建占用，不终止他人进程。构建命令与 Automation 运行方式见记忆文档 `C:\Users\俞鑫\.qoder\projects\g--GarbageProject-Y01\memory\y01-ue5-automation.md`（PowerShell 受限语言模式：通过 Node spawnSync + EncodedCommand 启动 `Scripts/Run-Y01Automation.ps1`；报告以 `Saved/<Label>/index.json` 为准；后台任务超时设 600000ms）。
- 本期完成即停，不实现第三期（时间/普攻成长）与第四期（金币获取%）。

## 语义锁定表

| 词条 | 配表字段 | 内部含义 |
| --- | --- | --- |
| 暴击率 | `CritChance`，`Amount` 填百分点（20=+20%） | 合计/100 后 clamp 到 [0,1]，封顶100% |
| 暴击倍率加成 | `CritMultiplierAdd`，`Amount` 为倍率值（0.5=+0.5倍） | 最终倍率 = DT_MatchRules.DefaultCritMultiplier(默认2.0) + 合计 |
| 吸血 | `Lifesteal`，`Amount` 填百分点（20=20%） | 合计/100，非负不封顶 |
| 周期护盾 | `ShieldPeriodic`，`Amount`/`DurationSeconds`/`LayerCap`/`IntervalSeconds` | 按间隔获得，同来源层累加至LayerCap并刷新时长 |
| 施放护盾 | `ShieldOnCast`，`IntervalSeconds` 必须为0 | 仅技能来源；释放该技能时获得 |
| 伤害可吸血 | `FY01DamageEffectRow.bLifesteal`（默认false=DOT关）；`FY01TestSkillConfig.bLifesteal`（默认true=普攻/主动开） | 源无吸血属性时无效果 |
| 暴击颜色 | `FY01MatchPresentation.CriticalColor` | 默认橙色；飘字三选一：治疗绿/暴击CriticalColor/普通DamageColor |

伤害顺序：快照基础 → ×暴击倍率（若暴击）→ 减护甲（下限0）→ 护盾吸收（仅玩家目标）→ 扣生命（不超过当前生命）→ 吸血（按实际生命损失，在受击方 PostGameplayEffectExecute 提交链内回给攻击者）。红色飘字显示扣甲后数值（与现口径一致）。

暴击判定时机：直接命中=取伤害快照时；飞行物=发射时（结果存产物）；DOT/持续区域=放置时判一次，后续每跳沿用。

## 文件职责表

| 文件 | 动作 | 职责 |
| --- | --- | --- |
| `Source/Y01/Data/Y01DataTypes.h` | 修改 | 效果种类枚举+5、SourceEffectRow 新增3字段、伤害行/测试技能配置 bLifesteal、MatchRules 默认暴击倍率、Presentation 暴击色、ValidSourceEffect 新种类校验 |
| `Source/Y01/Player/Y01SourceRuntimeTypes.h` | 修改 | Snapshot/ResolvedSourceEffect 透传新字段（追加在结构体末尾） |
| `Source/Y01/Player/Y01ItemRuntimeTypes.h` | 修改 | FY01ItemEffect 透传新字段 |
| `Source/Y01/Player/Y01AttributeResolver.h/.cpp` | 修改 | ResolvedAttributes 增 CritChance/CritMultiplierBonus/Lifesteal；Resolve 处理新种类 |
| `Source/Y01/Data/Y01DataCatalog.cpp` | 修改 | SourceEffect 行校验新字段、ShieldOnCast 仅技能来源、MatchRules 新字段校验 |
| `Source/Y01/Combat/Y01Combat.h/.cpp` | 修改 | AttributeSet 新属性×3、新Tag Data_Crit/Data_LifestealAllowed、执行器1接管线、提交链写base、事件标记/护盾扣减/吸血应用 |
| `Source/Y01/Combat/Y01DamagePipeline.h/.cpp` | 新建 | 共享暴击判定与伤害结算纯函数 |
| `Source/Y01/Player/Y01ShieldComponent.h/.cpp` | 新建 | 服务器分层护盾：Grant/Consume/过期/周期来源同步/TotalShield复制 |
| `Source/Y01/Player/Y01PlayerState.h/.cpp` | 修改 | 挂载护盾组件、死亡清空、初始化清空 |
| `Source/Y01/Player/Y01DamageEffectsComponent.h/.cpp` | 修改 | FProduct 存暴击、Launch/Hit/ConsumeAuthorization 透传、施放护盾钩子 |
| `Source/Y01/Combat/Y01DamageEffects.cpp` | 修改 | UY01TemplateDamageExecution 接管线 |
| `Source/Y01Tests/Private/Playground/Y01TestSkill.h/.cpp` | 修改 | 测试状态存暴击、执行器3接管线、普攻施放护盾钩子 |
| `Source/Y01/Match/Y01GameFlow.h/.cpp` | 修改 | GameMode 存 DefaultCritMultiplier + getter |
| `Source/Y01/Presentation/Y01DamageNumberPoolComponent.cpp` | 修改 | 飘字三色选择 |
| `Source/Y01Tests/Private/Playground/Y01R36UI.h/.cpp` | 修改 | TopVitals 拆金币文本 + 血条 + 护盾条 |
| `Source/Y01Tests/Private/Playground/Y01R37CritShieldTests.cpp` | 新建 | 定向测试 |
| 若干测试断言文件 | 修改 | 血条改造的既有文本断言更新 |

---

### Task 1: 数据契约与校验

**Files:**
- Modify: `Source/Y01/Data/Y01DataTypes.h`
- Modify: `Source/Y01/Player/Y01SourceRuntimeTypes.h`
- Modify: `Source/Y01/Player/Y01ItemRuntimeTypes.h`
- Modify: `Source/Y01/Data/Y01DataCatalog.cpp`

- [ ] **Step 1.1: 枚举与行字段**

`Y01DataTypes.h`：
```cpp
enum class EY01SourceEffectKind : uint8
{ AttackFlat = 0, GoldPerSecond = 1, AttributeFlat = 2, MappingCoefficientAdd = 3, AttributePercent = 4,
  CritChance = 5, CritMultiplierAdd = 6, Lifesteal = 7, ShieldPeriodic = 8, ShieldOnCast = 9 };
```
`FY01SourceEffectRow` 追加（保持既有成员不动）：
```cpp
UPROPERTY(EditAnywhere, BlueprintReadOnly) double DurationSeconds = 0;
UPROPERTY(EditAnywhere, BlueprintReadOnly) double LayerCap = 0;
UPROPERTY(EditAnywhere, BlueprintReadOnly) double IntervalSeconds = 0;
```
`FY01DamageEffectRow` 追加：`UPROPERTY(EditAnywhere, BlueprintReadOnly) bool bLifesteal = false;`
`FY01TestSkillConfig` 追加：`UPROPERTY(EditAnywhere, BlueprintReadOnly) bool bLifesteal = true;`（并把该字段加入 IsValid 无需校验；bool 恒合法）
`FY01MatchRulesRow` 追加：`UPROPERTY(EditAnywhere, BlueprintReadOnly) double DefaultCritMultiplier = 2;`
`FY01MatchPresentation` 追加：`UPROPERTY(EditAnywhere, BlueprintReadOnly) FLinearColor CriticalColor = FLinearColor(1.f, .45f, 0.f, 1.f);`

- [ ] **Step 1.2: ValidSourceEffect 新种类**

`Y01DataTypes.h` 的 `ValidSourceEffect` switch 追加（放在 `case MappingCoefficientAdd` 之后）：
```cpp
case EY01SourceEffectKind::CritChance:
case EY01SourceEffectKind::CritMultiplierAdd:
case EY01SourceEffectKind::Lifesteal:
    return E.Magnitude == 0 && E.SourceAttribute == EY01Attribute::None && E.TargetAttribute == EY01Attribute::None
        && E.DurationSeconds == 0 && E.LayerCap == 0 && E.IntervalSeconds == 0;
case EY01SourceEffectKind::ShieldPeriodic:
    return E.Magnitude == 0 && E.SourceAttribute == EY01Attribute::None && E.TargetAttribute == EY01Attribute::None
        && E.Amount > 0 && E.DurationSeconds > 0 && E.LayerCap > 0 && E.IntervalSeconds > 0;
case EY01SourceEffectKind::ShieldOnCast:
    return E.Magnitude == 0 && E.SourceAttribute == EY01Attribute::None && E.TargetAttribute == EY01Attribute::None
        && E.Amount > 0 && E.DurationSeconds > 0 && E.LayerCap > 0 && E.IntervalSeconds == 0;
```
（Condition==None 时 ConditionThreshold 已被既有全局检查强制为0；新种类同样受"Condition 只能挂 AttributeFlat/AttributePercent"约束，无需额外处理。）

- [ ] **Step 1.3: 运行时结构透传**

`Y01SourceRuntimeTypes.h`：`FY01SourceEffectSnapshot` 与 `FY01ResolvedSourceEffect` 末尾各追加：
```cpp
double DurationSeconds = 0;
double LayerCap = 0;
double IntervalSeconds = 0;
```
`Y01ItemRuntimeTypes.h`：`FY01ItemEffect` 末尾追加同样三个字段。

- [ ] **Step 1.4: 更新全部位置式聚合拷贝点**

Grep `bGrowthContribution` 找出所有 `{...Effect.bGrowthContribution}` 形式的位置初始化（已知6处：`Y01SourceEffectsComponent.cpp` 约336行与565行、`Y01GrowthComponent.cpp` 约176行、`Y01LoadoutComponent.cpp` 约368与543行、`Y01EconomyComponent.cpp` 约495行），每处在 `bGrowthContribution` 实参后追加对应行/效果结构体的三个新字段（行结构体用 `Row.DurationSeconds, Row.LayerCap, Row.IntervalSeconds`，效果结构体用 `Effect.DurationSeconds, ...`，快照来源用 `E.`）。若发现6处之外的同类聚合，一并补齐。

- [ ] **Step 1.5: DataCatalog 校验**

`Y01DataCatalog.cpp`：
1. `SourceEffects`（约110行）的"仅装备"检查改为：
```cpp
Check(Source.Type == EKind::Equipment || Effect->Kind == EY01SourceEffectKind::AttackFlat
    || Effect->Kind == EY01SourceEffectKind::GoldPerSecond
    || (Source.Type == EKind::Skill && Effect->Kind == EY01SourceEffectKind::ShieldOnCast),
    ECode::Invariant, Field, TEXT("新属性和映射效果首版仅接通装备；施放护盾仅接通技能"));
```
2. `Visit.operator()<FY01SourceEffectRow>`（约310行）在 `V.Number(Row.ConditionThreshold, ...)` 后追加：
```cpp
V.Number(Row.DurationSeconds, TEXT("DurationSeconds"));
V.Number(Row.LayerCap, TEXT("LayerCap"));
V.Number(Row.IntervalSeconds, TEXT("IntervalSeconds"));
```
并把 `V.Check(Y01Data::ValidSourceEffect(Row), ...)` 消息文本扩为"……护盾数值、持续时间、本层上限与间隔须为正，施放护盾间隔须为零"。
3. Grep `FY01MatchRulesRow` 找到 MatchRules 行 visit，在 Presentaion 校验附近追加：
```cpp
V.Number(Row.DefaultCritMultiplier, TEXT("DefaultCritMultiplier"));
```
4. Grep `Presentation` 的 MatchRules visit（约608行）确认 `CriticalColor` 是否需要显式校验；既有颜色校验若为逐字段枚举则补充，若为整体结构校验则无需改动（FLinearColor 恒有效）。

- [ ] **Step 1.6: 编译验证数据层**

构建（先确认无其他构建占用）：
```
"G:\UE_Engine\g01_engine\Engine\Build\BatchFiles\Build.bat" Y01Editor Win64 Development -project="G:\GarbageProject\Y01\Y01.uproject" -WaitMutex
```
预期编译通过（此时新字段尚未被消费，不应有未使用告警以外的错误）。

- [ ] **Step 1.7: Commit**

不提交（纪律：dirty main 原地）。跳过。

---

### Task 2: 属性集与结算器

**Files:**
- Modify: `Source/Y01/Combat/Y01Combat.h/.cpp`
- Modify: `Source/Y01/Player/Y01AttributeResolver.h/.cpp`
- Modify: `Source/Y01/Player/Y01SourceEffectsComponent.cpp`

- [ ] **Step 2.1: AttributeSet 新属性**

`Y01Combat.h` `UY01AttributeSet` public 区（与 HealthRegen 声明并列）追加：
```cpp
UPROPERTY(ReplicatedUsing=OnRep_CritChance) FGameplayAttributeData CritChance;
UPROPERTY(ReplicatedUsing=OnRep_CritMultiplierBonus) FGameplayAttributeData CritMultiplierBonus;
UPROPERTY(ReplicatedUsing=OnRep_Lifesteal) FGameplayAttributeData Lifesteal;
GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UY01AttributeSet, CritChance)
GAMEPLAYATTRIBUTE_VALUE_GETTER(CritChance)
GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UY01AttributeSet, CritMultiplierBonus)
GAMEPLAYATTRIBUTE_VALUE_GETTER(CritMultiplierBonus)
GAMEPLAYATTRIBUTE_PROPERTY_GETTER(UY01AttributeSet, Lifesteal)
GAMEPLAYATTRIBUTE_VALUE_GETTER(Lifesteal)
```
private 追加三个 `UFUNCTION() void OnRep_...();`。`Y01Combat.cpp`：`GetLifetimeReplicatedProps` 追加三行
`DOREPLIFETIME_CONDITION_NOTIFY(UY01AttributeSet, CritChance, COND_None, REPNOTIFY_Always);`（其余同型）；
三个 OnRep 实现 `GAMEPLAYATTRIBUTE_REPNOTIFY(UY01AttributeSet, CritChance, OldValue);`；
`ClampAttribute` 追加分支：
```cpp
else if (Attribute == GetCritChanceAttribute() || Attribute == GetLifestealAttribute()) { Value = FMath::Clamp(Value, 0.f, 1.f); }
else if (Attribute == GetCritMultiplierBonusAttribute()) { Value = FMath::Max(0.f, Value); }
```
（CritChance 上限放 Cl 外层合计已封顶；此处兜底。）

- [ ] **Step 2.2: 结算器输出**

`Y01AttributeResolver.h` `FY01ResolvedAttributes` 追加：
```cpp
double CritChance = 0;          // 0-1，合计/100封顶1
double CritMultiplierBonus = 0; // 倍率值直接相加
double Lifesteal = 0;           // 0-1
```
`Y01AttributeResolver.cpp` `Resolve`：在 `switch (E.Kind)` 前插入新种类旁路（必须在 `Result.Breakdowns[Index - 1]` 使用之前 return/continue，避免 None 目标越界）：
```cpp
double CritPoints = 0, StealPoints = 0;
```
（函数局部，位于 Sorted 循环前。）switch 前加：
```cpp
if (E.Kind == K::ShieldPeriodic || E.Kind == K::ShieldOnCast) { continue; }
if (E.Kind == K::CritChance || E.Kind == K::CritMultiplierAdd || E.Kind == K::Lifesteal)
{
    Entry.Contribution = Entry.bActive ? Entry.Amount : 0;
    if (E.Kind == K::CritMultiplierAdd) { if (!Add(CritBonus, Entry.Contribution)) { return Fail(); } }
    else if (E.Kind == K::CritChance) { if (!Add(CritPoints, Entry.Contribution)) { return Fail(); } }
    else { if (!Add(StealPoints, Entry.Contribution)) { return Fail(); } }
    continue;
}
```
其中 `double CritBonus = 0;` 与 CritPoints/StealPoints 同处声明。函数末尾（`Result.GoldPerSecond = ...` 前）：
```cpp
Result.CritChance = FMath::Clamp(CritPoints / 100., 0., 1.);
Result.CritMultiplierBonus = CritBonus;
Result.Lifesteal = StealPoints / 100.;
if (!FMath::IsFinite(Result.CritMultiplierBonus) || (CritBonus > 0 && Result.CritMultiplierBonus == 0)
    || !FMath::IsFinite(Result.Lifesteal) || (StealPoints > 0 && Result.Lifesteal == 0)) { return Fail(); }
```
`Describe`（`Y01AttributeResolver.cpp` 底部）追加：
```cpp
case EY01SourceEffectKind::CritChance: Text = FString::Printf(TEXT("暴击率 +%g%%"), E.Amount); break;
case EY01SourceEffectKind::CritMultiplierAdd: Text = FString::Printf(TEXT("暴击倍率 +%g"), E.Amount); break;
case EY01SourceEffectKind::Lifesteal: Text = FString::Printf(TEXT("吸血 +%g%%"), E.Amount); break;
case EY01SourceEffectKind::ShieldPeriodic:
    Text = FString::Printf(TEXT("每%g秒获得%g护盾，持续%g秒，本层上限%g"), E.IntervalSeconds, E.Amount, E.DurationSeconds, E.LayerCap); break;
case EY01SourceEffectKind::ShieldOnCast:
    Text = FString::Printf(TEXT("施放时获得%g护盾，持续%g秒，本层上限%g"), E.Amount, E.DurationSeconds, E.LayerCap); break;
```

- [ ] **Step 2.3: 提交链写 base**

`Y01SourceEffectsComponent.cpp` `CommitMutationAt` 的 `FScopedAggregatorOnDirtyBatch` 块（约483-489行）追加：
```cpp
CommittedASC->SetNumericAttributeBase(UY01AttributeSet::GetCritChanceAttribute(), CommittedAttributes.CritChance);
CommittedASC->SetNumericAttributeBase(UY01AttributeSet::GetCritMultiplierBonusAttribute(), CommittedAttributes.CritMultiplierBonus);
CommittedASC->SetNumericAttributeBase(UY01AttributeSet::GetLifestealAttribute(), CommittedAttributes.Lifesteal);
```
（`InitializeProfile` 首次提交与后续批量变更都经 `CommitMutationAt`，无需其他写入点。）

- [ ] **Step 2.4: 编译验证**

同 Task 1 构建命令。预期通过。

---

### Task 3: 共享伤害管线与三执行器

**Files:**
- Create: `Source/Y01/Combat/Y01DamagePipeline.h`、`Source/Y01/Combat/Y01DamagePipeline.cpp`
- Modify: `Source/Y01/Y01.Build.cs`（如 Source 按目录无需改动则跳过；先检查 Build.cs 是否按文件列举——本项目默认按模块通配，通常无需改）
- Modify: `Source/Y01/Combat/Y01Combat.h/.cpp`
- Modify: `Source/Y01/Combat/Y01DamageEffects.cpp`
- Modify: `Source/Y01/Player/Y01DamageEffectsComponent.h/.cpp`
- Modify: `Source/Y01Tests/Private/Playground/Y01TestSkill.h/.cpp`
- Modify: `Source/Y01/Match/Y01GameFlow.h/.cpp`

- [ ] **Step 3.1: GameMode 默认暴击倍率**

`Y01GameFlow.h` `AY01GameMode` public 追加：
```cpp
double GetDefaultCritMultiplier() const { return DefaultCritMultiplier; }
```
private：`double DefaultCritMultiplier = 2;`
`Y01GameFlow.cpp` Configure（约221行）：
```cpp
if (const auto* Rules = Catalog.FindRow<FY01MatchRulesRow>(RulesRef, PresentationErrors))
{
    GetFlowState()->Presentation.Values = Rules->Presentation;
    DefaultCritMultiplier = Rules->DefaultCritMultiplier;
}
```

- [ ] **Step 3.2: 新建管线**

`Source/Y01/Combat/Y01DamagePipeline.h`：
```cpp
#pragma once
#include "CoreMinimal.h"
#include "Combat/Y01Combat.h"

struct Y01_API FY01DamageResolve
{
    bool bValid = false;
    bool bCritical = false;
    float AfterArmor = 0;    // 扣甲后，飘字展示值
    float HealthLoss = 0;    // 实际生命损失（护盾吸收后）
    float ShieldAbsorbed = 0;
};
// 三执行器共享：基础 → ×暴击 → 减甲(下限0) → 护盾吸收(仅玩家) → 扣血(不超当前)。
// 吸血按实际生命损失在 AttributeSet 提交链内应用，不在此处。
class Y01_API Y01DamagePipeline
{
public:
    static bool RollCritical(const UY01CombatAbilitySystem* Source);
    static FY01DamageResolve Resolve(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target,
        float BaseDamage, bool bCritical);
};
```
`Source/Y01/Combat/Y01DamagePipeline.cpp`：
```cpp
#include "Combat/Y01DamagePipeline.h"
#include "Player/Y01PlayerState.h"
#include "Match/Y01GameFlow.h"

bool Y01DamagePipeline::RollCritical(const UY01CombatAbilitySystem* Source)
{
    if (!Source || !Source->GetOwner() || !Source->GetOwner()->HasAuthority()) { return false; }
    const float Chance = Source->GetNumericAttribute(UY01AttributeSet::GetCritChanceAttribute());
    return FMath::IsFinite(Chance) && Chance > 0 && FMath::FRand() < Chance;
}
FY01DamageResolve Y01DamagePipeline::Resolve(const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target,
    float BaseDamage, bool bCritical)
{
    FY01DamageResolve R;
    if (!Source || !Target || !FMath::IsFinite(BaseDamage) || BaseDamage < 0) { return R; }
    float Base = BaseDamage;
    if (bCritical)
    {
        double Multiplier = 2.;
        if (const auto* Mode = Source->GetWorld() ? Source->GetWorld()->GetAuthGameMode<AY01GameMode>() : nullptr)
        { Multiplier = Mode->GetDefaultCritMultiplier(); }
        const float Bonus = Source->GetNumericAttribute(UY01AttributeSet::GetCritMultiplierBonusAttribute());
        Base = static_cast<float>(FMath::Clamp(static_cast<double>(Base) * (Multiplier + Bonus), 0., static_cast<double>(MAX_flt)));
    }
    const float Armor = Target->GetNumericAttribute(UY01AttributeSet::GetArmorAttribute());
    R.bCritical = bCritical;
    R.AfterArmor = FMath::Max(0.f, Base - (FMath::IsFinite(Armor) ? FMath::Max(0.f, Armor) : 0.f));
    float ReachHealth = R.AfterArmor;
    if (auto* TargetPlayer = Cast<AY01PlayerState>(Target->GetOwner()))
    {
        if (auto* Shield = TargetPlayer->GetShield())
        {
            const double Total = Shield->GetTotal();
            if (Total > 0)
            {
                ReachHealth = R.AfterArmor - static_cast<float>(FMath::Min(Total, static_cast<double>(R.AfterArmor)));
                R.ShieldAbsorbed = R.AfterArmor - ReachHealth;
                if (R.ShieldAbsorbed > 0) { Shield->Consume(R.ShieldAbsorbed); }
            }
        }
    }
    const float Health = Target->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
    R.HealthLoss = FMath::IsFinite(Health) && Health > 0 ? FMath::Min(ReachHealth, Health) : 0.f;
    R.bValid = true;
    return R;
}
```
注意：护盾在 Resolve 内扣减是安全的，因为三个执行器都以 `IsLegalCombatTarget`/`IsValidCombatTarget` 为前置，而它已含 `IsCombatPairAllowed`（`Y01Combat.cpp:670`），`PreGameplayEffectExecute` 的唯一拒绝路径不会在这些 spec 上触发。**执行时若发现新的拒绝路径，必须先停下报告。**

- [ ] **Step 3.3: 新 Tag**

`Y01Combat.h` `Y01Tags` 追加：
```cpp
Y01_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_Crit);
Y01_API UE_DECLARE_GAMEPLAY_TAG_EXTERN(Data_LifestealAllowed);
```
`Y01Combat.cpp` 追加：
```cpp
UE_DEFINE_GAMEPLAY_TAG(Data_Crit, "Data.Crit");
UE_DEFINE_GAMEPLAY_TAG(Data_LifestealAllowed, "Data.LifestealAllowed");
```

- [ ] **Step 3.4: 执行器1（Data_Damage / 普攻与怪物攻击）**

`UY01BasicAttackAbility::ExecuteSkill`（`Y01Combat.cpp` 约249行）在 `SetSetByCallerMagnitude(Data_Damage, ...)` 后追加：
```cpp
Spec.Data->SetSetByCallerMagnitude(Y01Tags::Data_Crit, Y01DamagePipeline::RollCritical(ASC) ? 1.f : 0.f);
```
`UY01DamageExecution::Execute_Implementation`（约187行）替换 `Damage` 计算与输出为：
```cpp
const bool bCritical = Parameters.GetOwningSpec().GetSetByCallerMagnitude(Y01Tags::Data_Crit, false, 0.f) > 0.f;
const auto Result = Y01DamagePipeline::Resolve(Source, Target, Snapshot, bCritical);
if (Result.bValid && Result.HealthLoss > 0)
{
    if (auto* Spec = Parameters.GetOwningSpecForPreExecuteMod())
    {
        Spec->SetSetByCallerMagnitude(Y01Tags::Data_DamageDisplay, Result.AfterArmor);
        Spec->SetSetByCallerMagnitude(Y01Tags::Data_LifestealAllowed, 1.f); // 普攻恒可吸血
    }
    Output.AddOutputModifier(FGameplayModifierEvaluatedData(UY01AttributeSet::GetHealthAttribute(),
        EGameplayModOp::Additive, -Result.HealthLoss));
}
```
（原 `Damage/Health` 局部变量与 Min 逻辑删除。）

- [ ] **Step 3.5: 事件暴击标记、护盾补偿校验与吸血（AttributeSet 管线）**

`Y01DamageEvents.h` `FY01DamageEvent` 追加：`UPROPERTY() bool bCritical = false;`（纯值结构经 ClientDamageBatch 传输，服务器与客户端必须同构建，本期一起上）。
`Y01Combat.h` `FHealthExecution` 追加：
```cpp
bool bLifestealAllowed = false;
TWeakObjectPtr<UY01CombatAbilitySystem> LifestealSource;
float LifestealPending = 0;
```
`PreGameplayEffectExecute` 的 `bDamage` 分支（约110-114行 DisplayAmount 赋值后）追加：
```cpp
Execution.bLifestealAllowed = Data.EffectSpec.GetSetByCallerMagnitude(Y01Tags::Data_LifestealAllowed, false, 0.f) > 0.f;
Execution.Pending.Event.bCritical = Data.EffectSpec.GetSetByCallerMagnitude(Y01Tags::Data_Crit, false, 0.f) > 0.f;
Execution.LifestealSource = Cast<UY01CombatAbilitySystem>(Data.EffectSpec.GetContext().GetOriginalInstigatorAbilitySystemComponent());
```
`PostAttributeChange` 冻结块（约141-153行）在 `Execution.Pending.Event.DisplayAmount = FMath::Max(...)` 后追加吸血冻结：
```cpp
if (Execution.bLifestealAllowed && Execution.LifestealSource.IsValid())
{
    const float Steal = Execution.LifestealSource->GetNumericAttribute(UY01AttributeSet::GetLifestealAttribute());
    if (FMath::IsFinite(Steal) && Steal > 0) { Execution.LifestealPending = Actual * Steal; }
}
```
`PostGameplayEffectExecute`（约121-134行）在 `Completed.Service->Commit(...)` 之后追加：
```cpp
if (Completed.LifestealPending > 0 && Completed.LifestealSource.IsValid())
{ Completed.LifestealSource.Get()->ApplyHealingTo(Completed.LifestealSource.Get(), Completed.LifestealPending); }
```
（吸血=实际生命损失×吸血%，超上限由 Health 钳制；绿色飘字经既有治疗事件路径；护盾不可被回复——治疗 GE 只触 Health。）
说明：护盾扣减已在管线内完成；`DisplayAmount - Actual` 即吸收量，不再二次扣减。

- [ ] **Step 3.6: 执行器2（模板伤害 / 装备与技能伤害效果）**

`Y01DamageEffectsComponent.h`：`FProduct` 追加 `bool bCritical = false;`；`FActiveCast` 追加 `bool bCritical = false;`；`Hit` 签名改为
`void Hit(uint64 Execution, uint64 Beat, AActor* Target, float Damage, bool bCritical, bool bLifesteal);`
`ConsumeAuthorization` 签名改为
`bool ConsumeAuthorization(const FGameplayEffectSpec& Spec, const UY01CombatAbilitySystem* Source, const UY01CombatAbilitySystem* Target, float& Damage, bool& bCritical, bool& bLifesteal);`
private 成员追加：`bool AuthorizedCritical = false; bool AuthorizedLifesteal = false;`

`Y01DamageEffectsComponent.cpp`：
1. `Launch`（约191行）开头快照时刻判定一次：
```cpp
const bool bCritical = Y01DamagePipeline::RollCritical(Player()->GetCombatASC());
UE_LOG(... // 原日志追加 bCritical
```
`IndependentArea` 分支：`P.bCritical = bCritical;`；`Launch` 的 Instant 分支调用改为 `Hit(Execution,0,Target.Get(),Snapshot,bCritical,R.bLifesteal);`（`R` 为 `Source.Effect.Config`）；挂身/飞行物产物 `P.bCritical = bCritical;`。
2. `SynchronizeCommitted` 的 BoundArea 产物创建处（约90行）追加 `Product.bCritical = Y01DamagePipeline::RollCritical(Player()->GetCombatASC());`。
3. `ExecuteActive`/`PrepareActive`：`FActiveCast C; ... C.bCritical = Y01DamagePipeline::RollCritical(...);` 并传给 `Launch`——为此 `Launch` 增加参数 `bool bCritical`（快照传入，Instant/DOT/产物统一用它，不再内部掷点）。调整：`Launch(const FSource& Source, const FY01DamageSelection& Selected, float Snapshot, double Time, bool bCritical)`；周期路径在 `Update` 的 `Launch(Source,Selected,Snapshot,Time)` 处补 `Y01DamagePipeline::RollCritical(...)` 实参。
4. `Advance` 每跳（约285行）：`Hit(Id,BeatSerial,Target,Damage, Copy.bCritical, Copy.Effect.Config.bLifesteal);`（用 Copy 的产物暴击，DOT 沿用放置时判定）。飞行物命中（约253行）同。
5. `Hit`（约289行）签名扩展，授权记录存 `AuthorizedCritical/AuthorizedLifesteal`。
6. `ConsumeAuthorization`（约305行）返回三项。
`Y01DamageEffects.cpp` `UY01TemplateDamageExecution::Execute_Implementation`（约176行）替换为管线：
```cpp
float Base = 0; bool bCritical = false; bool bLifesteal = false;
if (!Manager || !Manager->ConsumeAuthorization(Parameters.GetOwningSpec(), Source, Target, Base, bCritical, bLifesteal)) { return; }
const auto Result = Y01DamagePipeline::Resolve(Source, Target, Base, bCritical);
if (Result.bValid && Result.HealthLoss > 0)
{
    if (auto* Spec = Parameters.GetOwningSpecForPreExecuteMod())
    {
        Spec->SetSetByCallerMagnitude(Y01Tags::Data_DamageDisplay, Result.AfterArmor);
        Spec->SetSetByCallerMagnitude(Y01Tags::Data_LifestealAllowed, bLifesteal ? 1.f : 0.f);
    }
    Output.AddOutputModifier(FGameplayModifierEvaluatedData(UY01AttributeSet::GetHealthAttribute(),
        EGameplayModOp::Additive, -Result.HealthLoss));
}
```

- [ ] **Step 3.7: 执行器3（测试技能）**

`Y01TestSkill.h` `FY01TestAttackState` 追加：`UPROPERTY() bool bCritical = false;`（bLifesteal 从 Config 读取，不复制）。
`Y01TestSkill.cpp`：
1. `ActivateState` 中 `State.DamageSnapshot = Damage;` 附近追加 `State.bCritical = Y01DamagePipeline::RollCritical(ASC);`。
2. `UY01TestSkillDamageExecution::Execute_Implementation`（约129行）替换为管线：
```cpp
const auto& Config = Attack->GetState().Config;
const auto Result = Y01DamagePipeline::Resolve(Source, Target, Attack->GetState().DamageSnapshot, Attack->GetState().bCritical);
if (Result.bValid && Result.HealthLoss > 0)
{
    if (auto* Spec = Parameters.GetOwningSpecForPreExecuteMod())
    {
        Spec->SetSetByCallerMagnitude(Y01Tags::Data_DamageDisplay, Result.AfterArmor);
        Spec->SetSetByCallerMagnitude(Y01Tags::Data_LifestealAllowed, Config.bLifesteal ? 1.f : 0.f);
    }
    Output.AddOutputModifier(FGameplayModifierEvaluatedData(UY01AttributeSet::GetHealthAttribute(),
        EGameplayModOp::Additive, -Result.HealthLoss));
}
```

- [ ] **Step 3.8: 施放护盾钩子**

`UY01DamageAbility::ExecuteSkill`（`Y01DamageEffects.cpp` 约157行）`CommitAbility` 成功后追加：
```cpp
if (auto* Shield = P->GetShield())
{
    const auto* Grant = P->GetCombatASC()->FindGrantedAbility(Handle);
    if (Grant && Grant->LearnedId.IsValid())
    { Shield->NotifyCast(P->GetLoadout()->SourceId(Grant->LearnedId, EY01RuntimeSourceKind::Skill).Id); }
}
```
`UY01TestSkillAbility::ExecuteSkill`（`Y01TestSkill.cpp`）同样在创建攻击前追加（SourceId 用 `ASC->GetOwner()` 的 PlayerState 组件）。`NotifyCast` 见 Task 4。

- [ ] **Step 3.9: 飘字暴击色**

`Y01DamageNumberPoolComponent.cpp` 81行改为：
```cpp
const FLinearColor Color = Event.Type == EY01DamageEventType::Healing ? Presentation.HealingColor
    : Event.bCritical ? Presentation.CriticalColor : Presentation.DamageColor;
Entry.Widget->SetNumber(Event.DisplayAmount, Color);
```

- [ ] **Step 3.10: 编译验证**

构建命令同上。预期通过；修复一切编译错误后进入 Task 4。

---

### Task 4: 护盾组件

**Files:**
- Create: `Source/Y01/Player/Y01ShieldComponent.h`、`Source/Y01/Player/Y01ShieldComponent.cpp`
- Modify: `Source/Y01/Player/Y01PlayerState.h/.cpp`
- Modify: `Source/Y01/Player/Y01SourceEffectsComponent.h`（friend）

- [ ] **Step 4.1: 组件头文件**

`Source/Y01/Player/Y01ShieldComponent.h`：
```cpp
#pragma once
#include "Components/ActorComponent.h"
#include "Player/Y01SourceRuntimeTypes.h"
#include "Y01ShieldComponent.generated.h"

class AY01PlayerState;

// 服务器唯一真源：按来源分层护盾；仅复制总量供血条。同来源累加至层上限并刷新时长；
// 受伤先扣最快到期层；死亡/复活/离局清空。不计入生命条件、不可被吸血回复（治疗只触Health）。
UCLASS(ClassGroup=(Y01), meta=(BlueprintSpawnableComponent))
class Y01_API UY01ShieldComponent : public UActorComponent
{
    GENERATED_BODY()
public:
    UY01ShieldComponent();
    void ResetLayers();
    void GrantLayer(const FString& LayerKey, double Value, double LayerCap, double DurationSeconds);
    void Consume(double Amount);
    double GetTotal() const { return TotalShield; }
    // ShieldOnCast 入口：LayerKey 须形如 SourceId/EffectId。
    void NotifyCast(const FString& CastSourceId);
    virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
private:
    struct FLayer { FString Key; double Value = 0; double Cap = 0; double Expires = 0; };
    UPROPERTY(ReplicatedUsing=OnRep_TotalShield) double TotalShield = 0;
    TArray<FLayer> Layers;
    TMap<FString, double> NextPeriodic;   // ShieldPeriodic 来源下次获得时刻
    void RecomputeTotal();
    UFUNCTION() void OnRep_TotalShield();
    AY01PlayerState* Player() const;
};
```

- [ ] **Step 4.2: 组件实现**

`Source/Y01/Player/Y01ShieldComponent.cpp`：
```cpp
#include "Player/Y01ShieldComponent.h"
#include "Player/Y01PlayerState.h"
#include "Player/Y01SourceEffectsComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"

UY01ShieldComponent::UY01ShieldComponent()
{ PrimaryComponentTick.bCanEverTick = true; PrimaryComponentTick.TickInterval = .1f; SetIsReplicatedByDefault(false); }
AY01PlayerState* UY01ShieldComponent::Player() const { return Cast<AY01PlayerState>(GetOwner()); }
void UY01ShieldComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{ Super::GetLifetimeReplicatedProps(OutLifetimeProps); DOREPLIFETIME(UY01ShieldComponent, TotalShield); }
void UY01ShieldComponent::OnRep_TotalShield() {}
void UY01ShieldComponent::RecomputeTotal()
{
    double Total = 0;
    for (const auto& L : Layers) { Total += L.Value; }
    TotalShield = FMath::IsFinite(Total) && Total > 0 ? Total : 0;
}
void UY01ShieldComponent::ResetLayers()
{
    if (!GetOwner() || !GetOwner()->HasAuthority()) { return; }
    Layers.Reset(); NextPeriodic.Reset(); RecomputeTotal();
}
void UY01ShieldComponent::GrantLayer(const FString& LayerKey, double Value, double LayerCap, double DurationSeconds)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !FMath::IsFinite(Value) || Value <= 0
        || !FMath::IsFinite(LayerCap) || LayerCap <= 0 || !FMath::IsFinite(DurationSeconds) || DurationSeconds <= 0) { return; }
    const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.;
    FLayer* Existing = Layers.FindByPredicate([&](const FLayer& L) { return L.Key == LayerKey; });
    if (Existing)
    {
        Existing->Value = FMath::Min(LayerCap, Existing->Value + Value);
        Existing->Expires = Now + DurationSeconds;
    }
    else { Layers.Add({LayerKey, FMath::Min(Value, LayerCap), LayerCap, Now + DurationSeconds}); }
    RecomputeTotal();
}
void UY01ShieldComponent::Consume(double Amount)
{
    if (!GetOwner() || !GetOwner()->HasAuthority() || !FMath::IsFinite(Amount) || Amount <= 0 || Layers.IsEmpty()) { return; }
    double Left = Amount;
    while (Left > 0)
    {
        int32 Soonest = INDEX_NONE;
        for (int32 I = 0; I < Layers.Num(); ++I)
        { if (Layers[I].Value > 0 && (Soonest == INDEX_NONE || Layers[I].Expires < Layers[Soonest].Expires)) { Soonest = I; } }
        if (Soonest == INDEX_NONE) { break; }
        const double Take = FMath::Min(Left, Layers[Soonest].Value);
        Layers[Soonest].Value -= Take; Left -= Take;
    }
    Layers.RemoveAll([](const FLayer& L) { return L.Value <= 0; });
    RecomputeTotal();
}
void UY01ShieldComponent::NotifyCast(const FString& CastSourceId)
{
    auto* P = Player();
    if (!GetOwner() || !GetOwner()->HasAuthority() || !P || CastSourceId.IsEmpty()) { return; }
    for (const auto& Record : P->GetSourceEffects()->Records)
    {
        if (Record.Source.Id != CastSourceId) { continue; }
        for (const auto& E : Record.Effects)
        {
            if (E.Kind == EY01SourceEffectKind::ShieldOnCast)
            { GrantLayer(CastSourceId + TEXT("/") + E.EffectId.ToString(), E.Amount, E.LayerCap, E.DurationSeconds); }
        }
    }
}
void UY01ShieldComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    auto* P = Player();
    if (!P || !P->HasAuthority()) { return; }
    const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.;
    const int32 Before = Layers.Num();
    Layers.RemoveAll([&](const FLayer& L) { return L.Expires <= Now; });
    // 周期来源同步：新提交立即获得首层，之后按各自间隔。
    TSet<FString> Live;
    for (const auto& Record : P->GetSourceEffects()->Records)
    {
        for (const auto& E : Record.Effects)
        {
            if (E.Kind != EY01SourceEffectKind::ShieldPeriodic) { continue; }
            const FString Key = Record.Source.Id + TEXT("/") + E.EffectId.ToString();
            Live.Add(Key);
            const double* Next = NextPeriodic.Find(Key);
            if (!Next)
            {
                GrantLayer(Key, E.Amount, E.LayerCap, E.DurationSeconds);
                NextPeriodic.Add(Key, Now + E.IntervalSeconds);
            }
            else if (Now >= *Next)
            {
                GrantLayer(Key, E.Amount, E.LayerCap, E.DurationSeconds);
                NextPeriodic[Key] = *Next + E.IntervalSeconds * FMath::FloorToDouble((Now - *Next) / E.IntervalSeconds + 1.);
            }
        }
    }
    for (auto It = NextPeriodic.CreateIterator(); It; ++It) { if (!Live.Contains(It.Key())) { It.RemoveCurrent(); } }
    if (Before != Layers.Num() || !Live.IsEmpty()) { RecomputeTotal(); }
}
```
`Y01SourceEffectsComponent.h` private friend 列表追加：`friend class UY01ShieldComponent;`

- [ ] **Step 4.3: PlayerState 挂载与清空点**

`Y01PlayerState.h`：前置声明 `class UY01ShieldComponent;`；public getter `UY01ShieldComponent* GetShield() const { return Shield; }`；private `UPROPERTY(VisibleAnywhere) TObjectPtr<UY01ShieldComponent> Shield;`
`Y01PlayerState.cpp` 构造函数（与其他组件 CreateDefaultSubobject 并列）：`Shield = CreateDefaultSubobject<UY01ShieldComponent>(TEXT("Shield"));`
`TryInitializeCharacter` 成功路径（初始化属性后）追加 `Shield->ResetLayers();`
`Y01Combat.cpp` `PostAttributeChange` 玩家死亡分支（约169-173行 `Player->GetRevive()->RecordDeath();` 处）追加：
```cpp
if (auto* TargetPlayer = Cast<AY01PlayerState>(GetOwningActor())) { TargetPlayer->GetShield()->ResetLayers(); }
```
（复活后护盾保持空直到再次获得；离局=PlayerState 销毁随之析构。）
`Y01Combat.cpp` 头部 include `Player/Y01ShieldComponent.h`。

- [ ] **Step 4.4: 编译验证**

构建同上。

---

### Task 5: 左上角血条 HUD

**Files:**
- Modify: `Source/Y01Tests/Private/Playground/Y01R36UI.h/.cpp`
- Modify: 测试断言文件（见 Step 5.3）

- [ ] **Step 5.1: 控件重建**

`Y01R36UI.cpp` `RebuildWidget`（约298-305行）替换 TopVitals/ReviveCountdown 创建为：
```cpp
TopVitals = MakeText(WidgetTree, TEXT("TopVitals"), TEXT("等待服务器"), 20);
TopVitals->SetColorAndOpacity(FSlateColor(FLinearColor(1.f,.88f,.32f,1.f)));
Position(Canvas, TopVitals, {16,12}, {410,24});
auto MakeBar = [&](const TCHAR* Name, FLinearColor Fill, int32 Z)
{
    auto* Bar = WidgetTree->ConstructWidget<UProgressBar>(UProgressBar::StaticClass(), Name);
    Bar->SetFillColorAndOpacity(Fill);
    Bar->WidgetStyle.BackgroundImage.TintColor = FSlateColor(FLinearColor(0,0,0,.55f));
    Bar->SetVisibility(ESlateVisibility::HitTestInvisible);
    Position(Canvas, Bar, {16,44}, {410,20}, Z);
    return Bar;
};
HealthBar = MakeBar(TEXT("HealthBar"), FLinearColor(.85f,.1f,.1f,1), 1);
ShieldBar = MakeBar(TEXT("ShieldBar"), FLinearColor(.25f,.55f,1.f,.65f), 2);
ShieldBar->SetPercent(0);
ReviveCountdown = MakeText(WidgetTree, TEXT("ReviveCountdown"), TEXT(""), 18);
ReviveCountdown->SetAutoWrapText(false);
ReviveCountdown->SetColorAndOpacity(FSlateColor(FLinearColor(1.f,.88f,.32f,1.f)));
Position(Canvas, ReviveCountdown, {16,68}, {410,24}, 20);
ReviveCountdown->SetVisibility(ESlateVisibility::Collapsed);
```
`Y01R36UI.h` `UY01R36Panel` 成员：`TopVitals` 类型保持 `TObjectPtr<UTextBlock>`（语义改为金币文本）；追加
```cpp
UPROPERTY() TObjectPtr<UProgressBar> HealthBar;
UPROPERTY() TObjectPtr<UProgressBar> ShieldBar;
```
（若 `Position` 辅助不带 ZOrder 参数，则检查其签名并沿用 5 参重载——ReviveCountdown 已用第5参20，说明存在。）

- [ ] **Step 5.2: 刷新逻辑**

`Refresh`（约613-614行）把 `if (bRefreshStatic) { TopVitals->SetText(...金币...生命...) }` 改为：
```cpp
TopVitals->SetText(FText::FromString(FString::Printf(TEXT("金币 %d"), V.Gold)));
{
    const double Max = FMath::Max(1e-6, static_cast<double>(S.Health));
    HealthBar->SetPercent(FMath::Clamp(static_cast<float>(V.Health / Max), 0.f, 1.f));
    double ShieldTotal = 0;
    if (const auto* PC = Cast<AY01R36Controller>(GetOwningPlayer()))
    { if (const auto* P = PC->GetPlayerState<AY01PlayerState>()) { ShieldTotal = P->GetShield() ? P->GetShield()->GetTotal() : 0.; } }
    ShieldBar->SetPercent(ShieldTotal > 0 ? FMath::Clamp(static_cast<float>(ShieldTotal / Max), 0.f, 1.f) : 0.f);
    ShieldBar->SetVisibility(ShieldTotal > 0 ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}
```
（Refresh 每帧执行，血条随复制值实时刷新；金币文本不再等 bRefreshStatic。V.Health 与 S.Health 为既有视图字段。）

- [ ] **Step 5.3: 更新既有断言**

Grep 断言点（执行时以 grep 结果为准，已知：`Y01R36UIWidgetTests.cpp` 约744、1138、1157-1161（"181/420"）、1322、1493；`Y01CoreReviveTests.cpp` 约432；`Y01R36Verification.cpp` 约422；`Y01R36VerificationTests.cpp` 约587）。模式：
- 断言"金币 X\n生命 A/B"的改为断言 TopVitals 文本=="金币 X" 且 `HealthBar->GetPercent()==A/B`（浮点用 FMath::IsNearlyEqual）。
- 断言包含"生命"子串的显示文案改为读血条百分比。
- 复活计数相关（ReviveCountdown）位置未变，断言应保持。

- [ ] **Step 5.4: 编译验证**

构建同上。

---

### Task 6: 定向测试

**Files:**
- Create: `Source/Y01Tests/Private/Playground/Y01R37CritShieldTests.cpp`

- [ ] **Step 6.1: 测试文件**

仿照 `Y01R36RuntimeTests.cpp` 的夹具模式（读该文件开头确认宏与目录常量，例如 `IMPLEMENT_SIMPLE_AUTOMATION_TEST`/项目自定义宏、内存目录构造方式），写四个测试（每个一条核心断言链，不追求全组合）：

1. `Y01.R37.Crit.BasicAttack`——内存表：角色+怪（怪护甲0、血量1000）；装备挂 `CritChance Amount=100`（必暴击）；匹配规则 DefaultCritMultiplier=2。普攻命中后断言：怪损血 == 2×攻击（护甲0），并断言伤害事件 `bCritical==true`（可用 `UY01DamageEvents::OnDamageCommitted` 或读飘字日志；若事件捕获不便，允许只断言损血值+`Presentation.CriticalColor` 配置传递）。
2. `Y01.R37.Lifesteal.BasicAttack`——装备挂 `Lifesteal Amount=50`，`bLifesteal` 默认开；玩家先扣一半血；普攻打怪实际损血10 → 玩家回血5；再打 DOT 行（`bLifesteal=false`）断言不回血。
3. `Y01.R37.Shield.Periodic`——装备挂 `ShieldPeriodic Amount=30, LayerCap=60, DurationSeconds=2, IntervalSeconds=1`；推进世界时钟（既有测试夹具的时间推进方式）断言：首层30、同源再获得累计60（封顶）、怪打玩家50时玩家生命损=20（护盾吸30）且 TotalShield 递减。
4. `Y01.R37.Shield.OnCast`——技能来源挂 `ShieldOnCast Amount=25, LayerCap=25, DurationSeconds=5`；释放该技能后玩家 TotalShield==25；死亡后清空。

所有测试必须断言完成后自清理（沿用现有夹具销毁路径）。若现有夹具不支持内存 MatchRules 行，允许通过 GameMode Configure 的目录参数注入（参考现有 R36 测试如何构造 Catalog）。

- [ ] **Step 6.2: 运行定向测试**

按记忆文档方式运行：
```
powershell -ExecutionPolicy Bypass -File Scripts/Run-Y01Automation.ps1 -Label R37P2 -Tests "Y01.R37.Crit.BasicAttack;Y01.R37.Lifesteal.BasicAttack;Y01.R37.Shield.Periodic;Y01.R37.Shield.OnCast;Y01.R36.UI" -TimeoutSeconds 150
```
（把既有血条相关 widget 测试一并带上，确认 Step 5.3 改动无回归。）报告以 `Saved/R37P2/index.json` 为准；失败先按 systematic-debugging 定位，不允许为绿灯改断言语义。

- [ ] **Step 6.3: 收尾核对**

- Grep `bGrowthContribution` 确认无遗漏聚合点未编译错误（已过编译即证）。
- Grep `GetSetByCallerMagnitude(Y01Tags::Data_Crit` 确认只有 PreGameplayEffectExecute 一处消费。
- 确认未触碰任何 `Content/**/*.uasset`（`git status --short Content/` 应与开工前一致）。

---

### Task 7: 文档同步

**Files:**
- Modify: `Docs/DT字段说明.md`
- Modify: `Docs/Y01_配表指南.md`
- Modify: `Docs/Handoffs/Y01_程序窗口续接记录.md`

- [ ] **Step 7.1: DT 字段说明**

追加/更新：DT_SourceEffect 新 Kind 5-9 的字段语义（Amount 百分点/倍率约定、护盾三字段、ShieldOnCast 仅技能）；DT_DamageEffect `bLifesteal`；DT_Skill TestSkill `bLifesteal`；DT_MatchRules `DefaultCritMultiplier`；DT_MatchRules.Presentation `CriticalColor`。

- [ ] **Step 7.2: 配表指南**

R35 唯一指南中补第二期词条小节：配表示例行（内存示例，不写正式表）、暴击判定时机表、护盾分层规则、吸血开关默认值约定（主动开/DOT关）。

- [ ] **Step 7.3: 续接记录**

在 Handoffs 程序窗口续接记录追加本期小节：改动文件清单、新约束（护盾扣减在管线内、`Data_Crit`/`Data_LifestealAllowed` SetByCaller 契约、`FY01DamageEvent.bCritical` 同构建要求、血条控件名 TopVitals/HealthBar/ShieldBar）、未完成项（正式 UI 交 COLLAB-03、正式表未加行）。
