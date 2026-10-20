# Y01 R37 第四期实施计划：金币获取% 与主线怪金币均分

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让第一期已解析的 `GoldGainPercent` 真正生效：自然收入×(1+%)；主线怪 `DT_Monster.Gold` 死亡时在局全部玩家均分再各自乘 (1+%)；清波奖励每人全额×(1+%)；练功房按归属×(1+%)；每玩家一个小数余额满1入账。其他金币入口（失守结清等）保持原样。

**Architecture:** 自然收入在来源校验出口处把 `GoldPerSecond` 乘上 (1+%)，复用既有收入账本不动分毫；一次性金币（主线怪均分/清波/练功房）经 PlayerState 新 `TryAddFractionalGold` 小数钱包入账；主线怪识别用 `AY01GameMode::GetWaveMonsters()`，击杀事件补弱引用怪物指针。

**Tech Stack:** UE5.7.4、既有 R36 收入账本/波次奖励/击杀广播。

---

## 纪律（每任务开始前重读）

- 原地 dirty main：**不提交、不推送、不重置、不 stash、不清理、不建 worktree、不终止其他窗口进程**。不覆盖其他窗口调值；不改正式 `.uasset`（DT_Monster.Gold 列已存在于表结构中，无需改表）。
- 设计依据：spec 第4节 + 用户确认：均分含死亡等待与练功房玩家、不含断线，重连不补发；击杀者无额外份；每人小数按人累计满1入账；金币副本暂不存在只登记规则。
- 构建与测试运行方式见记忆 `y01-ue5-automation.md`。
- **前置**：第二、三期已完成。

## 语义锁定表

| 入口 | 规则 |
| --- | --- |
| 自然收入 | `State.Rate = GoldPerSecond × (1+%)`（ValidateTotals 出口乘，账本零改动） |
| 主线怪 Gold | 死亡时在局全部玩家均分：每人 `Gold/人数 × (1+各自%)`；击杀者无额外份 |
| 清波奖励 | 每人 `Reward.Gold × (1+各自%)`；经验不动 |
| 练功房 | 归属者（本人或击杀者）`Reward.Gold × (1+各自%)` |
| 小数钱包 | 每玩家 `RewardRemainder`，满1取整入账；自然收入不动它（账本自带余量） |

"在局"判定：`PlayerArray` 中 `IsCharacterInitialized()` 且持有有效 PlayerController 的玩家（死亡等待/练功房都满足；断线者 Controller 为空）。

## 文件职责表

| 文件 | 动作 | 职责 |
| --- | --- | --- |
| `Source/Y01/Player/Y01SourceEffectsComponent.cpp` | 修改 | ValidateTotals 出口乘 (1+%) |
| `Source/Y01/Player/Y01PlayerState.h/.cpp` | 修改 | TryAddFractionalGold + RewardRemainder |
| `Source/Y01/Match/Y01KillRewards.h/.cpp` | 修改 | FY01KillEvent 增 Monster 弱引用 |
| `Source/Y01/Match/Y01GameFlow.h/.cpp` | 修改 | 订阅击杀广播、OnMainlineMonsterKill 均分 |
| `Source/Y01Tests/Private/Playground/Y01TrainingRooms.cpp` | 修改 | 练功房奖励乘 (1+%) 走小数钱包 |
| `Source/Y01Tests/Private/Playground/Y01R37GoldGainTests.cpp` | 新建 | 定向测试 |
| 三份 Docs | 修改 | 文档同步 |

---

### Task 1: 自然收入乘算与小数钱包

**Files:**
- Modify: `Source/Y01/Player/Y01SourceEffectsComponent.cpp`
- Modify: `Source/Y01/Player/Y01PlayerState.h/.cpp`

- [ ] **Step 1.1: 出口乘算**

`Y01SourceEffectsComponent.cpp` `ValidateTotals`（:282 现为 `OutRate = Resolved.GoldPerSecond;`，整行替换）：
```cpp
// R37第四期：金币获取%只放大自然收入速率，不进属性明细。
const double Gained = Resolved.GoldPerSecond * (1. + Resolved.GoldGainPercent);
if (!FMath::IsFinite(Gained) || Gained < 0 || (Gained > 0 && Resolved.GoldPerSecond == 0))
{ return Fail(EError::InvalidNumber, TEXT("金币获取%放大后的收入速率无效")); }
OutRate = Gained;
```
（乘后 `OutRate` 经 :638 `PendingRate` 进收入账本；`*OutAttributes = Resolved`（:313）保持基础速率与归一化百分比，故 `ObserveAttributes` 与HUD读数不变——`Y01.Framework.R37.Resolver` 的"GoldGain只输出归一化不改变收入"（求解器层）与 `PercentPrecision` 的"GoldGain不改变每秒收入"（基础速率0）断言不受影响，无需改旧测试。）

- [ ] **Step 1.2: 小数钱包**

`Y01PlayerState.h` public 追加：
```cpp
// R37第四期一次性金币入口：按 (1+金币获取%) 折算后传入；小数余额满1入账。
bool TryAddFractionalGold(double Amount);
double GetRewardRemainder() const { return RewardRemainder; }
```
private：`double RewardRemainder = 0;`
`Y01PlayerState.cpp` 实现：
```cpp
bool AY01PlayerState::TryAddFractionalGold(double Amount)
{
    if (!HasAuthority() || !FMath::IsFinite(Amount) || Amount < 0) { return false; }
    const double Next = RewardRemainder + Amount;
    if (!FMath::IsFinite(Next) || Next < 0 || (Amount > 0 && Next == RewardRemainder)) { return false; }
    if (Next < 1.) { RewardRemainder = Next; return true; }
    const double Whole = FMath::FloorToDouble(Next);
    if (Whole >= 2147483648.0 || static_cast<int64>(Whole) > static_cast<int64>(MAX_int32) - Gold) { return false; }
    if (!TryAddMatchReward(static_cast<int32>(Whole), 0)) { return false; }
    RewardRemainder = Next - Whole;
    return true;
}
```
（容量不足返回 false 且余额不动——调用方用既有 check/失败文案路径。）

- [ ] **Step 1.3: 编译**

构建同前。

---

### Task 2: 主线怪金币均分

**Files:**
- Modify: `Source/Y01/Match/Y01KillRewards.h/.cpp`
- Modify: `Source/Y01/Match/Y01GameFlow.h/.cpp`

- [ ] **Step 2.1: 击杀事件带怪物**

`Y01KillRewards.h` `FY01KillEvent`（:19-33，既有 `Killer` 为 `TWeakObjectPtr<AY01PlayerState>` 同型）追加 `TWeakObjectPtr<AY01Monster> Monster;` 与前置声明 `class AY01Monster;`。
`Y01KillRewards.cpp` `FreezeDeath`（:42，Event 构造在 :50-54）：:48 已有 `auto* Spawn = Spawns.Find(Cast<AY01Monster>(A.Target->GetAvatarActor()));`——把该 Cast 提为局部变量复用，并在 :51 附近追加：
```cpp
auto* Monster = Cast<AY01Monster>(A.Target->GetAvatarActor());
auto* Spawn = Spawns.Find(Monster);
...
Event.Monster = Monster;   // Cast 失败时 Spawn 为空已提前 return，此处必为有效主线/训练怪对象。
```

- [ ] **Step 2.2: GameMode 均分处理器**

`Y01GameFlow.h` `AY01GameMode` private 追加：`void OnMainlineMonsterKill(const FY01KillEvent& Event); FDelegateHandle KillRewardHandle;`
`Y01GameFlow.cpp`：
1. BeginPlay（:44 `Super::BeginPlay();` 附近，执行时读该函数找既有子系统获取写法）订阅：
```cpp
if (auto* Kills = GetWorld()->GetSubsystem<UY01KillRewards>())
{ KillRewardHandle = Kills->OnEffectiveKill.AddUObject(this, &AY01GameMode::OnMainlineMonsterKill); }
```
EndPlay 对称退订 `OnEffectiveKill.Remove(KillRewardHandle)`。
2. 处理器：
```cpp
void AY01GameMode::OnMainlineMonsterKill(const FY01KillEvent& Event)
{
    auto* Monster = Event.Monster.Get();
    if (!HasAuthority() || !bConfigured || !Monster || !GetWaveMonsters().Contains(Monster)) { return; }
    FY01DataValidation Query;
    const auto* Row = MatchCatalog.FindRow<FY01MonsterRow>(Event.Definition, Query);
    if (!Row || Row->Gold <= 0) { return; }
    TArray<AY01PlayerState*> Present;
    for (APlayerState* State : GetFlowState()->PlayerArray)
    {
        auto* P = Cast<AY01PlayerState>(State);
        if (!IsValid(P) || !P->IsCharacterInitialized()) { continue; }
        const auto* PC = Cast<APlayerController>(P->GetOwner());
        if (!PC || !IsValid(PC)) { continue; }   // 断线不含；死亡等待与练功房玩家仍在局
        Present.Add(P);
    }
    if (Present.IsEmpty()) { return; }
    const double Share = static_cast<double>(Row->Gold) / Present.Num();
    for (auto* P : Present)
    {
        FY01ResolvedAttributes Resolved; double Gain = 0;
        if (P->GetSourceEffects()->ObserveAttributes(Resolved).IsOk()) { Gain = Resolved.GoldGainPercent; }
        else { UE_LOG(LogTemp, Display, TEXT("Y01R37 GOLD_SPLIT_OBSERVE_FAIL Player=%s"), *P->GetName()); }
        const double Amount = Share * (1. + Gain);
        if (!P->TryAddFractionalGold(Amount))
        { UE_LOG(LogTemp, Display, TEXT("Y01R37 GOLD_SPLIT_REJECT Player=%s Gold=%d Share=%.17g"), *P->GetName(), Row->Gold, Amount); }
    }
}
```
（`GetWaveMonsters()` 返回 `const TArray<TObjectPtr<AY01Monster>>&`（:107），`Contains` 接受裸指针；广播发生在 `Drain` 的 `Consume` 之后（Y01KillRewards.cpp:80），共同事务已闭合，`ObserveAttributes` 正常返回，Busy 兜底仅防外层重入。）

**测试夹具硬约束**：`WaveMonsters` 仅由波次系统 `SpawnEntry→RegisterWaveMonster`（:342-385）填充，夹具直刷的 `SpawnMonster` 不在列表——`MainlineSplit` 测试必须配置真实波次（内存表注入 DT_Monster 行 `Gold=30` ＋ DT_Wave 生成条目），`TickUntilBattle` 后 Tick 等真实波次生成，再 `KillTarget`；不能直刷。双玩家夹具参考既有多人场景测试（如 CoreRevive/共享核心用例）的第二玩家登录方式。

- [ ] **Step 2.3: 编译**

构建同前。

---

### Task 3: 清波与练功房

**Files:**
- Modify: `Source/Y01/Match/Y01GameFlow.cpp`
- Modify: `Source/Y01Tests/Private/Playground/Y01TrainingRooms.cpp`

- [ ] **Step 3.1: 清波奖励**

`CommitWaveReward`（实际位于 :448-468，函数头部守卫、`RewardedWaveKeys.Add`、bAutomaticWaves 分支、`++S.CompletedWaves; Publish(); OnWaveRewardCommitted();` 均保持原样），仅把接收者预检与发放循环改为：
```cpp
TArray<AY01PlayerState*> Recipients;
TArray<double> GoldAmounts;
for (APlayerState* State : GetFlowState()->PlayerArray)
{
    auto* Player = Cast<AY01PlayerState>(State);
    if (!IsValid(Player) || !Player->IsCharacterInitialized()) { continue; }
    FY01ResolvedAttributes Resolved; double Gain = 0;
    if (Player->GetSourceEffects()->ObserveAttributes(Resolved).IsOk()) { Gain = Resolved.GoldGainPercent; }
    const double Gold = Reward.Gold * (1. + Gain);
    // 经验仍走整数预检；金币经小数钱包（floor 部分容量并入校验）。
    if (!Player->CanAddMatchReward(0, Reward.Experience)) { return false; }
    if (FMath::FloorToDouble(Gold + Player->GetRewardRemainder()) > static_cast<double>(MAX_int32) - Player->GetGold()) { return false; }
    Recipients.Add(Player); GoldAmounts.Add(Gold);
}
// 服务器局内单调波次号作为幂等key；没有客户端key入口。预校验到提交之间不调用外部政策或委托。
RewardedWaveKeys.Add(WaveIndex);
for (int32 I = 0; I < Recipients.Num(); ++I)
{
    const bool bApplied = Recipients[I]->TryAddFractionalGold(GoldAmounts[I])
        && Recipients[I]->TryAddMatchReward(0, Reward.Experience);
    check(bApplied);
}
```
（原"任一玩家预检失败则整体不提交"的原子性保留：预检循环里任意失败 return false 且未写任何状态；`Reward.Gold` 非负由配置校验保证，`Amount=0` 时 `TryAddFractionalGold` 合法返回 true 不动余额。）

- [ ] **Step 3.2: 练功房**

`Y01TrainingRooms.cpp` `OnHealth`（:226 现为 `if (Player->TryAddMatchReward(Record->Reward.Gold, Record->Reward.Experience))`）改为：
```cpp
FY01ResolvedAttributes Resolved; double Gain = 0;
if (Player->GetSourceEffects()->ObserveAttributes(Resolved).IsOk()) { Gain = Resolved.GoldGainPercent; }
if (Player->TryAddFractionalGold(Record->Reward.Gold * (1. + Gain))
    && Player->TryAddMatchReward(0, Record->Reward.Experience))
{ ++Session->View.Kills; }
else { Session->View.Error = TEXT("训练奖励溢出或非法，未发放"); }
```

- [ ] **Step 3.3: 编译 + 定向测试**

**Files:**
- Create: `Source/Y01Tests/Private/Playground/Y01R37GoldGainTests.cpp`

仿既有夹具写四个测试：
1. `Y01.R37.Gold.NaturalIncome`——玩家挂 `AttributePercent Target=GoldGain Amount=100`（金币获取+100%），基础金币/秒=2；推进5秒断言余额增量≈20（2×2×5），账本 Rate 观测值≈4。
2. `Y01.R37.Gold.MainlineSplit`——两只玩家（一人挂金币获取+100%、一人0%）；DT_Monster 行 Gold=30 的主线怪被击杀；断言两人各 +15 与 +30 的小数钱包行为：等价断言 `GetRewardRemainder()+GetGold()` 增量分别为 15 与 30；击杀者无额外份（由击杀者与旁观者份额一致证明）。
3. `Y01.R37.Gold.WaveReward`——清波奖励 Gold=10、玩家 +100%：断言金币+钱包增量=20，经验不变。
4. `Y01.R37.Gold.FractionalWallet`——直连 `TryAddFractionalGold(0.4)` 两次断言余额不变钱包=0.8；再 0.3 断言入账1、钱包=0.1；负值/NaN 返回 false。

运行（带前三期与收入敏感套件防回归；过滤器用 `+` 连接，真实前缀 `Y01.Framework.*`，短前缀匹配不到）：
```
-Label R37P4 -Tests "Y01.R37.Gold+Y01.R37.Growth+Y01.R37.Shield+Y01.R37.Crit+Y01.Framework.R37+Y01.Framework.R36.Economy+Y01.Framework.R36.UI.HUD08"
```
（`Y01.Framework.R37` 含 Resolver／LifeCondition／ViewBreakdowns／PercentPrecision——第一期的收入与百分比断言重点回归面；`Y01.Framework.R36.Economy` 含收入会话速率断言。首轮红灯则按失败清单最小修复后复跑。）

---

### Task 4: 文档同步

**Files:**
- Modify: `Docs/DT字段说明.md`、`Docs/Y01_配表指南.md`、`Docs/Handoffs/Y01_程序窗口续接记录.md`

- [ ] **Step 4.1:** DT_Monster.Gold 语义更新："此前主线未发放，R37第四期起死亡时在局玩家均分"（改掉旧描述）；金币获取%四入口汇总表。
- [ ] **Step 4.2:** 配表指南补第四期小节：各入口公式、"在局"定义（断线不发不补）、失守结清等入口不变的清单、金币副本待登记说明。
- [ ] **Step 4.3:** 续接记录追加：`TryAddFractionalGold` 契约（调用方需自乘 (1+%)）、`FY01KillEvent.Monster` 弱引用、自然收入乘算位置、遗留项（金币副本、正式表行）。
