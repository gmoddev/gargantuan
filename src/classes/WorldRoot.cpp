#include "gargantuan/classes/WorldRoot.hpp"
#include "gargantuan/classes/BasePart.hpp"
#include "gargantuan/classes/Constraint.hpp"
#include "gargantuan/classes/Instance.hpp"
#include "gargantuan/datatypes/CFrame.hpp"
#include "gargantuan/physics/Conversions.hpp"

#include <SDL3/SDL.h>

#include <box3d/box3d.h>
#include <box3d/collision.h>
#include <box3d/id.h>
#include <box3d/math_functions.h>
#include <box3d/types.h>
#include <cstddef>
#include <memory>
#include <optional>
#include <unordered_map>
#include <variant>
#include <vector>

namespace gargantuan {
	const auto ZERO_VEC3 = glm::vec3(0.0f, 0.0f, 0.0f).value;

	// TODO: Observe properties and reconstruct physics objects when they
	// change

	b3BodyId WorldRoot::CreatePartBody(std::shared_ptr<BasePart> it) {
		if (!it || it->GetDestroyed() || it->IsDestroying() || !b3World_IsValid(World)) return b3_nullBodyId;

		b3BodyDef bodyDefinition = b3DefaultBodyDef();
		bodyDefinition.position = ToBox3(it->GetCFrame().Position);
		bodyDefinition.rotation = ToBox3(it->GetCFrame().ToQuaternion());

		b3ShapeDef shapeDefinition = b3DefaultShapeDef();

		if (it->GetAnchored()) {
			bodyDefinition.type = (b3BodyType)b3_staticBody;
		} else {
			bodyDefinition.type = (b3BodyType)b3_dynamicBody;
			shapeDefinition.density = 0.7f;
		};

		if (it->GetCanCollide()) {
			shapeDefinition.isSensor = false;
		} else {
			shapeDefinition.isSensor = true;
			shapeDefinition.enableSensorEvents = it->GetCanTouch();
		}

		bodyDefinition.userData = it.get();

		b3BodyId bodyId = b3CreateBody(World, &bodyDefinition);
		if (!b3Body_IsValid(bodyId)) return b3_nullBodyId;
		it->CreateBodyShape(bodyId, shapeDefinition);
		Parts.push_back(it);
		PartBodies[it.get()] = bodyId;
		return bodyId;
	}

	b3JointId WorldRoot::CreateConstraintJoint(std::shared_ptr<Constraint> it) {
		if (!it || it->GetDestroyed() || it->IsDestroying() || !b3World_IsValid(World)) return b3_nullJointId;
		auto [part0, part1] = it->GetActiveParts();
		if (!part0 || !part1 || part0 == part1 || part0->GetDestroyed() || part1->GetDestroyed() ||
			part0->IsDestroying() || part1->IsDestroying())
			return b3_nullJointId;

		auto constraintWorld = it->FindFirstAncestorWhichIsA("WorldRoot");
		auto part0World = part0->FindFirstAncestorWhichIsA("WorldRoot");
		auto part1World = part1->FindFirstAncestorWhichIsA("WorldRoot");
		if (constraintWorld.get() != this || part0World.get() != this || part1World.get() != this)
			return b3_nullJointId;

		if (auto existing = ConstraintJoints.find(it.get()); existing != ConstraintJoints.end()) {
			return b3Joint_IsValid(existing->second) ? existing->second : b3_nullJointId;
		}

		auto getOrCreateBody = [this](const std::shared_ptr<BasePart> &part) {
			if (auto existing = PartBodies.find(part.get()); existing != PartBodies.end()) {
				return b3Body_IsValid(existing->second) ? existing->second : b3_nullBodyId;
			}
			return CreatePartBody(part);
		};

		b3BodyId body0 = getOrCreateBody(part0);
		if (!b3Body_IsValid(body0)) return b3_nullJointId;
		b3BodyId body1 = getOrCreateBody(part1);
		if (!b3Body_IsValid(body1)) return b3_nullJointId;

		b3JointId joint = it->CreateJoint(&World, body0, body1);
		if (!b3Joint_IsValid(joint)) return b3_nullJointId;
		ConstraintJoints[it.get()] = joint;
		return joint;
	}

	WorldRoot::WorldRoot() {
		b3WorldDef worldDefinition = b3DefaultWorldDef();
		worldDefinition.enableSleep = true;
		worldDefinition.gravity = b3Vec3{0.0f, -Gravity, 0.0f};

		World = b3CreateWorld(&worldDefinition);

		Destroying->Once([this](std::monostate _) {
			for (auto &[_, joint] : ConstraintJoints) {
				if (b3Joint_IsValid(joint)) b3DestroyJoint(joint, false);
			};
			ConstraintJoints.clear();

			for (auto &[_, body] : PartBodies) {
				if (b3Body_IsValid(body)) b3DestroyBody(body);
			};
			PartBodies.clear();

			if (b3World_IsValid(World)) b3DestroyWorld(World);
		});

		BindDescendants([this](std::shared_ptr<Instance> instance) -> void {
			if (auto it = std::dynamic_pointer_cast<BasePart>(instance)) {
				CreatePartBody(it);
			} else if (auto it = std::dynamic_pointer_cast<Constraint>(instance)) {
				CreateConstraintJoint(it);
			}
		});

		DescendantRemoved->Connect([this](std::shared_ptr<Instance> instance) {
			if (auto it = std::dynamic_pointer_cast<BasePart>(instance); it && PartBodies.contains(it.get())) {
				erase(Parts, it);
				if (b3Body_IsValid(this->PartBodies[it.get()])) b3DestroyBody(this->PartBodies[it.get()]);
				this->PartBodies.erase(it.get());
				std::erase_if(this->ConstraintJoints, [](const auto &entry) {
					return !b3Joint_IsValid(entry.second);
				});
			} else if (auto it = std::dynamic_pointer_cast<Constraint>(instance);
					   it && ConstraintJoints.contains(it.get())) {
				if (b3Joint_IsValid(this->ConstraintJoints[it.get()]))
					b3DestroyJoint(this->ConstraintJoints[it.get()], true);
				this->ConstraintJoints.erase(it.get());
			}
		});
	};

	void WorldRoot::StepPhysics(double deltaTime, std::optional<std::vector<std::shared_ptr<Instance>>> instances) {
		StepAccumulator += deltaTime;

		int steps = 0;
		while (StepAccumulator >= STEP_INTERVAL && steps < MAX_STEPS_PER_FRAME) {
			b3World_Step(World, STEP_INTERVAL, SUB_STEP_COUNT);

			b3BodyEvents events = b3World_GetBodyEvents(World);
			b3ContactEvents contactEvents = b3World_GetContactEvents(World);

			for (int i = 0; i < events.moveCount; ++i) {
				const b3BodyMoveEvent &move = events.moveEvents[i];

				BasePart *part = static_cast<BasePart *>(move.userData);
				if (part == nullptr) continue;

				part->SetCFrame(
					gargantuan::CFrame(FromBox3(move.transform.p), glm::mat3_cast(FromBox3(move.transform.q)))
				);

				if (auto body = PartBodies[part]; part->AccumulatedImpulse.value > ZERO_VEC3) {
					b3Body_ApplyLinearImpulseToCenter(body, ToBox3(part->AccumulatedImpulse), true);
					part->AccumulatedImpulse = {};
				}
			}

			for (int i = 0; i < contactEvents.beginCount; ++i) {
				const b3ContactBeginTouchEvent &event = contactEvents.beginEvents[i];

				BasePart *partA = static_cast<BasePart *>(b3Body_GetUserData(b3Shape_GetBody(event.shapeIdA)));
				if (partA == nullptr) continue;

				BasePart *partB = static_cast<BasePart *>(b3Body_GetUserData(b3Shape_GetBody(event.shapeIdB)));
				if (partB == nullptr) continue;

				partA->Touched->Fire(std::static_pointer_cast<BasePart>(partB->shared_from_this()));
				partB->Touched->Fire(std::static_pointer_cast<BasePart>(partA->shared_from_this()));
			}

			for (int i = 0; i < contactEvents.endCount; ++i) {
				const b3ContactEndTouchEvent &event = contactEvents.endEvents[i];

				BasePart *partA = static_cast<BasePart *>(b3Body_GetUserData(b3Shape_GetBody(event.shapeIdA)));
				if (partA == nullptr) continue;

				BasePart *partB = static_cast<BasePart *>(b3Body_GetUserData(b3Shape_GetBody(event.shapeIdB)));
				if (partB == nullptr) continue;

				partA->TouchEnded->Fire(std::static_pointer_cast<BasePart>(partB->shared_from_this()));
				partB->TouchEnded->Fire(std::static_pointer_cast<BasePart>(partA->shared_from_this()));
			}

			StepAccumulator -= STEP_INTERVAL;
			++steps;
		}

		if (steps == MAX_STEPS_PER_FRAME) StepAccumulator = 0.0f;
	}

}
