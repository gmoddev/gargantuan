#include "UnifiedJoltProvider.hpp"
#include "../ExperimentSupport.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    using namespace gargantuan;
    using Clock = std::chrono::steady_clock;
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
    constexpr bool Instrumented = true;
#else
    constexpr bool Instrumented = false;
#endif

    double Milliseconds(Clock::duration Duration) {
        return std::chrono::duration<double, std::milli>(Duration).count();
    }

    SoftBodyDefinition MakeCloth(std::uint32_t Resolution, std::uint32_t Index) {
        SoftBodyDefinition Definition;
        Definition.Kind = SoftBodyKind::Cloth;
        Definition.Position = {20.0f * Index, 3.0f, 0.0f};
        Definition.Size = {8.0f, 8.0f, 0.5f};
        Definition.ResolutionX = Resolution;
        Definition.ResolutionY = Resolution;
        for (std::uint32_t X = 0; X < Resolution; ++X) {
            const float Horizontal = float(X) / float(Resolution - 1) - 0.5f;
            Definition.Attachments.push_back({X, Definition.Position +
                glm::vec3(Definition.Size.x * Horizontal, Definition.Size.y * 0.5f, 0)});
        }
        return Definition;
    }

#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
    nlohmann::json Phase(const PhysicsEvaluation::JoltAdmissionPhase &Value) {
        return {{"Milliseconds", Value.Milliseconds},
            {"AllocationCalls", Value.AllocationCalls},
            {"AllocatedBytes", Value.AllocatedBytes}};
    }

    nlohmann::json Profile(const PhysicsEvaluation::JoltAdmissionProfile &Value) {
        const auto Known = [&](std::uint64_t Count) -> nlohmann::json {
            return Value.GroupStatsAvailable ? nlohmann::json(Count) : nlohmann::json(nullptr);
        };
        return {{"CacheHits", Value.CacheHits},
            {"UpdateGroups", Known(Value.UpdateGroups)},
            {"NonParallelEdges", Known(Value.NonParallelEdges)},
            {"NonParallelVolumes", Known(Value.NonParallelVolumes)},
            {"ParallelEdges", Known(Value.ParallelEdges)},
            {"ParallelVolumes", Known(Value.ParallelVolumes)},
            {"Topology", Phase(Value.Topology)},
            {"Precompute", Phase(Value.Precompute)},
            {"Optimize", Phase(Value.Optimize)},
            {"NativeCreate", Phase(Value.NativeCreate)},
            {"SlotCommit", Phase(Value.SlotCommit)}};
    }

    nlohmann::json WorldProfile(const PhysicsEvaluation::JoltWorldAdmissionProfile &Value) {
        return {{"TempArena", Phase(Value.TempArena)}, {"Runtime", Phase(Value.Runtime)},
            {"Jobs", Phase(Value.Jobs)}, {"WorldInit", Phase(Value.WorldInit)}};
    }
#endif

    struct BodySample {
        std::uint32_t Repeat = 0;
        std::uint32_t Body = 0;
        double CreateMs = 0;
        double ExtractMs = 0;
        double RetireMs = 0;
        PhysicsEvaluation::ResourceSnapshot BeforeCreate;
        PhysicsEvaluation::ResourceSnapshot AfterCreate;
        PhysicsEvaluation::ResourceSnapshot AfterExtract;
        PhysicsEvaluation::ResourceSnapshot AfterRetire;
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
        PhysicsEvaluation::JoltAdmissionProfile Admission;
#endif
    };
}

int main(int ArgumentCount, char **Arguments) {
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        if (ArgumentCount != 5 && ArgumentCount != 6)
            throw std::invalid_argument("Usage: AdmissionBenchmark jolt|xpbd cloth4k|cloth16k bodies[1..4] repeats[1..5] [workers[1..8]]");
        const std::string Backend = Arguments[1];
        const std::string Scene = Arguments[2];
        if (Backend != "jolt" && Backend != "xpbd")
            throw std::invalid_argument("Unknown backend");
        if (Scene != "cloth4k" && Scene != "cloth16k")
            throw std::invalid_argument("Unknown scene");
        const auto BodyCount = std::stoi(Arguments[3]);
        const auto Repeats = std::stoi(Arguments[4]);
        const auto Workers = ArgumentCount == 6 ? std::stoi(Arguments[5]) : 1;
        if (BodyCount < 1 || BodyCount > 4 || Repeats < 1 || Repeats > 5 ||
            Workers < 1 || Workers > 8)
            throw std::invalid_argument("Body, repeat or worker count out of bounds");

        const auto Resolution = Scene == "cloth4k" ? 64u : 128u;
        std::vector<SoftBodyDefinition> Definitions;
        Definitions.reserve(BodyCount);
        for (int Index = 0; Index < BodyCount; ++Index)
            Definitions.push_back(MakeCloth(Resolution, Index));

        PhysicsWorldConfig Config;
        Config.Gravity = {0, -9.81f, 0};
        SoftBodyWorldLimits Limits;
        Limits.MaximumWorkers = Workers;
        const auto BeforeWorld = PhysicsEvaluation::CaptureResources();
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
        PhysicsEvaluation::ResetJoltWorldAdmissionProfile();
#endif
        const auto WorldStart = Clock::now();
        PhysicsEvaluation::UnifiedJoltProvider Jolt;
        std::unique_ptr<ISoftBodyBackend> Xpbd;
        if (Backend == "jolt") Jolt = PhysicsEvaluation::CreateUnifiedJoltProvider(Config, Limits, Workers);
        else Xpbd = CreateSoftBodyBackend(Limits);
        auto *Soft = Backend == "jolt" ? Jolt.Soft.get() : Xpbd.get();
        if (!Soft || !Soft->IsValid()) throw std::runtime_error("World creation failed");
        const double WorldMs = Milliseconds(Clock::now() - WorldStart);
        const auto AfterWorld = PhysicsEvaluation::CaptureResources();
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
        const auto WorldAdmission = PhysicsEvaluation::GetJoltWorldAdmissionProfile();
#endif

        std::vector<BodySample> Samples;
        Samples.reserve(std::size_t(BodyCount) * Repeats);
        for (int Repeat = 0; Repeat < Repeats; ++Repeat) {
            std::vector<SoftBodyId> Ids;
            Ids.reserve(BodyCount);
            for (int Index = 0; Index < BodyCount; ++Index) {
                BodySample Sample;
                Sample.Repeat = Repeat;
                Sample.Body = Index;
                Sample.BeforeCreate = PhysicsEvaluation::CaptureResources();
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
                PhysicsEvaluation::ResetJoltAdmissionProfile();
#endif
                const auto CreateStart = Clock::now();
                const auto Id = Soft->CreateBody(Definitions[Index]);
                Sample.CreateMs = Milliseconds(Clock::now() - CreateStart);
                Sample.AfterCreate = PhysicsEvaluation::CaptureResources();
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
                Sample.Admission = PhysicsEvaluation::GetJoltAdmissionProfile();
#endif
                if (!Id.IsValid()) throw std::runtime_error("Body admission failed");
                const auto ExtractStart = Clock::now();
                {
                    const auto State = Soft->GetBodyState(Id);
                    Sample.ExtractMs = Milliseconds(Clock::now() - ExtractStart);
                    if (!State || !State->Positions || State->Positions->size() != Resolution * Resolution)
                        throw std::runtime_error("Initial state mismatch");
                }
                Sample.AfterExtract = PhysicsEvaluation::CaptureResources();
                Ids.push_back(Id);
                Samples.push_back(Sample);
            }
            for (int Index = BodyCount - 1; Index >= 0; --Index) {
                auto &Sample = Samples[std::size_t(Repeat) * BodyCount + Index];
                const auto RetireStart = Clock::now();
                if (!Soft->DestroyBody(Ids[Index]).Succeeded())
                    throw std::runtime_error("Body retirement failed");
                Sample.RetireMs = Milliseconds(Clock::now() - RetireStart);
                Sample.AfterRetire = PhysicsEvaluation::CaptureResources();
            }
        }
        const auto BeforeWorldRetire = PhysicsEvaluation::CaptureResources();
        const auto WorldRetireStart = Clock::now();
        Xpbd.reset();
        Jolt.Soft.reset();
        Jolt.Rigid.reset();
        const double WorldRetireMs = Milliseconds(Clock::now() - WorldRetireStart);
        const auto AfterWorldRetire = PhysicsEvaluation::CaptureResources();

        nlohmann::json Rows = nlohmann::json::array();
        for (const auto &Sample : Samples) {
            nlohmann::json Row = {{"Repeat", Sample.Repeat}, {"Body", Sample.Body},
                {"CreateMs", Sample.CreateMs}, {"ExtractMs", Sample.ExtractMs},
                {"RetireMs", Sample.RetireMs},
                {"CreateResources", PhysicsEvaluation::ResourceDelta(Sample.BeforeCreate, Sample.AfterCreate)},
                {"ExtractResources", PhysicsEvaluation::ResourceDelta(Sample.AfterCreate, Sample.AfterExtract)},
                {"RetireResources", PhysicsEvaluation::ResourceDelta(Sample.AfterExtract, Sample.AfterRetire)}};
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
            Row["Admission"] = Profile(Sample.Admission);
#endif
            Rows.push_back(std::move(Row));
        }
        nlohmann::json Result = {
            {"Schema", "PhysicsJoltAdmissionProfile1"}, {"Backend", Backend}, {"Scene", Scene},
            {"Bodies", BodyCount}, {"Repeats", Repeats}, {"Workers", Workers},
            {"Instrumented", Instrumented},
            {"WorldMs", WorldMs}, {"WorldResources", PhysicsEvaluation::ResourceDelta(BeforeWorld, AfterWorld)},
            {"WorldRetireMs", WorldRetireMs},
            {"WorldRetireResources", PhysicsEvaluation::ResourceDelta(BeforeWorldRetire, AfterWorldRetire)},
            {"WholeResources", PhysicsEvaluation::ResourceDelta(BeforeWorld, AfterWorldRetire)},
            {"Samples", Rows}
        };
#ifdef PHYSICS_EVALUATION_ADMISSION_PROFILE
        Result["WorldAdmission"] = WorldProfile(WorldAdmission);
#endif
        std::cout << "[Physics:Admission] " << Result.dump() << '\n';
        return 0;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:Admission] " << Error.what() << '\n';
        return 1;
    }
}
