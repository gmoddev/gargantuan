#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace gargantuan::test::pooled_service_model {
using U = std::uint64_t;
constexpr U KiB=1024, MiB=1024*KiB, G=512*KiB, StepUs=500;
enum class Mode:std::uint8_t{FullReservation,PooledService};
enum class Feedback:std::uint8_t{Missing,Fresh,Error};
enum class Offer:std::uint8_t{Queued,Illegal,Bounded,Disconnected};
struct Profile{
 Mode mode=Mode::PooledService; std::uint32_t peers=32; U group=G,backend=96*MiB,game=8*MiB,structural=64*MiB,control=4*MiB,transport=8*MiB;
 U drain=16*MiB,peerRate=2*MiB,globalRate=64*MiB,peerBurst=G,globalBurst=4*G; std::uint32_t grants=4;
 U peerPending=G,globalPending=32*G,staleUs=50'000; std::uint32_t nonqueueMs=100,rpcP95Ms=150,eventMaxMs=250;
 U peerGameBurst=20*KiB,globalGameBurst=160*KiB,globalGameSteady=256*KiB,controlBurst=64*KiB;
};
inline bool addOv(U a,U b,U& o){if(b>std::numeric_limits<U>::max()-a)return true;o=a+b;return false;}
inline bool mulOv(U a,U b,U& o){if(a&&b>std::numeric_limits<U>::max()/a)return true;o=a*b;return false;}
inline U rateBytes(U rate,U us,U rem=0){constexpr U S=1'000'000;U whole=0,frac=0,n=0,out=0;if(mulOv(us/S,rate,whole)||mulOv(us%S,rate,frac)||addOv(frac,rem,n)||addOv(whole,n/S,out))throw std::overflow_error("rate");return out;}
inline U rateRem(U rate,U us,U rem=0){U p=0,n=0;if(mulOv(us%1'000'000,rate,p)||addOv(p,rem,n))throw std::overflow_error("rem");return n%1'000'000;}
inline U ceilUs(U bytes,U rate){if(!rate)throw std::overflow_error("zero");U s=bytes/rate,r=bytes%rate,w=0,n=0,o=0;if(mulOv(s,1'000'000,w))throw std::overflow_error("dur");if(!r)return w;if(mulOv(r,1'000'000,n))throw std::overflow_error("dur");U f=n/rate+(n%rate!=0);if(addOv(w,f,o))throw std::overflow_error("dur");return o;}
inline bool Valid(const Profile&p){
 if(p.mode!=Mode::PooledService||p.peers!=32||p.group!=G||!p.backend||!p.game||!p.structural||!p.control||!p.transport||!p.drain||!p.peerRate||!p.globalRate||!p.grants||!p.staleUs)return false;
 if(p.nonqueueMs>=p.rpcP95Ms||p.rpcP95Ms>p.eventMaxMs)return false;
 U x=0,y=0;
 if(addOv(p.structural,p.game,x)||addOv(x,p.control,y)||addOv(y,p.transport,x)||x>p.backend)return false;
 if(mulOv(p.grants,p.drain,x)||x>p.structural||mulOv(p.peers,p.peerRate,x)||x>p.structural)return false;
 if(mulOv(p.grants,p.group,x)||p.globalBurst<x||p.peerBurst<p.group||p.peerPending<p.group)return false;
 if(mulOv(p.peers,p.peerPending,x)||p.globalPending<x)return false;
 U q=U(p.rpcP95Ms-p.nonqueueMs)*1000;
 if(p.staleUs>q)return false;
 try{
  U peerFollower=0;if(addOv(p.group,p.peerGameBurst,peerFollower)||ceilUs(peerFollower,p.drain)>q)return false;
  if(p.game<=p.globalGameSteady||ceilUs(p.globalGameBurst,p.game-p.globalGameSteady)>q)return false;
  U funded=rateBytes(p.backend,q),total=0,t=rateBytes(p.transport,q);if(mulOv(p.grants,p.group,total)||addOv(total,p.globalGameBurst,total)||addOv(total,p.controlBurst,total)||addOv(total,t,total)||total>funded)return false;
 }catch(...){return false;}
 return true;
}
struct Metrics{U known=0,rollback=0,overload=0,maxCommitted=0,maxPending=0,maxGame=0,maxControl=0,maxGrantGap=0,maxGameDelay=0;};
struct Peer{
 bool used=false,connected=false,teardown=false,pending=false,reserved=false,grant=false;std::uint32_t gen=0;Feedback fb=Feedback::Missing;U fbUs=0;
 U credit=0,creditRem=0,creditUs=0,pendingBytes=0,reservedBytes=0,token=0,structDebt=0,gameAhead=0,gameBehind=0,controlAhead=0,controlBehind=0;
 U rate=0,serviceRem=0;std::optional<U> firstGrant,firstService,lastGrant,structDone,gameQueued,gameDone;U completions=0;
};
struct Reservation{U token=0;std::uint32_t slot=0,gen=0;U bytes=0;};
class Model{
 Profile p;std::array<Peer,32>a{};Metrics m{};U now=0,gCredit=0,gRem=0,gUs=0,pending=0,reserved=0,committed=0,game=0,control=0,nextToken=1,ordinaryRem=0;std::uint32_t cursor=0,ordinaryCursor=0;std::optional<std::uint32_t> earmark;std::optional<Reservation> active;std::size_t grantCount=0;
 static std::size_t I(std::uint32_t s){return s-1;} Peer* F(std::uint32_t s,std::uint32_t g){if(!s||s>p.peers)return nullptr;auto&v=a[I(s)];return v.used&&v.gen==g?&v:nullptr;}
 void refill(U&b,U&r,U&u,U rate,U cap){U e=now-u;u=now;if(!e||b>=cap)return;U x=rateBytes(rate,e,r);r=rateRem(rate,e,r);b+=std::min(cap-b,x);if(b==cap)r=0;}
 void refillP(Peer&v){refill(v.credit,v.creditRem,v.creditUs,p.peerRate,p.peerBurst);}void refillG(){refill(gCredit,gRem,gUs,p.globalRate,p.globalBurst);}
 bool fresh(const Peer&v)const{return v.fb==Feedback::Fresh&&v.rate>=p.drain&&now>=v.fbUs&&now-v.fbUs<=p.staleUs;}
 bool eligible(Peer&v){if(!v.used||!v.connected||!v.pending||v.reserved||v.grant)return false;refillP(v);return fresh(v)&&v.credit>=v.pendingBytes;}
 void refund(Peer&v,U b){v.credit+=std::min(b,p.peerBurst-v.credit);gCredit+=std::min(b,p.globalBurst-gCredit);}
 void drain(U&d,U&gd,U&budget){U x=std::min(d,budget);d-=x;gd-=x;budget-=x;}
 void drainPeer(Peer&v,U b){U before=v.structDebt;if(v.controlAhead)drain(v.controlAhead,control,b);if(b&&v.gameAhead)drain(v.gameAhead,game,b);if(b&&v.structDebt){if(!v.firstService)v.firstService=now;drain(v.structDebt,committed,b);}if(before&&!v.structDebt){v.structDone=now;++v.completions;}if(b&&v.controlBehind)drain(v.controlBehind,control,b);if(b&&v.gameBehind)drain(v.gameBehind,game,b);if(!v.gameAhead&&!v.gameBehind&&v.gameQueued){v.gameDone=now;m.maxGameDelay=std::max(m.maxGameDelay,now-*v.gameQueued);v.gameQueued.reset();}if(!v.structDebt&&!v.gameAhead&&!v.gameBehind&&!v.controlAhead&&!v.controlBehind&&v.grant){v.grant=false;--grantCount;}}
 void drainOrd(U us){U rate=p.game+p.control,b=rateBytes(rate,us,ordinaryRem);ordinaryRem=rateRem(rate,us,ordinaryRem);for(std::uint32_t k=0;k<p.peers&&b;++k){auto s=(ordinaryCursor+k)%p.peers+1;auto&v=a[I(s)];if(!v.used||v.grant)continue;if(v.controlAhead)drain(v.controlAhead,control,b);if(b&&v.gameAhead)drain(v.gameAhead,game,b);if(!v.gameAhead&&!v.gameBehind&&v.gameQueued){v.gameDone=now;m.maxGameDelay=std::max(m.maxGameDelay,now-*v.gameQueued);v.gameQueued.reset();}}ordinaryCursor=(ordinaryCursor+1)%p.peers;}
 std::optional<Reservation> reservePeer(std::uint32_t s){auto&v=a[I(s)];refillP(v);refillG();if(!fresh(v)||v.credit<v.pendingBytes||grantCount>=p.grants||!Invariant(v.pendingBytes))return{};if(gCredit<v.pendingBytes){earmark=s;return{};}U b=v.pendingBytes;v.credit-=b;gCredit-=b;v.reserved=true;v.reservedBytes=b;if(nextToken==std::numeric_limits<U>::max())return{};v.token=nextToken++;reserved+=b;++grantCount;cursor=s%p.peers;if(earmark==s)earmark.reset();active=Reservation{v.token,s,v.gen,b};return active;}
 void clearDebt(Peer&v){committed-=v.structDebt;game-=v.gameAhead+v.gameBehind;control-=v.controlAhead+v.controlBehind;v.structDebt=v.gameAhead=v.gameBehind=v.controlAhead=v.controlBehind=0;}
public:
 explicit Model(Profile x={}):p(x){if(!Valid(p))throw std::invalid_argument("profile");}
 const Profile&P()const{return p;}const Metrics&M()const{return m;}const Peer&At(std::uint32_t s)const{return a.at(I(s));}U Now()const{return now;}U Committed()const{return committed;}U Reserved()const{return reserved;}U GameDebt()const{return game;}U ControlDebt()const{return control;}U Pending()const{return pending;}U GlobalCredit()const{return gCredit;}std::size_t Grants()const{return grantCount;}std::size_t LogicalBytes()const{return sizeof(*this);}std::size_t Used()const{return std::count_if(a.begin(),a.end(),[](auto&v){return v.used;});}
 bool Connect(std::uint32_t s,std::uint32_t g){if(!s||s>p.peers||!g||a[I(s)].used)return false;auto&v=a[I(s)];v={};v.used=v.connected=true;v.gen=g;v.creditUs=v.fbUs=now;v.rate=p.drain;return true;}
 bool Fresh(std::uint32_t s,std::uint32_t g){auto*v=F(s,g);if(!v||!v->connected)return false;v->fb=Feedback::Fresh;v->fbUs=now;return true;}bool Missing(std::uint32_t s,std::uint32_t g){auto*v=F(s,g);if(!v)return false;v->fb=Feedback::Missing;return true;}bool Error(std::uint32_t s,std::uint32_t g){auto*v=F(s,g);if(!v)return false;v->fb=Feedback::Error;return true;}bool DrainRate(std::uint32_t s,std::uint32_t g,U r){auto*v=F(s,g);if(!v)return false;v->rate=r;return true;}
 Offer OfferStructural(std::uint32_t s,std::uint32_t g,U b){auto*v=F(s,g);if(!v||!v->connected)return Offer::Disconnected;if(!b||b>p.group)return Offer::Illegal;if(b>p.peerPending||v->pending||v->reserved||v->grant||b>p.globalPending-std::min(p.globalPending,pending)){++m.overload;return Offer::Bounded;}v->pending=true;v->pendingBytes=b;pending+=b;m.maxPending=std::max(m.maxPending,pending);return Offer::Queued;}
 bool Gameplay(std::uint32_t s,std::uint32_t g,U b){auto*v=F(s,g);if(!v||!v->connected||!b||b>p.peerGameBurst||b>p.globalGameBurst-std::min(p.globalGameBurst,game))return false;if(!v->gameQueued)v->gameQueued=now;(v->grant||v->reserved||v->structDebt?v->gameBehind:v->gameAhead)+=b;game+=b;m.maxGame=std::max(m.maxGame,game);return true;}
 bool Control(std::uint32_t s,std::uint32_t g,U b){auto*v=F(s,g);if(!v||!v->connected||!b||b>p.controlBurst-std::min(p.controlBurst,control))return false;(v->grant||v->reserved||v->structDebt?v->controlBehind:v->controlAhead)+=b;control+=b;m.maxControl=std::max(m.maxControl,control);return true;}
 void Advance(U us){if(us>std::numeric_limits<U>::max()-now)throw std::overflow_error("time");now+=us;refillG();for(auto&v:a)if(v.used)refillP(v);}void Service(U us){Advance(us);for(auto&v:a)if(v.used&&v.grant&&v.rate){U b=rateBytes(v.rate,us,v.serviceRem);v.serviceRem=rateRem(v.rate,us,v.serviceRem);if(b)drainPeer(v,b);}drainOrd(us);}
 bool Invariant(U prospective=0)const{U q=U(p.rpcP95Ms-p.nonqueueMs)*1000,f=0,t=0;try{f=rateBytes(p.backend,q);t=rateBytes(p.transport,q);}catch(...){return false;}U total=committed+reserved;if(prospective>std::numeric_limits<U>::max()-total)return false;total+=prospective;U gd=std::max(game,p.globalGameBurst),cd=std::max(control,p.controlBurst);return !addOv(total,gd,total)&&!addOv(total,cd,total)&&!addOv(total,t,total)&&total<=f;}
 std::optional<Reservation> Reserve(){if(active||grantCount>=p.grants)return{};refillG();if(earmark){auto&v=a[I(*earmark)];if(!eligible(v))earmark.reset();else return reservePeer(*earmark);}for(std::uint32_t k=0;k<p.peers;++k){auto s=(cursor+k)%p.peers+1;auto&v=a[I(s)];if(!v.used||!v.connected||!v.pending||v.reserved||v.grant)continue;refillP(v);if(!fresh(v)||v.credit<v.pendingBytes)continue;if(gCredit<v.pendingBytes){earmark=s;return{};}return reservePeer(s);}return{};}
 bool Accept(const Reservation&r){if(!active||active->token!=r.token)return false;auto*v=F(r.slot,r.gen);if(!v||!v->reserved||v->token!=r.token)return false;v->reserved=false;v->reservedBytes=v->token=0;v->pending=false;v->pendingBytes=0;v->grant=true;v->structDebt+=r.bytes;reserved-=r.bytes;committed+=r.bytes;pending-=r.bytes;++m.known;m.maxCommitted=std::max(m.maxCommitted,committed);if(!v->firstGrant)v->firstGrant=now;if(v->lastGrant)m.maxGrantGap=std::max(m.maxGrantGap,now-*v->lastGrant);v->lastGrant=now;active.reset();return true;}
 bool Fail(const Reservation&r){if(!active||active->token!=r.token)return false;auto*v=F(r.slot,r.gen);if(!v||!v->reserved)return false;refund(*v,r.bytes);v->reserved=false;v->reservedBytes=v->token=0;reserved-=r.bytes;--grantCount;m.rollback+=r.bytes;active.reset();return true;}
 bool Disconnect(std::uint32_t s,std::uint32_t g){auto*v=F(s,g);if(!v)return false;if(v->reserved&&active&&active->slot==s){auto r=*active;if(!Fail(r))return false;}if(v->pending){pending-=v->pendingBytes;v->pending=false;v->pendingBytes=0;}v->credit=v->creditRem=0;if(earmark==s)earmark.reset();v->connected=false;if(v->grant||v->structDebt||v->gameAhead||v->gameBehind||v->controlAhead||v->controlBehind){v->teardown=true;return true;}v->used=false;return true;}
 bool FinishTeardown(std::uint32_t s,std::uint32_t g){auto*v=F(s,g);if(!v||!v->teardown)return false;clearDebt(*v);if(v->grant){v->grant=false;--grantCount;}*v={};return true;}
 bool Converged()const{return !pending&&!reserved&&!committed;}
};
inline void R(bool x,std::string_view m){if(!x)throw std::runtime_error(std::string(m));}
inline void All(Model&m){for(std::uint32_t s=1;s<=32;++s){R(m.Connect(s,1),"connect");R(m.Fresh(s,1),"fresh");}}
inline void FreshAll(Model&m){for(std::uint32_t s=1;s<=32;++s)if(m.At(s).used&&m.At(s).connected)R(m.Fresh(s,m.At(s).gen),"refresh");}
inline void Admit(Model&m){while(auto r=m.Reserve())R(m.Accept(*r),"accept");}
inline void Healthy(Model&m,U us=StepUs){m.Service(us);FreshAll(m);Admit(m);}
inline U pct(std::array<U,32>v,unsigned p){std::sort(v.begin(),v.end());auto n=(v.size()*p+99)/100;return v[std::min(v.size()-1,n-1)];}
inline bool RunPooledReliableServiceModelTests(){
 Profile c{};if(!Valid(c)){std::cerr<<"[PooledServiceModel] candidate-invalid\n";return false;}std::size_t passed=0;
 auto T=[&](const char*n,auto f){try{f();++passed;std::cout<<"[PooledServiceModel] "<<n<<"=pass\n";}catch(const std::exception&e){std::cerr<<"[PooledServiceModel] "<<n<<"=FAIL "<<e.what()<<'\n';throw;}};
 try{
 T("SmallGroupSinglePeer",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");R(m.OfferStructural(1,1,4*KiB)==Offer::Queued,"o");m.Advance(2'000);m.Fresh(1,1);auto r=m.Reserve();R(r&&m.Accept(*r),"a");while(!m.Converged()){m.Service(StepUs);m.Fresh(1,1);}R(m.M().known==1,"k");});
 T("MaximumGroupSinglePeer",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");R(m.OfferStructural(1,1,G)==Offer::Queued,"o");m.Advance(250'000);m.Fresh(1,1);auto r=m.Reserve();R(r&&m.Accept(*r),"a");while(!m.Converged()){m.Service(StepUs);m.Fresh(1,1);}R(m.At(1).structDone&&*m.At(1).structDone<=282'000,"d");});
 T("MaximumGroupPlusOneRejected",[]{Model m;R(m.Connect(1,1),"c");R(m.OfferStructural(1,1,G+1)==Offer::Illegal&&m.M().known==0&&!m.Pending(),"reject");});
 T("CreditInsufficientDefers",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");m.OfferStructural(1,1,G);m.Advance(249'999);m.Fresh(1,1);R(!m.Reserve()&&!m.M().known,"defer");});
 T("AggregateCapacityInsufficientDefers",[]{Model m;All(m);for(std::uint32_t s=1;s<=32;++s)m.OfferStructural(s,1,G);m.Advance(250'000);FreshAll(m);Admit(m);R(m.Grants()==4&&m.Committed()==4*G&&!m.Reserve(),"cap");});
 T("GameplayBehindMaximumGroup",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");m.OfferStructural(1,1,G);m.Advance(250'000);m.Fresh(1,1);auto r=m.Reserve();R(r&&m.Accept(*r),"a");R(m.Gameplay(1,1,20*KiB),"g");while(m.GameDebt()){m.Service(StepUs);m.Fresh(1,1);}R(m.M().maxGameDelay<=33'000&&m.M().maxGameDelay+100'000<=150'000,"fifo");std::cout<<"[PooledServiceModel] GameplayBehindMaximumGroup delay_us="<<m.M().maxGameDelay<<" e2e_bound_us="<<m.M().maxGameDelay+100'000<<'\n';});
 T("GameplayBurstBeforeStructuralGrant",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");R(m.Gameplay(1,1,20*KiB),"g");m.OfferStructural(1,1,G);while(m.Now()<250'000)Healthy(m);R(m.M().maxGameDelay<=3'000&&m.At(1).firstGrant,"before");});
 T("ThirtyTwoMaximumGroups",[]{Model m;All(m);for(std::uint32_t s=1;s<=32;++s)m.OfferStructural(s,1,G);U origin=m.Now();m.Advance(250'000);FreshAll(m);Admit(m);while(!m.Converged()){Healthy(m);R(m.Now()-origin<1'000'000,"finite");}std::array<U,32>d{};U maxGrant=0,maxService=0;for(std::uint32_t s=1;s<=32;++s){auto&v=m.At(s);R(v.structDone&&v.firstGrant&&v.firstService&&v.completions==1,"all");d[s-1]=*v.structDone-origin;maxGrant=std::max(maxGrant,*v.firstGrant-250'000);maxService=std::max(maxService,*v.firstService-250'000);}R(*std::max_element(d.begin(),d.end())<=525'000&&maxGrant<=221'000&&maxService<=222'000&&m.M().maxCommitted<=4*G&&m.M().maxPending<=32*G,"bounds");std::cout<<"[PooledServiceModel] ThirtyTwoMaximumGroups p50_us="<<pct(d,50)<<" p95_us="<<pct(d,95)<<" p99_us="<<pct(d,99)<<" max_us="<<*std::max_element(d.begin(),d.end())<<" max_first_grant_from_eligible_us="<<maxGrant<<" max_first_service_from_eligible_us="<<maxService<<" committed_high="<<m.M().maxCommitted<<" pending_high="<<m.M().maxPending<<'\n';});
 T("TinyDoesNotStarveLarge",[]{Model m;All(m);m.OfferStructural(1,1,G);for(std::uint32_t s=2;s<=32;++s)m.OfferStructural(s,1,4*KiB);m.Advance(250'000);FreshAll(m);Admit(m);while(!m.At(1).structDone)Healthy(m);R(*m.At(1).structDone<=625'000,"large");std::cout<<"[PooledServiceModel] TinyDoesNotStarveLarge large_complete_us="<<*m.At(1).structDone<<'\n';});
 T("LargeDoesNotStarveTiny",[]{Model m;All(m);for(std::uint32_t s=1;s<=31;++s)m.OfferStructural(s,1,G);m.OfferStructural(32,1,4*KiB);m.Advance(250'000);FreshAll(m);Admit(m);while(!m.At(32).structDone)Healthy(m);R(*m.At(32).structDone<=625'000,"tiny");std::cout<<"[PooledServiceModel] LargeDoesNotStarveTiny tiny_complete_us="<<*m.At(32).structDone<<'\n';});
 T("AlternatingTinyAndLargeFair",[]{Model m;R(m.Connect(1,1)&&m.Connect(2,1)&&m.Fresh(1,1)&&m.Fresh(2,1),"c");for(int i=0;i<8;++i){m.OfferStructural(1,1,G);m.OfferStructural(2,1,4*KiB);U a=m.At(1).completions,b=m.At(2).completions,deadline=m.Now()+400'000;while((m.At(1).completions==a||m.At(2).completions==b)&&m.Now()<deadline)Healthy(m);R(m.At(1).completions>a&&m.At(2).completions>b,"fair");}R(m.At(1).completions==8&&m.At(2).completions==8,"counts");std::cout<<"[PooledServiceModel] AlternatingTinyAndLargeFair large=8 tiny=8 max_grant_gap_us="<<m.M().maxGrantGap<<'\n';});
 T("GameplayBurstDuringStructuralWave",[]{Model m;All(m);for(std::uint32_t s=1;s<=32;++s)m.OfferStructural(s,1,G);m.Advance(250'000);FreshAll(m);Admit(m);for(std::uint32_t s=1;s<=8;++s)R(m.Gameplay(s,1,20*KiB),"g");R(m.Control(9,1,64*KiB),"ctrl");while(!m.Converged()||m.GameDebt()||m.ControlDebt())Healthy(m);R(m.M().maxGameDelay+100'000<=150'000&&m.M().maxControl==64*KiB,"mix");std::cout<<"[PooledServiceModel] GameplayBurstDuringStructuralWave gameplay_delay_us="<<m.M().maxGameDelay<<" control_high="<<m.M().maxControl<<'\n';});
 T("SlowPeerDoesNotMonopolize",[]{Model m;All(m);for(std::uint32_t s=1;s<=32;++s)m.OfferStructural(s,1,G);m.Advance(250'000);FreshAll(m);Admit(m);m.DrainRate(1,1,1*MiB);for(int i=0;i<2000;++i){Healthy(m);bool ok=true;for(std::uint32_t s=2;s<=32;++s)ok&=bool(m.At(s).structDone);if(ok)break;}U mx=0;for(std::uint32_t s=2;s<=32;++s){R(m.At(s).structDone.has_value(),"other");mx=std::max(mx,*m.At(s).structDone);}R(m.At(1).grant||m.At(1).structDone,"slow");std::cout<<"[PooledServiceModel] SlowPeerDoesNotMonopolize healthy_max_complete_us="<<mx<<" slow_remaining="<<m.At(1).structDebt<<'\n';});
 T("GameplayWhileSlowPeerOutstanding",[]{Model m;All(m);for(std::uint32_t s=1;s<=4;++s)m.OfferStructural(s,1,G);m.Advance(250'000);FreshAll(m);Admit(m);m.DrainRate(1,1,1*MiB);R(m.Gameplay(5,1,20*KiB),"g");while(m.GameDebt())Healthy(m);R(m.M().maxGameDelay+100'000<=150'000&&m.At(1).structDebt>0&&m.OfferStructural(1,1,4*KiB)==Offer::Bounded,"protect");std::cout<<"[PooledServiceModel] GameplayWhileSlowPeerOutstanding gameplay_delay_us="<<m.M().maxGameDelay<<" slow_remaining="<<m.At(1).structDebt<<'\n';});
 T("NonDrainingPeerContainsDebt",[]{Model m;All(m);for(std::uint32_t s=1;s<=32;++s)m.OfferStructural(s,1,G);m.Advance(250'000);FreshAll(m);Admit(m);m.DrainRate(1,1,0);for(int i=0;i<3000;++i)Healthy(m);R(m.At(1).structDebt==G&&m.Committed()==G&&m.Grants()==1,"stall");for(std::uint32_t s=2;s<=32;++s)R(m.At(s).structDone.has_value(),"others");std::cout<<"[PooledServiceModel] NonDrainingPeerContainsDebt committed="<<m.Committed()<<" active_grants="<<m.Grants()<<'\n';});
 T("MissingFeedbackFailsConservatively",[]{Model m;R(m.Connect(1,1),"c");m.OfferStructural(1,1,G);m.Advance(250'000);R(!m.Reserve(),"missing");m.Fresh(1,1);auto r=m.Reserve();R(r&&m.Accept(*r),"restore");});
 T("ContradictoryFeedbackFailsConservatively",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");m.OfferStructural(1,1,G);m.Advance(250'000);m.Error(1,1);R(!m.Reserve()&&!m.M().known,"error");});
 T("FailedAcceptanceRollsBackGrant",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");m.OfferStructural(1,1,G);m.Advance(250'000);m.Fresh(1,1);U pc=m.At(1).credit,gc=m.GlobalCredit();auto r=m.Reserve();R(r&&m.Fail(*r),"fail");R(!m.Reserved()&&!m.Committed()&&!m.Grants()&&m.At(1).credit==pc&&m.GlobalCredit()==gc&&!m.M().known,"rollback");});
 T("DisconnectBeforeAdmission",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");m.OfferStructural(1,1,G);m.Advance(100'000);R(m.Disconnect(1,1)&&!m.Pending()&&!m.Used(),"disc");});
 T("DisconnectAfterGrantBeforeAcceptance",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");m.OfferStructural(1,1,G);m.Advance(250'000);m.Fresh(1,1);auto r=m.Reserve();R(r&&m.Disconnect(1,1),"disc");R(!m.Reserved()&&!m.Committed()&&!m.Grants()&&!m.M().known&&m.M().rollback==G,"release");});
 T("DisconnectWithCommittedDebt",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");m.OfferStructural(1,1,G);m.Advance(250'000);m.Fresh(1,1);auto r=m.Reserve();R(r&&m.Accept(*r),"a");R(m.Disconnect(1,1)&&m.Committed()==G&&!m.Connect(1,2),"hold");R(m.FinishTeardown(1,1)&&!m.Committed()&&m.Connect(1,2),"clear");});
 T("AcceptedBackendFailureRetainsDebtUntilTeardown",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");m.OfferStructural(1,1,G);m.Advance(250'000);m.Fresh(1,1);auto r=m.Reserve();R(r&&m.Accept(*r),"a");m.DrainRate(1,1,0);m.Error(1,1);m.Advance(500'000);R(m.Committed()==G&&m.M().known==1,"keep");R(m.Disconnect(1,1)&&m.Committed()==G&&m.FinishTeardown(1,1)&&!m.Committed()&&!m.M().rollback,"cleanup");});
 T("ReconnectStartsZeroCredit",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");m.Advance(250'000);R(m.At(1).credit==G&&m.Disconnect(1,1)&&m.Connect(1,2),"re");R(!m.At(1).credit&&!m.At(1).pending&&!m.At(1).grant,"zero");});
 T("IdleCreditCaps",[]{Model m;All(m);m.Advance(60'000'000);for(std::uint32_t s=1;s<=32;++s)R(m.At(s).credit==G,"pc");R(m.GlobalCredit()==4*G,"gc");});
 T("ReconnectChurnNoAdvantage",[]{Model stable,churn;R(stable.Connect(1,1)&&stable.Fresh(1,1)&&churn.Connect(1,1)&&churn.Fresh(1,1),"c");U sb=0,cb=0;std::uint32_t g=1;for(int i=0;i<20;++i){stable.Advance(100'000);stable.Fresh(1,1);if(!stable.At(1).pending&&!stable.At(1).grant)stable.OfferStructural(1,1,G);if(auto r=stable.Reserve()){stable.Accept(*r);sb+=r->bytes;}stable.Service(100'000);stable.Fresh(1,1);churn.Advance(100'000);churn.Fresh(1,g);churn.OfferStructural(1,g,G);if(auto r=churn.Reserve()){churn.Accept(*r);cb+=r->bytes;}if(churn.At(1).grant)churn.Service(100'000);R(churn.Disconnect(1,g),"d");if(churn.At(1).used&&churn.At(1).teardown)R(churn.FinishTeardown(1,g),"t");++g;R(churn.Connect(1,g)&&churn.Fresh(1,g),"r");}R(cb<=sb,"adv");std::cout<<"[PooledServiceModel] ReconnectChurnNoAdvantage stable_bytes="<<sb<<" churn_bytes="<<cb<<'\n';});
 T("SustainedOverloadBounded",[]{Model m;All(m);while(m.Now()<10'000'000){for(std::uint32_t s=1;s<=32;++s)m.OfferStructural(s,1,G);FreshAll(m);Admit(m);if(m.Now()%1'000'000==0)for(std::uint32_t s=1;s<=8;++s)(void)m.Gameplay(s,1,20*KiB);Healthy(m);R(m.Pending()<=32*G&&m.Committed()<=4*G&&m.GlobalCredit()<=4*G,"bound");for(std::uint32_t s=1;s<=32;++s)R(m.At(s).credit<=G,"credit");}R(m.M().overload&&m.M().maxGameDelay+100'000<=150'000&&m.LogicalBytes()<64*KiB,"overload");std::cout<<"[PooledServiceModel] SustainedOverload pending_high="<<m.M().maxPending<<" committed_high="<<m.M().maxCommitted<<" gameplay_delay_us="<<m.M().maxGameDelay<<" logical_bytes="<<m.LogicalBytes()<<'\n';});
 T("RecoveryConverges",[]{Model m;All(m);for(int i=0;i<2000;++i){for(std::uint32_t s=1;s<=32;++s)if(!m.At(s).pending&&!m.At(s).reserved&&!m.At(s).grant)m.OfferStructural(s,1,G);FreshAll(m);Admit(m);Healthy(m);}U stop=m.Now();while(!m.Converged()||m.GameDebt()||m.ControlDebt()){Healthy(m);R(m.Now()-stop<1'000'000,"finite");}R(m.Now()-stop<=500'000,"recover");std::cout<<"[PooledServiceModel] RecoveryConverges recovery_us="<<m.Now()-stop<<'\n';});
 T("InvalidProfileRejects",[]{auto p=Profile{};p.drain=2*MiB;R(!Valid(p),"fifo");p={};p.game=256*KiB;R(!Valid(p),"game");p={};p.structural=0;R(!Valid(p),"zero");p={};p.globalBurst=G-1;R(!Valid(p),"burst");p={};p.backend=70*MiB;R(!Valid(p),"fund");});
 T("ArithmeticOverflowRejects",[]{auto p=Profile{};p.peerRate=std::numeric_limits<U>::max();R(!Valid(p),"rate");p={};p.peerPending=std::numeric_limits<U>::max();R(!Valid(p),"pending");});
 T("CreditIsNotService",[]{Model m;All(m);for(std::uint32_t s=1;s<=5;++s)m.OfferStructural(s,1,G);m.Advance(250'000);FreshAll(m);Admit(m);R(m.At(5).credit==G&&!m.At(5).grant,"credit");});
 T("TimeAloneDoesNotReleaseDebt",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");m.OfferStructural(1,1,G);m.Advance(250'000);m.Fresh(1,1);auto r=m.Reserve();R(r&&m.Accept(*r),"a");m.Advance(10'000'000);R(m.Committed()==G,"time");});
 T("FullReservationModeDistinct",[]{auto p=Profile{};p.mode=Mode::FullReservation;R(!Valid(p),"mode");});
 T("FeedbackStalenessLimit",[]{Model m;R(m.Connect(1,1)&&m.Fresh(1,1),"c");m.OfferStructural(1,1,G);m.Advance(250'000);R(!m.Reserve(),"stale");m.Fresh(1,1);auto r=m.Reserve();R(r&&m.Accept(*r),"fresh");});
 }catch(...){return false;}
 U q=U(c.rpcP95Ms-c.nonqueueMs)*1000,f=ceilUs(c.group+c.peerGameBurst,c.drain);std::cout<<"[PooledServiceModel] candidate=pass tests="<<passed<<" peers="<<c.peers<<" backend_bps="<<c.backend<<" structural_bps="<<c.structural<<" gameplay_bps="<<c.game<<" grants="<<c.grants<<" fifo_queue_us="<<f<<" fifo_e2e_us="<<f+c.nonqueueMs*1000ull<<" queue_budget_us="<<q<<'\n';return passed==33;
}
} // namespace gargantuan::test::pooled_service_model
