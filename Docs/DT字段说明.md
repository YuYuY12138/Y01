# DT 字段说明

对应主关卡 [L_Y01R36][map]，24 张正式表统一在 `/Game/Y01Playground/Data/`。日常无参数 Play 或显式 `-R36Profile=content` 会直接加载这24表，不依赖磁盘 [DA_R36Profile][profile]；仅显式 `-R36Profile=profile` 使用旧 Content Profile，`evidence`／`compare` 仍保留，地图 Config 中原软引用未删除。今后停止PIE→修改正式Data表并保存→再次Play；不支持运行中热更新。Run下旧Data仅保留历史，不再作为日常调值入口；根旧DA_PlaygroundProfile未迁移，不用于当前试玩。下文“已接入”表示当前玩法读取，“条件生效”表示须满足注明的引用或模式条件，“未接入”统一表示**当前未接入，改了无玩法效果**，但仍可能影响配置合法性。标注“日常样例”的行和值是本关卡已接配置；其他“例”为解释用假设值。

## 2026-10-09核心／复活收尾：DONE_WITH_CONCERNS

两项审查修复已编译成功；前序59项定向通过保留，不计本轮重跑。此次[首轮报告](file:///G:/GarbageProject/Y01/Saved/Core33-Final-Focused-20261009-01/index.json)40成功／2失败，原因是运行中Static组件拒绝设置mesh，阻挡测试夹具未成立；仅修夹具后[Step1＋Step2报告](file:///G:/GarbageProject/Y01/Saved/Core33-Final-Focused-20261009-02/index.json)42成功、0失败／警告成功／未运行。原两项修复没有先跑红再跑绿，不回滚补形式。外部进程已释放；本轮未停止外部进程，未重跑apply或MainData迁移。

[唯一一次真实DS＋双客户端结果](file:///G:/GarbageProject/Y01/Saved/Core33_20261009/Step3/network-result.json)与[原始日志复核](file:///G:/GarbageProject/Y01/Saved/Core33_20261009/Step3/network-check.json)通过：27个共有核心快照一致（含受击／失守），双方在练功房看到“大本营遭到攻击”，场景血条文本实际存在；普通怪存活并重复攻击核心。受控玩家死亡锁定10秒，实际倒计时9.8→0.1秒，同Pawn在核心外围满300／300复活；二次死亡后受控失守，取消复活、停止攻击、双方离房并显示“大本营已失守”。DS／A／B进程均正常退出0，有限27包未改。最终证据索引见[final-result](file:///G:/GarbageProject/Y01/Saved/Core33_20261009/Step3/final-result.json)。

关注／未验：三端各有GameplayCue路径警告，两个客户端各一次96条移动缓存警告；不宣称网络性能无问题。未做全量／全四波／Boss／Shipping、鼠标全流程、截图视觉与多分辨率；闪红／危急画面、复活后实际行走连通性、非空pending掉落和库存／CD／成长全链路未在本次双端复验，仍按既有定向证据区分。

[保存结果](file:///G:/GarbageProject/Y01/Saved/Core33_20261009/Step3/save-result.json)为passed=true，仅日常[地图][map]／[Profile][profile]与正式[DT_MatchRules][dt-matchrules]／[DT_Wave][dt-wave]四包变化；[apply日志](file:///G:/GarbageProject/Y01/Saved/Core33_20261009/Step3/apply.log)有四包移入记录，[进程回执](file:///G:/GarbageProject/Y01/Saved/Core33_20261009/Step3/apply-process.json)exit=0。[saved-assets快照](file:///G:/GarbageProject/Y01/Saved/Core33_20261009/Step3/saved-assets.json)显示核心启用、[CoreRules](file:///G:/GarbageProject/Y01/Source/Y01Tests/Private/Playground/Y01R36Profile.h)=MatchRules／Default、唯一核心(-650,0,90)cm；摄像机Yaw=0、Pitch=-65，屏幕下方为-X。**这是保存同进程快照，不是冷读**。本轮[严格cold](file:///G:/GarbageProject/Y01/Saved/Core33_20261009/Step3/cold-error.json)因外部TemplatePack变化失败，证据保留；随后[独立只读快照](file:///G:/GarbageProject/Y01/Saved/Core33_20261009/Step3/current-read.json)进程退出0，[24表逐字段核对](file:///G:/GarbageProject/Y01/Saved/Core33_20261009/Step3/current-check.json)通过。核心四值1000／5／3／0.3、四波复活10秒、全部波次其他值、唯一核心与镜头、正式24表绑定保持。外部新增模板及Profile数组追加已核明；139条旧行中只有三条基础映射系数变化：0.2→0.5、0.1→0.02、0.05→0.01，不能写成所有旧值不变。角色和原怪物行、19商品、金币0／基础收入1保持。本轮不保存资产、不覆盖外部变更，根旧Profile不改；过程见[续接记录](file:///G:/GarbageProject/Y01/Docs/Handoffs/Y01_程序窗口续接记录.md)。

## 表目录（按玩法分类）

| 分类 | 表 | 当前用途 |
|---|---|---|
| 角色与战斗 | [DT_Character][dt-character] | 角色基础属性及可选测试成长 |
| 角色与战斗 | [DT_Monster][dt-monster] | 怪物战斗属性、物品掉率及奖励包引用 |
| 角色与战斗 | [DT_Skill][dt-skill] | 主动技能与被动技能书 |
| 来源与成长 | [DT_Equipment][dt-equipment] | 六槽装备定义 |
| 来源与成长 | [DT_SourceEffect][dt-sourceeffect] | 旧攻击／收入及装备直接属性、映射系数增量 |
| 来源与成长 | [DT_AttributeMapping][dt-attributemapping] | 三维到六种派生属性的基础固定系数 |
| 角色与战斗 | [DT_DamageEffect][dt-damageeffect] | 共用选敌、公式、飞行、持续与区域模板 |
| 来源与成长 | [DT_GrowthRule][dt-growthrule] | 成长规则（有效击杀／时间／普攻） |
| 来源与成长 | [DT_Talent][dt-talent] | 天赋来源定义 |
| 来源与成长 | [DT_TalentChoice][dt-talentchoice] | 固定三个候选、每局选一个 |
| 商店与合成 | [DT_ShopOffer][dt-shopoffer] | 公开商品的价格及交付方式 |
| 商店与合成 | [DT_LocalItemPool][dt-localitempool] | 同类型同品质物品加权池 |
| 商店与合成 | [DT_QualityRollPool][dt-qualityrollpool] | 先抽品质、再抽物品的第一层池 |
| 商店与合成 | [DT_Synthesis][dt-synthesis] | 免费五合一、恰好升一品质 |
| 波次与训练 | [DT_Wave][dt-wave] | 有序波次的准备、刷怪和清波奖励 |
| 波次与训练 | [DT_TrainingStage][dt-trainingstage] | 单次入房经过时间对应的循环刷怪阶段 |
| 波次与训练 | [DT_RewardPackage][dt-rewardpackage] | 按使用场景区分数值奖励或物品奖励 |
| 品质定义 | [DT_Quality][dt-quality] | 五品质结构完整性；不驱动当前品质文案 |
| 预留玩法 | [DT_TowerFloor][dt-towerfloor] | 爬塔未接入，仍须保留合法表 |
| 预留玩法 | [DT_LevelExperience][dt-levelexperience] | 升级未接入，仍参与等级覆盖校验 |
| 核心与对局 | [DT_MatchRules][dt-matchrules] | 对局启动、核心共享规则及飘字表现；等级仍仅覆盖校验 |
| 预留玩法 | [DT_MetaItem][dt-metaitem] | 局外物品未接入 |
| 预留玩法 | [DT_MetaLootPool][dt-metalootpool] | 局外抽取未接入 |
| 预留玩法 | [DT_WarehouseEvent][dt-warehouseevent] | 仓库升级事件未接入 |

## 通用字段与合法范围

- 范围以[字段定义][types]、[目录校验][catalog]及[专用配置限制][r36profile]为准；未写特殊上限的浮点值也必须有限，不能填 NaN 或无穷。非负整数范围为 0～2,147,483,647，正整数范围为 1～2,147,483,647；实际求和、余额、容器与浮点精度仍可能进一步拒绝极端值。
- 品质仅有 Green／Blue／Purple／Gold／Red，依次绿／蓝／紫／金／红。品质本身不自动乘攻击或改变掉率；红色是合成顶级，不能再升级。
- 每张表的行名是身份键，不是显示名。引用必须指向**已绑定表中的真实行**；重命名须同步所有引用。数组中的 `[]` 表示每个元素，不是让用户新增一个叫 `[]` 的字段。
- 正式入口固定加载 `/Game/Y01Playground/Data/` 的 **24 表**；全部须存在、结构正确且通过引用校验。“未接入”绝不等于可以删表、清空必填表或破坏引用。旧 Profile 入口仍按其数组与表绑定校验，只用于显式兼容／证据模式。
- 新增且合法的商品行、配方行由各自 [Enabled][types] 控制启用并按 [SortOrder][types] 排序；合法商品结构默认启用。波次由 [Enabled][types]＋[Order][types] 控制。装备／技能定义行会被自动发现，但不会自动发放、装备或学习；购买仍须有启用商品行和合法物品池引用，RandomPool 的 [Pool.Type][types] 必须为 LocalItemPool 且 [Pool.RowName][types] 指向真实行。角色、天赋选择及其它启动值从 [DT_MatchRules][dt-matchrules]／Default 的 [Startup][types] 读取。
- 日常初始金币、基础每秒金币、掉落飞行秒数位于 [Startup.InitialGold][types]、[Startup.BaseGoldPerSecond][types]、[Startup.DropFlightSeconds][types]；结构默认依次为0、0、1，当前Default行依次为0、1、1。三维基础派生比例在 [AttributeMapping][dt-attributemapping]，由 [bBaseEnabled][types] 控制；装备系数增量在 [SourceEffect][dt-sourceeffect]。装备槽数量、技能槽数量、合成收费没有可调 DT 字段。

### 通用引用结构 [FY01DataRef][types]（所有引用字段均适用）

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Type][types] | 所引用表的类型 | 枚举；须等于使用处指定的表类型 | 例：装备引用用 Equipment，技能引用用 Skill；不是资产路径，也不是品质 |
| [RowName][types] | 目标表内的行名 | 非空 FName，目标行必须存在 | 例：假设装备表存在 Blade 行，引用就是 Equipment＋Blade；不能填显示名 |

下列各表中的单个引用、引用数组元素以及嵌套元素内的引用，均继续展开为这两个字段，不另藏字段。只有明确允许“空引用”的位置才可用 [Type][types]=None **且** [RowName][types]=None；只清其中一项不算空。普通可选引用数组使用空数组，不能塞入空引用占位。

装备、技能、天赋、商品中的两种来源数组，元素字段统一如下；各表另列数组能否为空、来源启用条件及目标限制。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [EffectIds[].Type][types] | 效果元素的目标表类型 | 必须 SourceEffect | 不能填属性名或 GrowthRule |
| [EffectIds[].RowName][types] | 效果行身份 | 效果表真实行名；同一数组不重复 | 读取该行种类与幅度；行存在不代表来源已装备、已学或已选 |
| [GrowthRuleIds[].Type][types] | 成长元素的目标表类型 | 必须 GrowthRule | 不是成长目标；玩家／装备实例目标在规则行内填写 |
| [GrowthRuleIds[].RowName][types] | 成长规则行身份 | 成长规则表真实行名；同一数组不重复 | 非装备来源只能指向玩家侧规则（力量／生命上限／敏捷／智慧） |

装备和技能另有 [DamageEffectIds][types] 数组：每项 [Type][types]=DamageEffect、[RowName][types] 为真实伤害模板行，同数组不得重复；天赋及直接效果商品没有此字段。

其余数组元素分别是 [FY01SpawnEntry][types]（旧刷怪）、[FY01SpawnBatch][types]／[FY01BatchMonster][types]（点位批次）、[FY01DamageTerm][types]（伤害公式项）、[FY01RewardEntry][types]（奖励）、[FY01WeightedEntry][types]（物品权重）、[FY01QualityRollEntry][types]（品质权重）；它们的全部字段已在相应表中按路径展开。

## 1. [DT_Character][dt-character] — 角色

正式入口仅使用 [DT_MatchRules][dt-matchrules]／Default 的 [Startup.Character][types] 指向行初始化角色；当前为 Character／Default。属性从该行读取，装备和已学技能另由实例来源叠加。显式旧Profile入口仍使用其 Character 引用；当前R36校验仍拒绝角色行内非空 [InitialEquipment][types]／[InitialSkills][types]。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Health][types] | 初始生命及生命上限 | HP；float，必须大于 0 | 已接入；例：500 表示出生满血 500、基础上限 500 |
| [AttackDamage][types] | 基础攻击力 | 攻击点；float，非负 | 已接入；普攻使用当前攻击快照，装备等加值在基础值之外叠加 |
| [AttackInterval][types] | 两次成功普攻发动的最小间隔 | 秒／次；float，必须大于 0 | 已接入；例：0.5 约每秒最多发动两次；近战普攻判定窗口取该值与 0.35 秒的较小者；不是技能冷却 |
| [AttackType][types] | 普攻形态 | Melee 或 Ranged | 已接入；近战扇形或远程飞行物，不改变手动技能模式 |
| [MeleeAngle][types] | 近战扇形完整夹角 | 度；float，0＜值≤360 | 条件生效：Melee；例：90 是朝向两侧各 45°，不是半角；远程也须填合法值 |
| [ProjectileSpeed][types] | 远程普攻弹速 | cm／秒；float，必须大于 0 | 条件生效：Ranged；普攻飞行寿命由射程／弹速决定；不是技能球的速度 |
| [Armor][types] | 每次受击的固定减伤 | 伤害点；float，非负 | 已接入；攻击伤害快照减去目标护甲，结果最低为 0，不是百分比；例：20 伤害遇 3 护甲扣 17 HP（目标剩余生命足够时） |
| [HealthRegen][types] | 自动生命回复速率 | HP／秒；float，非负 | 已接入；有效在场、未结束且存活时按经过时间回复，不超过上限，不复活；例：2 每秒回 2 HP |
| [Range][types] | 普攻寻敌距离、近战半径或远程射程 | cm；float，非负 | 已接入；普攻按角色与目标中心的水平距离判定；0 不表示无限距离 |
| [MoveSpeed][types] | 角色最大步行速度 | cm／秒；float，非负 | 已接入；例：600 对应每秒 6 米；不是攻击速度 |
| [Strength][types] | 初始力量属性点 | 点；float，非负 | 基础值加玩家力量成长及装备直接加值后，再按有效映射派生；当前冷读力攻系数0.5、力量到生命系数5；装备可追加同向系数 |
| [Agility][types] | 初始敏捷属性点 | 点；float，非负 | 读取、显示并参与有效映射；当前冷读敏捷到护甲系数0.02、到AttackRate系数0.01；不会自行增加移速，无隐含比例 |
| [Wisdom][types] | 初始智慧属性点 | 点；float，非负 | 读取、显示并参与有效映射；当前冷读智慧到金币／秒系数0.01、到HealthRegen系数0.02；可供伤害公式显式引用，治疗护盾未开放 |
| [R33TestGrowth][types] | 独立测试成长配置 | 结构；全部子字段见下表 | 条件生效；不是正式三维派生公式，也不是物品的击杀成长规则 |
| [InitialEquipment][types] | 初始装备引用数组 | 通用最多 6 项；**当前 R36 必须为空** | 固定六个通用槽；当前装备须先取得实例再装入，不能用此数组预装 |
| [InitialEquipment[].Type][types] | 初始装备元素的表类型 | Equipment；但当前数组须空 | 仅说明完整嵌套结构，不可据此绕过空槽初始化限制 |
| [InitialEquipment[].RowName][types] | 初始装备元素的行名 | 对应装备表真实行；但当前数组须空 | 与上一项组成一个引用 |
| [InitialSkills][types] | 初始技能引用数组 | 通用最多 4 项；**当前 R36 必须为空** | 主动、被动共用固定四个技能槽；当前须取得书并学习 |
| [InitialSkills[].Type][types] | 初始技能元素的表类型 | Skill；但当前数组须空 | 仅说明完整嵌套结构 |
| [InitialSkills[].RowName][types] | 初始技能元素的行名 | 对应技能表真实行；但当前数组须空 | 不可用空引用当空槽 |
| [Mesh][types] | 角色骨骼网格资源 | USkeletalMesh 软引用；可空 | **当前未接入，改了无玩法效果**；当前显示不从此字段加载模型 |

### [R33TestGrowth][types] 全部子字段

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [R33TestGrowth.bEnabled][types] | 启用这一组测试成长 | 布尔值 | 关闭则下面三项均不授予，但数值仍须合法；不控制装备、技能、天赋或商品的成长规则 |
| [R33TestGrowth.AttackPerEffectiveSecond][types] | 每个完整有效秒增加基础攻击 | 攻击点／有效秒；double，有限非负 | 例：0.5 每个有效整秒加 0.5；需有效角色与 Pawn、存活且未结束；等待开波也可累计，不仅限战斗 |
| [R33TestGrowth.AttackPerEffectiveKill][types] | 每次有效击杀增加基础攻击 | 攻击点／次；double，有限非负 | 主线合法击杀或本人训练房击杀，一只怪计一次；不是命中次数、普攻发动次数或全队均摊 |
| [R33TestGrowth.GoldPerEffectiveSecond][types] | 每个有效秒增加金币 | 金币／有效秒；double，有限非负 | 例：0.25 累积四个有效秒可入账 1 金币；小数累计，非一次奖励；独立于基础收入与来源收入 |

[测试成长消费者][testgrowth]只改攻击基础值或金币，不改在途攻击快照；死亡、登出或结束停止。攻击增量和累计须可表示为 float，不能溢出或小到加不上；金币受整数余额上限约束。这里的有效时间与[来源收入][income]的启动时点不同，不能混为一条公式。

## 2. [DT_Monster][dt-monster] — 怪物

必须被波次或训练阶段的刷怪元素引用才会生成。主线和训练房均使用该表战斗属性；奖励要区分下列上下文。怪物死亡（生命≤0）后尸体立即关闭碰撞并约3秒后销毁；尸体不可再被选敌或攻击，掉落与金币/经验结算都在死亡瞬间完成，与销毁时机无关。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Health][types] | 怪物初始生命及上限 | HP；float，必须大于 0 | 已接入；例：120 表示出生 120 HP |
| [AttackDamage][types] | 怪物基础攻击 | 攻击点；float，非负 | 已接入；0 表示无基础伤害，不表示停止攻击 |
| [AttackInterval][types] | 普攻发动间隔 | 秒／次；float，必须大于 0 | 已接入；例：2 表示两次成功发动至少间隔 2 秒；近战普攻判定窗口取该值与 0.35 秒的较小者 |
| [AttackType][types] | 怪物普攻形态 | Melee 或 Ranged | 已接入；形态由此字段决定，不由行名决定 |
| [MeleeAngle][types] | 近战完整扇形夹角 | 度；float，0＜值≤360 | 近战消费；例：120 表示两侧各 60°；远程也须保留合法值 |
| [ProjectileSpeed][types] | 怪物远程普攻弹速 | cm／秒；float，必须大于 0 | 远程消费；不是移动速度；近战也须填合法正值 |
| [Armor][types] | 怪物固定减伤 | 伤害点；float，非负 | 已接入；普攻、旧测试技能及共享伤害模板均通过现有伤害链减此值，最低 0 |
| [HealthRegen][types] | 怪物每秒回血 | HP／秒；float，非负 | 已接入；主线、训练怪存活且未结束时回复，最多到生命上限 |
| [Range][types] | 普攻水平寻敌距离与射程 | cm；float，非负 | 已接入；近战也作扇形半径，远程参与飞行寿命计算 |
| [DetectionRadius][types] | 主动发现／保持目标半径 | cm；float，有限非负，默认2000 | XY最近合法玩家；超出就丢弃并重搜，0关闭主动发现；攻击Range独立。日常四种怪明确为2000 |
| [MoveSpeed][types] | 追击的最大步行速度 | cm／秒；float，非负 | 已接入；0 可令怪物无法正常移动，但仍可在合法距离内攻击 |
| [Skills][types] | 怪物初始化技能引用数组 | 可空；每项为 Skill 引用 | 初始化会读取，不是当前自动普攻的开关；当前 AI 走独立普攻，不自动逐个释放此数组技能 |
| [Skills[].Type][types] | 怪物技能元素的表类型 | 必须 Skill | 怪物初始化不支持带非空效果 ID 的技能；不要直接拿玩家来源技能当怪物技能 |
| [Skills[].RowName][types] | 怪物技能元素行名 | 技能表真实行 | 初始化可加载所指 GA／GE；玩家成长规则没有怪物成长消费者，填了不会令怪物成长 |
| [Experience][types] | 怪物单体经验字段 | 经验点；非负整数 | **当前未接入，改了无玩法效果**；主线经验来自清波奖励包，训练经验来自阶段奖励包 |
| [Gold][types] | 怪物单体金币字段 | 金币；非负整数 | R37第四期起主线波次怪死亡时**在局全部玩家均分**：每人 Gold÷人数×(1+各自金币获取%)，小数经玩家钱包满1入账，击杀者无额外份，断线玩家不发不补；训练房怪物金币仍不覆盖训练包；不是逐怪全队金币、不是训练击杀金币 |
| [DropReward][types] | 物品掉落包引用 | 必须 RewardPackage＋真实行名；掉率为 0 也须合法引用 | 条件生效：正掉率且有效击杀；必须为个人局内纯物品包，金币与经验均为 0，物品非空 |
| [ItemDropChance][types] | 每次有效击杀触发整个物品包的概率 | double，0～1 | 例：0.25 表示 25%；0 不掉，1 必掉；不是各条目分别判率。主线和本人训练房有效击杀均可触发，物品归击杀者 |
| [Mesh][types] | 怪物骨骼网格资源 | USkeletalMesh 软引用；可空 | **当前未接入，改了无玩法效果**；当前显示不用此模型字段 |

[物品掉落消费者][drop]在触发后确定整个包的物品并安排到货，进入背包不等于装备或学习；不读取怪物金币、经验。飞行时长不在DT。核心模式中玩家死亡不取消已归属物品，仍按原ETA入包；核心失守先冻结，再将已归属掉落一次结清后结束，不扣回已得物品、不竞拍。普通结束的未交付取消策略不等于核心失守结清；忙或交付故障不能假报结清。

## 3. [DT_Skill][dt-skill] — 技能与技能书

技能书进入背包不授予技能效果，学习后才占用一个技能槽并启用来源；主动与被动合计最多四槽。主动技能的静态效果在学习后持续存在，不是每次施法重新叠加。主关卡公开内容采用下面的专用约束。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Icon][types] | 技能书／槽位图标 | UTexture2D 软引用；可空 | 已接入 UI；有效纹理显示图标，空或加载失败走品质占位显示 |
| [Type][types] | 主动或被动 | Active／Passive | Active 学习后可按槽释放；Passive 只启用来源，不施法 |
| [Quality][types] | 技能书品质 | 五品质之一 | 用于显示、池匹配和合成；不会自动放大技能倍率 |
| [CooldownSeconds][types] | 技能提交后的冷却 | 秒；float，主动必须大于0，被动必须为0 | 例：3 表示成功提交后 3 秒才可再用；失败请求不等于成功施法，不控制普攻频率 |
| [TestSkill][types] | 原生技能模板参数 | 结构；全部子字段见下表 | 模式须与实际 GA 匹配；单改模式不会自动切换 GA 类 |
| [Ability][types] | 主动技能实现类（GA） | 主动必须是可加载的指定原生类；被动为空 | 无伤害ID的None模式配 `/Script/Y01.Y01BasicAttackAbility`；旧Area／Projectile配 `/Script/Y01Tests.Y01TestSkillAbility`；非空伤害ID的主动技能必须配 `/Script/Y01.Y01DamageAbility`，且TestSkill.Mode=None；不能任意换蓝图GA |
| [Effect][types] | 直接 GE 授予兼容字段 | 当前公开技能必须为空 | 当前玩家来源走效果 ID／成长规则，不与此 GE 混填；不是技能命中伤害 GE 的选择器 |
| [DamageEffectIds][types] | 学习后生效的伤害模板数组 | 可空；DamageEffect引用，禁止重复 | 主动仅Active；被动仅Periodic／Bound。与旧TestSkill攻击模式互斥；日常主动TrialFormulaBolt、被动TrialStackPoison |
| [EffectIds][types] | 学习后常驻的效果引用数组 | 可空；SourceEffect引用，仅旧AttackFlat／GoldPerSecond；同数组不得重复 | 例：一个加攻击效果和一个每秒金币效果可以并存；移除技能时撤销静态来源 |
| [GrowthRuleIds][types] | 学习期间生效的成长规则数组 | 可空；元素为 GrowthRule＋行名；不得重复 | **所有非装备来源只允许 Player＋力量／生命上限／敏捷／智慧**；主动、被动都不能引用 EquipmentInstance 成长 |
| [DisplayName][types] | 技能书与技能显示名 | 文本；公开内容去首尾空白后不得为空 | 已接入；不是行名，不决定伤害、品质或主动被动 |

被动必须同时满足：[Ability][types] 为空、[Effect][types] 为空、[CooldownSeconds][types]=0、[TestSkill.Mode][types]=None。其余测试技能数值即使不消费也须有限非负；可清为默认或零。被动允许 [EffectIds][types]、[GrowthRuleIds][types] 均空；若仅填 [DamageEffectIds][types]，仍会启用对应周期／绑定伤害，但不提供静态属性或击杀成长。

### [TestSkill][types] 全部子字段

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [TestSkill.Mode][types] | 技能执行模式 | None／Area／Projectile | 主动的None须结合DamageEffectIds判断：空数组走旧即时GA，非空走共享DamageAbility；被动None表示无主动攻击。Area 为定点区域，Projectile 为定向球，两者只允许主动技能 |
| [TestSkill.DamageCoefficient][types] | 测试技能攻击倍率 | 倍；float，有限非负 | Area／Projectile 消费；例：假设攻击 40、倍率 1.5，护甲前伤害为 60；乘积也须有限。None 不消费此倍率 |
| [TestSkill.Radius][types] | 区域半径或球体碰撞半径 | cm；float，非负；Area／Projectile 必须大于 0 | Area 按目标中心水平距离判断；Projectile 用球形扫掠；不是直径 |
| [TestSkill.HalfHeight][types] | 区域上下半高 | cm；float，非负 | 仅 Area 消费；例：100 允许目标中心在释放原点上下各 100 cm；球体不读取此值 |
| [TestSkill.ActiveSeconds][types] | 区域存在时长 | 秒；float，非负；Area 必须大于 0 | 仅 Area 消费；区域固定在释放原点，期间每个目标最多命中一次，不是周期伤害间隔 |
| [TestSkill.TargetRange][types] | 发球时选择最近合法目标的距离 | cm；float，非负；Projectile 必须大于 0 | 仅 Projectile 消费，按三维中心距离；不是球的硬性最大飞行距离 |
| [TestSkill.Speed][types] | 技能球飞行速度 | cm／秒；float，非负；Projectile 必须大于 0 | 仅 Projectile 消费；方向在释放时确定，不持续追踪目标 |
| [TestSkill.Lifetime][types] | 技能球最长飞行时间 | 秒；float，非负；Projectile 必须大于 0 | 仅 Projectile 消费；例：速度 300、寿命 2，未受阻时约飞 600 cm |
| [TestSkill.HitFeedbackSeconds][types] | 球命中后的反馈停留时间 | 秒；float，非负 | 仅 Projectile 消费；命中后不继续伤害或穿透，0 表示不额外停留；不是伤害间隔 |
| [TestSkill.bLifesteal][types] | 测试技能伤害可否吸血 | bool；结构默认true | 仅旧Area／Projectile测试技能消费；true时该技能伤害走共享伤害管线，按攻击者吸血属性以实际生命损失回复；主动默认开，配false可关闭 |

[技能消费者][testskill]的 Area／Projectile 按施法提交时攻击快照乘倍率，再减目标护甲；途中换装不追改已有攻击。没有伤害ID的主动None使用[即时单目标实现][combat]，不读取上述几何、时长与倍率，按当前攻击造成伤害并使用技能表冷却；有伤害ID时读取第24节模板；被动 None 不施法。测试球遇阻挡墙终止，区域不随角色移动。

## 4. [DT_Equipment][dt-equipment] — 装备

当前六槽没有部位限制；来源按已装备实例结算，显示名不是规则身份。日常两件生命规则使用不同装备行，固定系数相加；不要复制相同效果ID到同一数组来加倍。背包装备不提供属性、收入、新伤害发动或新的击杀成长。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Quality][types] | 装备品质 | 五品质之一 | 已接入显示、池匹配及合成；红色不能当升级材料 |
| [Icon][types] | 装备图标 | UTexture2D 软引用；可空 | 已接入 UI；缺图时走品质占位显示，不影响数值 |
| [AttackBonus][types] | 直接攻击加值兼容字段 | 通用 float 非负；**当前实例装备必须为 0** | 加攻击可引用AttackFlat，或AttributeFlat→AttackDamage；不可同时填这里 |
| [Effect][types] | 直接 GE 授予兼容字段 | **当前实例装备必须为空** | 不与实例来源混用；不能靠任意 GE 扩展当前装备属性 |
| [DamageEffectIds][types] | 穿戴时启用的伤害模板 | DamageEffect引用；同数组不得重复；仅Periodic／Bound | 可只填本数组，不需伪AttackFlat；卸装停止新发动，独立产物按寿命保留，绑定区域立即清除 |
| [EffectIds][types] | 穿戴时启用的效果数组 | SourceEffect引用，支持全部五种Kind；同数组不得重复，条件限制见第5节 | 卸下撤销直接属性／系数／百分比／收入；与GrowthRuleIds、DamageEffectIds三者至少一个非空 |
| [GrowthRuleIds][types] | 穿戴时生效的成长规则数组 | 元素为 GrowthRule＋行名；同数组不得重复 | 可用 EquipmentInstance＋AttackDamage 或 Player＋力量／生命上限／敏捷／智慧；含义见成长规则表 |
| [DisplayName][types] | 装备显示名 | 文本；公开装备及实际掉落产物不得空白 | 已接入；相同名字不等于相同实例；合成不要求同名 |

## 5. [DT_SourceEffect][dt-sourceeffect] — 静态来源效果

效果行本身不会生效，须由装备、已学技能、已选天赋或已购直接效果商品引用。不同有效来源可以引用同一效果行并叠加；同一个来源定义的数组中不能重复引用同一行。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Kind][types] | 效果种类 | AttackFlat／GoldPerSecond／AttributeFlat／MappingCoefficientAdd／AttributePercent／CritChance／CritMultiplierAdd／Lifesteal／ShieldPeriodic／ShieldOnCast | 前两种旧来源效果；中间三种运行时限Equipment／Base，技能／天赋／直接效果商品不放宽旧白名单；R37二期起暴击、倍率、吸血、周期护盾四种仅限装备来源，施放护盾仅限技能来源 |
| [Magnitude][types] | 旧效果幅度 | double，有限非负；后三种必须0 | AttackFlat为攻击点，GoldPerSecond为金币／秒；旧两种Amount必须0、SourceAttribute与TargetAttribute必须None |
| [SourceAttribute][types] | 映射源属性 | MappingCoefficientAdd仅Strength／Agility／Wisdom；其他Kind必须None | 禁止从派生属性出发、禁止三维互转 |
| [TargetAttribute][types] | 加值、映射或百分比目标 | AttributeFlat允许三维及六派生；MappingCoefficientAdd仅六派生；AttributePercent允许三维、五派生及GoldGain；旧Kind必须None | 六派生为AttackDamage／MaxHealth／Armor／HealthRegen／GoldPerSecond／AttackRate；百分比排除GoldPerSecond。GoldGain仅可用AttributePercent，不能固定加值或映射 |
| [Amount][types] | 新效果幅度 | double，有限非负；旧Kind必须0 | Flat按目标单位；Mapping为目标单位／源属性点；Percent用百分点，20表示+20%，同属性相加而非连乘。三维为属性点、攻击为攻击点、生命为HP、护甲为减伤点、恢复为HP／秒、收入为金币／秒、频率为次数／秒。R37二期：CritChance与Lifesteal用百分点（20=20%，暴击率合计／100后封顶1，吸血合计／100不封顶），CritMultiplierAdd为倍率直加（0.5=暴击倍率+0.5），ShieldPeriodic与ShieldOnCast为每层护盾值（HP） |
| [Condition][types] | 生命条件 | None／HealthBelow／HealthAbove，默认None | 非None仅可挂AttributeFlat／AttributePercent，目标限AttackDamage／Armor／HealthRegen／AttackRate／GoldGain；禁止三维、MaxHealth及映射条件 |
| [ConditionThreshold][types] | 生命比例阈值 | double，有限0—100；None必须0 | 30表示30%。Below严格低于、Above严格高于，恰好等于均不生效；当前Health／MaxHealth，不计护盾，零上限比例约定0 |
| [DurationSeconds][types] | 护盾层持续时长 | double，秒，有限非负；非护盾Kind必须0 | 仅ShieldPeriodic／ShieldOnCast消费；0为永久层。同源再触发在剩余值上累加至层上限并刷新到期时间 |
| [LayerCap][types] | 护盾单层上限 | double，HP，有限非负；非护盾Kind必须0 | 仅ShieldPeriodic／ShieldOnCast消费；同来源剩余值累加不超过本层上限 |
| [IntervalSeconds][types] | 周期护盾发放间隔 | double，秒，有限非负；仅ShieldPeriodic可正 | 仅ShieldPeriodic消费；装备生效首层立即获得，此后每满一个间隔按层数上限补发；ShieldOnCast必须0 |

R37第一期（百分比／生命条件）、第二期（暴击／倍率／吸血／护盾与左上血条）、第三期（时间／普攻成长与玩家侧四属性）与第四期（金币获取%消费）源码已接入，Editor构建与定向回归已通过。新增字段与Kind本批仅用内存DataTable测试，**未保存或添加正式DT样例**。原日常样例与其他窗口调值不改写；具体证据与未验边界见程序窗口续接记录。

护盾规则：按来源分层，服务器唯一真源，客户端只复制总量。受伤先扣最快到期层，同源再触发在剩余值上累加至层上限并刷新持续时间，不同来源独立相加。死亡、复活初始化与离局清空；护盾不计入生命条件比例，不可被吸血回复。测试HUD左上在血条上沿半透明蓝条显示护盾总量，为0时隐藏。

公式：有效三维＝（基础＋固定＋玩家成长）×（1＋三维百分比合计／100）；衍生小计＝基础＋固定＋装备成长＋Σ有效三维×合计系数；衍生最终＝小计×（1＋该属性百分比合计／100）。AttackFlat也进入攻击小计，所有自身攻击来源统一为一个派生加法GE；任意外部GE保留，但不纳入本来源百分比。GoldPerSecond只固定／映射；GoldGain明细Final为百分点，运行时归一化为比例；R37第四期起实际消费——自然收入速率、主线怪金币均分、清波奖励、练功房奖励各按 (1+金币获取%) 放大，小数经每玩家钱包满1入账（自然收入自带小数累计不用钱包）；属性明细仍显示基础速率与百分点，不显示乘算后值。失守结清等其他金币入口不乘。

以下是可手工填写的字段示例，不代表已新增正式资产。示例行的 `Magnitude=0`、`SourceAttribute=None`；成长贡献标记仅由运行时生成，不是配表字段。护盾两种不放此表：ShieldPeriodic 例 Amount=30、DurationSeconds=2、LayerCap=60、IntervalSeconds=1（首层立即，之后每秒补至层上限60）；ShieldOnCast 例 Amount=25、DurationSeconds=5、LayerCap=25（已学主动技能每次施放触发）。

| 示例 | Kind | TargetAttribute | Amount | Condition | ConditionThreshold |
|---|---|---|---|---|---|
| 力量＋20% | AttributePercent | Strength | 20 | None | 0 |
| 低血攻击＋50% | AttributePercent | AttackDamage | 50 | HealthBelow | 30 |
| 高血护甲＋10 | AttributeFlat | Armor | 10 | HealthAbove | 70 |
| 金币获取＋20% | AttributePercent | GoldGain | 20 | None | 0 |
| 暴击率＋20% | CritChance | None | 20 | None | 0 |
| 暴击倍率＋0.5 | CritMultiplierAdd | None | 0.5 | None | 0 |
| 吸血＋15% | Lifesteal | None | 15 | None | 0 |

日常样例：[TrialStrength10][dt-sourceeffect]为AttributeFlat→Strength、Amount=10；[TrialStrengthHealth1][dt-sourceeffect]与[TrialStrengthHealth2][dt-sourceeffect]为力量→生命系数1和2。上限增加不回血；下降仅成功提交后钳当前HP，失败不扣血。暂存条件按候选上限及提交后会钳至的生命比例计算。AttackRate基础为1／AttackInterval，先加固定／派生及频率百分比再取倒数，不重置已经等待中的普攻截止时间。

[来源运行时][sources]额外要求攻击幅度不超过 float 上限（约 \(3.402823\times10^{38}\)），正数转 float 后不能归零；攻击汇总及叠到实际当前攻击时不能溢出或完全吞掉正值。金币速率以 double 汇总，也不能溢出或完全吞掉正值；没有“允许负收入抵扣”的规则，余额入账仍受整数上限约束。

[来源收入][income]从主流程进入准备阶段启动，期间合计基础速率和所有有效来源速率，累计小数后入账整数；结束或离场停止。卸装／移除技能撤销后续速率，但不扣回已经赚到的金币。基础速率不在这张表，调单个来源也不会替换基础速率。

## 6. [DT_GrowthRule][dt-growthrule] — 成长规则（有效击杀／时间／普攻）

R37第三期起支持三种触发。EffectiveKill 只统计合法伤害造成、归属于本玩家的有效击杀；主线和本人训练房均可触发。一只怪的致死事件只消费一次，不按伤害段数、命中数或普攻发动计次，不按全队人数均分。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Trigger][types] | 成长触发事件 | EffectiveKill／CombatTime／BasicAttack | EffectiveKill=每次有效击杀；CombatTime=主线战斗（Battle）阶段按秒累计，死亡等待与练功房不暂停、卸下装备暂停该件计时、局终停止；BasicAttack=每次普攻真正发动（近战判定或飞行物发射）计1次，不按命中人数，按约1秒批延迟结算；开局即结算，不要求 Battle（开波前、练功房普攻都算），仅局终停止 |
| [Target][types] | 成长归属 | EquipmentInstance／Player | EquipmentInstance 只能被装备引用；Player 可被装备、技能、天赋及直接效果商品引用（玩家侧三触发器通用） |
| [Attribute][types] | 增长属性 | EquipmentInstance 时仅 AttackDamage；Player 时 Strength／MaxHealth／Agility／Wisdom | 装备实例不支持其他属性，玩家侧不支持 AttackDamage；生命上限与敏捷智慧为 R37第三期新增，按用户确认口径开放 |
| [Delta][types] | 每次触发增量 | 对应属性点；double，有限非负 | EffectiveKill=每击杀、CombatTime=每秒、BasicAttack=每次发动；例：CombatTime Delta=2 表示战斗阶段每秒+2。应用时单次及累计须可表示为有限非负 float，正增量不能转成 0、溢出或被吞值 |

[成长消费者][growth]按触发器分路结算：击杀批只带 EffectiveKill 规则，时间与普攻成长由约1秒的 Tick 批统一结算（时间成长只在 Battle 阶段计时；普攻成长局终前任何阶段都结算），两种批互不串触发器。装备实例攻击成长存回**那一件装备**：卸下保留累计但不继续增长（时间成长该件计时同时暂停），也不贡献攻击，重穿恢复；免费合成消耗材料后，产物是新实例，不继承材料累计。玩家侧成长直接累加在玩家账本：撤销来源不会扣回已得，但不再产生新增长；力量／敏捷／智慧进三维值，经既有映射派生攻击、生命等；生命上限成长进换算链上限小计（先于衍生百分比），**上限升当前生命同步加同差值（所有来源，含装卸装备；0血死亡态除外）、降按现有血量规则钳制**。明细面板中四项玩家成长均显示为"玩家成长"贡献行。

## 7. [DT_Talent][dt-talent] — 天赋

须出现在当前天赋选择行的三个候选中，且被玩家选中才启用；写入天赋表不等于自动获得。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Quality][types] | 天赋品质 | 五品质之一 | 当前用于显示；不自动决定候选概率、效果倍数或解锁等级 |
| [Ability][types] | 直接 GA 授予兼容字段 | 当前公开候选必须为空 | 当前天赋只使用来源效果和玩家成长，不授予此类技能 |
| [Effect][types] | 直接 GE 授予兼容字段 | 当前公开候选必须为空 | 不可和效果 ID 或成长规则混填 |
| [EffectIds][types] | 选中后启用的效果数组 | SourceEffect引用，仅AttackFlat／GoldPerSecond；不得重复 | 可空，但与成长规则数组至少一项非空 |
| [GrowthRuleIds][types] | 选中后生效的成长数组 | GrowthRule 引用；不得重复；只允许 Player＋力量／生命上限／敏捷／智慧 | 不支持装备实例成长；可与静态效果同时配置 |
| [DisplayName][types] | 候选与已选天赋的显示名 | 文本；公开候选不得空白 | 已接入；不同候选须有不同真实行身份，不是靠不同显示名区分 |

## 8. [DT_TalentChoice][dt-talentchoice] — 三选一天赋

正式入口由 [DT_MatchRules][dt-matchrules]／Default 的 [Startup.TalentChoice][types] 引用，当前为 TalentChoice／EvidenceChoice。当前是每位玩家本局一次的固定三选一，不是升级抽样表；显式旧Profile入口仍使用其 TalentChoice 引用。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Candidates][types] | 三个天赋候选引用 | **恰好 3 项**；当前 R36 要求三个不同真实行 | 顺序用于候选显示；不是权重数组，不随机补满 |
| [Candidates[].Type][types] | 候选的表类型 | Talent | 只能引用天赋，不能填技能或装备 |
| [Candidates[].RowName][types] | 候选天赋行名 | 天赋表真实行；当前三项不得重复 | 该行必须满足天赋表的空 GA／GE 与有效来源要求 |
| [RefreshesPerCandidate][types] | 每个候选刷新次数字段 | 通用目录校验**必须等于 1** | **当前未接入，改了无玩法效果**；R36 不提供刷新，选定后不可换选；填 0 或 2 会校验失败，并不会关闭或增加刷新 |

## 9. [DT_ShopOffer][dt-shopoffer] — 商品

正式入口自动发现全表中 [Enabled][types]=true 的合法商品，并按 [SortOrder][types]、RowName 稳定排序；结构默认 Enabled=true、SortOrder=0，因此新增合法商品默认上架。随机商品可重复购买；直接效果商品每位玩家每局最多购买一次。商品 ID 不决定商品类型或实际品质；显式旧Profile入口仍受 ContentShopOffers 限制。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Enabled][types] | 是否在正式入口上架 | bool；结构默认true | false不进入正式商品列表；不用于绕过其它字段和引用合法性 |
| [SortOrder][types] | 商品排序键 | int32；结构默认0 | 升序排列；同值按RowName稳定排序，不是价格或品质 |
| [Pool][types] | 单品质随机物品池 | RandomPool：Type=LocalItemPool且RowName为真实行；其他类型须完全空 | 池内物品类型须为装备或技能，池品质须等于商品品质 |
| [TierPool][types] | 随机品质池 | RandomTier：QualityRollPool 引用；其他类型须完全空 | 每件产物先在此抽品质池项，再在该项局内池抽物品 |
| [Quality][types] | 单品质商品标示品质／中性占位 | 五品质之一；DirectEffect、RandomTier **必须 Green** | RandomPool 与物品池品质相同；RandomTier 的 Green 不是强制抽绿色；直接效果不进行品质抽样 |
| [Price][types] | 一次购买总价 | 金币；非负整数 | 例：Price=100、Quantity=2 是付 100 发两件，不是每件 100；0 为免费购买 |
| [Quantity][types] | 每次购买交付数量 | 随机商品为正整数；DirectEffect **必须 1** | 随机商品每件分别抽样生成独立实例；受容器及序号可表示范围限制 |
| [ShopKind][types] | 商品交付方式 | RandomPool／DirectEffect／RandomTier | 单品质随机物品／直接永久本局来源／随机品质后随机物品 |
| [EffectIds][types] | 直接效果商品的效果数组 | DirectEffect仅可填旧AttackFlat／GoldPerSecond的SourceEffect引用；不得重复；随机商品必须空 | 与成长数组至少一个非空；购买成功后启用，不发装备或书 |
| [GrowthRuleIds][types] | 直接效果商品的成长数组 | DirectEffect 可填 GrowthRule；不得重复；只允许 Player＋Strength；随机商品必须空 | 购买后未来有效击杀加力量，不支持装备实例成长 |
| [DisplayName][types] | 商店商品名 | 文本；公开商品不得空白 | 已接入；显示名和抽中的装备／技能名称互不替代 |

**DirectEffect 的完整组合是：**[Pool][types] 完全空、[TierPool][types] 完全空、[Quality][types]=Green、[Quantity][types]=1，且两组来源数组至少一组非空。不能用一个非空池配合直接效果“顺便发装备”。

## 10. [DT_LocalItemPool][dt-localitempool] — 局内物品池

供商店、合成及怪物物品包引用；只有被实际使用的引用链触达才会抽取。专用配置还会校验表内所有池的物品定义，包括只被间接引用的池。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [ItemType][types] | 池内统一物品类型 | Equipment／Skill | 一池不能混装备与技能，也不能混局外物品 |
| [Quality][types] | 池内统一品质 | 五品质之一 | 每个被引用物品的品质必须相同，零权重条目也不例外 |
| [Entries][types] | 加权物品数组 | 非空；总权重有限且大于 0 | 当前 R36 同池物品行引用还须不重复；不得以复制同一物品条目代替加权 |
| [Entries[].Item][types] | 一个候选物品引用 | 类型等于本池 ItemType，且目标行品质等于本池 Quality | 内含 Type／RowName，规则见通用引用；装备、技能须满足当前实例定义限制 |
| [Entries[].Weight][types] | 候选物品相对权重 | float，有限非负 | 例：1 和 3 的两项概率是 25% 与 75%；0 不抽中但仍必须引用合法行；不要求总和为 100 |

[经济消费者][economy]对实际抽样池还检查精度：所有正区间必须在 23 位随机票域内至少含一个可表示票值，权重求和也不能完全吞值。因此不要把极小正权重和极大权重混用；结构校验通过不代表任意极端比例都可运行。

## 11. [DT_QualityRollPool][dt-qualityrollpool] — 品质抽样池

当前供 RandomTier 商品引用；不是怪物额外掉率表。最终某物品概率是“所在品质项概率 × 该品质局内池中物品概率”。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [ItemType][types] | 整个品质池产物类型 | Equipment／Skill | 所有条目引用的局内池都须同类型 |
| [Entries][types] | 品质加权数组 | 非空；每种品质最多一项；总权重有限且大于 0 | 可以只配部分品质，不要求五色齐全 |
| [Entries[].Quality][types] | 此项品质 | 五品质之一；同数组不得重复 | 必须等于此项引用局内池的品质 |
| [Entries[].Pool][types] | 此品质对应的局内物品池 | LocalItemPool＋真实行；类型、品质均须匹配 | 内含 Type／RowName；不能填另一张品质池形成多层套娃 |
| [Entries[].Weight][types] | 抽到此品质项的相对权重 | double，有限非负 | 例：绿 3、蓝 1 为绿 75%、蓝 25%；0 不抽中；同样受正区间票域精度限制 |

## 12. [DT_Synthesis][dt-synthesis] — 合成

正式入口自动发现全表中 [Enabled][types]=true 的合法配方，并按 [SortOrder][types]、RowName 稳定排序；结构默认 Enabled=true、SortOrder=0，同类型、同输入品质只能启用一个配方。当前**免费五合一**：五件同类型同品质的背包实例换一件高一品质的新实例，不要求同名；不能消耗已穿装备或已学技能，也不能把同一件材料重复选五次。显式旧Profile入口仍受 ContentRecipes 限制。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Enabled][types] | 是否在正式入口启用 | bool；结构默认true | false不进入正式配方列表 |
| [SortOrder][types] | 配方排序键 | int32；结构默认0 | 升序排列；同值按RowName稳定排序 |
| [InputType][types] | 材料物品类型 | Equipment／Skill | 五件都须同类型，不能装备与书混合 |
| [InputQuality][types] | 五件材料的共同品质 | Green／Blue／Purple／Gold；**禁止 Red** | 例：Green 材料只能产出 Blue；红色到顶 |
| [InputCount][types] | 材料件数 | **固定整数 5** | 不是可自由调整的成本参数；填 3 或 6 会失败 |
| [OutputPool][types] | 产物抽样池 | LocalItemPool＋真实行 | 池类型与输入相同，池品质必须恰好高一级；每次只生成一个新实例 |

没有金币成本字段；商品价格不影响合成。材料被消耗，材料实例攻击成长不转给产物；玩家已经获得的力量不会因材料消耗倒扣。

## 13. [DT_Wave][dt-wave] — 主线波次

正式入口自动发现全表中 [Enabled][types]=true 的波次，按 [Order][types]、RowName 稳定排序；新增启用行会参与主线，不再登记 OrderedWaves。当前四波为 Wave1=10、Wave2=20、Wave3=30、FourthWatch=40。当前主关卡自动倒计时出波：首波从玩家与配置就绪时计时，后续每波从上一波开始生成时计时。旧怪未清不阻挡下一波，旧波的延迟批次照常生成。每波自身全部生成且全灭才独立结算一次；最后一波全部生成且全场主线怪清空后胜利。显式旧Profile入口仍使用 OrderedWaves。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Enabled][types] | 是否进入正式主线 | bool；结构默认true | false不进入正式波次序列 |
| [Order][types] | 正式波次顺序键 | int32；结构默认0 | 升序排列；同值按RowName稳定排序 |
| [Type][types] | 波次分类 | Normal／Boss | Boss 参与更长准备时间校验；不会自动增加血量、数量或替换为 BOSS 怪 |
| [PreparationSeconds][types] | 本波开始生成前的倒计时时长 | 秒；float，有限非负 | 首波从就绪时计；后续从上一波开始生成时计，时间到不等旧怪清完。例：第二波填15，即第一波开始生成后约15秒开始第二波。0在下一次可执行权威调度出波。所有Boss行仍须严格大于整张表所有Normal行的最大值 |
| [RespawnSeconds][types] | 本波玩家基础复活等待 | 秒；float，有限且必须大于0；日常每行10 | 死亡时锁定最近已开始波；首波前取第一波；后续开波不重算本次等待。只影响玩家复活，不是怪物再刷时间；零秒仅由后续方案策略覆盖，不把普通表填0 |
| [Schedule][types] | 波内刷怪模式 | Legacy／Batches | Legacy要求Spawns非空且Batches空；Batches反之。日常四波使用Batches |
| [Spawns][types] | 旧模式刷怪数组 | 仅Legacy非空；Batches必须空；全波Count求和不超int32 | 不循环；每个元素各调度一次。例：一项近战两只、一项远程一只 |
| [Spawns[].Monster][types] | 刷怪项的怪物引用 | Monster＋真实行；内含 Type／RowName | 引用决定该批怪物的战斗属性与逐怪物品掉落 |
| [Spawns[].Count][types] | 此项生成数量 | 只；正整数 | 例：3 是在该项触发时生成 3 只，不是每秒三只 |
| [Spawns[].DelaySeconds][types] | 本波倒计时结束后，该批怪物的额外延迟 | 秒；float，有限非负 | 例：本波等待10秒、此批延迟2秒，则从本波倒计时起点算约12秒生成。各批都相对本波开始生成时刻，不相对上一批；即使期间已开始下一波，此批仍按原计划生成 |
| [Reward][types] | 本波独立清怪后的数值奖励包 | RewardPackage＋真实行；当前必须 MatchPersonal 且 Items 为空 | 本波待生成数和存活数均为0后结算，可与其它波乱序完成。金币、经验整份发给每个结算玩家一次，**不是按怪发放，也不是除以人数均摊** |

### [Batches][types] 与出生点

| UE字段名 | 中文含义 | 单位与合法范围 | 当前配置／作用 |
|---|---|---|---|
| [Batches][types] | 点位批次列表 | Batches模式必须非空 | 同刻可配多个批次；不会自动按人数放大 |
| [Batches[].BatchId][types] | 批次身份 | 非空FName，波内唯一 | 日常例Wave1_1_1、Wave1_1_2同刻双点 |
| [Batches[].OffsetSeconds][types] | 相对本波开始生成的偏移 | float，秒，有限非负 | 不相对上一批；日常首波0／0／2秒 |
| [Batches[].PointKind][types] | 出生点类型 | Normal／Boss | 必须与场景点类型相同，与Wave.Type不混淆 |
| [Batches[].PointTag][types] | 稳定点引用 | Normal为无前导零的正int32数字；Boss为Boss | 不填Actor显示名或对象路径；日常引用1、2 |
| [Batches[].Monsters][types] | 同批混合怪物数组 | 非空 | 每项生成Count只，可同批混怪 |
| [Batches[].Monsters[].Monster][types] | 怪物引用 | Type=Monster、RowName真实行 | 全部项仍用旧Monster数值 |
| [Batches[].Monsters[].Count][types] | 固定生成数量 | 正int32；全波求和不超int32 | 直接数量，不乘人数；日常四波总量3／4／4／2 |

场景放置 [Y01SpawnPoint][spawnpoint]：Actor Tags恰好一个，Normal为规范数字，Boss类型配Boss Tag，全场Tag唯一；编辑器箭头可见。日常普通1号(650,-240,150)、2号(650,240,150)、Boss(850,0,150)，单位cm；Boss预留而不改变原四个普通波。胶囊排布按半径×2＋8间距，以地面射线和阻挡重叠检测选点；每怪最多128候选、每批游标最多4096。生成中心为地面＋胶囊半高＋2；无合法地面／空间则诊断并终止局，不能依赖静默漏怪。

[波次奖励消费者][flow]不支持在清波包里填物品；填了非空 [Items][types] 会拒绝配置，而不是变成怪物掉落或静默忽略。怪物掉落请使用怪物自身的物品掉落包引用。

## 14. [DT_TrainingStage][dt-trainingstage] — 训练房阶段

训练已启用时，这张表全部行按“**本次进入训练房后的经过秒数**”选阶段；不是对局总时间，也不是击杀升级。入房立即调度一批，离房后重入从零计时；切阶段不清理旧怪，也不重新赋予旧怪奖励。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Stage][types] | 阶段编号 | 正整数；全表不得重复 | 用于阶段标识和起点排序校验；不要求编号连续，不是玩家等级 |
| [StartSeconds][types] | 本阶段开始适用的入房经过时间 | 秒；float，有限非负；全表起点唯一 | 按 Stage 升序，首项必须 0，其后严格递增；例：0、30 表示第 30 秒起可选第二阶段 |
| [SpawnInterval][types] | 当前一批到下一批的调度间隔 | 秒；float，必须大于 0 | 例：3 每隔 3 秒调度一批；下一批按当时经过时间重新选阶段，非到起点立刻额外补刷 |
| [MaxAlive][types] | 房内怪物存活数量上限 | 只；整数 1～256 | 排批时已活怪和待生成数都占名额；实际生成再次看当前上限；不会强杀超过新上限的旧怪 |
| [Spawns][types] | 每批重复使用的刷怪数组 | 非空 | **每批遍历所有项**，不是随机抽一项；前项先占容量，后项可能被截少或不生成 |
| [Spawns[].Monster][types] | 此项怪物引用 | Monster＋真实行；内含 Type／RowName | 每批按此行生成；其有效击杀仍可另触发怪物物品掉落 |
| [Spawns[].Count][types] | 每批该项期望数量 | 只／批；正整数 | 受当前剩余容量裁剪；例：期望 5、剩余 2，只安排 2 |
| [Spawns[].DelaySeconds][types] | 本批调度后该项的生成延迟 | 秒；float，有限非负 | 相对**每一批起点**，不相对入房起点；可以跨阶段，待生成项已保存怪物与该批奖励包 |
| [Reward][types] | 该批怪物每次有效击杀的数值奖励 | RewardPackage＋真实行；必须 MatchPersonal 且 Items 为空 | 同一批生成的**每只**怪都携带一份该包；仅房主合法击杀后发给房主，不是整批清空奖、阶段完成奖或周期自动收入 |

[训练消费者][training]保存排批时的数值奖励包；旧怪被击杀仍用其原包，不读当前新阶段包。怪物的 [Gold][types]／[Experience][types] 不覆盖训练包；怪物物品掉落走独立链，可以与训练数值奖励同时发生。

## 15. [DT_RewardPackage][dt-rewardpackage] — 奖励包

一个包可被多处引用，但同一包必须满足**所有使用处**的限制；不能把清波包直接改成物品包后仍留作清波奖励。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Domain][types] | 奖励归属域 | MatchPersonal／MetaPublic | 当前清波、训练、怪物物品掉落均用 MatchPersonal；MetaPublic 当前未接入，改了无玩法效果 |
| [Experience][types] | 包含的经验 | 经验点；非负整数 | 清波按玩家、训练按击杀入账；物品掉落包必须 0。当前只累加经验，不执行升级 |
| [Gold][types] | 包含的金币 | 金币；非负整数 | 清波逐人×(1+各自金币获取%)、训练按归属×(1+)入账，小数经玩家钱包满1取整；物品掉落包必须 0；不是每秒金币速率 |
| [Items][types] | 物品奖励数组 | 通用允许空；当前清波／训练必须空，正概率怪物掉落必须非空 | 怪物掉落触发一次会展开**所有**项，不是在这些项之间抽一个 |
| [Items[].Item][types] | 一项奖励物品或池引用 | MatchPersonal：Equipment／Skill／LocalItemPool；MetaPublic：MetaItem／MetaLootPool | 内含 Type／RowName；当前物品掉落只消费个人局内三种类型，不支持 QualityRollPool |
| [Items[].Quantity][types] | 这一项交付次数 | 件或抽取次数；正整数 | 直接物品例：2 发两个实例；局内池例：2 独立抽两次。掉落包总数量及待交付容器须在整数可表示范围内 |

## 16. [DT_Quality][dt-quality] — 品质定义

全表须把五种品质各定义一次；这一要求独立于玩法是否读取显示名。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Quality][types] | 此行代表的品质 | 五品质之一；全表不重复且不可缺任何一种 | 当前参与完整性校验；不是添加第六种品质的入口 |
| [DisplayName][types] | 品质显示名称 | 文本；目录未要求非空 | **当前未接入，改了无玩法效果**；当前 UI 的绿／蓝／紫／金／红文字和品质颜色由[UI代码][ui]固定，不读此字段 |

## 17. [DT_TowerFloor][dt-towerfloor] — 爬塔层（预留）

**整表当前未接入，改了无玩法效果**；仍是必需非空表，其引用和数值必须合法，不会自动生成主线或训练怪。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Floor][types] | 塔层编号 | 正整数；全表不重复 | 例：3 表示编号 3；没有当前爬塔推进消费者 |
| [Spawns][types] | 此层刷怪数组 | 非空 | 结构预留；当前不会执行 |
| [Spawns[].Monster][types] | 怪物定义引用 | Monster＋真实行；内含 Type／RowName | 仍须有有效目标行 |
| [Spawns[].Count][types] | 该刷怪项数量 | 只；正整数 | 例：4 表示该项数量 4；当前不生成 |
| [Spawns[].DelaySeconds][types] | 刷怪项延迟字段 | 秒；float，有限非负 | 当前没有塔层运行时，不能把它说成已实现的“入层后计时” |
| [Reward][types] | 塔层奖励包引用 | RewardPackage＋真实行 | 通用结构要求存在；当前无发奖时点或塔层结算 |

## 18. [DT_LevelExperience][dt-levelexperience] — 等级经验（预留）

**当前未接入，改了无玩法效果**；经验余额增加不代表会升级。本表仍必须满足等级覆盖校验。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Level][types] | 经验配置对应的等级编号 | 正整数；全表不重复 | 需覆盖所有对局规则要求的 1 至 MaxLevel；不是当前玩家等级 |
| [RequiredExperience][types] | 等级关联的经验需求值 | 经验点；正整数 | 例：100 仅表示该行需求 100；当前没有升级消费者，未定义为“升入本级”“升出本级”或累计阈值，不可据此推导曲线 |

## 19. [DT_MatchRules][dt-matchrules] — 对局启动、核心与表现

正式入口固定读取 MatchRules／Default：从 [Startup][types] 构建对局入口，核心规则仍读取 [Core][types]，飘字读取 [Presentation][types]。下表“结构默认”来自C++新行默认值，“当前”是迁移后的Default行，两者不要混写；显式旧Profile入口仍保留原配置字段。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [MaxLevel][types] | 要求经验表覆盖到的等级 | 正整数 | 例：5 要求经验表有 1、2、3、4、5 级。**当前未接入，改了无玩法效果**；只参与结构覆盖校验，不是当前玩法封顶等级 |
| [Startup.SchemaVersion][types] | 启动配置版本 | int32；结构默认0 | 当前1；迁移身份，不是策划数值开关 |
| [Startup.Character][types] | 出生角色定义 | Character＋真实RowName | 当前 Character／Default |
| [Startup.bTrainingEnabled][types] | 启用练功房 | bool；结构默认false | 当前true |
| [Startup.bR34AutoAttackEnabled][types] | 启用R34自动攻击 | bool；结构默认false | 当前true |
| [Startup.bEconomyEnabled][types] | 启用局内经济 | bool；结构默认false | 当前true |
| [Startup.bCoreDefenseEnabled][types] | 启用核心防守 | bool；结构默认false | 当前true |
| [Startup.TalentChoice][types] | 本局三选一配置 | TalentChoice＋真实RowName | 当前 TalentChoice／EvidenceChoice |
| [Startup.InitialGold][types] | 出生金币 | 非负int32；结构默认0 | 当前0 |
| [Startup.BaseGoldPerSecond][types] | 基础自然收入 | 金币／秒；double，结构默认0，有限非负 | 当前1；不要把当前资产值误写成结构默认 |
| [Startup.DropFlightSeconds][types] | 掉落飞行时长 | 秒；double，结构默认1，有限非负 | 当前1 |
| [Core.MaxHealth][types] | 全场唯一核心初始生命上限 | HP；float，有限且大于0 | 日常1000；全场共享，不为每位玩家生成一份血量 |
| [Core.HealthRegen][types] | 核心持续回血 | HP／秒；float，有限非负 | 日常5；0关闭。战斗、受击、准备均回复，跨波不回满；归零后不复起 |
| [Core.HitAlertInterval][types] | 受击提示节流间隔 | 秒；float，有限且大于0 | 日常3；限制提示频率，不是伤害免疫间隔 |
| [Core.CriticalHealthRatio][types] | 危急生命比例阈值 | 比例；float，有限，0＜值≤1 | 日常0.3；当前生命／上限**严格低于**阈值才危急，恰好30%不算 |
| [Presentation.DamageColor][types] | 伤害飘字颜色 | FLinearColor；结构默认红 | 当前红，可在Presentation调整 |
| [Presentation.HealingColor][types] | 治疗／回血飘字颜色 | FLinearColor；结构默认绿 | 当前绿，可在Presentation调整 |
| [Presentation.RegenMergeSeconds][types] | 自然回血合并窗口 | 秒；double，结构默认1，有限且至少0.001 | 当前1；每目标独立汇总 |
| [DefaultCritMultiplier][types] | 基础暴击倍率 | 倍；double，有限且大于0；结构默认2 | 暴击伤害=基础×(本值＋攻击方暴击倍率加成合计)，再走减甲、护盾与生命截断；配置在TryConfigureMatch真实路径读入GameMode，改后新配置对局生效 |
| [Presentation.CriticalColor][types] | 暴击飘字颜色 | FLinearColor；结构默认橙金(1,0.45,0,1)，分量须有限 | 暴击伤害飘字用此色；未暴击走DamageColor，治疗走HealingColor——三色飘字已接入测试HUD |

飘字口径：伤害显示“扣甲后、按目标剩余生命截断前”的结算量，实际扣血仍用于奖励与击杀归属；护甲后为0不显示。治疗显示有效恢复，满血不显示；自然回血按每目标 [Presentation.RegenMergeSeconds][types] 汇总。初始化、复活和生命上限调整不显示飘字。

核心模式行为：
- 怪物优先发现范围内最近合法玩家；无合法玩家时主线怪转向核心，私房怪不得攻击核心；不把核心改为阻挡碰撞。
- 玩家死亡按[RespawnSeconds][types]锁定等待，普通复活在核心实际胶囊及外观占地外缘＋玩家胶囊半径＋8cm安全间隔外搜点；保留地面、坡度及阻挡检测，无安全点继续等待。复活用当前有效[MaxHealth](file:///G:/GarbageProject/Y01/Source/Y01/Player/Y01ReviveComponent.cpp)满血，无无敌。
- 金币、背包、装备、成长、技能与CD保留；自然收入暂停但保留小数；死亡期间服务器拒绝主动操作，掉落按原ETA到账。死亡清本人攻击效果，复活周期效果重新等完整周期。
- 核心归零只触发一次失败流程：冻结战斗／操作，取消复活、练功房返回；已归属掉落结清后才结束，不扣、不竞拍。
- [PlanPolicy](file:///G:/GarbageProject/Y01/Source/Y01/Player/Y01ReviveComponent.h)仅预留后续十字章原地零秒或减时扩展；原地策略可优先合法死亡点，但仍排除核心体积与其他阻挡。本次不实现道具和叠加规则。
- 调值仅停止Play→保存当前正式DT→再Play生效，不改用户其他既有值。源码及资产验收范围以文首DONE_WITH_CONCERNS及未验边界为准。

## 20. [DT_MetaItem][dt-metaitem] — 局外物品（预留）

**整表当前未接入，改了无玩法效果**；仍须非空且品质合法，不会出现在当前局内背包。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Quality][types] | 局外物品品质 | 五品质之一 | 被局外池引用时须与池品质一致 |
| [DisplayName][types] | 局外物品名称 | 文本；目录未要求非空 | 当前无局外仓库 UI 消费 |
| [Icon][types] | 局外物品图标 | UTexture2D 软引用；可空 | 当前无加载显示消费者 |

## 21. [DT_MetaLootPool][dt-metalootpool] — 局外物品池（预留）

**整表当前未接入，改了无玩法效果**；目录已有通用抽样能力不代表当前主关卡有局外抽取或发货入口。

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [Quality][types] | 池内统一品质 | 五品质之一 | 所有候选局外物品品质须相同 |
| [Entries][types] | 局外物品加权数组 | 非空；总权重有限且大于 0 | 当前不参与怪物局内掉落 |
| [Entries[].Item][types] | 局外候选物品引用 | MetaItem＋真实行；品质与池一致 | 内含 Type／RowName；不能引用局内装备、技能或其他池 |
| [Entries[].Weight][types] | 候选相对权重 | float，有限非负 | 例：2 与 6 代表通用抽样比例 1∶3；0 不抽中但引用仍须合法；当前无玩法抽样入口 |

## 22. [DT_WarehouseEvent][dt-warehouseevent] — 仓库升级事件（预留）

| UE 字段名 | 中文含义 | 单位与合法范围 | 例／引用条件／当前作用 |
|---|---|---|---|
| [UpgradeCount][types] | 要提升的现有仓库物品件数 | 件；正整数 | 例：2 指两件物品，不是某件升两级。**当前未接入，改了无玩法效果**；字段未定义挑选目标策略、升级级数或实际执行入口 |

## 23. [DT_AttributeMapping][dt-attributemapping] — 基础三维映射

表类型AttributeMapping=23。正式入口自动发现 [bBaseEnabled][types]=true 的基础映射；结构默认false，新增行须显式启用。当前六行 BaseStrengthAttack、BaseAgilityArmor、BaseWisdomIncome、Tpl_Map_StrengthHealth5、Tpl_Map_AgilityAttackRate001、Tpl_Map_WisdomRegen002 均为true。显式旧Profile入口仍使用 BaseAttributeMappings 数组。

| UE字段名 | 中文含义 | 单位与合法范围 | 日常样例 |
|---|---|---|---|
| [bBaseEnabled][types] | 是否作为正式基础映射 | bool；结构默认false | 当前迁移六行均true；false不进入正式基础映射集合 |
| [SourceAttribute][types] | 三维源属性 | Strength／Agility／Wisdom | BaseStrengthAttack用Strength |
| [TargetAttribute][types] | 派生目标 | AttackDamage／MaxHealth／Armor／HealthRegen／GoldPerSecond／AttackRate | 只开放六种目标，无移速、治疗、护盾、三维互转 |
| [Coefficient][types] | 每点源属性派生量 | double，有限非负；单位为目标单位／源属性点，非百分比；目标单位见第5节Amount | 当前冷读BaseStrengthAttack→AttackDamage=0.5；BaseAgilityArmor→Armor=0.02；BaseWisdomIncome→GoldPerSecond=0.01；外部另增力量→MaxHealth=5、敏捷→AttackRate=0.01、智慧→HealthRegen=0.02 |

基础映射与装备规则的同向系数相加。角色原三维仍各0；旧实测护符力10／攻击32／上限310→330属于模板改值前历史。按当前冷读系数推导，只穿护符为力10、攻击35、上限360，再穿增幅为380；这是配置推导，本轮未重跑该装备链。上限增加不回复当前HP；收入仍走原资格和分段累计，不新增金币表。数值计算仍受有限性、float可表示性和正增量不能吞值限制。

## 24. [DT_DamageEffect][dt-damageeffect] — 共用伤害模板

表类型DamageEffect=24。由装备／技能 [DamageEffectIds][types] 引用；天赋和直接效果商品未接本模板。伤害公式为固定值＋各属性×对应系数，再走现有伤害、护甲和击杀链；系数0.5表示50%，没有通用百分比增益字段。

| UE字段名 | 中文含义 | 单位与合法范围 | 当前作用／日常样例 |
|---|---|---|---|
| [Trigger][types] | 发动方式 | Periodic／Active／Bound | 装备与被动技能仅Periodic／Bound；主动技能仅Active |
| [Targeting][types] | 选敌形状 | NearbyCount／SelfCircle／RemoteCircle | 附近N个／自身圆／远处圆；均按XY半径 |
| [Order][types] | 附近N个顺序 | Nearest／Random | NearbyCount消费；随机由服务器决定 |
| [Center][types] | 远处圆心来源 | NearestEnemy／RandomEnemy／RandomGround | RemoteCircle消费；随机地面在搜索圆内，目前仍需存在搜索范围内合法敌人才能发动 |
| [Product][types] | 伤害产物 | Instant／Projectile／AttachedDot／IndependentArea／BoundArea | 即时／飞行／挂身DOT／独立区域／绑定区域；不是Status或Area枚举 |
| [Position][types] | 区域位置方式 | Fixed／FollowPlayer | 非区域只能Fixed；日常火圈FollowPlayer，毒区Fixed |
| [TargetCount][types] | 附近目标上限 | int32，1～256 | NearbyCount消费；脉冲与叠毒最多2个 |
| [SearchRadius][types] | 选敌搜索半径 | double，cm，有限非负且不超float上限 | 非SelfCircle必须正；日常通常1000，主动飞弹1500 |
| [Radius][types] | 圆形作用半径 | double，cm，有限非负且不超float上限 | SelfCircle／RemoteCircle必须正；日常区域240 |
| [FixedDamage][types] | 公式常数 | double，伤害点，有限非负且不超float上限 | 飞弹5，其他样例2 |
| [Terms][types] | 属性公式项数组 | 0～4项，属性不重复 | 可空表示固定伤害 |
| [Terms[].Attribute][types] | 公式读取属性 | Strength／Agility／Wisdom／AttackDamage | 不允许生命、护甲、收入等作为公式源 |
| [Terms[].Coefficient][types] | 对应属性倍率 | double，有限非负且不超float上限 | 飞弹依次0.5／0.25／0.25／0.5；最终结果也须有限且可表示float |
| [PeriodSeconds][types] | 周期发动间隔 | double，秒，有限非负且不超float上限；Periodic至少0.001 | 脉冲3、毒区4、叠毒2；主动CD只看Skill.CooldownSeconds |
| [Lifetime][types] | 单个产物寿命 | double，秒，有限非负且不超float上限 | Instant不消费；Projectile／AttachedDot／IndependentArea至少0.001；BoundArea必须0 |
| [TickSeconds][types] | DOT／区域跳伤间隔 | double，秒，有限非负且不超float上限；DOT／区域至少0.001 | 日常1秒；与发动间隔、寿命互相独立 |
| [Speed][types] | 飞行速度 | double，cm／秒，有限非负且不超float上限；Projectile必须正 | 日常飞弹600 |
| [Stacking][types] | 挂身DOT叠加方式 | Refresh／Independent | 仅AttachedDot可Independent；其他产物必须Refresh |
| [MaxStacks][types] | DOT最大独立层数 | int32，1～256；Refresh必须1 | 日常叠毒3；区域独立性不通过此字段增加层数 |
| [bLifesteal][types] | 该模板伤害可否吸血 | bool；结构默认false | Periodic／Bound保持false（DOT／区域不吸血）；主动技能可显式true。开启后按攻击方吸血属性、以目标实际生命损失结算回复，走绿色治疗飘字 |

组合约束：Bound必须且只能配BoundArea，SelfCircle、Lifetime=0；日常绑定样例配FollowPlayer。两种区域禁止NearbyCount。主动技能必须使用原生DamageAbility、正CD、TestSkill.Mode=None；被动无GA且CD=0。旧TestSkill区域／球与新DamageEffectIds不能同时启用。

生命周期：周期再穿重新等满周期；绑定穿即建、卸即失效。飞行释放时快照，DOT每跳取当前有效属性；卸装不保留装备加成，但已生成独立DOT／区域留至寿命结束。区域首次进入立即跳，重入不补首跳且保留节拍，不同区域独立，到期不再跳。死亡、切区、退出清理本人产物。R37来源属性百分比与[bLifesteal][types]已接入；本伤害模板没有通用百分比增益字段，治疗词条、护盾（走第5节来源效果）、真正重连及R37其余期未开放。

[dt-attributemapping]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_AttributeMapping.uasset
[dt-damageeffect]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_DamageEffect.uasset
[spawnpoint]: file:///G:/GarbageProject/Y01/Source/Y01/Match/Y01SpawnPoint.h
[types]: file:///G:/GarbageProject/Y01/Source/Y01/Data/Y01DataTypes.h
[catalog]: file:///G:/GarbageProject/Y01/Source/Y01/Data/Y01DataCatalog.cpp
[r36profile]: file:///G:/GarbageProject/Y01/Source/Y01Tests/Private/Playground/Y01R36Profile.cpp
[r36profile-h]: file:///G:/GarbageProject/Y01/Source/Y01Tests/Private/Playground/Y01R36Profile.h
[baseprofile-h]: file:///G:/GarbageProject/Y01/Source/Y01Tests/Private/Playground/Y01PlaygroundProfile.h
[testgrowth]: file:///G:/GarbageProject/Y01/Source/Y01Tests/Private/Playground/Y01PlaygroundGrowth.cpp
[growth]: file:///G:/GarbageProject/Y01/Source/Y01/Player/Y01GrowthComponent.cpp
[sources]: file:///G:/GarbageProject/Y01/Source/Y01/Player/Y01SourceEffectsComponent.cpp
[income]: file:///G:/GarbageProject/Y01/Source/Y01/Player/Y01IncomeComponent.cpp
[economy]: file:///G:/GarbageProject/Y01/Source/Y01/Player/Y01EconomyComponent.cpp
[drop]: file:///G:/GarbageProject/Y01/Source/Y01/Player/Y01DropDeliveryComponent.cpp
[flow]: file:///G:/GarbageProject/Y01/Source/Y01/Match/Y01GameFlow.cpp
[training]: file:///G:/GarbageProject/Y01/Source/Y01Tests/Private/Playground/Y01TrainingRooms.cpp
[testskill]: file:///G:/GarbageProject/Y01/Source/Y01Tests/Private/Playground/Y01TestSkill.cpp
[combat]: file:///G:/GarbageProject/Y01/Source/Y01/Combat/Y01Combat.cpp
[ui]: file:///G:/GarbageProject/Y01/Source/Y01Tests/Private/Playground/Y01R36UI.cpp
[map]: file:///G:/GarbageProject/Y01/Content/Developers/Y01PlaygroundTests/R36/Runs/S4_AddedRows_20261001_01/Maps/L_Y01R36.umap
[profile]: file:///G:/GarbageProject/Y01/Content/Developers/Y01PlaygroundTests/R36/Runs/S4_AddedRows_20261001_01/Content/DA_R36Profile.uasset
[dt-character]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_Character.uasset
[dt-monster]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_Monster.uasset
[dt-skill]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_Skill.uasset
[dt-equipment]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_Equipment.uasset
[dt-sourceeffect]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_SourceEffect.uasset
[dt-growthrule]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_GrowthRule.uasset
[dt-talent]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_Talent.uasset
[dt-talentchoice]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_TalentChoice.uasset
[dt-shopoffer]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_ShopOffer.uasset
[dt-localitempool]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_LocalItemPool.uasset
[dt-qualityrollpool]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_QualityRollPool.uasset
[dt-synthesis]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_Synthesis.uasset
[dt-wave]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_Wave.uasset
[dt-trainingstage]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_TrainingStage.uasset
[dt-rewardpackage]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_RewardPackage.uasset
[dt-quality]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_Quality.uasset
[dt-towerfloor]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_TowerFloor.uasset
[dt-levelexperience]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_LevelExperience.uasset
[dt-matchrules]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_MatchRules.uasset
[dt-metaitem]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_MetaItem.uasset
[dt-metalootpool]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_MetaLootPool.uasset
[dt-warehouseevent]: file:///G:/GarbageProject/Y01/Content/Y01Playground/Data/DT_WarehouseEvent.uasset
