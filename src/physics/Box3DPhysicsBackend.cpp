#include "gargantuan/physics/PhysicsBackend.hpp"
#include "physics/Box3DConversions.hpp"

#include <box3d/box3d.h>
#include <box3d/collision.h>
#include <box3d/id.h>
#include <box3d/types.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <limits>
#include <memory>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gargantuan {
	namespace {
		constexpr int CylinderSides = 24;

		[[nodiscard]] bool IsFinite(const glm::vec3 &Value) {
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		[[nodiscard]] bool IsFinite(const CFrame &Value) {
			if (!IsFinite(Value.Position)) return false;
			for (int Column = 0; Column < 3; ++Column)
				for (int Row = 0; Row < 3; ++Row)
					if (!std::isfinite(Value.Rotation[Column][Row])) return false;
			return true;
		}

		[[nodiscard]] bool IsValidBodyDescription(const PhysicsBodyDesc &Description) {
			return IsFinite(Description.Transform) && IsFinite(Description.Shape.Size) &&
				Description.Shape.Size.x > 0.0f && Description.Shape.Size.y > 0.0f &&
				Description.Shape.Size.z > 0.0f && std::isfinite(Description.Density) && Description.Density >= 0.0f;
		}

		[[nodiscard]] bool SameSize(const glm::vec3 &A, const glm::vec3 &B) {
			return A.x == B.x && A.y == B.y && A.z == B.z;
		}

		class Box3DPhysicsBackend final : public IPhysicsBackend {
		  public:
			explicit Box3DPhysicsBackend(const PhysicsWorldConfig &Config) {
				if (!IsFinite(Config.Gravity)) return;
				b3WorldDef Definition = b3DefaultWorldDef();
				Definition.enableSleep = Config.EnableSleep;
				Definition.gravity = Box3DConversions::ToBox3(Config.Gravity);
				World = b3CreateWorld(&Definition);
				BodySlots.push_back({});
				ConstraintSlots.push_back({});
			}

			~Box3DPhysicsBackend() override {
				if (!b3World_IsValid(World)) return;
				for (std::size_t Index = 1; Index < ConstraintSlots.size(); ++Index) {
					auto &Entry = ConstraintSlots[Index];
					if (Entry.Record && b3Joint_IsValid(Entry.Record->Joint)) b3DestroyJoint(Entry.Record->Joint, false);
					Entry.Record.reset();
				}
				for (std::size_t Index = 1; Index < BodySlots.size(); ++Index) {
					auto &Entry = BodySlots[Index];
					if (Entry.Record && b3Body_IsValid(Entry.Record->Body)) b3DestroyBody(Entry.Record->Body);
					Entry.Record.reset();
				}
				ShapeOwners.clear();
				b3DestroyWorld(World);
				World = b3_nullWorldId;
			}

			[[nodiscard]] bool IsValid() const override { return b3World_IsValid(World); }

			[[nodiscard]] bool IsBodyValid(PhysicsBodyId Body) const override {
				return FindBody(Body) != nullptr;
			}

			[[nodiscard]] bool IsConstraintValid(PhysicsConstraintId Constraint) const override {
				return FindConstraint(Constraint) != nullptr;
			}

			[[nodiscard]] PhysicsBodyId CreateBody(const PhysicsBodyDesc &Description) override {
				if (!b3World_IsValid(World) || !IsValidBodyDescription(Description)) return {};
				const auto Id = AllocateBody();
				auto &Entry = BodySlots[Id.Slot];
				Entry.Record = std::make_unique<BodyRecord>();
				Entry.Record->Id = Id;
				Entry.Record->Description = Description;

				b3BodyDef BodyDefinition = b3DefaultBodyDef();
				BodyDefinition.position = Box3DConversions::ToBox3(Description.Transform.Position);
				BodyDefinition.rotation = Box3DConversions::ToBox3(Description.Transform.ToQuaternion());
				BodyDefinition.type = Description.Anchored ? b3_staticBody : b3_dynamicBody;
				BodyDefinition.userData = Entry.Record.get();
				Entry.Record->Body = b3CreateBody(World, &BodyDefinition);
				if (!b3Body_IsValid(Entry.Record->Body)) {
					ReleaseBody(Id);
					return {};
				}

				Entry.Record->Shape = CreateShape(Entry.Record->Body, Id, Description, true);
				if (!b3Shape_IsValid(Entry.Record->Shape)) {
					b3DestroyBody(Entry.Record->Body);
					ReleaseBody(Id);
					return {};
				}
				return Id;
			}

			PhysicsOperationResult UpdateBody(
				PhysicsBodyId Body,
				const PhysicsBodyDesc &Description
			) override {
				auto *Record = FindBody(Body);
				if (!Record) return InvalidBody();
				if (!IsValidBodyDescription(Description))
					return {PhysicsOperationStatus::InvalidDescription, "Physics body description is invalid"};

				const auto &Previous = Record->Description;
				const bool RebuildShape = Previous.Shape.Kind != Description.Shape.Kind ||
					!SameSize(Previous.Shape.Size, Description.Shape.Size) ||
					Previous.CanCollide != Description.CanCollide || Previous.Density != Description.Density;
				if (RebuildShape) {
					const auto Replacement = CreateShape(Record->Body, Body, Description, false);
					if (!b3Shape_IsValid(Replacement))
						return {PhysicsOperationStatus::BackendFailure, "Physics shape replacement failed"};
					ShapeOwners.erase(b3StoreShapeId(Record->Shape));
					b3DestroyShape(Record->Shape, false);
					Record->Shape = Replacement;
					b3Body_ApplyMassFromShapes(Record->Body);
				} else if (Previous.CanTouch != Description.CanTouch) {
					b3Shape_EnableSensorEvents(Record->Shape, Description.CanTouch);
					b3Shape_EnableContactEvents(Record->Shape, Description.CanCollide && Description.CanTouch);
				}

				if (Previous.Anchored != Description.Anchored)
					b3Body_SetType(Record->Body, Description.Anchored ? b3_staticBody : b3_dynamicBody);
				b3Body_SetTransform(
					Record->Body,
					Box3DConversions::ToBox3(Description.Transform.Position),
					Box3DConversions::ToBox3(Description.Transform.ToQuaternion())
				);
				Record->Description = Description;
				return {};
			}

			PhysicsOperationResult DestroyBody(PhysicsBodyId Body) override {
				auto *Record = FindBody(Body);
				if (!Record) return InvalidBody();
				std::vector<PhysicsConstraintId> Attached;
				for (std::size_t Index = 1; Index < ConstraintSlots.size(); ++Index) {
					const auto &Entry = ConstraintSlots[Index];
					if (!Entry.Record) continue;
					if (Entry.Record->Description.BodyA == Body || Entry.Record->Description.BodyB == Body)
						Attached.push_back({static_cast<std::uint32_t>(Index), Entry.Generation});
				}
				for (const auto Constraint : Attached) DestroyConstraint(Constraint);
				ShapeOwners.erase(b3StoreShapeId(Record->Shape));
				if (b3Body_IsValid(Record->Body)) b3DestroyBody(Record->Body);
				ReleaseBody(Body);
				return {};
			}

			[[nodiscard]] PhysicsConstraintId CreateConstraint(
				const PhysicsConstraintDesc &Description
			) override {
				auto *BodyA = FindBody(Description.BodyA);
				auto *BodyB = FindBody(Description.BodyB);
				if (!BodyA || !BodyB || Description.BodyA == Description.BodyB) return {};
				const auto Id = AllocateConstraint();
				auto &Entry = ConstraintSlots[Id.Slot];
				Entry.Record = std::make_unique<ConstraintRecord>();
				Entry.Record->Id = Id;
				Entry.Record->Description = Description;
				switch (Description.Kind) {
				case PhysicsConstraintKind::Weld: {
					b3WeldJointDef Definition = b3DefaultWeldJointDef();
					Definition.base.bodyIdA = BodyA->Body;
					Definition.base.bodyIdB = BodyB->Body;
					Definition.base.collideConnected = Description.CollideConnected;
					Entry.Record->Joint = b3CreateWeldJoint(World, &Definition);
					break;
				}
				}
				if (!b3Joint_IsValid(Entry.Record->Joint)) {
					ReleaseConstraint(Id);
					return {};
				}
				return Id;
			}

			PhysicsOperationResult DestroyConstraint(PhysicsConstraintId Constraint) override {
				auto *Record = FindConstraint(Constraint);
				if (!Record)
					return {PhysicsOperationStatus::InvalidId, "Physics constraint identity is invalid or stale"};
				if (b3Joint_IsValid(Record->Joint)) b3DestroyJoint(Record->Joint, false);
				ReleaseConstraint(Constraint);
				return {};
			}

			PhysicsOperationResult ApplyLinearImpulse(PhysicsBodyId Body, glm::vec3 Impulse) override {
				auto *Record = FindBody(Body);
				if (!Record) return InvalidBody();
				if (!IsFinite(Impulse))
					return {PhysicsOperationStatus::InvalidDescription, "Physics impulse is not finite"};
				b3Body_ApplyLinearImpulseToCenter(Record->Body, Box3DConversions::ToBox3(Impulse), true);
				return {};
			}

			PhysicsOperationResult SetGravity(glm::vec3 Gravity) override {
				if (!b3World_IsValid(World))
					return {PhysicsOperationStatus::BackendFailure, "Physics world is unavailable"};
				if (!IsFinite(Gravity))
					return {PhysicsOperationStatus::InvalidDescription, "Physics gravity is not finite"};
				b3World_SetGravity(World, Box3DConversions::ToBox3(Gravity));
				return {};
			}

			[[nodiscard]] std::optional<PhysicsBodyState> GetBodyState(PhysicsBodyId Body) const override {
				const auto *Record = FindBody(Body);
				if (!Record) return std::nullopt;
				return PhysicsBodyState{
					.Transform = Box3DConversions::FromBox3(b3Body_GetTransform(Record->Body)),
					.LinearVelocity = Box3DConversions::FromBox3(b3Body_GetLinearVelocity(Record->Body)),
					.Description = Record->Description,
				};
			}

			[[nodiscard]] PhysicsKinematicMotionResult
			MoveKinematicCapsule(const PhysicsKinematicMotionRequest &Request) const override {
				PhysicsKinematicMotionResult Result{
					.Position = Request.Position,
					.Velocity = Request.Velocity,
				};
				if (!b3World_IsValid(World)) {
					Result.Status = PhysicsOperationStatus::BackendFailure;
					Result.Message = "Physics world is unavailable";
					return Result;
				}
				if (!IsFinite(Request.Position) || !IsFinite(Request.Translation) || !IsFinite(Request.Velocity) ||
					!std::isfinite(Request.Radius) || !std::isfinite(Request.Height) || Request.Radius <= 0.0f ||
					Request.Height < Request.Radius * 2.0f) {
					Result.Status = PhysicsOperationStatus::InvalidDescription;
					Result.Message = "Kinematic capsule request is invalid";
					return Result;
				}

				const float HalfSegment = std::max(0.0f, Request.Height * 0.5f - Request.Radius);
				const b3Capsule Mover{
					.center1 = {0.0f, -HalfSegment, 0.0f},
					.center2 = {0.0f, HalfSegment, 0.0f},
					.radius = Request.Radius,
				};
				auto AcceptCollidable = [](b3ShapeId Shape, void *Context) {
					auto *Self = static_cast<const Box3DPhysicsBackend *>(Context);
					auto Owner = Self->FindShapeOwner(Shape);
					if (!Owner) return false;
					const auto *Record = Self->FindBody(*Owner);
					return Record && Record->Description.CanCollide;
				};
				struct PlaneContext {
					const Box3DPhysicsBackend *Self = nullptr;
					std::vector<b3CollisionPlane> Planes;
					glm::vec3 ContactNormal{0.0f};
					glm::vec3 FloorNormal{0.0f};
					float ContactScore = 1.0f;
					float FloorScore = 0.0f;
					glm::vec3 MotionDirection{0.0f};
					bool Truncated = false;
				} Context;
				Context.Self = this;
				Context.Planes.reserve(MaximumKinematicCollisionPlanes);
				Context.MotionDirection = Request.Translation == glm::vec3(0.0f) ? glm::vec3(0.0f)
																				 : glm::normalize(Request.Translation);

				auto CollectPlanes =
					[](b3ShapeId Shape, const b3PlaneResult *Planes, int PlaneCount, void *RawContext) {
						auto *Context = static_cast<PlaneContext *>(RawContext);
						auto Owner = Context->Self->FindShapeOwner(Shape);
						if (!Owner) return true;
						const auto *Record = Context->Self->FindBody(*Owner);
						if (!Record || !Record->Description.CanCollide) return true;
						for (int Index = 0; Index < PlaneCount; ++Index) {
							if (Context->Planes.size() == MaximumKinematicCollisionPlanes) {
								Context->Truncated = true;
								return false;
							}
							Context->Planes.push_back({
								.plane = Planes[Index].plane,
								.pushLimit = FLT_MAX,
								.push = 0.0f,
								.clipVelocity = true,
							});
							const auto Normal = Box3DConversions::FromBox3(Planes[Index].plane.normal);
							if (Normal.y > Context->FloorScore) {
								Context->FloorScore = Normal.y;
								Context->FloorNormal = Normal;
							}
							const float Score = glm::dot(Normal, Context->MotionDirection);
							if (Score < Context->ContactScore) {
								Context->ContactScore = Score;
								Context->ContactNormal = Normal;
							}
						}
						return true;
					};
				auto GatherPlanes = [&](const glm::vec3 &Position) {
					Context.Planes.clear();
					Context.ContactNormal = {};
					Context.FloorNormal = {};
					Context.ContactScore = 1.0f;
					Context.FloorScore = 0.0f;
					Context.Truncated = false;
					const b3Pos Origin{Position.x, Position.y, Position.z};
					b3World_CollideMover(World, Origin, &Mover, b3DefaultQueryFilter(), CollectPlanes, &Context);
				};

				const glm::vec3 TargetPosition = Request.Position + Request.Translation;
				glm::vec3 CurrentPosition = Request.Position;
				bool Collided = false;
				bool PlanesTruncated = false;
				constexpr float MotionToleranceSquared = 0.0001f;
				for (std::size_t Iteration = 0; Iteration < MaximumKinematicMotionIterations; ++Iteration) {
					GatherPlanes(CurrentPosition);
					Collided = Collided || !Context.Planes.empty();
					PlanesTruncated = PlanesTruncated || Context.Truncated;
					const auto TargetDelta = TargetPosition - CurrentPosition;
					const auto SolvedDelta = Box3DConversions::FromBox3(
						b3SolvePlanes(
							Box3DConversions::ToBox3(TargetDelta),
							Context.Planes.data(),
							static_cast<int>(Context.Planes.size())
						).delta
					);
					if (glm::dot(SolvedDelta, SolvedDelta) < MotionToleranceSquared) break;

					const b3Pos Origin{CurrentPosition.x, CurrentPosition.y, CurrentPosition.z};
					const auto Box3Delta = Box3DConversions::ToBox3(SolvedDelta);
					const float Fraction = std::clamp(
						b3World_CastMover(
							World,
							Origin,
							&Mover,
							Box3Delta,
							b3DefaultQueryFilter(),
							AcceptCollidable,
							const_cast<Box3DPhysicsBackend *>(this)
						),
						0.0f,
						1.0f
					);
					Collided = Collided || Fraction < 1.0f;
					const auto AppliedDelta = SolvedDelta * Fraction;
					CurrentPosition += AppliedDelta;
					if (glm::dot(AppliedDelta, AppliedDelta) < MotionToleranceSquared) break;
				}

				GatherPlanes(CurrentPosition);
				Collided = Collided || !Context.Planes.empty();
				PlanesTruncated = PlanesTruncated || Context.Truncated;
				if (!Context.Planes.empty()) {
					const auto Correction = Box3DConversions::FromBox3(
						b3SolvePlanes(b3Vec3_zero, Context.Planes.data(), static_cast<int>(Context.Planes.size())).delta
					);
					CurrentPosition += Correction;
					Result.Velocity = Box3DConversions::FromBox3(b3ClipVector(
						Box3DConversions::ToBox3(Request.Velocity),
						Context.Planes.data(),
						static_cast<int>(Context.Planes.size())
					));
				}
				Result.Position = CurrentPosition;
				Result.AppliedTranslation = CurrentPosition - Request.Position;
				Result.ContactNormal = Context.ContactNormal;
				Result.FloorNormal = Context.FloorNormal;
				Result.HasFloor = Context.FloorScore > 0.0f;
				Result.Collided = Collided;
				Result.PlanesTruncated = PlanesTruncated;
				return Result;
			}

			[[nodiscard]] PhysicsRaycastResult Raycast(const PhysicsRaycastRequest &Request) const override {
				PhysicsRaycastResult Result;
				if (!b3World_IsValid(World)) {
					Result.Status = PhysicsOperationStatus::BackendFailure;
					Result.Message = "Physics world is unavailable";
					return Result;
				}
				const float Distance = glm::length(Request.Direction);
				if (!IsFinite(Request.Origin) || !IsFinite(Request.Direction) || !std::isfinite(Distance) ||
					Distance < MinimumRaycastDistance || Distance > MaximumRaycastDistance ||
					Request.Filter.Bodies.size() > MaximumRaycastFilterBodies) {
					Result.Status = PhysicsOperationStatus::InvalidDescription;
					Result.Message = "Physics raycast request is invalid or exceeds a query bound";
					return Result;
				}

				struct RayHit {
					PhysicsBodyId Body{};
					glm::vec3 Position{0.0f};
					glm::vec3 Normal{0.0f};
					float Fraction = 0.0f;
				};
				struct RayContext {
					const Box3DPhysicsBackend *Self = nullptr;
					const PhysicsQueryFilter *Filter = nullptr;
					std::vector<RayHit> Hits;
					bool Truncated = false;
				} Context{.Self = this, .Filter = &Request.Filter};
				Context.Hits.reserve(std::min<std::size_t>(BodySlots.size(), MaximumRaycastCandidates));

				auto CollectHit = [](b3ShapeId Shape,
									 b3Pos Point,
									 b3Vec3 Normal,
									 float Fraction,
									 std::uint64_t,
									 int,
									 int,
									 void *RawContext) {
					auto *Context = static_cast<RayContext *>(RawContext);
					auto Owner = Context->Self->FindShapeOwner(Shape);
					if (!Owner) return 1.0f;
					const auto *Record = Context->Self->FindBody(*Owner);
					if (!Record || !Record->Description.CanCollide) return 1.0f;
					const bool Listed = std::binary_search(
						Context->Filter->Bodies.begin(), Context->Filter->Bodies.end(), *Owner
					);
					const bool Accepted = Context->Filter->Type == PhysicsQueryFilterType::Include ? Listed : !Listed;
					if (!Accepted) return 1.0f;
					if (Context->Hits.size() == MaximumRaycastCandidates) {
						Context->Truncated = true;
						return 0.0f;
					}
					Context->Hits.push_back({
						.Body = *Owner,
						.Position =
							{static_cast<float>(Point.x), static_cast<float>(Point.y), static_cast<float>(Point.z)},
						.Normal = Box3DConversions::FromBox3(Normal),
						.Fraction = Fraction,
					});
					return 1.0f;
				};

				(void)b3World_CastRay(
					World,
					b3Pos{Request.Origin.x, Request.Origin.y, Request.Origin.z},
					Box3DConversions::ToBox3(Request.Direction),
					b3DefaultQueryFilter(),
					CollectHit,
					&Context
				);
				if (Context.Truncated) {
					Result.Status = PhysicsOperationStatus::BackendFailure;
					Result.Message = "Physics raycast candidate limit reached";
					Result.CandidatesTruncated = true;
					return Result;
				}
				if (Context.Hits.empty()) return Result;

				std::ranges::sort(Context.Hits, [](const RayHit &Left, const RayHit &Right) {
					if (Left.Fraction != Right.Fraction) return Left.Fraction < Right.Fraction;
					return Left.Body < Right.Body;
				});
				Result.Candidates.reserve(Context.Hits.size());
				for (const auto &Hit : Context.Hits) {
					const float NormalLength = glm::length(Hit.Normal);
					if (!IsFinite(Hit.Position) || !IsFinite(Hit.Normal) || !std::isfinite(Hit.Fraction) ||
						Hit.Fraction < 0.0f || Hit.Fraction > 1.0f || !std::isfinite(NormalLength) ||
						NormalLength < MinimumRaycastDistance) {
						Result.Status = PhysicsOperationStatus::BackendFailure;
						Result.Message = "Physics backend returned an invalid raycast hit";
						Result.Candidates.clear();
						return Result;
					}
					Result.Candidates.push_back({
						.Body = Hit.Body,
						.Position = Hit.Position,
						.Normal = Hit.Normal / NormalLength,
						.Distance = Hit.Fraction * Distance,
					});
				}
				return Result;
			}

			[[nodiscard]] PhysicsStepResult Step(const PhysicsStepConfig &Config) override {
				PhysicsStepResult Result;
				if (!b3World_IsValid(World) || !std::isfinite(Config.DeltaTime) || Config.DeltaTime <= 0.0f ||
					Config.SubStepCount <= 0) return Result;
				b3World_Step(World, Config.DeltaTime, Config.SubStepCount);

				const auto BodyEvents = b3World_GetBodyEvents(World);
				const auto MotionCount = std::min(
					static_cast<std::size_t>(std::max(BodyEvents.moveCount, 0)),
					MaximumPhysicsEventsPerStep
				);
				Result.Motions.reserve(MotionCount);
				Result.EventsTruncated = static_cast<std::size_t>(std::max(BodyEvents.moveCount, 0)) > MotionCount;
				for (std::size_t Index = 0; Index < MotionCount; ++Index) {
					const auto &Move = BodyEvents.moveEvents[Index];
					auto *Record = static_cast<BodyRecord *>(Move.userData);
					if (!Record || FindBody(Record->Id) != Record) continue;
					Record->Description.Transform = Box3DConversions::FromBox3(Move.transform);
					Result.Motions.push_back({Record->Id, Record->Description.Transform});
				}

				std::set<std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t, PhysicsContactPhase>> Seen;
				auto AddContact = [&](b3ShapeId ShapeA, b3ShapeId ShapeB, PhysicsContactPhase Phase) {
					if (Result.Contacts.size() >= MaximumPhysicsEventsPerStep) {
						Result.EventsTruncated = true;
						return;
					}
					auto BodyA = FindShapeOwner(ShapeA);
					auto BodyB = FindShapeOwner(ShapeB);
					if (!BodyA || !BodyB || *BodyA == *BodyB) return;
					if (*BodyB < *BodyA) std::swap(BodyA, BodyB);
					auto Key = std::tuple{
						BodyA->Slot, BodyA->Generation, BodyB->Slot, BodyB->Generation, Phase
					};
					if (Seen.insert(Key).second) Result.Contacts.push_back({*BodyA, *BodyB, Phase});
				};

				const auto ContactEvents = b3World_GetContactEvents(World);
				for (int Index = 0; Index < ContactEvents.beginCount; ++Index)
					AddContact(
						ContactEvents.beginEvents[Index].shapeIdA,
						ContactEvents.beginEvents[Index].shapeIdB,
						PhysicsContactPhase::Began
					);
				for (int Index = 0; Index < ContactEvents.endCount; ++Index)
					AddContact(
						ContactEvents.endEvents[Index].shapeIdA,
						ContactEvents.endEvents[Index].shapeIdB,
						PhysicsContactPhase::Ended
					);

				const auto SensorEvents = b3World_GetSensorEvents(World);
				for (int Index = 0; Index < SensorEvents.beginCount; ++Index)
					AddContact(
						SensorEvents.beginEvents[Index].sensorShapeId,
						SensorEvents.beginEvents[Index].visitorShapeId,
						PhysicsContactPhase::Began
					);
				for (int Index = 0; Index < SensorEvents.endCount; ++Index)
					AddContact(
						SensorEvents.endEvents[Index].sensorShapeId,
						SensorEvents.endEvents[Index].visitorShapeId,
						PhysicsContactPhase::Ended
					);
				return Result;
			}

		  private:
			struct BodyRecord {
				PhysicsBodyId Id{};
				PhysicsBodyDesc Description{};
				b3BodyId Body{};
				b3ShapeId Shape{};
			};

			struct ConstraintRecord {
				PhysicsConstraintId Id{};
				PhysicsConstraintDesc Description{};
				b3JointId Joint{};
			};

			template <typename RecordType> struct SlotEntry {
				std::uint32_t Generation = 1;
				std::unique_ptr<RecordType> Record;
			};

			b3WorldId World{};
			std::vector<SlotEntry<BodyRecord>> BodySlots;
			std::vector<std::uint32_t> FreeBodySlots;
			std::vector<SlotEntry<ConstraintRecord>> ConstraintSlots;
			std::vector<std::uint32_t> FreeConstraintSlots;
			std::unordered_map<std::uint64_t, PhysicsBodyId> ShapeOwners;

			[[nodiscard]] BodyRecord *FindBody(PhysicsBodyId Id) {
				return const_cast<BodyRecord *>(std::as_const(*this).FindBody(Id));
			}

			[[nodiscard]] const BodyRecord *FindBody(PhysicsBodyId Id) const {
				if (!Id.IsValid() || Id.Slot >= BodySlots.size()) return nullptr;
				const auto &Entry = BodySlots[Id.Slot];
				if (Entry.Generation != Id.Generation || !Entry.Record || !b3Body_IsValid(Entry.Record->Body)) return nullptr;
				return Entry.Record.get();
			}

			[[nodiscard]] ConstraintRecord *FindConstraint(PhysicsConstraintId Id) {
				return const_cast<ConstraintRecord *>(std::as_const(*this).FindConstraint(Id));
			}

			[[nodiscard]] const ConstraintRecord *FindConstraint(PhysicsConstraintId Id) const {
				if (!Id.IsValid() || Id.Slot >= ConstraintSlots.size()) return nullptr;
				const auto &Entry = ConstraintSlots[Id.Slot];
				if (Entry.Generation != Id.Generation || !Entry.Record || !b3Joint_IsValid(Entry.Record->Joint))
					return nullptr;
				return Entry.Record.get();
			}

			[[nodiscard]] PhysicsBodyId AllocateBody() {
				std::uint32_t Slot;
				if (FreeBodySlots.empty()) {
					Slot = static_cast<std::uint32_t>(BodySlots.size());
					BodySlots.push_back({});
				} else {
					Slot = FreeBodySlots.back();
					FreeBodySlots.pop_back();
				}
				return {Slot, BodySlots[Slot].Generation};
			}

			void ReleaseBody(PhysicsBodyId Id) {
				auto &Entry = BodySlots[Id.Slot];
				Entry.Record.reset();
				if (Entry.Generation == std::numeric_limits<std::uint32_t>::max()) return;
				++Entry.Generation;
				FreeBodySlots.push_back(Id.Slot);
			}

			[[nodiscard]] PhysicsConstraintId AllocateConstraint() {
				std::uint32_t Slot;
				if (FreeConstraintSlots.empty()) {
					Slot = static_cast<std::uint32_t>(ConstraintSlots.size());
					ConstraintSlots.push_back({});
				} else {
					Slot = FreeConstraintSlots.back();
					FreeConstraintSlots.pop_back();
				}
				return {Slot, ConstraintSlots[Slot].Generation};
			}

			void ReleaseConstraint(PhysicsConstraintId Id) {
				auto &Entry = ConstraintSlots[Id.Slot];
				Entry.Record.reset();
				if (Entry.Generation == std::numeric_limits<std::uint32_t>::max()) return;
				++Entry.Generation;
				FreeConstraintSlots.push_back(Id.Slot);
			}

			[[nodiscard]] b3ShapeId CreateShape(
				b3BodyId Body,
				PhysicsBodyId Owner,
				const PhysicsBodyDesc &Description,
				bool UpdateMass
			) {
				b3ShapeDef ShapeDefinition = b3DefaultShapeDef();
				ShapeDefinition.density = Description.Density;
				ShapeDefinition.isSensor = !Description.CanCollide;
				ShapeDefinition.enableSensorEvents = Description.CanTouch;
				ShapeDefinition.enableContactEvents = Description.CanCollide && Description.CanTouch;
				ShapeDefinition.updateBodyMass = UpdateMass;
				const auto Size = Description.Shape.Size;
				const auto HalfSize = Size * 0.5f;
				b3ShapeId Shape{};
				switch (Description.Shape.Kind) {
				case PhysicsShapeKind::Box: {
					auto Hull = b3MakeBoxHull(HalfSize.x, HalfSize.y, HalfSize.z);
					Shape = b3CreateHullShape(Body, &ShapeDefinition, &Hull.base);
					break;
				}
				case PhysicsShapeKind::Wedge: {
					b3Vec3 Points[6] = {
						{-HalfSize.x, -HalfSize.y, -HalfSize.z},
						{HalfSize.x, -HalfSize.y, -HalfSize.z},
						{HalfSize.x, -HalfSize.y, HalfSize.z},
						{-HalfSize.x, -HalfSize.y, HalfSize.z},
						{HalfSize.x, HalfSize.y, -HalfSize.z},
						{-HalfSize.x, HalfSize.y, -HalfSize.z},
					};
					auto *Hull = b3CreateHull(Points, 6, 6);
					if (Hull) {
						Shape = b3CreateHullShape(Body, &ShapeDefinition, Hull);
						b3DestroyHull(Hull);
					}
					break;
				}
				case PhysicsShapeKind::CornerWedge: {
					b3Vec3 Points[5] = {
						{-HalfSize.x, -HalfSize.y, -HalfSize.z},
						{HalfSize.x, -HalfSize.y, -HalfSize.z},
						{HalfSize.x, -HalfSize.y, HalfSize.z},
						{-HalfSize.x, -HalfSize.y, HalfSize.z},
						{-HalfSize.x, HalfSize.y, -HalfSize.z},
					};
					auto *Hull = b3CreateHull(Points, 5, 5);
					if (Hull) {
						Shape = b3CreateHullShape(Body, &ShapeDefinition, Hull);
						b3DestroyHull(Hull);
					}
					break;
				}
				case PhysicsShapeKind::Ball: {
					b3Sphere Sphere{.center = {0.0f, 0.0f, 0.0f}, .radius = std::min({HalfSize.x, HalfSize.y, HalfSize.z})};
					Shape = b3CreateSphereShape(Body, &ShapeDefinition, &Sphere);
					break;
				}
				case PhysicsShapeKind::Cylinder: {
					auto *Cylinder = b3CreateCylinder(
						Size.y,
						std::min(HalfSize.x, HalfSize.z * 0.5f),
						0.0f,
						CylinderSides
					);
					if (Cylinder) {
						Shape = b3CreateHullShape(Body, &ShapeDefinition, Cylinder);
						b3DestroyHull(Cylinder);
					}
					break;
				}
				}
				if (b3Shape_IsValid(Shape)) ShapeOwners[b3StoreShapeId(Shape)] = Owner;
				return Shape;
			}

			[[nodiscard]] std::optional<PhysicsBodyId> FindShapeOwner(b3ShapeId Shape) const {
				auto Found = ShapeOwners.find(b3StoreShapeId(Shape));
				if (Found == ShapeOwners.end() || !FindBody(Found->second)) return std::nullopt;
				return Found->second;
			}

			[[nodiscard]] static PhysicsOperationResult InvalidBody() {
				return {PhysicsOperationStatus::InvalidId, "Physics body identity is invalid or stale"};
			}
		};
	}

	std::unique_ptr<IPhysicsBackend> CreatePhysicsBackend(const PhysicsWorldConfig &Config) {
		return std::make_unique<Box3DPhysicsBackend>(Config);
	}
}
