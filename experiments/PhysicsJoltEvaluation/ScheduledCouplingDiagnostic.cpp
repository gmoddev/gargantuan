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
#include "ExperimentalVelocityReconstruction.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
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
    int TraceStart = 16, TraceEnd = 24, ProjectionPasses = 1, ContactThrough = 3600;
    int WorldUpdates = 1;
    int VelocityReconstruction = 0;
    bool ObserveUpdates = false;
    bool Contact = true, Pinned = true, Dynamic = true, Particles = true;
    float Compliance = 0.0f, BendCompliance = 1.0e-4f, Damping = 2.4f, Friction = 0.2f;
    float Restitution = 0.0f, Seed = 0.0f, SeedX = 0.0f, SeedY = 0.0f, SphereZ = -2.0f, Speed = 5.0f;
    bool RefreshSpherePlane = false;
    float ContactProjectionScale = 1.0f;
    int ProjectionFrame = -1, ProjectionIteration = -1, ProjectionVertex = -1;
    std::string Trace, ExportGraph, VelocityFile, DisplacementFile, Fixture = "Cloth", SolverObservations;
};
Options Config;
std::ofstream TraceFile;
std::ofstream SolverObservationFile;
std::mutex TraceMutex;
std::mutex SolverObservationMutex;
std::atomic<bool> SolverObservationWriteFailed{false};
int CurrentFrame = -1;
int CurrentWorldUpdate = -1;
int CurrentSolverOrdinal = 0;
#ifdef PHYSICS_ENERGY_OBSERVER
std::function<void(const JPH::SoftBodyMotionProperties *, const JPH::SoftBodyUpdateContext *, double)> EnergyObserver;
#endif
template<class T> Json V(T A) { return Json::array({A.GetX(),A.GetY(),A.GetZ()}); }
double Ms(Clock::time_point Start) { return std::chrono::duration<double,std::milli>(Clock::now()-Start).count(); }

Options Parse(int Count, char** Args) {
    Options O;
    for (int I=1; I<Count; I+=2) {
        if (I+1 == Count) throw std::runtime_error("[Physics:Coupling] Missing option");
        const std::string K=Args[I], S=Args[I+1];
        if(K=="--frames") O.Frames=std::stoi(S);
        else if(K=="--world-updates") O.WorldUpdates=std::stoi(S);
        else if(K=="--velocity-reconstruction") O.VelocityReconstruction=PhysicsJoltEvaluation::ParseVelocityReconstruction(S);
        else if(K=="--fixture") O.Fixture=S;
        else if(K=="--observe-updates") O.ObserveUpdates=std::stoi(S)!=0;
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
        else if(K=="--seed-x") O.SeedX=std::stof(S);
        else if(K=="--seed-y") O.SeedY=std::stof(S);
        else if(K=="--export-graph") O.ExportGraph=S;
        else if(K=="--velocity-file") O.VelocityFile=S;
        else if(K=="--displacement-file") O.DisplacementFile=S;
        else if(K=="--projection-passes") O.ProjectionPasses=std::stoi(S);
        else if(K=="--refresh-sphere-plane") O.RefreshSpherePlane=std::stoi(S)!=0;
        else if(K=="--contact-projection-scale") O.ContactProjectionScale=std::stof(S);
        else if(K=="--contact-through") O.ContactThrough=std::stoi(S);
        else if(K=="--projection-frame") O.ProjectionFrame=std::stoi(S);
        else if(K=="--projection-iteration") O.ProjectionIteration=std::stoi(S);
        else if(K=="--projection-vertex") O.ProjectionVertex=std::stoi(S);
        else if(K=="--sphere-z") O.SphereZ=std::stof(S);
        else if(K=="--speed") O.Speed=std::stof(S);
        else if(K=="--trace") O.Trace=S;
        else if(K=="--solver-observations") O.SolverObservations=S;
        else if(K=="--trace-start") O.TraceStart=std::stoi(S);
        else if(K=="--trace-end") O.TraceEnd=std::stoi(S);
        else throw std::runtime_error("[Physics:Coupling] Unknown option "+K);
    }
    if(O.Frames<1||O.Frames>3600||O.Steps<1||O.Steps>96||O.WorldUpdates<1||O.WorldUpdates>384||O.Steps*O.WorldUpdates*O.Iterations>2304||O.Iterations<1||O.Iterations>64||O.Workers<1||O.Workers>8||O.Count<1||O.Count>2||O.ProjectionPasses<1||O.ProjectionPasses>8||O.ContactThrough< -1||O.ContactThrough>3600||!std::isfinite(O.ContactProjectionScale)||O.ContactProjectionScale<0||O.ContactProjectionScale>1||O.Compliance<0||O.BendCompliance<0||O.Damping<0||O.Friction<0||O.Restitution<0)
        throw std::runtime_error("[Physics:Coupling] Option outside bounded range");
#ifndef PHYSICS_ENERGY_OBSERVER
    if(!O.SolverObservations.empty())
        throw std::runtime_error("[Physics:Coupling] Solver observations require the isolated energy observer build");
#endif
    if(O.ProjectionFrame< -1||O.ProjectionFrame>299||O.ProjectionIteration< -1||O.ProjectionIteration>64||O.ProjectionVertex< -1||O.ProjectionVertex>168)
        throw std::runtime_error("[Physics:Coupling] Projection selector outside bounded range");
    if((O.Fixture!="Cloth"&&O.Fixture!="Spring")||(O.Fixture=="Spring"&&(O.Compliance<=0||O.Contact||O.Speed!=0)))
        throw std::runtime_error("[Physics:Coupling] Spring calibration requires positive compliance, no contact, zero rigid speed");
#ifndef PHYSICS_S1_DIAGNOSTIC
    if(O.ProjectionPasses!=1||O.RefreshSpherePlane||O.ContactProjectionScale!=1)
        throw std::runtime_error("[Physics:Coupling] Counterfactual requires isolated S1 diagnostic Jolt");
#endif
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
        if((!Config.Contact||CurrentFrame>Config.ContactThrough)&&((A==1&&B==2)||(A==2&&B==1))) return false;
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
    if(Config.Fixture=="Spring") {
        JPH::SoftBodySharedSettings::Vertex Pin,Moving;
        Pin.mPosition=JPH::Float3(0,0,0); Pin.mInvMass=0;
        Moving.mPosition=JPH::Float3(1.1f,0,0); Moving.mInvMass=10;
        Moving.mVelocity=JPH::Float3(Config.SeedX,0,0);
        S->mVertices.push_back(Pin); S->mVertices.push_back(Moving);
        S->mEdgeConstraints.emplace_back(0,1,Config.Compliance);
        S->mEdgeConstraints.back().mRestLength=1.0f;
        S->Optimize(); return S;
    }
    constexpr int N=13;
    for(int Y=0;Y<N;++Y) for(int X=0;X<N;++X) {
        JPH::SoftBodySharedSettings::Vertex P;
        P.mPosition=JPH::Float3((float(X)/(N-1)-0.5f)*4.0f,-(float(Y)/(N-1)-0.5f)*4.0f,0);
        P.mInvMass=Config.Pinned&&Y==0?0.0f:10.0f;
        if(X==N/2&&Y==N/2) P.mVelocity=JPH::Float3(Config.SeedX,Config.SeedY,Config.Seed);
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
    S->CalculateEdgeLengths();
    const auto Load=[&](const std::string& Path,bool Velocity) {
        if(Path.empty()) return;
        std::ifstream Input(Path); Json Values;
        if(!Input || !(Input>>Values) || !Values.is_array() || Values.size()!=S->mVertices.size())
            throw std::runtime_error("[Physics:Coupling] Invalid perturbation file");
        for(size_t I=0;I<Values.size();++I) {
            if(!Values[I].is_array()||Values[I].size()!=3) throw std::runtime_error("[Physics:Coupling] Invalid perturbation vector");
            JPH::Vec3 Value(Values[I][0].get<float>(),Values[I][1].get<float>(),Values[I][2].get<float>());
            if(!std::isfinite(Value.GetX())||!std::isfinite(Value.GetY())||!std::isfinite(Value.GetZ())) throw std::runtime_error("[Physics:Coupling] Nonfinite perturbation");
            if(S->mVertices[I].mInvMass==0 && Value.LengthSq()!=0) throw std::runtime_error("[Physics:Coupling] Perturbation moves pin");
            if(Velocity) Value.StoreFloat3(&S->mVertices[I].mVelocity);
            else (JPH::Vec3(S->mVertices[I].mPosition)+Value).StoreFloat3(&S->mVertices[I].mPosition);
        }
    };
    S->Optimize();
    // A perturbation changes state while preserving the unperturbed edge order.
    Load(Config.VelocityFile,true); Load(Config.DisplacementFile,false);
    if(!Config.ExportGraph.empty()) {
        Json Graph={{"Positions",Json::array()},{"InverseMasses",Json::array()},{"Edges",Json::array()},
            {"Order","Native optimized projection order; rest lengths calculated before perturbations"}};
        for(const auto& P:S->mVertices) { Graph["Positions"].push_back(V(JPH::Vec3(P.mPosition))); Graph["InverseMasses"].push_back(P.mInvMass); }
        for(const auto& E:S->mEdgeConstraints) Graph["Edges"].push_back({{"I",E.mVertex[0]},{"J",E.mVertex[1]},{"RestLength",E.mRestLength},{"Compliance",E.mCompliance}});
        std::ofstream Output(Config.ExportGraph);
        if(!Output || !(Output<<Graph.dump(2)<<'\n')) throw std::runtime_error("[Physics:Coupling] Graph export failed");
    }
    return S;
}

Json Observe(const JPH::SoftBodyMotionProperties* M,float SubDt,bool Detailed) {
    double K=0, CandidateK=0, Elastic=0, Movable=0, PinSpeed=0, Stretch=0, Structural=0, PinDrift=0;
    bool Finite=true;
    JPH::Vec3 Momentum=JPH::Vec3::sZero();
    Json Particles=Json::array();
    for(const auto& P:M->GetVertices()) {
        Finite &= std::isfinite(P.mPosition.GetX())&&std::isfinite(P.mPosition.GetY())&&std::isfinite(P.mPosition.GetZ())
            &&std::isfinite(P.mVelocity.GetX())&&std::isfinite(P.mVelocity.GetY())&&std::isfinite(P.mVelocity.GetZ());
        if(P.mInvMass==0) {
            const auto Index=&P-M->GetVertices().data();
            PinDrift=std::max(PinDrift,double((P.mPosition-JPH::Vec3(M->GetSettings()->mVertices[Index].mPosition)).Length()));
        }
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
    const auto Edge=[&](int A,int B,float Rest) {
        Structural=std::max(Structural,std::abs(double((M->GetVertex(B).mPosition-M->GetVertex(A).mPosition).Length()/Rest)-1.0));
    };
    for(int Y=0;M->GetVertices().size()==169&&Y<13;++Y) for(int X=0;X<13;++X) {
        const int I=Y*13+X;
        if(X+1<13) Edge(I,I+1,4.0f/12);
        if(Y+1<13) Edge(I,I+13,4.0f/12);
        if(X+1<13&&Y+1<13) { Edge(I,I+14,(4.0f/12)*std::sqrt(2.0f)); Edge(I+1,I+13,(4.0f/12)*std::sqrt(2.0f)); }
    }
    Json R={{"SoftKE",K},{"ReconstructedCandidateKE",CandidateK},{"SoftMomentum",V(Momentum)},
        {"MovableMass",Movable},{"MaxPinSpeed",PinSpeed},{"AllEdgeStrain",Stretch},
        {"StructuralStrain",Structural},{"MaxPinDrift",PinDrift},{"Finite",Finite},
        {"CompliantEdgeElasticEstimate",Elastic},{"ElasticScope","sum C^2/(2*compliance) for positive-compliance edges; hard constraints excluded; not solver-internal stored energy"}};
    if(Detailed) R["Particles"]=std::move(Particles);
    if(SubDt<=0) R["ReconstructedCandidateKE"]=nullptr;
    return R;
}
}

#ifdef PHYSICS_ENERGY_OBSERVER
// The isolated Jolt observer calls after soft velocity/contact updates and before
// pending rigid impulse velocities are written to bodies. No solver state is changed.
extern "C" void PhysicsJoltEnergyState(const void *Motion, const void *Context, double PendingRigidKineticDelta) {
    if(EnergyObserver) EnergyObserver(
        static_cast<const JPH::SoftBodyMotionProperties *>(Motion),
        static_cast<const JPH::SoftBodyUpdateContext *>(Context), PendingRigidKineticDelta);
}
#endif

// Called only by the isolated diagnostic Jolt patch. Normal upstream is unmodified.
extern "C" int PhysicsS1DiagnosticProjectionPasses() { return Config.ProjectionPasses; }
extern "C" bool PhysicsS1DiagnosticRefreshSpherePlane() { return Config.RefreshSpherePlane; }
extern "C" float PhysicsS1DiagnosticContactProjectionScale(const void* Motion,const void* Context,const void* Vertex) {
    const auto* M=static_cast<const JPH::SoftBodyMotionProperties*>(Motion);
    const auto* C=static_cast<const JPH::SoftBodyUpdateContext*>(Context);
    const auto* P=static_cast<const JPH::SoftBodyVertex*>(Vertex);
    if((Config.ProjectionFrame>=0&&CurrentFrame!=Config.ProjectionFrame)
        ||(Config.ProjectionIteration>=0&&int(C->mNextIteration.load())!=Config.ProjectionIteration)
        ||(Config.ProjectionVertex>=0&&P-M->GetVertices().data()!=Config.ProjectionVertex)) return 1.0f;
    return Config.ContactProjectionScale;
}
extern "C" void PhysicsS1DiagnosticProjection(const char* Phase,const void* Motion,const void* Vertex,const void* Center,const void* Normal,float Depth) {
    if(!TraceFile.is_open()||CurrentFrame<Config.TraceStart||CurrentFrame>Config.TraceEnd) return;
    const auto* M=static_cast<const JPH::SoftBodyMotionProperties*>(Motion);
    const auto& P=*static_cast<const JPH::SoftBodyVertex*>(Vertex);
    const auto& C=*static_cast<const JPH::Vec3*>(Center);
    Json R=Observe(M,0,false);
    R["Type"]="ContactProjection"; R["Phase"]=Phase; R["Frame"]=CurrentFrame;
    R["Vertex"]=&P-M->GetVertices().data(); R["P"]=V(P.mPosition); R["RigidFrozenCenter"]=V(C);
    R["Normal"]=V(*static_cast<const JPH::Vec3*>(Normal)); R["PlaneDepth"]=Depth;
    R["TrueFrozenSphereGap"]=(P.mPosition-C).Length()-(Radius+0.05f);
    R["CollisionShapeIndex"]=P.mCollidingShapeIndex;
    std::lock_guard Lock(TraceMutex); TraceFile<<R.dump()<<'\n';
}
extern "C" void PhysicsCouplingDiagnosticReconstruction(const void* Motion,const void* Vertex,const void* Before,float DtValue) {
    if(!TraceFile.is_open()||CurrentFrame<Config.TraceStart||CurrentFrame>Config.TraceEnd) return;
    const auto& P=*static_cast<const JPH::SoftBodyVertex*>(Vertex);
    const auto* M=static_cast<const JPH::SoftBodyMotionProperties*>(Motion);
    const auto& B=*static_cast<const JPH::Vec3*>(Before);
    Json R={{"Type","Reconstruction"},{"Frame",CurrentFrame},{"Vertex",&P-M->GetVertices().data()},{"P",V(P.mPosition)},{"Prev",V(P.mPreviousPosition)},
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
        PhysicsJoltEvaluation::ConfigureVelocityReconstruction(Config.VelocityReconstruction);
        PhysicsEvaluation::InstallPhysicsAllocators();
        if(!Config.Trace.empty()) { TraceFile.open(Config.Trace); if(!TraceFile) throw std::runtime_error("[Physics:Coupling] Trace open failed"); }
#ifdef PHYSICS_ENERGY_OBSERVER
        if(!Config.SolverObservations.empty()) {
            SolverObservationFile.open(Config.SolverObservations);
            if(!SolverObservationFile) throw std::runtime_error("[Physics:Coupling] Solver observation file open failed");
        }
#endif
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
            PhysicsJoltEvaluation::ValidateVelocityConstraints(*Shared, Config.VelocityReconstruction);
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
            const auto GetObservation=[&]() {
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
                Row["RigidKE"]=RigidK; Row["RigidAngularKE"]=AngularK; Row["KineticEnergy"]=double(Row["SoftKE"])+RigidK+AngularK;
                Row["Rigids"]=std::move(States); Row["ParticlePenetration"]=Penetration;
                return Row;
            };
#ifdef PHYSICS_ENERGY_OBSERVER
            if(SolverObservationFile.is_open()) {
                // BodyInterfaceNoLock is used only within Jolt's soft update callback.
                // At this point rigid bodies still hold their pre-writeback velocities;
                // the hook supplies the pending kinetic delta from collision shapes.
                EnergyObserver=[&](const JPH::SoftBodyMotionProperties *Motion,
                                   const JPH::SoftBodyUpdateContext *Context,
                                   double PendingRigidKineticDelta) {
                    std::lock_guard Guard(SolverObservationMutex);
                    const int Ordinal=CurrentSolverOrdinal++;
                    Json Row=Observe(Motion,0,false);
                    const auto& NoLock=World.GetBodyInterfaceNoLock();
                    const auto SoftOrigin=NoLock.GetPosition(Cloth);
                    const JPH::Vec3 SoftOriginFloat(float(SoftOrigin.GetX()),float(SoftOrigin.GetY()),float(SoftOrigin.GetZ()));
                    double RigidLinearK=0, RigidAngularK=0, Penetration=0;
                    bool Finite=Row["Finite"].get<bool>() && std::isfinite(PendingRigidKineticDelta);
                    Json BaselineRigids=Json::array();
                    for(auto Id:Rigids) {
                        const auto Position=NoLock.GetPosition(Id);
                        const auto Linear=NoLock.GetLinearVelocity(Id);
                        const auto Angular=NoLock.GetAngularVelocity(Id);
                        if(Config.Dynamic) {
                            RigidLinearK+=0.5*Mass*Linear.LengthSq();
                            RigidAngularK+=0.5*Inertia*Angular.LengthSq();
                        }
                        Finite &= std::isfinite(Position.GetX()) && std::isfinite(Position.GetY()) &&
                            std::isfinite(Position.GetZ()) && std::isfinite(Linear.GetX()) &&
                            std::isfinite(Linear.GetY()) && std::isfinite(Linear.GetZ()) &&
                            std::isfinite(Angular.GetX()) && std::isfinite(Angular.GetY()) &&
                            std::isfinite(Angular.GetZ());
                        BaselineRigids.push_back({{"P",V(Position)},{"V",V(Linear)},{"W",V(Angular)}});
                        const JPH::Vec3 Center(float(Position.GetX()),float(Position.GetY()),float(Position.GetZ()));
                        for(const auto& Particle:Motion->GetVertices())
                            Penetration=std::max(Penetration,std::max(0.0,double(Radius+0.05f-
                                (Particle.mPosition+SoftOriginFloat-Center).Length())));
                    }
                    const double SoftK=Row["SoftKE"].get<double>();
                    const double TotalK=SoftK+RigidLinearK+RigidAngularK+PendingRigidKineticDelta;
                    const double EdgeU=Row["CompliantEdgeElasticEstimate"].get<double>();
                    Finite &= std::isfinite(TotalK) && std::isfinite(EdgeU) &&
                        std::isfinite(Penetration) && TotalK>=0.0;
                    Row["Type"]="SolverMicrostep";
                    Row["Frame"]=CurrentFrame;
                    Row["WorldUpdate"]=CurrentWorldUpdate;
                    Row["SolverOrdinal"]=Ordinal;
                    Row["CollisionStep"]=Ordinal/Config.Iterations;
                    Row["InternalIteration"]=Ordinal%Config.Iterations+1;
                    Row["ContextInternalIteration"]=Context->mNextIteration.load();
                    Row["InternalIntegrationDt"]=Context->mSubStepDeltaTime;
                    Row["RigidBaselineLinearKE"]=RigidLinearK;
                    Row["RigidBaselineAngularKE"]=RigidAngularK;
                    Row["PendingRigidKineticDelta"]=PendingRigidKineticDelta;
                    Row["RigidBaselineStates"]=std::move(BaselineRigids);
                    Row["RigidVelocityScope"]="Body velocities before pending soft contact impulse writeback";
                    Row["KineticEnergy"]=TotalK;
                    Row["KPlusEdgeModelEnergy"]=TotalK+EdgeU;
                    Row["ParticlePenetration"]=Penetration;
                    Row["Finite"]=Finite;
                    SolverObservationFile<<Row.dump()<<'\n';
                    if(!SolverObservationFile) SolverObservationWriteFailed.store(true);
                };
            }
#endif
            const auto Initial=GetObservation();
            const bool PositiveCompliance=std::all_of(Shared->mEdgeConstraints.begin(),Shared->mEdgeConstraints.end(),[](const auto& E){return E.mCompliance>0;});
            std::cout<<Json({{"Type","Config"},{"Backend","IndependentNativeJolt"},{"Jolt","e77f175595e64cb44218cc9d9d56fc365ad0e36a"},
                {"Frames",Config.Frames},{"Steps",Config.Steps},{"Iterations",Config.Iterations},{"Workers",Config.Workers},
                {"Compliance",Config.Compliance},{"BendCompliance",Config.BendCompliance},{"Damping",Config.Damping},
                {"Friction",Config.Friction},{"Restitution",Config.Restitution},{"Pinned",Config.Pinned},{"Contact",Config.Contact},
                {"Dynamic",Config.Dynamic},{"Seed",Config.Seed},{"RigidMass",Mass},{"SphereInertia",Inertia},
                {"SeedX",Config.SeedX},{"SeedY",Config.SeedY},{"ProjectionPasses",Config.ProjectionPasses},
                {"RefreshSpherePlane",Config.RefreshSpherePlane},{"VelocityFile",Config.VelocityFile},{"DisplacementFile",Config.DisplacementFile},
                {"ContactProjectionScale",Config.ContactProjectionScale},{"ContactThrough",Config.ContactThrough},
                {"ProjectionFrame",Config.ProjectionFrame},{"ProjectionIteration",Config.ProjectionIteration},{"ProjectionVertex",Config.ProjectionVertex},
                {"RigidCount",Config.Count},{"InternalIntegrationDt",((Dt/Config.WorldUpdates)/Config.Steps)/Config.Iterations},
                {"WorldUpdatesPerFrame",Config.WorldUpdates},{"ObserveUpdates",Config.ObserveUpdates},{"Speed",Config.Speed},
                {"VelocityReconstruction",PhysicsJoltEvaluation::VelocityReconstructionName(Config.VelocityReconstruction)},
                {"NumericalReconstructionPatch",PhysicsJoltEvaluation::NumericalReconstructionPatch},
                {"InitialKineticEnergy",Initial["KineticEnergy"]},{"InitialCompliantEdgeElasticEstimate",Initial["CompliantEdgeElasticEstimate"]},
                {"AllEdgesPositiveCompliance",PositiveCompliance},
                {"Fixture",Config.Fixture},{"SolverObservations",Config.SolverObservations},
                {"SolverObservationScope",Config.SolverObservations.empty() ? "None" :
                    "After each soft contact velocity update, before rigid impulse writeback; body baseline K plus pending collision-shape kinetic delta"},
                {"DiagnosticTrace",Config.Trace},{"ZeroGravity",true}}).dump()<<'\n';
            double Maximum=0,MaximumAccepted=0;
            for(CurrentFrame=0;CurrentFrame<Config.Frames;++CurrentFrame) {
                double StepMs=0;
                for(int Update=0;Update<Config.WorldUpdates;++Update) {
                    CurrentWorldUpdate=Update;
                    CurrentSolverOrdinal=0;
                    const auto Start=Clock::now();
                    if(World.Update(Dt/Config.WorldUpdates,Config.Steps,&Temp,&Jobs)!=JPH::EPhysicsUpdateError::None) throw std::runtime_error("[Physics:Coupling] Update failed");
                    StepMs+=Ms(Start);
#ifdef PHYSICS_ENERGY_OBSERVER
                    if(SolverObservationFile.is_open() && CurrentSolverOrdinal!=Config.Steps*Config.Iterations)
                        throw std::runtime_error("[Physics:Coupling] Solver observation coverage mismatch");
                    if(SolverObservationWriteFailed.load())
                        throw std::runtime_error("[Physics:Coupling] Solver observation write failed");
#endif
                    if(Config.ObserveUpdates) {
                        auto Micro=GetObservation(); Micro["Type"]="Microstep"; Micro["Frame"]=CurrentFrame; Micro["Microstep"]=Update;
                        Micro["ContactCountSoFar"]=Contact.Count.load();
                        MaximumAccepted=std::max(MaximumAccepted,double(Micro["KineticEnergy"])); std::cout<<Micro.dump()<<'\n';
                    }
                }
                auto Row=GetObservation();
                Row["Type"]="Frame"; Row["Frame"]=CurrentFrame; Row["StepMs"]=StepMs; Row["Contacts"]=Contact.Count.exchange(0);
                Maximum=std::max(Maximum,double(Row["KineticEnergy"])); std::cout<<Row.dump()<<'\n';
            }
#ifdef PHYSICS_ENERGY_OBSERVER
            EnergyObserver={};
#endif
            for(auto Id:Rigids) { Bodies.RemoveBody(Id); Bodies.DestroyBody(Id); }
            Bodies.RemoveBody(Cloth); Bodies.DestroyBody(Cloth);
            Final={{"MaximumKineticEnergy",Maximum},{"MaximumAcceptedUpdateKineticEnergy",MaximumAccepted},{"Type","Summary"}};
        }
        JPH::UnregisterTypes(); delete JPH::Factory::sInstance; JPH::Factory::sInstance=nullptr;
        Final["Resources"]=PhysicsEvaluation::ResourceDelta(Before,PhysicsEvaluation::CaptureResources());
        Final["TimingScope"]=Config.SolverObservations.empty() ?
            "Native Update only; separately observed updates are not performance evidence" :
            "Native Update includes per-solver-step JSON observation; not performance evidence";
        std::cout<<Final.dump()<<'\n'; return 0;
    } catch(const std::exception& E) { std::cerr<<E.what()<<'\n'; return 2; }
}
