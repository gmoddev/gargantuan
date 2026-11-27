#include "gargantuan/network/SpatialTransferProtection.hpp"
#include "gargantuan/classes/BlockGrid.hpp"
#include "gargantuan/classes/Character.hpp"
#include "gargantuan/classes/DataModel.hpp"
#include "gargantuan/classes/MeshPart.hpp"
#include "gargantuan/classes/SpatialRegion.hpp"
#include "gargantuan/classes/Terrain.hpp"
#include "gargantuan/classes/ViewportFrame.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace gargantuan {
	std::shared_ptr<network::SpatialTransferProtection>
	DataModel::AcquireSpatialTransferProtection(std::function<bool(ObjectId)> IsControlled) {
		AssertCanMutate();
		if (auto Existing = MovementProtection.lock())
			throw std::logic_error("[Spatial:Protection] Session already owns movement admission");
		auto Result = std::make_shared<network::SpatialTransferProtection>(std::move(IsControlled));
		MovementProtection = Result;
		return Result;
	}
}
namespace gargantuan::network {
	namespace {
		SpatialDestinationBounds Bounds(const CFrame &Pose, glm::vec3 Size) {
			const auto Half = glm::abs(glm::dvec3(Size)) * .5;
			const auto Extent = glm::abs(glm::dvec3(Pose.Rotation[0])) * Half.x +
								glm::abs(glm::dvec3(Pose.Rotation[1])) * Half.y +
								glm::abs(glm::dvec3(Pose.Rotation[2])) * Half.z;
			const auto Origin = glm::dvec3(Pose.Position);
			SpatialDestinationBounds Result{glm::vec3(Origin - Extent), glm::vec3(Origin + Extent)};
			for (int I = 0; I < 3; ++I) {
				Result.Minimum[I] = std::nextafter(Result.Minimum[I], -std::numeric_limits<float>::infinity());
				Result.Maximum[I] = std::nextafter(Result.Maximum[I], std::numeric_limits<float>::infinity());
			}
			return Result;
		}
		bool Occupies(
			const SpatialTransferReservations &Holds,
			const Instance &Object,
			const WorldRoot *World,
			const CFrame *Pose = nullptr,
			const glm::vec3 *Size = nullptr
		) {
			if (!World) return false;
			if (const auto *Part = dynamic_cast<const BasePart *>(&Object);
				Part && !dynamic_cast<const MeshPart *>(Part)) {
				return Holds.HoldsBounds(
					World->GetObjectId(), Bounds(Pose ? *Pose : Part->GetCFrame(), Size ? *Size : Part->GetSize())
				);
			}
			if (dynamic_cast<const MeshPart *>(&Object) || dynamic_cast<const BlockGrid *>(&Object) ||
				dynamic_cast<const Terrain *>(&Object))
				return Holds.HoldsDestination(World->GetObjectId());
			return false;
		}
		bool PhysicalProperty(std::string_view Key) {
			return Key == "SpatialRegion" || Key == "CFrame" || Key == "Position" || Key == "Rotation" ||
				   Key == "Size" || Key == "Shape" || Key == "CanCollide" || Key == "Anchored" ||
				   Key == "CollisionGroup" || Key == "CollisionMask" || Key == "Mesh" || Key == "CollisionMode" ||
				   Key == "Kinematic" || Key == "CellSize" || Key == "RootPart" || Key == "CapsuleRadius" ||
				   Key == "CapsuleHeight" || Key == "Velocity" || Key == "GravityScale" || Key == "Friction" ||
				   Key == "Restitution" || Key == "LinearDamping" || Key == "AngularDamping" || Key == "SleepEnabled" ||
				   Key == "MovingBodyCCD" || Key == "Part0" || Key == "Part1" || Key == "Attachment0" ||
				   Key == "Attachment1" || Key == "Enabled";
		}
	}
	SpatialTransferProtection::SpatialTransferProtection(std::function<bool(ObjectId)> Value)
		: Controlled(std::move(Value)) {}
	bool SpatialTransferProtection::Controls(const Instance &Object) const {
		if (!Controlled) return false;
		if (Controlled(Object.GetObjectId())) return true;
		if (const auto *Actor = dynamic_cast<const Character *>(&Object)) {
			const auto Root = Actor->GetRootPart();
			return Root && Controlled((*Root)->GetObjectId());
		}
		return false;
	}
	bool SpatialTransferProtection::AllowsMotion(
		const Instance &Object, const WorldRoot &World, const SpatialDestinationBounds &Swept
	) const {
		return !Reservations.HoldsMember(Object.GetObjectId()) && !Reservations.HoldsBounds(World.GetObjectId(), Swept);
	}
	bool SpatialTransferProtection::AllowsSimulationPose(const BasePart &Object, const CFrame &Pose) const {
		const auto World = WorldRoot::GetEffectiveWorld(Object);
		if (!World) return false;
		const auto From = Bounds(Object.GetCFrame(), Object.GetSize()), To = Bounds(Pose, Object.GetSize());
		return AllowsMotion(Object, *World, {glm::min(From.Minimum, To.Minimum), glm::max(From.Maximum, To.Maximum)});
	}
	bool SpatialTransferProtection::AllowsResource(const Instance &Object) const {
		if (!Reservations.HasHolds()) return true;
		const auto Id = Object.GetObjectId();
		if (Reservations.HoldsMember(Id) || Reservations.HoldsProvider(Id)) return false;
		const auto World = WorldRoot::GetEffectiveWorld(Object);
		return !Occupies(Reservations, Object, World.get());
	}
	bool SpatialTransferProtection::AllowsProperty(
		const Instance &Object, std::string_view Property, const std::any &Value
	) const {
		if (!PhysicalProperty(Property)) return true;
		if (Property == "RootPart")
			if (const auto *Root = std::any_cast<std::optional<std::shared_ptr<BasePart>>>(&Value); Root && *Root)
				if (Controls(***Root) || Reservations.HoldsMember((**Root)->GetObjectId())) return false;
		if (Controls(Object)) return false;
		if (!Reservations.HasHolds() && Property != "SpatialRegion") return true;
		if (!AllowsResource(Object)) return false;
		const auto World = WorldRoot::GetEffectiveWorld(Object);
		if (const auto *Part = dynamic_cast<const BasePart *>(&Object)) {
			if (World && Reservations.HoldsDestination(World->GetObjectId()) &&
				(Property == "Anchored" || Property == "Kinematic" ||
				 (Property == "CanCollide" && !Part->GetAnchored())))
				return false;
			CFrame Proposed = Part->GetCFrame();
			if (Property == "CFrame") {
				if (const auto *Frame = std::any_cast<CFrame>(&Value))
					return !Occupies(Reservations, Object, World.get(), Frame);
			} else if (Property == "Position") {
				if (const auto *Position = std::any_cast<glm::vec3>(&Value)) {
					Proposed.Position = *Position;
					return !Occupies(Reservations, Object, World.get(), &Proposed);
				}
			} else if (Property == "Size") {
				if (const auto *Size = std::any_cast<glm::vec3>(&Value))
					return !Occupies(Reservations, Object, World.get(), nullptr, Size);
			} else if (Property == "Rotation") {
				if (World && Reservations.HoldsDomain(World->GetObjectId())) return false;
			}
		}
		if (Property == "SpatialRegion") {
			const auto *ValueRegion = std::any_cast<std::optional<std::shared_ptr<SpatialRegion>>>(&Value);
			const auto Parent = Object.GetParent().value_or(nullptr);
			const auto Destination = ValueRegion && *ValueRegion
										 ? std::static_pointer_cast<WorldRoot>(**ValueRegion)
										 : (Parent ? WorldRoot::GetEffectiveWorld(*Parent) : nullptr);
			std::vector<const Instance *> Nodes{&Object};
			for (std::size_t I = 0; I < Nodes.size(); ++I) {
				if (!AllowsReparent(*Nodes[I], Destination.get())) return false;
				for (const auto &Child : Nodes[I]->Children) {
					if (dynamic_cast<const WorldRoot *>(Child.get()) ||
						dynamic_cast<const ViewportFrame *>(Child.get()))
						continue;
					if (const auto *Member = dynamic_cast<const SpatialDomainMember *>(Child.get());
						Member && Member->HasAssignedRegion())
						continue;
					if (Nodes.size() >= 16384) return false;
					Nodes.push_back(Child.get());
				}
			}
			return true;
		}
		if (const auto *Joint = dynamic_cast<const Constraint *>(&Object);
			Joint && World && Reservations.HoldsDomain(World->GetObjectId()))
			return false;
		return true;
	}
	bool SpatialTransferProtection::AllowsReparent(const Instance &Object, const WorldRoot *NewWorld) const {
		if (Controls(Object)) return false;
		if (!Reservations.HasHolds()) return true;
		if (!AllowsResource(Object)) return false;
		if (NewWorld && Reservations.HoldsDestination(NewWorld->GetObjectId())) {
			if (const auto *Part = dynamic_cast<const BasePart *>(&Object);
				Part && !Part->GetAnchored() && Part->GetCanCollide())
				return false;
			if (dynamic_cast<const DeformableBody *>(&Object)) return false;
		}
		if (const auto *World = dynamic_cast<const WorldRoot *>(&Object);
			World && Reservations.HoldsDomain(World->GetObjectId()))
			return false;
		if (dynamic_cast<const Constraint *>(&Object) && NewWorld && Reservations.HoldsDomain(NewWorld->GetObjectId()))
			return false;
		return !Occupies(Reservations, Object, NewWorld);
	}
	bool SpatialTransferProtection::RequiresGrant(
		const Instance &Subject, const WorldRoot &Target, const SpatialDestinationBounds &Area
	) const {
		return Controls(Subject) || Reservations.HoldsMember(Subject.GetObjectId()) ||
			   Reservations.HoldsBounds(Target.GetObjectId(), Area);
	}
	void SpatialTransferProtection::CheckDestruction(const Instance &Root) {
		if (dynamic_cast<const DataModel *>(&Root)) {
			Retire();
			return;
		}
		if (!Controlled && !Reservations.HasHolds()) return;
		std::vector<const Instance *> Nodes{&Root};
		for (std::size_t I = 0; I < Nodes.size(); ++I) {
			const auto *Node = Nodes[I];
			if (!AllowsReparent(*Node, nullptr))
				throw std::runtime_error("[Spatial:Protection] Reserved subject or collision cannot be destroyed");
			if (Nodes.size() + Node->Children.size() > 16384)
				throw std::runtime_error("[Spatial:Protection] Subtree admission limit");
			for (const auto &Child : Node->Children)
				Nodes.push_back(Child.get());
		}
	}
	void SpatialTransferProtection::Retire() {
		Controlled = {};
		Reservations.Reset();
	}
}
