#include "gargantuan/classes/Part.hpp"

constexpr int NUM_CYLINDER_SIDES = 24;

namespace gargantuan {
	void Part::CreateBodyShape(b3BodyId bodyId, b3ShapeDef &bodyShape) {
		auto size = GetSize();
		auto halfSize = size * 0.5f;

		switch (GetShape()) {
		case Enums::PartType::Block: {
			auto boxHull = b3MakeBoxHull(halfSize.x, halfSize.y, halfSize.z);
			b3CreateHullShape(bodyId, &bodyShape, &boxHull.base);
			return;
		}
		case Enums::PartType::Wedge: {
			b3Vec3 points[6] = {
				{-halfSize.x, -halfSize.y, -halfSize.z},
				{halfSize.x, -halfSize.y, -halfSize.z},
				{halfSize.x, -halfSize.y, halfSize.z},
				{-halfSize.x, -halfSize.y, halfSize.z},
				{halfSize.x, halfSize.y, -halfSize.z},
				{-halfSize.x, halfSize.y, -halfSize.z},
			};
			b3HullData *hull = b3CreateHull(points, 6, 6);
			b3CreateHullShape(bodyId, &bodyShape, hull);
			b3DestroyHull(hull);
			return;
		}
		case Enums::PartType::CornerWedge: {
			b3Vec3 points[5] = {
				{-halfSize.x, -halfSize.y, -halfSize.z},
				{halfSize.x, -halfSize.y, -halfSize.z},
				{halfSize.x, -halfSize.y, halfSize.z},
				{-halfSize.x, -halfSize.y, halfSize.z},
				{-halfSize.x, halfSize.y, -halfSize.z},
			};
			b3HullData *hull = b3CreateHull(points, 5, 5);
			b3CreateHullShape(bodyId, &bodyShape, hull);
			b3DestroyHull(hull);
			return;
		}
		case Enums::PartType::Ball: {
			b3Sphere sphere = b3Sphere{.center = {0, 0, 0}, .radius = fmin(fmin(halfSize.x, halfSize.y), halfSize.z)};
			b3CreateSphereShape(bodyId, &bodyShape, &sphere);
			break;
		}
		case Enums::PartType::Cylinder: {
			b3HullData *cylinder = b3CreateCylinder(size.y, fmin(halfSize.x, halfSize.z * 0.5f), 0, NUM_CYLINDER_SIDES);
			b3CreateHullShape(bodyId, &bodyShape, cylinder);
			b3DestroyHull(cylinder);
			break;
		}
		}
	}
}
