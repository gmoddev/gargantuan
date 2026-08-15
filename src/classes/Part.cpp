#include "gargantuan/classes/Part.hpp"

namespace gargantuan {
	PhysicsShapeDesc Part::GetPhysicsShape() const {
		PhysicsShapeKind Kind;
		switch (GetShape()) {
		case Enums::PartType::Block: Kind = PhysicsShapeKind::Box; break;
		case Enums::PartType::Ball: Kind = PhysicsShapeKind::Ball; break;
		case Enums::PartType::Cylinder: Kind = PhysicsShapeKind::Cylinder; break;
		case Enums::PartType::Wedge: Kind = PhysicsShapeKind::Wedge; break;
		case Enums::PartType::CornerWedge: Kind = PhysicsShapeKind::CornerWedge; break;
		}
		return {.Kind = Kind, .Size = GetSize()};
	}
}
