#include "SoftFixtures.hpp"
#include "ExperimentSupport.hpp"
#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace {
    using Json = nlohmann::json;
    constexpr float DeltaTime = 1.0f / 60.0f;

    struct Options {
        std::string Backend = "Jolt";
        std::string Case = "TinyCloth";
        int Workers = 1;
        int Frames = 150;
        int Warmup = 30;
        float Gravity = 196.2f;
        std::string Track = "Historical";
        int Iterations = 0;
    };

    Options Parse(int ArgumentCount, char **Arguments) {
        Options Result;
        for (int Index = 1; Index < ArgumentCount; Index += 2) {
            if (Index + 1 == ArgumentCount) throw std::runtime_error("[Physics:SoftEvaluation] Missing option value");
            const std::string_view Name = Arguments[Index];
            const std::string Value = Arguments[Index + 1];
            if (Name == "--backend") Result.Backend = Value;
            else if (Name == "--case") Result.Case = Value;
            else if (Name == "--workers") Result.Workers = std::stoi(Value);
            else if (Name == "--frames") Result.Frames = std::stoi(Value);
            else if (Name == "--warmup") Result.Warmup = std::stoi(Value);
            else if (Name == "--gravity") Result.Gravity = std::stof(Value);
            else if (Name == "--track") Result.Track = Value;
            else if (Name == "--iterations") Result.Iterations = std::stoi(Value);
            else throw std::runtime_error("[Physics:SoftEvaluation] Unknown option");
        }
        if (Result.Backend != "XPBD" && Result.Backend != "Jolt")
            throw std::runtime_error("[Physics:SoftEvaluation] Invalid backend");
        if (Result.Case != "TinyCloth" && Result.Case != "Cloth4K" && Result.Case != "Cloth16K" &&
            Result.Case != "FourCloths" && Result.Case != "Rubber" && Result.Case != "MovingCollider" &&
            Result.Case != "HighSpeedCollider" && Result.Case != "Attachments" &&
            Result.Case != "PointImpulse" && Result.Case != "Lifecycle")
            throw std::runtime_error("[Physics:SoftEvaluation] Invalid case");
        if (Result.Workers < 1 || Result.Workers > 8 || Result.Frames < 1 || Result.Frames > 600 ||
            Result.Warmup < 0 || Result.Warmup >= Result.Frames || !std::isfinite(Result.Gravity) ||
            Result.Gravity < 0.0f || Result.Gravity > 196.2f ||
            (Result.Track != "Historical" && Result.Track != "FixedTotalMass") ||
            Result.Iterations < 0 || Result.Iterations > 64)
            throw std::runtime_error("[Physics:SoftEvaluation] Option outside bounded range");
        return Result;
    }

    int Resolution(const Options &Config) {
        if (Config.Case == "TinyCloth") return 8;
        if (Config.Case == "Cloth16K") return 128;
        if (Config.Case == "Rubber") return 8;
        return 64;
    }

    bool IsCloth(const Options &Config) { return Config.Case != "Rubber"; }
    int BodyCount(const Options &Config) { return Config.Case == "FourCloths" ? 4 : 1; }
    bool HasMovingCollider(const Options &Config) {
        return Config.Case == "MovingCollider" || Config.Case == "HighSpeedCollider";
    }
    float ColliderZ(const Options &Config, int Frame) {
        return -2.0f + (Config.Case == "HighSpeedCollider" ? 0.8f : 0.06f) * Frame;
    }

    float ParticleMass(const Options &Config) {
        if (Config.Track == "Historical" || Config.Case == "Rubber") return 0.1f;
        const auto N = Resolution(Config);
        return (64.0f * 64.0f - 64.0f) * 0.1f / static_cast<float>(N * N - N);
    }

    int Iterations(const Options &Config) {
        if (Config.Iterations > 0) return Config.Iterations;
        if (Config.Case == "Rubber") return 16;
        return Resolution(Config) <= 64 ? 8 : 4;
    }

    struct Quality {
        bool Finite = true;
        double MaxEdgeStretch = 0.0;
        double MaxPinDrift = 0.0;
        double VolumeError = 0.0;
        double MaxPenetration = 0.0;
        std::size_t InvertedTetrahedra = 0;
        double MinY = 1.0e30;
        double MaxY = -1.0e30;
        std::size_t VertexCount = 0;
    };

    template <class PositionAt> double ClothGridStrain(int Resolution, float Size, PositionAt GetPosition) {
        const float Unit = Size / static_cast<float>(Resolution - 1);
        double Maximum = 0.0;
        const auto Check = [&](int A, int B, float Rest) {
            Maximum = std::max(Maximum,
                std::abs(static_cast<double>(GetPosition(A, B) / Rest) - 1.0));
        };
        for (int Y = 0; Y < Resolution; ++Y) for (int X = 0; X < Resolution; ++X) {
            const int I = Y * Resolution + X;
            if (X + 1 < Resolution) Check(I, I + 1, Unit);
            if (Y + 1 < Resolution) Check(I, I + Resolution, Unit);
            if (X + 1 < Resolution && Y + 1 < Resolution) {
                Check(I, I + Resolution + 1, Unit * std::sqrt(2.0f));
                Check(I + 1, I + Resolution, Unit * std::sqrt(2.0f));
            }
        }
        return Maximum;
    }

    Quality ObserveJolt(const PhysicsJoltEvaluation::JoltWorld &World, JPH::BodyID Body,
                        const JPH::SoftBodySharedSettings &Shared, int Resolution, bool Rubber,
                        float PinOffset,
                        bool BallCollider, float ColliderZ) {
        JPH::BodyLockRead Lock(World.World.GetBodyLockInterface(), Body);
        if (!Lock.Succeeded()) throw std::runtime_error("[Physics:SoftEvaluation] Jolt soft body lock failed");
        const auto *Motion = static_cast<const JPH::SoftBodyMotionProperties *>(Lock.GetBody().GetMotionProperties());
        const auto &Vertices = Motion->GetVertices();
        Quality Result;
        Result.VertexCount = Vertices.size();
        if (Rubber) Result.MaxEdgeStretch = PhysicsJoltEvaluation::MaxEdgeStretch(Shared, *Motion);
        else Result.MaxEdgeStretch = ClothGridStrain(Resolution, 8.0f, [&](int A, int B) {
            return (Vertices[B].mPosition - Vertices[A].mPosition).Length();
        });
        const auto BodyPosition = Lock.GetBody().GetPosition();
        double RestVolume = 0.0;
        double CurrentVolume = 0.0;
        for (const auto &Volume : Shared.mVolumeConstraints) {
            const auto A = Vertices[Volume.mVertex[0]].mPosition;
            const auto B = Vertices[Volume.mVertex[1]].mPosition;
            const auto C = Vertices[Volume.mVertex[2]].mPosition;
            const auto D = Vertices[Volume.mVertex[3]].mPosition;
            RestVolume += Volume.mSixRestVolume;
            CurrentVolume += std::abs((B - A).Cross(C - A).Dot(D - A));
            const auto RestA = JPH::Vec3(Shared.mVertices[Volume.mVertex[0]].mPosition);
            const auto RestB = JPH::Vec3(Shared.mVertices[Volume.mVertex[1]].mPosition);
            const auto RestC = JPH::Vec3(Shared.mVertices[Volume.mVertex[2]].mPosition);
            const auto RestD = JPH::Vec3(Shared.mVertices[Volume.mVertex[3]].mPosition);
            const auto RestSigned = (RestB - RestA).Cross(RestC - RestA).Dot(RestD - RestA);
            const auto CurrentSigned = (B - A).Cross(C - A).Dot(D - A);
            if (RestSigned * CurrentSigned <= 0.0f) ++Result.InvertedTetrahedra;
        }
        if (RestVolume > 0.0) Result.VolumeError = std::abs(CurrentVolume / RestVolume - 1.0);
        for (std::size_t Index = 0; Index < Vertices.size(); ++Index) {
            const auto Position = Vertices[Index].mPosition;
            Result.Finite &= std::isfinite(Position.GetX()) && std::isfinite(Position.GetY()) && std::isfinite(Position.GetZ());
            const double WorldY = BodyPosition.GetY() + Position.GetY();
            Result.MinY = std::min(Result.MinY, WorldY);
            Result.MaxY = std::max(Result.MaxY, WorldY);
            if (BallCollider) {
                const auto WorldPosition = Position + JPH::Vec3(static_cast<float>(BodyPosition.GetX()),
                    static_cast<float>(BodyPosition.GetY()), static_cast<float>(BodyPosition.GetZ()));
                Result.MaxPenetration = std::max(Result.MaxPenetration,
                    std::max(0.0, static_cast<double>(0.75f - (WorldPosition - JPH::Vec3(0.0f, 0.0f, ColliderZ)).Length())));
            } else {
                Result.MaxPenetration = std::max(Result.MaxPenetration, std::max(0.0, -9.45 - WorldY));
            }
            if (Shared.mVertices[Index].mInvMass == 0.0f) {
                const auto Rest = JPH::Vec3(Shared.mVertices[Index].mPosition) + JPH::Vec3(0.0f, PinOffset, 0.0f);
                Result.MaxPinDrift = std::max(Result.MaxPinDrift, static_cast<double>((Position - Rest).Length()));
            }
        }
        return Result;
    }

    Quality ObserveXpbd(const gargantuan::SoftBodyState &State, const gargantuan::SoftBodyDefinition &Definition,
                        bool BallCollider, float ColliderZ) {
        if (!State.Positions) throw std::runtime_error("[Physics:SoftEvaluation] Missing XPBD positions");
        const auto &Vertices = *State.Positions;
        Quality Result;
        Result.VertexCount = Vertices.size();
        const int Resolution = static_cast<int>(Definition.ResolutionX);
        for (std::size_t Index = 0; Index < Vertices.size(); ++Index) {
            const auto &Position = Vertices[Index];
            Result.Finite &= std::isfinite(Position.x) && std::isfinite(Position.y) && std::isfinite(Position.z);
            Result.MinY = std::min(Result.MinY, static_cast<double>(Position.y));
            Result.MaxY = std::max(Result.MaxY, static_cast<double>(Position.y));
            if (BallCollider)
                Result.MaxPenetration = std::max(Result.MaxPenetration,
                    std::max(0.0, static_cast<double>(0.75f - glm::length(Position - glm::vec3(0.0f, 0.0f, ColliderZ)))));
            else Result.MaxPenetration = std::max(Result.MaxPenetration,
                std::max(0.0, -9.45 - static_cast<double>(Position.y)));
        }
        if (Definition.Kind == gargantuan::SoftBodyKind::Cloth) {
            Result.MaxEdgeStretch = ClothGridStrain(Resolution, Definition.Size.x, [&](int A, int B) {
                return glm::length(Vertices[B] - Vertices[A]);
            });
        } else {
            const auto XCount = Definition.ResolutionX;
            const auto YCount = Definition.ResolutionY;
            const auto ZCount = Definition.ResolutionZ;
            const auto At = [XCount, YCount](std::uint32_t X, std::uint32_t Y, std::uint32_t Z) {
                return (Z * YCount + Y) * XCount + X;
            };
            const auto CheckRubberEdge = [&](std::uint32_t A, std::uint32_t B, float EdgeRest) {
                const auto Length = glm::length(Vertices[B] - Vertices[A]);
                Result.MaxEdgeStretch = std::max(Result.MaxEdgeStretch,
                    std::abs(static_cast<double>(Length / EdgeRest) - 1.0));
            };
            for (std::uint32_t Z = 0; Z < ZCount; ++Z)
                for (std::uint32_t Y = 0; Y < YCount; ++Y)
                    for (std::uint32_t X = 0; X < XCount; ++X) {
                        if (X + 1 < XCount) CheckRubberEdge(At(X, Y, Z), At(X + 1, Y, Z),
                            Definition.Size.x / static_cast<float>(XCount - 1));
                        if (Y + 1 < YCount) CheckRubberEdge(At(X, Y, Z), At(X, Y + 1, Z),
                            Definition.Size.y / static_cast<float>(YCount - 1));
                        if (Z + 1 < ZCount) CheckRubberEdge(At(X, Y, Z), At(X, Y, Z + 1),
                            Definition.Size.z / static_cast<float>(ZCount - 1));
                    }
            constexpr std::array<std::array<int, 4>, 5> Tetra{{
                {{0, 1, 2, 4}}, {{1, 3, 2, 7}}, {{1, 2, 4, 7}}, {{1, 4, 5, 7}}, {{2, 6, 4, 7}}
            }};
            double CurrentVolume = 0.0;
            for (std::uint32_t Z = 0; Z + 1 < ZCount; ++Z)
                for (std::uint32_t Y = 0; Y + 1 < YCount; ++Y)
                    for (std::uint32_t X = 0; X + 1 < XCount; ++X) {
                        const std::array<std::uint32_t, 8> Corner{{
                            At(X, Y, Z), At(X + 1, Y, Z), At(X, Y + 1, Z), At(X + 1, Y + 1, Z),
                            At(X, Y, Z + 1), At(X + 1, Y, Z + 1), At(X, Y + 1, Z + 1), At(X + 1, Y + 1, Z + 1)
                        }};
                        for (const auto &Indices : Tetra) {
                            const auto A = Vertices[Corner[Indices[0]]];
                            const auto B = Vertices[Corner[Indices[1]]];
                            const auto C = Vertices[Corner[Indices[2]]];
                            const auto D = Vertices[Corner[Indices[3]]];
                            const float Signed = glm::dot(glm::cross(B - A, C - A), D - A);
                            const float RestSigned = glm::dot(glm::cross(
                                glm::vec3((Indices[1] & 1) ? 1.0f : 0.0f, (Indices[1] & 2) ? 1.0f : 0.0f, (Indices[1] & 4) ? 1.0f : 0.0f) -
                                glm::vec3((Indices[0] & 1) ? 1.0f : 0.0f, (Indices[0] & 2) ? 1.0f : 0.0f, (Indices[0] & 4) ? 1.0f : 0.0f),
                                glm::vec3((Indices[2] & 1) ? 1.0f : 0.0f, (Indices[2] & 2) ? 1.0f : 0.0f, (Indices[2] & 4) ? 1.0f : 0.0f) -
                                glm::vec3((Indices[0] & 1) ? 1.0f : 0.0f, (Indices[0] & 2) ? 1.0f : 0.0f, (Indices[0] & 4) ? 1.0f : 0.0f)),
                                glm::vec3((Indices[3] & 1) ? 1.0f : 0.0f, (Indices[3] & 2) ? 1.0f : 0.0f, (Indices[3] & 4) ? 1.0f : 0.0f) -
                                glm::vec3((Indices[0] & 1) ? 1.0f : 0.0f, (Indices[0] & 2) ? 1.0f : 0.0f, (Indices[0] & 4) ? 1.0f : 0.0f));
                            if (Signed * RestSigned <= 0.0f) ++Result.InvertedTetrahedra;
                            CurrentVolume += std::abs(Signed) / 6.0;
                        }
                    }
            Result.VolumeError = std::abs(CurrentVolume / (Definition.Size.x * Definition.Size.y * Definition.Size.z) - 1.0);
        }
        for (const auto &Attachment : Definition.Attachments)
            Result.MaxPinDrift = std::max(Result.MaxPinDrift,
                static_cast<double>(glm::length(Vertices[Attachment.Vertex] - Attachment.Position)));
        return Result;
    }

    Json QualityJson(const Quality &Value) {
        return {{"Finite", Value.Finite}, {"MaxEdgeStretch", Value.MaxEdgeStretch},
                {"MaxPinDrift", Value.MaxPinDrift}, {"VolumeError", Value.VolumeError},
                {"MaxPenetration", Value.MaxPenetration}, {"InvertedTetrahedra", Value.InvertedTetrahedra},
                {"MinY", Value.MinY}, {"MaxY", Value.MaxY}, {"Vertices", Value.VertexCount}};
    }

    Json RunJolt(const Options &Config) {
        PhysicsJoltEvaluation::JoltWorld World(Config.Workers - 1);
        World.World.SetGravity(JPH::Vec3(0.0f, -Config.Gravity, 0.0f));
        const bool Rubber = !IsCloth(Config);
        const int Count = BodyCount(Config);
        const int N = Resolution(Config);
        auto Shared = Rubber ? PhysicsJoltEvaluation::Rubber(N, 2.0f / (N - 1)) :
            PhysicsJoltEvaluation::Cloth(N, 8.0f, false, false, ParticleMass(Config));
        if (Rubber) for (auto &Vertex : Shared->mVertices) Vertex.mInvMass = 1.0f / ParticleMass(Config);
        std::vector<JPH::BodyID> Bodies;
        for (int Index = 0; Index < Count; ++Index)
            Bodies.push_back(World.AddSoft(Shared, JPH::RVec3(12.0 * Index, Rubber ? 4.0 : 0.0, 0.0),
                                           Iterations(Config), Rubber, 2.4f));
        JPH::BodyID Collider;
        if (HasMovingCollider(Config)) {
            Collider = World.AddRigid(new JPH::SphereShape(0.7f), JPH::RVec3(0.0, 0.0, -2.0),
                                      JPH::EMotionType::Kinematic, 1);
        } else {
            Collider = World.AddRigid(new JPH::BoxShape(JPH::Vec3(100.0f, 0.5f, 100.0f)),
                                      JPH::RVec3(0.0, -10.0, 0.0), JPH::EMotionType::Static, 0);
        }
        Json Frames = Json::array();
        double MaximumStretch = 0.0;
        double MaximumPinDrift = 0.0;
        double MaximumVolumeError = 0.0;
        double MaximumPenetration = 0.0;
        std::size_t InvertedTetrahedra = 0;
        std::size_t DisabledFrames = 0;
        std::size_t Replacements = 0;
        std::size_t StaleIdRejections = 0;
        bool Finite = true;
        const auto Start = PhysicsJoltEvaluation::Clock::now();
        for (int Frame = 0; Frame < Config.Frames; ++Frame) {
            if (HasMovingCollider(Config))
                World.World.GetBodyInterface().SetPositionAndRotation(Collider,
                    JPH::RVec3(0.0, 0.0, ColliderZ(Config, Frame)), JPH::Quat::sIdentity(), JPH::EActivation::Activate);
            if (Config.Case == "Attachments" && Frame == Config.Frames / 2) {
                JPH::BodyLockWrite Lock(World.World.GetBodyLockInterface(), Bodies[0]);
                if (!Lock.Succeeded()) throw std::runtime_error("[Physics:SoftEvaluation] Jolt attachment lock failed");
                auto *Motion = static_cast<JPH::SoftBodyMotionProperties *>(Lock.GetBody().GetMotionProperties());
                for (int X = 0; X < N; ++X) {
                    auto &Vertex = Motion->GetVertex(X);
                    Vertex.mPosition += JPH::Vec3(0.0f, 1.0f, 0.0f);
                    Vertex.mPreviousPosition = Vertex.mPosition;
                }
            }
            if (Config.Case == "PointImpulse" && Frame == Config.Frames / 3) {
                JPH::BodyLockWrite Lock(World.World.GetBodyLockInterface(), Bodies[0]);
                if (!Lock.Succeeded()) throw std::runtime_error("[Physics:SoftEvaluation] Jolt impulse lock failed");
                auto *Motion = static_cast<JPH::SoftBodyMotionProperties *>(Lock.GetBody().GetMotionProperties());
                // Match XPBD ApplyImpulseAtPosition: total impulse 10, split
                // among the four nearest movable vertices by inverse distance.
                std::array<std::size_t, 4> Closest{};
                std::array<float, 4> Distances;
                Distances.fill(std::numeric_limits<float>::infinity());
                for (std::size_t VertexIndex = 0; VertexIndex < Motion->GetVertices().size(); ++VertexIndex) {
                    const auto &Vertex = Motion->GetVertex(static_cast<JPH::uint>(VertexIndex));
                    if (Vertex.mInvMass == 0.0f) continue;
                    const float Distance = Vertex.mPosition.LengthSq();
                    for (std::size_t Candidate = 0; Candidate < 4; ++Candidate) {
                        if (Distance >= Distances[Candidate]) continue;
                        for (std::size_t Shift = 3; Shift > Candidate; --Shift) {
                            Distances[Shift] = Distances[Shift - 1];
                            Closest[Shift] = Closest[Shift - 1];
                        }
                        Distances[Candidate] = Distance;
                        Closest[Candidate] = VertexIndex;
                        break;
                    }
                }
                std::array<float, 4> Weights{};
                float TotalWeight = 0.0f;
                for (std::size_t Candidate = 0; Candidate < 4; ++Candidate) {
                    if (!std::isfinite(Distances[Candidate])) continue;
                    Weights[Candidate] = 1.0f / std::max(std::sqrt(Distances[Candidate]), 0.001f);
                    TotalWeight += Weights[Candidate];
                }
                if (TotalWeight == 0.0f) throw std::runtime_error("[Physics:SoftEvaluation] No Jolt impulse vertices");
                for (std::size_t Candidate = 0; Candidate < 4; ++Candidate)
                    if (Weights[Candidate] > 0.0f)
                        Motion->GetVertex(static_cast<JPH::uint>(Closest[Candidate])).mVelocity +=
                            JPH::Vec3(0.0f, 0.0f, 10.0f * Weights[Candidate] / TotalWeight *
                                Motion->GetVertex(static_cast<JPH::uint>(Closest[Candidate])).mInvMass);
            }
            if (Config.Case == "Lifecycle" && Frame > 0) {
                if (Frame % 40 == 10 || Frame % 40 == 30) {
                    const auto Old = Bodies[0];
                    World.Remove(Old);
                    JPH::BodyLockRead Stale(World.World.GetBodyLockInterface(), Old);
                    if (Stale.Succeeded()) throw std::runtime_error("[Physics:SoftEvaluation] Jolt stale body resolved");
                    ++StaleIdRejections;
                    Bodies.clear();
                    if (Frame % 40 == 30) ++Replacements;
                }
                if (Frame % 40 == 20 || Frame % 40 == 30)
                    Bodies.push_back(World.AddSoft(Shared, JPH::RVec3(0.0, Rubber ? 4.0 : 0.0, 0.0),
                                                   Iterations(Config), Rubber, 2.4f));
            }
            if (Bodies.empty()) ++DisabledFrames;
            const double NativeStepMs = World.Step();
            const auto ExtractionStart = PhysicsJoltEvaluation::Clock::now();
            std::size_t OwnedPositionBytes = 0;
            double OwnedPositionChecksum = 0.0;
            for (auto Body : Bodies) {
                JPH::BodyLockRead Lock(World.World.GetBodyLockInterface(), Body);
                if (!Lock.Succeeded()) throw std::runtime_error("[Physics:SoftEvaluation] Jolt extraction lock failed");
                const auto *Motion = static_cast<const JPH::SoftBodyMotionProperties *>(Lock.GetBody().GetMotionProperties());
                std::vector<JPH::Vec3> OwnedPositions;
                OwnedPositions.reserve(Motion->GetVertices().size());
                for (const auto &Vertex : Motion->GetVertices()) OwnedPositions.push_back(Vertex.mPosition);
                OwnedPositionBytes += OwnedPositions.size() * sizeof(JPH::Vec3);
                if (!OwnedPositions.empty()) OwnedPositionChecksum += OwnedPositions.back().GetY();
            }
            const double OwnedExtractionMs = PhysicsJoltEvaluation::Milliseconds(ExtractionStart);
            const double StepMs = NativeStepMs + OwnedExtractionMs;
            Json BodyQuality = Json::array();
            for (auto Body : Bodies) {
                const auto QualityValue = ObserveJolt(World, Body, *Shared, N, Rubber,
                    Config.Case == "Attachments" && Frame >= Config.Frames / 2 ? 1.0f : 0.0f,
                    HasMovingCollider(Config), ColliderZ(Config, Frame));
                MaximumStretch = std::max(MaximumStretch, QualityValue.MaxEdgeStretch);
                MaximumPinDrift = std::max(MaximumPinDrift, QualityValue.MaxPinDrift);
                MaximumVolumeError = std::max(MaximumVolumeError, QualityValue.VolumeError);
                MaximumPenetration = std::max(MaximumPenetration, QualityValue.MaxPenetration);
                InvertedTetrahedra += QualityValue.InvertedTetrahedra;
                Finite &= QualityValue.Finite;
                BodyQuality.push_back(QualityJson(QualityValue));
            }
            Frames.push_back({{"Frame", Frame}, {"StepMs", StepMs},
                              {"NativeStepMs", NativeStepMs}, {"OwnedExtractionMs", OwnedExtractionMs},
                              {"OwnedPositionBytes", OwnedPositionBytes},
                              {"OwnedPositionChecksum", OwnedPositionChecksum},
                              {"Bodies", std::move(BodyQuality)}});
        }
        return {{"Backend", "Jolt"}, {"Case", Config.Case}, {"Frames", std::move(Frames)},
                {"SetupPlusRunMs", PhysicsJoltEvaluation::Milliseconds(Start)},
                {"Finite", Finite}, {"MaxEdgeStretch", MaximumStretch},
                {"MaxPinDrift", MaximumPinDrift}, {"MaxVolumeError", MaximumVolumeError},
                {"MaxPenetration", MaximumPenetration}, {"InvertedTetrahedronFrames", InvertedTetrahedra},
                {"BackgroundWorkers", Config.Workers - 1}, {"CallerParticipatesInSolve", true},
                {"TimingScope", "NativeStepPlusOwnedPositionCopy; quality analysis excluded"},
                {"SolverIterations", Iterations(Config)}, {"ParticleMass", ParticleMass(Config)},
                {"Track", Config.Track},
                {"SoftVerticesPerBody", Shared->mVertices.size()}, {"Bodies", Count},
                {"TetrahedraPerCell", Rubber ? 6 : 0},
                {"DisabledFrames", DisabledFrames}, {"Replacements", Replacements},
                {"StaleIdRejections", StaleIdRejections},
                {"EnableDisableSemantics", "Jolt removal and readmission; no semantic Enabled property"},
                {"ColliderMotion", HasMovingCollider(Config) ? "DiscreteKinematicTransform" : "Static"},
                {"ColliderSpeed", HasMovingCollider(Config) ?
                    (Config.Case == "HighSpeedCollider" ? 48.0 : 3.6) : 0.0}};
    }

    Json RunXpbd(const Options &Config) {
        gargantuan::SoftBodyWorldLimits Limits;
        Limits.MaximumWorkers = static_cast<std::size_t>(Config.Workers);
        gargantuan::SoftBodyWorld World(Limits);
        const bool Rubber = !IsCloth(Config);
        const int Count = BodyCount(Config);
        const int N = Resolution(Config);
        gargantuan::SoftBodyDefinition Definition;
        Definition.Kind = Rubber ? gargantuan::SoftBodyKind::Rubber : gargantuan::SoftBodyKind::Cloth;
        Definition.ResolutionX = Definition.ResolutionY = static_cast<std::uint32_t>(N);
        Definition.ResolutionZ = Rubber ? static_cast<std::uint32_t>(N) : 4;
        Definition.Size = Rubber ? glm::vec3(2.0f) : glm::vec3(8.0f, 8.0f, 0.5f);
        Definition.Material.ParticleMass = ParticleMass(Config);
        Definition.Position = {0.0f, Rubber ? 4.0f : 0.0f, 0.0f};
        for (int X = 0; X < N && !Rubber; ++X)
            Definition.Attachments.push_back({static_cast<std::uint32_t>(X),
                {Definition.Position.x + (static_cast<float>(X) / (N - 1) - 0.5f) * 8.0f, 4.0f, 0.0f}});
        std::vector<gargantuan::SoftBodyId> Bodies;
        std::vector<gargantuan::SoftBodyDefinition> Definitions;
        for (int Index = 0; Index < Count; ++Index) {
            auto Current = Definition;
            Current.Position.x += 12.0f * Index;
            for (auto &Attachment : Current.Attachments) Attachment.Position.x += 12.0f * Index;
            const auto Body = World.CreateBody(Current);
            if (!Body.IsValid()) throw std::runtime_error("[Physics:SoftEvaluation] XPBD admission failed");
            Bodies.push_back(Body);
            Definitions.push_back(std::move(Current));
        }
        Json Frames = Json::array();
        double MaximumStretch = 0.0;
        double MaximumPinDrift = 0.0;
        double MaximumVolumeError = 0.0;
        double MaximumPenetration = 0.0;
        std::size_t InvertedTetrahedra = 0;
        std::size_t DisabledFrames = 0;
        std::size_t Replacements = 0;
        std::size_t StaleIdRejections = 0;
        bool Finite = true;
        const auto Start = PhysicsJoltEvaluation::Clock::now();
        for (int Frame = 0; Frame < Config.Frames; ++Frame) {
            if (Config.Case == "Attachments" && Frame == Config.Frames / 2) {
                for (auto &Attachment : Definitions[0].Attachments) Attachment.Position.y += 1.0f;
                if (!World.UpdateBody(Bodies[0], Definitions[0]).Succeeded())
                    throw std::runtime_error("[Physics:SoftEvaluation] XPBD attachment update failed");
            }
            if (Config.Case == "PointImpulse" && Frame == Config.Frames / 3 &&
                !World.ApplyImpulseAtPosition(Bodies[0], {0.0f, 0.0f, 10.0f}, {0.0f, 0.0f, 0.0f}).Succeeded())
                throw std::runtime_error("[Physics:SoftEvaluation] XPBD point impulse failed");
            if (Config.Case == "Lifecycle" && Frame > 0) {
                if (Frame % 40 == 10 || Frame % 40 == 20) {
                    Definitions[0].Enabled = Frame % 40 == 20;
                    if (!World.UpdateBody(Bodies[0], Definitions[0]).Succeeded())
                        throw std::runtime_error("[Physics:SoftEvaluation] XPBD enable update failed");
                }
                if (Frame % 40 == 30) {
                    const auto Old = Bodies[0];
                    if (!World.DestroyBody(Old).Succeeded() || World.IsBodyValid(Old))
                        throw std::runtime_error("[Physics:SoftEvaluation] XPBD stale body survived destruction");
                    ++StaleIdRejections;
                    Bodies[0] = World.CreateBody(Definitions[0]);
                    if (!Bodies[0].IsValid()) throw std::runtime_error("[Physics:SoftEvaluation] XPBD replacement failed");
                    ++Replacements;
                }
            }
            if (!Definitions[0].Enabled) ++DisabledFrames;
            gargantuan::SoftBodyStepConfig Step;
            Step.Gravity = {0.0f, -Config.Gravity, 0.0f};
            gargantuan::SoftBodyCollider Collider;
            if (HasMovingCollider(Config)) {
                Collider.Shape.Kind = gargantuan::PhysicsShapeKind::Ball;
                Collider.Shape.Size = {1.4f, 1.4f, 1.4f};
                Collider.Transform.Position = {0.0f, 0.0f, ColliderZ(Config, Frame)};
            } else {
                Collider.Shape.Size = {200.0f, 1.0f, 200.0f};
                Collider.Transform.Position = {0.0f, -10.0f, 0.0f};
            }
            Step.Colliders.push_back(Collider);
            const auto StepStart = PhysicsJoltEvaluation::Clock::now();
            auto Result = World.Step(Step);
            const double StepMs = PhysicsJoltEvaluation::Milliseconds(StepStart);
            Json BodyQuality = Json::array();
            for (std::size_t Index = 0; Index < Bodies.size(); ++Index) {
                const auto Found = std::find_if(Result.States.begin(), Result.States.end(),
                    [&](const auto &State) { return State.Body == Bodies[Index]; });
                if (Found == Result.States.end()) throw std::runtime_error("[Physics:SoftEvaluation] XPBD state missing");
                const auto QualityValue = ObserveXpbd(*Found, Definitions[Index],
                    HasMovingCollider(Config), ColliderZ(Config, Frame));
                MaximumStretch = std::max(MaximumStretch, QualityValue.MaxEdgeStretch);
                MaximumPinDrift = std::max(MaximumPinDrift, QualityValue.MaxPinDrift);
                MaximumVolumeError = std::max(MaximumVolumeError, QualityValue.VolumeError);
                MaximumPenetration = std::max(MaximumPenetration, QualityValue.MaxPenetration);
                InvertedTetrahedra += QualityValue.InvertedTetrahedra;
                Finite &= QualityValue.Finite;
                BodyQuality.push_back(QualityJson(QualityValue));
            }
            Frames.push_back({{"Frame", Frame}, {"StepMs", StepMs}, {"Bodies", std::move(BodyQuality)},
                              {"WorkerCount", Result.Profile.WorkerCount},
                              {"CollisionNanoseconds", Result.Profile.CollisionNanoseconds},
                              {"ConstraintNanoseconds", Result.Profile.ConstraintNanoseconds}});
        }
        World.Shutdown();
        return {{"Backend", "XPBD"}, {"Case", Config.Case}, {"Frames", std::move(Frames)},
                {"SetupPlusRunMs", PhysicsJoltEvaluation::Milliseconds(Start)},
                {"Finite", Finite}, {"MaxEdgeStretch", MaximumStretch},
                {"MaxPinDrift", MaximumPinDrift}, {"MaxVolumeError", MaximumVolumeError},
                {"MaxPenetration", MaximumPenetration}, {"InvertedTetrahedronFrames", InvertedTetrahedra},
                {"MaximumWorkers", Config.Workers}, {"CallerParticipatesInDispatchWait", true},
                {"TimingScope", "ProductionSoftBodyWorldStep including owned positions and indices; quality analysis excluded"},
                {"SolverIterations", "Automatic"},
                {"ParticleMass", ParticleMass(Config)}, {"Track", Config.Track},
                {"Bodies", Count}, {"TetrahedraPerCell", Rubber ? 5 : 0},
                {"DisabledFrames", DisabledFrames}, {"Replacements", Replacements},
                {"StaleIdRejections", StaleIdRejections},
                {"EnableDisableSemantics", "Semantic Enabled false and true; destroy/recreate independently"},
                {"ColliderMotion", HasMovingCollider(Config) ? "DiscreteNeutralTransformSnapshot" : "Static"},
                {"ColliderSpeed", HasMovingCollider(Config) ?
                    (Config.Case == "HighSpeedCollider" ? 48.0 : 3.6) : 0.0}};
    }
}

int main(int ArgumentCount, char **Arguments) {
    PhysicsEvaluation::SuppressErrorDialogs();
    try {
        const auto Config = Parse(ArgumentCount, Arguments);
        PhysicsEvaluation::InstallPhysicsAllocators();
        std::optional<PhysicsJoltEvaluation::JoltRuntime> Runtime;
        if (Config.Backend == "Jolt") Runtime.emplace();
        const auto Before = PhysicsEvaluation::CaptureResources();
        auto Result = Config.Backend == "Jolt" ? RunJolt(Config) : RunXpbd(Config);
        Result["Resources"] = PhysicsEvaluation::ResourceDelta(Before, PhysicsEvaluation::CaptureResources());
        Result["DeltaTime"] = DeltaTime;
        Result["Gravity"] = Config.Gravity;
        Result["WarmupFrames"] = Config.Warmup;
        Result["MeasuredFrames"] = Config.Frames - Config.Warmup;
        const bool MetricsPresent = Result.contains("Finite") && Result.contains("MaxEdgeStretch") &&
            Result.contains("MaxPinDrift") && Result.contains("MaxPenetration") &&
            Result.contains("MaxVolumeError") && Result.contains("InvertedTetrahedronFrames") &&
            Result["MaxEdgeStretch"].is_number() && Result["MaxPinDrift"].is_number() &&
            Result["MaxPenetration"].is_number() && Result["MaxVolumeError"].is_number();
        Result["QualityQualified"] = MetricsPresent && Result["Finite"].get<bool>() &&
            std::isfinite(Result["MaxEdgeStretch"].get<double>()) &&
            std::isfinite(Result["MaxPinDrift"].get<double>()) &&
            std::isfinite(Result["MaxPenetration"].get<double>()) &&
            std::isfinite(Result["MaxVolumeError"].get<double>()) &&
            Result["MaxEdgeStretch"].get<double>() <= 0.1 &&
            Result["MaxPinDrift"].get<double>() <= 1.0e-4 &&
            Result["MaxPenetration"].get<double>() <= 0.1 &&
            (Config.Case != "Rubber" ||
                (Result["MaxVolumeError"].get<double>() <= 0.15 &&
                 Result["InvertedTetrahedronFrames"].get<std::size_t>() == 0));
        Result["QualityGates"] = {{"MaxEdgeStrain", 0.1}, {"MaxPinDrift", 1.0e-4},
                                  {"MaxPenetration", 0.1}, {"RubberVolumeError", 0.15},
                                  {"RubberInvertedTetrahedra", 0}, {"Finite", true}};
        std::cout << Result.dump() << '\n';
        return Result["QualityQualified"].get<bool>() ? 0 : 3;
    } catch (const std::exception &Error) {
        std::cout << Json{{"Status", "Failed"}, {"Error", std::string("[Physics:SoftEvaluation] ") + Error.what()}}.dump() << '\n';
        return 2;
    }
}
