#pragma once
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace gargantuan::test::reliable_feedback_model {
using U = std::uint64_t;
constexpr U KiB = 1024, MiB = 1024 * KiB, G = 512 * KiB;
constexpr U DrainFloor = 16 * MiB, FreshnessUs = 50'000, RequalificationUs = 1'000'000;

enum class SegmentState : std::uint8_t { Pending, InFlight, Retry, Acked };
enum class FeedbackState : std::uint8_t { Missing, Fresh, Contradictory, Terminal };
struct Message { U Payload = 0, Header = 0; SegmentState State = SegmentState::Pending; bool PayloadRetired = false; };
struct Snapshot {
    std::uint32_t Generation = 0; U ObservedAtUs = 0;
    U UniqueStreamBytesFirstSent = 0, UniqueStreamBytesAcked = 0, PayloadBytesAcked = 0, RetransmitStreamBytes = 0;
    U PendingStreamBytes = 0, SentUnackedStreamBytes = 0; FeedbackState State = FeedbackState::Missing;
};

class Model {
public:
    explicit Model(std::uint32_t GenerationValue = 1) : Generation(GenerationValue) { if (!Generation) throw std::invalid_argument("generation"); }
    std::size_t Submit(U Payload, U Header = 3) {
        if (!Payload || Payload > G || Header > 16 || Count == Messages.size()) throw std::invalid_argument("message");
        Messages[Count] = {.Payload = Payload, .Header = Header}; Pending += Stream(Messages[Count]); Created += Payload; return Count++;
    }
    void FirstSend(std::size_t Index) { auto &M=At(Index); Need(M.State==SegmentState::Pending,"first-send"); Move(Pending,Unacked,Stream(M)); FirstSent+=Stream(M); Physical+=Stream(M); M.State=SegmentState::InFlight; }
    void Lose(std::size_t Index) { auto &M=At(Index); Need(M.State==SegmentState::InFlight,"loss"); Move(Unacked,Pending,Stream(M)); M.State=SegmentState::Retry; }
    void Retransmit(std::size_t Index) { auto &M=At(Index); Need(M.State==SegmentState::Retry,"retransmit"); Move(Pending,Unacked,Stream(M)); Retransmitted+=Stream(M); Physical+=Stream(M); M.State=SegmentState::InFlight; }
    void Ack(std::size_t Index) {
        auto &M=At(Index); Need(M.State==SegmentState::InFlight||M.State==SegmentState::Retry,"ack");
        U S=Stream(M); if(M.State==SegmentState::InFlight) Sub(Unacked,S); else Sub(Pending,S); Acked+=S; M.State=SegmentState::Acked;
        if(!M.PayloadRetired){M.PayloadRetired=true;PayloadAcked+=M.Payload;}
    }
    void DuplicateAck(std::size_t Index) { Need(At(Index).State==SegmentState::Acked,"duplicate"); }
    void TerminalRelease() { Terminal=true; Released += Outstanding(); Pending=Unacked=0; }
    Snapshot Observe(U AtUs) const { return {Generation,AtUs,FirstSent,Acked,PayloadAcked,Retransmitted,Pending,Unacked,Terminal?FeedbackState::Terminal:FeedbackState::Fresh}; }
    U LogicalCreated()const{return Created;} U LogicalDrained()const{return PayloadAcked;} U LogicalReleased()const{return Released;} U Outstanding()const{return Created-PayloadAcked-Released;}
    U PhysicalBytes()const{return Physical;} U PendingBytes()const{return Pending;} U UnackedBytes()const{return Unacked;} bool Conserved()const{return Created==PayloadAcked+Released+Outstanding();}
private:
    std::array<Message,16> Messages{}; std::size_t Count=0; std::uint32_t Generation=0; bool Terminal=false;
    U Created=0,PayloadAcked=0,Released=0,FirstSent=0,Acked=0,Retransmitted=0,Pending=0,Unacked=0,Physical=0;
    Message &At(std::size_t I){if(I>=Count)throw std::out_of_range("message");return Messages[I];}
    static U Stream(const Message&M){if(M.Payload>std::numeric_limits<U>::max()-M.Header)throw std::overflow_error("stream");return M.Payload+M.Header;}
    static void Need(bool V,const char*W){if(!V)throw std::runtime_error(W);} static void Sub(U&A,U B){Need(A>=B,"underflow");A-=B;} static void Move(U&A,U&B,U N){Sub(A,N);if(N>std::numeric_limits<U>::max()-B)throw std::overflow_error("move");B+=N;}
};

struct Qualifier {
    std::uint32_t Generation=0; std::optional<Snapshot> Previous; bool Qualified=false; U NextQualificationGrantUs=0;
    bool Sample(const Snapshot&S) {
        if(!S.Generation || (Generation && S.Generation!=Generation)){Reset(S.Generation);Previous=S;return false;}
        if(!Generation)Generation=S.Generation;
        if(S.State!=FeedbackState::Fresh){Qualified=false;Previous=S;return false;}
        if(Previous){
            if(S.ObservedAtUs<Previous->ObservedAtUs||S.UniqueStreamBytesFirstSent<Previous->UniqueStreamBytesFirstSent||S.UniqueStreamBytesAcked<Previous->UniqueStreamBytesAcked||S.PayloadBytesAcked<Previous->PayloadBytesAcked||S.RetransmitStreamBytes<Previous->RetransmitStreamBytes){Qualified=false;Previous=S;return false;}
            U Dt=S.ObservedAtUs-Previous->ObservedAtUs, First=S.UniqueStreamBytesFirstSent-Previous->UniqueStreamBytesFirstSent, Ack=S.UniqueStreamBytesAcked-Previous->UniqueStreamBytesAcked;
            if(Dt && Dt<=FreshnessUs && Dt<=std::numeric_limits<U>::max()/DrainFloor){U Need=(DrainFloor*Dt+999'999)/1'000'000; Qualified=First>=Need && Ack>0;}else Qualified=false;
        }
        Previous=S; return Qualified;
    }
    bool MayOrdinaryGrant(bool OutstandingDebt)const{return Qualified&&!OutstandingDebt;}
    bool MayQualificationGrant(U Now,bool OutstandingDebt){if(OutstandingDebt||Now<NextQualificationGrantUs)return false;NextQualificationGrantUs=Now+RequalificationUs;return true;}
    void Reset(std::uint32_t G){Generation=G;Previous.reset();Qualified=false;NextQualificationGrantUs=0;}
};
inline void Check(bool V,std::string_view W){if(!V)throw std::runtime_error(std::string(W));}

inline bool RunReliableTransportFeedbackModelTests(){std::size_t Passed=0;auto T=[&](const char*N,auto F){try{F();++Passed;std::cout<<"[ReliableFeedbackModel] "<<N<<"=pass\n";}catch(const std::exception&E){std::cerr<<"[ReliableFeedbackModel] "<<N<<"=FAIL "<<E.what()<<'\n';throw;}};try{
T("UniqueFirstSendDrain",[]{Model M;auto I=M.Submit(64*KiB);M.FirstSend(I);M.Ack(I);Check(M.LogicalDrained()==64*KiB&&M.Conserved(),"drain");});
T("OneRetransmission",[]{Model M;auto I=M.Submit(64*KiB);M.FirstSend(I);M.Lose(I);M.Retransmit(I);M.Ack(I);Check(M.LogicalDrained()==64*KiB&&M.PhysicalBytes()>M.LogicalCreated(),"retry");});
T("RepeatedRetransmissions",[]{Model M;auto I=M.Submit(G);M.FirstSend(I);for(int N=0;N<3;++N){M.Lose(I);M.Retransmit(I);}M.Ack(I);Check(M.LogicalDrained()==G&&M.PhysicalBytes()>3*G&&M.Conserved(),"retries");});
T("DelayedAck",[]{Model M;auto I=M.Submit(G);M.FirstSend(I);Check(M.Observe(40'000).PayloadBytesAcked==0,"early");M.Ack(I);Check(M.LogicalDrained()==G,"late");});
T("DuplicateAck",[]{Model M;auto I=M.Submit(8*KiB);M.FirstSend(I);M.Ack(I);auto A=M.Observe(1);M.DuplicateAck(I);auto B=M.Observe(2);Check(A.PayloadBytesAcked==B.PayloadBytesAcked&&A.UniqueStreamBytesAcked==B.UniqueStreamBytesAcked,"dup");});
T("AckAfterRetransmission",[]{Model M;auto I=M.Submit(8*KiB);M.FirstSend(I);M.Lose(I);M.Retransmit(I);M.Ack(I);Check(M.LogicalDrained()==8*KiB,"ack retry");});
T("QueueBytesReenterPending",[]{Model M;auto I=M.Submit(32*KiB);U S=M.PendingBytes();M.FirstSend(I);M.Lose(I);Check(M.PendingBytes()==S&&M.UnackedBytes()==0,"reenter");M.Retransmit(I);Check(M.PendingBytes()+M.UnackedBytes()==S,"sum");});
T("PeerDrainsAtFloor",[]{Qualifier Q;Snapshot A{.Generation=1,.ObservedAtUs=0,.State=FeedbackState::Fresh};Q.Sample(A);auto B=A;B.ObservedAtUs=25'000;B.UniqueStreamBytesFirstSent=410*KiB;B.UniqueStreamBytesAcked=64*KiB;Check(Q.Sample(B)&&Q.MayOrdinaryGrant(false),"floor");});
T("PeerDrainsBelowFloor",[]{Qualifier Q;Snapshot A{.Generation=1,.ObservedAtUs=0,.State=FeedbackState::Fresh};Q.Sample(A);auto B=A;B.ObservedAtUs=25'000;B.UniqueStreamBytesFirstSent=300*KiB;B.UniqueStreamBytesAcked=64*KiB;Check(!Q.Sample(B)&&!Q.MayOrdinaryGrant(false),"slow");});
T("IdlePeer",[]{Qualifier Q;Snapshot A{.Generation=1,.ObservedAtUs=0,.State=FeedbackState::Fresh};Q.Sample(A);auto B=A;B.ObservedAtUs=25'000;Check(!Q.Sample(B)&&Q.MayQualificationGrant(25'000,false),"idle bootstrap");});
T("FirstGrantBootstrap",[]{Qualifier Q;Check(Q.MayQualificationGrant(0,false),"first");Check(!Q.MayQualificationGrant(1,false),"bounded");});
T("DrainedSlowPeerRequalification",[]{Qualifier Q;Check(Q.MayQualificationGrant(0,false),"first");Snapshot A{.Generation=1,.ObservedAtUs=0,.State=FeedbackState::Fresh};Q.Sample(A);auto B=A;B.ObservedAtUs=25'000;B.UniqueStreamBytesFirstSent=200*KiB;B.UniqueStreamBytesAcked=32*KiB;Check(!Q.Sample(B)&&!Q.MayOrdinaryGrant(false),"deny");Check(!Q.MayQualificationGrant(500'000,false),"cooldown");Check(Q.MayQualificationGrant(1'000'000,false),"probe");auto C=B;C.ObservedAtUs=45'000;C.UniqueStreamBytesFirstSent+=400*KiB;C.UniqueStreamBytesAcked+=64*KiB;Check(Q.Sample(C)&&Q.MayOrdinaryGrant(false),"requalified");});
T("StaleFeedback",[]{Qualifier Q;Snapshot A{.Generation=1,.ObservedAtUs=0,.State=FeedbackState::Fresh};Q.Sample(A);auto B=A;B.ObservedAtUs=FreshnessUs+1;B.UniqueStreamBytesFirstSent=G;B.UniqueStreamBytesAcked=G;Check(!Q.Sample(B),"stale");});
T("MissingFeedback",[]{Qualifier Q;Snapshot A{.Generation=1,.State=FeedbackState::Missing};Check(!Q.Sample(A),"missing");});
T("ConnectionTeardown",[]{Model M;auto A=M.Submit(100*KiB),B=M.Submit(200*KiB);M.FirstSend(A);M.Ack(A);M.FirstSend(B);M.TerminalRelease();Check(M.LogicalDrained()==100*KiB&&M.LogicalReleased()==200*KiB&&M.Conserved(),"terminal");});
T("ReconnectNewGeneration",[]{Qualifier Q;Snapshot A{.Generation=1,.ObservedAtUs=0,.State=FeedbackState::Fresh};Q.Sample(A);auto B=A;B.Generation=2;B.ObservedAtUs=1;Check(!Q.Sample(B)&&Q.Generation==2&&!Q.Qualified&&Q.MayQualificationGrant(1,false),"generation");});
T("CounterResetWrap",[]{Qualifier Q;Snapshot A{.Generation=1,.ObservedAtUs=0,.UniqueStreamBytesFirstSent=1000,.UniqueStreamBytesAcked=900,.PayloadBytesAcked=800,.State=FeedbackState::Fresh};Q.Sample(A);auto B=A;B.ObservedAtUs=10'000;B.UniqueStreamBytesAcked=899;Check(!Q.Sample(B)&&!Q.Qualified,"reset");});
T("TransportFailure",[]{Qualifier Q;Snapshot A{.Generation=1,.State=FeedbackState::Contradictory};Check(!Q.Sample(A),"failure");Model M;auto I=M.Submit(G);M.FirstSend(I);M.TerminalRelease();Check(M.LogicalReleased()==G&&M.Conserved(),"release");});
T("LogicalDebtPhysicalCostSeparated",[]{Model M;auto I=M.Submit(G);M.FirstSend(I);M.Lose(I);M.Retransmit(I);M.Lose(I);M.Retransmit(I);M.Ack(I);Check(M.LogicalCreated()==G&&M.LogicalDrained()==G&&M.PhysicalBytes()>2*G&&M.Conserved(),"separate");});
}catch(...){return false;}std::cout<<"[ReliableFeedbackModel] result=pass tests="<<Passed<<'\n';return Passed==19;}
} // namespace gargantuan::test::reliable_feedback_model
