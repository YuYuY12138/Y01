#pragma once
#include "Match/Y01GameFlow.h"
#include "Playground/Y01R36Runtime.h"
#include "Y01R36Verification.generated.h"

struct FY01CommittedView;
namespace Y01R36Trace
{
    FString ViewRecord(const FY01CommittedView& View);
    FString PanelRecord(const UY01R36Panel* Panel);
    TArray<FString> Capture(const AY01R36Controller* Controller, bool bIncludeView);
    // 仅controlled压力验证，复用现有身份意图和RPC，不作为鼠标或购买证据。
    TArray<FY01ItemRequest> BuildBurst(const FY01CommittedView& View, EY01MatchPhase Phase);
    FString TryBurst(const FY01CommittedView& View, EY01MatchPhase Phase, bool bEnabled, bool& bSent,
        TFunctionRef<void(const FY01ItemRequest&)> Send);
    FString RequestRecord(const AY01R36Controller* Controller);
    // 显式Core33Net令牌模式：真实DS双端短场，只改变本次运行内存。
    void TickCoreVerification(AY01R36Controller* Controller);
}

// 仅截获反射RPC包装入口的本地接线探针，不把截获当作跨进程发送成功。
UCLASS()
class AY01R36BurstControllerProbe : public AY01R36Controller
{
    GENERATED_BODY()
public:
    TArray<FY01ItemRequest> CapturedRequests;
    TArray<int32> CapturedActions;
    virtual void ProcessEvent(UFunction* Function, void* Parameters) override;
};

// 开发模块生命周期探针，不是网络验收替身，也不改生产Mode默认行为。
UCLASS()
class AY01R36FlowProbe : public AY01GameMode
{
    GENERATED_BODY()
public:
    int32 InitializedCalls = 0;
    bool bSawInitializedCharacter = false;
    TArray<EY01MatchPhase> Phases;
    TArray<EY01MatchPhase> PreviousPhases;
protected:
    virtual void OnPlayerInitialized(AY01PlayerState* Player) override;
    virtual void OnMatchPhaseChanged(EY01MatchPhase Previous, EY01MatchPhase Current) override;
};

UCLASS()
class AY01R36RuntimeProbe : public AY01R36GameMode
{
    GENERATED_BODY()
public:
    using AY01PlaygroundGameMode::LoadMatchConfiguration;
    using AY01R36GameMode::SelectProfile;
    using AY01R36GameMode::UseAutomaticWaves;
    using AY01R36GameMode::OnPlayerInitialized;
    int32 StopRequests = 0;
protected:
    virtual void RequestControlledExit() override { ++StopRequests; }
};
