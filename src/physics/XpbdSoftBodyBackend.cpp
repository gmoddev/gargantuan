// Copyright (c) 2026 Team Fireworks
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "gargantuan/physics/SoftBodyBackend.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include <glm/common.hpp>
#include <glm/geometric.hpp>

namespace gargantuan {
	namespace {
		using ProfileClock = std::chrono::steady_clock;

		struct SoftBodyNode {
			glm::vec3 Position{0.0f};
			glm::vec3 PreviousPosition{0.0f};
			glm::vec3 Velocity{0.0f};
			glm::vec3 RestOffset{0.0f};
			float InverseMass = 1.0f;
		};

		struct SoftBodyDistanceConstraint {
			std::uint32_t VertexA = 0;
			std::uint32_t VertexB = 0;
			float RestLength = 0.0f;
			float Compliance = 0.0f;
			float Lambda = 0.0f;
		};

		struct SoftBodyRecord {
			SoftBodyDefinition Definition;
			std::vector<SoftBodyNode> Nodes;
			std::vector<SoftBodyDistanceConstraint> Constraints;
			std::shared_ptr<const std::vector<std::uint32_t>> Indices;
			std::uint64_t TopologyRevision = 1;
			std::uint64_t VertexRevision = 1;
			glm::vec3 PendingForce{0.0f};
			glm::vec3 PendingImpulse{0.0f};
		};

		struct SoftBodySlot {
			std::uint32_t Generation = 1;
			std::optional<SoftBodyRecord> Record;
			bool Retired = false;
		};

		[[nodiscard]] bool IsFinite(float Value) { return std::isfinite(Value); }
		[[nodiscard]] bool IsFinite(const glm::vec3 &Value) {
			return IsFinite(Value.x) && IsFinite(Value.y) && IsFinite(Value.z);
		}
		[[nodiscard]] bool IsFinite(const glm::mat3 &Value) {
			return IsFinite(Value[0]) && IsFinite(Value[1]) && IsFinite(Value[2]);
		}

		[[nodiscard]] bool ValidCollider(const SoftBodyCollider &Collider) {
			switch (Collider.Shape.Kind) {
				case PhysicsShapeKind::Box:
				case PhysicsShapeKind::Ball:
				case PhysicsShapeKind::Cylinder:
				case PhysicsShapeKind::Wedge:
				case PhysicsShapeKind::CornerWedge: break;
				default: return false;
			}
			if (!IsFinite(Collider.Shape.Size) || Collider.Shape.Size.x <= 0.0f || Collider.Shape.Size.y <= 0.0f ||
				Collider.Shape.Size.z <= 0.0f || !IsFinite(Collider.Transform.Position) ||
				!IsFinite(Collider.Transform.Rotation)) return false;
			const auto &Rotation = Collider.Transform.Rotation;
			constexpr float RotationTolerance = 0.02f;
			return std::abs(glm::dot(Rotation[0], Rotation[0]) - 1.0f) <= RotationTolerance &&
				std::abs(glm::dot(Rotation[1], Rotation[1]) - 1.0f) <= RotationTolerance &&
				std::abs(glm::dot(Rotation[2], Rotation[2]) - 1.0f) <= RotationTolerance &&
				std::abs(glm::dot(Rotation[0], Rotation[1])) <= RotationTolerance &&
				std::abs(glm::dot(Rotation[0], Rotation[2])) <= RotationTolerance &&
				std::abs(glm::dot(Rotation[1], Rotation[2])) <= RotationTolerance;
		}

		[[nodiscard]] std::uint64_t Nanoseconds(ProfileClock::duration Duration) {
			return static_cast<std::uint64_t>(
				std::chrono::duration_cast<std::chrono::nanoseconds>(Duration).count()
			);
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
				Material.ShapeCompliance >= 0.0f && IsFinite(Material.Friction) && Material.Friction >= 0.0f &&
				Material.Friction <= 1.0f && IsFinite(Material.Thickness) && Material.Thickness >= 0.0f &&
				Material.Thickness <= 10.0f;
		}

		[[nodiscard]] std::optional<std::size_t> VertexCount(const SoftBodyDefinition &Definition) {
			if (Definition.ResolutionX < 2 || Definition.ResolutionY < 2) return std::nullopt;
			std::size_t Result = 0;
			if (!Multiply(Definition.ResolutionX, Definition.ResolutionY, Result)) return std::nullopt;
			if (Definition.Kind == SoftBodyKind::Rubber) {
				if (Definition.ResolutionZ < 2 || !Multiply(Result, Definition.ResolutionZ, Result)) return std::nullopt;
			}
			return Result;
		}

		[[nodiscard]] bool SameTopology(const SoftBodyDefinition &Left, const SoftBodyDefinition &Right) {
			return Left.Kind == Right.Kind && Left.ResolutionX == Right.ResolutionX &&
				Left.ResolutionY == Right.ResolutionY &&
				(Left.Kind == SoftBodyKind::Cloth || Left.ResolutionZ == Right.ResolutionZ);
		}

		void AddConstraint(
			SoftBodyRecord &Record,
			std::uint32_t VertexA,
			std::uint32_t VertexB,
			float Compliance
		) {
			const auto Delta = Record.Nodes[VertexB].Position - Record.Nodes[VertexA].Position;
			Record.Constraints.push_back({VertexA, VertexB, glm::length(Delta), Compliance, 0.0f});
		}

		void AddQuad(std::vector<std::uint32_t> &Indices, std::uint32_t A, std::uint32_t B, std::uint32_t C, std::uint32_t D) {
			Indices.insert(Indices.end(), {A, B, C, A, C, D});
		}

		void BuildCloth(SoftBodyRecord &Record) {
			const auto XCount = Record.Definition.ResolutionX;
			const auto YCount = Record.Definition.ResolutionY;
			const auto VertexTotal = static_cast<std::size_t>(XCount) * YCount;
			Record.Nodes.reserve(VertexTotal);
			for (std::uint32_t Y = 0; Y < YCount; ++Y) {
				const auto V = static_cast<float>(Y) / static_cast<float>(YCount - 1);
				for (std::uint32_t X = 0; X < XCount; ++X) {
					const auto U = static_cast<float>(X) / static_cast<float>(XCount - 1);
					const glm::vec3 Offset{
						(U - 0.5f) * Record.Definition.Size.x,
						(0.5f - V) * Record.Definition.Size.y,
						0.0f,
					};
					const auto Position = Record.Definition.Position + Offset;
					Record.Nodes.push_back({Position, Position, {}, Offset, 1.0f / Record.Definition.Material.ParticleMass});
				}
			}

			auto Index = [XCount](std::uint32_t X, std::uint32_t Y) { return Y * XCount + X; };
			auto Indices = std::make_shared<std::vector<std::uint32_t>>();
			Indices->reserve(static_cast<std::size_t>(XCount - 1) * (YCount - 1) * 6);
			for (std::uint32_t Y = 0; Y + 1 < YCount; ++Y) {
				for (std::uint32_t X = 0; X + 1 < XCount; ++X) {
					AddQuad(*Indices, Index(X, Y), Index(X, Y + 1), Index(X + 1, Y + 1), Index(X + 1, Y));
				}
			}
			Record.Indices = std::move(Indices);

			for (std::uint32_t Y = 0; Y < YCount; ++Y) {
				for (std::uint32_t X = 0; X < XCount; ++X) {
					if (X + 1 < XCount)
						AddConstraint(Record, Index(X, Y), Index(X + 1, Y), Record.Definition.Material.StretchCompliance);
					if (Y + 1 < YCount)
						AddConstraint(Record, Index(X, Y), Index(X, Y + 1), Record.Definition.Material.StretchCompliance);
					if (X + 1 < XCount && Y + 1 < YCount) {
						AddConstraint(Record, Index(X, Y), Index(X + 1, Y + 1), Record.Definition.Material.StretchCompliance);
						AddConstraint(Record, Index(X + 1, Y), Index(X, Y + 1), Record.Definition.Material.StretchCompliance);
					}
					if (X + 2 < XCount)
						AddConstraint(Record, Index(X, Y), Index(X + 2, Y), Record.Definition.Material.BendCompliance);
					if (Y + 2 < YCount)
						AddConstraint(Record, Index(X, Y), Index(X, Y + 2), Record.Definition.Material.BendCompliance);
				}
			}
		}

		void BuildRubber(SoftBodyRecord &Record) {
			const auto XCount = Record.Definition.ResolutionX;
			const auto YCount = Record.Definition.ResolutionY;
			const auto ZCount = Record.Definition.ResolutionZ;
			const auto VertexTotal = static_cast<std::size_t>(XCount) * YCount * ZCount;
			Record.Nodes.reserve(VertexTotal);
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
						Record.Nodes.push_back({Position, Position, {}, Offset, 1.0f / Record.Definition.Material.ParticleMass});
					}
				}
			}

			auto Indices = std::make_shared<std::vector<std::uint32_t>>();
			for (std::uint32_t Y = 0; Y + 1 < YCount; ++Y)
				for (std::uint32_t X = 0; X + 1 < XCount; ++X) {
					AddQuad(*Indices, Index(X, Y, 0), Index(X + 1, Y, 0), Index(X + 1, Y + 1, 0), Index(X, Y + 1, 0));
					AddQuad(*Indices, Index(X, Y, ZCount - 1), Index(X, Y + 1, ZCount - 1), Index(X + 1, Y + 1, ZCount - 1), Index(X + 1, Y, ZCount - 1));
				}
			for (std::uint32_t Z = 0; Z + 1 < ZCount; ++Z)
				for (std::uint32_t X = 0; X + 1 < XCount; ++X) {
					AddQuad(*Indices, Index(X, 0, Z), Index(X, 0, Z + 1), Index(X + 1, 0, Z + 1), Index(X + 1, 0, Z));
					AddQuad(*Indices, Index(X, YCount - 1, Z), Index(X + 1, YCount - 1, Z), Index(X + 1, YCount - 1, Z + 1), Index(X, YCount - 1, Z + 1));
				}
			for (std::uint32_t Z = 0; Z + 1 < ZCount; ++Z)
				for (std::uint32_t Y = 0; Y + 1 < YCount; ++Y) {
					AddQuad(*Indices, Index(0, Y, Z), Index(0, Y + 1, Z), Index(0, Y + 1, Z + 1), Index(0, Y, Z + 1));
					AddQuad(*Indices, Index(XCount - 1, Y, Z), Index(XCount - 1, Y, Z + 1), Index(XCount - 1, Y + 1, Z + 1), Index(XCount - 1, Y + 1, Z));
				}
			Record.Indices = std::move(Indices);

			for (std::uint32_t Z = 0; Z < ZCount; ++Z)
				for (std::uint32_t Y = 0; Y < YCount; ++Y)
					for (std::uint32_t X = 0; X < XCount; ++X) {
						if (X + 1 < XCount)
							AddConstraint(Record, Index(X, Y, Z), Index(X + 1, Y, Z), Record.Definition.Material.StretchCompliance);
						if (Y + 1 < YCount)
							AddConstraint(Record, Index(X, Y, Z), Index(X, Y + 1, Z), Record.Definition.Material.StretchCompliance);
						if (Z + 1 < ZCount)
							AddConstraint(Record, Index(X, Y, Z), Index(X, Y, Z + 1), Record.Definition.Material.StretchCompliance);
					}
		}

		[[nodiscard]] std::optional<SoftBodyRecord> BuildRecord(const SoftBodyDefinition &Definition) {
			SoftBodyRecord Result;
			Result.Definition = Definition;
			if (Definition.Kind == SoftBodyKind::Cloth) BuildCloth(Result);
			else BuildRubber(Result);
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
				case SoftBodyQuality::Low: return 2;
				case SoftBodyQuality::Medium: return 4;
				case SoftBodyQuality::High: return 8;
				case SoftBodyQuality::Automatic:
					if (Record.Nodes.size() <= 4096) return 8;
					if (Record.Nodes.size() <= 16384) return 4;
					return 2;
			}
			return 2;
		}

		[[nodiscard]] std::size_t VertexBudget(const SoftBodyRecord &Record, const SoftBodyWorldLimits &Limits) {
			switch (Record.Definition.Quality) {
				case SoftBodyQuality::Low: return std::min<std::size_t>(4096, Limits.MaximumWorldVertices);
				case SoftBodyQuality::Medium: return std::min<std::size_t>(16384, Limits.MaximumWorldVertices);
				case SoftBodyQuality::High:
				case SoftBodyQuality::Automatic: return Limits.MaximumWorldVertices;
			}
			return 0;
		}

		void SolveDistance(SoftBodyRecord &Record, float DeltaTime) {
			for (auto &Constraint : Record.Constraints) {
				auto &NodeA = Record.Nodes[Constraint.VertexA];
				auto &NodeB = Record.Nodes[Constraint.VertexB];
				const auto Delta = NodeB.Position - NodeA.Position;
				const auto Length = glm::length(Delta);
				if (Length <= 1e-7f) continue;
				const auto Weight = NodeA.InverseMass + NodeB.InverseMass;
				const auto Alpha = Constraint.Compliance / (DeltaTime * DeltaTime);
				if (Weight + Alpha <= 1e-9f) continue;
				const auto ConstraintValue = Length - Constraint.RestLength;
				const auto DeltaLambda = (-ConstraintValue - Alpha * Constraint.Lambda) / (Weight + Alpha);
				Constraint.Lambda += DeltaLambda;
				const auto Correction = (Delta / Length) * DeltaLambda;
				NodeA.Position -= NodeA.InverseMass * Correction;
				NodeB.Position += NodeB.InverseMass * Correction;
			}
		}

		void SolveShapeRecovery(SoftBodyRecord &Record, float DeltaTime) {
			if (Record.Definition.Kind != SoftBodyKind::Rubber) return;
			glm::vec3 Translation{0.0f};
			std::size_t Count = 0;
			for (const auto &Node : Record.Nodes) {
				if (Node.InverseMass != 0.0f) continue;
				Translation += Node.Position - Node.RestOffset;
				++Count;
			}
			if (Count == 0) for (const auto &Node : Record.Nodes) {
				Translation += Node.Position - Node.RestOffset;
				++Count;
			}
			if (Count == 0) return;
			Translation /= static_cast<float>(Count);
			const auto Compliance = Record.Definition.Material.ShapeCompliance / (DeltaTime * DeltaTime);
			const auto Factor = 1.0f / (1.0f + Compliance);
			for (auto &Node : Record.Nodes) {
				if (Node.InverseMass == 0.0f) continue;
				const auto Target = Translation + Node.RestOffset;
				Node.Position += (Target - Node.Position) * Factor;
			}
		}

		void ApplyAttachments(SoftBodyRecord &Record) {
			for (const auto &Attachment : Record.Definition.Attachments) {
				auto &Node = Record.Nodes[Attachment.Vertex];
				Node.Position = Attachment.Position;
				Node.Velocity = {};
			}
		}

		[[nodiscard]] std::optional<glm::vec3> CollideNode(
			SoftBodyNode &Node,
			const SoftBodyCollider &Collider,
			float Thickness
		) {
			const auto InverseRotation = glm::transpose(Collider.Transform.Rotation);
			auto Local = InverseRotation * (Node.Position - Collider.Transform.Position);
			glm::vec3 LocalNormal{0.0f};
			float Push = 0.0f;
			if (Collider.Shape.Kind == PhysicsShapeKind::Ball) {
				const auto Half = glm::abs(Collider.Shape.Size) * 0.5f;
				const auto Radius = std::max({Half.x, Half.y, Half.z}) + Thickness;
				const auto Distance = glm::length(Local);
				if (Distance >= Radius) return std::nullopt;
				LocalNormal = Distance > 1e-7f ? Local / Distance : glm::vec3(0.0f, 1.0f, 0.0f);
				Push = Radius - Distance;
			} else {
				const auto Half = glm::abs(Collider.Shape.Size) * 0.5f;
				const auto Expanded = Half + glm::vec3(Thickness);
				const auto Closest = glm::clamp(Local, -Expanded, Expanded);
				const auto Delta = Local - Closest;
				const auto Distance = glm::length(Delta);
				const bool Inside = glm::all(glm::lessThanEqual(glm::abs(Local), Expanded));
				if (!Inside && Distance >= Thickness) return std::nullopt;
				if (!Inside && Distance > 1e-7f) {
					LocalNormal = Delta / Distance;
					Push = Thickness - Distance;
				} else {
					const auto Penetration = Expanded - glm::abs(Local);
					int Axis = Penetration.y < Penetration.x ? 1 : 0;
					if (Penetration.z < Penetration[Axis]) Axis = 2;
					LocalNormal[Axis] = Local[Axis] < 0.0f ? -1.0f : 1.0f;
					Push = Penetration[Axis];
				}
			}
			const auto WorldNormal = Collider.Transform.Rotation * LocalNormal;
			Node.Position += WorldNormal * Push;
			return WorldNormal;
		}

		void Collide(SoftBodyRecord &Record, const std::vector<SoftBodyCollider> &Colliders, bool ApplyFriction) {
			if (Record.Definition.CollisionMode == SoftBodyCollisionMode::None) return;
			for (auto &Node : Record.Nodes) {
				if (Node.InverseMass == 0.0f) continue;
				for (const auto &Collider : Colliders) {
					const auto Normal = CollideNode(Node, Collider, Record.Definition.Material.Thickness);
					if (!Normal || !ApplyFriction) continue;
					const auto Movement = Node.Position - Node.PreviousPosition;
					const auto NormalMovement = glm::dot(Movement, *Normal);
					const auto TangentialMovement = Movement - NormalMovement * *Normal;
					const auto RetainedMovement = std::max(NormalMovement, 0.0f) * *Normal +
						TangentialMovement * (1.0f - Record.Definition.Material.Friction);
					Node.PreviousPosition = Node.Position - RetainedMovement;
				}
			}
		}

		[[nodiscard]] SoftBodyState ExtractState(SoftBodyId Body, const SoftBodyRecord &Record, bool Simulated) {
			auto Positions = std::make_shared<std::vector<glm::vec3>>();
			Positions->reserve(Record.Nodes.size());
			for (const auto &Node : Record.Nodes) Positions->push_back(Node.Position);
			return {
				.Body = Body,
				.TopologyRevision = Record.TopologyRevision,
				.VertexRevision = Record.VertexRevision,
				.Simulated = Simulated,
				.Positions = std::move(Positions),
				.Indices = Record.Indices,
			};
		}

		class XpbdSoftBodyBackend final : public ISoftBodyBackend {
		  public:
			explicit XpbdSoftBodyBackend(SoftBodyWorldLimits LimitsValue) : Limits(LimitsValue) {
				if (Limits.MaximumBodies == 0 || Limits.MaximumVerticesPerBody == 0 ||
					Limits.MaximumWorldVertices == 0 || Limits.MaximumConstraints == 0 ||
					Limits.MaximumAttachments == 0 || Limits.MaximumColliders == 0)
					throw std::invalid_argument("Soft-body limits must be nonzero");
			}

			[[nodiscard]] bool IsValid() const override { return true; }
			[[nodiscard]] bool IsBodyValid(SoftBodyId Body) const override { return Find(Body) != nullptr; }

			[[nodiscard]] SoftBodyId CreateBody(const SoftBodyDefinition &Definition) override {
				auto Record = ValidateAndBuild(Definition);
				if (!Record || LiveBodies >= Limits.MaximumBodies ||
					LiveVertices > Limits.MaximumWorldVertices - Record->Nodes.size() ||
					LiveConstraints > Limits.MaximumConstraints - Record->Constraints.size()) return {};

				std::uint32_t SlotIndex = 0;
				while (!FreeSlots.empty()) {
					SlotIndex = FreeSlots.back();
					FreeSlots.pop_back();
					if (SlotIndex < Slots.size() && !Slots[SlotIndex].Retired && !Slots[SlotIndex].Record) break;
					SlotIndex = 0;
				}
				if (SlotIndex == 0) {
					if (Slots.size() >= std::numeric_limits<std::uint32_t>::max()) return {};
					SlotIndex = static_cast<std::uint32_t>(Slots.size());
					Slots.emplace_back();
				}
				auto &Slot = Slots[SlotIndex];
				Slot.Record = std::move(*Record);
				++LiveBodies;
				LiveVertices += Slot.Record->Nodes.size();
				LiveConstraints += Slot.Record->Constraints.size();
				return {SlotIndex, Slot.Generation};
			}

			PhysicsOperationResult UpdateBody(SoftBodyId Body, const SoftBodyDefinition &Definition) override {
				auto *Existing = Find(Body);
				if (!Existing) return InvalidId("Soft-body ID is invalid or stale");
				auto Replacement = ValidateAndBuild(Definition);
				if (!Replacement) return InvalidDescription("Soft-body definition is invalid or exceeds a hard bound");
				const auto RemainingVertices = LiveVertices - Existing->Nodes.size();
				const auto RemainingConstraints = LiveConstraints - Existing->Constraints.size();
				if (Replacement->Nodes.size() > Limits.MaximumWorldVertices - RemainingVertices ||
					Replacement->Constraints.size() > Limits.MaximumConstraints - RemainingConstraints)
					return InvalidDescription("Soft-body world budget would be exceeded");
				Replacement->TopologyRevision = Existing->TopologyRevision + (SameTopology(Existing->Definition, Definition) ? 0 : 1);
				Replacement->VertexRevision = Existing->VertexRevision + 1;
				Replacement->PendingForce = Existing->PendingForce;
				Replacement->PendingImpulse = Existing->PendingImpulse;
				LiveVertices = RemainingVertices + Replacement->Nodes.size();
				LiveConstraints = RemainingConstraints + Replacement->Constraints.size();
				*Existing = std::move(*Replacement);
				return {};
			}

			PhysicsOperationResult DestroyBody(SoftBodyId Body) override {
				if (!Body.IsValid() || Body.Slot >= Slots.size()) return InvalidId("Soft-body ID is invalid or stale");
				auto &Slot = Slots[Body.Slot];
				if (Slot.Generation != Body.Generation || !Slot.Record) return InvalidId("Soft-body ID is invalid or stale");
				LiveVertices -= Slot.Record->Nodes.size();
				LiveConstraints -= Slot.Record->Constraints.size();
				--LiveBodies;
				Slot.Record.reset();
				if (Slot.Generation == std::numeric_limits<std::uint32_t>::max()) Slot.Retired = true;
				else {
					++Slot.Generation;
					FreeSlots.push_back(Body.Slot);
				}
				return {};
			}

			PhysicsOperationResult ApplyForce(SoftBodyId Body, glm::vec3 Force) override {
				auto *Record = Find(Body);
				if (!Record) return InvalidId("Soft-body ID is invalid or stale");
				if (!IsFinite(Force)) return InvalidDescription("Soft-body force must be finite");
				const auto Candidate = Record->PendingForce + Force;
				if (!IsFinite(Candidate)) return InvalidDescription("Accumulated soft-body force overflowed");
				Record->PendingForce = Candidate;
				return {};
			}

			PhysicsOperationResult ApplyImpulse(SoftBodyId Body, glm::vec3 Impulse) override {
				auto *Record = Find(Body);
				if (!Record) return InvalidId("Soft-body ID is invalid or stale");
				if (!IsFinite(Impulse)) return InvalidDescription("Soft-body impulse must be finite");
				const auto Candidate = Record->PendingImpulse + Impulse;
				if (!IsFinite(Candidate)) return InvalidDescription("Accumulated soft-body impulse overflowed");
				Record->PendingImpulse = Candidate;
				return {};
			}

			[[nodiscard]] std::optional<SoftBodyState> GetBodyState(SoftBodyId Body) const override {
				const auto *Record = Find(Body);
				return Record ? std::optional(ExtractState(Body, *Record, false)) : std::nullopt;
			}

			[[nodiscard]] SoftBodyStepResult Step(const SoftBodyStepConfig &Config) override {
				if (!IsFinite(Config.DeltaTime) || Config.DeltaTime <= 0.0f || Config.DeltaTime > 0.25f ||
					!IsFinite(Config.Gravity)) return {};
				SoftBodyStepResult Result;
				std::vector<SoftBodyCollider> Colliders;
				Colliders.reserve(std::min(Config.Colliders.size(), Limits.MaximumColliders));
				for (const auto &Collider : Config.Colliders) {
					if (!ValidCollider(Collider) || Colliders.size() == Limits.MaximumColliders) {
						Result.CollidersTruncated = true;
						continue;
					}
					Colliders.push_back(Collider);
				}
				std::size_t UsedVertices = 0;

				for (std::uint32_t SlotIndex = 1; SlotIndex < Slots.size(); ++SlotIndex) {
					auto &Slot = Slots[SlotIndex];
					if (!Slot.Record) continue;
					auto &Record = *Slot.Record;
					const SoftBodyId Body{SlotIndex, Slot.Generation};
					const auto Budget = VertexBudget(Record, Limits);
					const bool Simulate = Record.Definition.Enabled && Record.Nodes.size() <= Budget - std::min(Budget, UsedVertices);
					if (!Simulate) {
						Result.VerticesTruncated = Result.VerticesTruncated || Record.Definition.Enabled;
						auto ExtractionStart = ProfileClock::now();
						Result.States.push_back(ExtractState(Body, Record, false));
						Result.Profile.ExtractionNanoseconds += Nanoseconds(ProfileClock::now() - ExtractionStart);
						continue;
					}
					UsedVertices += Record.Nodes.size();
					++Result.Profile.SimulatedBodies;
					Result.Profile.SimulatedVertices += Record.Nodes.size();
					Result.Profile.ConstraintCount += Record.Constraints.size();

					const auto IntegrationStart = ProfileClock::now();
					std::size_t MovableCount = 0;
					for (const auto &Node : Record.Nodes) if (Node.InverseMass > 0.0f) ++MovableCount;
					const auto TotalMass = static_cast<float>(MovableCount) * Record.Definition.Material.ParticleMass;
					const auto Damping = std::clamp(1.0f - Record.Definition.Material.Damping, 0.0f, 1.0f);
					for (auto &Node : Record.Nodes) {
						Node.PreviousPosition = Node.Position;
						if (Node.InverseMass == 0.0f || MovableCount == 0) continue;
						Node.Velocity += Config.Gravity * Config.DeltaTime;
						Node.Velocity += Record.PendingForce / TotalMass * Config.DeltaTime;
						Node.Velocity += Record.PendingImpulse / TotalMass;
						Node.Velocity *= Damping;
						Node.Position += Node.Velocity * Config.DeltaTime;
					}
					Record.PendingForce = {};
					Record.PendingImpulse = {};
					Result.Profile.IntegrationNanoseconds += Nanoseconds(ProfileClock::now() - IntegrationStart);

					for (auto &Constraint : Record.Constraints) Constraint.Lambda = 0.0f;
					const auto Iterations = IterationCount(Record);
					for (int Iteration = 0; Iteration < Iterations; ++Iteration) {
						const auto ConstraintStart = ProfileClock::now();
						SolveDistance(Record, Config.DeltaTime);
						SolveShapeRecovery(Record, Config.DeltaTime);
						Result.Profile.ConstraintNanoseconds += Nanoseconds(ProfileClock::now() - ConstraintStart);
						const auto CollisionStart = ProfileClock::now();
						Collide(Record, Colliders, Iteration + 1 == Iterations);
						Result.Profile.CollisionNanoseconds += Nanoseconds(ProfileClock::now() - CollisionStart);
						ApplyAttachments(Record);
					}
					for (auto &Node : Record.Nodes)
						Node.Velocity = Node.InverseMass == 0.0f ? glm::vec3{} :
							(Node.Position - Node.PreviousPosition) / Config.DeltaTime;
					++Record.VertexRevision;
					const auto ExtractionStart = ProfileClock::now();
					Result.States.push_back(ExtractState(Body, Record, true));
					Result.Profile.ExtractionNanoseconds += Nanoseconds(ProfileClock::now() - ExtractionStart);
				}

				Result.Profile.EstimatedBytes = sizeof(*this) + Slots.capacity() * sizeof(SoftBodySlot);
				for (const auto &Slot : Slots) if (Slot.Record) {
					Result.Profile.EstimatedBytes += Slot.Record->Nodes.capacity() * sizeof(SoftBodyNode);
					Result.Profile.EstimatedBytes += Slot.Record->Constraints.capacity() * sizeof(SoftBodyDistanceConstraint);
					if (Slot.Record->Indices)
						Result.Profile.EstimatedBytes += Slot.Record->Indices->capacity() * sizeof(std::uint32_t);
				}
				return Result;
			}

			[[nodiscard]] const SoftBodyWorldLimits &GetLimits() const override { return Limits; }

		  private:
			SoftBodyWorldLimits Limits;
			std::vector<SoftBodySlot> Slots = std::vector<SoftBodySlot>(1);
			std::vector<std::uint32_t> FreeSlots;
			std::size_t LiveBodies = 0;
			std::size_t LiveVertices = 0;
			std::size_t LiveConstraints = 0;

			[[nodiscard]] SoftBodyRecord *Find(SoftBodyId Body) {
				if (!Body.IsValid() || Body.Slot >= Slots.size()) return nullptr;
				auto &Slot = Slots[Body.Slot];
				return Slot.Generation == Body.Generation && Slot.Record ? &*Slot.Record : nullptr;
			}
			[[nodiscard]] const SoftBodyRecord *Find(SoftBodyId Body) const {
				if (!Body.IsValid() || Body.Slot >= Slots.size()) return nullptr;
				const auto &Slot = Slots[Body.Slot];
				return Slot.Generation == Body.Generation && Slot.Record ? &*Slot.Record : nullptr;
			}

			[[nodiscard]] std::optional<SoftBodyRecord> ValidateAndBuild(const SoftBodyDefinition &Definition) const {
				if (!IsFinite(Definition.Position) || !IsFinite(Definition.Size) || Definition.Size.x <= 0.0f ||
					Definition.Size.y <= 0.0f || Definition.Size.z <= 0.0f || !ValidMaterial(Definition.Material) ||
					Definition.Attachments.size() > Limits.MaximumAttachments) return std::nullopt;
				const auto Count = VertexCount(Definition);
				if (!Count || *Count > Limits.MaximumVerticesPerBody) return std::nullopt;
				for (const auto &Attachment : Definition.Attachments)
					if (!IsFinite(Attachment.Position) || Attachment.Vertex >= *Count) return std::nullopt;
				auto Record = BuildRecord(Definition);
				if (!Record || Record->Constraints.size() > Limits.MaximumConstraints) return std::nullopt;
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
}
