#pragma once

#include "Data/Y01DataTypes.h"

class UY01SourceEffectsComponent;

enum class EY01RuntimeSourceKind : uint8 { Base, Equipment, Skill, Talent, Shop, Devoured };
enum class EY01RuntimeError : uint8
{
    None, Authority, Session, Disabled, Stopped, Busy, Stale, InvalidReference, DuplicateEffect,
    InvalidNumber, InvalidIdentity, PayloadConflict, Precision, IntegerRange, BalanceRange,
    AuditRange, Time, EffectRejected, HandleLost, Interrupted
};
struct Y01_API FY01RuntimeResult
{
    EY01RuntimeError Code = EY01RuntimeError::None;
    FString Message;
    bool IsOk() const { return Code == EY01RuntimeError::None; }
    static FY01RuntimeResult Fail(EY01RuntimeError Code, const TCHAR* Message) { return {Code, Message}; }
};
struct Y01_API FY01RuntimeSourceId
{
    EY01RuntimeSourceKind Kind = EY01RuntimeSourceKind::Base;
    FString LocalKey;
    FString Id;
};
struct Y01_API FY01SourceEffectSnapshot
{
    FY01RuntimeSourceId Source;
    FName EffectId;
    EY01SourceEffectKind Kind = EY01SourceEffectKind::AttackFlat;
    double Magnitude = 0;
    EY01Attribute SourceAttribute = EY01Attribute::None;
    EY01Attribute TargetAttribute = EY01Attribute::None;
    double Amount = 0;
    EY01EffectCondition Condition = EY01EffectCondition::None;
    double ConditionThreshold = 0;
    bool bGrowthContribution = false;
    double DurationSeconds = 0;
    double LayerCap = 0;
    double IntervalSeconds = 0;
};
// 玩家成长四项合计；可从单一力量值隐式构造，保持既有单力量调用点（含测试）不变。
struct Y01_API FY01PlayerGrowthTotals
{
    double Strength = 0, Agility = 0, Wisdom = 0, MaxHealth = 0;
    FY01PlayerGrowthTotals() = default;
    FY01PlayerGrowthTotals(double StrengthOnly) : Strength(StrengthOnly) {}
    FY01PlayerGrowthTotals(double InStrength, double InAgility, double InWisdom, double InMaxHealth)
        : Strength(InStrength), Agility(InAgility), Wisdom(InWisdom), MaxHealth(InMaxHealth) {}
    bool operator==(const FY01PlayerGrowthTotals&) const = default;
};
enum class EY01IncomePhase : uint8 { Disabled, Waiting, Running, Stopped, Fault };
enum class EY01IncomeEntryKind : uint8 { Natural, Spending };
struct Y01_API FY01IncomeEntry
{
    int64 Sequence = 0;
    EY01IncomeEntryKind Kind = EY01IncomeEntryKind::Natural;
    double SegmentStart = 0;
    double Time = 0;
    double Rate = 0;
    int64 Delta = 0;
    int32 Balance = 0;
    double Remainder = 0;
};
// 账本仅保留已提交计算状态，不另存钱包；余额始终读取PlayerState。
struct Y01_API FY01IncomeState
{
    EY01IncomePhase Phase = EY01IncomePhase::Disabled;
    double StartTime = 0;
    double StartRemainder = 0;
    double Rate = 0;
    int64 PostedWhole = 0;
    double LastTime = 0;
    double Remainder = 0;
    int64 TotalIncome = 0;
    int64 TotalSpending = 0;
    int64 Sequence = 0;
    TArray<FY01IncomeEntry> Recent;
    FY01RuntimeResult Fault;
};
struct Y01_API FY01IncomeSnapshot
{
    FY01IncomeState State;
    int32 Balance = 0;
};
struct Y01_API FY01SourceSnapshot
{
    TArray<FY01SourceEffectSnapshot> Effects;
    float BaseAttack = 0;
    float CurrentAttack = 0;
    uint64 Revision = 0;
};
// 服务器生成的不可修改计划；不持有目录或表裸指针，跨owner/版本使用会拒绝。
class Y01_API FY01PreparedSource
{
    friend class UY01SourceEffectsComponent;
    TWeakObjectPtr<UY01SourceEffectsComponent> Owner;
    uint64 Revision = 0;
    FY01RuntimeSourceId Source;
    TArray<FY01SourceEffectSnapshot> Effects;
    bool bRepeat = false;
};
// 仅服务器C++调用方提供已解析值；准备时重新验证并复制，不保留目录或行指针。
struct Y01_API FY01ResolvedSourceEffect
{
    FName EffectId;
    EY01SourceEffectKind Kind = EY01SourceEffectKind::AttackFlat;
    double Magnitude = 0;
    EY01Attribute SourceAttribute = EY01Attribute::None;
    EY01Attribute TargetAttribute = EY01Attribute::None;
    double Amount = 0;
    EY01EffectCondition Condition = EY01EffectCondition::None;
    double ConditionThreshold = 0;
    bool bGrowthContribution = false;
    double DurationSeconds = 0;
    double LayerCap = 0;
    double IntervalSeconds = 0;
};
struct Y01_API FY01ResolvedSource
{
    EY01RuntimeSourceKind Kind = EY01RuntimeSourceKind::Base;
    FString LocalKey;
    TArray<FY01ResolvedSourceEffect> Effects;
};
class Y01_API FY01PreparedMutation
{
    friend class UY01SourceEffectsComponent;
    friend class UY01GrowthComponent;
    TOptional<FY01PlayerGrowthTotals> PlayerGrowth;
    TWeakObjectPtr<UY01SourceEffectsComponent> Owner;
    FGuid Match;
    int32 JoinSequence = 0;
    uint64 Revision = 0;
    TArray<FY01PreparedSource> Additions;
    TArray<FY01RuntimeSourceId> Removals;
};
class Y01_API FY01SourceTransaction
{
    friend class UY01SourceEffectsComponent;
    friend class FY01ScopedSourceMutation;
    TWeakObjectPtr<UY01SourceEffectsComponent> Owner;
    FGuid Token;
};
// 转移显式事务的收尾责任；不可复制或移动，未成功提交时析构尝试回滚。
class Y01_API FY01ScopedSourceMutation
{
public:
    explicit FY01ScopedSourceMutation(FY01SourceTransaction&& Transaction);
    ~FY01ScopedSourceMutation();
    FY01ScopedSourceMutation(const FY01ScopedSourceMutation&) = delete;
    FY01ScopedSourceMutation& operator=(const FY01ScopedSourceMutation&) = delete;
    FY01ScopedSourceMutation(FY01ScopedSourceMutation&&) = delete;
    FY01ScopedSourceMutation& operator=(FY01ScopedSourceMutation&&) = delete;
    FY01RuntimeResult Commit();
private:
    friend class UY01LoadoutComponent;
    friend class UY01GrowthComponent;
    FY01RuntimeResult CommitAt(TFunctionRef<void()> CommitOwner);
    FY01SourceTransaction Transaction;
};
