// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/physics/SoftBodyBackend.hpp"
#include "gargantuan/runtime/JobSystem.hpp"
#include "physics/SoftBodyTestHooks.hpp"
#include "ContactExperimentSeams.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/matrix.hpp>

namespace PhysicsEvaluation {
    glm::quat ExperimentalXpbdClothRotation{1.0f, 0.0f, 0.0f, 0.0f};
    std::mutex ExperimentalXpbdColliderMotionMutex;
    std::vector<glm::vec3> ExperimentalXpbdColliderDeltas;
    std::atomic<bool> ExperimentalXpbdAccountingEnabled{false};
    void SetExperimentalXpbdAccountingEnabled(bool Enabled) {
        ExperimentalXpbdAccountingEnabled.store(Enabled, std::memory_order_release);
    }
    void SetExperimentalXpbdColliderDeltas(std::vector<glm::vec3> Deltas) {
        if (Deltas.size() > 8) throw std::invalid_argument("Too many experimental collider deltas");
        for (const auto &Delta : Deltas)
            if (!std::isfinite(Delta.x) || !std::isfinite(Delta.y) || !std::isfinite(Delta.z))
                throw std::invalid_argument("Nonfinite experimental collider delta");
        std::scoped_lock Lock(ExperimentalXpbdColliderMotionMutex);
        ExperimentalXpbdColliderDeltas = std::move(Deltas);
    }
    glm::vec3 ResolveExperimentalXpbdRelativeMovement(glm::vec3 ClothMovement,
        glm::vec3 SurfaceMovement, glm::vec3 ContactNormal, float Friction) {
        const glm::vec3 Relative = ClothMovement - SurfaceMovement;
        const float NormalMovement = glm::dot(Relative, ContactNormal);
        const glm::vec3 Tangential = Relative - NormalMovement * ContactNormal;
        return SurfaceMovement + std::max(NormalMovement, 0.0f) * ContactNormal +
            Tangential * (1.0f - Friction);
    }
    void SetExperimentalXpbdClothRotation(glm::quat Rotation) {
        if (!std::isfinite(Rotation.w) || !std::isfinite(Rotation.x) ||
            !std::isfinite(Rotation.y) || !std::isfinite(Rotation.z) ||
            std::abs(glm::length(Rotation) - 1.0f) > 0.0001f)
            throw std::invalid_argument("Experimental cloth rotation must be a finite unit quaternion");
        ExperimentalXpbdClothRotation = Rotation;
    }
}

namespace gargantuan {
	namespace {
		using ProfileClock = std::chrono::steady_clock;

		// This translation unit is an isolated copy of the production XPBD solver.
		// The ordinary factory and src/physics/XpbdSoftBodyBackend.cpp stay unchanged.
		enum class ExperimentalContactOrder {
			Existing, ContactThenStructure, ContactStructureContact, CoupledNormal, TriangleSurface,
			KinematicRelative
		};
		[[nodiscard]] ExperimentalContactOrder GetExperimentalContactOrder() {
			static const auto Order = [] {
				const char *Value = std::getenv("GANTRIA_XPBD_CONTACT_ORDER");
				if (!Value || std::string_view(Value) == "A") return ExperimentalContactOrder::Existing;
				if (std::string_view(Value) == "B") return ExperimentalContactOrder::ContactThenStructure;
				if (std::string_view(Value) == "C") return ExperimentalContactOrder::ContactStructureContact;
				if (std::string_view(Value) == "D") return ExperimentalContactOrder::CoupledNormal;
				if (std::string_view(Value) == "E") return ExperimentalContactOrder::TriangleSurface;
				if (std::string_view(Value) == "G") return ExperimentalContactOrder::KinematicRelative;
				throw std::invalid_argument("[Physics:XpbdContactProbe] unsupported contact order");
			}();
			return Order;
		}
		[[nodiscard]] int GetVelocityPrecisionMode() {
			static const int Mode = [] {
				const char *Value = std::getenv("GANTRIA_XPBD_IDEAL_VELOCITY");
				if (!Value || std::string_view(Value) == "0") return 0;
				if (std::string_view(Value) == "1") return 1;
				if (std::string_view(Value) == "2") return 2;
				throw std::invalid_argument("[Physics:XpbdContactProbe] invalid ideal velocity flag");
			}();
			return Mode;
		}
		[[nodiscard]] float GetCoupledNormalCompliance(float StretchCompliance) {
			const char *Value = std::getenv("GANTRIA_XPBD_CONTACT_NORMAL_COMPLIANCE");
			if (!Value) return 0.1f * StretchCompliance;
			char *End = nullptr;
			const float Parsed = std::strtof(Value, &End);
			if (!End || *End || !std::isfinite(Parsed) || Parsed < 0.0f || Parsed > 1.0f)
				throw std::invalid_argument("[Physics:XpbdContactProbe] invalid contact normal compliance");
			return Parsed;
		}

		struct SoftBodyAabb {
			glm::vec3 Minimum{0.0f};
			glm::vec3 Maximum{0.0f};
		};

		struct SoftBodyNode {
			glm::vec3 Position{0.0f};
			glm::vec3 PreviousPosition{0.0f};
			glm::vec3 Velocity{0.0f};
			glm::vec3 RestOffset{0.0f};
			float InverseMass = 1.0f;
		};

		// The opt-in sidecar records the final substep's actual node velocities.
		// It has no role in production, solver ordering, or normal benchmark runs.
		class ExperimentalVelocityTrace {
		  public:
			ExperimentalVelocityTrace() {
				const char *Path = std::getenv("GANTRIA_CONTACT_VELOCITY_TRACE");
				if (!Path) Path = std::getenv("GANTRIA_XPBD_VELOCITY_TRACE");
				if (!Path) return;
				const char *StrideText = std::getenv("GANTRIA_CONTACT_VELOCITY_STRIDE");
				if (!StrideText) StrideText = std::getenv("GANTRIA_XPBD_VELOCITY_STRIDE");
				if (!*Path || !StrideText)
					throw std::invalid_argument("[Physics:XpbdContactProbe] velocity trace needs path and stride");
				char *End = nullptr;
				const unsigned long Parsed = std::strtoul(StrideText, &End, 10);
				if (!End || *End || Parsed < 1 || Parsed > 600)
					throw std::invalid_argument("[Physics:XpbdContactProbe] invalid velocity trace stride");
				Stride = static_cast<std::size_t>(Parsed);
				Out.open(Path, std::ios::binary | std::ios::trunc);
				if (!Out) throw std::runtime_error("[Physics:XpbdContactProbe] velocity trace open failed");
				Enabled = true;
			}
			void Capture(SoftBodyId Id, const std::vector<SoftBodyNode> &Nodes) {
				if (!Enabled) return;
				std::scoped_lock Lock(Mutex);
				if (Id.Slot != 1 || (VertexCount && VertexCount != Nodes.size()))
					throw std::runtime_error("[Physics:XpbdContactProbe] velocity trace needs one fixed-topology body");
				VertexCount = Nodes.size();
				++StepCount;
				if (StepCount % Stride != 0) return;
				std::vector<float> Values;
				Values.reserve(Nodes.size() * 3);
				for (const auto &Node : Nodes) {
					Values.push_back(Node.Velocity.x);
					Values.push_back(Node.Velocity.y);
					Values.push_back(Node.Velocity.z);
				}
				Out.write(reinterpret_cast<const char *>(Values.data()),
					static_cast<std::streamsize>(Values.size() * sizeof(float)));
				Out.flush();
				if (!Out) throw std::runtime_error("[Physics:XpbdContactProbe] velocity trace write failed");
			}
			void Finish() {
				std::scoped_lock Lock(Mutex);
				if (!Enabled) return;
				Out.flush();
				if (!Out) throw std::runtime_error("[Physics:XpbdContactProbe] velocity trace flush failed");
				Out.close();
				if (Out.fail()) throw std::runtime_error("[Physics:XpbdContactProbe] velocity trace close failed");
				Enabled = false;
			}
		  private:
			std::mutex Mutex;
			std::ofstream Out;
			std::size_t Stride = 0;
			std::size_t StepCount = 0;
			std::size_t VertexCount = 0;
			bool Enabled = false;
		};
		ExperimentalVelocityTrace &GetExperimentalVelocityTrace() {
			static ExperimentalVelocityTrace Trace;
			return Trace;
		}

		struct SoftBodyDistanceConstraint {
			std::uint32_t VertexA = 0;
			std::uint32_t VertexB = 0;
			float RestLength = 0.0f;
			float Compliance = 0.0f;
			float Lambda = 0.0f;
		};

		struct SoftBodyVolumeConstraint {
			std::array<std::uint32_t, 4> Vertices{};
			float RestVolume = 0.0f;
			float Lambda = 0.0f;
		};

		struct IdealVelocitySnapshot {
			glm::vec3 IntegratedPosition{0.0f};
			glm::vec3 InitialPreviousPosition{0.0f};
			glm::dvec3 StructuralFloatBias{0.0};
		};

		struct DoubleShadowNode {
			glm::dvec3 Position{0.0};
			glm::dvec3 PreviousPosition{0.0};
			glm::vec3 PublishedPosition{0.0f};
			glm::vec3 PublishedPreviousPosition{0.0f};
		};
		struct DoubleShadowDistance {
			double RestLength = 0.0;
			double Lambda = 0.0;
		};

		struct SoftBodyRecord {
			SoftBodyDefinition Definition;
			std::vector<SoftBodyNode> Nodes;
			std::vector<SoftBodyDistanceConstraint> Distances;
			std::vector<SoftBodyVolumeConstraint> Volumes;
			std::shared_ptr<const std::vector<std::uint32_t>> Indices;
			std::uint64_t TopologyRevision = 1;
			std::uint64_t VertexRevision = 1;
			PhysicsEvaluation::XpbdContactProjection ContactProjection;
			bool CaptureAccounting = false;
			bool ReconstructIdealVelocity = false;
			bool UseDoubleShadow = false;
			std::vector<IdealVelocitySnapshot> IdealVelocitySnapshots;
			std::vector<DoubleShadowNode> DoubleShadowNodes;
			std::vector<DoubleShadowDistance> DoubleShadowDistances;
		};

		[[nodiscard]] double NodeKineticEnergy(const SoftBodyNode &Node) {
			if (Node.InverseMass <= 0.0f) return 0.0;
			const glm::dvec3 Velocity(Node.Velocity);
			return 0.5 * glm::dot(Velocity, Velocity) / double(Node.InverseMass);
		}
		[[nodiscard]] double StoredKineticEnergy(const SoftBodyRecord &Record) {
			double Energy = 0.0;
			for (const auto &Node : Record.Nodes) Energy += NodeKineticEnergy(Node);
			return Energy;
		}
		[[nodiscard]] glm::dvec3 NodeMomentum(const SoftBodyNode &Node) {
			return Node.InverseMass > 0.0f ?
				glm::dvec3(Node.Velocity) / double(Node.InverseMass) : glm::dvec3(0.0);
		}
		[[nodiscard]] glm::dvec3 StoredMomentum(const SoftBodyRecord &Record) {
			glm::dvec3 Momentum(0.0);
			for (const auto &Node : Record.Nodes) Momentum += NodeMomentum(Node);
			return Momentum;
		}
		[[nodiscard]] double PositionKineticEnergy(const SoftBodyRecord &Record, float DeltaTime) {
			double Energy = 0.0;
			for (const auto &Node : Record.Nodes) if (Node.InverseMass > 0.0f) {
				const glm::dvec3 Velocity =
					glm::dvec3(Node.Position - Node.PreviousPosition) / double(DeltaTime);
				Energy += 0.5 * glm::dot(Velocity, Velocity) / double(Node.InverseMass);
			}
			return Energy;
		}

		void RecordContactProjection(SoftBodyRecord &Record, const SoftBodyNode &Node,
			glm::vec3 BeforePosition, glm::vec3 BeforePrevious,
			glm::vec3 ContactNormal, glm::vec3 SurfaceDelta, float DeltaTime) {
			if (!Record.CaptureAccounting || Node.InverseMass <= 0.0f) return;
			const double Mass = 1.0 / double(Node.InverseMass);
			const glm::dvec3 PositionChange = Record.UseDoubleShadow ?
				glm::dvec3(Node.Position) - glm::dvec3(BeforePosition) :
				glm::dvec3(Node.Position - BeforePosition);
			const glm::dvec3 PreviousChange = Record.UseDoubleShadow ?
				glm::dvec3(Node.PreviousPosition) - glm::dvec3(BeforePrevious) :
				glm::dvec3(Node.PreviousPosition - BeforePrevious);
			const glm::dvec3 PositionImpulse = Mass * PositionChange / double(DeltaTime);
			const glm::dvec3 VelocityResponseImpulse = -Mass * PreviousChange /
				double(DeltaTime);
			const glm::dvec3 Direction(ContactNormal);
			const glm::dvec3 NormalResponseImpulse = glm::dot(VelocityResponseImpulse, Direction) * Direction;
			const glm::dvec3 NormalImpulse = PositionImpulse + NormalResponseImpulse;
			const glm::dvec3 FrictionImpulse = VelocityResponseImpulse - NormalResponseImpulse;
			const glm::dvec3 TotalImpulse = PositionImpulse + VelocityResponseImpulse;
			const glm::dvec3 BeforeVelocity =
				glm::dvec3(BeforePosition - BeforePrevious) / double(DeltaTime);
			const glm::dvec3 AfterVelocity =
				glm::dvec3(Node.Position - Node.PreviousPosition) / double(DeltaTime);
			Record.ContactProjection.NormalImpulseOnClothNs += NormalImpulse;
			Record.ContactProjection.FrictionImpulseOnClothNs += FrictionImpulse;
			Record.ContactProjection.VelocityDerivedMomentumDeltaNs += TotalImpulse;
			Record.ContactProjection.PrescribedColliderReactionImpulseNs -= TotalImpulse;
			Record.ContactProjection.ContactKineticDeltaJ += 0.5 * Mass *
				(glm::dot(AfterVelocity, AfterVelocity) - glm::dot(BeforeVelocity, BeforeVelocity));
			Record.ContactProjection.PrescribedSurfaceWorkJ +=
				glm::dot(TotalImpulse, glm::dvec3(SurfaceDelta) / double(DeltaTime));
			++Record.ContactProjection.Corrections;
		}

		struct PendingSoftBodyAttachmentUpdate {
			std::uint64_t SourceDefinitionRevision = 0;
			std::uint64_t TargetDefinitionRevision = 0;
			std::vector<SoftBodyAttachmentDesc> RecordAttachments;
			std::vector<bool> Attached;
			std::shared_ptr<std::vector<glm::vec3>> MergePositions;
		};

		struct SoftBodySlot {
			std::uint32_t Generation = 1;
			std::uint64_t DefinitionRevision = 1;
			bool Live = false;
			bool Retired = false;
			SoftBodyDefinition Definition;
			std::optional<SoftBodyRecord> Record;
			std::optional<PendingSoftBodyAttachmentUpdate> PendingAttachmentUpdate;
			SoftBodyState LastState;
			std::size_t VertexCount = 0;
			std::size_t ConstraintCount = 0;
			glm::vec3 PendingForce{0.0f};
			glm::vec3 PendingImpulse{0.0f};
			std::vector<SoftBodyPointImpulse> PendingPointImpulses;
		};

		struct SoftBodyWorkItem {
			SoftBodyId Body;
			std::uint64_t DefinitionRevision = 0;
			SoftBodyRecord Record;
			glm::vec3 Force{0.0f};
			glm::vec3 Impulse{0.0f};
			std::vector<SoftBodyPointImpulse> PointImpulses;
			SoftBodyState State;
			SoftBodyStepProfile Profile;
			std::uint64_t WorkerNanoseconds = 0;
			bool Completed = false;
			bool Failed = false;
		};

		[[nodiscard]] bool IsFinite(float Value) {
			return std::isfinite(Value);
		}
		[[nodiscard]] bool IsFinite(const glm::vec3 &Value) {
			return IsFinite(Value.x) && IsFinite(Value.y) && IsFinite(Value.z);
		}
		[[nodiscard]] bool IsFinite(const glm::mat3 &Value) {
			return IsFinite(Value[0]) && IsFinite(Value[1]) && IsFinite(Value[2]);
		}

		[[nodiscard]] std::uint64_t Nanoseconds(ProfileClock::duration Duration) {
			return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count());
		}

		[[nodiscard]] bool Multiply(std::size_t Left, std::size_t Right, std::size_t &Result) {
			if (Left != 0 && Right > std::numeric_limits<std::size_t>::max() / Left) return false;
			Result = Left * Right;
			return true;
		}

		[[nodiscard]] bool ValidMaterial(const SoftBodyMaterialDesc &Material) {
			return IsFinite(Material.ParticleMass) && Material.ParticleMass > 0.0f && IsFinite(Material.Damping) &&
				   Material.Damping >= 0.0f && Material.Damping <= 1.0f && IsFinite(Material.StretchCompliance) &&
				   Material.StretchCompliance >= 0.0f && IsFinite(Material.BendCompliance) &&
				   Material.BendCompliance >= 0.0f && IsFinite(Material.ShapeCompliance) &&
				   Material.ShapeCompliance >= 0.0f && IsFinite(Material.VolumeCompliance) &&
				   Material.VolumeCompliance >= 0.0f && IsFinite(Material.Friction) && Material.Friction >= 0.0f &&
				   Material.Friction <= 1.0f && IsFinite(Material.Thickness) && Material.Thickness >= 0.0f &&
				   Material.Thickness <= 10.0f;
		}

		[[nodiscard]] bool ValidCollider(const SoftBodyCollider &Collider) {
			switch (Collider.Shape.Kind) {
			case PhysicsShapeKind::Box:
			case PhysicsShapeKind::Ball:
			case PhysicsShapeKind::Cylinder:
			case PhysicsShapeKind::Wedge:
			case PhysicsShapeKind::CornerWedge:
				break;
			default:
				return false;
			}
			if (!IsFinite(Collider.Shape.Size) || Collider.Shape.Size.x <= 0.0f || Collider.Shape.Size.y <= 0.0f ||
				Collider.Shape.Size.z <= 0.0f || !IsFinite(Collider.Transform.Position) ||
				!IsFinite(Collider.Transform.Rotation))
				return false;
			const auto &Rotation = Collider.Transform.Rotation;
			constexpr float Tolerance = 0.02f;
			return std::abs(glm::dot(Rotation[0], Rotation[0]) - 1.0f) <= Tolerance &&
				   std::abs(glm::dot(Rotation[1], Rotation[1]) - 1.0f) <= Tolerance &&
				   std::abs(glm::dot(Rotation[2], Rotation[2]) - 1.0f) <= Tolerance &&
				   std::abs(glm::dot(Rotation[0], Rotation[1])) <= Tolerance &&
				   std::abs(glm::dot(Rotation[0], Rotation[2])) <= Tolerance &&
				   std::abs(glm::dot(Rotation[1], Rotation[2])) <= Tolerance;
		}

		[[nodiscard]] std::optional<std::size_t> VertexCount(const SoftBodyDefinition &Definition) {
			if (Definition.ResolutionX < 2 || Definition.ResolutionY < 2) return std::nullopt;
			std::size_t Result = 0;
			if (!Multiply(Definition.ResolutionX, Definition.ResolutionY, Result)) return std::nullopt;
			if (Definition.Kind == SoftBodyKind::Rubber) {
				if (Definition.ResolutionZ < 2 || !Multiply(Result, Definition.ResolutionZ, Result))
					return std::nullopt;
			}
			return Result;
		}

		[[nodiscard]] bool SameTopology(const SoftBodyDefinition &Left, const SoftBodyDefinition &Right) {
			return Left.Kind == Right.Kind && Left.ResolutionX == Right.ResolutionX &&
				   Left.ResolutionY == Right.ResolutionY &&
				   (Left.Kind == SoftBodyKind::Cloth || Left.ResolutionZ == Right.ResolutionZ);
		}

		[[nodiscard]] bool SameDefinitionExceptAttachments(
			const SoftBodyDefinition &Left,
			const SoftBodyDefinition &Right
		) {
			const auto &A = Left.Material;
			const auto &B = Right.Material;
			return SameTopology(Left, Right) && glm::all(glm::equal(Left.Position, Right.Position)) &&
				   glm::all(glm::equal(Left.Size, Right.Size)) && Left.Quality == Right.Quality &&
				   Left.CollisionMode == Right.CollisionMode && Left.Enabled == Right.Enabled &&
				   A.ParticleMass == B.ParticleMass && A.Damping == B.Damping &&
				   A.StretchCompliance == B.StretchCompliance && A.BendCompliance == B.BendCompliance &&
				   A.ShapeCompliance == B.ShapeCompliance && A.VolumeCompliance == B.VolumeCompliance &&
				   A.Friction == B.Friction && A.Thickness == B.Thickness;
		}

		void ApplyAttachmentNodes(
			SoftBodyRecord &Record,
			const std::vector<SoftBodyAttachmentDesc> &Attachments,
			const std::vector<bool> &Attached
		) {
			for (std::size_t Index = 0; Index < Record.Nodes.size(); ++Index) {
				auto &Node = Record.Nodes[Index];
				if (!Attached[Index] && Node.InverseMass == 0.0f) {
					Node.InverseMass = 1.0f / Record.Definition.Material.ParticleMass;
					Node.Velocity = {};
				}
			}
			for (const auto &Attachment : Attachments) {
				auto &Node = Record.Nodes[Attachment.Vertex];
				Node.Position = Attachment.Position;
				Node.PreviousPosition = Attachment.Position;
				Node.Velocity = {};
				Node.InverseMass = 0.0f;
			}
		}

		void AddDistance(SoftBodyRecord &Record, std::uint32_t VertexA, std::uint32_t VertexB, float Compliance) {
			const auto Delta = Record.Nodes[VertexB].Position - Record.Nodes[VertexA].Position;
			Record.Distances.push_back({VertexA, VertexB, glm::length(Delta), Compliance, 0.0f});
		}

		void AddVolume(SoftBodyRecord &Record, std::array<std::uint32_t, 4> Vertices) {
			const auto &A = Record.Nodes[Vertices[0]].Position;
			const auto &B = Record.Nodes[Vertices[1]].Position;
			const auto &C = Record.Nodes[Vertices[2]].Position;
			const auto &D = Record.Nodes[Vertices[3]].Position;
			Record.Volumes.push_back({Vertices, glm::dot(B - A, glm::cross(C - A, D - A)) / 6.0f, 0.0f});
		}

		void AddQuad(
			std::vector<std::uint32_t> &Indices, std::uint32_t A, std::uint32_t B, std::uint32_t C, std::uint32_t D
		) {
			Indices.insert(Indices.end(), {A, B, C, A, C, D});
		}

		void BuildCloth(SoftBodyRecord &Record) {
			const auto XCount = Record.Definition.ResolutionX;
			const auto YCount = Record.Definition.ResolutionY;
			Record.Nodes.reserve(static_cast<std::size_t>(XCount) * YCount);
			for (std::uint32_t Y = 0; Y < YCount; ++Y) {
				const auto V = static_cast<float>(Y) / static_cast<float>(YCount - 1);
				for (std::uint32_t X = 0; X < XCount; ++X) {
					const auto U = static_cast<float>(X) / static_cast<float>(XCount - 1);
					const glm::vec3 Offset{
						(U - 0.5f) * Record.Definition.Size.x,
						(0.5f - V) * Record.Definition.Size.y,
						0.0f,
					};
					const auto RotatedOffset = PhysicsEvaluation::ExperimentalXpbdClothRotation * Offset;
					const auto Position = Record.Definition.Position + RotatedOffset;
					Record.Nodes.push_back(
						{Position, Position, {}, RotatedOffset, 1.0f / Record.Definition.Material.ParticleMass}
					);
				}
			}
			auto Index = [XCount](std::uint32_t X, std::uint32_t Y) { return Y * XCount + X; };
			auto Indices = std::make_shared<std::vector<std::uint32_t>>();
			Indices->reserve(static_cast<std::size_t>(XCount - 1) * (YCount - 1) * 6);
			for (std::uint32_t Y = 0; Y + 1 < YCount; ++Y)
				for (std::uint32_t X = 0; X + 1 < XCount; ++X)
					AddQuad(*Indices, Index(X, Y), Index(X, Y + 1), Index(X + 1, Y + 1), Index(X + 1, Y));
			Record.Indices = std::move(Indices);
			for (std::uint32_t Y = 0; Y < YCount; ++Y) {
				for (std::uint32_t X = 0; X < XCount; ++X) {
					if (X + 1 < XCount)
						AddDistance(Record, Index(X, Y), Index(X + 1, Y), Record.Definition.Material.StretchCompliance);
					if (Y + 1 < YCount)
						AddDistance(Record, Index(X, Y), Index(X, Y + 1), Record.Definition.Material.StretchCompliance);
					if (X + 1 < XCount && Y + 1 < YCount) {
						AddDistance(
							Record, Index(X, Y), Index(X + 1, Y + 1), Record.Definition.Material.StretchCompliance
						);
						AddDistance(
							Record, Index(X + 1, Y), Index(X, Y + 1), Record.Definition.Material.StretchCompliance
						);
					}
					if (X + 2 < XCount)
						AddDistance(Record, Index(X, Y), Index(X + 2, Y), Record.Definition.Material.BendCompliance);
					if (Y + 2 < YCount)
						AddDistance(Record, Index(X, Y), Index(X, Y + 2), Record.Definition.Material.BendCompliance);
				}
			}
		}

		void BuildRubber(SoftBodyRecord &Record) {
			const auto XCount = Record.Definition.ResolutionX;
			const auto YCount = Record.Definition.ResolutionY;
			const auto ZCount = Record.Definition.ResolutionZ;
			Record.Nodes.reserve(static_cast<std::size_t>(XCount) * YCount * ZCount);
			auto Index = [XCount, YCount](std::uint32_t X, std::uint32_t Y, std::uint32_t Z) {
				return (Z * YCount + Y) * XCount + X;
			};
			for (std::uint32_t Z = 0; Z < ZCount; ++Z) {
				const auto W = static_cast<float>(Z) / static_cast<float>(ZCount - 1);
				for (std::uint32_t Y = 0; Y < YCount; ++Y) {
					const auto V = static_cast<float>(Y) / static_cast<float>(YCount - 1);
					for (std::uint32_t X = 0; X < XCount; ++X) {
						const auto U = static_cast<float>(X) / static_cast<float>(XCount - 1);
						const glm::vec3 Offset{
							(U - 0.5f) * Record.Definition.Size.x,
							(V - 0.5f) * Record.Definition.Size.y,
							(W - 0.5f) * Record.Definition.Size.z,
						};
						const auto Position = Record.Definition.Position + Offset;
						Record.Nodes.push_back(
							{Position, Position, {}, Offset, 1.0f / Record.Definition.Material.ParticleMass}
						);
					}
				}
			}
			auto Indices = std::make_shared<std::vector<std::uint32_t>>();
			for (std::uint32_t Y = 0; Y + 1 < YCount; ++Y)
				for (std::uint32_t X = 0; X + 1 < XCount; ++X) {
					AddQuad(*Indices, Index(X, Y, 0), Index(X + 1, Y, 0), Index(X + 1, Y + 1, 0), Index(X, Y + 1, 0));
					AddQuad(
						*Indices,
						Index(X, Y, ZCount - 1),
						Index(X, Y + 1, ZCount - 1),
						Index(X + 1, Y + 1, ZCount - 1),
						Index(X + 1, Y, ZCount - 1)
					);
				}
			for (std::uint32_t Z = 0; Z + 1 < ZCount; ++Z)
				for (std::uint32_t X = 0; X + 1 < XCount; ++X) {
					AddQuad(*Indices, Index(X, 0, Z), Index(X, 0, Z + 1), Index(X + 1, 0, Z + 1), Index(X + 1, 0, Z));
					AddQuad(
						*Indices,
						Index(X, YCount - 1, Z),
						Index(X + 1, YCount - 1, Z),
						Index(X + 1, YCount - 1, Z + 1),
						Index(X, YCount - 1, Z + 1)
					);
				}
			for (std::uint32_t Z = 0; Z + 1 < ZCount; ++Z)
				for (std::uint32_t Y = 0; Y + 1 < YCount; ++Y) {
					AddQuad(*Indices, Index(0, Y, Z), Index(0, Y + 1, Z), Index(0, Y + 1, Z + 1), Index(0, Y, Z + 1));
					AddQuad(
						*Indices,
						Index(XCount - 1, Y, Z),
						Index(XCount - 1, Y, Z + 1),
						Index(XCount - 1, Y + 1, Z + 1),
						Index(XCount - 1, Y + 1, Z)
					);
				}
			Record.Indices = std::move(Indices);
			for (std::uint32_t Z = 0; Z < ZCount; ++Z)
				for (std::uint32_t Y = 0; Y < YCount; ++Y)
					for (std::uint32_t X = 0; X < XCount; ++X) {
						if (X + 1 < XCount)
							AddDistance(
								Record, Index(X, Y, Z), Index(X + 1, Y, Z), Record.Definition.Material.StretchCompliance
							);
						if (Y + 1 < YCount)
							AddDistance(
								Record, Index(X, Y, Z), Index(X, Y + 1, Z), Record.Definition.Material.StretchCompliance
							);
						if (Z + 1 < ZCount)
							AddDistance(
								Record, Index(X, Y, Z), Index(X, Y, Z + 1), Record.Definition.Material.StretchCompliance
							);
					}
			for (std::uint32_t Z = 0; Z + 1 < ZCount; ++Z)
				for (std::uint32_t Y = 0; Y + 1 < YCount; ++Y)
					for (std::uint32_t X = 0; X + 1 < XCount; ++X) {
						const auto V000 = Index(X, Y, Z);
						const auto V100 = Index(X + 1, Y, Z);
						const auto V010 = Index(X, Y + 1, Z);
						const auto V110 = Index(X + 1, Y + 1, Z);
						const auto V001 = Index(X, Y, Z + 1);
						const auto V101 = Index(X + 1, Y, Z + 1);
						const auto V011 = Index(X, Y + 1, Z + 1);
						const auto V111 = Index(X + 1, Y + 1, Z + 1);
						AddVolume(Record, {V000, V100, V010, V001});
						AddVolume(Record, {V100, V110, V010, V111});
						AddVolume(Record, {V100, V010, V001, V111});
						AddVolume(Record, {V100, V001, V101, V111});
						AddVolume(Record, {V010, V011, V001, V111});
					}
		}

		[[nodiscard]] std::optional<SoftBodyRecord> BuildRecord(const SoftBodyDefinition &Definition) {
			SoftBodyRecord Result;
			Result.Definition = Definition;
			if (Definition.Kind == SoftBodyKind::Cloth)
				BuildCloth(Result);
			else
				BuildRubber(Result);
			for (const auto &Attachment : Definition.Attachments) {
				if (Attachment.Vertex >= Result.Nodes.size()) return std::nullopt;
				auto &Node = Result.Nodes[Attachment.Vertex];
				Node.Position = Attachment.Position;
				Node.PreviousPosition = Attachment.Position;
				Node.Velocity = {};
				Node.InverseMass = 0.0f;
			}
			return Result;
		}

		[[nodiscard]] int IterationCount(const SoftBodyRecord &Record) {
			switch (Record.Definition.Quality) {
			case SoftBodyQuality::Low:
				return 2;
			case SoftBodyQuality::Medium:
				return 4;
			case SoftBodyQuality::High:
				return 8;
			case SoftBodyQuality::Automatic:
				if (Record.Nodes.size() <= 4096) return 8;
				if (Record.Nodes.size() <= 16384) return 4;
				return 2;
			}
			return 2;
		}

		[[nodiscard]] std::size_t VertexBudget(const SoftBodyRecord &Record, const SoftBodyWorldLimits &Limits) {
			switch (Record.Definition.Quality) {
			case SoftBodyQuality::Low:
				return std::min<std::size_t>(4096, Limits.MaximumWorldVertices);
			case SoftBodyQuality::Medium:
				return std::min<std::size_t>(16384, Limits.MaximumWorldVertices);
			case SoftBodyQuality::High:
			case SoftBodyQuality::Automatic:
				return Limits.MaximumWorldVertices;
			}
			return 0;
		}

		[[nodiscard]] SoftBodyState ExtractState(SoftBodyId Body, const SoftBodyRecord &Record, bool Simulated) {
			auto Positions = std::make_shared<std::vector<glm::vec3>>();
			Positions->reserve(Record.Nodes.size());
			for (const auto &Node : Record.Nodes)
				Positions->push_back(Node.Position);
			return {
				.Body = Body,
				.TopologyRevision = Record.TopologyRevision,
				.VertexRevision = Record.VertexRevision,
				.Simulated = Simulated,
				.Positions = std::move(Positions),
				.Indices = Record.Indices,
			};
		}

		[[nodiscard]] glm::vec3 AbsoluteMultiply(const glm::mat3 &Matrix, const glm::vec3 &Vector) {
			return glm::abs(Matrix[0]) * Vector.x + glm::abs(Matrix[1]) * Vector.y + glm::abs(Matrix[2]) * Vector.z;
		}

		[[nodiscard]] SoftBodyAabb ColliderBounds(const SoftBodyCollider &Collider) {
			auto Half = glm::abs(Collider.Shape.Size) * 0.5f;
			if (Collider.Shape.Kind == PhysicsShapeKind::Ball) Half = glm::vec3(std::min({Half.x, Half.y, Half.z}));
			const auto Extent = AbsoluteMultiply(Collider.Transform.Rotation, Half);
			return {Collider.Transform.Position - Extent, Collider.Transform.Position + Extent};
		}

		[[nodiscard]] bool Overlaps(const SoftBodyAabb &Left, const SoftBodyAabb &Right) {
			return Left.Minimum.x <= Right.Maximum.x && Left.Maximum.x >= Right.Minimum.x &&
				   Left.Minimum.y <= Right.Maximum.y && Left.Maximum.y >= Right.Minimum.y &&
				   Left.Minimum.z <= Right.Maximum.z && Left.Maximum.z >= Right.Minimum.z;
		}

		class SoftBodyBroadphase {
		  public:
			struct Entry {
				SoftBodyAabb Bounds;
				std::uint32_t Collider = 0;
			};
			explicit SoftBodyBroadphase(const std::vector<SoftBodyCollider> &Colliders) {
				Entries.reserve(Colliders.size());
				for (std::uint32_t Index = 0; Index < Colliders.size(); ++Index)
					Entries.push_back({ColliderBounds(Colliders[Index]), Index});
				std::ranges::sort(Entries, [](const Entry &Left, const Entry &Right) {
					if (Left.Bounds.Minimum.x != Right.Bounds.Minimum.x)
						return Left.Bounds.Minimum.x < Right.Bounds.Minimum.x;
					return Left.Collider < Right.Collider;
				});
			}
			[[nodiscard]] std::vector<std::uint32_t>
			Query(const SoftBodyAabb &Bounds, SoftBodyBroadphaseMode Mode) const {
				std::vector<std::uint32_t> Result;
				Result.reserve(Entries.size());
				for (const auto &Entry : Entries) {
					if (Mode == SoftBodyBroadphaseMode::DeterministicSweep && Entry.Bounds.Minimum.x > Bounds.Maximum.x)
						break;
					if (Mode == SoftBodyBroadphaseMode::BruteForceReference || Overlaps(Bounds, Entry.Bounds))
						Result.push_back(Entry.Collider);
				}
				std::ranges::sort(Result);
				return Result;
			}
			[[nodiscard]] std::size_t EstimatedBytes() const {
				return Entries.capacity() * sizeof(Entry);
			}

		  private:
			std::vector<Entry> Entries;
		};

		struct SoftBodySimulationBatch {
			float DeltaTime = SoftBodyStepInterval;
			glm::vec3 Gravity{0.0f};
			SoftBodyBroadphaseMode BroadphaseMode = SoftBodyBroadphaseMode::DeterministicSweep;
			std::vector<SoftBodyCollider> Colliders;
			std::unique_ptr<const SoftBodyBroadphase> Broadphase;
			std::vector<SoftBodyWorkItem> Items;
		};

		[[nodiscard]] SoftBodyAabb BodyBounds(const SoftBodyRecord &Record) {
			SoftBodyAabb Result;
			if (Record.Nodes.empty()) return Result;
			Result.Minimum = Result.Maximum = Record.Nodes.front().Position;
			for (const auto &Node : Record.Nodes) {
				Result.Minimum = glm::min(Result.Minimum, Node.Position);
				Result.Maximum = glm::max(Result.Maximum, Node.Position);
			}
			const auto Thickness = glm::vec3(Record.Definition.Material.Thickness);
			Result.Minimum -= Thickness;
			Result.Maximum += Thickness;
			return Result;
		}

		void InitializeDoubleShadow(SoftBodyRecord &Record) {
			if (!Record.DoubleShadowNodes.empty()) {
				if (Record.DoubleShadowNodes.size() != Record.Nodes.size() ||
					Record.DoubleShadowDistances.size() != Record.Distances.size())
					throw std::runtime_error(
						"[Physics:XpbdContactProbe] double-shadow node count changed");
				return;
			}
			Record.DoubleShadowNodes.resize(Record.Nodes.size());
			for (std::size_t Index = 0; Index < Record.Nodes.size(); ++Index) {
				const auto &Node = Record.Nodes[Index];
				auto &Shadow = Record.DoubleShadowNodes[Index];
				Shadow.Position = glm::dvec3(Node.Position);
				Shadow.PreviousPosition = Shadow.Position;
				Shadow.PublishedPosition = Node.Position;
				Shadow.PublishedPreviousPosition = Node.PreviousPosition;
			}
			Record.DoubleShadowDistances.resize(Record.Distances.size());
			for (std::size_t Index = 0; Index < Record.Distances.size(); ++Index) {
				const auto &Constraint = Record.Distances[Index];
				auto &ShadowDistance = Record.DoubleShadowDistances[Index];
				// BuildCloth creates rest lengths before attachment targets move nodes.
				// Rebuild those original float world positions, then take their
				// distance in double so an offset pin cannot redefine the rest mesh.
				const glm::vec3 RestA = Record.Definition.Position +
					Record.Nodes[Constraint.VertexA].RestOffset;
				const glm::vec3 RestB = Record.Definition.Position +
					Record.Nodes[Constraint.VertexB].RestOffset;
				ShadowDistance.RestLength = glm::length(
					glm::dvec3(RestB) - glm::dvec3(RestA));
				ShadowDistance.Lambda = 0.0;
			}
		}

		void SyncDoubleShadowFromContact(SoftBodyRecord &Record) {
			if (!Record.UseDoubleShadow) return;
			for (std::size_t Index = 0; Index < Record.Nodes.size(); ++Index) {
				const auto &Node = Record.Nodes[Index];
				auto &Shadow = Record.DoubleShadowNodes[Index];
				Shadow.Position += glm::dvec3(Node.Position) -
					glm::dvec3(Shadow.PublishedPosition);
				Shadow.PreviousPosition += glm::dvec3(Node.PreviousPosition) -
					glm::dvec3(Shadow.PublishedPreviousPosition);
				Shadow.PublishedPosition = Node.Position;
				Shadow.PublishedPreviousPosition = Node.PreviousPosition;
			}
		}

		void SolveDistance(SoftBodyRecord &Record, float DeltaTime, bool RecordAccounting) {
			if (Record.UseDoubleShadow) {
				const double StepSeconds = double(DeltaTime);
				for (std::size_t Index = 0; Index < Record.Distances.size(); ++Index) {
					const auto &Constraint = Record.Distances[Index];
					auto &ShadowDistance = Record.DoubleShadowDistances[Index];
					auto &A = Record.DoubleShadowNodes[Constraint.VertexA];
					auto &B = Record.DoubleShadowNodes[Constraint.VertexB];
					const glm::dvec3 Delta = B.Position - A.Position;
					const double Length = glm::length(Delta);
					if (Length <= 1e-7) continue;
					const double InverseMassA = double(Record.Nodes[Constraint.VertexA].InverseMass);
					const double InverseMassB = double(Record.Nodes[Constraint.VertexB].InverseMass);
					const double Alpha = double(Constraint.Compliance) / (StepSeconds * StepSeconds);
					if (InverseMassA + InverseMassB + Alpha <= 1e-9) continue;
					const double DeltaLambda = (-(Length - ShadowDistance.RestLength) -
						Alpha * ShadowDistance.Lambda) / (InverseMassA + InverseMassB + Alpha);
					ShadowDistance.Lambda += DeltaLambda;
					const glm::dvec3 Correction = (Delta / Length) * DeltaLambda;
					A.Position -= InverseMassA * Correction;
					B.Position += InverseMassB * Correction;
					if (RecordAccounting) {
						if (InverseMassA == 0.0 && InverseMassB > 0.0)
							Record.ContactProjection.PinConstraintImpulseOnClothNs +=
								Correction / StepSeconds;
						else if (InverseMassB == 0.0 && InverseMassA > 0.0)
							Record.ContactProjection.PinConstraintImpulseOnClothNs -=
								Correction / StepSeconds;
					}
				}
				for (std::size_t Index = 0; Index < Record.Nodes.size(); ++Index) {
					auto &Node = Record.Nodes[Index];
					auto &Shadow = Record.DoubleShadowNodes[Index];
					Node.Position = glm::vec3(Shadow.Position);
					if (!IsFinite(Node.Position))
						throw std::runtime_error(
							"[Physics:XpbdContactProbe] nonfinite double-shadow position");
					Shadow.PublishedPosition = Node.Position;
				}
				return;
			}
			for (auto &Constraint : Record.Distances) {
				auto &NodeA = Record.Nodes[Constraint.VertexA];
				auto &NodeB = Record.Nodes[Constraint.VertexB];
				const auto Delta = NodeB.Position - NodeA.Position;
				const auto Length = glm::length(Delta);
				if (Length <= 1e-7f) continue;
				const auto Weight = NodeA.InverseMass + NodeB.InverseMass;
				const auto Alpha = Constraint.Compliance / (DeltaTime * DeltaTime);
				if (Weight + Alpha <= 1e-9f) continue;
				const auto DeltaLambda = (-(Length - Constraint.RestLength) - Alpha * Constraint.Lambda) /
										 (Weight + Alpha);
				Constraint.Lambda += DeltaLambda;
				const auto Correction = (Delta / Length) * DeltaLambda;
				const glm::vec3 BeforeA = NodeA.Position, BeforeB = NodeB.Position;
				NodeA.Position -= NodeA.InverseMass * Correction;
				NodeB.Position += NodeB.InverseMass * Correction;
				if (Record.ReconstructIdealVelocity) {
					const glm::dvec3 IdealCorrection(Correction);
					Record.IdealVelocitySnapshots[Constraint.VertexA].StructuralFloatBias +=
						-double(NodeA.InverseMass) * IdealCorrection -
						(glm::dvec3(NodeA.Position) - glm::dvec3(BeforeA));
					Record.IdealVelocitySnapshots[Constraint.VertexB].StructuralFloatBias +=
						double(NodeB.InverseMass) * IdealCorrection -
						(glm::dvec3(NodeB.Position) - glm::dvec3(BeforeB));
				}
				if (RecordAccounting) {
					if (NodeA.InverseMass == 0.0f && NodeB.InverseMass > 0.0f) {
						const glm::dvec3 Actual = glm::dvec3(NodeB.Position - BeforeB) /
							(double(NodeB.InverseMass) * double(DeltaTime));
						const glm::dvec3 Ideal = glm::dvec3(Correction) / double(DeltaTime);
						Record.ContactProjection.PinConstraintNumericalImpulseNs += Actual - Ideal;
						Record.ContactProjection.PinConstraintImpulseOnClothNs +=
							Record.ReconstructIdealVelocity ? Ideal : Actual;
					} else if (NodeB.InverseMass == 0.0f && NodeA.InverseMass > 0.0f) {
						const glm::dvec3 Actual = glm::dvec3(NodeA.Position - BeforeA) /
							(double(NodeA.InverseMass) * double(DeltaTime));
						const glm::dvec3 Ideal = -glm::dvec3(Correction) / double(DeltaTime);
						Record.ContactProjection.PinConstraintNumericalImpulseNs += Actual - Ideal;
						Record.ContactProjection.PinConstraintImpulseOnClothNs +=
							Record.ReconstructIdealVelocity ? Ideal : Actual;
					}
					else if (NodeA.InverseMass > 0.0f && NodeB.InverseMass > 0.0f)
						Record.ContactProjection.UnpinnedStructuralNumericalImpulseNs +=
							glm::dvec3(NodeA.Position - BeforeA) /
								(double(NodeA.InverseMass) * double(DeltaTime)) +
							glm::dvec3(NodeB.Position - BeforeB) /
								(double(NodeB.InverseMass) * double(DeltaTime));
				}
			}
		}

		void SolveVolume(SoftBodyRecord &Record, float DeltaTime) {
			if (Record.Definition.Kind != SoftBodyKind::Rubber) return;
			const auto Alpha = Record.Definition.Material.VolumeCompliance / (DeltaTime * DeltaTime);
			for (auto &Constraint : Record.Volumes) {
				auto &A = Record.Nodes[Constraint.Vertices[0]];
				auto &B = Record.Nodes[Constraint.Vertices[1]];
				auto &C = Record.Nodes[Constraint.Vertices[2]];
				auto &D = Record.Nodes[Constraint.Vertices[3]];
				const auto GradientB = glm::cross(C.Position - A.Position, D.Position - A.Position) / 6.0f;
				const auto GradientC = glm::cross(D.Position - A.Position, B.Position - A.Position) / 6.0f;
				const auto GradientD = glm::cross(B.Position - A.Position, C.Position - A.Position) / 6.0f;
				const auto GradientA = -GradientB - GradientC - GradientD;
				const auto Weight = A.InverseMass * glm::dot(GradientA, GradientA) +
									B.InverseMass * glm::dot(GradientB, GradientB) +
									C.InverseMass * glm::dot(GradientC, GradientC) +
									D.InverseMass * glm::dot(GradientD, GradientD);
				if (Weight + Alpha <= 1e-9f) continue;
				const auto Volume =
					glm::dot(B.Position - A.Position, glm::cross(C.Position - A.Position, D.Position - A.Position)) /
					6.0f;
				const auto DeltaLambda = (-(Volume - Constraint.RestVolume) - Alpha * Constraint.Lambda) /
										 (Weight + Alpha);
				Constraint.Lambda += DeltaLambda;
				A.Position += A.InverseMass * GradientA * DeltaLambda;
				B.Position += B.InverseMass * GradientB * DeltaLambda;
				C.Position += C.InverseMass * GradientC * DeltaLambda;
				D.Position += D.InverseMass * GradientD * DeltaLambda;
			}
		}

		[[nodiscard]] glm::mat3 BestFitRotation(const SoftBodyRecord &Record, const glm::vec3 &Centroid) {
			glm::vec3 RestCentroid{0.0f};
			for (const auto &Node : Record.Nodes)
				RestCentroid += Node.RestOffset;
			RestCentroid /= static_cast<float>(Record.Nodes.size());
			glm::mat3 Covariance{0.0f};
			for (const auto &Node : Record.Nodes)
				Covariance += glm::outerProduct(Node.Position - Centroid, Node.RestOffset - RestCentroid);
			glm::quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
			for (int Iteration = 0; Iteration < 12; ++Iteration) {
				const auto Matrix = glm::mat3_cast(Rotation);
				const auto Numerator = glm::cross(Matrix[0], Covariance[0]) + glm::cross(Matrix[1], Covariance[1]) +
									   glm::cross(Matrix[2], Covariance[2]);
				const auto Denominator = std::abs(
											 glm::dot(Matrix[0], Covariance[0]) + glm::dot(Matrix[1], Covariance[1]) +
											 glm::dot(Matrix[2], Covariance[2])
										 ) +
										 1e-9f;
				const auto Omega = Numerator / Denominator;
				const auto Magnitude = glm::length(Omega);
				if (!IsFinite(Magnitude) || Magnitude < 1e-6f) break;
				Rotation = glm::normalize(glm::angleAxis(Magnitude, Omega / Magnitude) * Rotation);
			}
			const auto Result = glm::mat3_cast(Rotation);
			return IsFinite(Result) ? Result : glm::mat3(1.0f);
		}

		void SolveShapeRecovery(SoftBodyRecord &Record, float DeltaTime) {
			if (Record.Definition.Kind != SoftBodyKind::Rubber || Record.Nodes.empty()) return;
			glm::vec3 Centroid{0.0f};
			for (const auto &Node : Record.Nodes)
				Centroid += Node.Position;
			Centroid /= static_cast<float>(Record.Nodes.size());
			const auto Rotation = BestFitRotation(Record, Centroid);
			const auto Compliance = Record.Definition.Material.ShapeCompliance / (DeltaTime * DeltaTime);
			const auto Factor = 1.0f / (1.0f + Compliance);
			for (auto &Node : Record.Nodes) {
				if (Node.InverseMass == 0.0f) continue;
				Node.Position += (Centroid + Rotation * Node.RestOffset - Node.Position) * Factor;
			}
		}

		void ApplyAttachments(SoftBodyRecord &Record) {
			for (const auto &Attachment : Record.Definition.Attachments) {
				auto &Node = Record.Nodes[Attachment.Vertex];
				Node.Position = Attachment.Position;
				Node.Velocity = {};
			}
		}

		[[nodiscard]] std::optional<glm::vec3> CollideBall(glm::vec3 &Local, const glm::vec3 &Half, float Thickness) {
			const auto Radius = std::min({Half.x, Half.y, Half.z}) + Thickness;
			const auto Distance = glm::length(Local);
			if (Distance >= Radius) return std::nullopt;
			const auto Normal = Distance > 1e-7f ? Local / Distance : glm::vec3(0.0f, 1.0f, 0.0f);
			Local += Normal * (Radius - Distance);
			return Normal;
		}

		[[nodiscard]] std::optional<glm::vec3> CollideBox(glm::vec3 &Local, const glm::vec3 &Half, float Thickness) {
			const auto Closest = glm::clamp(Local, -Half, Half);
			const auto Delta = Local - Closest;
			const auto Distance = glm::length(Delta);
			const bool Inside = glm::all(glm::lessThanEqual(glm::abs(Local), Half));
			if (!Inside && Distance >= Thickness) return std::nullopt;
			glm::vec3 Normal{0.0f};
			float Push = 0.0f;
			if (!Inside && Distance > 1e-7f) {
				Normal = Delta / Distance;
				Push = Thickness - Distance;
			} else {
				const auto Penetration = Half - glm::abs(Local);
				int Axis = Penetration.y < Penetration.x ? 1 : 0;
				if (Penetration.z < Penetration[Axis]) Axis = 2;
				Normal[Axis] = Local[Axis] < 0.0f ? -1.0f : 1.0f;
				Push = Penetration[Axis] + Thickness;
			}
			Local += Normal * Push;
			return Normal;
		}

		[[nodiscard]] std::optional<glm::vec3>
		CollideCylinder(glm::vec3 &Local, const glm::vec3 &Half, float Thickness) {
			const auto Radius = std::min(Half.x, Half.z);
			const auto Radial = std::sqrt(Local.x * Local.x + Local.z * Local.z);
			const auto Scale = Radial > Radius && Radial > 1e-7f ? Radius / Radial : 1.0f;
			const glm::vec3 Closest{Local.x * Scale, std::clamp(Local.y, -Half.y, Half.y), Local.z * Scale};
			const auto Delta = Local - Closest;
			const auto Distance = glm::length(Delta);
			const bool Inside = Radial <= Radius && std::abs(Local.y) <= Half.y;
			if (!Inside && Distance >= Thickness) return std::nullopt;
			glm::vec3 Normal{0.0f};
			float Push = 0.0f;
			if (!Inside && Distance > 1e-7f) {
				Normal = Delta / Distance;
				Push = Thickness - Distance;
			} else {
				const auto SidePenetration = Radius - Radial;
				const auto CapPenetration = Half.y - std::abs(Local.y);
				if (CapPenetration < SidePenetration)
					Normal.y = Local.y < 0.0f ? -1.0f : 1.0f;
				else if (Radial > 1e-7f)
					Normal = {Local.x / Radial, 0.0f, Local.z / Radial};
				else
					Normal = {1.0f, 0.0f, 0.0f};
				Push = std::min(SidePenetration, CapPenetration) + Thickness;
			}
			Local += Normal * Push;
			return Normal;
		}

		struct ConvexPlane {
			glm::vec3 Normal{0.0f};
			float Offset = 0.0f;
		};

		[[nodiscard]] ConvexPlane MakePlane(glm::vec3 Normal, float Offset) {
			const auto Length = glm::length(Normal);
			return {Normal / Length, Offset / Length};
		}

		[[nodiscard]] std::optional<glm::vec3>
		CollideConvex(glm::vec3 &Local, const glm::vec3 &Half, PhysicsShapeKind Kind, float Thickness) {
			std::array<ConvexPlane, 5> Planes;
			if (Kind == PhysicsShapeKind::Wedge) {
				Planes = {
					MakePlane({1.0f, 0.0f, 0.0f}, Half.x),
					MakePlane({-1.0f, 0.0f, 0.0f}, Half.x),
					MakePlane({0.0f, -1.0f, 0.0f}, Half.y),
					MakePlane({0.0f, 0.0f, -1.0f}, Half.z),
					MakePlane({0.0f, 1.0f / Half.y, 1.0f / Half.z}, 0.0f),
				};
			} else {
				Planes = {
					MakePlane({-1.0f, 0.0f, 0.0f}, Half.x),
					MakePlane({0.0f, -1.0f, 0.0f}, Half.y),
					MakePlane({0.0f, 0.0f, -1.0f}, Half.z),
					MakePlane({1.0f / Half.x, 1.0f / Half.y, 0.0f}, 0.0f),
					MakePlane({0.0f, 1.0f / Half.y, 1.0f / Half.z}, 0.0f),
				};
			}
			float MaximumDistance = -std::numeric_limits<float>::infinity();
			glm::vec3 Normal{0.0f};
			for (const auto &Plane : Planes) {
				const auto Distance = glm::dot(Plane.Normal, Local) - Plane.Offset;
				if (Distance > MaximumDistance) {
					MaximumDistance = Distance;
					Normal = Plane.Normal;
				}
			}
			if (MaximumDistance >= Thickness) return std::nullopt;
			Local += Normal * (Thickness - MaximumDistance);
			return Normal;
		}

		[[nodiscard]] std::optional<glm::vec3>
		CollideNode(SoftBodyNode &Node, const SoftBodyCollider &Collider, float Thickness) {
			const auto InverseRotation = glm::transpose(Collider.Transform.Rotation);
			auto Local = InverseRotation * (Node.Position - Collider.Transform.Position);
			const auto Half = glm::abs(Collider.Shape.Size) * 0.5f;
			std::optional<glm::vec3> LocalNormal;
			switch (Collider.Shape.Kind) {
			case PhysicsShapeKind::Ball:
				LocalNormal = CollideBall(Local, Half, Thickness);
				break;
			case PhysicsShapeKind::Box:
				LocalNormal = CollideBox(Local, Half, Thickness);
				break;
			case PhysicsShapeKind::Cylinder:
				LocalNormal = CollideCylinder(Local, Half, Thickness);
				break;
			case PhysicsShapeKind::Wedge:
			case PhysicsShapeKind::CornerWedge:
				LocalNormal = CollideConvex(Local, Half, Collider.Shape.Kind, Thickness);
				break;
			}
			if (!LocalNormal) return std::nullopt;
			Node.Position = Collider.Transform.Position + Collider.Transform.Rotation * Local;
			return Collider.Transform.Rotation * *LocalNormal;
		}

		void Collide(SoftBodyWorkItem &Item, const SoftBodySimulationBatch &Batch, bool ApplyFriction,
			std::vector<float> *NormalLambdas = nullptr,
			const std::vector<glm::vec3> *ColliderDeltas = nullptr,
			bool RelativeKinematic = false) {
			auto &Record = Item.Record;
			if (Record.Definition.CollisionMode == SoftBodyCollisionMode::None || Batch.Colliders.empty()) return;
			const auto QueryStart = ProfileClock::now();
			const auto Candidates = Batch.Broadphase->Query(BodyBounds(Record), Batch.BroadphaseMode);
			Item.Profile.BroadphaseQueryNanoseconds += Nanoseconds(ProfileClock::now() - QueryStart);
			++Item.Profile.ColliderQueries;
			Item.Profile.CandidateColliders += Candidates.size();
			const auto CollisionStart = ProfileClock::now();
			const float NormalAlpha = NormalLambdas ?
				GetCoupledNormalCompliance(Record.Definition.Material.StretchCompliance) /
					(Batch.DeltaTime * Batch.DeltaTime) : 0.0f;
			for (std::size_t NodeIndex = 0; NodeIndex < Record.Nodes.size(); ++NodeIndex) {
				auto &Node = Record.Nodes[NodeIndex];
				if (Node.InverseMass == 0.0f) continue;
				for (const auto ColliderIndex : Candidates) {
					const glm::vec3 BeforePosition = Node.Position;
					const glm::vec3 BeforePrevious = Node.PreviousPosition;
					const auto &Collider = Batch.Colliders[ColliderIndex];
					const glm::vec3 ColliderDelta = ColliderDeltas &&
						ColliderIndex < ColliderDeltas->size() &&
						Collider.Shape.Kind == PhysicsShapeKind::Ball ?
						(*ColliderDeltas)[ColliderIndex] : glm::vec3(0.0f);
					std::optional<glm::vec3> Normal;
					if (NormalLambdas && Collider.Shape.Kind == PhysicsShapeKind::Ball) {
						const glm::vec3 Delta = Node.Position - Collider.Transform.Position;
						const float Distance = glm::length(Delta);
						const float Radius = std::min({Collider.Shape.Size.x, Collider.Shape.Size.y,
							Collider.Shape.Size.z}) * 0.5f + Record.Definition.Material.Thickness;
						float &Lambda = (*NormalLambdas)[NodeIndex * Batch.Colliders.size() + ColliderIndex];
						if (Distance < Radius || Lambda > 0.0f) {
							const glm::vec3 Direction = Distance > 1e-7f ? Delta / Distance : glm::vec3(0,1,0);
							const float NewLambda = std::max(0.0f, Lambda +
								(Radius - Distance - NormalAlpha * Lambda) / (Node.InverseMass + NormalAlpha));
							const float DeltaLambda = NewLambda - Lambda;
							Lambda = NewLambda;
							if (DeltaLambda != 0.0f)
								Node.Position += Node.InverseMass * DeltaLambda * Direction;
							if (DeltaLambda != 0.0f || Lambda > 0.0f)
								Normal = Direction;
						}
					} else {
						Normal = CollideNode(Node, Collider, Record.Definition.Material.Thickness);
					}
					if (!Normal) continue;
					if (ApplyFriction) {
						glm::vec3 RetainedMovement;
						if (RelativeKinematic)
							RetainedMovement = PhysicsEvaluation::ResolveExperimentalXpbdRelativeMovement(
								Node.Position - Node.PreviousPosition, ColliderDelta, *Normal,
								Record.Definition.Material.Friction);
						else {
							const auto Movement = Node.Position - Node.PreviousPosition;
							const auto NormalMovement = glm::dot(Movement, *Normal);
							const auto TangentialMovement = Movement - NormalMovement * *Normal;
							RetainedMovement = std::max(NormalMovement, 0.0f) * *Normal +
								TangentialMovement * (1.0f - Record.Definition.Material.Friction);
						}
						Node.PreviousPosition = Node.Position - RetainedMovement;
					}
					RecordContactProjection(Record, Node, BeforePosition, BeforePrevious,
						*Normal, ColliderDelta, Batch.DeltaTime);
				}
			}
			Item.Profile.CollisionNanoseconds += Nanoseconds(ProfileClock::now() - CollisionStart);
		}

		[[nodiscard]] glm::vec3 ClosestTriangleWeights(glm::vec3 Point,
			glm::vec3 A, glm::vec3 B, glm::vec3 C) {
			const glm::vec3 AB = B - A, AC = C - A, AP = Point - A;
			const float AreaSquared = glm::dot(glm::cross(AB, AC), glm::cross(AB, AC));
			if (AreaSquared <= 1e-12f) {
				// Severe contact can collapse a face. Use its longest surviving
				// segment rather than a zero-area barycentric division.
				const glm::vec3 BC = C - B;
				const float AB2 = glm::dot(AB, AB), AC2 = glm::dot(AC, AC);
				const float BC2 = glm::dot(BC, BC);
				if (AB2 >= AC2 && AB2 >= BC2 && AB2 > 1e-12f) {
					const float T = std::clamp(glm::dot(AP, AB) / AB2, 0.0f, 1.0f);
					return {1 - T,T,0};
				}
				if (AC2 >= BC2 && AC2 > 1e-12f) {
					const float T = std::clamp(glm::dot(AP, AC) / AC2, 0.0f, 1.0f);
					return {1 - T,0,T};
				}
				if (BC2 > 1e-12f) {
					const float T = std::clamp(glm::dot(Point - B, BC) / BC2, 0.0f, 1.0f);
					return {0,1 - T,T};
				}
				return {1,0,0};
			}
			const float D1 = glm::dot(AB, AP), D2 = glm::dot(AC, AP);
			if (D1 <= 0 && D2 <= 0) return {1,0,0};
			const glm::vec3 BP = Point - B;
			const float D3 = glm::dot(AB, BP), D4 = glm::dot(AC, BP);
			if (D3 >= 0 && D4 <= D3) return {0,1,0};
			const float VC = D1 * D4 - D3 * D2;
			if (VC <= 0 && D1 >= 0 && D3 <= 0) {
				const float V = D1 / (D1 - D3);
				return {1 - V,V,0};
			}
			const glm::vec3 CP = Point - C;
			const float D5 = glm::dot(AB, CP), D6 = glm::dot(AC, CP);
			if (D6 >= 0 && D5 <= D6) return {0,0,1};
			const float VB = D5 * D2 - D1 * D6;
			if (VB <= 0 && D2 >= 0 && D6 <= 0) {
				const float W = D2 / (D2 - D6);
				return {1 - W,0,W};
			}
			const float VA = D3 * D6 - D5 * D4;
			if (VA <= 0 && D4 >= D3 && D5 >= D6) {
				const float W = (D4 - D3) / ((D4 - D3) + (D5 - D6));
				return {0,1 - W,W};
			}
			const float Denominator = VA + VB + VC;
			if (std::abs(Denominator) <= 1e-12f) return {1,0,0};
			const float V = VB / Denominator, W = VC / Denominator;
			return {1 - V - W,V,W};
		}

		// Private E geometry probe. A face can enter a sphere while all three
		// vertices remain outside; the ordinary vertex-only solver cannot see it.
		void CollideFaces(SoftBodyWorkItem &Item, const SoftBodySimulationBatch &Batch,
			const std::vector<glm::vec3> *ColliderDeltas) {
			auto &Record = Item.Record;
			if (Record.Definition.CollisionMode == SoftBodyCollisionMode::None ||
				!Record.Indices || Batch.Colliders.empty()) return;
			const auto Start = ProfileClock::now();
			const auto &Indices = *Record.Indices;
			for (std::size_t ColliderIndex = 0; ColliderIndex < Batch.Colliders.size(); ++ColliderIndex) {
				const auto &Collider = Batch.Colliders[ColliderIndex];
				if (Collider.Shape.Kind != PhysicsShapeKind::Ball) continue;
				const glm::vec3 SurfaceDelta = ColliderDeltas && ColliderIndex < ColliderDeltas->size() ?
					(*ColliderDeltas)[ColliderIndex] : glm::vec3(0.0f);
				const float Radius = std::min({Collider.Shape.Size.x, Collider.Shape.Size.y,
					Collider.Shape.Size.z}) * 0.5f + Record.Definition.Material.Thickness;
				const glm::vec3 Center = Collider.Transform.Position;
				for (std::size_t Face = 0; Face + 2 < Indices.size(); Face += 3) {
					auto &A = Record.Nodes[Indices[Face]];
					auto &B = Record.Nodes[Indices[Face + 1]];
					auto &C = Record.Nodes[Indices[Face + 2]];
					const glm::vec3 Weights = ClosestTriangleWeights(Center,
						A.Position, B.Position, C.Position);
					const glm::vec3 Closest = Weights.x * A.Position +
						Weights.y * B.Position + Weights.z * C.Position;
					const glm::vec3 Delta = Closest - Center;
					const float Distance = glm::length(Delta);
					if (Distance >= Radius) continue;
					glm::vec3 Normal;
					if (Distance > 1e-7f) Normal = Delta / Distance;
					else {
						glm::vec3 FaceNormal = glm::cross(B.Position - A.Position,
							C.Position - A.Position);
						const float FaceLength = glm::length(FaceNormal);
						if (FaceLength <= 1e-7f)
							FaceNormal = glm::cross(B.RestOffset - A.RestOffset,
								C.RestOffset - A.RestOffset);
						const float FallbackLength = glm::length(FaceNormal);
						Normal = FallbackLength > 1e-7f ? FaceNormal / FallbackLength : glm::vec3(0,0,1);
					}
					const float Weight = A.InverseMass * Weights.x * Weights.x +
						B.InverseMass * Weights.y * Weights.y +
						C.InverseMass * Weights.z * Weights.z;
					if (Weight <= 1e-9f) continue;
					const glm::vec3 Correction = ((Radius - Distance) / Weight) * Normal;
					const glm::vec3 BeforeA = A.Position, BeforeB = B.Position, BeforeC = C.Position;
					A.Position += A.InverseMass * Weights.x * Correction;
					B.Position += B.InverseMass * Weights.y * Correction;
					C.Position += C.InverseMass * Weights.z * Correction;
					RecordContactProjection(Record, A, BeforeA, A.PreviousPosition,
						Normal, SurfaceDelta, Batch.DeltaTime);
					RecordContactProjection(Record, B, BeforeB, B.PreviousPosition,
						Normal, SurfaceDelta, Batch.DeltaTime);
					RecordContactProjection(Record, C, BeforeC, C.PreviousPosition,
						Normal, SurfaceDelta, Batch.DeltaTime);
				}
			}
			Item.Profile.CollisionNanoseconds += Nanoseconds(ProfileClock::now() - Start);
		}

		void ApplyPointImpulse(SoftBodyRecord &Record, const SoftBodyPointImpulse &PointImpulse) {
			std::array<std::size_t, 4> Closest{};
			std::array<float, 4> Distances;
			Distances.fill(std::numeric_limits<float>::infinity());
			for (std::size_t NodeIndex = 0; NodeIndex < Record.Nodes.size(); ++NodeIndex) {
				if (Record.Nodes[NodeIndex].InverseMass == 0.0f) continue;
				const auto Delta = Record.Nodes[NodeIndex].Position - PointImpulse.Position;
				const auto Distance = glm::dot(Delta, Delta);
				for (std::size_t Candidate = 0; Candidate < Closest.size(); ++Candidate) {
					if (Distance >= Distances[Candidate]) continue;
					for (std::size_t Shift = Closest.size() - 1; Shift > Candidate; --Shift) {
						Distances[Shift] = Distances[Shift - 1];
						Closest[Shift] = Closest[Shift - 1];
					}
					Distances[Candidate] = Distance;
					Closest[Candidate] = NodeIndex;
					break;
				}
			}
			float TotalWeight = 0.0f;
			std::array<float, 4> Weights{};
			for (std::size_t Index = 0; Index < Closest.size(); ++Index) {
				if (!std::isfinite(Distances[Index])) continue;
				Weights[Index] = 1.0f / std::max(std::sqrt(Distances[Index]), 0.001f);
				TotalWeight += Weights[Index];
			}
			if (TotalWeight == 0.0f) return;
			for (std::size_t Index = 0; Index < Closest.size(); ++Index) {
				if (Weights[Index] == 0.0f) continue;
				auto &Node = Record.Nodes[Closest[Index]];
				Node.Velocity += PointImpulse.Impulse * (Weights[Index] / TotalWeight) * Node.InverseMass;
			}
		}

		void SolveWorkItem(SoftBodyWorkItem &Item, const SoftBodySimulationBatch &Batch) {
			const auto WorkerStart = ProfileClock::now();
			auto &Record = Item.Record;
			Record.ContactProjection = {};
			const bool RecordAccounting =
				PhysicsEvaluation::ExperimentalXpbdAccountingEnabled.load(std::memory_order_acquire);
			Record.CaptureAccounting = RecordAccounting;
			const int VelocityPrecisionMode = GetVelocityPrecisionMode();
			const bool ReconstructIdealVelocity = VelocityPrecisionMode == 1;
			const bool UseDoubleShadow = VelocityPrecisionMode == 2;
			if (VelocityPrecisionMode != 0 && Record.Definition.Kind != SoftBodyKind::Cloth)
				throw std::invalid_argument(
					"[Physics:XpbdContactProbe] precision mode requires cloth");
			Record.ReconstructIdealVelocity = ReconstructIdealVelocity;
			Record.UseDoubleShadow = UseDoubleShadow;
			if (UseDoubleShadow) {
				InitializeDoubleShadow(Record);
				SyncDoubleShadowFromContact(Record);
			}
			if (RecordAccounting)
				Record.ContactProjection.KineticStartJ = StoredKineticEnergy(Record);
			if (RecordAccounting)
				Record.ContactProjection.MomentumStartKgMPerS = StoredMomentum(Record);
			static const unsigned long StageStride = [] {
				const char *Text = std::getenv("GANTRIA_XPBD_STAGE_STRIDE");
				if (!Text) return 0ul;
				char *End = nullptr;
				const auto Parsed = std::strtoul(Text, &End, 10);
				if (!End || *End || Parsed < 1 || Parsed > 600)
					throw std::invalid_argument("[Physics:XpbdContactProbe] invalid stage stride");
				return Parsed;
			}();
			static std::atomic<std::uint64_t> StageSteps{0};
			const std::uint64_t StageStep = StageStride ? StageSteps.fetch_add(1) : 0;
			const bool StageSample = StageStride && (StageStep + 1) % StageStride == 0;
			const auto LogStage = [&](int Iteration, const char *Phase) {
				if (!StageSample) return;
				if (Item.Body.Slot != 1 || Record.Definition.Kind != SoftBodyKind::Cloth)
					throw std::runtime_error("[Physics:XpbdContactProbe] stage trace needs one cloth body");
				const auto XCount = Record.Definition.ResolutionX;
				const auto YCount = Record.Definition.ResolutionY;
				float MaxTension = 0.0f, MaxCompression = 0.0f;
				float MaxPenetration = 0.0f, MaxAbsZ = 0.0f;
				for (std::uint32_t Y = 0; Y < YCount; ++Y)
					for (std::uint32_t X = 0; X < XCount; ++X) {
						const auto Index = std::size_t(Y) * XCount + X;
						const auto &Node = Record.Nodes[Index];
						MaxAbsZ = std::max(MaxAbsZ, std::abs(Node.Position.z));
						const auto Edge = [&](std::size_t Next) {
							const float Rest = glm::length(Record.Nodes[Next].RestOffset - Node.RestOffset);
							if (Rest <= 0) return;
							const float Strain = glm::length(Record.Nodes[Next].Position - Node.Position) / Rest - 1.0f;
							MaxTension = std::max(MaxTension, Strain);
							MaxCompression = std::min(MaxCompression, Strain);
						};
						if (X + 1 < XCount) Edge(Index + 1);
						if (Y + 1 < YCount) Edge(Index + XCount);
						for (const auto &Collider : Batch.Colliders) {
							if (Collider.Shape.Kind != PhysicsShapeKind::Ball) continue;
							const float Radius = std::min({Collider.Shape.Size.x, Collider.Shape.Size.y,
								Collider.Shape.Size.z}) * 0.5f + Record.Definition.Material.Thickness;
							MaxPenetration = std::max(MaxPenetration,
								Radius - glm::length(Node.Position - Collider.Transform.Position));
						}
					}
				float ReferenceEdgeStrain = 0.0f;
				if (XCount == 128 && YCount == 128) {
					const auto A = std::size_t(50) * XCount + 126, B = A + 1;
					const float Rest = glm::length(Record.Nodes[B].RestOffset - Record.Nodes[A].RestOffset);
					ReferenceEdgeStrain = glm::length(Record.Nodes[B].Position - Record.Nodes[A].Position) / Rest - 1.0f;
				}
				std::fprintf(stdout,
					"[Physics:XpbdContactStage] substep=%llu iteration=%d phase=%s tension=%.9g compression=%.9g penetration_m=%.9g max_abs_z_m=%.9g edge_126_50=%.9g\n",
					static_cast<unsigned long long>(StageStep), Iteration, Phase, MaxTension,
					MaxCompression, MaxPenetration, MaxAbsZ, ReferenceEdgeStrain);
			};
			const auto IntegrationStart = ProfileClock::now();
			std::size_t MovableCount = 0;
			for (const auto &Node : Record.Nodes)
				if (Node.InverseMass > 0.0f) ++MovableCount;
			const auto TotalMass = static_cast<float>(MovableCount) * Record.Definition.Material.ParticleMass;
			const auto Damping = std::clamp(1.0f - Record.Definition.Material.Damping, 0.0f, 1.0f);
			for (std::size_t Index = 0; Index < Record.Nodes.size(); ++Index) {
				auto &Node = Record.Nodes[Index];
				Node.PreviousPosition = Node.Position;
				if (UseDoubleShadow) {
					auto &Shadow = Record.DoubleShadowNodes[Index];
					Shadow.PreviousPosition = Shadow.Position;
					Shadow.PublishedPreviousPosition = Node.PreviousPosition;
				}
				if (Node.InverseMass == 0.0f || MovableCount == 0) continue;
				const double BeforeForceJ = RecordAccounting ? NodeKineticEnergy(Node) : 0.0;
				const glm::dvec3 BeforeForceMomentum = RecordAccounting ? NodeMomentum(Node) : glm::dvec3(0.0);
				Node.Velocity += Batch.Gravity * Batch.DeltaTime;
				Node.Velocity += Item.Force / TotalMass * Batch.DeltaTime;
				const double BeforeImpulseJ = RecordAccounting ? NodeKineticEnergy(Node) : 0.0;
				const glm::dvec3 BeforeImpulseMomentum = RecordAccounting ? NodeMomentum(Node) : glm::dvec3(0.0);
				Node.Velocity += Item.Impulse / TotalMass;
				if (RecordAccounting) {
					Record.ContactProjection.GravityForceImpulseNs +=
						BeforeImpulseMomentum - BeforeForceMomentum;
					Record.ContactProjection.UniformImpulseNs +=
						NodeMomentum(Node) - BeforeImpulseMomentum;
					Record.ContactProjection.GravityForceKineticDeltaJ += BeforeImpulseJ - BeforeForceJ;
					Record.ContactProjection.UniformImpulseKineticDeltaJ +=
						NodeKineticEnergy(Node) - BeforeImpulseJ;
				}
			}
			const double BeforePointImpulsesJ = RecordAccounting ? StoredKineticEnergy(Record) : 0.0;
			const glm::dvec3 BeforePointImpulseMomentum = RecordAccounting ?
				StoredMomentum(Record) : glm::dvec3(0.0);
			for (const auto &PointImpulse : Item.PointImpulses)
				ApplyPointImpulse(Record, PointImpulse);
			const double BeforeDampingJ = RecordAccounting ? StoredKineticEnergy(Record) : 0.0;
			if (RecordAccounting)
				Record.ContactProjection.PointImpulseKineticDeltaJ =
					BeforeDampingJ - BeforePointImpulsesJ;
			if (RecordAccounting)
				Record.ContactProjection.PointImpulseNs =
					StoredMomentum(Record) - BeforePointImpulseMomentum;
			const glm::dvec3 BeforeDampingMomentum = RecordAccounting ?
				StoredMomentum(Record) : glm::dvec3(0.0);
			if (RecordAccounting)
				Record.ContactProjection.AppliedDynamicsKineticDeltaJ =
					BeforeDampingJ - Record.ContactProjection.KineticStartJ;
			for (std::size_t Index = 0; Index < Record.Nodes.size(); ++Index) {
				auto &Node = Record.Nodes[Index];
				if (Node.InverseMass == 0.0f) continue;
				Node.Velocity *= Damping;
				if (UseDoubleShadow) {
					auto &Shadow = Record.DoubleShadowNodes[Index];
					Shadow.Position += glm::dvec3(Node.Velocity) * double(Batch.DeltaTime);
					Node.Position = glm::vec3(Shadow.Position);
					if (!IsFinite(Node.Position))
						throw std::runtime_error(
							"[Physics:XpbdContactProbe] nonfinite integrated shadow position");
					Shadow.PublishedPosition = Node.Position;
				} else {
					Node.Position += Node.Velocity * Batch.DeltaTime;
				}
			}
			if (RecordAccounting)
				Record.ContactProjection.DampingKineticDeltaJ =
					StoredKineticEnergy(Record) - BeforeDampingJ;
			if (RecordAccounting)
				Record.ContactProjection.DampingImpulseNs =
					StoredMomentum(Record) - BeforeDampingMomentum;
			if (ReconstructIdealVelocity) {
				Record.IdealVelocitySnapshots.resize(Record.Nodes.size());
				for (std::size_t Index = 0; Index < Record.Nodes.size(); ++Index) {
					const auto &Node = Record.Nodes[Index];
					auto &Snapshot = Record.IdealVelocitySnapshots[Index];
					Snapshot.IntegratedPosition = Node.Position;
					Snapshot.InitialPreviousPosition = Node.PreviousPosition;
					Snapshot.StructuralFloatBias = glm::dvec3(0.0);
				}
			}
			if (RecordAccounting && (ReconstructIdealVelocity || UseDoubleShadow))
				Record.ContactProjection.IntegrationPositionRoundingKineticDeltaJ =
					PositionKineticEnergy(Record, Batch.DeltaTime) - StoredKineticEnergy(Record);
			Item.Profile.IntegrationNanoseconds += Nanoseconds(ProfileClock::now() - IntegrationStart);

			for (auto &Constraint : Record.Distances)
				Constraint.Lambda = 0.0f;
			if (UseDoubleShadow)
				for (auto &ShadowDistance : Record.DoubleShadowDistances)
					ShadowDistance.Lambda = 0.0;
			for (auto &Constraint : Record.Volumes)
				Constraint.Lambda = 0.0f;
			const auto Iterations = IterationCount(Record);
			const auto ContactOrder = GetExperimentalContactOrder();
			std::vector<glm::vec3> ColliderDeltas;
			{
				std::scoped_lock Lock(PhysicsEvaluation::ExperimentalXpbdColliderMotionMutex);
				ColliderDeltas = PhysicsEvaluation::ExperimentalXpbdColliderDeltas;
			}
			if (ContactOrder == ExperimentalContactOrder::KinematicRelative &&
				ColliderDeltas.size() != Batch.Colliders.size())
				throw std::invalid_argument("[Physics:XpbdContactProbe] G requires one prescribed delta per collider");
			const auto *SourceDeltas = ColliderDeltas.size() == Batch.Colliders.size() ?
				&ColliderDeltas : nullptr;
			std::vector<float> ContactNormalLambdas;
			if (ContactOrder == ExperimentalContactOrder::CoupledNormal)
				ContactNormalLambdas.resize(Record.Nodes.size() * Batch.Colliders.size(), 0.0f);
			LogStage(-1, "BeforeSolve");
			for (int Iteration = 0; Iteration < Iterations; ++Iteration) {
				if (ContactOrder == ExperimentalContactOrder::ContactThenStructure ||
					ContactOrder == ExperimentalContactOrder::ContactStructureContact ||
					ContactOrder == ExperimentalContactOrder::CoupledNormal) {
					Collide(Item, Batch,
						ContactOrder == ExperimentalContactOrder::ContactThenStructure &&
						Iteration + 1 == Iterations,
						ContactOrder == ExperimentalContactOrder::CoupledNormal ? &ContactNormalLambdas : nullptr,
						SourceDeltas);
					if (UseDoubleShadow) SyncDoubleShadowFromContact(Record);
					LogStage(Iteration, "AfterPreContact");
				}
				const auto ConstraintStart = ProfileClock::now();
				const double BeforeStructureJ = RecordAccounting ?
					PositionKineticEnergy(Record, Batch.DeltaTime) : 0.0;
				SolveDistance(Record, Batch.DeltaTime, RecordAccounting);
				SolveVolume(Record, Batch.DeltaTime);
				SolveShapeRecovery(Record, Batch.DeltaTime);
				if (RecordAccounting)
					Record.ContactProjection.StructuralProjectionKineticDeltaJ +=
						PositionKineticEnergy(Record, Batch.DeltaTime) - BeforeStructureJ;
				Item.Profile.ConstraintNanoseconds += Nanoseconds(ProfileClock::now() - ConstraintStart);
				LogStage(Iteration, "AfterStructure");
				if (ContactOrder != ExperimentalContactOrder::ContactThenStructure) {
					Collide(Item, Batch, Iteration + 1 == Iterations,
						ContactOrder == ExperimentalContactOrder::CoupledNormal ? &ContactNormalLambdas : nullptr,
						SourceDeltas,
						ContactOrder == ExperimentalContactOrder::KinematicRelative);
					if (ContactOrder == ExperimentalContactOrder::TriangleSurface)
						CollideFaces(Item, Batch, SourceDeltas);
					if (UseDoubleShadow) SyncDoubleShadowFromContact(Record);
					LogStage(Iteration, "AfterPostContact");
				}
				const double BeforePinsJ = RecordAccounting ?
					PositionKineticEnergy(Record, Batch.DeltaTime) : 0.0;
				ApplyAttachments(Record);
				if (UseDoubleShadow) SyncDoubleShadowFromContact(Record);
				if (RecordAccounting)
					Record.ContactProjection.PinProjectionKineticDeltaJ +=
						PositionKineticEnergy(Record, Batch.DeltaTime) - BeforePinsJ;
			}
			LogStage(Iterations, "AfterSolve");
			const double BeforeReconstructionJ = RecordAccounting &&
				(ReconstructIdealVelocity || UseDoubleShadow) ?
				PositionKineticEnergy(Record, Batch.DeltaTime) : 0.0;
			for (std::size_t Index = 0; Index < Record.Nodes.size(); ++Index) {
				auto &Node = Record.Nodes[Index];
				if (Node.InverseMass == 0.0f) {
					Node.Velocity = glm::vec3{};
				} else if (UseDoubleShadow) {
					const auto &Shadow = Record.DoubleShadowNodes[Index];
					Node.Velocity = glm::vec3((Shadow.Position - Shadow.PreviousPosition) /
						double(Batch.DeltaTime));
					if (!IsFinite(Node.Velocity))
						throw std::runtime_error(
							"[Physics:XpbdContactProbe] nonfinite double-shadow velocity");
				} else if (ReconstructIdealVelocity) {
					const auto &Snapshot = Record.IdealVelocitySnapshots[Index];
					const glm::dvec3 OtherAndIdealDisplacement =
						glm::dvec3(Node.Position) - glm::dvec3(Snapshot.IntegratedPosition) -
						(glm::dvec3(Node.PreviousPosition) -
							glm::dvec3(Snapshot.InitialPreviousPosition)) +
						Snapshot.StructuralFloatBias;
					Node.Velocity = glm::vec3(glm::dvec3(Node.Velocity) +
						OtherAndIdealDisplacement / double(Batch.DeltaTime));
					if (!IsFinite(Node.Velocity))
						throw std::runtime_error(
							"[Physics:XpbdContactProbe] nonfinite ideal reconstructed velocity");
				} else {
					Node.Velocity = (Node.Position - Node.PreviousPosition) / Batch.DeltaTime;
				}
			}
			if (RecordAccounting) {
				if (ReconstructIdealVelocity || UseDoubleShadow)
					Record.ContactProjection.IdealVelocityReconstructionKineticDeltaJ =
						StoredKineticEnergy(Record) - BeforeReconstructionJ;
				Record.ContactProjection.KineticEndJ = StoredKineticEnergy(Record);
				Record.ContactProjection.MomentumEndKgMPerS = StoredMomentum(Record);
				const auto &Accounting = Record.ContactProjection;
				Record.ContactProjection.LedgerResidualJ = Accounting.KineticEndJ - Accounting.KineticStartJ -
					Accounting.AppliedDynamicsKineticDeltaJ - Accounting.DampingKineticDeltaJ -
					Accounting.StructuralProjectionKineticDeltaJ - Accounting.PinProjectionKineticDeltaJ -
					Accounting.ContactKineticDeltaJ -
					Accounting.IntegrationPositionRoundingKineticDeltaJ -
					Accounting.IdealVelocityReconstructionKineticDeltaJ;
				Record.ContactProjection.MomentumResidualNs = Accounting.MomentumEndKgMPerS -
					Accounting.MomentumStartKgMPerS - Accounting.GravityForceImpulseNs -
					Accounting.UniformImpulseNs - Accounting.PointImpulseNs -
					Accounting.DampingImpulseNs - Accounting.PinConstraintImpulseOnClothNs -
						((ReconstructIdealVelocity || UseDoubleShadow) ? glm::dvec3(0.0) :
							Accounting.UnpinnedStructuralNumericalImpulseNs) -
					Accounting.NormalImpulseOnClothNs - Accounting.FrictionImpulseOnClothNs;
			}
			GetExperimentalVelocityTrace().Capture(Item.Body, Record.Nodes);
			++Record.VertexRevision;
			const auto ExtractionStart = ProfileClock::now();
			Item.State = ExtractState(Item.Body, Record, true);
			Item.Profile.ExtractionNanoseconds += Nanoseconds(ProfileClock::now() - ExtractionStart);
			Item.Profile.SimulatedBodies = 1;
			Item.Profile.SimulatedVertices = Record.Nodes.size();
			Item.Profile.ConstraintCount = Record.Distances.size() + Record.Volumes.size();
			Item.WorkerNanoseconds = Nanoseconds(ProfileClock::now() - WorkerStart);
			Item.Completed = true;
		}

		[[nodiscard]] std::size_t DefaultWorkerCount(const SoftBodyWorldLimits &Limits) {
			const auto Hardware = std::max(1u, std::thread::hardware_concurrency());
			return std::max<std::size_t>(
				1, std::min<std::size_t>(Hardware > 1 ? Hardware - 1 : 1, Limits.MaximumWorkers)
			);
		}

		void AddProfile(SoftBodyStepProfile &Target, const SoftBodyStepProfile &Source) {
			Target.IntegrationNanoseconds += Source.IntegrationNanoseconds;
			Target.ConstraintNanoseconds += Source.ConstraintNanoseconds;
			Target.BroadphaseQueryNanoseconds += Source.BroadphaseQueryNanoseconds;
			Target.CollisionNanoseconds += Source.CollisionNanoseconds;
			Target.ExtractionNanoseconds += Source.ExtractionNanoseconds;
			Target.SimulatedBodies += Source.SimulatedBodies;
			Target.SimulatedVertices += Source.SimulatedVertices;
			Target.ConstraintCount += Source.ConstraintCount;
			Target.ColliderQueries += Source.ColliderQueries;
			Target.CandidateColliders += Source.CandidateColliders;
		}

		class XpbdSoftBodyBackend final : public ISoftBodyBackend {
		  public:
			explicit XpbdSoftBodyBackend(
				SoftBodyWorldLimits LimitsValue,
				std::unique_ptr<const detail::SoftBodyStepTestHooks> HooksValue = {}
			)
				: Limits(LimitsValue), Jobs(DefaultWorkerCount(LimitsValue)), TestHooks(std::move(HooksValue)) {
				if (Limits.MaximumBodies == 0 || Limits.MaximumVerticesPerBody == 0 ||
					Limits.MaximumWorldVertices == 0 || Limits.MaximumConstraints == 0 ||
					Limits.MaximumAttachments == 0 || Limits.MaximumColliders == 0 || Limits.MaximumWorkers == 0 ||
					Limits.MaximumBodiesPerBatch == 0 || Limits.MaximumBatchVertices == 0 ||
					Limits.MaximumBatchConstraints == 0 || Limits.MaximumQueuedResultBytes < sizeof(glm::vec3) ||
					Limits.MaximumPointImpulsesPerStep == 0)
					throw std::invalid_argument("Soft-body limits must be nonzero and internally coherent");
			}

			~XpbdSoftBodyBackend() override {
				Shutdown();
			}

			[[nodiscard]] bool IsValid() const override {
				std::scoped_lock Lock(Mutex);
				return !ShuttingDown;
			}

			[[nodiscard]] bool IsBodyValid(SoftBodyId Body) const override {
				std::scoped_lock Lock(Mutex);
				return FindSlot(Body) != nullptr;
			}

			[[nodiscard]] SoftBodyId CreateBody(const SoftBodyDefinition &Definition) override {
				auto Record = ValidateAndBuild(Definition);
				if (!Record) return {};
				std::scoped_lock Lock(Mutex);
				const auto Constraints = Record->Distances.size() + Record->Volumes.size();
				if (ShuttingDown || LiveBodies >= Limits.MaximumBodies ||
					Record->Nodes.size() > Limits.MaximumWorldVertices - LiveVertices ||
					Constraints > Limits.MaximumConstraints - LiveConstraints)
					return {};
				std::uint32_t SlotIndex = 0;
				while (!FreeSlots.empty()) {
					SlotIndex = FreeSlots.back();
					FreeSlots.pop_back();
					if (SlotIndex < Slots.size() && !Slots[SlotIndex].Retired && !Slots[SlotIndex].Live) break;
					SlotIndex = 0;
				}
				if (SlotIndex == 0) {
					if (Slots.size() >= std::numeric_limits<std::uint32_t>::max()) return {};
					SlotIndex = static_cast<std::uint32_t>(Slots.size());
					Slots.emplace_back();
				}
				auto &Slot = Slots[SlotIndex];
				Slot.Live = true;
				Slot.Definition = Definition;
				Slot.VertexCount = Record->Nodes.size();
				Slot.ConstraintCount = Constraints;
				Slot.PendingForce = Slot.PendingImpulse = {};
				Slot.PendingPointImpulses.clear();
				const SoftBodyId Body{SlotIndex, Slot.Generation};
				Slot.LastState = ExtractState(Body, *Record, false);
				Slot.Record = std::move(*Record);
				++LiveBodies;
				LiveVertices += Slot.VertexCount;
				LiveConstraints += Slot.ConstraintCount;
				return Body;
			}

			PhysicsOperationResult UpdateBody(SoftBodyId Body, const SoftBodyDefinition &Definition) override {
				try {
					return UpdateBodyUnchecked(Body, Definition);
				} catch (const std::bad_alloc &) {
					return {PhysicsOperationStatus::BackendFailure, {}};
				}
			}

			PhysicsOperationResult UpdateBodyUnchecked(SoftBodyId Body, const SoftBodyDefinition &Definition) {
				{
					std::scoped_lock Lock(Mutex);
					auto *Slot = FindSlot(Body);
					if (!Slot) return InvalidId("Soft-body ID is invalid or stale");
					if (SameDefinitionExceptAttachments(Slot->Definition, Definition)) {
						if (Definition.Attachments.size() > Limits.MaximumAttachments)
							return InvalidDescription("Soft-body attachment limit exceeded");
						std::vector<bool> Attached(Slot->VertexCount, false);
						for (const auto &Attachment : Definition.Attachments) {
							if (Attachment.Vertex >= Slot->VertexCount || !IsFinite(Attachment.Position) ||
								Attached[Attachment.Vertex])
								return InvalidDescription("Soft-body attachment is invalid or duplicated");
							Attached[Attachment.Vertex] = true;
						}
						if (!Slot->LastState.Positions || Slot->LastState.Positions->size() != Slot->VertexCount)
							return InvalidDescription("Soft-body state is unavailable for attachment update");
						// Stage every allocation before changing a live node, definition, or revision.
						auto SlotAttachments = Definition.Attachments;
						auto RecordAttachments = Definition.Attachments;
						auto PublishedPositions = std::make_shared<std::vector<glm::vec3>>(*Slot->LastState.Positions);
						for (const auto &Attachment : Definition.Attachments)
							(*PublishedPositions)[Attachment.Vertex] = Attachment.Position;
						auto PublishedState = Slot->LastState;
						PublishedState.Positions = std::move(PublishedPositions);
						PublishedState.Simulated = false;
						++PublishedState.VertexRevision;
						std::optional<PendingSoftBodyAttachmentUpdate> Pending;
						if (!Slot->Record) {
							Pending.emplace(PendingSoftBodyAttachmentUpdate{
								.SourceDefinitionRevision = Slot->PendingAttachmentUpdate
									? Slot->PendingAttachmentUpdate->SourceDefinitionRevision
									: Slot->DefinitionRevision,
								.TargetDefinitionRevision = Slot->DefinitionRevision + 1,
								.RecordAttachments = std::move(RecordAttachments),
								.Attached = std::move(Attached),
								.MergePositions = std::make_shared<std::vector<glm::vec3>>(Slot->VertexCount),
							});
						}
						if (TestHooks && TestHooks->BeforeAttachmentCommit) TestHooks->BeforeAttachmentCommit();
						if (Slot->Record) {
							ApplyAttachmentNodes(*Slot->Record, Definition.Attachments, Attached);
							Slot->Record->Definition.Attachments.swap(RecordAttachments);
							Slot->Record->VertexRevision = PublishedState.VertexRevision;
						} else {
							Slot->PendingAttachmentUpdate.emplace(std::move(*Pending));
						}
						Slot->Definition.Attachments.swap(SlotAttachments);
						Slot->LastState = std::move(PublishedState);
						++Slot->DefinitionRevision;
						return {};
					}
				}
				auto Replacement = ValidateAndBuild(Definition);
				if (!Replacement) return InvalidDescription("Soft-body definition is invalid or exceeds a hard bound");
				std::scoped_lock Lock(Mutex);
				auto *Slot = FindSlot(Body);
				if (!Slot) return InvalidId("Soft-body ID is invalid or stale");
				const auto Constraints = Replacement->Distances.size() + Replacement->Volumes.size();
				const auto RemainingVertices = LiveVertices - Slot->VertexCount;
				const auto RemainingConstraints = LiveConstraints - Slot->ConstraintCount;
				if (Replacement->Nodes.size() > Limits.MaximumWorldVertices - RemainingVertices ||
					Constraints > Limits.MaximumConstraints - RemainingConstraints)
					return InvalidDescription("Soft-body world budget would be exceeded");
				Replacement->TopologyRevision = Slot->LastState.TopologyRevision +
												(SameTopology(Slot->Definition, Definition) ? 0 : 1);
				Replacement->VertexRevision = Slot->LastState.VertexRevision + 1;
				auto NewDefinition = Definition;
				auto NewState = ExtractState(Body, *Replacement, false);
				++Slot->DefinitionRevision;
				Slot->PendingAttachmentUpdate.reset();
				Slot->Definition = std::move(NewDefinition);
				Slot->VertexCount = Replacement->Nodes.size();
				Slot->ConstraintCount = Constraints;
				Slot->LastState = std::move(NewState);
				Slot->Record = std::move(*Replacement);
				LiveVertices = RemainingVertices + Slot->VertexCount;
				LiveConstraints = RemainingConstraints + Slot->ConstraintCount;
				return {};
			}

			PhysicsOperationResult DestroyBody(SoftBodyId Body) override {
				std::scoped_lock Lock(Mutex);
				auto *Slot = FindSlot(Body);
				if (!Slot) return InvalidId("Soft-body ID is invalid or stale");
				LiveVertices -= Slot->VertexCount;
				LiveConstraints -= Slot->ConstraintCount;
				--LiveBodies;
				Slot->Live = false;
				Slot->Record.reset();
				Slot->PendingAttachmentUpdate.reset();
				Slot->LastState = {};
				Slot->PendingPointImpulses.clear();
				++Slot->DefinitionRevision;
				if (Slot->Generation == std::numeric_limits<std::uint32_t>::max())
					Slot->Retired = true;
				else {
					++Slot->Generation;
					FreeSlots.push_back(Body.Slot);
				}
				return {};
			}

			PhysicsOperationResult ApplyForce(SoftBodyId Body, glm::vec3 Force) override {
				if (!IsFinite(Force)) return InvalidDescription("Soft-body force must be finite");
				std::scoped_lock Lock(Mutex);
				auto *Slot = FindSlot(Body);
				if (!Slot) return InvalidId("Soft-body ID is invalid or stale");
				const auto Candidate = Slot->PendingForce + Force;
				if (!IsFinite(Candidate)) return InvalidDescription("Accumulated soft-body force overflowed");
				Slot->PendingForce = Candidate;
				return {};
			}

			PhysicsOperationResult ApplyImpulse(SoftBodyId Body, glm::vec3 Impulse) override {
				if (!IsFinite(Impulse)) return InvalidDescription("Soft-body impulse must be finite");
				std::scoped_lock Lock(Mutex);
				auto *Slot = FindSlot(Body);
				if (!Slot) return InvalidId("Soft-body ID is invalid or stale");
				const auto Candidate = Slot->PendingImpulse + Impulse;
				if (!IsFinite(Candidate)) return InvalidDescription("Accumulated soft-body impulse overflowed");
				Slot->PendingImpulse = Candidate;
				return {};
			}

			PhysicsOperationResult
			ApplyImpulseAtPosition(SoftBodyId Body, glm::vec3 Impulse, glm::vec3 Position) override {
				if (!IsFinite(Impulse) || !IsFinite(Position))
					return InvalidDescription("Soft-body point impulse values must be finite");
				std::scoped_lock Lock(Mutex);
				auto *Slot = FindSlot(Body);
				if (!Slot) return InvalidId("Soft-body ID is invalid or stale");
				if (Slot->PendingPointImpulses.size() == Limits.MaximumPointImpulsesPerStep)
					return InvalidDescription("Soft-body point impulse limit reached for this fixed step");
				Slot->PendingPointImpulses.push_back({Impulse, Position});
				return {};
			}

			[[nodiscard]] std::optional<SoftBodyState> GetBodyState(SoftBodyId Body) const override {
				std::scoped_lock Lock(Mutex);
				const auto *Slot = FindSlot(Body);
				return Slot ? std::optional(Slot->LastState) : std::nullopt;
			}

			[[nodiscard]] std::optional<std::vector<glm::vec3>> GetExperimentalVelocities(SoftBodyId Body) const {
				std::scoped_lock Lock(Mutex);
				const auto *Slot = FindSlot(Body);
				if (!Slot || !Slot->Record) return std::nullopt;
				std::vector<glm::vec3> Result;
				Result.reserve(Slot->Record->Nodes.size());
				for (const auto &Node : Slot->Record->Nodes) Result.push_back(Node.Velocity);
				return Result;
			}
			[[nodiscard]] std::optional<PhysicsEvaluation::XpbdContactProjection>
			GetExperimentalContactProjection(SoftBodyId Body) const {
				std::scoped_lock Lock(Mutex);
				const auto *Slot = FindSlot(Body);
				return Slot && Slot->Record ?
					std::optional(Slot->Record->ContactProjection) : std::nullopt;
			}

			[[nodiscard]] std::optional<SoftBodyInspection> GetBodyInspection(SoftBodyId Body) const override {
				std::scoped_lock Lock(Mutex);
				const auto *Slot = FindSlot(Body);
				if (!Slot) return std::nullopt;
				return SoftBodyInspection{
					.VertexCount = Slot->VertexCount,
					.ConstraintCount = Slot->ConstraintCount,
					.AttachmentCount = Slot->Definition.Attachments.size(),
					.TopologyRevision = Slot->LastState.TopologyRevision,
					.VertexRevision = Slot->LastState.VertexRevision,
					.Enabled = Slot->Definition.Enabled,
					.Simulated = Slot->Definition.Enabled && Slot->LastState.Simulated,
				};
			}

			[[nodiscard]] SoftBodyStepResult Step(const SoftBodyStepConfig &Config) override {
				SoftBodyStepResult Result;
				if (!IsFinite(Config.DeltaTime) || Config.DeltaTime <= 0.0f || Config.DeltaTime > 0.25f ||
					!IsFinite(Config.Gravity))
					return Result;
				{
					std::scoped_lock Lock(Mutex);
					if (ShuttingDown) return Result;
					if (StepActive) {
						Result.Profile.BacklogDrops = 1;
						return Result;
					}
					StepActive = true;
				}
				Result.Profile.WorkerCount = Jobs.GetWorkerCount();

				auto FinishStep = [this] {
					std::scoped_lock Lock(Mutex);
					StepActive = false;
					StepCompleted.notify_all();
				};

				std::shared_ptr<SoftBodySimulationBatch> Batch;
				try {
					if (TestHooks && TestHooks->StepAdmitted) TestHooks->StepAdmitted();
					const auto SnapshotStart = ProfileClock::now();
					Batch = std::make_shared<SoftBodySimulationBatch>();
					Batch->DeltaTime = Config.DeltaTime;
					Batch->Gravity = Config.Gravity;
					Batch->BroadphaseMode = Config.BroadphaseMode;
					Batch->Colliders.reserve(std::min(Config.Colliders.size(), Limits.MaximumColliders));
					for (const auto &Collider : Config.Colliders) {
						if (!ValidCollider(Collider) || Batch->Colliders.size() == Limits.MaximumColliders) {
							Result.CollidersTruncated = true;
							continue;
						}
						Batch->Colliders.push_back(Collider);
					}
					const auto BroadphaseStart = ProfileClock::now();
					Batch->Broadphase = std::make_unique<const SoftBodyBroadphase>(Batch->Colliders);
					Result.Profile.BroadphaseConstructionNanoseconds = Nanoseconds(
						ProfileClock::now() - BroadphaseStart
					);
					std::size_t UsedVertices = 0;
					std::size_t BatchVertices = 0;
					std::size_t BatchConstraints = 0;
					{
						std::scoped_lock Lock(Mutex);
						Result.States.reserve(LiveBodies);
						Batch->Items.reserve(std::min(LiveBodies, Limits.MaximumBodiesPerBatch));
						for (std::uint32_t SlotIndex = 1; SlotIndex < Slots.size(); ++SlotIndex) {
							auto &Slot = Slots[SlotIndex];
							if (!Slot.Live || !Slot.Record) continue;
							const SoftBodyId Body{SlotIndex, Slot.Generation};
							const auto Budget = VertexBudget(*Slot.Record, Limits);
							const auto ResultBytes = Slot.VertexCount * sizeof(glm::vec3);
							const bool QualityEligible = Slot.Definition.Enabled &&
														 Slot.VertexCount <= Budget - std::min(Budget, UsedVertices);
							const bool BatchEligible =
								Batch->Items.size() < Limits.MaximumBodiesPerBatch &&
								Slot.VertexCount <= Limits.MaximumBatchVertices -
														std::min(Limits.MaximumBatchVertices, BatchVertices) &&
								Slot.ConstraintCount <=
									Limits.MaximumBatchConstraints -
										std::min(Limits.MaximumBatchConstraints, BatchConstraints) &&
								ResultBytes <=
									Limits.MaximumQueuedResultBytes -
										std::min(Limits.MaximumQueuedResultBytes, Result.Profile.QueuedResultBytes);
							if (!QualityEligible || !BatchEligible) {
								Result.VerticesTruncated = Result.VerticesTruncated ||
														   (Slot.Definition.Enabled && !QualityEligible);
								Result.BodiesTruncated = Result.BodiesTruncated ||
														 (Slot.Definition.Enabled && !BatchEligible);
								++Result.Profile.FrozenBodies;
								Slot.LastState.Simulated = false;
								auto State = Slot.LastState;
								State.Simulated = false;
								Result.States.push_back(std::move(State));
								continue;
							}
							UsedVertices += Slot.VertexCount;
							BatchVertices += Slot.VertexCount;
							BatchConstraints += Slot.ConstraintCount;
							Result.Profile.QueuedResultBytes += ResultBytes;
							SoftBodyWorkItem Item;
							Item.Body = Body;
							Item.DefinitionRevision = Slot.DefinitionRevision;
							Item.Record = std::move(*Slot.Record);
							Item.Force = std::exchange(Slot.PendingForce, glm::vec3{});
							Item.Impulse = std::exchange(Slot.PendingImpulse, glm::vec3{});
							Item.PointImpulses = std::move(Slot.PendingPointImpulses);
							Slot.PendingPointImpulses.clear();
							Slot.Record.reset();
							Batch->Items.push_back(std::move(Item));
						}
					}
					Result.Profile.SnapshotNanoseconds = Nanoseconds(ProfileClock::now() - SnapshotStart);

					std::uint64_t WorkerWindowNanoseconds = 0;
					if (Config.ExecutionMode == SoftBodyExecutionMode::SynchronousReference) {
						const auto WindowStart = ProfileClock::now();
						for (auto &Item : Batch->Items)
							SolveWorkItem(Item, *Batch);
						WorkerWindowNanoseconds = Nanoseconds(ProfileClock::now() - WindowStart);
						Result.Profile.WorkerCount = 0;
					} else if (!Batch->Items.empty()) {
						auto Group = std::make_shared<JobGroup>();
						const auto WindowStart = ProfileClock::now();
						const auto DispatchStart = ProfileClock::now();
						for (std::size_t Index = 0; Index < Batch->Items.size(); ++Index) {
							Jobs.Submit(
								[Batch, Index] {
									try {
										SolveWorkItem(Batch->Items[Index], *Batch);
									} catch (...) {
										Batch->Items[Index].Failed = true;
									}
								},
								Group
							);
						}
						Result.Profile.JobsDispatched = Batch->Items.size();
						Result.Profile.DispatchNanoseconds = Nanoseconds(ProfileClock::now() - DispatchStart);
						const auto WaitStart = ProfileClock::now();
						Group->Wait();
						Result.Profile.WorkerWaitNanoseconds = Nanoseconds(ProfileClock::now() - WaitStart);
						WorkerWindowNanoseconds = Nanoseconds(ProfileClock::now() - WindowStart);
					}

					if (TestHooks && TestHooks->BeforeMerge) TestHooks->BeforeMerge(Result.Profile);
					const auto MergeStart = ProfileClock::now();
					std::uint64_t WorkerNanoseconds = 0;
					{
						std::scoped_lock Lock(Mutex);
						for (auto &Item : Batch->Items) {
							WorkerNanoseconds += Item.WorkerNanoseconds;
							AddProfile(Result.Profile, Item.Profile);
							auto *Slot = FindSlot(Item.Body);
							const bool Current = Slot && Slot->DefinitionRevision == Item.DefinitionRevision &&
												 !Slot->Record;
							const bool PendingCurrent = Slot && !Slot->Record && Slot->PendingAttachmentUpdate &&
								Slot->PendingAttachmentUpdate->SourceDefinitionRevision == Item.DefinitionRevision &&
								Slot->PendingAttachmentUpdate->TargetDefinitionRevision == Slot->DefinitionRevision;
							auto RestoreInputs = [&] {
								Slot->PendingForce += Item.Force;
								Slot->PendingImpulse += Item.Impulse;
								const auto Available =
									Limits.MaximumPointImpulsesPerStep -
									std::min(Limits.MaximumPointImpulsesPerStep, Slot->PendingPointImpulses.size());
								const auto Count = std::min(Available, Item.PointImpulses.size());
								Slot->PendingPointImpulses.insert(
									Slot->PendingPointImpulses.begin(), Item.PointImpulses.begin(),
									Item.PointImpulses.begin() + static_cast<std::ptrdiff_t>(Count)
								);
							};
							if (PendingCurrent && !ShuttingDown) {
								auto &Pending = *Slot->PendingAttachmentUpdate;
								if (Item.Completed && !Item.Failed) {
									auto &Positions = *Pending.MergePositions;
									std::copy(
										Item.State.Positions->begin(), Item.State.Positions->end(), Positions.begin()
									);
									for (const auto &Attachment : Pending.RecordAttachments)
										Positions[Attachment.Vertex] = Attachment.Position;
									ApplyAttachmentNodes(Item.Record, Pending.RecordAttachments, Pending.Attached);
									Item.Record.Definition.Attachments.swap(Pending.RecordAttachments);
									Item.Record.VertexRevision = Slot->LastState.VertexRevision + 1;
									auto State = Item.State;
									State.VertexRevision = Item.Record.VertexRevision;
									State.Positions = Pending.MergePositions;
									Slot->Record = std::move(Item.Record);
									Slot->LastState = State;
									Slot->PendingAttachmentUpdate.reset();
									Result.States.push_back(std::move(State));
								} else {
									++Result.Profile.StaleResults;
									ApplyAttachmentNodes(Item.Record, Pending.RecordAttachments, Pending.Attached);
									Item.Record.Definition.Attachments.swap(Pending.RecordAttachments);
									Item.Record.VertexRevision = Slot->LastState.VertexRevision;
									Slot->Record = std::move(Item.Record);
									Slot->PendingAttachmentUpdate.reset();
									RestoreInputs();
								}
								continue;
							}
							if (!Current || !Item.Completed || Item.Failed || ShuttingDown) {
								++Result.Profile.StaleResults;
								if (Current && !ShuttingDown) {
									Slot->Record = std::move(Item.Record);
									RestoreInputs();
								}
								continue;
							}
							Slot->Record = std::move(Item.Record);
							Slot->LastState = Item.State;
							Result.States.push_back(std::move(Item.State));
						}
						Result.Profile.EstimatedBytes = sizeof(*this) + Slots.capacity() * sizeof(SoftBodySlot) +
														Batch->Broadphase->EstimatedBytes();
						for (const auto &Slot : Slots) {
							if (!Slot.Live || !Slot.Record) continue;
							Result.Profile.EstimatedBytes += Slot.Record->Nodes.capacity() * sizeof(SoftBodyNode);
							Result.Profile.EstimatedBytes += Slot.Record->Distances.capacity() *
															 sizeof(SoftBodyDistanceConstraint);
							Result.Profile.EstimatedBytes += Slot.Record->Volumes.capacity() *
															 sizeof(SoftBodyVolumeConstraint);
							if (Slot.Record->Indices)
								Result.Profile.EstimatedBytes += Slot.Record->Indices->capacity() *
																 sizeof(std::uint32_t);
						}
					}
					std::ranges::sort(Result.States, {}, &SoftBodyState::Body);
					Result.Profile.ResultMergeNanoseconds = Nanoseconds(ProfileClock::now() - MergeStart);
					if (Config.ExecutionMode == SoftBodyExecutionMode::Jobified && WorkerWindowNanoseconds != 0 &&
						Result.Profile.WorkerCount != 0) {
						const auto Denominator = static_cast<double>(WorkerWindowNanoseconds) *
												 static_cast<double>(Result.Profile.WorkerCount);
						Result.Profile.WorkerUtilization = std::min(
							1.0, static_cast<double>(WorkerNanoseconds) / Denominator
						);
					}
					FinishStep();
					return Result;
				} catch (...) {
					Jobs.Drain();
					if (Batch) {
						std::scoped_lock Lock(Mutex);
						for (auto &Item : Batch->Items) {
							auto *Slot = FindSlot(Item.Body);
							if (!Slot || Slot->Record) continue;
							if (Slot->PendingAttachmentUpdate &&
								Slot->PendingAttachmentUpdate->SourceDefinitionRevision == Item.DefinitionRevision &&
								Slot->PendingAttachmentUpdate->TargetDefinitionRevision == Slot->DefinitionRevision) {
								auto &Pending = *Slot->PendingAttachmentUpdate;
								ApplyAttachmentNodes(Item.Record, Pending.RecordAttachments, Pending.Attached);
								Item.Record.Definition.Attachments.swap(Pending.RecordAttachments);
								Item.Record.VertexRevision = Slot->LastState.VertexRevision;
								Slot->PendingAttachmentUpdate.reset();
							} else if (Slot->DefinitionRevision != Item.DefinitionRevision) continue;
							Slot->Record = std::move(Item.Record);
							Slot->PendingForce += Item.Force;
							Slot->PendingImpulse += Item.Impulse;
							const auto Available =
								Limits.MaximumPointImpulsesPerStep -
								std::min(Limits.MaximumPointImpulsesPerStep, Slot->PendingPointImpulses.size());
							const auto Count = std::min(Available, Item.PointImpulses.size());
							Slot->PendingPointImpulses.insert(
								Slot->PendingPointImpulses.begin(),
								Item.PointImpulses.begin(),
								Item.PointImpulses.begin() + static_cast<std::ptrdiff_t>(Count)
							);
						}
					}
					FinishStep();
					throw;
				}
			}

			[[nodiscard]] bool HasInFlightStep() const override {
				std::scoped_lock Lock(Mutex);
				return StepActive;
			}

			void Shutdown() override {
				std::unique_lock Lock(Mutex);
				if (JobsStopped) return;
				ShuttingDown = true;
				StepCompleted.wait(Lock, [this] { return !StepActive; });
				for (auto &Slot : Slots) Slot.PendingAttachmentUpdate.reset();
				Jobs.Shutdown(true);
				JobsStopped = true;
			}

			[[nodiscard]] const SoftBodyWorldLimits &GetLimits() const override {
				return Limits;
			}

		  private:
			SoftBodyWorldLimits Limits;
			JobSystem Jobs;
			const std::unique_ptr<const detail::SoftBodyStepTestHooks> TestHooks;
			mutable std::mutex Mutex;
			std::condition_variable StepCompleted;
			std::vector<SoftBodySlot> Slots = std::vector<SoftBodySlot>(1);
			std::vector<std::uint32_t> FreeSlots;
			std::size_t LiveBodies = 0;
			std::size_t LiveVertices = 0;
			std::size_t LiveConstraints = 0;
			bool StepActive = false;
			bool ShuttingDown = false;
			bool JobsStopped = false;

			[[nodiscard]] SoftBodySlot *FindSlot(SoftBodyId Body) {
				if (!Body.IsValid() || Body.Slot >= Slots.size()) return nullptr;
				auto &Slot = Slots[Body.Slot];
				return Slot.Live && Slot.Generation == Body.Generation ? &Slot : nullptr;
			}

			[[nodiscard]] const SoftBodySlot *FindSlot(SoftBodyId Body) const {
				if (!Body.IsValid() || Body.Slot >= Slots.size()) return nullptr;
				const auto &Slot = Slots[Body.Slot];
				return Slot.Live && Slot.Generation == Body.Generation ? &Slot : nullptr;
			}

			[[nodiscard]] std::optional<SoftBodyRecord> ValidateAndBuild(const SoftBodyDefinition &Definition) const {
				if (!IsFinite(Definition.Position) || !IsFinite(Definition.Size) || Definition.Size.x <= 0.0f ||
					Definition.Size.y <= 0.0f || Definition.Size.z <= 0.0f || !ValidMaterial(Definition.Material) ||
					Definition.Attachments.size() > Limits.MaximumAttachments)
					return std::nullopt;
				const auto Count = VertexCount(Definition);
				if (!Count || *Count > Limits.MaximumVerticesPerBody) return std::nullopt;
				for (const auto &Attachment : Definition.Attachments)
					if (!IsFinite(Attachment.Position) || Attachment.Vertex >= *Count) return std::nullopt;
				auto Record = BuildRecord(Definition);
				if (!Record || Record->Distances.size() + Record->Volumes.size() > Limits.MaximumConstraints)
					return std::nullopt;
				return Record;
			}

			[[nodiscard]] static PhysicsOperationResult InvalidId(std::string Message) {
				return {PhysicsOperationStatus::InvalidId, std::move(Message)};
			}

			[[nodiscard]] static PhysicsOperationResult InvalidDescription(std::string Message) {
				return {PhysicsOperationStatus::InvalidDescription, std::move(Message)};
			}
		};
	}

	std::unique_ptr<ISoftBodyBackend> CreateSoftBodyBackend(SoftBodyWorldLimits Limits) {
		return std::make_unique<XpbdSoftBodyBackend>(Limits);
	}

	std::unique_ptr<ISoftBodyBackend> detail::CreateSoftBodyBackendForTesting(
		SoftBodyStepTestHooks Hooks,
		SoftBodyWorldLimits Limits
	) {
		return std::make_unique<XpbdSoftBodyBackend>(
			Limits, std::make_unique<const SoftBodyStepTestHooks>(std::move(Hooks))
		);
	}
}

namespace PhysicsEvaluation {
    void FinishExperimentalXpbdTrace() { gargantuan::GetExperimentalVelocityTrace().Finish(); }
    std::optional<std::vector<glm::vec3>> GetExperimentalXpbdVelocities(
        const gargantuan::ISoftBodyBackend &Soft, gargantuan::SoftBodyId Body) {
        const auto *Experimental = dynamic_cast<const gargantuan::XpbdSoftBodyBackend *>(&Soft);
        return Experimental ? Experimental->GetExperimentalVelocities(Body) : std::nullopt;
    }
    std::optional<XpbdContactProjection> GetExperimentalXpbdContactProjection(
        const gargantuan::ISoftBodyBackend &Soft, gargantuan::SoftBodyId Body) {
        const auto *Experimental = dynamic_cast<const gargantuan::XpbdSoftBodyBackend *>(&Soft);
        return Experimental ? Experimental->GetExperimentalContactProjection(Body) : std::nullopt;
    }
}

