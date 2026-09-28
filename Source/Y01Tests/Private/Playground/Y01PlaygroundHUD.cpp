#include "Playground/Y01PlaygroundHUD.h"
#include "Playground/Y01PlaygroundController.h"
#include "Player/Y01PlayerState.h"
#include "Combat/Y01Monster.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "CanvasItem.h"
#include "GameFramework/CharacterMovementComponent.h"

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
    { DrawRect(FLinearColor(0.015f, 0.025f, 0.04f, 0.88f), X * Scale, Y * Scale, W * Scale, H * Scale); };
    auto Text = [&](const FString& Value, float X, float Y, FLinearColor Color = FLinearColor::White, float Size = 1.f)
    {
        FCanvasTextItem Item(FVector2D(X * Scale, Y * Scale), FText::FromString(Value), GEngine->GetMediumFont(), Color);
        Item.Scale = FVector2D(Scale * Size); Item.EnableShadow(FLinearColor::Black); Canvas->DrawItem(Item);
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
            Phase = FString::Printf(TEXT("准备：%.1f 秒"), FMath::Max(0., S.PreparationDeadline - GS->GetServerWorldTimeSeconds())); break;
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
    Panel(16, 608, 425, 176);
    if (State)
    {
        const auto* ASC = State->GetCombatASC();
        const float HP = Y01Playground::Health(ASC), Max = ASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute());
        Text(FString::Printf(TEXT("生命 HP %.0f / %.0f"), HP, Max), 28, 619);
        DrawRect(FLinearColor(0.16f, 0.2f, 0.24f), 28 * Scale, 648 * Scale, 395 * Scale, 14 * Scale);
        DrawRect(FLinearColor(0.05f, 0.85f, 0.65f), 28 * Scale, 648 * Scale, 395 * Scale * (Max > 0 ? FMath::Clamp(HP / Max, 0.f, 1.f) : 0), 14 * Scale);
        Text(FString::Printf(TEXT("AttackDamage %.0f   MoveSpeed %.0f"), ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute()), ASC->GetInitialMoveSpeed()), 28, 675);
        Text(FString::Printf(TEXT("Gold %d     XP %d"), State->GetGold(), State->GetExperience()), 28, 705, FLinearColor(1.f, 0.8f, 0.2f), 1.2f);
        Text(TEXT("青色=玩家；橙色=普通怪；红色=Boss"), 28, 751, FLinearColor(0.65f, 0.85f, 0.9f), 0.9f);
    }
    Panel(458, 646, 806, 138);
    for (int32 I = 0; I < 4; ++I)
    {
        const float X = 472 + I * 195;
        FString Name = TEXT("等待同步"), Status = TEXT("--");
        if (O.bReady && O.Slots.IsValidIndex(I) && GS)
        {
            const auto& Slot = O.Slots[I]; Name = Slot.Row.IsNone() ? TEXT("空槽") : Slot.Row.ToString();
            if (Slot.Row.IsNone()) { Status = TEXT("Empty"); }
            else if (!Slot.bActive) { Status = Slot.bEffectApplied ? TEXT("被动已生效") : TEXT("被动未生效"); }
            else
            {
                const double CD = FMath::Max(0., Slot.Deadline - GS->GetServerWorldTimeSeconds());
                Status = CD > 0 ? FString::Printf(TEXT("CD %.2f s"), CD) : TEXT("Ready");
            }
        }
        Text(FString::Printf(TEXT("[%d] %s"), I + 1, *Name), X, 660, FLinearColor::White, 0.95f);
        Text(Status, X, 690, FLinearColor(0.3f, 0.9f, 0.85f));
    }
    Text(TEXT("1 / 2 均为同一基础攻击GA，仅表CD不同；3 被动，4 空"), 472, 748, FLinearColor(0.75f, 0.8f, 0.9f), 0.9f);
    if (IsValid(O.Target))
    {
        const auto* ASC = O.Target->GetCombatASC(); Panel(920, 16, 344, 140);
        Text(TEXT("选中怪物：") + O.TargetRow.RowName.ToString(), 936, 28, FLinearColor(1.f, 0.45f, 0.2f));
        Text(FString::Printf(TEXT("HP %.0f / %.0f   Atk %.0f"), Y01Playground::Health(ASC), ASC->GetNumericAttribute(UY01AttributeSet::GetMaxHealthAttribute()), ASC->GetNumericAttribute(UY01AttributeSet::GetAttackDamageAttribute())), 936, 59);
        Text(FString::Printf(TEXT("Speed %.0f   Range %.0f"), O.TargetSpeed, O.TargetRange), 936, 89);
        Text(TEXT("权威范围内自动选择最近活怪"), 936, 121, FLinearColor(0.8f, 0.85f, 0.9f), 0.85f);
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
