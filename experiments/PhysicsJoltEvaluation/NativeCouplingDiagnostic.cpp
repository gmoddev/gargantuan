// Independent native Jolt scene: no Gargantuan physics adapter or SoftFixtures.
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/SoftBody/SoftBodyCreationSettings.h>
#include <Jolt/Physics/SoftBody/SoftBodyMotionProperties.h>
#include <Jolt/Physics/SoftBody/SoftBodyContactListener.h>
#include <Jolt/Physics/SoftBody/SoftBodyManifold.h>
#include "ExperimentSupport.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Json = nlohmann::json;
using Clock = std::chrono::steady_clock;
constexpr float Dt = 1.0f / 60.0f;
constexpr float Radius = 0.45f;
constexpr float Mass = 0.7f * (4.0f / 3.0f) * 3.14159265358979323846f * Radius * Radius * Radius;
constexpr float Inertia = 0.4f * Mass * Radius * Radius;
struct Options {
    int Frames = 150, Steps = 1, Iterations = 24, Workers = 1, Count = 1;
    int TraceStart = 16, TraceEnd = 24;
    bool Contact = true, Pinned = true, Dynamic = true, Particles = true;
    float Compliance = 0.0f, BendCompliance = 1.0e-4f, Damping = 2.4f, Friction = 0.2f;
    float Restitution = 0.0f, Seed = 0.0f, SphereZ = -2.0f, Speed = 5.0f;
    std::string Trace;
};
Options Config;
std::ofstream TraceFile;
std::mutex TraceMutex;
int CurrentFrame = -1;
template<class T> Json V(T A) { return Json::array({A.GetX(),A.GetY(),A.GetZ()}); }
double Ms(Clock::time_point Start) { return std::chrono::duration<double,std::milli>(Clock::now()-Start).count(); }

Options Parse(int Count, char** Args) {
    Options O;
    for (int I=1; I<Count; I+=2) {
        if (I+1 == Count) throw std::runtime_error("[Physics:Coupling] Missing option");
        const std::string K=Args[I], S=Args[I+1];
        if(K=="--frames") O.Frames=std::stoi(S);
        else if(K=="--steps") O.Steps=std::stoi(S);
        else if(K=="--iterations") O.Iterations=std::stoi(S);
        else if(K=="--workers") O.Workers=std::stoi(S);
        else if(K=="--count") O.Count=std::stoi(S);
        else if(K=="--contact") O.Contact=std::stoi(S)!=0;
        else if(K=="--pinned") O.Pinned=std::stoi(S)!=0;
        else if(K=="--dynamic") O.Dynamic=std::stoi(S)!=0;
        else if(K=="--particles") O.Particles=std::stoi(S)!=0;
        else if(K=="--compliance") O.Compliance=std::stof(S);
        else if(K=="--bend-compliance") O.BendCompliance=std::stof(S);
        else if(K=="--damping") O.Damping=std::stof(S);
        else if(K=="--friction") O.Friction=std::stof(S);
        else if(K=="--restitution") O.Restitution=std::stof(S);
        else if(K=="--seed") O.Seed=std::stof(S);
        else if(K=="--sphere-z") O.SphereZ=std::stof(S);
        else if(K=="--speed") O.Speed=std::stof(S);
        else if(K=="--trace") O.Trace=S;
        else if(K=="--trace-start") O.TraceStart=std::stoi(S);
        else if(K=="--trace-end") O.TraceEnd=std::stoi(S);
        else throw std::runtime_error("[Physics:Coupling] Unknown option "+K);
    }
    if(O.Frames<1||O.Frames>300||O.Steps<1||O.Steps>8||O.Iterations<1||O.Iterations>64||O.Workers<1||O.Workers>8||O.Count<1||O.Count>2||O.Compliance<0||O.BendCompliance<0||O.Damping<0||O.Friction<0||O.Restitution<0)
        throw std::runtime_error("[Physics:Coupling] Option outside bounded range");
    return O;
}

struct Layers final: JPH::BroadPhaseLayerInterface {
    JPH::uint GetNumBroadPhaseLayers() const override { return 3; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer L) const override { return JPH::BroadPhaseLayer(static_cast<JPH::uint8>(L)); }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer) const override { return "Diagnostic"; }
#endif
};
struct Pairs final: JPH::ObjectLayerPairFilter {
    bool ShouldCollide(JPH::ObjectLayer A,JPH::ObjectLayer B) const override {
        if(!Config.Contact&&((A==1&&B==2)||(A==2&&B==1))) return false;
        return A!=0||B!=0;
    }
};
struct BroadPairs final: JPH::ObjectVsBroadPhaseLayerFilter {
    bool ShouldCollide(JPH::ObjectLayer A,JPH::BroadPhaseLayer B) const override { return Pairs{}.ShouldCollide(A,B.GetValue()); }
};
struct Contacts final: JPH::SoftBodyContactListener {
    std::atomic<unsigned> Count{};
    void OnSoftBodyContactAdded(const JPH::Body&,const JPH::SoftBodyManifold& M) override {
        for(const auto& P:M.GetVertices()) if(M.HasContact(P)) Count.fetch_add(1,std::memory_order_relaxed);
    }
};
JPH::Ref<JPH::SoftBodySharedSettings> GetCloth() {
    JPH::Ref<JPH::SoftBodySharedSettings> S = new JPH::SoftBodySharedSettings;
    constexpr int N=13;
    for(int Y=0;Y<N;++Y) for(int X=0;X<N;++X) {
        JPH::SoftBodySharedSettings::Vertex P;
        P.mPosition=JPH::Float3((float(X)/(N-1)-0.5f)*4.0f,-(float(Y)/(N-1)-0.5f)*4.0f,0);
        P.mInvMass=Config.Pinned&&Y==0?0.0f:10.0f;
        if(X==N/2&&Y==N/2) P.mVelocity=JPH::Float3(0,0,Config.Seed);
        S->mVertices.push_back(P);
    }
    for(int Y=0;Y<N;++Y) for(int X=0;X<N;++X) {
        const int I=Y*N+X;
        if(X+1<N) S->mEdgeConstraints.emplace_back(I,I+1,Config.Compliance);
        if(Y+1<N) S->mEdgeConstraints.emplace_back(I,I+N,Config.Compliance);
        if(X+1<N&&Y+1<N) {
            S->mEdgeConstraints.emplace_back(I,I+N+1,Config.Compliance);
            S->mEdgeConstraints.emplace_back(I+1,I+N,Config.Compliance);
            S->mFaces.emplace_back(I,I+N,I+N+1);
            S->mFaces.emplace_back(I,I+N+1,I+1);
        }
        if(X+2<N) S->mEdgeConstraints.emplace_back(I,I+2,Config.BendCompliance);
        if(Y+2<N) S->mEdgeConstraints.emplace_back(I,I+2*N,Config.BendCompliance);
    }
    S->CalculateEdgeLengths(); S->Optimize(); return S;
}

Json Observe(const JPH::SoftBodyMotionProperties* M,float SubDt,bool Detailed) {
    double K=0, CandidateK=0, Elastic=0, Movable=0, PinSpeed=0, Stretch=0;
    JPH::Vec3 Momentum=JPH::Vec3::sZero();
    Json Particles=Json::array();
    for(const auto& P:M->GetVertices()) {
        if(P.mInvMass>0) {
            const double Mv=1.0/double(P.mInvMass);
            K+=0.5*Mv*P.mVelocity.LengthSq(); Movable+=Mv;
            Momentum+=float(Mv)*P.mVelocity;
            if(SubDt>0) CandidateK+=0.5*Mv*((P.mPosition-P.mPreviousPosition)/SubDt).LengthSq();
        } else PinSpeed=std::max(PinSpeed,double(P.mVelocity.Length()));
        if(Detailed) Particles.push_back({{"P",V(P.mPosition)},{"Prev",V(P.mPreviousPosition)},{"V",V(P.mVelocity)},
            {"InvMass",P.mInvMass},{"HasContact",P.mHasContact},{"CollisionShapeIndex",P.mCollidingShapeIndex}});
    }
    for(const auto& E:M->GetSettings()->mEdgeConstraints) {
        const float C=(M->GetVertex(E.mVertex[1]).mPosition-M->GetVertex(E.mVertex[0]).mPosition).Length()-E.mRestLength;
        Stretch=std::max(Stretch,std::abs(double(C/E.mRestLength)));
        if(E.mCompliance>0) Elastic+=double(C)*C/(2.0*E.mCompliance);
    }
    Json R={{"SoftKE",K},{"ReconstructedCandidateKE",CandidateK},{"SoftMomentum",V(Momentum)},
        {"MovableMass",Movable},{"MaxPinSpeed",PinSpeed},{"AllEdgeStrain",Stretch},
        {"CompliantEdgeElasticEstimate",Elastic},{"ElasticScope","sum C^2/(2*compliance) for positive-compliance edges; hard constraints excluded; not solver-internal stored energy"}};
    if(Detailed) R["Particles"]=std::move(Particles);
    return R;
}
}

// Called only by the isolated diagnostic Jolt patch. Normal upstream is unmodified.
extern "C" void PhysicsCouplingDiagnosticReconstruction(const void* Vertex,const void* Before,float DtValue) {
    if(!TraceFile.is_open()||CurrentFrame<Config.TraceStart||CurrentFrame>Config.TraceEnd) return;
    const auto& P=*static_cast<const JPH::SoftBodyVertex*>(Vertex);
    const auto& B=*static_cast<const JPH::Vec3*>(Before);
    Json R={{"Type","Reconstruction"},{"Frame",CurrentFrame},{"P",V(P.mPosition)},{"Prev",V(P.mPreviousPosition)},
        {"BeforeVelocity",V(B)},{"AfterVelocity",V(P.mVelocity)},{"InvMass",P.mInvMass},{"IntegrationDt",DtValue},
        {"KineticChange",0.5/double(P.mInvMass)*(P.mVelocity.LengthSq()-B.LengthSq())}};
    std::lock_guard Lock(TraceMutex); TraceFile<<R.dump()<<'\n';
}
extern "C" void PhysicsCouplingDiagnosticStage(const char* Phase,const void* Motion,const void* Context) {
    if(!TraceFile.is_open()||CurrentFrame<Config.TraceStart||CurrentFrame>Config.TraceEnd) return;
    const auto* M=static_cast<const JPH::SoftBodyMotionProperties*>(Motion);
    const auto* C=static_cast<const JPH::SoftBodyUpdateContext*>(Context);
    Json R=Observe(M,C->mSubStepDeltaTime,true);
    R["Type"]="Stage"; R["Phase"]=Phase; R["Frame"]=CurrentFrame;
    R["InternalIteration"]=C->mNextIteration.load(); R["CollisionStepDt"]=C->mDeltaTime; R["IntegrationDt"]=C->mSubStepDeltaTime;
    std::lock_guard Lock(TraceMutex); TraceFile<<R.dump()<<'\n';
}
extern "C" void PhysicsCouplingDiagnosticContact(const void* Vertex,const void* Impulse,const void* Normal,float Depth,
    float RigidInverseMass,const void* BeforeParticle,const void* BeforeLinear,const void* BeforeAngular,
    const void* AfterLinear,const void* AfterAngular) {
    if(!TraceFile.is_open()||CurrentFrame<Config.TraceStart||CurrentFrame>Config.TraceEnd) return;
    const auto& P=*static_cast<const JPH::SoftBodyVertex*>(Vertex);
    const auto Read=[](const void* A){return *static_cast<const JPH::Vec3*>(A);};
    Json R={{"Type","DynamicContact"},{"Frame",CurrentFrame},{"ParticlePosition",V(P.mPosition)},
        {"ParticleVelocityBeforeImpulse",V(Read(BeforeParticle))},{"ParticleVelocityAfterImpulse",V(P.mVelocity)},
        {"ImpulseToRigid",V(Read(Impulse))},{"Normal",V(Read(Normal))},{"ProjectionDepth",Depth},
        {"ParticleInverseMass",P.mInvMass},{"RigidInverseMass",RigidInverseMass},
        {"RigidVelocityBefore",V(Read(BeforeLinear))},{"RigidVelocityAfter",V(Read(AfterLinear))},
        {"RigidAngularBefore",V(Read(BeforeAngular))},{"RigidAngularAfter",V(Read(AfterAngular))}};
    std::lock_guard Lock(TraceMutex); TraceFile<<R.dump()<<'\n';
}

int main(int ArgCount,char** Args) {
    PhysicsEvaluation::SuppressErrorDialogs();
    try {
        Config=Parse(ArgCount,Args);
        PhysicsEvaluation::InstallPhysicsAllocators();
        if(!Config.Trace.empty()) { TraceFile.open(Config.Trace); if(!TraceFile) throw std::runtime_error("[Physics:Coupling] Trace open failed"); }
        const auto Before=PhysicsEvaluation::CaptureResources();
        JPH::Factory::sInstance=new JPH::Factory; JPH::RegisterTypes();
        Json Final;
        {
            Layers Layer; Pairs Pair; BroadPairs Broad; Contacts Contact;
            JPH::TempAllocatorImpl Temp(32*1024*1024);
            JPH::JobSystemThreadPool Jobs(JPH::cMaxPhysicsJobs,JPH::cMaxPhysicsBarriers,Config.Workers-1);
            JPH::PhysicsSystem World; World.Init(8192,0,16384,8192,Layer,Broad,Pair);
            World.SetGravity(JPH::Vec3::sZero()); World.SetSoftBodyContactListener(&Contact);
            auto Shared=GetCloth();
            JPH::SoftBodyCreationSettings Soft(Shared,JPH::RVec3::sZero(),JPH::Quat::sIdentity(),2);
            Soft.mNumIterations=Config.Iterations; Soft.mUpdatePosition=false; Soft.mAllowSleeping=false;
            Soft.mLinearDamping=Config.Damping; Soft.mVertexRadius=0.05f; Soft.mFacesDoubleSided=true;
            Soft.mFriction=Config.Friction; Soft.mRestitution=Config.Restitution;
            auto& Bodies=World.GetBodyInterface();
            const auto Cloth=Bodies.CreateAndAddSoftBody(Soft,JPH::EActivation::Activate);
            if(Cloth.IsInvalid()) throw std::runtime_error("[Physics:Coupling] Soft create failed");
            std::vector<JPH::BodyID> Rigids;
            for(int I=0;I<Config.Count;++I) {
                JPH::BodyCreationSettings R(new JPH::SphereShape(Radius),I?JPH::RVec3(1,-0.4,-Config.SphereZ):JPH::RVec3(0,0,Config.SphereZ),JPH::Quat::sIdentity(),Config.Dynamic?JPH::EMotionType::Dynamic:JPH::EMotionType::Static,1);
                R.mAllowSleeping=false; R.mLinearDamping=0; R.mAngularDamping=0;
                R.mFriction=Config.Friction; R.mRestitution=Config.Restitution;
                R.mOverrideMassProperties=JPH::EOverrideMassProperties::CalculateInertia; R.mMassPropertiesOverride.mMass=Mass;
                auto Id=Bodies.CreateAndAddBody(R,Config.Dynamic?JPH::EActivation::Activate:JPH::EActivation::DontActivate);
                if(Id.IsInvalid()) throw std::runtime_error("[Physics:Coupling] Rigid create failed");
                if(Config.Dynamic) Bodies.SetLinearVelocity(Id,JPH::Vec3(0,0,I?-Config.Speed:Config.Speed));
                Rigids.push_back(Id);
            }
            std::cout<<Json({{"Type","Config"},{"Backend","IndependentNativeJolt"},{"Jolt","e77f175595e64cb44218cc9d9d56fc365ad0e36a"},
                {"Frames",Config.Frames},{"Steps",Config.Steps},{"Iterations",Config.Iterations},{"Workers",Config.Workers},
                {"Compliance",Config.Compliance},{"BendCompliance",Config.BendCompliance},{"Damping",Config.Damping},
                {"Friction",Config.Friction},{"Restitution",Config.Restitution},{"Pinned",Config.Pinned},{"Contact",Config.Contact},
                {"Dynamic",Config.Dynamic},{"Seed",Config.Seed},{"RigidMass",Mass},{"SphereInertia",Inertia},
                {"RigidCount",Config.Count},{"InternalIntegrationDt",Dt/(Config.Steps*Config.Iterations)},
                {"DiagnosticTrace",Config.Trace},{"ZeroGravity",true}}).dump()<<'\n';
            double Maximum=0;
            for(CurrentFrame=0;CurrentFrame<Config.Frames;++CurrentFrame) {
                const auto Start=Clock::now();
                if(World.Update(Dt,Config.Steps,&Temp,&Jobs)!=JPH::EPhysicsUpdateError::None) throw std::runtime_error("[Physics:Coupling] Update failed");
                const double StepMs=Ms(Start);
                JPH::BodyLockRead Lock(World.GetBodyLockInterface(),Cloth);
                const auto* M=static_cast<const JPH::SoftBodyMotionProperties*>(Lock.GetBody().GetMotionProperties());
                Json Row=Observe(M,0,Config.Particles); double RigidK=0,AngularK=0,Penetration=0;
                Json States=Json::array();
                for(auto Id:Rigids) {
                    JPH::BodyLockRead RLock(World.GetBodyLockInterface(),Id);
                    const auto& R=RLock.GetBody(); const auto Velocity=R.GetLinearVelocity(), Angular=R.GetAngularVelocity();
                    if(Config.Dynamic) { RigidK+=0.5*Mass*Velocity.LengthSq(); AngularK+=0.5*Inertia*Angular.LengthSq(); }
                    States.push_back({{"P",V(R.GetPosition())},{"V",V(Velocity)},{"W",V(Angular)}});
                    for(const auto& P:M->GetVertices()) {
                        const auto Position=Lock.GetBody().GetCenterOfMassTransform()*P.mPosition;
                        Penetration=std::max(Penetration,std::max(0.0,double(Radius+0.05f-(Position-R.GetPosition()).Length())));
                    }
                }
                Row["Type"]="Frame"; Row["Frame"]=CurrentFrame; Row["StepMs"]=StepMs; Row["Contacts"]=Contact.Count.exchange(0);
                Row["RigidKE"]=RigidK; Row["RigidAngularKE"]=AngularK; Row["KineticEnergy"]=double(Row["SoftKE"])+RigidK+AngularK;
                Row["Rigids"]=std::move(States); Row["ParticlePenetration"]=Penetration;
                Maximum=std::max(Maximum,double(Row["KineticEnergy"])); std::cout<<Row.dump()<<'\n';
            }
            for(auto Id:Rigids) { Bodies.RemoveBody(Id); Bodies.DestroyBody(Id); }
            Bodies.RemoveBody(Cloth); Bodies.DestroyBody(Cloth);
            Final={{"MaximumKineticEnergy",Maximum},{"Type","Summary"}};
        }
        JPH::UnregisterTypes(); delete JPH::Factory::sInstance; JPH::Factory::sInstance=nullptr;
        Final["Resources"]=PhysicsEvaluation::ResourceDelta(Before,PhysicsEvaluation::CaptureResources());
        Final["TimingScope"]="Native Update only; diagnostic trace instrumentation is not performance evidence";
        std::cout<<Final.dump()<<'\n'; return 0;
    } catch(const std::exception& E) { std::cerr<<E.what()<<'\n'; return 2; }
}
