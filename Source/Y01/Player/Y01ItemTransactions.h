#pragma once

#include "Player/Y01ItemRuntimeTypes.h"

// 唯一串行锁和请求序列账本，仅由所属注册表使用；没有第二条请求执行路径。
class Y01_API FY01ItemTransactions
{
    friend class UY01LoadoutComponent;
    friend class UY01SourceEffectsComponent;
    friend class UY01EconomyComponent;
    friend class UY01GrowthComponent;
    friend class UY01DropDeliveryComponent;
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FY01R36EconomyTestAccess;
    friend struct FY01R36DeliveryTestAccess;
#endif
    struct FReceipt
    {
        FY01ItemRequest Payload;
        FY01ItemResult Result;
    };
    TFunction<bool()> BeforeEnter;
    TFunction<void()> AfterLeave;
    bool Enter();
    void Leave();
    bool Admit(const FY01ItemRequest& Request, uint64 Revision, FY01ItemResult& OutResult) const;
    void Record(const FY01ItemRequest& Request, FY01ItemResult&& Result);
    void Record(FY01ItemRequest&& Request, FY01ItemResult&& Result);
    static bool SamePayload(const FY01ItemRequest& A, const FY01ItemRequest& B);
    // 固定槽位避免提交点容器扩容，按Seq寻址但逐字段比对，不依赖hash或结构padding。
    FReceipt Recent[64];
    uint64 ExpectedSeq = 1;
    bool bBusy = false;
    bool bInterrupted = false;
};
