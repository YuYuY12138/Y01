# Y01 R37 第三期实施计划：时间成长、普攻成长触发与玩家侧目标属性扩展

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `EY01GrowthTrigger` 新增 `CombatTime`（战斗阶段每秒成长）与 `BasicAttack`（每次普攻成长），复用既有成长应用事务；同时按用户 2026-10-19 确认口径把玩家侧成长目标从仅力量扩展为 力量／生命上限／敏捷／智慧（任何触发器通用），装备实例成长维持仅攻击力。

**Architecture:** `UY01GrowthComponent` 开启 1 秒 Tick：时间成长按"经过秒数 × 每秒成长"为每条规则生成增量批；普攻成长由 `NotifyBasicAttack()` 计数（普攻发起点调用，含 bAutoAttackStarting 守卫窗口），Tick 一并结算。两种批都走既有 `Apply` 事务（改造成 `ApplyBatches`，击杀路径复用）。玩家侧新属性经 generalized 成长合计（`FY01PlayerGrowthTotals`）进 `FY01AttributeResolver`：三维（力量/敏捷/智慧）进三维值、生命上限进小计（先于衍生百分比），全部 riding 既有提交链与血量规则（升不治疗、降钳制）。

**Tech Stack:** UE5.7.4、既有 R36 成长/来源事务体系、第二期共享伤害管线基建。

---

## 纪律（每任务开始前重读）

- 原地 dirty main：**不提交、不推送、不重置、不 stash、不清理、不建 worktree、不终止其他窗口进程**。不覆盖其他窗口调值；不改正式 `.uasset`；配表结构变更只用内存 DataTable 测试。
- 设计依据：spec 第3节 + 用户确认：**死亡等待与练功房都不暂停**（用户改写了原文"死亡等待不计时"）；死亡不清空不扣减；合成产物不继承材料成长（合成路径不复制 Growth，无需改代码——执行时用测试证实即可）；成长入百分比前小计。
- **2026-10-19 用户补充口径（本计划已按其更新）**：`EY01GrowthAttribute` 扩展为 {AttackDamage, Strength, MaxHealth, Agility, Wisdom}；玩家侧成长通用——击杀/时间/普攻任何触发器的玩家侧规则都可配 生命上限/敏捷/智慧；装备实例成长维持仅攻击力；「生命」=生命上限（直接进换算链，走现有血量规则，非新维度）。见记忆 `y01-r37-rules.md`。
- 计时基准：`GameState->GetWaveSnapshot().Phase == EY01MatchPhase::Battle` 时计时；Ended/Preparing 不计时；不依赖真实时钟差值跨阶段补偿（阶段外直接不累计）。
- 构建与测试运行方式见记忆 `y01-ue5-automation.md`；报告标签每次唯一不覆盖；**UE Automation 崩溃退出 3 时无 index.json，从 runner log/-abslog 取证**。
- 测试夹具教训（第二期实测）：长挂起波次须**同时**放大普通波与 Boss 波 `PreparationSeconds`（DataCatalog.cpp:539 BossPreparation 跨行校验，否则 TryConfigureMatch 整体失败报"数据目录未就绪"）；`SpawnMonster` 必须把产物赋给夹具成员，否则 MonsterHealth 恒 0、HitPlayer 空解引用崩溃。
- **前置**：第二期已实施完成并验证（R37P2c 37/0、P1Regress 65/0）。

## 语义锁定表

| 规则 | 字段 | 行为 |
| --- | --- | --- |
| 时间成长 | `Trigger=CombatTime`，`Delta`=每秒成长值 | Battle 阶段每秒结算：增量=经过秒×Delta，保留小数 |
| 普攻成长 | `Trigger=BasicAttack`，`Delta`=每次成长值 | 每次普攻真正发动计1次（近战判定执行或飞行物发射），增量=次数×Delta |
| 组合矩阵 | 装备实例→攻击（仅装备来源）；玩家→力量/生命上限/敏捷/智慧（技能/天赋/商品/装备均可） | 玩家侧配 AttackDamage、或装备实例配其他属性 → 运行时 InvalidDefinition，开局校验同矩阵（DataCatalog.cpp:341） |
| 生命上限成长 | 玩家侧 `MaxHealth` | 进换算链生命上限小计（与 AttributeFlat→MaxHealth 同通道、先于衍生百分比）；上限升不治疗、降按现有血量规则钳制；死亡/复活/离局与既有规则一致 |
| 敏捷/智慧成长 | 玩家侧 `Agility`/`Wisdom` | 与力量同通道进三维值，经既有映射系数到衍生属性；提交时写真实 UnitStats/复制 |

暂停语义：时间成长的"计时器"就是 Tick 累计——枚举当刻在装备槽内的实例，卸下后自然不再累计，已得保留。玩家侧规则（技能等）只在来源被移除时停止（来源移除后 Records 无此来源，枚举不到）。

## 文件职责表

| 文件 | 动作 | 职责 |
| --- | --- | --- |
| `Source/Y01/Data/Y01DataTypes.h` | 修改 | Trigger 枚举 +2 值、Attribute 枚举 +3 值 |
| `Source/Y01/Data/Y01DataCatalog.cpp` | 修改 | 成长规则开局校验矩阵（:341） |
| `Source/Y01/Player/Y01SourceRuntimeTypes.h` | 修改 | 新增 `FY01PlayerGrowthTotals`；`FY01PreparedMutation.PlayerStrength`→`PlayerGrowth` |
| `Source/Y01/Player/Y01AttributeResolver.h/.cpp` | 修改 | Resolve 签名收 totals；三维+生命上限成长贡献 |
| `Source/Y01/Player/Y01SourceEffectsComponent.h/.cpp` | 修改 | `PlayerStrength/PendingStrength/CandidateStrength` 管线改为 totals |
| `Source/Y01/Player/Y01ItemRuntimeTypes.h` | 修改 | `FY01EffectiveKillGrowthLedger` 追加 Agility/Wisdom/MaxHealth |
| `Source/Y01/Player/Y01GrowthComponent.h/.cpp` | 修改 | Apply 玩家分支 switch、拆批 ApplyBatches、Tick 批结算、NotifyBasicAttack |
| `Source/Y01/Player/Y01ViewTypes.cpp` | 修改 | Ledger 不变量（:163/:167）覆盖新字段 |
| `Source/Y01/Combat/Y01Combat.cpp` | 修改 | UY01BasicAttackAbility::ExecuteSkill 计数 |
| `Source/Y01Tests/Private/Playground/Y01TestSkill.cpp` | 修改 | 测试自动普攻计数 |
| `Source/Y01Tests/Private/Playground/Y01R37GrowthTriggerTests.cpp` | 新建 | 定向测试 |
| 三份 Docs | 修改 | 文档同步 |

---

### Task 1: 枚举与校验矩阵

**Files:**
- Modify: `Source/Y01/Data/Y01DataTypes.h`
- Modify: `Source/Y01/Data/Y01DataCatalog.cpp`

- [ ] **Step 1.1: 枚举**

`Y01DataTypes.h`（:134、:138）——保留既有值序，只追加：
```cpp
enum class EY01GrowthTrigger : uint8 { EffectiveKill, CombatTime, BasicAttack };
enum class EY01GrowthAttribute : uint8 { AttackDamage, Strength, MaxHealth, Agility, Wisdom };
```

- [ ] **Step 1.2: 开局校验矩阵**

`Y01DataCatalog.cpp` :341-342 替换为：
```cpp
V.Check((Row.Target == EY01GrowthTarget::EquipmentInstance && Row.Attribute == EY01GrowthAttribute::AttackDamage)
    || (Row.Target == EY01GrowthTarget::Player && Row.Attribute != EY01GrowthAttribute::AttackDamage),
    ECode::Invariant, Field, TEXT("装备实例成长仅支持攻击；玩家侧成长支持力量/生命上限/敏捷/智慧"));
```
`:157` 既有"装备实例成长只能由装备来源引用"检查保留不动。

- [ ] **Step 1.3: 编译**

构建同第二期命令（Build.bat Y01Editor，log 名 R37P3-Build<N> 唯一）。预期通过——此步后运行时仍按旧矩阵拒绝新属性（InvalidDefinition），属一致中间态。

---

### Task 2: 玩家成长账本与结算管线泛化

**Files:**
- Modify: `Source/Y01/Player/Y01SourceRuntimeTypes.h`
- Modify: `Source/Y01/Player/Y01AttributeResolver.h/.cpp`
- Modify: `Source/Y01/Player/Y01SourceEffectsComponent.h/.cpp`
- Modify: `Source/Y01/Player/Y01ItemRuntimeTypes.h`
- Modify: `Source/Y01/Player/Y01GrowthComponent.cpp`
- Modify: `Source/Y01/Player/Y01ViewTypes.cpp`

- [ ] **Step 2.1: totals 结构与 Plan 字段**

`Y01SourceRuntimeTypes.h` 在 `FY01SourceEffectSnapshot`（:27-42）之后新增：
```cpp
// 玩家成长四项合计；可从单一力量值隐式构造，保持既有单力量调用点（含测试）不变。
struct Y01_API FY01PlayerGrowthTotals
{
    double Strength = 0, Agility = 0, Wisdom = 0, MaxHealth = 0;
    FY01PlayerGrowthTotals() = default;
    FY01PlayerGrowthTotals(double StrengthOnly) : Strength(StrengthOnly) {}
    bool operator==(const FY01PlayerGrowthTotals&) const = default;
};
```
`FY01PreparedMutation` :120 `TOptional<double> PlayerStrength;` → `TOptional<FY01PlayerGrowthTotals> PlayerGrowth;`

- [ ] **Step 2.2: Resolver 签名与贡献**

`Y01AttributeResolver.h` :53-54：
```cpp
static FY01RuntimeResult Resolve(const FY01UnitCombatStats& Base, const FY01PlayerGrowthTotals& PlayerGrowth,
    const TArray<FY01SourceEffectSnapshot>& Effects, FY01ResolvedAttributes& Out, double HealthRatio = 1);
```
`Y01AttributeResolver.cpp` :64 签名同步；:100-103 替换为：
```cpp
const EY01Attribute DimAttrs[] = { A::Strength, A::Agility, A::Wisdom };
const double DimAmounts[] = { PlayerGrowth.Strength, PlayerGrowth.Agility, PlayerGrowth.Wisdom };
for (int32 I = 0; I < 3; ++I)
{
    if (!AddValue(static_cast<int32>(DimAttrs[I]), DimAmounts[I])) { return Fail(); }
    FY01AttributeContribution Growth; Growth.Kind = C::Growth; Growth.SourceKey = TEXT("玩家成长");
    Growth.Amount = DimAmounts[I]; Growth.Contribution = DimAmounts[I];
    Result.Breakdowns[static_cast<int32>(DimAttrs[I]) - 1].Contributions.Add(MoveTemp(Growth));
}
// 生命上限成长与AttributeFlat→MaxHealth同通道：进小计、先于衍生百分比。
if (!AddValue(static_cast<int32>(A::MaxHealth), PlayerGrowth.MaxHealth)) { return Fail(); }
FY01AttributeContribution HealthGrowth; HealthGrowth.Kind = C::Growth; HealthGrowth.SourceKey = TEXT("玩家成长");
HealthGrowth.Amount = PlayerGrowth.MaxHealth; HealthGrowth.Contribution = PlayerGrowth.MaxHealth;
Result.Breakdowns[static_cast<int32>(A::MaxHealth) - 1].Contributions.Add(MoveTemp(HealthGrowth));
```
（隐式单参构造使既有测试直调点 Y01R36GrowthDropTests:553/559/1115/1268、Y01R36UIWidgetTests:983 全部不改编译。）

- [ ] **Step 2.3: SourceEffects 管线**

`Y01SourceEffectsComponent.h` :66 `TOptional<double> CandidateStrength` → `TOptional<FY01PlayerGrowthTotals> CandidateStrength`；:96-97 `double PlayerStrength/PendingStrength` → `FY01PlayerGrowthTotals PlayerGrowth/PendingGrowth`。
`Y01SourceEffectsComponent.cpp`：grep `PlayerStrength|PendingStrength|CandidateStrength` 逐处替换（:185 `Resolve(BaseAttributes, PlayerGrowth, ...)`；:275/:280 `CandidateStrength.Get(PlayerGrowth)`；:462 `PendingGrowth != PlayerGrowth`；:473 `PlayerGrowth = PendingGrowth`）。语义不变，只拓宽类型。

- [ ] **Step 2.4: 账本字段**

`Y01ItemRuntimeTypes.h` :107 `double Strength = 0;` 后追加（保留 Strength 原名，append 保证旧序列化兼容）：
```cpp
UPROPERTY() double Agility = 0;
UPROPERTY() double Wisdom = 0;
UPROPERTY() double MaxHealth = 0;
```

- [ ] **Step 2.5: Apply 玩家分支 switch**

`Y01GrowthComponent.cpp` :131 `double StrengthDelta = 0;` → `double DimDeltas[4] = {};`（序：力量/敏捷/智慧/上限）。
:148-162 玩家分支改为按属性分流（Entry 查找/建账逻辑不动）：
```cpp
else if (Rule.Target == EY01GrowthTarget::Player && Rule.Attribute != EY01GrowthAttribute::AttackDamage)
{
    auto* Entry = NextLedger.PlayerRules.FindByPredicate(/* 既有谓词不变 */);
    if (!Entry) { /* 既有建账不变 */ }
    if (Entry->Rule.AppliedCount == MAX_uint64 || !Add(Entry->Rule.Accumulated, Rule.Delta)
        || !FloatValue(Entry->Rule.Accumulated))
    { return L->Fail(E::Precision, TEXT("玩家成长批次累计溢出或吞值")); }
    const int32 Dim = Rule.Attribute == EY01GrowthAttribute::Strength ? 0
        : Rule.Attribute == EY01GrowthAttribute::Agility ? 1
        : Rule.Attribute == EY01GrowthAttribute::Wisdom ? 2 : 3;
    if (!Add(DimDeltas[Dim], Rule.Delta)) { return L->Fail(E::Precision, TEXT("玩家成长批次累计溢出或吞值")); }
    ++Entry->Rule.AppliedCount;
}
```
:166-169 批校验改四项（3维经 `ASC->UnitStats`、上限经账本）：
```cpp
const float BeforeStrength = ASC->UnitStats.Strength, BeforeAgility = ASC->UnitStats.Agility, BeforeWisdom = ASC->UnitStats.Wisdom;
if (!FloatValue(BeforeStrength) || !FloatValue(DimDeltas[0]) || !Add(NextLedger.Strength, DimDeltas[0]) || !FloatValue(NextLedger.Strength)
    || !Add(NextLedger.Agility, DimDeltas[1]) || !FloatValue(NextLedger.Agility)
    || !Add(NextLedger.Wisdom, DimDeltas[2]) || !FloatValue(NextLedger.Wisdom)
    || !Add(NextLedger.MaxHealth, DimDeltas[3]) || !FloatValue(NextLedger.MaxHealth))
{ return L->Fail(E::Precision, TEXT("玩家成长整批不能表示为有限非负累计值")); }
```
:183 → `Plan.PlayerGrowth = FY01PlayerGrowthTotals{NextLedger.Strength, NextLedger.Agility, NextLedger.Wisdom, NextLedger.MaxHealth};`
:186 中断检查扩三维：`... || ASC->UnitStats.Strength != BeforeStrength || ASC->UnitStats.Agility != BeforeAgility || ASC->UnitStats.Wisdom != BeforeWisdom`。
注意 MaxHealth 走 resolver→提交链（SourceEffectsComponent.cpp:489 SetNumericAttributeBase），升不治疗/降钳制由既有血量规则承担，本组件不另写血量。

- [ ] **Step 2.6: ViewTypes 不变量**

`Y01ViewTypes.cpp` :163 `FloatValue(Ledger.Strength)` → `FloatValue(Ledger.Strength) && FloatValue(Ledger.Agility) && FloatValue(Ledger.Wisdom) && FloatValue(Ledger.MaxHealth)`；:167 `Ledger.Strength != 0` → 四项均 `!= 0`。

- [ ] **Step 2.7: 消费端矩阵放宽（Task 2 实施中发现的遗漏，同批补做）**

四处消费端仍硬编码 `Trigger==EffectiveKill`＋玩家侧 `Attribute==Strength`，会拒绝第三期新规则，统一放宽（装备实例分支保持原样；玩家侧 Attribute 改 `!= AttackDamage`；Trigger 改三值白名单）：
- `Y01LoadoutComponent.cpp` :155-165（装配/学习成长引用校验）：`Row->Trigger != EY01GrowthTrigger::EffectiveKill` 改 `!(Row->Trigger == EY01GrowthTrigger::EffectiveKill || Row->Trigger == EY01GrowthTrigger::CombatTime || Row->Trigger == EY01GrowthTrigger::BasicAttack)`；Player 分支 `Row->Attribute == EY01GrowthAttribute::Strength` 改 `Row->Attribute != EY01GrowthAttribute::AttackDamage`。
- `Y01EconomyComponent.cpp` :168-173（直接商品）与 :306-311（天赋）：同上两点放宽；错误文案"直接商品仅支持玩家力量成长规则"改"直接商品仅支持玩家侧成长规则"、"天赋仅支持玩家力量成长快照"改"天赋仅支持玩家侧成长快照"。
- `Y01ViewTypes.cpp` :182-185（PlayerRules 账本校验）：Trigger 改三值白名单；`G.Attribute != EY01GrowthAttribute::Strength` 改 `== AttackDamage` 拒绝式；**`G.AppliedCount > Ledger.AppliedKills` 仅对 `Trigger==EffectiveKill` 保留**——时间/普攻成长按批结算，次数与击杀数无关（如 `G.Trigger == EY01GrowthTrigger::EffectiveKill && G.AppliedCount > Ledger.AppliedKills` 拒绝）。
- `Y01ViewTypes.cpp` :193-196（ValidRules 装备/技能实例）：Instance/Player 两布尔与 Trigger 同口径放宽；其 `AppliedCount` 约束同上按 Trigger 区分（以该处现有约束语义为准：EffectiveKill 保留原约束，CombatTime/BasicAttack 不受 AppliedKills 限制）。
- `Y01ViewTypes.cpp` :240-245、:285-290（技能/商品快照 `AppliedCount != 0 || Accumulated != 0` 未应用约束）：Trigger 与 Attribute 放宽同上；未应用零值约束保留。

- [ ] **Step 2.8: 编译**

构建同上。预期通过（含既有测试直调点零改动）。

---

### Task 3: Apply 拆批与时间/普攻 Tick 结算

**Files:**
- Modify: `Source/Y01/Player/Y01GrowthComponent.h/.cpp`

- [ ] **Step 3.1: 头文件扩展**

`Y01GrowthComponent.h`：
```cpp
public:
    // 普攻真正发动（近战判定/飞行物发射）时计数；守卫窗口内调用安全，Tick 统一结算。
    void NotifyBasicAttack();
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
private:
    struct FGrowthBatch
    {
        FY01RuntimeSourceId Source;
        FGuid ItemId;                      // 装备实例批非空
        TArray<FY01ItemGrowth> Rules;      // Rule.Delta 本次增量
    };
    FY01ItemResult ApplyBatches(const TArray<FGrowthBatch>& Batches);
    bool SettleTick();
    double LastTickAt = 0;
    uint64 NextTickBatchId = 1;
    int32 PendingBasicAttacks = 0;
```
构造函数改为 `PrimaryComponentTick.bCanEverTick = true; PrimaryComponentTick.TickInterval = 1.f;`（替换 `bCanEverTick = false`）。

- [ ] **Step 3.2: Apply 拆批**

`Y01GrowthComponent.cpp`：把 `Apply(const FY01KillEvent&)` 的主体抽出为：
```cpp
FY01ItemResult UY01GrowthComponent::ApplyBatches(const TArray<FGrowthBatch>& Batches)
```
内容与现 `Apply` 相同，仅两处泛化：
1. 事件枚举 `for (const auto& Source : Event.Sources)` 改为 `for (const auto& Source : Batches)`（`Source.Rules` 语义不变：`.Delta` 为本次增量）。
2. 触发矩阵检查改为：
```cpp
const bool bKnownTrigger = Rule.Trigger == EY01GrowthTrigger::EffectiveKill
    || Rule.Trigger == EY01GrowthTrigger::CombatTime || Rule.Trigger == EY01GrowthTrigger::BasicAttack;
if (Rule.RuleId.IsNone() || !bKnownTrigger || !FloatValue(Rule.Delta))
{ return L->Fail(E::Precision, TEXT("成长增量不能表示为有限非负float")); }
```
其余（EquipmentInstance→AttackDamage、Player→四属性 switch、InvalidDefinition 兜底）保持 Task 2 形态。
`ApplyBatches` 带第二参 `const FGuid& KillEventId = {}`：**AppliedKills/LastKillEventId 仅由击杀路径维护**（尾部 `if (KillEventId.IsValid()) { ++NextLedger.AppliedKills; NextLedger.LastKillEventId = KillEventId; }`），Tick 批不改写——与视图双向配对校验（`(AppliedKills != 0) == LastKillEventId.IsValid()`）及既有测试语义一致。
`Apply(const FY01KillEvent& Event)` 变薄壳：把 `Event.Sources` 转成 Batches（`ItemId = Source.ItemId`），**并按触发器过滤只带 EffectiveKill 规则**（FreezeSources 冻结来源全部规则，不过滤会让时间/普攻规则在击杀时被整额结算）；空规则来源不入批。后调 `ApplyBatches(Batches, Event.KillEventId)`；保留 kill 幂等（Consumed/KillEventId）在 `Consume` 中不动。Tick 故障（SettleTick 写 GrowthFault＋FaultKillEventId 保持无效）与视图故障校验的单向蕴含（有故障Id必须有故障码）配套；击杀故障仍由 Consume 成对写入。

- [ ] **Step 3.3: Tick 结算**

```cpp
void UY01GrowthComponent::NotifyBasicAttack()
{
    if (IsInGameThread() && Player.IsValid() && bInitialized && !bEnded && PendingBasicAttacks < MAX_int32) { ++PendingBasicAttacks; }
}
bool UY01GrowthComponent::SettleTick()
{
    auto* P = Player.Get(); auto* L = P->GetLoadout(); auto* S = P->GetSourceEffects();
    if (!P->HasAuthority() || bEnded || !bInitialized || L->bEnded || S->bEnded) { return false; }
    const double Now = GetWorld()->GetTimeSeconds();
    const double Elapsed = LastTickAt > 0 ? FMath::Min(Now - LastTickAt, 10.) : 0.;
    LastTickAt = Now;
    auto* GS = GetWorld()->GetGameState<AY01GameState>();
    const bool bBattle = GS && GS->GetWaveSnapshot().Phase == EY01MatchPhase::Battle;
    if (!bBattle || (Elapsed <= 0 && PendingBasicAttacks == 0)) { return false; }
    TArray<FGrowthBatch> Batches;
    // 装备实例：只在槽内计时（卸下暂停并保留已得）。
    for (const auto& Id : L->EquipmentSlots)
    {
        const auto* Item = L->Items.Find(Id); if (!Item) { continue; }
        const auto Source = L->SourceId(Id);
        if (!S->Records.ContainsByPredicate([&](const auto& R) { return R.Source.Id == Source.Id; })) { continue; }
        FGrowthBatch Batch; Batch.Source = Source; Batch.ItemId = Id;
        for (const auto& Rule : Item->Growth)
        {
            if (Rule.Trigger == EY01GrowthTrigger::CombatTime && Elapsed > 0)
            { FY01ItemGrowth R2 = Rule; R2.Delta = Elapsed * Rule.Delta; Batch.Rules.Add(R2); }
            else if (Rule.Trigger == EY01GrowthTrigger::BasicAttack && PendingBasicAttacks > 0)
            { FY01ItemGrowth R2 = Rule; R2.Delta = PendingBasicAttacks * Rule.Delta; Batch.Rules.Add(R2); }
        }
        if (!Batch.Rules.IsEmpty()) { Batches.Add(MoveTemp(Batch)); }
    }
    // 玩家侧规则（技能/天赋/商品）：来源仍在 Records 即计时。
    // 具体做法：照抄本文件 FreezeSources 的三个玩家侧来源枚举块
    // （Skills 遍历 / Economy->OfferOrder 遍历 / TalentState 遍历），
    // 把其中 Event.Sources.Add({...}) 改为构造 FGrowthBatch：
    //   Batch.Source = 既有 Source / Record->Source；Batch.ItemId 保持 FGuid()（玩家侧无实例）；
    //   Batch.Rules = 对应 Rules 数组中 Trigger 匹配且增量>0 的规则拷贝（换算同上）。
    // 这样来源 Kind/LocalKey 与击杀路径完全一致。
```
批构造完：`PendingBasicAttacks = 0;` 先清零再应用（应用失败按既有 Fault 路径记录，计数不回滚——与击杀成长故障语义一致）。
```cpp
    if (Batches.IsEmpty()) { return false; }
    TGuardValue<bool> Consuming(bConsuming, true);
    if (!L->Transactions.Enter()) { return false; }
    ON_SCOPE_EXIT { L->Transactions.Leave(); L->PublishCommittedView(); };
    const auto Result = ApplyBatches(Batches);
    if (!Result.IsOk())
    {
        Ledger.FaultKillEventId.Invalidate(); Ledger.GrowthFault = Result.Code; Ledger.FaultMessage = Result.Message;
        UE_LOG(LogTemp, Display, TEXT("Y01R37 GrowthTickFault Batch=%llu Code=%d Reason=%s"),
            NextTickBatchId++, int32(Result.Code), *Result.Message);
    }
    return true;
}
void UY01GrowthComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
    if (!GetOwner() || !GetOwner()->HasAuthority() || !Player.IsValid() || bEnded) { return; }
    if (GetWorld()->GetSubsystem<UY01KillRewards>()->HasPending(Player.Get())) { return; }  // 击杀优先，避免双批排队
    SettleTick();
}
```
注意 `HasPending` 也含 bDraining 全局真——与 BlocksMutation 相同语义，避免与击杀 Drain 竞争事务。

- [ ] **Step 3.4: 编译**

构建同上。

---

### Task 4: 普攻计数接入

**Files:**
- Modify: `Source/Y01/Combat/Y01Combat.cpp`
- Modify: `Source/Y01Tests/Private/Playground/Y01TestSkill.cpp`

- [ ] **Step 4.1: 生产普攻**

`UY01BasicAttackAbility::ExecuteSkill`（`Y01Combat.cpp` 约249行）`ApplyGameplayEffectSpecToTarget` 之后、`EndAbility` 之前：
```cpp
if (auto* P = Cast<AY01PlayerState>(ASC->GetOwner()); P && P->GetGrowth()) { P->GetGrowth()->NotifyBasicAttack(); }
```

- [ ] **Step 4.2: 测试自动普攻**

`Y01TestSkill.cpp` `AY01TestAttack::ActivateState`（普攻在守卫窗口内真正发动点）末尾追加：
```cpp
if (State.bAutoAttack)
{
    if (auto* P = Cast<AY01PlayerState>(ASC->GetOwner()); P && P->GetGrowth()) { P->GetGrowth()->NotifyBasicAttack(); }
}
```
（`bAutoAttackStarting`/库存事务窗口内只计数不结算，正好符合延迟设计；计数点=发动，不按 ApplyHit 命中人数。）

- [ ] **Step 4.3: 编译**

构建同上。

---

### Task 5: 定向测试

**Files:**
- Create: `Source/Y01Tests/Private/Playground/Y01R37GrowthTriggerTests.cpp`

仿第二期 `Y01R37CritShieldTests.cpp` 夹具（真实权威 Mode 目录 + TryConfigureMatch + TryStartNextWave + 0.01s 步进；遵守纪律节两条夹具教训），五个测试：

1. `Y01.R37.Growth.CombatTime`——装备挂 `CombatTime Player→Strength Delta=2`；进入 Battle；推进仿真约3秒；断言力量增长≈6（保留小数），随后离开 Battle（Preparing）期间数值不再变化；卸下装备后等待数值不变、重新装备后继续累计。
2. `Y01.R37.Growth.BasicAttack`——装备挂 `BasicAttack Target=EquipmentInstance Attribute=AttackDamage Delta=1`；发起3次普攻；断言该实例攻击成长≈3；死亡后数值保留（击杀怪打死玩家再查）。
3. `Y01.R37.Growth.CombatTimeTrainingRoom`——玩家在练功房内（Battle 阶段）时间成长仍累计。
4. `Y01.R37.Growth.PlayerAttributes`——同件挂 `CombatTime` 玩家侧四规则：Strength Delta=1、Agility Delta=2、Wisdom Delta=3、MaxHealth Delta=10；先受伤再进 Battle 3秒：断言三维按 3/6/9 增长、生命上限按 +30 增长且**当前血不回升**（升不治疗）；随后卸下装备：上限回落且当前血被钳到新上限（降钳制）；另配一条 `EffectiveKill Player→MaxHealth` 规则击杀验证触发器通用。
5. `Y01.R37.Growth.PlayerAttributesReject`——纯 resolver 断言：`FY01PlayerGrowthTotals` 隐式单参构造兼容既有调用（`Resolve(Base, 2.0, ...)` 编译执行等价 Strength=2）；`MaxHealth=50` 配 `AttributePercent→MaxHealth 10%` 时上限=(300+50)×1.1=385（成长小计先于衍生百分比）。
6. `Y01.R37.Growth.EnumInjection`——修正 `Y01R36DataTests.cpp:715/717`：整型 1/2 注入"未知枚举"在枚举扩充后变成合法值（1=CombatTime、2=MaxHealth），不再触发 InvalidEnum；改用确定超界值（如 `static_cast<EY01GrowthTrigger>(200)`）保持原测试意图。

运行（带第二期与第一期UI防回归；脚本过滤器用 `+` 分隔前缀）：
```
powershell -NoProfile -ExecutionPolicy Bypass -File Scripts/Run-Y01Automation.ps1 -Label R37P3-First -Tests "Y01.R37.Growth+Y01.R37.Crit+Y01.R37.Shield+Y01.R36.UI" -TimeoutSeconds 240
```

---

### Task 6: 文档同步

**Files:**
- Modify: `Docs/DT字段说明.md`、`Docs/Y01_配表指南.md`、`Docs/Handoffs/Y01_程序窗口续接记录.md`

- [ ] **Step 6.1:** DT_GrowthRule 字段说明：Trigger 三值语义（Delta 单位：击杀/每秒/每次）、Attribute 五值与组合矩阵、暂停规则（卸装暂停该件、死亡等待与练功房不暂停、局终停止）、玩家侧新属性走换算链与血量规则。
- [ ] **Step 6.2:** 配表指南补第三期小节 + 示例行说明（内存示例，不改正式资产）。
- [ ] **Step 6.3:** 续接记录追加：`NotifyBasicAttack` 计数点清单（两处）、Tick 与击杀 Drain 的互斥约定、GrowthFault 新日志名、`FY01PlayerGrowthTotals` 管线与账本新字段（同构建要求）。
