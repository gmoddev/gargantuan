#include "UnifiedJoltProvider.hpp"
#include "../ExperimentSupport.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    using namespace gargantuan;
    using Clock = std::chrono::steady_clock;

    struct Options {
        bool Fast = true;
        int Frames = 120;
        int Workers = 1;
    };
    Options Parse(int Count, char **Arguments) {
        Options Result;
        for (int Index = 1; Index < Count; ++Index) {
            const std::string Key = Arguments[Index];
            if (Index + 1 == Count) throw std::runtime_error("missing benchmark argument");
            const std::string Value = Arguments[++Index];
            if (Key == "--mode") {
                if (Value != "fast" && Value != "full") throw std::runtime_error("invalid mode");
                Result.Fast = Value == "fast";
            } else if (Key == "--frames") Result.Frames = std::stoi(Value);
            else if (Key == "--workers") Result.Workers = std::stoi(Value);
            else throw std::runtime_error("unknown benchmark argument");
        }
        if (Result.Frames < 1 || Result.Frames > 3600 ||
            Result.Workers < 1 || Result.Workers > 8)
            throw std::runtime_error("invalid benchmark bounds");
        return Result;
    }
    PhysicsBodyDesc Sphere(glm::vec3 Position) {
        PhysicsBodyDesc Description;
        Description.Shape.Kind = PhysicsShapeKind::Ball;
        Description.Shape.Size = glm::vec3(0.9f);
        Description.Transform.Position = Position;
        Description.SleepEnabled = false;
        return Description;
    }
    double Milliseconds(Clock::time_point Start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - Start).count();
    }
}

int main(int Count, char **Arguments) {
    PhysicsEvaluation::SuppressErrorDialogs();
    PhysicsEvaluation::InstallPhysicsAllocators();
    try {
        const auto Config = Parse(Count, Arguments);
        PhysicsWorldConfig WorldConfig;
        WorldConfig.Gravity = {0, -9.81f, 0};
        WorldConfig.EnableSleep = false;
        auto Provider = PhysicsEvaluation::CreateUnifiedJoltProvider(WorldConfig, {}, Config.Workers);
        if (!Provider.Rigid || !Provider.Soft ||
            !PhysicsEvaluation::SetJoltRigidFastScheduleEnabled(*Provider.Rigid, Config.Fast))
            throw std::runtime_error("provider setup failed");
        PhysicsBodyDesc Floor;
        Floor.Anchored = true;
        Floor.Shape.Size = {64, 1, 64};
        Floor.Transform.Position = {0, -0.5f, 0};
        if (!Provider.Rigid->CreateBody(Floor).IsValid())
            throw std::runtime_error("floor admission failed");
        std::vector<PhysicsBodyId> Bodies;
        Bodies.reserve(128);
        for (int Index = 0; Index < 128; ++Index) {
            const auto Body = Provider.Rigid->CreateBody(Sphere({
                (Index % 8) * 0.91f, 0.55f + (Index / 8) * 0.91f, 0}));
            if (!Body.IsValid()) throw std::runtime_error("sphere admission failed");
            Bodies.push_back(Body);
        }
        double TotalUpdateMs = 0;
        double MaximumFloorPenetration = 0;
        bool FiniteEveryFrame = true;
        std::size_t ContactEvents = 0;
        std::vector<double> FrameMs;
        FrameMs.reserve(Config.Frames);
        for (int Frame = 0; Frame < Config.Frames; ++Frame) {
            const auto Start = Clock::now();
            const auto Result = Provider.Rigid->Step({
                .DeltaTime = SoftBodyStepInterval, .SubStepCount = 16});
            const double UpdateMs = Milliseconds(Start);
            TotalUpdateMs += UpdateMs;
            FrameMs.push_back(UpdateMs);
            ContactEvents += Result.Contacts.size();
            const auto Schedule = PhysicsEvaluation::GetJoltStepSchedule(*Provider.Rigid);
            if (!Schedule || Schedule->WorldUpdates != (Config.Fast ? 1u : 2u) ||
                Schedule->EnabledSoftBodies != 0)
                throw std::runtime_error("benchmark schedule mismatch");
            for (const auto Body : Bodies) {
                const auto State = Provider.Rigid->GetBodyState(Body);
                if (!State) throw std::runtime_error("rigid state missing");
                const auto Position = State->Transform.Position;
                FiniteEveryFrame &= std::isfinite(Position.x) && std::isfinite(Position.y) &&
                    std::isfinite(Position.z) && std::isfinite(State->LinearVelocity.x) &&
                    std::isfinite(State->LinearVelocity.y) && std::isfinite(State->LinearVelocity.z);
                MaximumFloorPenetration = std::max(MaximumFloorPenetration,
                    std::max(0.0, 0.45 - double(Position.y)));
            }
        }
        std::ranges::sort(FrameMs);
        const auto P95 = FrameMs[std::min(FrameMs.size() - 1,
            std::size_t(std::ceil(0.95 * FrameMs.size())) - 1)];
        const bool QualityPass = FiniteEveryFrame && MaximumFloorPenetration <= 0.25;
        std::cout << std::setprecision(17) << "{\"Mode\":\""
            << (Config.Fast ? "fast" : "full") << "\",\"Frames\":" << Config.Frames
            << ",\"Workers\":" << Config.Workers
            << ",\"TotalUpdateMs\":" << TotalUpdateMs
            << ",\"UpdateP95Ms\":" << P95
            << ",\"ContactEvents\":" << ContactEvents
            << ",\"MaximumFloorPenetration\":" << MaximumFloorPenetration
            << ",\"FiniteEveryFrame\":" << (FiniteEveryFrame ? "true" : "false")
            << ",\"QualityPass\":" << (QualityPass ? "true" : "false") << "}\n";
        return QualityPass ? 0 : 3;
    } catch (const std::exception &Error) {
        std::cerr << "[Physics:JoltScheduling] " << Error.what() << '\n';
        return 2;
    }
}
