#include "Player/Y01ItemTransactions.h"

bool FY01ItemTransactions::Enter()
{
    if (bBusy) { bInterrupted = true; return false; }
    if (BeforeEnter && !BeforeEnter()) { return false; }
    bBusy = true; bInterrupted = false; return true;
}
void FY01ItemTransactions::Leave() { bBusy = false; if (AfterLeave) { AfterLeave(); } }
bool FY01ItemTransactions::SamePayload(const FY01ItemRequest& A, const FY01ItemRequest& B)
{
    return A.MatchId == B.MatchId && A.JoinSequence == B.JoinSequence && A.Seq == B.Seq
        && A.Operation == B.Operation && A.ItemId == B.ItemId && A.ExpectedEntityRevision == B.ExpectedEntityRevision
        && A.Slot == B.Slot && A.ExpectedSlotItemId == B.ExpectedSlotItemId
        && A.ExpectedSlotEntityRevision == B.ExpectedSlotEntityRevision
        && A.ExpectedLearnedId == B.ExpectedLearnedId && A.ExpectedLearnedRevision == B.ExpectedLearnedRevision
        && A.ConfigRevision == B.ConfigRevision && A.TicketId == B.TicketId
        && A.Offer.Type == B.Offer.Type && A.Offer.RowName == B.Offer.RowName && A.Materials == B.Materials
        && A.Recipe.Type == B.Recipe.Type && A.Recipe.RowName == B.Recipe.RowName
        && A.Talent.Type == B.Talent.Type && A.Talent.RowName == B.Talent.RowName && A.TalentChoiceId == B.TalentChoiceId;
}
bool FY01ItemTransactions::Admit(const FY01ItemRequest& R, uint64 Revision, FY01ItemResult& Out) const
{
    using E = EY01ItemError;
    if (R.Seq == 0 || R.Seq == MAX_uint64)
    { Out = FY01ItemResult::Fail(E::InvalidSequence, Revision, TEXT("请求序号必须从1起且留有下一个序号")); return false; }
    if (R.Seq > ExpectedSeq)
    { Out = FY01ItemResult::Fail(E::SequenceGap, Revision, TEXT("跳号拒绝且不推进序列")); return false; }
    if (R.Seq < ExpectedSeq)
    {
        const auto& Old = Recent[(R.Seq - 1) % UE_ARRAY_COUNT(Recent)];
        if (Old.Payload.Seq != R.Seq)
        { Out = FY01ItemResult::Fail(E::ExpiredSequence, Revision, TEXT("请求已超过最近64条，不得再次执行")); }
        else if (!SamePayload(Old.Payload, R))
        { Out = FY01ItemResult::Fail(E::PayloadConflict, Revision, TEXT("相同序号的规范化载荷不同")); }
        else { Out = Old.Result; }
        return false;
    }
    return true;
}
void FY01ItemTransactions::Record(const FY01ItemRequest& Request, FY01ItemResult&& Result)
{
    check(bBusy && Request.Seq == ExpectedSeq && ExpectedSeq < MAX_uint64);
    auto& Slot = Recent[(Request.Seq - 1) % UE_ARRAY_COUNT(Recent)];
    Slot.Payload = Request; Slot.Result = MoveTemp(Result); ++ExpectedSeq;
}
void FY01ItemTransactions::Record(FY01ItemRequest&& Request, FY01ItemResult&& Result)
{
    check(bBusy && Request.Seq == ExpectedSeq && ExpectedSeq < MAX_uint64);
    auto& Slot = Recent[(Request.Seq - 1) % UE_ARRAY_COUNT(Recent)];
    Slot.Payload = MoveTemp(Request); Slot.Result = MoveTemp(Result); ++ExpectedSeq;
}
