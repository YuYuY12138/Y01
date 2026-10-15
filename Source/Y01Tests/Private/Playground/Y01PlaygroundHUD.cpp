#include "Playground/Y01PlaygroundHUD.h"
#include "Playground/Y01PlaygroundController.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "CanvasItem.h"
#include "GameFramework/CharacterMovementComponent.h"

void AY01PlaygroundHUD::NotifyHitBoxClick(FName BoxName)
{
    Super::NotifyHitBoxClick(BoxName);
    auto* PC = Cast<AY01PlaygroundController>(PlayerOwner);
    if (!PC) { return; }
    if (BoxName == TEXT("SkillDetails")) { bSkillDetailsExpanded = !bSkillDetailsExpanded; bAttributesExpanded = false; return; }
    if (BoxName == TEXT("Attributes")) { bAttributesExpanded = !bAttributesExpanded; bSkillDetailsExpanded = false; return; }
    if (BoxName == TEXT("EquipmentEquip") || BoxName == TEXT("EquipmentUnequip"))
    { PC->ServerPlaygroundAction(BoxName == TEXT("EquipmentEquip") ? 8 : 9); return; }
    if (BoxName == TEXT("TrainingEnter") || BoxName == TEXT("TrainingReturn"))
    {
        UE_LOG(LogTemp, Display, TEXT("Y01Training UI_CLICK Box=%s"), *BoxName.ToString());
        PC->ServerPlaygroundAction(BoxName == TEXT("TrainingEnter") ? 6 : 7);
    }
}
void AY01PlaygroundHUD::DrawHUD()
{
    Super::DrawHUD();
    auto* PC = Cast<AY01PlaygroundController>(PlayerOwner);
    const auto* State = PC ? PC->GetPlayerState<AY01PlayerState>() : nullptr;
    const auto* GS = GetWorld()->GetGameState<AY01GameState>();
    if (!Canvas || !PC) { return; }
    const auto& O = PC->GetObserver();
    const float Scale = FMath::Min(Canvas->SizeX / 1280.f, Canvas->SizeY / 800.f);
    auto Panel = [&](float X, float Y, float W, float H)
    {
        DrawRect(FLinearColor(0.015f, 0.025f, 0.04f, 0.88f), X * Scale, Y * Scale, W * Scale, H * Scale);
#if WITH_DEV_AUTOMATION_TESTS
        if (ObserveDrawForTest) { ObserveDrawForTest(FBox2D(FVector2D(X, Y) * Scale, FVector2D(X + W, Y + H) * Scale), FString()); }
#endif
    };
    auto Text = [&](const FString& Value, float X, float Y, FLinearColor Color = FLinearColor::White, float Size = 1.f)
    {
        FCanvasTextItem Item(FVector2D(X * Scale, Y * Scale), FText::FromString(Value), GEngine->GetMediumFont(), Color);
        Item.Scale = FVector2D(Scale * Size); Item.EnableShadow(FLinearColor::Black); Canvas->DrawItem(Item);
#if WITH_DEV_AUTOMATION_TESTS
        if (ObserveDrawForTest) { ObserveDrawForTest(FBox2D(Item.Position, Item.Position + Item.DrawnSize + FVector2D(1)), Value); }
#endif
    };
    Panel(16, 16, 440, 176);
    Text(TEXT("Y01 配表测试（非正式规则）"), 28, 25, FLinearColor(0.1f, 0.9f, 1.f), 1.2f);
    Text(TEXT("测试索敌 / 直线追击；非正式AI"), 28, 55, FLinearColor(0.7f, 0.8f, 0.9f));
    Text(O.ProfilePath.IsEmpty() ? TEXT("等待服务器配置...") : O.ProfilePath, 28, 83, FLinearColor::White, 0.7f);
    if (GS)
    {
        const auto& S = GS->GetWaveSnapshot();
        FString Phase;
        switch (S.Phase)
        {
        case EY01MatchPhase::Waiting: Phase = TEXT("等待 / Waiting：Enter 开始"); break;
        case EY01MatchPhase::Preparing:
        {
            const auto Remaining = PC->GetSnapshotRemaining(O.PreparationDeadline);
            Phase = Remaining.IsSet() ? FString::Printf(TEXT("准备：%.1f 秒"), Remaining.GetValue()) : TEXT("准备：等待服务器快照"); break;
        }
        case EY01MatchPhase::Battle: Phase = S.bWaveCompleted ? TEXT("已结算：Enter 下一波") : TEXT("战斗 / Battle"); break;
        case EY01MatchPhase::Ended: Phase = S.Result == EY01MatchResult::Success ? TEXT("测试完成 / SUCCESS") : TEXT("测试失败 / FAILED"); break;
        }
        Text(Phase, 28, 108, S.Result == EY01MatchResult::Failure ? FLinearColor::Red : FLinearColor(0.6f, 1.f, 0.4f));
        Text(FString::Printf(TEXT("波次 %d  %s     存活 %d / %d"), S.WaveIndex + 1, *S.Wave.RowName.ToString(), O.Alive, O.Total), 28, 136);
        Text(FString::Printf(TEXT("预期本波奖励：Gold +%d   XP +%d"), O.RewardGold, O.RewardXP), 28, 164, FLinearColor(1.f, 0.8f, 0.3f));
    }
    if (!O.Error.IsEmpty())
    {
        Panel(16, 205, 940, 130);
        Text(TEXT("配置错误：保持 Waiting；修正并保存后停止重开"), 28, 216, FLinearColor::Red);
        FString Remaining = O.Error;
        for (int32 I = 0; I < 4 && !Remaining.IsEmpty(); ++I)
        { Text(Remaining.Left(110), 28, 245 + I * 20, FLinearColor(1.f, 0.5f, 0.4f), 0.8f); Remaining = Remaining.Mid(110); }
    }
    if (O.Error.IsEmpty())
    {
        Panel(16, 205, 440, 137);
        const FLinearColor Colors[] = {FLinearColor(0.2f,1.f,0.4f), FLinearColor(0.3f,0.6f,1.f),
            FLinearColor(0.75f,0.35f,1.f), FLinearColor(1.f,0.8f,0.15f), FLinearColor(1.f,0.2f,0.25f)};
        for (int32 I = 0; I < O.Equipment.Num(); ++I)
        {
            const auto& Item = O.Equipment[I]; const float X = 28 + (I % 3) * 140, Y = 211 + (I / 3) * 30;
            const int32 Quality = static_cast<int32>(Item.Quality);
            const FLinearColor Color = Quality >= 0 && Quality < UE_ARRAY_COUNT(Colors) ? Colors[Quality] : FLinearColor::Gray;
            Text(FString::Printf(TEXT("[%d] %s"), I + 1, Item.Row.IsNone() ? TEXT("空") : *Item.Row.ToString().Left(14)), X, Y, Item.Row.IsNone() ? FLinearColor::Gray : Color, 0.60f);
            if (!Item.Row.IsNone())
            {
                Text(FString::Printf(TEXT("%s +%g%s"), *StaticEnum<EY01Quality>()->GetNameStringByValue(Quality), Item.AttackBonus,
                    Item.bDebugLocked ? TEXT(" 锁") : TEXT("")), X, Y + 13, Color, 0.55f);
            }
        }
        for (int32 I = 0; I < 2; ++I)
        {
            const FVector2D Position(28 + I * 210, 272), Size(202, 28);
            DrawRect(FLinearColor(0.04f,0.24f,0.3f), Position.X * Scale, Position.Y * Scale, Size.X * Scale, Size.Y * Scale);
            AddHitBox(Position * Scale, Size * Scale, I == 0 ? TEXT("EquipmentEquip") : TEXT("EquipmentUnequip"), true, 10);
            Text(I == 0 ? TEXT("第1槽：装上 EQ_0001") : TEXT("第1槽：卸下"), Position.X + 8, 278, FLinearColor::White, 0.70f);
        }
        Text(FString::Printf(TEXT("Attack base %g / current %g；品质无倍率"), O.AttackBase, O.AttackCurrent), 28, 305, FLinearColor(0.8f,0.9f,1.f), 0.65f);
        Text(O.EquipmentResult.IsEmpty() ? TEXT("仅调试第1槽；旧Effect装备锁定，不提供库存") : O.EquipmentResult.Left(43),
            28, 324, O.EquipmentResult.IsEmpty() || O.bEquipmentSucceeded ? FLinearColor(0.4f,1.f,0.7f) : FLinearColor(1.f,0.35f,0.3f), 0.60f);
    }
    if (O.bTrainingAvailable)
    {
        Panel(16, 350, 440, 175);
        const auto& T = O.Training;
        Text(T.bInside ? FString::Printf(TEXT("个人练功房 #%d  阶段 %d"), T.Room, T.Stage) : TEXT("当前区域：主线 / Main"), 28, 360, FLinearColor(0.2f, 1.f, 0.8f));
        const auto Next = PC->GetSnapshotRemaining(T.bInside ? T.NextBatchAt : 0);
        const FString NextText = Next.IsSet() ? FString::Printf(TEXT("%.1fs"), Next.GetValue()) : TEXT("等待服务器快照");
        Text(FString::Printf(TEXT("本房活怪 %d / %d  下批 %s  奖励成功击杀 %d"), T.Alive, T.MaxAlive, *NextText, T.Kills), 28, 390, FLinearColor::White, 0.85f);
        const FName Box = T.bInside ? TEXT("TrainingReturn") : TEXT("TrainingEnter");
        // 绘制与命中共用完全相同的缩放坐标；鼠标只经AHUD入口，不另绑左键。
        const FVector2D Position(28 * Scale, 425 * Scale), Size(260 * Scale, 42 * Scale);
        DrawRect(FLinearColor(0.04f, 0.35f, 0.4f), Position.X, Position.Y, Size.X, Size.Y);
        AddHitBox(Position, Size, Box, true, 10);
        Text(T.bInside ? TEXT("返回 / Return") : TEXT("进入练功房 / Enter training"), 40, 436);
        Text(O.bAutoAttackEnabled ? TEXT("独立普攻无需按键；切区不重置攻击间隔") : TEXT("1 / 2 手动攻击；切区不回血、不刷新CD"), 28, 485, FLinearColor(1.f, 0.8f, 0.3f), 0.85f);
        if (!T.Error.IsEmpty()) { Text(T.Error, 28, 510, FLinearColor::Red, 0.65f); }
    }
    Text(PC->GetSnapshotRemaining(0).IsSet()
        ? FString::Printf(TEXT("服务器快照 %.2fs；无新样本保持（非实时）"), O.ServerSampleTime)
        : TEXT("等待服务器快照（尚未就绪）"), 28, 535, FLinearColor(0.7f, 0.8f, 0.9f), 0.7f);
    Panel(16, 608, 425, 176);
    if (State)
    {
        const float HP = O.Health, Max = O.MaxHealth;
        Text(FString::Printf(TEXT("生命 HP %.0f / %.0f"), HP, Max), 28, 619);
        DrawRect(FLinearColor(0.16f, 0.2f, 0.24f), 28 * Scale, 648 * Scale, 395 * Scale, 14 * Scale);
        DrawRect(FLinearColor(0.05f, 0.85f, 0.65f), 28 * Scale, 648 * Scale, 395 * Scale * (Max > 0 ? FMath::Clamp(HP / Max, 0.f, 1.f) : 0), 14 * Scale);
        Text(FString::Printf(TEXT("AttackDamage %.0f   MoveSpeed %.0f"), O.AttackCurrent, O.MoveSpeed), 28, 675);
        Text(FString::Printf(TEXT("Gold %d     XP %d"), O.Gold, O.XP), 28, 705, FLinearColor(1.f, 0.8f, 0.2f), 1.2f);
        Text(TEXT("青色=玩家；橙色=普通怪；红色=Boss"), 28, 751, FLinearColor(0.65f, 0.85f, 0.9f), 0.9f);
    }
    Panel(458, 646, 806, 138);
    for (int32 I = 0; I < 4; ++I)
    {
        const float X = 472 + I * 195;
        FString Name = TEXT("等待同步"), Status = TEXT("--");
        if (O.bReady && O.Slots.IsValidIndex(I))
        {
            const auto& Slot = O.Slots[I]; Name = Slot.Row.IsNone() ? TEXT("空槽") : Slot.Row.ToString();
            if (Slot.Row.IsNone()) { Status = TEXT("Empty"); }
            else if (!Slot.bActive) { Status = Slot.bEffectApplied ? TEXT("被动已生效") : TEXT("被动未生效"); }
            else
            {
                const auto CD = PC->GetSnapshotRemaining(Slot.Deadline);
                Status = !CD.IsSet() ? TEXT("等待服务器快照")
                    : CD.GetValue() > 0 ? FString::Printf(TEXT("CD %.2f s"), CD.GetValue()) : TEXT("Ready");
            }
        }
        Text(FString::Printf(TEXT("[%d] %s"), I + 1, *Name), X, 660, FLinearColor::White, 0.95f);
        Text(Status, X, 690, FLinearColor(0.3f, 0.9f, 0.85f));
        if (O.Slots.IsValidIndex(I) && !O.Slots[I].Description.IsEmpty())
        {
            FString Summary; O.Slots[I].Description.Split(TEXT("："), &Summary, nullptr);
            Text(Summary, X, 721, FLinearColor(0.8f, 0.9f, 1.f), 0.68f);
        }
    }
    Text(O.bAutoAttackEnabled ? TEXT("独立普攻：自动选敌、不追击；1 / 2 手动技能独立，可并行") : TEXT("1 / 2 手动释放可并行、移动不取消；自动普攻未接；3 被动，4 空"), 472, 748, FLinearColor(0.75f, 0.8f, 0.9f), 0.85f);
    Panel(920, 124, 344, 34);
    AddHitBox(FVector2D(920, 124) * Scale, FVector2D(344, 34) * Scale, TEXT("Attributes"), true, 10);
    Text(O.bAutoAttackEnabled ? (bAttributesExpanded ? TEXT("统一属性（R34）[-] 点击收起") : TEXT("统一属性（R34）[+] 点击展开"))
        : (bAttributesExpanded ? TEXT("属性（R33测试）[-] 点击收起") : TEXT("属性（R33测试）[+] 点击展开")), 932, 132, FLinearColor(0.8f, 0.9f, 1.f), 0.85f);
    if (bAttributesExpanded)
    {
        Panel(920, 208, 344, 350);
        float Y = 216;
        auto Line = [&](const FString& S) { Text(S, 932, Y, FLinearColor(0.8f, 0.9f, 1.f), 0.64f); Y += O.bAutoAttackEnabled ? 16 : 17; };
        const auto& G = O.Growth;
        if (O.bAutoAttackEnabled)
        {
            const auto& S = O.UnitStats;
            auto Type = [](EY01AttackType T) { return T == EY01AttackType::Melee ? TEXT("近战") : TEXT("远程"); };
            Line(TEXT("[基础] R34服务器快照；非实时/非正式平衡"));
            Line(FString::Printf(TEXT("Health上限 %g / HP %g  AttackDamage %g"), S.Health, O.Health, S.AttackDamage));
            Line(FString::Printf(TEXT("AttackInterval %gs  AttackType %s"), S.AttackInterval, Type(S.AttackType)));
            Line(FString::Printf(TEXT("Range %gcm  MeleeAngle %g°"), S.Range, S.MeleeAngle));
            Line(FString::Printf(TEXT("ProjectileSpeed %gcm/s"), S.ProjectileSpeed));
            Line(FString::Printf(TEXT("Armor %g  HealthRegen %g HP/s"), S.Armor, S.HealthRegen));
            Line(FString::Printf(TEXT("MoveSpeed %gcm/s"), S.MoveSpeed));
            Line(FString::Printf(TEXT("Strength %g  Agility %g  Wisdom %g"), S.Strength, S.Agility, S.Wisdom));
            Line(TEXT("三维仅显示，不产生下游属性"));
            const auto Ready = PC->GetSnapshotRemaining(O.AutoAttackDeadline);
            Line(Ready.IsSet() ? FString::Printf(TEXT("独立普攻：剩余 %.2fs；无目标不消耗"), Ready.GetValue()) : TEXT("独立普攻：等待服务器样本"));
            const auto& T = O.TargetStats;
            Line(IsValid(O.Target) ? TEXT("[目标] 同一服务器样本") : TEXT("[目标] 无合法范围内目标"));
            Line(FString::Printf(TEXT("Health %g / HP %g  AttackDamage %g"), T.Health, O.TargetHealth, T.AttackDamage));
            Line(FString::Printf(TEXT("AttackInterval %gs  AttackType %s"), T.AttackInterval, Type(T.AttackType)));
            Line(FString::Printf(TEXT("Range %gcm  MeleeAngle %g°"), T.Range, T.MeleeAngle));
            Line(FString::Printf(TEXT("ProjectileSpeed %gcm/s"), T.ProjectileSpeed));
            Line(FString::Printf(TEXT("Armor %g  HealthRegen %g HP/s"), T.Armor, T.HealthRegen));
            Line(FString::Printf(TEXT("MoveSpeed %gcm/s"), T.MoveSpeed));
            Line(FString::Printf(TEXT("[经济] Gold %d  XP %d；时间金币 %lld"), O.Gold, O.XP, G.TimeGoldGranted));
            Line(FString::Printf(TEXT("[成长] GrowthKills %lld；攻击 +%g / +%g"), G.GrowthKills, G.TimeAttackGranted, G.KillAttackGranted));
            Line(G.Error.IsEmpty() ? TEXT("命中减甲无保底；回血不暂停、不复活") : G.Error.Left(30));
        }
        else
        {
        Line(TEXT("[基础] 服务器快照；非实时/非正式平衡"));
        Line(FString::Printf(TEXT("HP %g / MaxHP %g"), O.Health, O.MaxHealth));
        Line(FString::Printf(TEXT("Attack base %g / current %g"), O.AttackBase, O.AttackCurrent));
        Line(TEXT("[经济]"));
        Line(FString::Printf(TEXT("Gold %d  XP %d  测试金币/秒 %g"), O.Gold, O.XP, G.Config.GoldPerEffectiveSecond));
        Line(FString::Printf(TEXT("时间金币实际累计 %lld；余数 %.4f"), G.TimeGoldGranted, G.GoldRemainder));
        Line(TEXT("[成长] R33测试授予，非正式科技装备"));
        Line(FString::Printf(TEXT("开关 %s；%s"), G.Config.bEnabled ? TEXT("开") : TEXT("关"), *G.StopReason));
        Line(FString::Printf(TEXT("攻击/有效秒 %g  攻击/有效杀 %g"), G.Config.AttackPerEffectiveSecond, G.Config.AttackPerEffectiveKill));
        Line(FString::Printf(TEXT("有效秒 %lld + %.4f；GrowthKills %lld"), G.EffectiveSeconds, G.SecondRemainder, G.GrowthKills));
        Line(FString::Printf(TEXT("实际时间攻击 +%g；击杀攻击 +%g"), G.TimeAttackGranted, G.KillAttackGranted));
        Line(G.Error.IsEmpty() ? TEXT("错误：无") : TEXT("错误：") + G.Error.Left(24));
        if (!G.Error.IsEmpty()) { Line(G.Error.Mid(24)); }
        Line(TEXT("[特殊战斗] 已授予主动技能系数"));
        FString Coefficients;
        for (int32 I = 0; I < O.Slots.Num(); ++I)
        { if (O.Slots[I].bActive) { Coefficients += FString::Printf(TEXT("槽%d Attack×%g  "), I + 1, O.Slots[I].DamageCoefficient); } }
        Line(Coefficients);
        Line(TEXT("提交时取current快照；在途不追改"));
        Line(TEXT("三维/攻速/护甲/金币增幅/多重：未接"));
        Line(TEXT("CD不是攻速；技能详细参数见技能说明"));
        Line(TEXT("训练奖励成功击杀与GrowthKills独立"));
        }
    }
    Panel(920, 166, 344, 34);
    AddHitBox(FVector2D(920, 166) * Scale, FVector2D(344, 34) * Scale, TEXT("SkillDetails"), true, 10);
    Text(bSkillDetailsExpanded ? TEXT("技能说明 [-] 点击收起") : TEXT("技能说明 [+] 点击展开"), 932, 174, FLinearColor(0.8f, 0.9f, 1.f), 0.85f);
    if (bSkillDetailsExpanded)
    {
        Panel(920, 208, 344, 350);
        float SkillY = 218;
        auto WrappedText = [&](const FString& Value, FLinearColor Color)
        {
            FString Line;
            auto FlushLine = [&]
            {
                if (!Line.IsEmpty()) { Text(Line, 932, SkillY, Color, 0.68f); SkillY += 20; Line.Reset(); }
            };
            for (TCHAR Character : Value)
            {
                if (Character == '\n') { FlushLine(); continue; }
                float Width = 0, Height = 0;
                Canvas->StrLen(GEngine->GetMediumFont(), Line + Character, Width, Height);
                if (Width * 0.68f > 320 && !Line.IsEmpty()) { FlushLine(); }
                Line += Character;
            }
            FlushLine();
        };
        for (int32 I = 0; I < O.Slots.Num(); ++I)
        {
            if (!O.Slots[I].Description.IsEmpty())
            { WrappedText(FString::Printf(TEXT("[%d] "), I + 1) + O.Slots[I].Description, FLinearColor(0.8f, 0.9f, 1.f)); }
        }
        WrappedText(TEXT("离区 / 死亡 / Ended / 登出立即撤去攻击与反馈；不退CD；无正式复活"), FLinearColor(1.f, 0.8f, 0.35f));
    }
    if (IsValid(O.Target))
    {
        const auto* ASC = O.Target->GetCombatASC(); Panel(920, 16, 344, 100);
        Text(TEXT("选中怪物：") + O.TargetRow.RowName.ToString(), 936, 28, FLinearColor(1.f, 0.45f, 0.2f));
        Text(FString::Printf(TEXT("HP %.0f / %.0f   Atk %.0f"), O.bAutoAttackEnabled ? O.TargetHealth : Y01Playground::Health(ASC),
            O.bAutoAttackEnabled ? O.TargetStats.Health : ASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()),
            O.bAutoAttackEnabled ? O.TargetStats.AttackDamage : ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute())), 936, 59);
        Text(FString::Printf(TEXT("Speed %.0f   Range %.0f"), O.bAutoAttackEnabled ? O.TargetStats.MoveSpeed : O.TargetSpeed, O.bAutoAttackEnabled ? O.TargetStats.Range : O.TargetRange), 936, 89);
    }
    Panel(458, 570, 806, 64);
    Text(TEXT("Enter 开始/下一波   WASD 移动   1-4 技能   F5 重开指引"), 472, 582, FLinearColor::White, 0.95f);
    Text(TEXT("修改 DataTable 后保存；停止测试再启动，读取最新磁盘数据"), 472, 606, FLinearColor(1.f, 0.8f, 0.35f), 0.9f);
    if (!O.Notice.IsEmpty())
    {
        Panel(220, 360, 850, 100);
        Text(TEXT("重开：先保存表；启动器 Ctrl+C / PIE Stop，再重新启动"), 238, 378, FLinearColor::Yellow);
        Text(TEXT("不回写资产，不退出用户编辑器；联网会话请一起停止重开"), 238, 414);
    }
}
