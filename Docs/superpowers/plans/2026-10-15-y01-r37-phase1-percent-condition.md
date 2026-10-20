# Y01 R37 第一期：百分比与生命条件 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让装备和基础来源支持百分比、严格生命条件与可核对的属性来源明细，全部攻击来源统一经过现有服务器来源事务。

**Architecture:** 自写 `FY01AttributeResolver` 先放大三维，再进行换算，最后放大衍生属性；不使用 GAS 乘法或 Tag 条件。攻击只保留本来源组件的一条 Additive 派生 GE，其他属性在共同提交点后写入 GAS；生命条件使用权威生命比例，忙碌时延迟处理。已提交明细通过现有 owner-only 完整/增量视图同步，测试 UI 不反推基础值或映射系数。

**Tech Stack:** Unreal Engine 5.7.4、C++、GAS、DataTable、UMG、现有 FY01ViewSender/Receiver、PowerShell 定向 Automation。

---

## 交付边界与实施纪律

- 依据：`Docs/superpowers/specs/2026-10-15-y01-r37-extended-affixes-design.md` 第 1 节；不改动该批准设计。
- 本文是实施计划，所有 checkbox 初始未完成，不表示代码已经实现。
- 原地继续当前 dirty main；不建 worktree、不提交、不推送、不重置、不清理、不终止其他窗口的进程。
- HEAD `3f2a54d` 的 Profile 简化、Client delta、怪物穿行、飘字不重做，也不重复验收。
- 第一期开启数据契约、结算和显示；**金币获取%不影响实际授金**，消费属于第四期。
- 不加入暴击、吸血、护盾、时间成长或普攻成长；这些属于后续三期。
- 正式 UI 交 COLLAB-03 的 UI 窗口；本期只改测试 HUD。
- 不覆盖其他窗口的新调值。配表结构变更本期只用内存 DataTable 测试，不自动改写正式 `.uasset`。
- 默认一次集中 Editor 构建和必要的定向测试，不运行全量 `Y01`，不作多轮视觉量化或审查。
- 下文行号是计划编写时定位；实施以函数/结构名为准，先检查当时工作区 diff。

## 本期必须锁定的语义

| 项目 | 规则 |
| --- | --- |
| `Amount=20` | +20 个百分点；运算乘数为 `1 + 20/100` |
| 百分比叠加 | 同属性先相加再乘，不连乘 |
| 三维 | `(基础 + 固定 + 玩家成长) × (1 + 三维%)` |
| 衍生 | `(基础 + 固定 + 装备成长 + Σ有效三维×系数) × (1 + 属性%)` |
| 攻速 | 先得到次数/秒，再取倒数；不重置正在等待的普攻 CD |
| 金币/秒 | 可固定增加和三维换算；禁止 `AttributePercent -> GoldPerSecond` |
| 金币获取 | 仅百分比；`AttributeFlat -> GoldGain` 和系数映射到 GoldGain 都拒绝 |
| `GoldGainPercent` | 运行时小数，例如两条 20、30 输出 `0.5`；UI 显示 `50%` |
| 生命条件 | `HealthBelow` 用 `<`，`HealthAbove` 用 `>`；等于阈值双方都不生效 |
| 条件目标 | 攻击、护甲、生命恢复、攻击次数/秒、金币获取；禁生命上限、三维、金币/秒 |
| 条件来源 | Equipment/Base；商店、技能、天赋等继续仅允许旧 AttackFlat/GoldPerSecond |
| 生命上限变化 | 上涨不治疗；下降成功提交后才钳血，Stage/失败回滚不损血 |
| 零生命上限 | 生命比例约定为 0；不除零、不计护盾 |
| 来源明细 | 按属性输出最终值、小计、百分比合计及来源条目；未生效条件也保留条目 |

客户端只有最终 `Stats` 和物品，缺少基础三维、基础属性映射及全部 Base 来源，不能准确反推出小计。因此本期新增显示明细到现有视图，不开第二条复制通道。线格式发生变化时提升版本号，双方同版本运行，不增加兼容分支。

## 文件职责

| 文件 | 改动职责 |
| --- | --- |
| `Source/Y01/Data/Y01DataTypes.h` | 枚举、SourceEffect 条件字段及纯校验 |
| `Source/Y01/Data/Y01DataCatalog.cpp` | 字段验证与现有非装备白名单 |
| `Source/Y01/Player/Y01SourceRuntimeTypes.h` | resolved/snapshot 透传条件及成长来源标记 |
| `Source/Y01/Player/Y01ItemRuntimeTypes.h` | 物品效果尾部追加条件及成长来源标记 |
| `Source/Y01/Player/Y01AttributeResolver.h/.cpp` | 公式、数值精度、条件判定及结构化明细 |
| `Source/Y01/Player/Y01SourceEffectsComponent.h/.cpp` | 单 GE、原子提交、已提交明细及条件延迟处理 |
| `Source/Y01/Combat/Y01Combat.h/.cpp` | 移除旧攻击聚合器重排钩子；监听权威生命及动态攻击 base |
| `Source/Y01/Player/Y01LoadoutComponent.cpp` | 效果字段完整复制 |
| `Source/Y01/Player/Y01GrowthComponent.cpp` | 标记生成的装备成长攻击并完整复制 |
| `Source/Y01/Player/Y01LoadoutView.cpp` | 完整 payload 比较及已提交明细发布 |
| `Source/Y01/Player/Y01ViewTypes.h/.cpp` | 明细的 full/delta 编码、验证与版本 |
| `Source/Y01Tests/Private/Playground/Y01R36GrowthDropTests.cpp` | 现有属性夹具添加 R37 场景及纯函数测试 |
| `Source/Y01Tests/Private/Playground/Y01R36RuntimeTests.cpp` | 新单 GE 契约与失败注入迁移 |
| `Source/Y01Tests/Private/Playground/Y01R36LoadoutTests.cpp` | 交易/传输回归及版本号测试迁移 |
| `Source/Y01Tests/Private/Playground/Y01R36UI.cpp` | 词条描述、GoldGain 行和明细 tooltip |
| `Source/Y01Tests/Private/Playground/Y01R36UIWidgetTests.cpp` | 显示与行滚动回归 |
| `Docs/DT字段说明.md`、`Docs/Y01_配表指南.md`、`Docs/Handoffs/Y01_程序窗口续接记录.md` | 数据填写规则与真实验证记录 |

不新建生产组件或测试框架。明细结构放在现有 resolver 头文件，避免循环依赖。

## 命令与红绿节奏

各任务使用小步 TDD，但集中使用已有 Automation 入口，不新写验证包装器。每次代码改动后需要构建才能运行新测试；不能把旧 DLL 的通过当新代码通过。

构建前检查（只读，不 Kill）：

```powershell
Get-CimInstance Win32_Process | Where-Object { $_.Name -in @('UnrealEditor.exe','UnrealEditor-Cmd.exe','UnrealBuildTool.exe') } | Select-Object ProcessId,Name,CommandLine
```

构建命令，Label 每次换唯一值：

```powershell
& 'G:\UE_Engine\g01_engine\Engine\Build\BatchFiles\Build.bat' Y01Editor Win64 Development '-Project=G:\GarbageProject\Y01\Y01.uproject' -WaitMutex -NoHotReloadFromIDE -NoEngineChanges -NoUBA '-Log=G:\GarbageProject\Y01\Saved\R37P1-Editor-Build.log'
```

若预计构建超过工具等待上限，用独立进程而不是重复启动：

```powershell
$p = Start-Process -FilePath $env:ComSpec -ArgumentList '/d /c ""G:\UE_Engine\g01_engine\Engine\Build\BatchFiles\Build.bat" Y01Editor Win64 Development -Project="G:\GarbageProject\Y01\Y01.uproject" -WaitMutex -NoHotReloadFromIDE -NoEngineChanges -NoUBA -Log="G:\GarbageProject\Y01\Saved\R37P1-Editor-Build.log""' -PassThru
$p.Id
```

保留该 PID；后续读取已有日志/退出状态，不重复编译、不杀外部 Editor。活跃 Editor 导致目标不可写时说明阻塞，先继续不依赖构建的工作。

测试命令示例：

```powershell
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-Attribute-Green-01 -Tests Y01.Framework.R36.Attribute -TimeoutSeconds 150
```

报告：`Saved/R37P1-Attribute-Green-01/index.json`。没有报告、零测试或日志尾部未结束不算通过；若测试入口暂不存在，用编译/发现测试的失败作为红灯，并注明不是行为断言红灯。

---

## Task 1：数据契约与完整 payload 透传

**Files:** DataTypes、DataCatalog、SourceRuntimeTypes、ItemRuntimeTypes、LoadoutComponent、GrowthComponent、LoadoutView、R36GrowthDropTests。

- [ ] **Step 1：在现有 Attribute.Validation 分支先写失败断言。**

在 `Y01R36GrowthDropTests.cpp:216-237` 添加以下块（include 现有 resolver）：

```cpp
FY01SourceEffectRow P;
P.Kind = EY01SourceEffectKind::AttributePercent;
P.TargetAttribute = EY01Attribute::AttackDamage;
P.Amount = 20;
TestTrue(TEXT("允许攻击百分比"), Y01Data::ValidSourceEffect(P));
P.TargetAttribute = EY01Attribute::GoldGain;
TestTrue(TEXT("允许金币获取百分比"), Y01Data::ValidSourceEffect(P));
P.TargetAttribute = EY01Attribute::GoldPerSecond;
TestFalse(TEXT("拒绝金币每秒百分比"), Y01Data::ValidSourceEffect(P));
P.TargetAttribute = EY01Attribute::Strength;
P.Condition = EY01EffectCondition::HealthBelow;
P.ConditionThreshold = 30;
TestFalse(TEXT("条件禁止三维"), Y01Data::ValidSourceEffect(P));
P.TargetAttribute = EY01Attribute::MaxHealth;
TestFalse(TEXT("条件禁止生命上限"), Y01Data::ValidSourceEffect(P));
P.TargetAttribute = EY01Attribute::AttackDamage;
TestTrue(TEXT("允许低血攻击百分比"), Y01Data::ValidSourceEffect(P));
P.ConditionThreshold = 101;
TestFalse(TEXT("拒绝越界阈值"), Y01Data::ValidSourceEffect(P));
P.ConditionThreshold = 30;
P.Condition = EY01EffectCondition::None;
TestFalse(TEXT("None必须零阈值"), Y01Data::ValidSourceEffect(P));
P.ConditionThreshold = 0;
P.Amount = -1;
TestFalse(TEXT("拒绝负百分比"), Y01Data::ValidSourceEffect(P));
P.Amount = 20;
P.Kind = EY01SourceEffectKind::AttributeFlat;
P.TargetAttribute = EY01Attribute::GoldGain;
TestFalse(TEXT("GoldGain不接受固定值"), Y01Data::ValidSourceEffect(P));
P.Kind = EY01SourceEffectKind::MappingCoefficientAdd;
P.SourceAttribute = EY01Attribute::Strength;
TestFalse(TEXT("GoldGain不接受三维系数"), Y01Data::ValidSourceEffect(P));
P.Condition = static_cast<EY01EffectCondition>(255);
TestFalse(TEXT("拒绝未知条件枚举"), Y01Data::ValidSourceEffect(P));
```

- [ ] **Step 2：确认红灯。** 构建预计因新枚举尚未声明而失败；记录编译错误，不能运行旧二进制宣称行为已验证。

- [ ] **Step 3：在 `DataTypes.h:59-92` 实现枚举和校验。** 既有枚举数值保持不变；新字段全部追加末尾。

```cpp
UENUM(BlueprintType)
enum class EY01SourceEffectKind : uint8
{
    AttackFlat = 0, GoldPerSecond = 1, AttributeFlat = 2,
    MappingCoefficientAdd = 3, AttributePercent = 4
};
UENUM(BlueprintType)
enum class EY01Attribute : uint8
{
    None, Strength, Agility, Wisdom, AttackDamage, MaxHealth,
    Armor, HealthRegen, GoldPerSecond, AttackRate, GoldGain
};
UENUM(BlueprintType)
enum class EY01EffectCondition : uint8 { None, HealthBelow, HealthAbove };
```

保留现有 `IsPrimaryAttribute` 和 `IsDerivedAttribute`；**后者不加入 GoldGain**，这样旧映射校验仍拒绝该目标。替换 `ValidSourceEffect`：

```cpp
template<typename T> bool ValidSourceEffect(const T& E)
{
    if (!FMath::IsFinite(E.Magnitude) || E.Magnitude < 0
        || !FMath::IsFinite(E.Amount) || E.Amount < 0
        || !FMath::IsFinite(E.ConditionThreshold)
        || E.ConditionThreshold < 0 || E.ConditionThreshold > 100) { return false; }
    if (E.Condition != EY01EffectCondition::None
        && E.Condition != EY01EffectCondition::HealthBelow
        && E.Condition != EY01EffectCondition::HealthAbove) { return false; }
    if (E.Condition == EY01EffectCondition::None)
    {
        if (E.ConditionThreshold != 0) { return false; }
    }
    else
    {
        const bool Kind = E.Kind == EY01SourceEffectKind::AttributeFlat
            || E.Kind == EY01SourceEffectKind::AttributePercent;
        const bool Target = E.TargetAttribute == EY01Attribute::AttackDamage
            || E.TargetAttribute == EY01Attribute::Armor
            || E.TargetAttribute == EY01Attribute::HealthRegen
            || E.TargetAttribute == EY01Attribute::AttackRate
            || E.TargetAttribute == EY01Attribute::GoldGain;
        if (!Kind || !Target) { return false; }
    }
    switch (E.Kind)
    {
    case EY01SourceEffectKind::AttackFlat:
    case EY01SourceEffectKind::GoldPerSecond:
        return E.Amount == 0 && E.SourceAttribute == EY01Attribute::None
            && E.TargetAttribute == EY01Attribute::None;
    case EY01SourceEffectKind::AttributeFlat:
        return E.Magnitude == 0 && E.SourceAttribute == EY01Attribute::None
            && (IsPrimaryAttribute(E.TargetAttribute) || IsDerivedAttribute(E.TargetAttribute));
    case EY01SourceEffectKind::AttributePercent:
        return E.Magnitude == 0 && E.SourceAttribute == EY01Attribute::None
            && E.TargetAttribute != EY01Attribute::GoldPerSecond
            && (IsPrimaryAttribute(E.TargetAttribute) || IsDerivedAttribute(E.TargetAttribute)
                || E.TargetAttribute == EY01Attribute::GoldGain);
    case EY01SourceEffectKind::MappingCoefficientAdd:
        return E.Magnitude == 0 && IsPrimaryAttribute(E.SourceAttribute)
            && IsDerivedAttribute(E.TargetAttribute);
    default: return false;
    }
}
```

Row 尾部追加：

```cpp
UPROPERTY(EditAnywhere, BlueprintReadOnly) EY01EffectCondition Condition = EY01EffectCondition::None;
UPROPERTY(EditAnywhere, BlueprintReadOnly) double ConditionThreshold = 0;
```

`FY01ItemEffect`、`FY01SourceEffectSnapshot`、`FY01ResolvedSourceEffect` 尾部追加：

```cpp
UPROPERTY() EY01EffectCondition Condition = EY01EffectCondition::None;
UPROPERTY() double ConditionThreshold = 0;
UPROPERTY() bool bGrowthContribution = false;
```

只有本来是 USTRUCT 的类型使用 `UPROPERTY()`；普通 C++ 类型使用相同成员声明但去掉宏。`bGrowthContribution` 是服务器生成的显示来源标记，不是可填写的 DataTable 字段。

- [ ] **Step 4：同步复制和比较，不遗漏任何已核实入口。**

| 位置 | 修改 |
| --- | --- |
| `SourceEffectsComponent.cpp:287` | Row→Resolved 尾部加 `Row->Condition, Row->ConditionThreshold, false` |
| `SourceEffectsComponent.cpp:525` | Resolved→Snapshot 尾部加 `E.Condition, E.ConditionThreshold, E.bGrowthContribution` |
| `LoadoutComponent.cpp:147-148` | Row→Item 显式赋值两个条件字段；成长标记默认 false |
| `LoadoutComponent.cpp:366,540` | Item→Resolved 尾部完整复制三字段 |
| `GrowthComponent.cpp:178` | EquipmentEffects→Resolved 尾部完整复制三字段 |
| `GrowthComponent.cpp:101` | 创建成长 AttackFlat 时设置 `Effect.bGrowthContribution = true` |
| `SourceEffectsComponent.cpp:23-30` | Same 比较条件、阈值和成长标记 |
| `LoadoutView.cpp:107` | 效果谓词比较条件、阈值和成长标记 |

显式复制代码：

```cpp
Efx.Condition = Row->Condition;
Efx.ConditionThreshold = Row->ConditionThreshold;
```

resolved 聚合初始化的尾部示例（保留该处既有前缀变量名）：

```cpp
Source.Effects.Add({Effect.EffectId, Effect.Kind, Effect.Magnitude,
    Effect.SourceAttribute, Effect.TargetAttribute, Effect.Amount,
    Effect.Condition, Effect.ConditionThreshold, Effect.bGrowthContribution});
```

Same 比较附加条件：

```cpp
A[I].Condition != B[I].Condition
    || A[I].ConditionThreshold != B[I].ConditionThreshold
    || A[I].bGrowthContribution != B[I].bGrowthContribution
```

在 `PrepareMutation` 的 Equipment/Base 限制括号中加入 `E.Kind == EKind::AttributePercent`，保持 AttackFlat/GoldPerSecond 老白名单；不改 Economy 和 ViewTypes 的非装备白名单。

`DataCatalog.cpp:310-315` 加：

```cpp
V.Enum(Row.Condition, TEXT("Condition"));
V.Number(Row.ConditionThreshold, TEXT("ConditionThreshold"));
V.Check(Y01Data::ValidSourceEffect(Row), TEXT("SourceEffect必须符合固定/百分比/系数及生命条件规则；金币获取仅百分比，金币每秒不接受百分比"));
```

按该函数原有 `Check` 实参形式保留它的行/字段上下文，不删除既有 enum/number 校验。

- [ ] **Step 5：构建并运行 Attribute.Validation，确认绿灯。** 同时测试阈值 0、100、NaN/Infinity 与非法 Kind；重复来源只改变阈值/条件/成长标记必须报 PayloadConflict。
- [ ] **Step 6：检查 diff，不提交。** 只含上述契约和透传，不改正式资产调值。

---

## Task 2：纯结算器、百分比层和结构化明细

**Files:** `Y01AttributeResolver.h/.cpp`、`Y01R36GrowthDropTests.cpp`。

- [ ] **Step 1：在现有测试文件添加独立纯函数测试。** 不新建测试夹具。

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FY01R37ResolverTest, "Y01.Framework.R37.Resolver",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FY01R37ResolverTest::RunTest(const FString&)
{
    FY01UnitCombatStats Base;
    Base.Strength = 10; Base.Agility = 0; Base.Wisdom = 0;
    Base.AttackDamage = 5; Base.Health = 300; Base.Armor = 0;
    Base.HealthRegen = 0; Base.AttackInterval = 1;
    TArray<FY01SourceEffectSnapshot> Effects;
    auto Add = [&](EY01SourceEffectKind Kind, EY01Attribute Source,
        EY01Attribute Target, double Amount, const TCHAR* Id)
    {
        auto& E = Effects.AddDefaulted_GetRef(); E.EffectId = Id;
        E.Source.Kind = EY01RuntimeSourceKind::Equipment; E.Source.LocalKey = TEXT("E01");
        E.Kind = Kind; E.SourceAttribute = Source; E.TargetAttribute = Target; E.Amount = Amount;
    };
    Add(EY01SourceEffectKind::MappingCoefficientAdd, EY01Attribute::Strength, EY01Attribute::AttackDamage, 3, TEXT("Map"));
    Add(EY01SourceEffectKind::AttributePercent, EY01Attribute::None, EY01Attribute::Strength, 20, TEXT("StrPct"));
    Add(EY01SourceEffectKind::AttributePercent, EY01Attribute::None, EY01Attribute::AttackDamage, 20, TEXT("AtkPct1"));
    Add(EY01SourceEffectKind::AttributePercent, EY01Attribute::None, EY01Attribute::AttackDamage, 30, TEXT("AtkPct2"));
    Add(EY01SourceEffectKind::AttributePercent, EY01Attribute::None, EY01Attribute::AttackRate, 100, TEXT("RatePct"));
    Add(EY01SourceEffectKind::AttributePercent, EY01Attribute::None, EY01Attribute::GoldGain, 20, TEXT("GoldPct"));
    auto& Legacy = Effects.AddDefaulted_GetRef(); Legacy.EffectId = TEXT("Legacy");
    Legacy.Kind = EY01SourceEffectKind::AttackFlat; Legacy.Magnitude = 10;
    FY01ResolvedAttributes Out;
    if (!TestTrue(TEXT("混合来源可解析"), FY01AttributeResolver::Resolve(Base, 0, Effects, Out, .3).IsOk())) { return false; }
    TestEqual(TEXT("力量先乘20%"), Out.Stats.Strength, 12.f);
    TestEqual(TEXT("攻击小计含基础及旧AttackFlat"), Out.Breakdowns[int32(EY01Attribute::AttackDamage) - 1].Subtotal, 51.);
    TestEqual(TEXT("攻击20和30相加为50%"), Out.Stats.AttackDamage, 76.5f);
    TestEqual(TEXT("单GE补差"), Out.AttackBonus, 71.5);
    TestEqual(TEXT("攻速先放大再取倒数"), Out.Stats.AttackInterval, .5f);
    TestEqual(TEXT("金币获取输出小数"), Out.GoldGainPercent, .2);
    TestEqual(TEXT("本期不改金币每秒"), Out.GoldPerSecond, 0.);
    auto& Conditional = Effects.AddDefaulted_GetRef(); Conditional.EffectId = TEXT("LowHealth");
    Conditional.Kind = EY01SourceEffectKind::AttributeFlat;
    Conditional.TargetAttribute = EY01Attribute::AttackDamage; Conditional.Amount = 20;
    Conditional.Condition = EY01EffectCondition::HealthBelow; Conditional.ConditionThreshold = 30;
    TestTrue(TEXT("等于阈值可解析"), FY01AttributeResolver::Resolve(Base, 0, Effects, Out, .3).IsOk());
    TestEqual(TEXT("严格低于不包含等于"), Out.Stats.AttackDamage, 76.5f);
    TestTrue(TEXT("低于阈值可解析"), FY01AttributeResolver::Resolve(Base, 0, Effects, Out, .29).IsOk());
    TestEqual(TEXT("条件固定值也受攻击百分比"), Out.Stats.AttackDamage, 106.5f);
    Conditional.Condition = EY01EffectCondition::HealthAbove;
    TestTrue(TEXT("严格高于不包含等于"), FY01AttributeResolver::Resolve(Base, 0, Effects, Out, .3).IsOk());
    TestEqual(TEXT("高于边界不生效"), Out.Stats.AttackDamage, 76.5f);
    TestTrue(TEXT("高于阈值可解析"), FY01AttributeResolver::Resolve(Base, 0, Effects, Out, .31).IsOk());
    TestEqual(TEXT("高于阈值生效"), Out.Stats.AttackDamage, 106.5f);
    return true;
}
```

- [ ] **Step 2：构建及运行 `Y01.Framework.R37.Resolver` 确认红灯。** 新输出/方法未实现时是编译红灯；声明完成后是数值断言红灯。

- [ ] **Step 3：替换 resolver 头文件的输出结构，增加 generated include。**

```cpp
#pragma once
#include "Combat/Y01Combat.h"
#include "Player/Y01SourceRuntimeTypes.h"
#include "Y01AttributeResolver.generated.h"

UENUM()
enum class EY01ContributionKind : uint8 { Base, Flat, Percent, Mapping, Growth };
USTRUCT()
struct Y01_API FY01AttributeContribution
{
    GENERATED_BODY()
    UPROPERTY() EY01ContributionKind Kind = EY01ContributionKind::Base;
    UPROPERTY() FString SourceKey;
    UPROPERTY() FName EffectId;
    UPROPERTY() EY01Attribute SourceAttribute = EY01Attribute::None;
    UPROPERTY() double Amount = 0;
    UPROPERTY() double Contribution = 0;
    UPROPERTY() EY01EffectCondition Condition = EY01EffectCondition::None;
    UPROPERTY() double ConditionThreshold = 0;
    UPROPERTY() bool bActive = true;
};
USTRUCT()
struct Y01_API FY01AttributeBreakdown
{
    GENERATED_BODY()
    UPROPERTY() EY01Attribute Attribute = EY01Attribute::None;
    UPROPERTY() double Final = 0;
    UPROPERTY() double Subtotal = 0;
    UPROPERTY() double PercentTotal = 0;
    UPROPERTY() TArray<FY01AttributeContribution> Contributions;
};
struct Y01_API FY01ResolvedAttributes
{
    FY01UnitCombatStats Stats;
    double AttackBonus = 0;
    double GoldPerSecond = 0;
    double GoldGainPercent = 0;
    TArray<FY01AttributeBreakdown> Breakdowns;
};
class Y01_API FY01AttributeResolver
{
public:
    static FY01RuntimeResult Resolve(const FY01UnitCombatStats& Base, double PlayerStrength,
        const TArray<FY01SourceEffectSnapshot>& Effects, FY01ResolvedAttributes& Out, double HealthRatio = 1);
    static bool ConditionActive(EY01EffectCondition Condition, double Threshold, double HealthRatio);
    static FString Describe(const FY01SourceEffectRow& Effect);
    static FString DescribeBreakdown(const FY01AttributeBreakdown& Breakdown);
};
```

`HealthRatio` 放末尾并给默认值，保留现有四参数纯函数调用；生产来源入口必须显式传权威比例。`PercentTotal`、百分比条目的 Amount/Contribution 都以百分点表示，普通属性 Final/Subtotal 为属性单位；GoldGain.Final 与 PercentTotal 均为百分点，运行时 `GoldGainPercent = Final/100`。

- [ ] **Step 4：实现严格条件和公式，保留原有精度保护。**

```cpp
bool FY01AttributeResolver::ConditionActive(EY01EffectCondition Condition, double Threshold, double HealthRatio)
{
    const double Boundary = Threshold / 100.;
    switch (Condition)
    {
    case EY01EffectCondition::None: return true;
    case EY01EffectCondition::HealthBelow: return HealthRatio < Boundary;
    case EY01EffectCondition::HealthAbove: return HealthRatio > Boundary;
    default: return false;
    }
}
```

替换 `Resolve` 算法为以下完整阶段，保留匿名命名空间的 `Add`/`FloatValue`；`Name` 增加 GoldGain。每个阶段按稳定 Source.Kind/LocalKey/EffectId 顺序运算；不得把上次派生 Stats 用作 Base。

```cpp
FY01RuntimeResult FY01AttributeResolver::Resolve(const FY01UnitCombatStats& Base, double PlayerStrength,
    const TArray<FY01SourceEffectSnapshot>& Effects, FY01ResolvedAttributes& Out, double HealthRatio)
{
    Out = {};
    auto Fail = [] { return FY01RuntimeResult::Fail(EY01RuntimeError::Precision, TEXT("属性/百分比/系数非法、溢出或正增量被吞掉")); };
    if (!FMath::IsFinite(Base.AttackInterval) || Base.AttackInterval <= 0
        || !FMath::IsFinite(HealthRatio) || HealthRatio < 0 || HealthRatio > 1) { return Fail(); }
    constexpr int32 Count = int32(A::GoldGain) + 1;
    double Values[Count] = {}, Direct[Count] = {}, Percents[Count] = {};
    double Coefficients[4][Count] = {};
    TArray<FY01AttributeBreakdown> Details;
    Details.SetNum(Count - 1);
    for (int32 I = 1; I < Count; ++I) { Details[I - 1].Attribute = A(I); }
    auto AddValue = [&](int32 I, double Value)
    {
        const double Before = Values[I];
        if (!Add(Values[I], Value)) { return false; }
        if (I == int32(A::GoldPerSecond) || I == int32(A::GoldGain)) { return true; }
        return FloatValue(Values[I]) && (Value == 0
            || (float(Values[I]) > float(Before) && (Before == 0 || float(Values[I]) != float(Value))));
    };
    auto BaseValue = [&](A Attribute, double Value)
    {
        const int32 I = int32(Attribute);
        if (!FMath::IsFinite(Value) || Value < 0 || !AddValue(I, Value)) { return false; }
        auto& C = Details[I - 1].Contributions.AddDefaulted_GetRef();
        C.SourceKey = TEXT("基础"); C.Amount = Value; C.Contribution = Value;
        return true;
    };
    if (!BaseValue(A::Strength, Base.Strength) || !BaseValue(A::Agility, Base.Agility)
        || !BaseValue(A::Wisdom, Base.Wisdom) || !BaseValue(A::AttackDamage, Base.AttackDamage)
        || !BaseValue(A::MaxHealth, Base.Health) || !BaseValue(A::Armor, Base.Armor)
        || !BaseValue(A::HealthRegen, Base.HealthRegen) || !BaseValue(A::AttackRate, 1. / Base.AttackInterval)
        || !AddValue(int32(A::Strength), PlayerStrength)) { return Fail(); }
    if (PlayerStrength > 0)
    {
        auto& C = Details[int32(A::Strength) - 1].Contributions.AddDefaulted_GetRef();
        C.Kind = EY01ContributionKind::Growth; C.SourceKey = TEXT("玩家成长");
        C.Amount = PlayerStrength; C.Contribution = PlayerStrength;
    }
    auto Sorted = Effects;
    Sorted.Sort([](const auto& L, const auto& R)
    {
        if (L.Source.Kind != R.Source.Kind) { return L.Source.Kind < R.Source.Kind; }
        const int32 Order = L.Source.LocalKey.Compare(R.Source.LocalKey, ESearchCase::CaseSensitive);
        return Order != 0 ? Order < 0 : L.EffectId.LexicalLess(R.EffectId);
    });
    for (const auto& E : Sorted)
    {
        if (!Y01Data::ValidSourceEffect(E)) { return Fail(); }
        const bool Active = ConditionActive(E.Condition, E.ConditionThreshold, HealthRatio);
        A Target = E.TargetAttribute;
        double Amount = E.Amount;
        auto Kind = EY01ContributionKind::Flat;
        if (E.Kind == EY01SourceEffectKind::AttackFlat) { Target = A::AttackDamage; Amount = E.Magnitude; }
        if (E.Kind == EY01SourceEffectKind::GoldPerSecond) { Target = A::GoldPerSecond; Amount = E.Magnitude; }
        if (E.Kind == EY01SourceEffectKind::AttributePercent) { Kind = EY01ContributionKind::Percent; }
        if (E.Kind == EY01SourceEffectKind::MappingCoefficientAdd) { Kind = EY01ContributionKind::Mapping; }
        if (E.bGrowthContribution) { Kind = EY01ContributionKind::Growth; }
        const int32 I = int32(Target);
        auto& C = Details[I - 1].Contributions.AddDefaulted_GetRef();
        C.Kind = Kind; C.SourceKey = FString::Printf(TEXT("%d/%s"), int32(E.Source.Kind), *E.Source.LocalKey);
        C.EffectId = E.EffectId; C.SourceAttribute = E.SourceAttribute; C.Amount = Amount;
        C.Condition = E.Condition; C.ConditionThreshold = E.ConditionThreshold; C.bActive = Active;
        if (!Active) { continue; }
        if (E.Kind == EY01SourceEffectKind::AttributePercent)
        {
            C.Contribution = Amount;
            if (!Add(Percents[I], Amount)) { return Fail(); }
        }
        else if (E.Kind == EY01SourceEffectKind::MappingCoefficientAdd)
        {
            if (!Add(Coefficients[int32(E.SourceAttribute)][I], Amount)) { return Fail(); }
        }
        else
        {
            C.Contribution = Amount;
            if (!Add(Direct[I], Amount)) { return Fail(); }
        }
    }
    for (int32 I = 1; I < Count; ++I) { if (!AddValue(I, Direct[I])) { return Fail(); } }
    auto Scale = [&](int32 I)
    {
        auto& D = Details[I - 1]; D.Subtotal = Values[I]; D.PercentTotal = Percents[I];
        const double Before = Values[I], Factor = 1. + Percents[I] / 100.;
        const double After = Before * Factor;
        if (!FMath::IsFinite(Factor) || !FMath::IsFinite(After)
            || (Percents[I] > 0 && Before > 0 && After <= Before)) { return false; }
        if (I != int32(A::GoldPerSecond) && I != int32(A::GoldGain)
            && (!FloatValue(After) || (Percents[I] > 0 && Before > 0 && float(After) <= float(Before)))) { return false; }
        Values[I] = After; D.Final = After; return true;
    };
    for (int32 I = int32(A::Strength); I <= int32(A::Wisdom); ++I)
    { if (!Scale(I)) { return Fail(); } }
    for (int32 Target = int32(A::AttackDamage); Target <= int32(A::AttackRate); ++Target)
    {
        for (int32 Source = int32(A::Strength); Source <= int32(A::Wisdom); ++Source)
        {
            const double Product = Values[Source] * Coefficients[Source][Target];
            if ((Values[Source] > 0 && Coefficients[Source][Target] > 0 && Product == 0)
                || !AddValue(Target, Product)) { return Fail(); }
        }
        for (auto& C : Details[Target - 1].Contributions)
        {
            if (C.Kind == EY01ContributionKind::Mapping && C.bActive)
            { C.Contribution = Values[int32(C.SourceAttribute)] * C.Amount; }
        }
        if (!Scale(Target)) { return Fail(); }
    }
    auto& Gold = Details[int32(A::GoldGain) - 1];
    Gold.Subtotal = 0; Gold.PercentTotal = Percents[int32(A::GoldGain)]; Gold.Final = Gold.PercentTotal;
    const double Interval = 1. / Values[int32(A::AttackRate)];
    if (!FloatValue(Interval) || Interval <= 0
        || (Values[int32(A::AttackRate)] > 1. / Base.AttackInterval && float(Interval) >= Base.AttackInterval)) { return Fail(); }
    const double Bonus = Values[int32(A::AttackDamage)] - Base.AttackDamage;
    if (!FloatValue(Bonus) || (Bonus > 0 && float(Base.AttackDamage) + float(Bonus) <= float(Base.AttackDamage))) { return Fail(); }
    Out.Stats = Base;
    Out.Stats.Strength = float(Values[int32(A::Strength)]); Out.Stats.Agility = float(Values[int32(A::Agility)]);
    Out.Stats.Wisdom = float(Values[int32(A::Wisdom)]); Out.Stats.AttackDamage = float(Values[int32(A::AttackDamage)]);
    Out.Stats.Health = float(Values[int32(A::MaxHealth)]); Out.Stats.Armor = float(Values[int32(A::Armor)]);
    Out.Stats.HealthRegen = float(Values[int32(A::HealthRegen)]); Out.Stats.AttackInterval = float(Interval);
    Out.AttackBonus = Bonus; Out.GoldPerSecond = Values[int32(A::GoldPerSecond)];
    Out.GoldGainPercent = Gold.Final / 100.; Out.Breakdowns = MoveTemp(Details); return {};
}
```

失败时 Out 保持空值：以上实现直到成功末尾才写 Out；其他调用不能消费失败结果。

- [ ] **Step 5：补纯函数边界用例并运行绿灯。**

| 场景 | 期望 |
| --- | --- |
| 三维成长 10，基础 10，力量%20，系数3 | 力量24、攻击72（基础攻击设0） |
| 成长 AttackFlat=10，攻击%50 | 成长和固定都在小计内，不在百分比后再加 |
| 生命上限300 +100，再%50 | 上限600 |
| 100%金币获取 | `GoldGainPercent=1`，不产生金币/秒 |
| 条件 Below0、Above100 | 对合法比例都不生效 |
| 条件 Above0、Below100 | 满足严格边界；比例0/1正确 |
| Percent 0 | 与原结果一致，无额外GE |
| NaN/Infinity/极大百分比/正值被float吞掉 | Precision失败 |
| 来源逆序输入 | 相同 Stats、小计、条目顺序 |
| 条件未生效 | Contribution=0，保留 Amount/Condition/bActive=false |

运行：

```powershell
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-Resolver-Green-01 -Tests Y01.Framework.R37.Resolver -TimeoutSeconds 150
```

此时旧生产入口尚未迁移单GE，旧 Attribute 混合 AttackFlat 测试可能暂时失败；Task 3 必须闭合该迁移，不能报告可交付。

- [ ] **Step 6：检查 diff，不提交。** 不修改其他数值体系。

---

## Task 3：统一攻击 GE，保留精确事务回滚

**Files:** SourceEffectsComponent、Combat、R36RuntimeTests、R36LoadoutTests、R36GrowthDropTests。

- [ ] **Step 1：迁移失败注入并添加单 GE 断言，先观察红灯。**

`R36RuntimeTests.cpp:497-520` 的 `SecondGEFailure` 不再等待第二条 GE；将 case 改名 `DerivedGEFailure`（GetTests 名称同步），查询拒绝本次第一条新派生 GE：

```cpp
if (Case == TEXT("DerivedGEFailure"))
{
    F.ASC()->GameplayEffectApplicationQueries.Add(FGameplayEffectApplicationQuery::CreateLambda(
        [&](const FActiveGameplayEffectsContainer&, const FGameplayEffectSpec&) { ++Calls; return false; }));
}
```

失败后仍断言攻击35、收入1、精确旧 handle 存活、GE数量1，应用查询次数1。其他重入 case 不删，继续验证真实 GAS 回调。

`ExchangeTwo` 成功最终断言改为：

```cpp
TestEqual(TEXT("来源组件只保留一条派生GE"), F.ASC()->GetActiveEffects(FGameplayEffectQuery()).Num(), 1);
```

测试旧 `Handles[0]` 继续代表唯一派生 GE，不删除失效句柄、回滚精确恢复等断言。

- [ ] **Step 2：修改来源组件的表示与汇总。**

删除以下已失去用途的代码，不保留空 Handles：

- `FRecord::Handles`、ApplyingRecordIndex、ApplyingEffectIndex。
- `StabilizeAttackAggregator` 声明与实现；`Combat.cpp:47-70` 的攻击 dirty 包装和 `Combat.h` 中对应 override；其他 AttributeSet 行为保留。
- ValidateHandles 的逐记录 GE 循环。
- ValidatePending 的逐 Pending GE 循环。
- StageMutationAt 的逐 AttackFlat Apply 循环及逐 record inhibit。
- AbortPending 的逐 record remove。
- CommitMutationAt、FinishTransaction 的逐 record remove。
- ValidateTotals 对逐 AttackFlat 的两处额外累加及逐 record RemovedHandles。

Stage 新 record：

```cpp
FRecord Record; Record.Source = Item.Source; Record.Effects = Item.Effects;
Pending.Add(MoveTemp(Record));
```

ValidateTotals 中：

1. Base.AttackDamage 仍从 ASC `GetNumericAttributeBase` 读取，不从上次最终 Stats 读。
2. Resolver 已含旧 AttackFlat，`OutRate=Resolved.GoldPerSecond`。
3. 本地 Attack=base 后只 `AddFloat(Attack, Resolved.AttackBonus)` 一次。
4. 聚合器快照只排除 DerivedAttackHandle，不排除任何外部 GE。
5. Current=排除本组件旧派生GE后的真实快照值，再 `AddFloat(Current, Resolved.AttackBonus)` 一次，`OutCurrent=Current`。
6. 即使 Extra/Exclude 为空，也必须排除有效的旧派生 GE，避免 no-op/条件重新计算时双计。

保留现有 FrozenQualifications 快照方式，不在真实聚合器上模拟或删除外部 GE。

- [ ] **Step 3：保持单 GE 暂存和退休顺序。**

`bDerivedAttackChanged = PendingAttributes.AttackBonus != DerivedAttackBonus`；bonus 为0时不应用 GE。发生变化先 inhibit 旧派生 GE，再 Apply 新 bonus；失败先删新 GE 后恢复旧 handle。提交线性化后把 Pending handle 变为 committed，再删除 retired 旧 handle。不得先删除旧 handle 后造新 GE。

ValidatePending 容差改为本组件一条 Additive 的有界误差，保留真实 ASC 检查：

```cpp
const double Tolerance = FMath::Max(1e-6,
    FMath::Abs(PendingCurrent) * std::numeric_limits<float>::epsilon() * 2);
```

外部 GE 仍属于实际 current，不能用 `Resolved.Stats.AttackDamage` 覆盖真实 ASC current；本组件保证自身来源的百分比公式，不承诺把任意外部 GE 也纳入百分比。

- [ ] **Step 4：保存 committed 属性与明细，在共同线性化点提交。**

头文件新增：

```cpp
FY01ResolvedAttributes CommittedAttributes;
double CommittedHealthRatio = 1;
double PendingHealthRatio = 1;
double CommittedBaseAttack = 0;
bool bResolvedStateChanged = false;
```

Stage 计算 `bResolvedStateChanged` 至少包括 candidate/committed Breakdowns 的反射结构比较、GoldGainPercent、candidate/base attack，以及条件状态（通过条目 bActive）；不能只比较最终 AttackBonus，两个条件抵消时最终值相同但显示状态仍须提交。

在现有 `CommitMutationAt:418` 条件加入 `bResolvedStateChanged`：

```cpp
if (!Pending.IsEmpty() || !PendingRemoved.IsEmpty()
    || PendingStrength != PlayerStrength || bResolvedStateChanged)
```

在共同提交点、`CommitOwner()` 前移动/赋值：

```cpp
CommittedAttributes = MoveTemp(PendingAttributes);
CommittedHealthRatio = PendingHealthRatio;
CommittedBaseAttack = BeforeBase;
```

该行之后原本使用 `PendingAttributes` 的写属性及句柄幅度代码统一使用 `CommittedAttributes`。Stage 和失败回滚不得修改 committed 缓存，FinishTransaction 只重置 pending 状态。

Initialize 成功设置 initialized 前，以空 Effects 解析 Base+当前生命比例，初始化 CommittedAttributes、CommittedBaseAttack、CommittedHealthRatio；失败不标记 initialized。Observe 只在共同事务结束后返回明细，另加方法：

```cpp
FY01RuntimeResult ObserveAttributes(FY01ResolvedAttributes& Out) const;
```

实现复用 Observe 的 Busy/Authority/session 检查成功后复制 CommittedAttributes；GoldGainPercent 只提供读取，不向 Income 添加倍率。

- [ ] **Step 5：按实际结构迁移现有测试，不一律改所有 GE 数量。**

重点定位：RuntimeTests 187-197、251-280、491-523、630-809；LoadoutTests 475-530、935-1027、1575-1621、1718-1780、2042-2084、2156-2184；GrowthDropTests 810-821。

只改“每个来源各一条 GE”的数量和失败注入次数。以下都保留：来源条目数、技能 GA 数量、外部持续 GE、实际攻击结果、收入旧段、失败后精确旧句柄、事务期间禁止 GA/快照。

必须保留并运行 `Attribute.LegacyAndBaseAttack`：旧 AttackFlat10 + 换算30 + base5=45，移除旧来源后35。新补百分比 base 用例在 Task 4 闭合。

定向命令（同一轮报告过滤可用测试入口原有分隔规则；不确定时分别运行）：

```powershell
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-Effects-Green-01 -Tests Y01.Framework.R36.Effects -TimeoutSeconds 150
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-Attribute-Green-02 -Tests Y01.Framework.R36.Attribute -TimeoutSeconds 150
```

- [ ] **Step 6：检查 diff，不提交。** 不能留下旧逐来源攻击 GE、空 Handles 或 aggregator 重排 shim。

---

## Task 4：权威生命条件刷新及忙碌延迟

**Files:** SourceEffectsComponent、Combat、R36GrowthDropTests。

- [ ] **Step 1：复用 FGrowthWorld 添加 AttributeCondition 场景和失败测试。**

在 Attribute 初始化配置分支创建：

```cpp
FY01SourceEffectRow Low;
Low.Kind = EY01SourceEffectKind::AttributePercent;
Low.TargetAttribute = EY01Attribute::AttackDamage; Low.Amount = 100;
Low.Condition = EY01EffectCondition::HealthBelow; Low.ConditionThreshold = 30;
F.Data.Add(K::SourceEffect, TEXT("LowAttack"), Low);
```

使用该夹具原有 AddRow 方法及设备 EffectIds 拼装方式（上面的 `F.Data.Add` 是场景外表达；在构造器内部使用 `Data` 和现有表添加函数，不能另造 catalog API）。`AttributeCondition` 场景的装备 EffectIds **只含 LowAttack**，不带 AttackCoefficient/IncomeCoefficient/成长；基础 Strength10×3=30、MaxHealth300，穿戴后满血仍为30。测试必须覆盖以下可观察断言，按现有 `F.Send` 真交易装备：

```cpp
auto* ASC = F.P->GetCombatASC(); auto* Sources = F.P->GetSourceEffects();
auto Snapshot = [&] { FY01SourceSnapshot V; Sources->Observe(V); return V; };
ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 90);
TestEqual(TEXT("30%等号不生效"), F.Attack(), 30.f);
const uint64 AtBoundary = Snapshot().Revision;
ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 89);
TestEqual(TEXT("低于30%翻转生效"), F.Attack(), 60.f);
TestEqual(TEXT("翻转只提交一次"), Snapshot().Revision, AtBoundary + 1);
const uint64 Active = Snapshot().Revision;
ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 80);
TestEqual(TEXT("仍低于阈值不重复提交"), Snapshot().Revision, Active);
ASC->SetNumericAttributeBase(UY01AttributeSet::GetHealthAttribute(), 90);
TestEqual(TEXT("回到等号失效"), F.Attack(), 30.f);
```

红灯：只有穿戴时解析，没有 Health 监听则89血攻击仍30。

- [ ] **Step 2：新增无副作用的请求和按需 tick。**

公共/私有接口：

```cpp
void ReevaluateConditions();
virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
void DrainAttributeRefresh();
bool bAttributeRefreshDirty = false;
```

构造器：

```cpp
PrimaryComponentTick.bCanEverTick = true;
PrimaryComponentTick.bStartWithTickEnabled = false;
PrimaryComponentTick.TickGroup = TG_PostUpdateWork;
```

ReevaluateConditions 只在有效、已初始化、未结束、Authority、game-thread 时工作。忙碌/普攻发动/成长致死消费期间只设置 dirty 和启用 tick，**不调用 Interrupt()**，不使外层装卸事务失败。空闲时立即 Drain，确保本次受伤后的下一次攻击读到新值。

Drain 判定逻辑：

```cpp
const double Health = ASC->GetNumericAttribute(UY01AttributeSet::GetHealthAttribute());
const double Max = ASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute());
const double Ratio = Max > 0 ? FMath::Clamp(Health / Max, 0., 1.) : 0.;
const double BaseAttack = ASC->GetNumericAttributeBase(UY01AttributeSet::GetAttackDamageAttribute());
bool Changed = BaseAttack != CommittedBaseAttack;
for (const auto& Record : Records)
{
    for (const auto& E : Record.Effects)
    {
        Changed |= FY01AttributeResolver::ConditionActive(E.Condition, E.ConditionThreshold, Ratio)
            != FY01AttributeResolver::ConditionActive(E.Condition, E.ConditionThreshold, CommittedHealthRatio);
    }
}
```

Changed=false：清 dirty，关闭 tick，不调用 Settle，不增加 SourceRevision。比例在同一区间变化不必更新 CommittedHealthRatio，条件向量相同即可。

Changed=true：确认 Check() 不是 Busy，清 dirty后用 `PrepareMutation({}, {}, Plan) -> StageMutation(Plan, Tx) -> CommitMutation(Tx)`。失败时保留 committed 缓存；Busy 保留 dirty 后续 tick；非 Busy 错误记录现有 UE_LOG 并停止本次自动重试，不能制造无限重试。不吞掉实际错误，也不修改 Income 的 Fault 语义。

Tick 只在 dirty 时 Drain，完成后关闭。EndSession 清 dirty、关闭 tick；不注册局终后的 timer。不可在 FinishTransaction 内直接递归 Drain，因为那里调用者的 `TGuardValue<bool> Call` 尚未退栈；由外层退栈后的立即请求或按需 tick 完成。

- [ ] **Step 3：Stage 使用候选上限判断条件，防装卸时一次错误状态提交。**

ValidateTotals 使用两次无副作用解析：

1. 先按现有权威比例解析 candidate Effects，取得 candidate Stats.Health。
2. `CandidateHealth = Min(current Health, candidate MaxHealth)`，candidate MaxHealth为0时比例0，否则 `CandidateHealth/candidate MaxHealth`。
3. 按 candidate 比例再解析得到最终 PendingAttributes，并在 Stage 保存 PendingHealthRatio。

条件禁止影响 MaxHealth 和三维，所以第二次条件计算不会再次改变 candidate MaxHealth，两次足够，不写迭代求不动点。两个解析都不改 GAS、血量或 Income。

```cpp
const double CandidateMax = Resolved.Stats.Health;
const double CandidateRatio = CandidateMax > 0
    ? FMath::Clamp(FMath::Min(Health, CandidateMax) / CandidateMax, 0., 1.) : 0.;
ResolveResult = FY01AttributeResolver::Resolve(Base, CandidateStrength.Get(PlayerStrength), All, Resolved, CandidateRatio);
if (!ResolveResult.IsOk()) { return ResolveResult; }
```

ValidateTotals 签名末尾新增可选 `double* OutHealthRatio = nullptr`，Stage 传 `&PendingHealthRatio`；Prepare 可不接收。

ValidatePending 若暂存期间 Health/MaxHealth 改变，要对 candidate 上限计算比例并验证与 PendingHealthRatio 的**条件向量**相同；若发生翻转，返回 Interrupted 并自动回滚旧派生 GE，dirty 留待外层退出再算。单纯80→79且同一条件区间不得使事务失败。

- [ ] **Step 4：在 `AttributeSet::PostAttributeChange:199` 的钳血后加钩子。**

```cpp
if (auto* Player = Cast<AY01PlayerState>(GetOwningActor()); Player && Player->HasAuthority())
{
    if (Attribute == GetHealthAttribute() || Attribute == GetMaxHealthAttribute()
        || Attribute == GetAttackDamageAttribute())
    { Player->GetSourceEffects()->ReevaluateConditions(); }
}
```

攻击监听只是兼容公开动态 base API：Drain 比较 base 是否变化，派生 GE 的 current 广播不会再触发新事务；Stage 内部广播只标 dirty，不中断。原有死亡 RecordDeath、上限下降钳血顺序不改。

- [ ] **Step 5：补充真实场景测试并观察绿灯。**

| 场景 | 关键断言 |
| --- | --- |
| HealthBelow/Above30 | 29%、30%、31%精确严格边界 |
| Health同一区间多次变化 | SourceRevision不变、旧GE handle不变 |
| 条件只作用护甲/恢复/攻速/GoldGain | 没有攻击变化也更新缓存/明细/SourceRevision |
| 150/300生命，穿上上限+300与Above40% | 候选比例25%，条件失效；不提前按旧50%提交 |
| 上限下降成功 | 按成功后钳血比例判断；当前血量只在 Commit 后钳制 |
| Stage失败 | 旧上限、旧血量、旧句柄和旧明细完整保持 |
| Stage中受伤跨阈值 | 请求不触发Interrupt；提交校验识别过期条件并回滚，退栈后dirty刷新 |
| Commit广播中回血或EndSession | 已成功的共同提交不反悔；后续刷新/清理正确 |
| 动态base从5到15，攻击%50 | 自身最终攻击按新base重新计算；current广播不递归 |
| 攻速条件翻转 | AttackInterval更新，NextAutoAttackTime不重置 |
| 两玩家 | 同样装备，一个低血另一个满血，各自值及视图独立 |
| 复活 | 沿用复活恢复生命路径，生命条件自然退场，不清成长 |

用 `F.World->Tick(LEVELTICK_All, .01f)` 驱动 deferred 场景，禁止测试自己调用私有 Drain 来替代真实 tick；世界成员名沿用 FGrowthWorld 的 `World`。

- [ ] **Step 6：定向运行 Attribute 与 Resolver，不提交。**

```powershell
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-Conditions-Green-01 -Tests Y01.Framework.R36.Attribute -TimeoutSeconds 150
```

---

## Task 5：完整/增量视图传输权威明细

**Files:** ViewTypes、LoadoutView、R36LoadoutTests、R36GrowthDropTests。

- [ ] **Step 1：在现有 Attribute.Wire 扩展失败断言。** 完整编码带 Condition、Threshold、成长标记和 Breakdowns；只翻转条件时，物品 EntityRevision、库存 Revision 不变，新的 ViewRevision/SourceRevision 和明细必须到达 Receiver。

完整和 delta 均使用现有 Sender.Offer/Acknowledge/Receiver.Receive 窗口循环；不改 ChunkBytes=1024、WindowChunks=4，不另做 RPC。

- [ ] **Step 2：在视图结构末尾追加显示字段。**

`ViewTypes.h` generated include之前 include `Player/Y01AttributeResolver.h`。

`FY01CommittedView` 尾部：

```cpp
UPROPERTY() TArray<FY01AttributeBreakdown> AttributeBreakdowns;
```

`FY01ViewDeltaCommit` 尾部：

```cpp
UPROPERTY() TArray<FY01AttributeBreakdown> AttributeBreakdowns;
```

GoldGain 数值从对应 breakdown.Final读取；不再重复复制一个 GoldGain 标量。

- [ ] **Step 3：接入 existing delta bit、构建、应用与验证。**

```cpp
constexpr uint32 AttributeWireVersion = 0x59303105;
constexpr uint32 DeltaWireVersion = 0x59303106;
```

枚举尾部：

```cpp
DAttributes = 1u << 15
```

BuildDelta：

```cpp
if (!SameArray(Latest.AttributeBreakdowns, Base.AttributeBreakdowns))
{
    D.Fields |= DAttributes;
    D.AttributeBreakdowns = Latest.AttributeBreakdowns;
}
```

Apply：`All = (DAttributes << 1) - 1`；没有 bit却带数据拒绝，带 bit允许空数组用于清空。

```cpp
if (!(D.Fields & DAttributes) && !D.AttributeBreakdowns.IsEmpty()) { return false; }
if (D.Fields & DAttributes) { Out.AttributeBreakdowns = D.AttributeBreakdowns; }
```

完整 `SerializeBin` 已自动包含新增 UPROPERTY。旧完整版本魔数测试 `LoadoutTests.cpp:2338` 同步到0x59303105；新增旧版本拒绝测试，不保留旧版本解析代码。

ValidView 附加数值/结构校验：

- empty breakdown 对没有启用来源模块的旧测试夹具合法；有数据时必须按 Strength..GoldGain 恰好10行且无重复。
- Final/Subtotal/PercentTotal、Amount/Contribution/Threshold 必须有限非负；阈值0..100、枚举有效。
- None条件阈值必须0；条件只出现在允许的五种目标，且只属于 Flat/Percent；bActive=false则Contribution=0。
- Mapping 的 SourceAttribute 只允许三维；非Mapping该字段None；Base/Growth无生命条件。
- Percent目标不能GoldPerSecond；GoldGain行只有Percent、Subtotal=0、Final=PercentTotal。
- 普通行 `Final` 与 `Subtotal × (1+PercentTotal/100)` 在double运算容差内一致；所有数据仍受现有整包字节上限和接收边界约束，不扩大上限。
- 不要求 breakdown.Final 与包含外部GE的 ASC current攻击完全相等；测试UI将此项作为“本来源结算值”展示，真实Stats仍显示实际战斗值。

比较公式可复用：

```cpp
const double Expected = B.Attribute == EY01Attribute::GoldGain
    ? B.PercentTotal : B.Subtotal * (1. + B.PercentTotal / 100.);
const double Tolerance = FMath::Max(1e-9, FMath::Abs(Expected) * std::numeric_limits<double>::epsilon() * 8);
if (!FMath::IsFinite(Expected) || FMath::Abs(B.Final - Expected) > Tolerance) { return false; }
```

- [ ] **Step 4：LoadoutView仅发布 committed 缓存。**

在 `PublishCommittedView` 已有 SourceEffects Busy屏障通过后，读取 ObserveAttributes成功时：

```cpp
FY01ResolvedAttributes Attributes;
if (!Player->GetSourceEffects()->ObserveAttributes(Attributes).IsOk()) { return; }
Next.AttributeBreakdowns = MoveTemp(Attributes.Breakdowns);
```

变量 Player 用该函数原有 owner/player局部变量，不增加第二个 PlayerState查找。没有初始化来源的兼容夹具保持空明细；真实已初始化却读取失败不能发布半状态。ViewRevision 比较必须包含新字段，反射整体比较会自动覆盖，手写比较则补SameArray。

- [ ] **Step 5：运行 full/delta/非法包与交易定向回归。**

验证非法Condition、101阈值、NaN明细、重复属性、未知delta bit、没有bit却带明细、旧wire version都拒绝；拒绝后 Receiver committed视图不变。条件变化不把500个包内物品全部标Changed，不启动完整背包重传。只在本地编码测试检查这一点，不重跑另一窗口已验收的联机性能场报。

```powershell
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-Loadout-Green-01 -Tests Y01.Framework.R36.Loadout -TimeoutSeconds 150
```

- [ ] **Step 6：检查 diff，不提交。** 不改现有窗口/ACK/队列/伤害事件传输。

---

## Task 6：测试 HUD 词条文本与属性明细

**Files:** resolver Describe、R36UI.cpp、R36UIWidgetTests.cpp。

- [ ] **Step 1：先写文本与Widget失败断言。**

```cpp
FY01SourceEffectRow E;
E.Kind = EY01SourceEffectKind::AttributePercent;
E.TargetAttribute = EY01Attribute::AttackDamage; E.Amount = 20;
E.Condition = EY01EffectCondition::HealthBelow; E.ConditionThreshold = 30;
TestEqual(TEXT("百分比条件文本"), FY01AttributeResolver::Describe(E), FString(TEXT("攻击 +20%（生命低于30%时）")));
```

Widget测试新增 `StatRow_GoldGain`、`StatLabel_GoldGain`、`StatValue_GoldGain`存在；输入GoldGain明细Final=20，显示20%；攻击tooltip必须含小计、合计和生效标记。None条件不带后缀；未生效条件仍展示。

- [ ] **Step 2：实现Describe及明细文本。**

Name增加：

```cpp
case A::GoldGain: return TEXT("金币获取");
```

```cpp
FString FY01AttributeResolver::Describe(const FY01SourceEffectRow& E)
{
    FString Text;
    switch (E.Kind)
    {
    case EY01SourceEffectKind::AttackFlat: Text = FString::Printf(TEXT("攻击 +%g"), E.Magnitude); break;
    case EY01SourceEffectKind::GoldPerSecond: Text = FString::Printf(TEXT("金币/秒 +%g"), E.Magnitude); break;
    case EY01SourceEffectKind::AttributeFlat: Text = FString::Printf(TEXT("%s +%g"), Name(E.TargetAttribute), E.Amount); break;
    case EY01SourceEffectKind::AttributePercent: Text = FString::Printf(TEXT("%s +%g%%"), Name(E.TargetAttribute), E.Amount); break;
    case EY01SourceEffectKind::MappingCoefficientAdd:
        Text = FString::Printf(TEXT("每点%s → %s 系数 +%g"), Name(E.SourceAttribute), Name(E.TargetAttribute), E.Amount); break;
    default: return TEXT("未支持效果");
    }
    if (E.Condition != EY01EffectCondition::None)
    {
        Text += FString::Printf(TEXT("（生命%s%g%%时）"),
            E.Condition == EY01EffectCondition::HealthBelow ? TEXT("低于") : TEXT("高于"), E.ConditionThreshold);
    }
    return Text;
}
FString FY01AttributeResolver::DescribeBreakdown(const FY01AttributeBreakdown& B)
{
    FString Text = FString::Printf(TEXT("%s 本来源结算值 %g\n百分比前小计 %g\n百分比合计 +%g%%"),
        Name(B.Attribute), B.Final, B.Subtotal, B.PercentTotal);
    for (const auto& C : B.Contributions)
    {
        const TCHAR* Kind = TEXT("基础");
        switch (C.Kind)
        {
        case EY01ContributionKind::Flat: Kind = TEXT("固定"); break;
        case EY01ContributionKind::Percent: Kind = TEXT("百分比"); break;
        case EY01ContributionKind::Mapping: Kind = TEXT("系数"); break;
        case EY01ContributionKind::Growth: Kind = TEXT("成长"); break;
        default: break;
        }
        Text += FString::Printf(TEXT("\n%s/%s [%s] %g%s → %g"), *C.SourceKey, *C.EffectId.ToString(), Kind,
            C.Amount, C.Kind == EY01ContributionKind::Percent ? TEXT("%") : TEXT(""), C.Contribution);
        if (C.Kind == EY01ContributionKind::Mapping)
        { Text += FString::Printf(TEXT("（有效%s × 系数）"), Name(C.SourceAttribute)); }
        if (C.Condition != EY01EffectCondition::None)
        {
            Text += FString::Printf(TEXT("（生命%s%g%%；%s）"),
                C.Condition == EY01EffectCondition::HealthBelow ? TEXT("低于") : TEXT("高于"),
                C.ConditionThreshold, C.bActive ? TEXT("生效") : TEXT("未生效"));
        }
    }
    return Text;
}
```

`R36UI.cpp:56-72` 的EffectText在复制Row时追加条件/阈值，仍调用统一Describe，不另造第二套词条文本。

- [ ] **Step 3：追加GoldGain行及tooltip，保持现有布局。**

Keys尾部追加 `TEXT("GoldGain")`，Labels追加 `TEXT("金币获取")`。静态Refresh末尾加入：

```cpp
StatValues[TEXT("GoldGain")]->SetText(FText::FromString(TEXT("0%")));
for (const auto& B : V.AttributeBreakdowns)
{
    const TCHAR* Key = nullptr;
    switch (B.Attribute)
    {
    case EY01Attribute::Strength: Key = TEXT("Strength"); break;
    case EY01Attribute::Agility: Key = TEXT("Agility"); break;
    case EY01Attribute::Wisdom: Key = TEXT("Wisdom"); break;
    case EY01Attribute::AttackDamage: Key = TEXT("AttackDamage"); break;
    case EY01Attribute::MaxHealth: Key = TEXT("MaxHealth"); break;
    case EY01Attribute::Armor: Key = TEXT("Armor"); break;
    case EY01Attribute::HealthRegen: Key = TEXT("HealthRegen"); break;
    case EY01Attribute::AttackRate: Key = TEXT("AttackInterval"); break;
    case EY01Attribute::GoldPerSecond: Key = TEXT("IncomeRate"); break;
    case EY01Attribute::GoldGain: Key = TEXT("GoldGain"); break;
    default: break;
    }
    if (!Key) { continue; }
    StatValues[Key]->SetToolTipText(FText::FromString(FY01AttributeResolver::DescribeBreakdown(B)));
    if (B.Attribute == EY01Attribute::GoldGain)
    { StatValues[Key]->SetText(FText::FromString(FString::Printf(TEXT("%g%%"), B.Final))); }
}
```

每次静态Refresh先清除全部StatValues旧tooltip，避免视图清空后残留。AttackInterval行仍显示秒，tooltip明示次数/秒口径；不把频率最终值冒充间隔。保持原16项行ID/文本，新增第17项；原IncomeRate滚动断言仍有效，增加GoldGain滚到可视区域测试。

- [ ] **Step 4：定向Widget测试及一次真实Editor运行检查。**

```powershell
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-HUD-Green-01 -Tests Y01.Framework.R36.UI.HUD08 -TimeoutSeconds 150
```

在可用的本地Editor/PIE打开既有测试HUD：装备百分比词条→人物面板→检查小计/最终/条件标记→改变生命跨阈值→确认显示刷新→收起再打开→装备仍正常交易。检查文字溢出及日志，不重做手机横屏多分辨率或离屏量化。真实窗口被其他用户进程占用时不终止，明确“Widget已验证，未做真实窗口检查”，视觉和手感由用户试玩反馈。

- [ ] **Step 5：检查 diff，不提交。** 不触碰正式UI资产、既有三面板显隐边界和底部常驻槽位。

---

## Task 7：填写指南与轻量收尾验证

**Files:** `Docs/DT字段说明.md:197-205`、`Docs/Y01_配表指南.md:69`、`Docs/Handoffs/Y01_程序窗口续接记录.md`。

- [ ] **Step 1：先核对文档仍未描述为已实现。** 检查当前diff；不把计划内容直接复制成验收结论。
- [ ] **Step 2：实现通过后更新字段表和例子。**

字段说明追加：

| 字段/枚举 | 填写说明 |
| --- | --- |
| AttributePercent | Amount=20表示+20%；Magnitude=0，SourceAttribute=None；只允许Equipment/Base |
| GoldGain | 只用于AttributePercent；表示金币获取加成，本期仅计算显示，第四期才参与授金 |
| Condition | None/HealthBelow/HealthAbove；只允许Flat/Percent和限定目标 |
| ConditionThreshold | 0—100百分数；None必须0；低于/高于都是严格比较 |

可复制例子：

```text
力量 +20%：Kind=AttributePercent, Magnitude=0, SourceAttribute=None, TargetAttribute=Strength, Amount=20, Condition=None, ConditionThreshold=0
低血攻击 +50%：Kind=AttributePercent, Magnitude=0, SourceAttribute=None, TargetAttribute=AttackDamage, Amount=50, Condition=HealthBelow, ConditionThreshold=30
高血护甲 +10：Kind=AttributeFlat, Magnitude=0, SourceAttribute=None, TargetAttribute=Armor, Amount=10, Condition=HealthAbove, ConditionThreshold=70
金币获取 +20%：Kind=AttributePercent, Magnitude=0, SourceAttribute=None, TargetAttribute=GoldGain, Amount=20, Condition=None, ConditionThreshold=0
```

说明百分比影响全部自建固定攻击和成长攻击，三维先放大再换算；生命上限增加不治疗；金币每秒百分比、条件三维/上限、固定金币获取校验报错。成长来源标记不是配表字段。

- [ ] **Step 3：最终一次构建，运行尚未覆盖的受影响定向组。**

已通过且此后未修改的测试不重复运行。新增测试和涉及交易/回滚的组必跑；建议最小集合：

```powershell
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-Resolver-Final-01 -Tests Y01.Framework.R37.Resolver -TimeoutSeconds 150
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-Attribute-Final-01 -Tests Y01.Framework.R36.Attribute -TimeoutSeconds 150
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-Effects-Final-01 -Tests Y01.Framework.R36.Effects -TimeoutSeconds 150
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-Loadout-Final-01 -Tests Y01.Framework.R36.Loadout -TimeoutSeconds 150
powershell -NoProfile -File Scripts/Run-Y01Automation.ps1 -Label R37P1-HUD-Final-01 -Tests Y01.Framework.R36.UI.HUD08 -TimeoutSeconds 150
```

若Loadout组超预算，按已修改case拆分filter，不擅自升为全量或删除测试。GrowthDrop回滚case位于810-821，若不在Attribute筛选下，按该case的注册完整名称定向跑一次；用GetTests实际名称，不用猜测名称。

Server目标仅在Editor编译不能覆盖条件编译分支时追加一次，不默认双构建。联机复制本期以现有 full/delta 编码定向测试为主要证据；除非出现实际传输问题，不重跑500件网络性能专项。

- [ ] **Step 4：一次源码自查与 `git diff --check`。**

```bash
git diff --check
git diff --stat
git status --short
```

自查明确：无逐来源攻击GE、无AttackFlat双计、无旧Aggregator重排、无条件作用上限/三维、无busy刷新调用Interrupt、无Stage钳血、无第四期授金倍率提前生效、无client反推基础值、无旧wire版本兼容分支。

- [ ] **Step 5：续接记录仅写真实产物和证据。** 记录实际构建日志、测试Label/通过与失败计数、未完成的窗口检查或外部进程阻塞；明确下一期是暴击/吸血/护盾，不假称R37四期全完成。
- [ ] **Step 6：交付本期，保持未提交。** 简短说明公式/条件/明细已实现到哪一步，保留所有未通过项为未完成。

---

## 计划自查与验收映射

| 批准规格 | 落点 |
| --- | --- |
| Percent与GoldGain枚举、20=20%、非负 | Task 1、2 |
| 三维先乘、衍生后乘、同类相加、攻速倒数 | Task 2 |
| 旧固定与装备成长攻击都被百分比覆盖、一条GE | Task 2、3 |
| GoldGain仅计算显示 | Task 2、5、6；第四期消费明确排除 |
| 生命条件严格比较、禁上限和三维 | Task 1、4 |
| DS生命/上限变动、只在状态翻转重新事务 | Task 4 |
| 真实来源明细与条件状态 | Task 2、3、5、6 |
| 测试UI接入，正式UI交其他窗口 | Task 6 |
| 原有来源/交易/成长/回滚规则不被削弱 | Task 1、3、4、5 |

实施顺序：Task 1 → Task 2 → Task 3 → Task 4 → Task 5 → Task 6 → Task 7。共享源码不能由两个实施子代理并发修改；子代理模式每任务完成后先审实际diff，再进入下一任务。计划评审只做一次上述覆盖/类型/占位自查，不默认启动多轮审查。
